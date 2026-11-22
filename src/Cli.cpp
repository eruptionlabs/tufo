// Ferramenta de linha de comando do arquivo .tuf (tufo/Archive.hpp).
// Dois jeitos de chamar: `tufo <comando> ...` ou `eruption-engine --tuf-<comando> ...`.
// A ajuda completa (com exemplos) esta' em helpText() / `tufo --help`.
#include "tufo/Archive.hpp"
#include "tufo/Ops.hpp"
#include "tufo/Cli.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tufo {

namespace fs = std::filesystem;

namespace {

std::string human(uint64_t b) { return humanSize(b); }

struct Args {
    std::vector<std::string> pos;
    std::string base;
    int level = 19;
    unsigned threads = 0;
    bool dict = false, noSplit = false, overwrite = false, help = false, gpuReady = false;
    std::string prefix;
};

Args parseArgs(int argc, char** argv, int from) {
    Args a;
    for (int i = from; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--base" && i + 1 < argc) a.base = argv[++i];
        else if (s == "--level" && i + 1 < argc) a.level = std::atoi(argv[++i]);
        else if (s == "--threads" && i + 1 < argc) a.threads = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (s == "--prefix" && i + 1 < argc) a.prefix = argv[++i];
        else if (s == "--help" || s == "-h") a.help = true;
        else if (s == "--dict") a.dict = true;
        else if (s == "--no-split") a.noSplit = true;
        else if (s == "--overwrite") a.overwrite = true;
        else if (s == "--gpu-ready") a.gpuReady = true;
        else a.pos.push_back(s);
    }
    if (a.threads == 0) a.threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    return a;
}

// Mostra o andamento numa linha enquanto a tarefa roda em outro fio.
struct Ticker {
    Progress& p;
    const char* what;
    std::atomic<bool> stop{false};
    std::thread th;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    Ticker(Progress& pr, const char* w) : p(pr), what(w) {
        th = std::thread([this] {
            while (!stop.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                print();
            }
        });
    }
    void print() {
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("\r%llu/%llu %s, %.0f s", (unsigned long long)p.done.load(), (unsigned long long)p.total.load(), what, s);
        std::fflush(stdout);
    }
    ~Ticker() {
        stop = true;
        th.join();
        print();
        std::printf("\n");
    }
};

int cmdPack(const std::string& tuf, const Args& a, bool create) {
    AddOptions o;
    o.write.level = a.level;
    o.write.splitPng = !a.noSplit;
    o.base = a.base;
    o.prefix = a.prefix;
    o.threads = a.threads;
    o.create = create;
    o.overwrite = a.overwrite;
    o.train = a.dict;
    o.gpuReady = a.gpuReady;
    Progress prog;
    std::string err;
    long n;
    const auto t0 = std::chrono::steady_clock::now();
    {
        Ticker tick(prog, "arquivos");
        n = addPaths(tuf, a.pos, o, &prog, &err);
    }
    if (n < 0) {
        std::fprintf(stderr, "erro: %s\n", err.c_str());
        return err == "no files to add" ? 2 : 1;
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("feito em %.1f s\n", s);
    Archive ar;
    if (ar.open(tuf, &err)) {
        const Stats st = ar.stats();
        std::printf("%llu entradas, %s crus -> %s no arquivo (%s sem lixo)\n", (unsigned long long)st.entries,
                    human(st.rawBytes).c_str(), human(st.fileBytes).c_str(), human(st.fileBytes - st.deadBytes).c_str());
    }
    return 0;
}

int cmdList(const std::string& tuf, const Args& a) {
    Archive ar;
    std::string err;
    if (!ar.open(tuf, &err)) {
        std::fprintf(stderr, "erro: %s\n", err.c_str());
        return 1;
    }
    for (const std::string& k : ar.list(a.pos.empty() ? "" : a.pos[0])) {
        Info i;
        ar.stat(k, &i);
        std::printf("%12llu %12llu %2u %016llx %s%s\n", (unsigned long long)i.rawSize, (unsigned long long)i.storedSize, i.segments,
                    (unsigned long long)i.xxh64, k.c_str(),
                    (i.flags & kFlagGpuReady) ? "  [gpu-ready]" : (i.flags & kFlagCompanion) ? "  [texturas da gpu]" : "");
    }
    return 0;
}

int cmdExtract(const std::string& tuf, const Args& a) {
    if (a.pos.empty()) {
        std::fprintf(stderr, "uso: --tuf-extract <arq.tuf> <pasta> [prefixo]\n");
        return 2;
    }
    Archive ar;
    std::string err;
    if (!ar.open(tuf, &err)) {
        std::fprintf(stderr, "erro: %s\n", err.c_str());
        return 1;
    }
    const std::string prefix = a.pos.size() > 1 ? a.pos[1] : "";
    Progress prog;
    const long n = extractTo(ar, prefix, a.pos[0], &prog, &err);
    if (n < 0) {
        std::fprintf(stderr, "erro: %s\n", err.c_str());
        return 1;
    }
    std::printf("%ld arquivos extraidos para %s\n", n, a.pos[0].c_str());
    return 0;
}

int cmdCat(const std::string& tuf, const Args& a) {
    Archive ar;
    std::string err;
    std::vector<uint8_t> d;
    if (a.pos.empty() || !ar.open(tuf, &err) || !readRestored(ar, a.pos[0], d, &err)) {
        std::fprintf(stderr, "erro: %s\n", err.c_str());
        return 1;
    }
    std::fwrite(d.data(), 1, d.size(), stdout);
    return 0;
}

int cmdRm(const std::string& tuf, const Args& a) {
    std::string err;
    const long n = removeTargets(tuf, a.pos, &err);
    if (n < 0) {
        std::fprintf(stderr, "erro: %s\n", err.c_str());
        return 1;
    }
    std::printf("%ld caminhos removidos (rode repack para recuperar o espaco)\n", n);
    return 0;
}

int cmdVerify(const std::string& tuf, const Args& a) {
    Archive ar;
    std::string err;
    if (!ar.open(tuf, &err)) {
        std::fprintf(stderr, "FALHOU: %s\n", err.c_str());
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> rep;
    Progress prog;
    size_t bad;
    {
        Ticker tick(prog, "entradas");
        bad = ar.verifyAll(&rep, a.threads, &prog);
    }
    for (const std::string& r : rep) std::printf("RUIM %s\n", r.c_str());
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%zu entradas conferidas em %.1f s: %s\n", ar.fileCount(), s, bad ? "FALHOU" : "tudo confere");
    return bad ? 1 : 0;
}

int cmdStats(const std::string& tuf) {
    Archive ar;
    std::string err;
    if (!ar.open(tuf, &err)) {
        std::fprintf(stderr, "erro: %s\n", err.c_str());
        return 1;
    }
    const Stats s = ar.stats();
    std::printf("arquivo: %s (geracao %llu)\n", human(s.fileBytes).c_str(), (unsigned long long)s.generation);
    std::printf("caminhos: %llu, blobs vivos+mortos: %llu, composicoes: %llu, dicionarios: %llu\n", (unsigned long long)s.entries,
                (unsigned long long)s.blobs, (unsigned long long)s.composites, (unsigned long long)s.dicts);
    std::printf("tamanho cru somado dos caminhos: %s\n", human(s.rawBytes).c_str());
    std::printf("tamanho cru unico (apos dedupe): %s (%.1f%% do somado)\n", human(s.uniqueRawBytes).c_str(),
                s.rawBytes ? 100.0 * double(s.uniqueRawBytes) / double(s.rawBytes) : 0.0);
    std::printf("guardado (apos zstd): %s (%.2fx sobre o unico)\n", human(s.storedBytes).c_str(),
                s.storedBytes ? double(s.uniqueRawBytes) / double(s.storedBytes) : 0.0);
    std::printf("blobs crus (nao comprimiram): %llu; caminhos que dividem blob: %llu\n", (unsigned long long)s.rawCodecBlobs,
                (unsigned long long)s.dedupedEntries);
    std::printf("espaco perdido (recuperavel no repack): %s\n", human(s.deadBytes).c_str());
    return 0;
}

const char* helpText() {
    return
        "tufo: arquivo .tuf (Eruption Tuff Archive), pasta virtual compactada.\n"
        "\n"
        "Uso: tufo <comando> <arq.tuf> [argumentos]\n"
        "     eruption-engine --tuf-<comando> <arq.tuf> [argumentos]   (atalho dentro do motor)\n"
        "\n"
        "Comandos:\n"
        "  pack    <arq.tuf> [--base <pasta>] <caminho>...   cria o arquivo com arquivos e pastas\n"
        "  add     <arq.tuf> [--base <pasta>] <caminho>...   acrescenta (cria se nao existir)\n"
        "  list    <arq.tuf> [prefixo]                       tamanho cru, guardado, segmentos, hash, chave\n"
        "  extract <arq.tuf> <pasta> [prefixo]               extrai tudo, ou so' o arquivo/pasta do prefixo\n"
        "  cat     <arq.tuf> <chave>                         escreve uma entrada na saida padrao\n"
        "  rm      <arq.tuf> <chave|pasta/>...               tira caminhos (o espaco volta no repack)\n"
        "  verify  <arq.tuf> [--threads N]                   le' e confere tudo; sai com 1 se algo falhar\n"
        "  stats   <arq.tuf>                                 tamanhos, dedupe, espaco perdido\n"
        "  repack  <entrada.tuf> <saida.tuf>                 copia so' o que esta' vivo para um arquivo novo\n"
        "\n"
        "Opcoes de pack/add:\n"
        "  --base <pasta>   o caminho dentro do arquivo e' contado a partir daqui (senao, so' o nome)\n"
        "  --prefix <pasta> pasta virtual onde os arquivos entram\n"
        "  --level N        nivel do zstd (padrao 19; 0 = tudo cru)\n"
        "  --threads N      fios de compressao (padrao: metade dos nucleos)\n"
        "  --dict           treina um dicionario com os arquivos pequenos\n"
        "  --no-split       nao parte os arquivos nos PNG embutidos\n"
        "  --overwrite      pack sobre um arquivo que ja existe\n"
        "  --gpu-ready      .glb com imagens embutidas entram ja' comprimidas para a GPU (so' no\n"
        "                   eruption-engine --tuf-pack; formato versao 2, ver docs/format.md secao 9)\n"
        "\n"
        "Exemplos:\n"
        "  tufo pack assets.tuf --base projeto projeto/assets\n"
        "  tufo add assets.tuf --base novo novo/texturas --level 12\n"
        "  tufo list assets.tuf assets/external/x\n"
        "  tufo extract assets.tuf /tmp/saida assets/external/x\n"
        "  tufo cat assets.tuf assets/external/x/x.json\n"
        "  tufo rm assets.tuf assets/external/velho/\n"
        "  tufo repack assets.tuf assets_compacto.tuf\n"
        "  tufo verify assets.tuf --threads 8 && tufo stats assets.tuf\n"
        "\n"
        "Chaves: '/' como separador, minusculas (so' ASCII), sem '..'. Formato: docs/format.md.\n"
        "Na Eruption Engine, Janela > Pacotes (.tuf) faz o mesmo com interface grafica.\n";
}

bool isCommand(const std::string& c) {
    static const char* names[] = {"pack", "add", "list", "extract", "cat", "rm", "verify", "stats", "repack"};
    for (const char* n : names)
        if (c == n) return true;
    return false;
}

} // namespace

int runCli(int argc, char** argv) {
    int at = -1;
    std::string cmd;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--tuf-", 6) == 0) {
            at = i;
            cmd = argv[i] + 6;
            break;
        }
    if (at < 0 && argc > 1 && isCommand(argv[1])) {
        at = 1;
        cmd = argv[1];
    }
    const bool wantHelp = at < 0 ? (argc <= 1 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h" ||
                                    std::string(argv[1]) == "help")
                                 : (cmd == "help" || cmd == "-h" || cmd == "-help");
    if (wantHelp) {
        std::fputs(helpText(), stdout);
        return 0;
    }
    if (at < 0 || at + 1 >= argc) {
        if (at >= 0 && isCommand(cmd)) std::fprintf(stderr, "falta o arquivo .tuf: veja tufo --help\n");
        else std::fputs(helpText(), stderr);
        return 2;
    }
    const std::string tuf = argv[at + 1];
    if (tuf == "--help" || tuf == "-h") {
        std::fputs(helpText(), stdout);
        return 0;
    }
    const Args a = parseArgs(argc, argv, at + 2);
    if (cmd == "pack") return cmdPack(tuf, a, true);
    if (cmd == "add") return cmdPack(tuf, a, false);
    if (cmd == "list") return cmdList(tuf, a);
    if (cmd == "extract") return cmdExtract(tuf, a);
    if (cmd == "cat") return cmdCat(tuf, a);
    if (cmd == "rm") return cmdRm(tuf, a);
    if (cmd == "verify") return cmdVerify(tuf, a);
    if (cmd == "stats") return cmdStats(tuf);
    if (cmd == "repack") {
        std::string err;
        if (a.pos.empty() || !repack(tuf, a.pos[0], &err)) {
            std::fprintf(stderr, "erro: %s\n", err.c_str());
            return 1;
        }
        std::printf("repack: %s\n", a.pos[0].c_str());
        return cmdStats(a.pos[0]);
    }
    std::fprintf(stderr, "comando desconhecido: --tuf-%s\n", cmd.c_str());
    return 2;
}

} // namespace tufo
