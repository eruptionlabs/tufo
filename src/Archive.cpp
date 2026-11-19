#include "tufo/Archive.hpp"

#include <zstd.h>
#ifdef TUFO_HAVE_ZSTD
#include <zdict.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace tufo {

namespace fs = std::filesystem;

// ---- XXH64 -----------------------------------------------------------------

namespace {

constexpr uint64_t P1 = 11400714785074694791ull, P2 = 14029467366897019727ull, P3 = 1609587929392839161ull,
                   P4 = 9650029242287828579ull, P5 = 2870177450012600261ull;
inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t rd64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
inline uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
inline uint16_t rd16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
inline uint64_t xround(uint64_t acc, uint64_t in) { return rotl(acc + in * P2, 31) * P1; }
inline uint64_t xmerge(uint64_t h, uint64_t v) { return (h ^ xround(0, v)) * P1 + P4; }

// Segunda impressao do conteudo: seed diferente e o tamanho dentro dela, para
// que uma colisao do primeiro hash nao baste para confundir dois blobs.
inline uint64_t hash2Of(const uint8_t* p, size_t n) { return xxh64(p, n, 0x9E3779B97F4A7C15ull ^ n); }

void put8(std::string& s, uint8_t v) { s.push_back(static_cast<char>(v)); }
void put16(std::string& s, uint16_t v) { s.append(reinterpret_cast<const char*>(&v), 2); }
void put32(std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }
void put64(std::string& s, uint64_t v) { s.append(reinterpret_cast<const char*>(&v), 8); }

bool failWith(std::string* err, const std::string& msg) {
    if (err) *err = msg;
    return false;
}

} // namespace

