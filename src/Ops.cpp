#include "tufo/Ops.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>
#include <unordered_set>

namespace tufo {

namespace fs = std::filesystem;

namespace {

GpuReadyCodec g_codec;
bool g_codecSet = false;

bool fail(std::string* err, const std::string& m) {
    if (err) *err = m;
    return false;
}

bool readFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    out.resize(static_cast<size_t>(n));
    f.seekg(0);
    return n == 0 || static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), n));
}

int64_t mtimeSeconds(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    const auto sys = std::chrono::time_point_cast<std::chrono::seconds>(t - fs::file_time_type::clock::now() +
                                                                        std::chrono::system_clock::now());
    return sys.time_since_epoch().count();
}

struct Job {
    fs::path file;
    std::string key;
    uint64_t size = 0;
};

// Junta os arquivos (recursivo, ordenados por caminho: um mapa fica junto no arquivo).
std::vector<Job> collectJobs(const std::vector<std::string>& paths, const AddOptions& opt) {
    std::vector<Job> jobs;
    std::error_code ec;
    const fs::path base = opt.base;
    std::string pre = normalizeKey(opt.prefix);
    if (!pre.empty()) pre.push_back('/');
    auto add = [&](const fs::path& f) {
        Job j;
        j.file = f;
        const fs::path rel = opt.base.empty() ? f.filename() : (f.lexically_relative(base).empty() ? fs::relative(f, base, ec) : f.lexically_relative(base));
        j.key = pre + rel.generic_string();
        j.size = fs::file_size(f, ec);
        jobs.push_back(std::move(j));
    };
    for (const std::string& s : paths) {
        const fs::path p = s;
        if (fs::is_directory(p, ec)) {
            for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec)) add(it->path());
        } else if (fs::is_regular_file(p, ec)) {
            add(p);
        } else {
            std::fprintf(stderr, "aviso: nao existe: %s\n", p.string().c_str());
        }
    }
    std::sort(jobs.begin(), jobs.end(), [](const Job& x, const Job& y) { return x.key < y.key; });
    return jobs;
}

void insertPath(TreeNode& root, const std::string& key, int entry, const EntryInfo& e) {
    TreeNode* cur = &root;
    size_t at = 0;
    for (;;) {
        const size_t slash = key.find('/', at);
        const bool last = slash == std::string::npos;
        const std::string part = key.substr(at, last ? std::string::npos : slash - at);
        TreeNode* next = nullptr;
        for (TreeNode& c : cur->children)
            if (c.name == part && c.dir == !last) {
                next = &c;
                break;
            }
        if (!next) {
            cur->children.emplace_back();
            next = &cur->children.back();
            next->name = part;
            next->path = key.substr(0, last ? key.size() : slash);
            next->dir = !last;
        }
        if (last) {
            next->entry = entry;
            next->raw = e.info.rawSize;
            next->stored = e.info.storedSize;
            next->files = 1;
            return;
        }
        cur = next;
        at = slash + 1;
    }
}

void finishTree(TreeNode& n) {
    // Pastas antes dos arquivos; cada grupo em ordem de nome.
    std::sort(n.children.begin(), n.children.end(), [](const TreeNode& a, const TreeNode& b) {
        if (a.dir != b.dir) return a.dir;
        return a.name < b.name;
    });
    if (!n.dir) return;
    for (TreeNode& c : n.children) {
        finishTree(c);
        n.raw += c.raw;
        n.stored += c.stored;
        n.files += c.files;
    }
}

} // namespace

TreeNode buildTree(const std::vector<EntryInfo>& entries) {
    TreeNode root;
    root.dir = true;
    for (size_t i = 0; i < entries.size(); ++i) insertPath(root, entries[i].key, static_cast<int>(i), entries[i]);
    finishTree(root);
    return root;
}

void setGpuReadyCodec(GpuReadyCodec codec) {
    g_codec = std::move(codec);
    g_codecSet = static_cast<bool>(g_codec.pack) && static_cast<bool>(g_codec.unpack);
}
const GpuReadyCodec* gpuReadyCodec() { return g_codecSet ? &g_codec : nullptr; }

