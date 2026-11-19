#pragma once
// TUFO (.tuf, "Eruption Tuff Archive"): o arquivao compactado do projeto.
//
// Tufo e' a rocha que se forma quando a cinza de uma erupcao se compacta e
// endurece: milhoes de graos soltos (os arquivos do jogo) virando uma pedra so'.
// Funciona como uma PASTA VIRTUAL: o motor pede "assets/external/x/x.glb" e o
// arquivo entrega so' essa entrada, descomprimida na hora, sem tocar no resto.
//
// Principios (a especificacao binaria completa esta em docs/format.md):
//  - Nada de arquivo "solido": cada blob e' comprimido sozinho (zstd), entao ler
//    uma textura nao descomprime vizinhas. Blobs que nao ganham 3% ficam crus
//    (PNG, MP3) e o leitor os copia direto do mmap.
//  - Deduplicacao por conteudo: dois caminhos com os mesmos bytes apontam para o
//    MESMO blob. O empacotador tambem parte um arquivo ("entrada composta") nos
//    PNG que ele carrega por dentro (o .glb com texturas embutidas), para que a
//    mesma imagem em dois .glb ocupe um blob so'. A leitura remonta os bytes
//    originais: o .glb sai identico, bit a bit.
//  - Integridade: cada blob tem dois hashes XXH64 do conteudo cru (seeds
//    diferentes) e cada entrada o XXH64 do conteudo inteiro; a leitura confere.
//    O indice tem o seu XXH64 e o cabecalho e' duplicado (dois "slots").
//  - Acrescentar sem reescrever: os blobs novos vao no fim, depois o indice novo,
//    e so' entao o slot inativo do cabecalho passa a apontar para ele (fsync
//    antes). Parar no meio nunca perde o estado anterior. repack() compacta.
//
// Formato (little-endian), versao 1:
//   [0,128)   dois slots de 64 bytes: "ERUPTUF\0" | u16 versao | u16 menor |
//             u32 flags | u64 geracao | u64 pos do indice | u64 tamanho
//             guardado | u64 tamanho cru | u64 XXH64 do indice cru | u64 XXH64
//             dos 56 bytes anteriores. Vale o slot valido de maior geracao.
//   dados     blobs alinhados a 16 bytes (e os dicionarios), na ordem em que
//             foram acrescentados; entre eles o indice antigo vira lixo
//             recuperavel pelo repack.
//   indice    (zstd ou cru) "TIDX" | u32 versao | u32 blobs | u32 composicoes |
//             u32 entradas | u32 dicionarios | u32 reservado, depois:
//             dicionarios: u32 id | u64 pos | u64 tamanho | u64 XXH64
//             blobs: u64 pos | u64 guardado | u64 cru | u8 codec | u8 flags |
//                    u16 dicionario | u32 reservado | u64 hash1 | u64 hash2
//             composicoes: u32 n | n x u32 indice de blob
//             entradas (ordenadas pelos bytes do caminho): u64 hash do caminho |
//                    u16 tamanho | caminho | u8 tipo (0 blob, 1 composicao) |
//                    u8 flags | u32 referencia | u64 tamanho cru | i64 mtime (s)
//                    | u64 XXH64 do conteudo inteiro
//   codec: 0 cru, 1 zstd, 2 zstd com dicionario.
// Caminhos: '\\' vira '/', componentes vazios e "." somem, ".." invalida, A-Z
// vira a-z (so' ASCII).
//
// Threads: depois de open() o Archive e' imutavel; exists/stat/read/list podem
// rodar em varios fios sem trava. O Writer nao e' compartilhavel, exceto
// prepare(), que e' const e pode rodar em varios fios.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tufo {

constexpr char kMagic[8] = {'E', 'R', 'U', 'P', 'T', 'U', 'F', '\0'};
constexpr uint16_t kVersion = 1;
constexpr size_t kSlotSize = 64;
constexpr size_t kHeaderSize = 2 * kSlotSize;
constexpr size_t kAlign = 16;

enum Codec : uint8_t { kRaw = 0, kZstd = 1, kZstdDict = 2 };

// XXH64 (algoritmo de Yann Collet, dominio publico).
uint64_t xxh64(const void* data, size_t size, uint64_t seed = 0);
// Normaliza o caminho como o arquivo guarda; vazio se invalido.
std::string normalizeKey(const std::string& path);

struct Info {
    uint64_t rawSize = 0;
    uint64_t storedSize = 0; // soma dos blobs (compartilhados contam em cada entrada)
    uint64_t xxh64 = 0;      // do conteudo inteiro
    int64_t mtime = 0;       // segundos unix
    uint32_t segments = 1;   // >1: entrada composta
};

// Entrada com o que a interface mostra: o codec dos blobs e se algum deles e'
// dividido com outro caminho (deduplicado).
struct EntryInfo {
    std::string key;
    Info info;
    uint8_t codecMask = 0; // bit (1 << Codec) de cada codec usado pelos blobs da entrada
    bool deduped = false;
};

// Andamento de uma tarefa longa (verificar, extrair, empacotar), lido por outro
// fio; cancel e' pedido pelo chamador e a tarefa para na proxima entrada.
struct Progress {
    std::atomic<uint64_t> done{0}, total{0};
    std::atomic<bool> cancel{false};
};