uint64_t xxh64(const void* data, size_t size, uint64_t seed) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + size;
    uint64_t h;
    if (size >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t* limit = end - 32;
        do {
            v1 = xround(v1, rd64(p));
            v2 = xround(v2, rd64(p + 8));
            v3 = xround(v3, rd64(p + 16));
            v4 = xround(v4, rd64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = xmerge(h, v1);
        h = xmerge(h, v2);
        h = xmerge(h, v3);
        h = xmerge(h, v4);
    } else {
        h = seed + P5;
    }
    h += size;
    while (p + 8 <= end) {
        h = rotl(h ^ xround(0, rd64(p)), 27) * P1 + P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h = rotl(h ^ (static_cast<uint64_t>(rd32(p)) * P1), 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h = rotl(h ^ (*p * P5), 11) * P1;
        ++p;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

std::string normalizeKey(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    size_t i = 0;
    const size_t n = path.size();
    while (i <= n) {
        size_t j = i;
        while (j < n && path[j] != '/' && path[j] != '\\') ++j;
        const std::string_view comp(path.data() + i, j - i);
        if (comp == "..") return {};
        if (!comp.empty() && comp != ".") {
            if (!out.empty()) out.push_back('/');
            for (char c : comp) out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
        }
        i = j + 1;
    }
    return out;
}

// ---- indice ------------------------------------------------------------------

namespace {

struct BlobRec {
    uint64_t off = 0, stored = 0, raw = 0;
    uint8_t codec = kRaw, flags = 0;
    uint16_t dict = 0;
    uint64_t h1 = 0, h2 = 0;
};
struct DictRec {
    uint32_t id = 0;
    uint64_t off = 0, size = 0, xxh = 0;
};
struct EntryRec {
    uint64_t pathHash = 0;
    uint32_t pathOff = 0;
    uint16_t pathLen = 0;
    uint8_t kind = 0, flags = 0; // kind 0 blob, 1 composicao
    uint32_t ref = 0;
    uint64_t raw = 0;
    int64_t mtime = 0;
    uint64_t full = 0;
};
struct IndexData {
    std::vector<BlobRec> blobs;
    std::vector<std::vector<uint32_t>> comps;
    std::vector<EntryRec> entries; // ordenadas pelos bytes do caminho
    std::string pool;              // caminhos
    std::vector<DictRec> dicts;
    std::string_view pathOf(const EntryRec& e) const { return std::string_view(pool.data() + e.pathOff, e.pathLen); }
};

constexpr uint32_t kIndexMagic = 0x58444954; // "TIDX"
constexpr uint32_t kIndexVersion = 1;

void serializeIndex(const IndexData& d, std::string& out) {
    put32(out, kIndexMagic);
    put32(out, kIndexVersion);
    put32(out, static_cast<uint32_t>(d.blobs.size()));
    put32(out, static_cast<uint32_t>(d.comps.size()));
    put32(out, static_cast<uint32_t>(d.entries.size()));
    put32(out, static_cast<uint32_t>(d.dicts.size()));
    put32(out, 0);
    for (const DictRec& x : d.dicts) {
        put32(out, x.id);
        put64(out, x.off);
        put64(out, x.size);
        put64(out, x.xxh);
    }
    for (const BlobRec& b : d.blobs) {
        put64(out, b.off);
        put64(out, b.stored);
        put64(out, b.raw);
        put8(out, b.codec);
        put8(out, b.flags);
        put16(out, b.dict);
        put32(out, 0);
        put64(out, b.h1);
        put64(out, b.h2);
    }
    for (const auto& c : d.comps) {
        put32(out, static_cast<uint32_t>(c.size()));
        for (uint32_t b : c) put32(out, b);
    }
    for (const EntryRec& e : d.entries) {
        put64(out, e.pathHash);
        put16(out, e.pathLen);
        out.append(d.pool, e.pathOff, e.pathLen);
        put8(out, e.kind);
        put8(out, e.flags);
        put32(out, e.ref);
        put64(out, e.raw);
        put64(out, static_cast<uint64_t>(e.mtime));
        put64(out, e.full);
    }
}

bool parseIndex(const uint8_t* p, size_t n, uint64_t dataLimit, IndexData& d, std::string* err) {
    size_t c = 0;
    auto need = [&](size_t k) { return c + k <= n; };
    if (!need(28) || rd32(p) != kIndexMagic) return failWith(err, "indice: magic invalido");
    if (rd32(p + 4) != kIndexVersion) return failWith(err, "indice: versao desconhecida");
    const uint32_t nb = rd32(p + 8), nc = rd32(p + 12), ne = rd32(p + 16), nd = rd32(p + 20);
    c = 28;
    if (nb > (1u << 28) || nc > (1u << 28) || ne > (1u << 28) || nd > 64) return failWith(err, "indice: contagens absurdas");
    d.dicts.resize(nd);
    for (DictRec& x : d.dicts) {
        if (!need(28)) return failWith(err, "indice: dicionarios truncados");
        x.id = rd32(p + c);
        x.off = rd64(p + c + 4);
        x.size = rd64(p + c + 12);
        x.xxh = rd64(p + c + 20);
        c += 28;
        if (x.off < kHeaderSize || x.size > dataLimit || x.off > dataLimit - x.size) return failWith(err, "indice: dicionario fora do arquivo");
    }
    if (!need(static_cast<size_t>(nb) * 48)) return failWith(err, "indice: blobs truncados");
    d.blobs.resize(nb);
    for (BlobRec& b : d.blobs) {
        b.off = rd64(p + c);
        b.stored = rd64(p + c + 8);
        b.raw = rd64(p + c + 16);
        b.codec = p[c + 24];
        b.flags = p[c + 25];
        b.dict = rd16(p + c + 26);
        b.h1 = rd64(p + c + 32);
        b.h2 = rd64(p + c + 40);
        c += 48;
        if (b.codec > kZstdDict) return failWith(err, "indice: codec desconhecido");
        if (b.stored > dataLimit || b.off > dataLimit - b.stored || (b.stored && b.off < kHeaderSize))
            return failWith(err, "indice: blob fora do arquivo");
        if (b.codec == kRaw && b.stored != b.raw) return failWith(err, "indice: blob cru com tamanhos diferentes");
    }
    d.comps.resize(nc);
    for (auto& cp : d.comps) {
        if (!need(4)) return failWith(err, "indice: composicoes truncadas");
        const uint32_t k = rd32(p + c);
        c += 4;
        if (!need(static_cast<size_t>(k) * 4)) return failWith(err, "indice: composicao truncada");
        cp.resize(k);
        for (uint32_t i = 0; i < k; ++i) {
            cp[i] = rd32(p + c + i * 4);
            if (cp[i] >= nb) return failWith(err, "indice: composicao cita blob inexistente");
        }
        c += static_cast<size_t>(k) * 4;
    }
    d.entries.resize(ne);
    d.pool.reserve(n / 2);
    for (EntryRec& e : d.entries) {
        if (!need(10)) return failWith(err, "indice: entradas truncadas");
        e.pathHash = rd64(p + c);
        e.pathLen = rd16(p + c + 8);
        c += 10;
        if (!need(static_cast<size_t>(e.pathLen) + 30)) return failWith(err, "indice: entrada truncada");
        e.pathOff = static_cast<uint32_t>(d.pool.size());
        d.pool.append(reinterpret_cast<const char*>(p + c), e.pathLen);
        c += e.pathLen;
        e.kind = p[c];
        e.flags = p[c + 1];
        e.ref = rd32(p + c + 2);
        e.raw = rd64(p + c + 6);
        e.mtime = static_cast<int64_t>(rd64(p + c + 14));
        e.full = rd64(p + c + 22);
        c += 30;
        if (e.kind > 1 || (e.kind == 0 && e.ref >= nb) || (e.kind == 1 && e.ref >= nc)) return failWith(err, "indice: referencia invalida");
    }
    if (c != n) return failWith(err, "indice: bytes sobrando");
    for (size_t i = 1; i < d.entries.size(); ++i)
        if (!(d.pathOf(d.entries[i - 1]) < d.pathOf(d.entries[i]))) return failWith(err, "indice: caminhos fora de ordem");
    return true;
}

// Slot do cabecalho.
struct Slot {
    bool valid = false;
    uint64_t gen = 0, indexOff = 0, indexStored = 0, indexRaw = 0, indexXxh = 0;
    uint32_t flags = 0;
};

Slot readSlot(const uint8_t* p) {
    Slot s;
    if (std::memcmp(p, kMagic, 8) != 0) return s;
    if (rd16(p + 8) != kVersion) return s;
    if (xxh64(p, 56) != rd64(p + 56)) return s;
    s.flags = rd32(p + 12);
    s.gen = rd64(p + 16);
    s.indexOff = rd64(p + 24);
    s.indexStored = rd64(p + 32);
    s.indexRaw = rd64(p + 40);
    s.indexXxh = rd64(p + 48);
    s.valid = true;
    return s;
}

std::string makeSlot(const Slot& s) {
    std::string o;
    o.append(kMagic, 8);
    put16(o, kVersion);
    put16(o, 0);
    put32(o, s.flags);
    put64(o, s.gen);
    put64(o, s.indexOff);
    put64(o, s.indexStored);
    put64(o, s.indexRaw);
    put64(o, s.indexXxh);
    put64(o, xxh64(o.data(), 56));
    return o;
}

// Decodifica um blob para dst (raw bytes). false = dado ruim.
bool decodeBlob(const uint8_t* src, const BlobRec& b, uint8_t* dst, const std::vector<std::pair<uint32_t, ZSTD_DDict*>>& dd,
                std::string* err) {
    if (b.codec == kRaw) {
        if (b.raw) std::memcpy(dst, src, static_cast<size_t>(b.raw));
        return true;
    }
    thread_local ZSTD_DCtx* dctx = ZSTD_createDCtx();
    size_t got = 0;
    if (b.codec == kZstd) {
        got = ZSTD_decompressDCtx(dctx, dst, static_cast<size_t>(b.raw), src, static_cast<size_t>(b.stored));
    } else {
        ZSTD_DDict* d = nullptr;
        for (const auto& x : dd)
            if (x.first == b.dict) d = x.second;
        if (!d) return failWith(err, "dicionario do blob ausente");
        got = ZSTD_decompress_usingDDict(dctx, dst, static_cast<size_t>(b.raw), src, static_cast<size_t>(b.stored), d);
    }
    if (ZSTD_isError(got) || got != b.raw) return failWith(err, std::string("zstd: ") + (ZSTD_isError(got) ? ZSTD_getErrorName(got) : "tamanho cru diferente"));
    return true;
}

} // namespace

// ---- Archive -----------------------------------------------------------------

struct Archive::Impl {
    std::string path;
    const uint8_t* base = nullptr;
    size_t size = 0;
#if defined(_WIN32)
    HANDLE hFile = INVALID_HANDLE_VALUE, hMap = nullptr;
#else
    int fd = -1;
#endif
    IndexData idx;
    Slot slot;
    std::vector<std::pair<uint32_t, ZSTD_DDict*>> ddicts;

    void release() {
        for (auto& d : ddicts)
            if (d.second) ZSTD_freeDDict(d.second);
        ddicts.clear();
#if defined(_WIN32)
        if (base) UnmapViewOfFile(base);
        if (hMap) CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
        hMap = nullptr;
        hFile = INVALID_HANDLE_VALUE;
#else
        if (base) ::munmap(const_cast<uint8_t*>(base), size);
        if (fd >= 0) ::close(fd);
        fd = -1;
#endif
        base = nullptr;
        size = 0;
        idx = IndexData{};
        slot = Slot{};
    }

    const EntryRec* find(const std::string& key) const {
        const std::string k = normalizeKey(key);
        if (k.empty()) return nullptr;
        auto it = std::lower_bound(idx.entries.begin(), idx.entries.end(), k,
                                   [&](const EntryRec& e, const std::string& v) { return idx.pathOf(e) < std::string_view(v); });
        if (it == idx.entries.end() || idx.pathOf(*it) != std::string_view(k)) return nullptr;
        return &*it;
    }

    bool decodeInto(const BlobRec& b, uint8_t* dst, bool verify, std::string* err) const {
        if (!decodeBlob(base + b.off, b, dst, ddicts, err)) return false;
        if (verify && (xxh64(dst, static_cast<size_t>(b.raw)) != b.h1 || hash2Of(dst, static_cast<size_t>(b.raw)) != b.h2))
            return failWith(err, "hash do blob nao confere (arquivo corrompido)");
        return true;
    }

    bool readEntry(const EntryRec& e, std::vector<uint8_t>& out, bool verify, std::string* err) const {
        out.resize(static_cast<size_t>(e.raw));
        if (e.kind == 0) {
            if (!decodeInto(idx.blobs[e.ref], out.data(), verify, err)) return false;
        } else {
            size_t pos = 0;
            for (uint32_t bi : idx.comps[e.ref]) {
                const BlobRec& b = idx.blobs[bi];
                if (pos + b.raw > out.size()) return failWith(err, "composicao maior que a entrada");
                if (!decodeInto(b, out.data() + pos, verify, err)) return false;
                pos += static_cast<size_t>(b.raw);
            }
            if (pos != out.size()) return failWith(err, "composicao menor que a entrada");
        }
        if (verify && xxh64(out.data(), out.size()) != e.full) return failWith(err, "hash da entrada nao confere");
        return true;
    }
};

Archive::Archive() : m_impl(new Impl) {}
Archive::~Archive() { close(); }

bool Archive::open(const std::string& path, std::string* err) {
    close();
    Impl& m = *m_impl;
    m.path = path;
#if defined(_WIN32)
    m.hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m.hFile == INVALID_HANDLE_VALUE) return failWith(err, "nao abriu " + path);
    LARGE_INTEGER sz;
    GetFileSizeEx(m.hFile, &sz);
    m.size = static_cast<size_t>(sz.QuadPart);
    m.hMap = CreateFileMappingA(m.hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (m.size && m.hMap) m.base = static_cast<const uint8_t*>(MapViewOfFile(m.hMap, FILE_MAP_READ, 0, 0, 0));
#else
    m.fd = ::open(path.c_str(), O_RDONLY);
    if (m.fd < 0) return failWith(err, "nao abriu " + path);
    struct stat st{};
    if (::fstat(m.fd, &st) != 0) {
        m.release();
        return failWith(err, "fstat falhou: " + path);
    }
    m.size = static_cast<size_t>(st.st_size);
    if (m.size) {
        void* p = ::mmap(nullptr, m.size, PROT_READ, MAP_SHARED, m.fd, 0);
        if (p != MAP_FAILED) m.base = static_cast<const uint8_t*>(p);
    }
#endif
    if (!m.base || m.size < kHeaderSize) {
        m.release();
        return failWith(err, "arquivo .tuf vazio ou nao mapeavel: " + path);
    }
    Slot a = readSlot(m.base), b = readSlot(m.base + kSlotSize);
    // Tenta o slot de maior geracao; se o indice dele estiver ruim, o outro.
    Slot order[2] = {a, b};
    if (b.valid && (!a.valid || b.gen > a.gen)) std::swap(order[0], order[1]);
    std::string lastErr = "nenhum slot de cabecalho valido (nao e' um .tuf ou esta' corrompido)";
    for (const Slot& s : order) {
        if (!s.valid) continue;
        if (s.indexOff < kHeaderSize || s.indexStored > m.size || s.indexOff > m.size - s.indexStored) {
            lastErr = "indice fora do arquivo (arquivo truncado?)";
            continue;
        }
        std::vector<uint8_t> raw;
        const uint8_t* ip = m.base + s.indexOff;
        const uint8_t* use = ip;
        if (s.indexStored != s.indexRaw) {
            raw.resize(static_cast<size_t>(s.indexRaw));
            const size_t got = ZSTD_decompress(raw.data(), raw.size(), ip, static_cast<size_t>(s.indexStored));
            if (ZSTD_isError(got) || got != raw.size()) {
                lastErr = "indice nao descomprime";
                continue;
            }
            use = raw.data();
        }
        if (xxh64(use, static_cast<size_t>(s.indexRaw)) != s.indexXxh) {
            lastErr = "hash do indice nao confere";
            continue;
        }
        IndexData d;
        if (!parseIndex(use, static_cast<size_t>(s.indexRaw), m.size, d, &lastErr)) continue;
        m.idx = std::move(d);
        m.slot = s;
        for (const DictRec& dr : m.idx.dicts) {
            if (xxh64(m.base + dr.off, static_cast<size_t>(dr.size)) != dr.xxh) {
                lastErr = "hash do dicionario nao confere";
                m.idx = IndexData{};
                break;
            }
            ZSTD_DDict* dd = ZSTD_createDDict(m.base + dr.off, static_cast<size_t>(dr.size));
            if (!dd) {
                lastErr = "dicionario invalido";
                m.idx = IndexData{};
                break;
            }
            m.ddicts.emplace_back(dr.id, dd);
        }
        if (m.idx.dicts.size() == m.ddicts.size() && m.slot.valid) return true;
        for (auto& d2 : m.ddicts) ZSTD_freeDDict(d2.second);
        m.ddicts.clear();
        m.slot = Slot{};
    }
    m.release();
    return failWith(err, lastErr);
}

void Archive::close() {
    if (m_impl) m_impl->release();
}
bool Archive::isOpen() const { return m_impl->base != nullptr; }
const std::string& Archive::path() const { return m_impl->path; }

bool Archive::exists(const std::string& key) const { return m_impl->find(key) != nullptr; }

bool Archive::stat(const std::string& key, Info* info) const {
    const EntryRec* e = m_impl->find(key);
    if (!e) return false;
    if (info) {
        info->rawSize = e->raw;
        info->mtime = e->mtime;
        info->xxh64 = e->full;
        info->segments = e->kind == 0 ? 1u : static_cast<uint32_t>(m_impl->idx.comps[e->ref].size());
        info->storedSize = 0;
        if (e->kind == 0) info->storedSize = m_impl->idx.blobs[e->ref].stored;
        else
            for (uint32_t b : m_impl->idx.comps[e->ref]) info->storedSize += m_impl->idx.blobs[b].stored;
    }
    return true;
}

bool Archive::read(const std::string& key, std::vector<uint8_t>& out, bool verify, std::string* err) const {
    const EntryRec* e = m_impl->find(key);
    if (!e) return failWith(err, "entrada inexistente: " + key);
    return m_impl->readEntry(*e, out, verify, err);
}

std::vector<std::string> Archive::list(const std::string& dir) const {
    std::vector<std::string> out;
    std::string pre = normalizeKey(dir);
    if (!pre.empty()) pre.push_back('/');
    const IndexData& d = m_impl->idx;
    auto it = std::lower_bound(d.entries.begin(), d.entries.end(), pre,
                               [&](const EntryRec& e, const std::string& v) { return d.pathOf(e) < std::string_view(v); });
    for (; it != d.entries.end(); ++it) {
        const std::string_view p = d.pathOf(*it);
        if (p.compare(0, pre.size(), pre) != 0) break;
        out.emplace_back(p);
    }
    return out;
}

std::vector<EntryInfo> Archive::listInfo(const std::string& dir) const {
    const Impl& m = *m_impl;
    std::vector<uint32_t> refs(m.idx.blobs.size(), 0);
    for (const EntryRec& e : m.idx.entries) {
        if (e.kind == 0) ++refs[e.ref];
        else
            for (uint32_t b : m.idx.comps[e.ref]) ++refs[b];
    }
    std::vector<EntryInfo> out;
    for (const std::string& k : list(dir)) {
        const EntryRec* e = m.find(k);
        if (!e) continue;
        EntryInfo ei;
        ei.key = k;
        ei.info.rawSize = e->raw;
        ei.info.mtime = e->mtime;
        ei.info.xxh64 = e->full;
        auto addBlob = [&](uint32_t b) {
            ei.info.storedSize += m.idx.blobs[b].stored;
            ei.codecMask = static_cast<uint8_t>(ei.codecMask | (1u << (m.idx.blobs[b].codec & 7)));
            if (refs[b] > 1) ei.deduped = true;
        };
        if (e->kind == 0) {
            addBlob(e->ref);
        } else {
            ei.info.segments = static_cast<uint32_t>(m.idx.comps[e->ref].size());
            for (uint32_t b : m.idx.comps[e->ref]) addBlob(b);
        }
        out.push_back(std::move(ei));
    }
    return out;
}

size_t Archive::fileCount() const { return m_impl->idx.entries.size(); }

Stats Archive::stats() const {
    const Impl& m = *m_impl;
    Stats s;
    s.fileBytes = m.size;
    s.entries = m.idx.entries.size();
    s.blobs = m.idx.blobs.size();
    s.composites = m.idx.comps.size();
    s.dicts = m.idx.dicts.size();
    s.generation = m.slot.gen;
    std::vector<uint32_t> refs(m.idx.blobs.size(), 0);
    for (const EntryRec& e : m.idx.entries) {
        s.rawBytes += e.raw;
        if (e.kind == 0) ++refs[e.ref];
        else
            for (uint32_t b : m.idx.comps[e.ref]) ++refs[b];
    }
    for (size_t i = 0; i < refs.size(); ++i) {
        if (!refs[i]) continue;
        s.uniqueRawBytes += m.idx.blobs[i].raw;
        s.storedBytes += m.idx.blobs[i].stored;
        if (m.idx.blobs[i].codec == kRaw) ++s.rawCodecBlobs;
    }
    uint64_t dictBytes = 0;
    for (const DictRec& d : m.idx.dicts) dictBytes += d.size;
    for (const EntryRec& e : m.idx.entries)
        if (e.kind == 0 && refs[e.ref] > 1) ++s.dedupedEntries;
    const uint64_t live = kHeaderSize + s.storedBytes + dictBytes + m.slot.indexStored;
    s.deadBytes = m.size > live ? m.size - live : 0;
    return s;
}

size_t Archive::verifyAll(std::vector<std::string>* report, unsigned threads, Progress* progress) const {
    const Impl& m = *m_impl;
    if (progress) progress->total = m.idx.entries.size();
    std::atomic<size_t> next{0}, bad{0};
    std::mutex mu;
    auto note = [&](const std::string& s) {
        bad.fetch_add(1);
        std::lock_guard<std::mutex> lk(mu);
        if (report) report->push_back(s);
    };
    auto work = [&]() {
        std::vector<uint8_t> buf;
        for (size_t i = next.fetch_add(1); i < m.idx.entries.size(); i = next.fetch_add(1)) {
            if (progress && progress->cancel.load()) return;
            const EntryRec& e = m.idx.entries[i];
            const std::string key(m.idx.pathOf(e));
            if (progress) progress->done.fetch_add(1);
            if (xxh64(key.data(), key.size()) != e.pathHash) {
                note(key + ": hash do caminho nao confere");
                continue;
            }
            std::string err;
            if (!m.readEntry(e, buf, true, &err)) note(key + ": " + err);
        }
    };
    if (threads <= 1) work();
    else {
        std::vector<std::thread> th;
        for (unsigned t = 0; t < threads; ++t) th.emplace_back(work);
        for (auto& t : th) t.join();
    }
    return bad.load();
}

// ---- Writer ------------------------------------------------------------------

namespace {

struct BlobKey {
    uint64_t h1, h2, raw;
    bool operator==(const BlobKey& o) const { return h1 == o.h1 && h2 == o.h2 && raw == o.raw; }
};
struct BlobKeyHash {
    size_t operator()(const BlobKey& k) const { return static_cast<size_t>(k.h1 ^ (k.h2 * 31) ^ k.raw); }
};

struct Ent {
    uint8_t kind = 0;
    uint32_t ref = 0;
    uint64_t raw = 0;
    int64_t mtime = 0;
    uint64_t full = 0;
};

// Fim de um PNG que comeca em p[start] (assinatura + chunks ate' o IEND); 0 se nao for.
size_t pngEnd(const uint8_t* p, size_t n, size_t start) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (start + 8 > n || std::memcmp(p + start, sig, 8) != 0) return 0;
    size_t pos = start + 8;
    bool first = true;
    while (pos + 12 <= n) {
        const uint32_t len = (uint32_t(p[pos]) << 24) | (uint32_t(p[pos + 1]) << 16) | (uint32_t(p[pos + 2]) << 8) | p[pos + 3];
        const bool ihdr = std::memcmp(p + pos + 4, "IHDR", 4) == 0;
        if (first && (!ihdr || len != 13)) return 0;
        first = false;
        if (len > n - pos - 12) return 0;
        const bool iend = std::memcmp(p + pos + 4, "IEND", 4) == 0;
        pos += 12 + static_cast<size_t>(len);
        if (iend) return pos;
    }
    return 0;
}

} // namespace

struct Writer::Impl {
    std::string path;
    int fd = -1;
    uint64_t dataEnd = kHeaderSize;
    uint64_t generation = 0;
    IndexData idx; // blobs, comps e dicts; as entradas vivem em `ents`
    std::map<std::string, Ent> ents;
    std::unordered_map<BlobKey, uint32_t, BlobKeyHash> known;
    struct Flight {
        std::shared_ptr<const std::vector<uint8_t>> stored;
        uint8_t codec = kRaw;
        uint16_t dict = 0;
    };
    std::unordered_map<BlobKey, Flight, BlobKeyHash> inflight;
    mutable std::mutex mu;
    std::vector<uint8_t> dictBytes;
    uint16_t dictId = 0;
#ifdef TUFO_HAVE_ZSTD
    mutable std::map<int, ZSTD_CDict*> cdicts;
#endif
    bool finished = false;

    ~Impl() {
#ifdef TUFO_HAVE_ZSTD
        for (auto& c : cdicts) ZSTD_freeCDict(c.second);
#endif
#if !defined(_WIN32)
        if (fd >= 0) ::close(fd);
#endif
    }

    bool writeAt(uint64_t off, const void* p, size_t n, std::string* err) {
#if defined(_WIN32)
        (void)off; (void)p; (void)n;
        return failWith(err, "escrita de .tuf nao suportada no Windows");
#else
        const uint8_t* b = static_cast<const uint8_t*>(p);
        while (n) {
            const ssize_t w = ::pwrite(fd, b, n, static_cast<off_t>(off));
            if (w <= 0) return failWith(err, "pwrite falhou");
            b += w;
            off += static_cast<uint64_t>(w);
            n -= static_cast<size_t>(w);
        }
        return true;
#endif
    }

    bool appendBytes(const uint8_t* p, size_t n, uint64_t& offOut, std::string* err) {
        const uint64_t at = (dataEnd + kAlign - 1) & ~uint64_t(kAlign - 1);
        if (at > dataEnd) {
            static const uint8_t zeros[kAlign] = {};
            if (!writeAt(dataEnd, zeros, static_cast<size_t>(at - dataEnd), err)) return false;
        }
        if (n && !writeAt(at, p, n, err)) return false;
        offOut = at;
        dataEnd = at + n;
        return true;
    }

#ifdef TUFO_HAVE_ZSTD
    ZSTD_CDict* cdictFor(int level) const {
        if (dictBytes.empty()) return nullptr;
        std::lock_guard<std::mutex> lk(mu);
        auto it = cdicts.find(level);
        if (it != cdicts.end()) return it->second;
        ZSTD_CDict* c = ZSTD_createCDict(dictBytes.data(), dictBytes.size(), level);
        cdicts[level] = c;
        return c;
    }
#endif
};

Writer::Writer() : m_impl(new Impl) {}
Writer::~Writer() = default;

bool Writer::create(const std::string& path, std::string* err, bool overwrite) {
#if defined(_WIN32)
    (void)path; (void)overwrite;
    return failWith(err, "escrita de .tuf nao suportada no Windows");
#else
    Impl& m = *m_impl;
    std::error_code ec;
    if (!overwrite && fs::exists(path, ec)) return failWith(err, "ja' existe: " + path);
    m.fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (m.fd < 0) return failWith(err, "nao criou " + path);
    m.path = path;
    const std::string zeros(kHeaderSize, '\0'); // nenhum slot valido ate' o primeiro finish
    if (!m.writeAt(0, zeros.data(), zeros.size(), err)) return false;
    m.dataEnd = kHeaderSize;
    m.generation = 0;
    return true;
#endif
}

bool Writer::append(const std::string& path, std::string* err) {
#if defined(_WIN32)
    (void)path;
    return failWith(err, "escrita de .tuf nao suportada no Windows");
#else
    Impl& m = *m_impl;
    Archive a;
    if (!a.open(path, err)) return false;
    const Archive::Impl& ai = *a.m_impl;
    m.idx = ai.idx;
    m.idx.entries.clear();
    for (const EntryRec& e : ai.idx.entries)
        m.ents[std::string(ai.idx.pathOf(e))] = Ent{e.kind, e.ref, e.raw, e.mtime, e.full};
    for (size_t i = 0; i < m.idx.blobs.size(); ++i) {
        const BlobRec& b = m.idx.blobs[i];
        m.known.emplace(BlobKey{b.h1, b.h2, b.raw}, static_cast<uint32_t>(i));
    }
    if (!ai.idx.dicts.empty()) {
        const DictRec& d = ai.idx.dicts.front();
        m.dictBytes.assign(ai.base + d.off, ai.base + d.off + d.size);
        m.dictId = static_cast<uint16_t>(d.id);
    }
    m.generation = ai.slot.gen;
    const uint64_t fileSize = ai.size;
    a.close();
    m.fd = ::open(path.c_str(), O_RDWR);
    if (m.fd < 0) return failWith(err, "nao abriu para escrita " + path);
    m.path = path;
    m.dataEnd = fileSize;
    return true;
#endif
}

uint64_t Writer::dataBytes() const { return m_impl->dataEnd; }

bool Writer::trainDictionary(const std::vector<std::vector<uint8_t>>& samples, size_t dictSize, std::string* err) {
#ifdef TUFO_HAVE_ZSTD
    Impl& m = *m_impl;
    if (!m.dictBytes.empty()) return failWith(err, "o arquivo ja' tem dicionario");
    std::vector<uint8_t> all;
    std::vector<size_t> sizes;
    for (const auto& s : samples) {
        all.insert(all.end(), s.begin(), s.end());
        sizes.push_back(s.size());
    }
    if (sizes.size() < 8) return failWith(err, "amostras demais poucas para treinar dicionario");
    std::vector<uint8_t> dict(dictSize);
    const size_t n = ZDICT_trainFromBuffer(dict.data(), dict.size(), all.data(), sizes.data(), static_cast<unsigned>(sizes.size()));
    if (ZDICT_isError(n)) return failWith(err, std::string("ZDICT: ") + ZDICT_getErrorName(n));
    dict.resize(n);
    uint64_t off = 0;
    if (!m.appendBytes(dict.data(), dict.size(), off, err)) return false;
    DictRec r;
    r.id = 1;
    r.off = off;
    r.size = dict.size();
    r.xxh = xxh64(dict.data(), dict.size());
    m.idx.dicts.push_back(r);
    m.dictBytes = std::move(dict);
    m.dictId = 1;
    return true;
#else
    (void)samples; (void)dictSize;
    return failWith(err, "esta build nao tem o compressor zstd");
#endif
}

PreparedFile Writer::prepare(const std::string& keyIn, const uint8_t* data, size_t size, int64_t mtime,
                             const WriteOptions& opt) const {
    const Impl& m = *m_impl;
    PreparedFile f;
    f.key = normalizeKey(keyIn);
    f.mtime = mtime;
    f.raw = size;
    f.full = xxh64(data, size);

    // Segmentos: o arquivo inteiro, ou as pecas entre os PNG embutidos e os PNG.
    std::vector<std::pair<size_t, size_t>> segs;
    if (opt.splitPng && size >= opt.splitMinPng + 8) {
        size_t last = 0, i = 0;
        while (i + 8 <= size) {
            const void* q = std::memchr(data + i, 0x89, size - i);
            if (!q) break;
            const size_t at = static_cast<size_t>(static_cast<const uint8_t*>(q) - data);
            const size_t end = pngEnd(data, size, at);
            if (end && end - at >= opt.splitMinPng) {
                if (at > last) segs.emplace_back(last, at);
                segs.emplace_back(at, end);
                last = end;
                i = end;
            } else {
                i = at + 1;
            }
        }
        if (!segs.empty() && last < size) segs.emplace_back(last, size);
    }
    if (segs.empty()) segs.emplace_back(0, size);
    else if (segs.size() > 1) f.composite = true; // o arquivo todo ser um PNG so': blob simples

    for (const auto& s : segs) {
        const uint8_t* p = data + s.first;
        const size_t n = s.second - s.first;
        PreparedBlob b;
        b.raw = n;
        b.h1 = xxh64(p, n);
        b.h2 = hash2Of(p, n);
        const BlobKey key{b.h1, b.h2, b.raw};
        {
            std::lock_guard<std::mutex> lk(m.mu);
            if (m.known.count(key)) {
                b.known = true;
            } else {
                auto it = m.inflight.find(key);
                if (it != m.inflight.end()) {
                    b.stored = it->second.stored;
                    b.codec = it->second.codec;
                    b.dict = it->second.dict;
                }
            }
        }
        if (b.known || b.stored) {
            f.blobs.push_back(std::move(b));
            continue;
        }
        // Comprime: crua se nao ganhar. Amostra de 64 KB a nivel 1 poupa o nivel
        // alto nos dados que nao comprimem (PNG, MP3, BC7 sem RDO).
        std::vector<uint8_t> out;
        b.codec = kRaw;
#ifdef TUFO_HAVE_ZSTD
        if (opt.level > 0 && n > 0) {
            bool tryIt = true;
            if (n >= 64 * 1024) {
                std::vector<uint8_t> probe(ZSTD_compressBound(64 * 1024));
                const size_t pn = ZSTD_compress(probe.data(), probe.size(), p, 64 * 1024, 1);
                if (!ZSTD_isError(pn) && static_cast<double>(pn) > 64.0 * 1024.0 * (1.0 - opt.minGain)) tryIt = false;
            }
            if (tryIt) {
                thread_local ZSTD_CCtx* cctx = ZSTD_createCCtx();
                std::vector<uint8_t> z(ZSTD_compressBound(n));
                size_t zn = ZSTD_compressCCtx(cctx, z.data(), z.size(), p, n, opt.level);
                uint8_t codec = kZstd;
                uint16_t dictId = 0;
                if (ZSTD_isError(zn)) zn = z.size() + 1;
                if (n <= opt.dictMaxRaw) {
                    if (ZSTD_CDict* cd = m.cdictFor(opt.level)) {
                        std::vector<uint8_t> zd(ZSTD_compressBound(n));
                        const size_t dn = ZSTD_compress_usingCDict(cctx, zd.data(), zd.size(), p, n, cd);
                        if (!ZSTD_isError(dn) && dn < zn) {
                            z.swap(zd);
                            zn = dn;
                            codec = kZstdDict;
                            dictId = m.dictId;
                        }
                    }
                }
                if (zn <= z.size() && static_cast<double>(zn) <= static_cast<double>(n) * (1.0 - opt.minGain)) {
                    z.resize(zn);
                    out.swap(z);
                    b.codec = codec;
                    b.dict = dictId;
                }
            }
        }
#endif
        if (b.codec == kRaw) out.assign(p, p + n);
        auto sp = std::make_shared<const std::vector<uint8_t>>(std::move(out));
        b.stored = sp;
        {
            std::lock_guard<std::mutex> lk(m.mu);
            // O codec do blob em voo e' o do primeiro que o preparou: guardado junto.
            const_cast<Impl&>(m).inflight.emplace(key, Impl::Flight{sp, b.codec, b.dict});
        }
        f.blobs.push_back(std::move(b));
    }
    return f;
}

bool Writer::commit(PreparedFile&& f, std::string* err) {
    Impl& m = *m_impl;
    if (f.key.empty()) return failWith(err, "caminho invalido");
    std::vector<uint32_t> ids;
    for (PreparedBlob& b : f.blobs) {
        const BlobKey key{b.h1, b.h2, b.raw};
        auto it = m.known.find(key);
        if (it != m.known.end()) {
            ids.push_back(it->second);
            continue;
        }
        if (!b.stored) return failWith(err, "blob marcado como existente mas ausente: " + f.key);
        BlobRec r;
        r.raw = b.raw;
        r.stored = b.stored->size();
        r.codec = b.codec;
        r.dict = b.dict;
        r.h1 = b.h1;
        r.h2 = b.h2;
        if (!m.appendBytes(b.stored->data(), b.stored->size(), r.off, err)) return false;
        const uint32_t id = static_cast<uint32_t>(m.idx.blobs.size());
        m.idx.blobs.push_back(r);
        m.known.emplace(key, id);
        std::lock_guard<std::mutex> lk(m.mu);
        m.inflight.erase(key);
        ids.push_back(id);
    }
    Ent e;
    e.raw = f.raw;
    e.mtime = f.mtime;
    e.full = f.full;
    if (!f.composite && ids.size() == 1) {
        e.kind = 0;
        e.ref = ids[0];
    } else {
        e.kind = 1;
        e.ref = static_cast<uint32_t>(m.idx.comps.size());
        m.idx.comps.push_back(ids);
    }
    m.ents[f.key] = e;
    return true;
}

bool Writer::add(const std::string& key, const uint8_t* data, size_t size, int64_t mtime, const WriteOptions& opt,
                 std::string* err) {
    return commit(prepare(key, data, size, mtime, opt), err);
}

bool Writer::remove(const std::string& key) { return m_impl->ents.erase(normalizeKey(key)) > 0; }
bool Writer::hasKey(const std::string& key) const { return m_impl->ents.count(normalizeKey(key)) > 0; }

bool Writer::finish(std::string* err) {
#if defined(_WIN32)
    return failWith(err, "escrita de .tuf nao suportada no Windows");
#else
    Impl& m = *m_impl;
    IndexData d = m.idx;
    d.entries.clear();
    d.pool.clear();
    // Descarta composicoes orfas (de entradas trocadas ou removidas): so' as
    // vivas vao para o indice novo, com os numeros refeitos.
    std::vector<std::vector<uint32_t>> comps;
    for (const auto& kv : m.ents) {
        EntryRec e;
        e.pathHash = xxh64(kv.first.data(), kv.first.size());
        e.pathOff = static_cast<uint32_t>(d.pool.size());
        e.pathLen = static_cast<uint16_t>(kv.first.size());
        d.pool += kv.first;
        e.kind = kv.second.kind;
        e.raw = kv.second.raw;
        e.mtime = kv.second.mtime;
        e.full = kv.second.full;
        if (kv.second.kind == 0) {
            e.ref = kv.second.ref;
        } else {
            e.ref = static_cast<uint32_t>(comps.size());
            comps.push_back(m.idx.comps[kv.second.ref]);
        }
        d.entries.push_back(e);
    }
    d.comps = std::move(comps);
    std::string raw;
    serializeIndex(d, raw);
    std::vector<uint8_t> stored(raw.begin(), raw.end());
    uint64_t storedSize = raw.size();
#ifdef TUFO_HAVE_ZSTD
    {
        std::vector<uint8_t> z(ZSTD_compressBound(raw.size()));
        const size_t zn = ZSTD_compress(z.data(), z.size(), raw.data(), raw.size(), 19);
        if (!ZSTD_isError(zn) && zn < raw.size()) {
            z.resize(zn);
            stored.swap(z);
            storedSize = zn;
        }
    }
#endif
    uint64_t off = 0;
    if (!m.appendBytes(stored.data(), stored.size(), off, err)) return false;
    if (::fsync(m.fd) != 0) return failWith(err, "fsync falhou");
    Slot s;
    s.gen = m.generation + 1;
    s.indexOff = off;
    s.indexStored = storedSize;
    s.indexRaw = raw.size();
    s.indexXxh = xxh64(raw.data(), raw.size());
    const std::string slot = makeSlot(s);
    // O slot inativo e' o de paridade oposta a' da geracao anterior.
    if (!m.writeAt((s.gen & 1) * kSlotSize, slot.data(), slot.size(), err)) return false;
    if (::fsync(m.fd) != 0) return failWith(err, "fsync falhou");
    m.generation = s.gen;
    m.finished = true;
    return true;
#endif
}

// ---- repack ------------------------------------------------------------------

bool repack(const std::string& inPath, const std::string& outPath, std::string* err) {
    Archive a;
    if (!a.open(inPath, err)) return false;
    const Archive::Impl& ai = *a.m_impl;
    Writer w;
    if (!w.create(outPath, err)) return false;
    Writer::Impl& wi = *w.m_impl;
    // Dicionarios primeiro (mesmo id).
    for (const DictRec& d : ai.idx.dicts) {
        DictRec r = d;
        uint64_t off = 0;
        if (!wi.appendBytes(ai.base + d.off, static_cast<size_t>(d.size), off, err)) return false;
        r.off = off;
        wi.idx.dicts.push_back(r);
        wi.dictBytes.assign(ai.base + d.off, ai.base + d.off + d.size);
        wi.dictId = static_cast<uint16_t>(d.id);
    }
    std::vector<int64_t> remap(ai.idx.blobs.size(), -1);
    auto copyBlob = [&](uint32_t bi) -> bool {
        if (remap[bi] >= 0) return true;
        const BlobRec& b = ai.idx.blobs[bi];
        std::vector<uint8_t> chk(static_cast<size_t>(b.raw));
        if (!ai.decodeInto(b, chk.data(), true, err)) return false; // nao copia dado ruim
        BlobRec r = b;
        if (!wi.appendBytes(ai.base + b.off, static_cast<size_t>(b.stored), r.off, err)) return false;
        remap[bi] = static_cast<int64_t>(wi.idx.blobs.size());
        wi.known.emplace(BlobKey{r.h1, r.h2, r.raw}, static_cast<uint32_t>(wi.idx.blobs.size()));
        wi.idx.blobs.push_back(r);
        return true;
    };
    for (const EntryRec& e : ai.idx.entries) {
        Ent n{e.kind, 0, e.raw, e.mtime, e.full};
        if (e.kind == 0) {
            if (!copyBlob(e.ref)) return false;
            n.ref = static_cast<uint32_t>(remap[e.ref]);
        } else {
            std::vector<uint32_t> ids;
            for (uint32_t bi : ai.idx.comps[e.ref]) {
                if (!copyBlob(bi)) return false;
                ids.push_back(static_cast<uint32_t>(remap[bi]));
            }
            n.ref = static_cast<uint32_t>(wi.idx.comps.size());
            wi.idx.comps.push_back(std::move(ids));
        }
        wi.ents[std::string(ai.idx.pathOf(e))] = n;
    }
    return w.finish(err);
}

} // namespace tufo