bool readRestored(const Archive& ar, const std::string& key, std::vector<uint8_t>& out, std::string* err) {
    Info info;
    if (!ar.stat(key, &info)) return fail(err, "entrada inexistente: " + key);
    if (!ar.read(key, out, true, err)) return false;
    if (!(info.flags & kFlagGpuReady)) return true;
    if (!g_codecSet) return fail(err, key + ": entrada com texturas prontas para a GPU; remontar o .glb exige o codec do motor (eruption-engine --tuf-extract)");
    std::vector<uint8_t> stored = std::move(out);
    out.clear();
    return g_codec.unpack(normalizeKey(key), stored, ar, out, err);
}

std::string codecLabel(uint8_t mask) {
    static const char* names[3] = {"raw", "zstd", "zstd+dict"};
    std::string s;
    for (int c = 0; c < 3; ++c)
        if (mask & (1u << c)) s += (s.empty() ? "" : "+") + std::string(names[c]);
    return s.empty() ? "-" : s;
}

std::string humanSize(uint64_t b) {
    char buf[64];
    if (b >= (1ull << 30)) std::snprintf(buf, sizeof buf, "%.2f GB", double(b) / double(1ull << 30));
    else if (b >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.2f MB", double(b) / double(1ull << 20));
    else if (b >= 1024) std::snprintf(buf, sizeof buf, "%.1f KB", double(b) / 1024.0);
    else std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(b));
    return buf;
}

std::vector<std::string> keysUnder(const Archive& ar, const std::string& target) {
    std::vector<std::string> keys = ar.list(target);
    const std::string k = normalizeKey(target);
    if (!k.empty() && ar.exists(k)) keys.push_back(k);
    return keys;
}

