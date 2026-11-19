#pragma once
// Linha de comando do tufo: `tufo <comando> <arq.tuf> ...` ou, de dentro de um
// programa que reaproveita a biblioteca, `<programa> --tuf-<comando> <arq.tuf> ...`.
// Devolve o codigo de saida. `tufo --help` lista os comandos com exemplos.
namespace tufo {
int runCli(int argc, char** argv);
} // namespace tufo
