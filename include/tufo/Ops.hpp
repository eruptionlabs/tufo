#pragma once
// Operacoes de alto nivel sobre um .tuf (tufo/Archive.hpp), compartilhadas
// pela linha de comando (tufo) e por interfaces (o painel "Pacotes" do editor da Eruption Engine): arvore de pastas, extrair arquivo ou pasta, acrescentar do disco,
// remover. Tudo aqui e' sincrono e sem interface; quem chama decide o fio e le'
// o Progress de outro fio. Os erros voltam em `err`.

#include "tufo/Archive.hpp"

#include <string>
#include <vector>

namespace tufo {

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