long extractTo(const Archive& ar, const std::string& target, const std::string& outDir, Progress* progress,
               std::string* err) {
    const std::vector<std::string> keys = keysUnder(ar, target);
    if (progress) {
        progress->done = 0;
        progress->total = keys.size();
    }
    long ok = 0;
    std::vector<uint8_t> d;
    for (const std::string& k : keys) {
        if (progress && progress->cancel.load()) {
            fail(err, "cancelled");
            return -1;
        }
        std::string e2;
        Info ki;
        // A companheira (pacote de texturas) vai dentro do .glb remontado.
        if (g_codecSet && ar.stat(k, &ki) && (ki.flags & kFlagCompanion)) continue;
        if (!readRestored(ar, k, d, &e2)) {
            fail(err, k + ": " + e2);
            return -1;
        }
        const fs::path dst = fs::path(outDir) / k;
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        std::ofstream f(dst, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
        if (!f) {
            fail(err, "could not write " + dst.string());
            return -1;
        }
        ++ok;
        // Imagens por uri que o pacote de texturas cobre e o .tuf nao guarda: saem do formato de bloco.
        if (g_codecSet && g_codec.unpackExtras && ar.stat(k, &ki) && (ki.flags & kFlagGpuReady)) {
            std::vector<uint8_t> stored;
            std::vector<std::pair<std::string, std::vector<uint8_t>>> extra;
            if (ar.read(k, stored, true, &e2) && g_codec.unpackExtras(normalizeKey(k), stored, ar, extra, &e2)) {
                for (auto& x : extra) {
                    if (ar.exists(normalizeKey(x.first))) continue;
                    const fs::path xd = fs::path(outDir) / x.first;
                    fs::create_directories(xd.parent_path(), ec);
                    std::ofstream xf(xd, std::ios::binary | std::ios::trunc);
                    xf.write(reinterpret_cast<const char*>(x.second.data()), static_cast<std::streamsize>(x.second.size()));
                    if (xf) ++ok;
                }
            }
        }
        if (progress) progress->done = static_cast<uint64_t>(ok);
    }
    return ok;
}

long addPaths(const std::string& tuf, const std::vector<std::string>& paths, const AddOptions& opt, Progress* progress,
              std::string* err) {
    std::vector<Job> jobs = collectJobs(paths, opt);
    if (opt.gpuReady) {
        // Pacote de texturas solto em disco (<glb>.*.etexpack) e' cache derivado: o
        // codec o reaproveita ou refaz e o grava como entrada companheira; entrar
        // tambem como arquivo comum apagaria a marca da companheira.
        const auto isCache = [](const Job& j) {
            static const std::string suf = ".etexpack";
            return j.key.size() > suf.size() && j.key.compare(j.key.size() - suf.size(), suf.size(), suf) == 0;
        };
        jobs.erase(std::remove_if(jobs.begin(), jobs.end(), isCache), jobs.end());
        // Os .glb primeiro: o que o pacote de texturas de cada um cobre (imagens por uri) so' e' conhecido
        // depois de assar, e essas imagens nao entram.
        std::stable_partition(jobs.begin(), jobs.end(), [](const Job& j) {
            return j.key.size() > 4 && j.key.compare(j.key.size() - 4, 4, ".glb") == 0;
        });
    }
    std::unordered_set<std::string> skipKeys;
    if (jobs.empty()) {
        fail(err, "no files to add");
        return -1;
    }
    if (opt.gpuReady && !g_codecSet) {
        fail(err, "--gpu-ready exige o codec de texturas do motor: use eruption-engine --tuf-pack ... --gpu-ready");
        return -1;
    }
    Writer w;
    std::error_code ec;
    if (opt.create || !fs::exists(tuf, ec)) {
        if (!w.create(tuf, err, opt.overwrite)) return -1;
    } else if (!w.append(tuf, err)) {
        return -1;
    }
    WriteOptions wo = opt.write;
    if (opt.train) {
        // Amostras: arquivos pequenos que nao sao imagem, ate' 2000 e 32 MB.
        std::vector<std::vector<uint8_t>> samples;
        uint64_t total = 0;
        for (const Job& j : jobs) {
            if (j.size == 0 || j.size > wo.dictMaxRaw || samples.size() >= 2000 || total > (32u << 20)) continue;
            std::vector<uint8_t> d;
            if (!readFile(j.file, d) || (d.size() > 4 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')) continue;
            total += d.size();
            samples.push_back(std::move(d));
        }
        std::string derr;
        if (w.trainDictionary(samples, 112 * 1024, &derr)) std::printf("dicionario treinado com %zu arquivos pequenos\n", samples.size());
        else std::printf("sem dicionario: %s\n", derr.c_str());
    }

    const unsigned threads = std::max(1u, opt.threads);
    if (progress) {
        progress->done = 0;
        progress->total = jobs.size();
    }
    long done = 0;
    size_t i = 0;
    while (i < jobs.size()) {
        if (progress && progress->cancel.load()) {
            fail(err, "cancelled");
            return -1; // o Writer nunca chamou finish(): o arquivo segue como estava
        }
        // Lote: ate' 256 arquivos ou ~256 MB crus; preparado em paralelo, gravado em ordem.
        size_t j = i;
        uint64_t sum = 0;
        // gpuReady: cada .glb vira varios pacotes de texturas na memoria (um por combinacao de preset); lote
        // pequeno para o pico ficar longe do teto de memoria (8 GB estourou com 256 arquivos).
        const size_t maxFiles = opt.gpuReady ? 8 : 256;
        const uint64_t maxRaw = opt.gpuReady ? (48ull << 20) : (256ull << 20);
        while (j < jobs.size() && j - i < maxFiles && (j == i || sum + jobs[j].size <= maxRaw)) sum += jobs[j++].size;
        std::vector<PreparedFile> prepared(j - i);
        std::vector<char> okRead(j - i, 1);
        // Arquivos que o codec assume (.glb com imagens): em UM fio, porque o
        // codec ja' usa todos os nucleos por dentro; o resto segue em paralelo.
        std::vector<std::vector<uint8_t>> replaced(j - i);
        std::vector<GpuReadyCodec::Companions> companions(j - i);
        std::vector<char> baked(j - i, 0);
        if (opt.gpuReady) {
            std::vector<uint8_t> src;
            std::vector<std::string> newSkip;
            for (size_t k = i; k < j; ++k) {
                const std::string& jk = jobs[k].key;
                if (jk.size() <= 4 || jk.compare(jk.size() - 4, 4, ".glb") != 0) continue; // so' o .glb e' do codec
                if (!readFile(jobs[k].file, src)) {
                    okRead[k - i] = 0;
                    continue;
                }
                std::string cerr;
                if (g_codec.pack(jobs[k].key, jobs[k].file.string(), src, replaced[k - i], companions[k - i], newSkip, &cerr)) {
                    baked[k - i] = 1;
                    for (std::string& sk : newSkip) skipKeys.insert(std::move(sk));
                    newSkip.clear();
                }
                else if (!cerr.empty()) // um mapa que nao da' para assar entra como veio (o motor o comprime ao carregar)
                    std::fprintf(stderr, "aviso: %s entrou sem texturas prontas: %s\n", jobs[k].key.c_str(), cerr.c_str());
            }
        }
        std::vector<char> skipped(j - i, 0);
        if (opt.gpuReady)
            for (size_t k = i; k < j; ++k) skipped[k - i] = skipKeys.count(jobs[k].key) ? 1 : 0;
        std::atomic<size_t> next{i};
        auto work = [&]() {
            std::vector<uint8_t> buf;
            for (size_t k = next.fetch_add(1); k < j; k = next.fetch_add(1)) {
                if (skipped[k - i]) continue;
                if (baked[k - i]) {
                    prepared[k - i] = w.prepare(jobs[k].key, replaced[k - i].data(), replaced[k - i].size(),
                                                mtimeSeconds(jobs[k].file), wo);
                    prepared[k - i].flags = kFlagGpuReady;
                    continue;
                }
                if (!readFile(jobs[k].file, buf)) {
                    okRead[k - i] = 0;
                    continue;
                }
                prepared[k - i] = w.prepare(jobs[k].key, buf.data(), buf.size(), mtimeSeconds(jobs[k].file), wo);
            }
        };
        std::vector<std::thread> th;
        for (unsigned t = 0; t < threads; ++t) th.emplace_back(work);
        for (auto& t : th) t.join();
        for (size_t k = i; k < j; ++k) {
            if (skipped[k - i]) { // coberto pelo pacote de texturas de um .glb: nao entra
                ++done;
                if (progress) progress->done = static_cast<uint64_t>(done);
                continue;
            }
            if (!okRead[k - i]) {
                fail(err, "could not read " + jobs[k].file.string());
                return -1;
            }
            if (prepared[k - i].key.empty()) {
                fail(err, "caminho invalido: " + jobs[k].key);
                return -1;
            }
            if (!w.commit(std::move(prepared[k - i]), err)) return -1;
            for (auto& c : companions[k - i]) {
                if (normalizeKey(c.first).empty()) {
                    fail(err, "caminho invalido (texturas prontas): " + c.first);
                    return -1;
                }
                PreparedFile cf = w.prepare(normalizeKey(c.first), c.second.data(), c.second.size(), mtimeSeconds(jobs[k].file), wo);
                cf.flags = kFlagCompanion;
                if (!w.commit(std::move(cf), err)) return -1;
            }
            ++done;
            if (progress) progress->done = static_cast<uint64_t>(done);
        }
        i = j;
    }
    if (!w.finish(err)) return -1;
    return done;
}

long removeTargets(const std::string& tuf, const std::vector<std::string>& targets, std::string* err) {
    Writer w;
    if (!w.append(tuf, err)) return -1;
    Archive ar;
    ar.open(tuf, nullptr);
    long n = 0;
    for (const std::string& k : targets)
        for (const std::string& f : keysUnder(ar, k)) n += w.remove(f) ? 1 : 0;
    ar.close();
    if (!w.finish(err)) return -1;
    return n;
}

} // namespace tufo
