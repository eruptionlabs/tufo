# Tufo

[English](README.md) · **Português** · [日本語](README.ja.md)

O Tufo junta milhares de arquivos de um jogo em um único arquivo `.tuf` que funciona como uma **pasta virtual**. O nome vem de *tufo*, a rocha vulcânica que se forma quando a cinza solta de uma erupção se compacta e endurece numa pedra só. Um `.tuf` faz o mesmo com os arquivos soltos de um jogo.

Ele nasceu para a [Eruption Engine](https://github.com/eruptionlabs/eruption-engine), que lê os pacotes direto (`packs/*.tuf` ao lado do projeto), mas a biblioteca e a linha de comando não dependem do motor.

## Por que mais um formato de pacote

- **Acesso aleatório, sem bloco sólido.** Cada blob é comprimido sozinho (zstd). Pedir `assets/x/x.glb` descomprime só essa entrada; um mapa carrega de um pacote de vários gigabytes sem tocar no resto. Blobs que não ganham 3% (PNG, MP3) ficam crus e são copiados direto do mmap.
- **Deduplicação por conteúdo.** Dois caminhos com os mesmos bytes dividem um blob. O empacotador também parte o arquivo nos PNG que ele carrega por dentro (um `.glb` com texturas embutidas): a mesma imagem em dois modelos é guardada uma vez, e o arquivo volta idêntico, bit a bit.
- **Integridade.** Cada blob tem dois hashes XXH64 do conteúdo, cada entrada o XXH64 do arquivo inteiro e o índice o seu próprio. A leitura confere. O cabeçalho é guardado duas vezes.
- **Acréscimo seguro.** Os blobs novos vão no fim, depois o índice novo, e só então o slot inativo do cabeçalho passa a apontar para ele (com `fsync` antes). Parar no meio nunca perde o estado anterior. O `repack` recupera o espaço perdido.
- **Leitura em vários fios.** Depois do `open()` o arquivo é imutável e pode ser lido por muitos fios sem trava.

A especificação binária está em [docs/format.md](docs/format.md) (em inglês).

## Linha de comando

```bash
tufo pack   assets.tuf --base projeto projeto/assets   # cria a partir de pastas e arquivos
tufo add    assets.tuf --base extra extra/texturas     # acrescenta (cria se não existir)
tufo list   assets.tuf assets/external/x               # tamanho cru, guardado, segmentos, hash, caminho
tufo extract assets.tuf /tmp/saida assets/external/x   # tudo, ou só um arquivo/pasta
tufo cat    assets.tuf assets/external/x/x.json        # uma entrada na saída padrão
tufo rm     assets.tuf assets/external/velho/          # tira caminhos (o espaço volta no repack)
tufo verify assets.tuf --threads 8                     # lê e confere tudo; sai com 1 se algo falhar
tufo stats  assets.tuf                                 # tamanhos, dedupe, espaço perdido
tufo repack assets.tuf compacto.tuf                    # copia só o que está vivo para um arquivo novo
```

`tufo --help` lista todos os comandos com exemplos. Opções de `pack` e `add`: `--base <pasta>` (os caminhos dentro do arquivo contam a partir daqui), `--prefix <pasta>` (pasta virtual de destino), `--level N` (zstd, padrão 19, `0` guarda cru), `--threads N`, `--dict` (treina um dicionário com os arquivos pequenos), `--no-split` (não parte os arquivos nos PNG embutidos), `--overwrite`.

Os caminhos dentro do arquivo usam `/`, são ASCII em minúsculas e não podem ter `..`.

## Compilar

Precisa de compilador C++17, CMake 3.16 e libzstd (`libzstd-dev`).

```bash
cmake -S . -B build
cmake --build build -j
```

Opções: `TUFO_BUILD_CLI` e, para o zstd, `TUFO_ZSTD_INCLUDE` + `TUFO_ZSTD_LIB`, ou `TUFO_ZSTD_DECODER_TARGET` (um alvo que só descomprime: os arquivos podem ser lidos mas não gravados, que é o que o motor faz no Android e no navegador).

## Usar a biblioteca

```cmake
add_subdirectory(tufo)
target_link_libraries(meujogo PRIVATE tufo::tufo)
```

```cpp
#include "tufo/Archive.hpp"

tufo::Archive pack;
std::string err;
if (pack.open("assets.tuf", &err)) {
    std::vector<uint8_t> bytes;
    pack.read("assets/x/x.glb", bytes, /*verify=*/true, &err);
    for (const std::string& chave : pack.list("assets/x")) { /* ... */ }
}
```

`tufo/Ops.hpp` tem as operações de mais alto nível da linha de comando e das interfaces gráficas: árvore de pastas, extrair uma pasta, acrescentar do disco, remover.

## Dentro da Eruption Engine

O motor leva o Tufo como submódulo:

```bash
git clone --recurse-submodules https://github.com/eruptionlabs/eruption-engine.git
```

O motor monta cada `packs/*.tuf` do projeto como uma camada abaixo dos arquivos soltos, `eruption-engine --tuf-<comando>` roda os mesmos comandos do `tufo`, e o editor tem o painel **Janela > Pacotes (.tuf)**: árvore de pastas, prévia, extrair, adicionar, remover, verificar, repack e estatísticas.

## Licença

Apache-2.0, como o motor. Veja [LICENSE](LICENSE).