struct Stats {
    uint64_t fileBytes = 0;      // tamanho do arquivo .tuf
    uint64_t entries = 0;        // caminhos
    uint64_t blobs = 0;
    uint64_t rawBytes = 0;       // soma dos tamanhos dos caminhos (sem dedupe)
    uint64_t uniqueRawBytes = 0; // soma dos blobs vivos (com dedupe)
    uint64_t storedBytes = 0;    // dados guardados dos blobs vivos
    uint64_t deadBytes = 0;      // blobs sem uso + indices antigos (o repack recupera)
    uint64_t composites = 0;
    uint64_t dedupedEntries = 0; // caminhos que dividem blob com outro
    uint64_t dicts = 0;
    uint64_t rawCodecBlobs = 0;
    uint64_t generation = 0;
};

class Archive {
public:
    Archive();
    ~Archive();
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    bool open(const std::string& path, std::string* err = nullptr);
    void close();
    bool isOpen() const;
    const std::string& path() const;

    bool exists(const std::string& key) const;
    bool stat(const std::string& key, Info* info) const;
    // Descomprime so' esta entrada. verify: confere os hashes dos blobs e o da
    // entrada (a custo de ~1 GB/s); falha de hash devolve false e preenche err.
    bool read(const std::string& key, std::vector<uint8_t>& out, bool verify = true, std::string* err = nullptr) const;
    // Chaves sob `dir` (recursivo), na ordem dos bytes. dir "" lista tudo.
    std::vector<std::string> list(const std::string& dir = std::string()) const;
    // Como list(), com os dados de cada entrada, calculados de uma vez (a conta
    // de blobs divididos e' feita uma vez so', nao uma por entrada).
    std::vector<EntryInfo> listInfo(const std::string& dir = std::string()) const;
    size_t fileCount() const;
    Stats stats() const;
    // Le' e confere TODA entrada, blob e o proprio indice. Devolve quantas
    // entradas falharam; `report` recebe uma linha por problema.
    size_t verifyAll(std::vector<std::string>* report, unsigned threads = 1, Progress* progress = nullptr) const;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
    friend class Writer;
    friend bool repack(const std::string&, const std::string&, std::string*);
};

// ---- escrita ---------------------------------------------------------------

struct WriteOptions {
    int level = 19;               // zstd; 0 = tudo cru
    size_t minDictSize = 0;       // reservado
    size_t dictMaxRaw = 64 * 1024; // so' blobs ate' este tamanho usam o dicionario
    bool splitPng = true;         // parte o arquivo nos PNG que ele carrega por dentro
    size_t splitMinPng = 2048;    // PNG menor que isto nao vale um blob
    double minGain = 0.03;        // guarda cru se nao ganhar isso
};

// Um arquivo pronto para entrar: blobs ja' comprimidos (ou so' o hash, se o
// blob ja' existia). Produzido por Writer::prepare (em varios fios) e consumido
// por Writer::commit (em um fio so').
struct PreparedBlob {
    uint64_t h1 = 0, h2 = 0, raw = 0;
    uint8_t codec = kRaw;
    uint16_t dict = 0;
    bool known = false;              // ja' no arquivo na hora do prepare: sem `stored`
    // Bytes guardados (comprimidos). shared_ptr: dois arquivos preparados ao
    // mesmo tempo com o mesmo conteudo dividem o resultado em vez de comprimir de novo.
    std::shared_ptr<const std::vector<uint8_t>> stored;
};
struct PreparedFile {
    std::string key;
    int64_t mtime = 0;
    uint64_t raw = 0, full = 0;
    bool composite = false;
    std::vector<PreparedBlob> blobs;
};

class Writer {
public:
    Writer();
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // create: arquivo novo (falha se existir e !overwrite). append: abre um
    // arquivo existente para acrescentar (le' o indice dele).
    bool create(const std::string& path, std::string* err, bool overwrite = false);
    bool append(const std::string& path, std::string* err);

    // Dicionario zstd para blobs pequenos (um por arquivo): treina de amostras
    // ou usa um pronto. So' vale para os blobs gravados depois.
    bool trainDictionary(const std::vector<std::vector<uint8_t>>& samples, size_t dictSize, std::string* err);

    PreparedFile prepare(const std::string& key, const uint8_t* data, size_t size, int64_t mtime,
                         const WriteOptions& opt) const;
    // Grava os blobs novos e liga o caminho a eles (troca a entrada existente).
    bool commit(PreparedFile&& f, std::string* err);
    bool add(const std::string& key, const uint8_t* data, size_t size, int64_t mtime, const WriteOptions& opt,
             std::string* err);
    bool remove(const std::string& key);
    bool hasKey(const std::string& key) const;
    // Escreve o indice novo e passa o slot do cabecalho para ele. Obrigatorio.
    bool finish(std::string* err);
    uint64_t dataBytes() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
    friend bool repack(const std::string&, const std::string&, std::string*);
};

// Copia so' o que esta' vivo para um arquivo novo (blobs sem uso e indices
// antigos ficam de fora). Os blobs sao copiados como estao (sem recomprimir).
bool repack(const std::string& inPath, const std::string& outPath, std::string* err);


} // namespace tufo
