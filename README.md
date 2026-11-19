# Tufo

**English** · [Português](README.pt-BR.md) · [日本語](README.ja.md)

Tufo packs thousands of game files into one `.tuf` archive that works as a **virtual folder**. The name comes from *tufo*, Portuguese for tuff: the volcanic rock that forms when loose ash from an eruption compacts and hardens into one stone. A `.tuf` does the same with the loose files of a game.

It is made for [Eruption Engine](https://github.com/eruptionlabs/eruption-engine), which reads packs directly (`packs/*.tuf` next to a project), but the library and the command line have no dependency on the engine.

## Why another archive

- **Random access, no solid blocks.** Every blob is compressed alone (zstd). Asking for `assets/x/x.glb` decompresses only that entry; a map loads from a multi-gigabyte pack without touching the rest. Blobs that do not gain 3% (PNG, MP3) stay raw and are copied straight from the memory map.
- **Deduplication by content.** Two paths with the same bytes share one blob. The packer also splits a file at the PNG images it embeds (a `.glb` with embedded textures), so the same image in two models is stored once, and the file still comes back bit for bit identical.
- **Integrity.** Each blob has two XXH64 hashes of its contents, each entry the XXH64 of the whole file, and the index its own. Reads verify them. The header is stored twice.
- **Safe appending.** New blobs go at the end, then the new index, then the inactive header slot flips (with `fsync` before). Stopping halfway never loses the previous state. `repack` reclaims the dead space.
- **Thread-safe reading.** After `open()` an archive is immutable and can be read from many threads without locks.

The binary specification is in [docs/format.md](docs/format.md).

## Command line

```bash
tufo pack   assets.tuf --base project project/assets   # create from folders and files
tufo add    assets.tuf --base extra extra/textures     # append (creates if missing)
tufo list   assets.tuf assets/external/x               # raw size, stored size, segments, hash, path
tufo extract assets.tuf /tmp/out assets/external/x     # everything, or only a file/folder
tufo cat    assets.tuf assets/external/x/x.json        # one entry to standard output
tufo rm     assets.tuf assets/external/old/            # remove paths (space returns on repack)
tufo verify assets.tuf --threads 8                     # read and check everything; exit 1 on failure
tufo stats  assets.tuf                                 # sizes, dedupe, wasted space
tufo repack assets.tuf compact.tuf                     # copy only what is alive into a new file
```

`tufo --help` lists every command with examples. Options of `pack` and `add`: `--base <dir>` (paths inside the archive are counted from here), `--prefix <dir>` (virtual folder to add into), `--level N` (zstd, default 19, `0` stores raw), `--threads N`, `--dict` (train a dictionary on small files), `--no-split` (do not split files at embedded PNGs), `--overwrite`.

Paths inside an archive use `/`, are lowercase ASCII, and cannot contain `..`.

## Building

Needs a C++17 compiler, CMake 3.16 and libzstd (`libzstd-dev`).

```bash
cmake -S . -B build
cmake --build build -j
```

Options: `TUFO_BUILD_CLI`, and for zstd `TUFO_ZSTD_INCLUDE` + `TUFO_ZSTD_LIB`, or `TUFO_ZSTD_DECODER_TARGET` (an existing target that only decompresses: archives can then be read but not written, which is what the engine does on Android and in the browser).

## Using the library

```cmake
add_subdirectory(tufo)
target_link_libraries(mygame PRIVATE tufo::tufo)
```

```cpp
#include "tufo/Archive.hpp"

tufo::Archive pack;
std::string err;
if (pack.open("assets.tuf", &err)) {
    std::vector<uint8_t> bytes;
    pack.read("assets/x/x.glb", bytes, /*verify=*/true, &err);
    for (const std::string& key : pack.list("assets/x")) { /* ... */ }
}
```

`tufo/Ops.hpp` has the higher-level operations used by the command line and by graphical front ends: folder tree, extract a folder, add from disk, remove.

## Inside Eruption Engine

The engine ships Tufo as a submodule:

```bash
git clone --recurse-submodules https://github.com/eruptionlabs/eruption-engine.git
```

The engine mounts every `packs/*.tuf` of a project as a layer under the loose files, `eruption-engine --tuf-<command>` runs the same commands as `tufo`, and the editor has a **Window > Packages (.tuf)** panel: folder tree, preview, extract, add, remove, verify, repack and statistics.

## License

Apache-2.0, like the engine. See [LICENSE](LICENSE).
