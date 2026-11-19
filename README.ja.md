# Tufo

[English](README.md) · [Português](README.pt-BR.md) · **日本語**

Tufo は、ゲームの何千ものファイルを 1 つの `.tuf` アーカイブにまとめ、**仮想フォルダ**として扱えるようにします。名前はポルトガル語の *tufo*(凝灰岩)に由来します。噴火でばらばらに降った火山灰が固まって 1 つの岩になるように、`.tuf` はゲームの散らばったファイルを 1 つにまとめます。

[Eruption Engine](https://github.com/eruptionlabs/eruption-engine) のために作られており、エンジンはプロジェクトの隣の `packs/*.tuf` を直接読みます。ただしライブラリとコマンドラインはエンジンに依存しません。

## 特徴

- **ランダムアクセス、ソリッドブロックなし。** 各 blob は個別に圧縮(zstd)されます。`assets/x/x.glb` を要求すると、そのエントリだけが展開されます。数 GB のパックからでも、残りに触れずにマップを読み込めます。3% も縮まない blob(PNG、MP3)は無圧縮のまま保存され、メモリマップから直接コピーされます。
- **内容による重複排除。** 同じバイト列を持つ 2 つのパスは 1 つの blob を共有します。パッカーは埋め込み PNG(テクスチャ入りの `.glb` など)でファイルを分割するため、同じ画像が 2 つのモデルにあっても 1 回しか保存されず、元のファイルはビット単位で完全に復元されます。
- **整合性。** 各 blob に内容の XXH64 ハッシュが 2 つ、各エントリにファイル全体の XXH64、インデックスにも専用のハッシュがあり、読み込み時に検証されます。ヘッダーは 2 重に保存されます。
- **安全な追記。** 新しい blob を末尾に書き、次に新しいインデックスを書き、最後に非アクティブなヘッダースロットを切り替えます(前に `fsync`)。途中で止まっても以前の状態は失われません。`repack` で無駄な領域を回収できます。
- **スレッドセーフな読み込み。** `open()` の後、アーカイブは不変で、ロックなしで複数スレッドから読めます。

バイナリ仕様は [docs/format.md](docs/format.md)(英語)にあります。

## コマンドライン

```bash
tufo pack   assets.tuf --base project project/assets   # フォルダ/ファイルから作成
tufo add    assets.tuf --base extra extra/textures     # 追記(なければ作成)
tufo list   assets.tuf assets/external/x               # 元サイズ、保存サイズ、セグメント、ハッシュ、パス
tufo extract assets.tuf /tmp/out assets/external/x     # すべて、または 1 ファイル/フォルダだけ
tufo cat    assets.tuf assets/external/x/x.json        # 1 エントリを標準出力へ
tufo rm     assets.tuf assets/external/old/            # パスを削除(領域は repack で戻る)
tufo verify assets.tuf --threads 8                     # すべて読んで検証。失敗すると終了コード 1
tufo stats  assets.tuf                                 # サイズ、重複排除、無駄な領域
tufo repack assets.tuf compact.tuf                     # 生きているデータだけを新しいファイルへコピー
```

`tufo --help` で全コマンドと例が表示されます。`pack` と `add` のオプション: `--base <dir>`(アーカイブ内のパスの基準)、`--prefix <dir>`(追加先の仮想フォルダ)、`--level N`(zstd、既定 19、`0` は無圧縮)、`--threads N`、`--dict`(小さいファイルで辞書を学習)、`--no-split`(埋め込み PNG で分割しない)、`--overwrite`。

アーカイブ内のパスは `/` 区切り、ASCII の小文字で、`..` は使えません。

## ビルド

C++17 コンパイラ、CMake 3.16、libzstd(`libzstd-dev`)が必要です。

```bash
cmake -S . -B build
cmake --build build -j
```

オプション: `TUFO_BUILD_CLI`、zstd には `TUFO_ZSTD_INCLUDE` + `TUFO_ZSTD_LIB`、または `TUFO_ZSTD_DECODER_TARGET`(展開だけを行う既存ターゲット。読み込みのみ可能で、エンジンは Android とブラウザでこれを使います)。

## ライブラリとして使う

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

`tufo/Ops.hpp` には、コマンドラインとグラフィカルなフロントエンドが使う高レベルの操作(フォルダツリー、フォルダの展開、ディスクからの追加、削除)があります。

## Eruption Engine の中で

エンジンは Tufo をサブモジュールとして同梱します。

```bash
git clone --recurse-submodules https://github.com/eruptionlabs/eruption-engine.git
```

エンジンはプロジェクトの `packs/*.tuf` をすべて、散在するファイルの下のレイヤーとしてマウントします。`eruption-engine --tuf-<command>` は `tufo` と同じコマンドを実行し、エディターには **Window > Packages (.tuf)** パネル(フォルダツリー、プレビュー、展開、追加、削除、検証、repack、統計)があります。

## ライセンス

エンジンと同じ Apache-2.0。[LICENSE](LICENSE) を参照してください。
