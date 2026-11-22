#pragma once
// Operacoes de alto nivel sobre um .tuf (tufo/Archive.hpp), compartilhadas
// pela linha de comando (tufo) e por interfaces (o painel "Pacotes" do editor da Eruption Engine): arvore de pastas, extrair arquivo ou pasta, acrescentar do disco,
// remover. Tudo aqui e' sincrono e sem interface; quem chama decide o fio e le'
// o Progress de outro fio. Os erros voltam em `err`.

#include "tufo/Archive.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tufo {

// ---- texturas prontas para a GPU (formato versao 2, docs/format.md secao 9) --
//
// O Tufo nao sabe comprimir textura nem montar .glb: quem sabe (o motor)
// registra este codec antes de chamar a CLI ou o painel. Sem codec registrado,
// empacotar com gpuReady falha com mensagem clara, e extrair uma entrada
// kFlagGpuReady tambem (o .glb guardado nao tem as imagens).
struct GpuReadyCodec {
    using Companions = std::vector<std::pair<std::string, std::vector<uint8_t>>>;
    // Empacotar: `key` e' o caminho virtual do arquivo, `diskPath` onde ele esta'
    // no disco e `bytes` o conteudo. false = nao e' um arquivo que o codec trata (entra como e'). true =
    // `stored` e' o que se guarda no lugar do arquivo e `companions` as entradas
    // extras (caminho virtual, bytes) que ficam ao lado. `skip` recebe os caminhos virtuais
    // de arquivos que o pacote de texturas ja' cobre (imagens por uri): nao entram no .tuf.
    std::function<bool(const std::string& key, const std::string& diskPath, const std::vector<uint8_t>& bytes,
                       std::vector<uint8_t>& stored, Companions& companions, std::vector<std::string>& skip,
                       std::string* err)>
        pack;
    // Extrair: reconstroi o arquivo a partir do guardado e das entradas
    // companheiras do mesmo pacote. O resultado e' equivalente, nao identico
    // (as imagens voltam decodificadas do formato de bloco: com perda).
    std::function<bool(const std::string& key, const std::vector<uint8_t>& stored, const Archive& ar,
                       std::vector<uint8_t>& out, std::string* err)>
        unpack;
    // Arquivos que o pacote cobre e nao foram guardados (as imagens por uri do .glb), remontados
    // do formato de bloco (com perda): (caminho virtual, bytes). Vazio/ausente = nenhum.
    std::function<bool(const std::string& key, const std::vector<uint8_t>& stored, const Archive& ar,
                       std::vector<std::pair<std::string, std::vector<uint8_t>>>& files, std::string* err)>
        unpackExtras;
};
void setGpuReadyCodec(GpuReadyCodec codec);
const GpuReadyCodec* gpuReadyCodec(); // nullptr se ninguem registrou

// Le' a entrada como o usuario a quer ver: uma kFlagGpuReady volta remontada
// (com o codec), o resto como esta'. Falha com mensagem se faltar o codec.
bool readRestored(const Archive& ar, const std::string& key, std::vector<uint8_t>& out, std::string* err);

// Arvore virtual das entradas: pastas e arquivos, em ordem de nome.
struct TreeNode {
    std::string name;            // so' este componente ("" na raiz)
    std::string path;            // caminho completo (chave, ou pasta sem a barra final)
    bool dir = false;
    int entry = -1;              // indice em `entries` (arquivo)
    uint64_t raw = 0, stored = 0; // soma dos arquivos abaixo (pasta) ou da entrada
    uint32_t files = 0;          // arquivos abaixo (pasta) ou 1
    std::vector<TreeNode> children;
};
TreeNode buildTree(const std::vector<EntryInfo>& entries);

// "zstd", "raw", "zstd+dict", ou uma mistura ("raw+zstd") nas compostas.
std::string codecLabel(uint8_t codecMask);
// Tamanho legivel ("1.5 MB").
std::string humanSize(uint64_t bytes);
// As chaves que `target` cobre: a propria entrada, se for um arquivo, ou tudo
// abaixo dele, se for uma pasta (com ou sem "/" no fim).
std::vector<std::string> keysUnder(const Archive& ar, const std::string& target);

// Extrai `target` ("" = tudo) para `outDir`, mantendo a pasta virtual. Cada
// entrada e' conferida pelos hashes. Devolve quantos arquivos gravou; -1 se
// falhou (err) ou foi cancelado. As chaves ja' vem normalizadas (sem "..").
long extractTo(const Archive& ar, const std::string& target, const std::string& outDir, Progress* progress,
               std::string* err);

struct AddOptions {
    WriteOptions write;
    std::string base;      // pasta de onde o caminho virtual e' contado ("" = so' o nome)
    std::string prefix;    // pasta virtual onde os arquivos entram ("" = raiz)
    unsigned threads = 1;  // fios de compressao
    bool create = false;   // true: arquivo novo (falha se existe e !overwrite)
    bool overwrite = false;
    bool train = false;    // treina dicionario com os arquivos pequenos
    bool gpuReady = false; // .glb com imagens embutidas entram ja' comprimidos para a GPU (exige o codec)
};
// Acrescenta arquivos ou pastas do disco (recursivo) ao .tuf, criando-o se
// preciso. Seguro: o indice novo so' vale depois do ultimo byte gravado.
// Devolve quantos arquivos entraram; -1 se falhou ou foi cancelado (nada muda
// no que ja' estava no arquivo).
long addPaths(const std::string& tufPath, const std::vector<std::string>& paths, const AddOptions& opt,
              Progress* progress, std::string* err);

// Tira entradas e pastas ("pasta/" ou "pasta"); o espaco volta no repack.
// Devolve quantos caminhos saíram; -1 se falhou.
long removeTargets(const std::string& tufPath, const std::vector<std::string>& targets, std::string* err);

} // namespace tufo
