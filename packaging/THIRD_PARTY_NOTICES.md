# 第三者ソフトウェアのライセンス（Third-party notices）

SketchBRep は、次のオープンソースソフトウェアを使用しています。各ライセンスの全文は `licenses` フォルダにあります。`licenses/PACKAGES.md` には、同梱している各ライブラリの名前・版・ライセンスの一覧があります。

## 主なもの

| ソフトウェア | 用途 | ライセンス | 入手先 |
|---|---|---|---|
| Open CASCADE Technology (OCCT) | 形状の計算（B-rep） | GNU LGPL 2.1 + Open CASCADE の例外 | https://github.com/Open-Cascade-SAS/OCCT |
| Qt 6 | 画面（GUI） | GNU LGPL 3 | https://download.qt.io/official_releases/qt/ |
| openNURBS | Rhino（3DM）の読み書き。中に zlib・FreeType を含む | openNURBS のライセンス（MIT 形式） | https://github.com/mcneel/opennurbs |
| nlohmann/json | JSON | MIT | https://github.com/nlohmann/json |
| oneTBB | 並列処理（メモリ管理） | Apache 2.0 | https://github.com/uxlfoundation/oneTBB |
| ICU、FreeImage、FreeType、libpng、libjpeg、zlib、zstd、OpenEXR ほか | 画像・文字・圧縮 | 各ライセンス（PACKAGES.md 参照） | 各プロジェクト |
| Microsoft Visual C++ ランタイム | 実行に必要なランタイム | Microsoft の再配布可能コードのライセンス | Microsoft |

## GNU LGPL のライブラリについて

- OCCT と Qt は、別ファイルの DLL（共有ライブラリ）として同梱しており、SketchBRep 本体には静的に組み込んでいません。
- 利用者は、互換性のある版の DLL に差し替えて、SketchBRep を使用できます。
- 同梱した版のソースコードは、上の入手先から取得できます（版は `licenses/PACKAGES.md` に記載）。入手先から取得できない場合は、販売者に連絡すれば、同梱版のソースコードを提供します（配布から 3 年間）。
- 販売者は、これらのライブラリを改変していません。

## 商標

Rhino、SketchUp、AutoCAD、Jw_cad、Windows は、各社・各作者の商標または登録商標です。SketchBRep は、これらの製品の開発元とは関係ありません。
