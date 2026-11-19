# The .tuf format, version 1

A `.tuf` file ("Eruption Tuff Archive") is a read-optimized, append-safe archive that behaves as a **virtual folder**: a reader asks for `assets/x/x.glb` and receives only that entry, decompressed on the spot, without touching the rest of the file.

All integers are **little-endian**. All offsets are absolute byte offsets from the start of the file. `XXH64` is the 64-bit xxHash algorithm (Yann Collet, public domain) with seed 0 unless a seed is stated. Reference vectors: `XXH64("") = EF46DB3751D8E999`, `XXH64("abc") = 44BC2CF5AD770999`.

## 1. Layout

```
[0, 128)    header: two 64-byte slots
[128, ...)  data area: blobs and dictionaries, each starting at a multiple of 16,
            in the order they were appended; indexes of older generations stay
            in between as dead space
            the current index (somewhere in the data area, usually at the end)
```

Nothing in the file is "solid": every blob is compressed on its own, so reading one entry never decompresses its neighbours.

## 2. Header: two slots

The header holds two copies of a 64-byte slot, at offsets 0 and 64.

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `"ERUPTUF\0"` (`45 52 55 50 54 55 46 00`) |
| 8 | 2 | format version (`1`) |
| 10 | 2 | minor version (`0`, ignored by readers) |
| 12 | 4 | flags (`0`, reserved) |
| 16 | 8 | generation (starts at 1, +1 on every commit) |
| 24 | 8 | offset of the index |
| 32 | 8 | stored size of the index |
| 40 | 8 | raw (decompressed) size of the index |
| 48 | 8 | XXH64 of the raw index |
| 56 | 8 | XXH64 of bytes 0..55 of this slot |

**Choosing the slot.** A slot is valid when the magic and version match and its own XXH64 (bytes 56..63) is correct. The reader tries the valid slot with the highest generation first and falls back to the other one if the index it points to is outside the file, does not decompress, fails its XXH64, or fails the structural checks of section 3. A commit with generation `g` is written to slot `g & 1`, so the slot that held the previous state is never touched.

**The index.** If `stored size != raw size` the index is a single zstd frame that decompresses to `raw size` bytes; otherwise it is stored as is.

## 3. Index

```
u32 magic       = 0x58444954  ("TIDX")
u32 version     = 1
u32 blobCount
u32 compositeCount
u32 entryCount
u32 dictCount   (at most 64)
u32 reserved    = 0
dictionaries:   dictCount  x { u32 id, u64 offset, u64 size, u64 xxh64 }              28 bytes each
blobs:          blobCount  x { u64 offset, u64 stored, u64 raw, u8 codec, u8 flags,
                               u16 dictId, u32 reserved, u64 hash1, u64 hash2 }       48 bytes each
composites:     compositeCount x { u32 n, n x u32 blobIndex }
entries:        entryCount x { u64 pathHash, u16 pathLen, path bytes,
                               u8 kind, u8 flags, u32 ref, u64 rawSize,
                               i64 mtime (unix seconds), u64 fullHash }
```

Counts above 2^28 (blobs, composites, entries) are rejected as corrupt. Entries are sorted by the **bytes** of their path, strictly ascending, which allows binary search and prefix listing; a reader rejects an index that is not sorted.

### 3.1 Blobs

A blob is a byte range `[offset, offset + stored)` that decodes to `raw` bytes:

| `codec` | Meaning |
|---|---|
| 0 | raw: `stored == raw`, copy the bytes |
| 1 | zstd frame |
| 2 | zstd frame compressed with the dictionary `dictId` |

`hash1 = XXH64(raw bytes)` and `hash2 = XXH64(raw bytes, seed = 0x9E3779B97F4A7C15 ^ rawSize)`. Both are checked after decoding when the reader verifies. The pair together with `raw` is the **identity** of a blob, which is what deduplication uses. `flags` and `reserved` are `0`.

Every blob range must lie inside the file after the 128-byte header. A blob nobody references is dead space (see section 6).

### 3.2 Dictionaries

A dictionary is a zstd dictionary stored as raw bytes in the data area; `xxh64` is the XXH64 of those bytes and is checked when the file is opened. At most one dictionary is created per writer session, and it is only used for blobs of at most 64 KiB, and only when it makes the blob smaller than plain zstd.

### 3.3 Entries and path rules

`kind` 0: `ref` is a blob index; the entry is that blob. `kind` 1: `ref` is a composite index; the entry is the **concatenation** of the listed blobs, in order. `rawSize` is the size of the whole file and `fullHash` the XXH64 of the whole file; a verifying read checks it. `flags` is `0`. `pathHash` is the XXH64 of the path bytes and is checked by a full verification.

Paths are stored normalized:

- `\` becomes `/`;
- empty components and `.` disappear;
- a `..` component makes the path invalid (extractors therefore cannot be tricked into writing outside the destination);
- ASCII `A-Z` becomes `a-z` (only ASCII; other bytes are kept as they are);
- no leading `/`; the length fits in a `u16`.

## 4. Deduplication

Two paths with the same content point to the **same blob**: the writer looks up `(hash1, hash2, raw)` before storing a new one. Deduplication also works inside files through composite entries: when a file embeds PNG images (a glTF `.glb` with embedded textures, for example), the writer cuts the file into the pieces between the PNGs and the PNGs themselves, and stores a composite of those blobs. The same image in two different files then occupies a single blob, and reading the composite reassembles the original bytes: the file comes back bit for bit identical.

A byte range is recognized as an embedded PNG when it starts with the 8-byte PNG signature, the first chunk is an `IHDR` of length 13, and the chunks follow (length, type, data, CRC) up to an `IEND`. Only PNGs of at least 2048 bytes are cut out (`splitMinPng`). A file that is one single PNG is a plain blob, not a composite.

## 5. Compression policy (writer)

This is writer behaviour, not part of the format: readers decode whatever the codec says.

- Level 19 by default (`0` stores everything raw).
- A blob is kept raw unless zstd saves at least 3% (`minGain`), so already-compressed data (PNG, MP3) is copied straight from the memory map when read.
- For blobs of 64 KiB or more, a 64 KiB sample is first compressed at level 1; if even that does not gain 3%, the high level is skipped.
- Compression runs on many threads (`Writer::prepare` is thread-safe); committing is single-threaded and in order.

## 6. Appending, removing, repacking

**Append without rewriting.** To add or replace files the writer (1) appends the new blobs at the end, each aligned to 16 bytes, (2) appends the new index, (3) `fsync`s, (4) writes the **inactive** slot with generation + 1 pointing to the new index, (5) `fsync`s again. Until step 4 completes, the previous slot is intact and valid, so a crash or power loss at any point leaves the previous state readable. The old index and any replaced blobs become dead space.

**Remove.** Removing a path only writes a new index without it (same sequence). Its blobs become dead space unless another path still uses them.

**Repack.** `repack(in, out)` creates a new file with only live data: dictionaries first (same ids), then each live blob copied byte for byte (**not recompressed**; each blob is decoded and checked before it is copied, so bad data is never propagated), then the index. The original is never modified.

## 7. Verification

`Archive::read(key, out, verify=true)` checks the hashes of every blob it decodes and the entry's `fullHash`. `Archive::verifyAll` additionally walks every entry (path hashes included) and reports each failure. Opening an archive checks the header slot, the index hash, the structure of the index and the dictionary hashes, but does not read the blobs.

## 8. Reading from several threads

After `open()` an `Archive` is immutable: `exists`, `stat`, `read`, `list` and `listInfo` may run concurrently without locks. The file is memory-mapped.
