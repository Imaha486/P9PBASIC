# P9PBASIC

> **P9PBASIC は、PC-98 の BIOS ROM の逆アセンブルや、N88日本語BASIC(86) のコード盗用は一切行っていません。**
> **すべて C 言語で新規に書いたプログラムです。そのため、実機の BIOS ROM が無くても EXE ひとつだけで動作します。**
>
> 画面・キーボード・グラフィック・ビープは、BIOS を一度も呼ばずに I/O ポートと VRAM を直接操作しています。
> 必要なのは MS-DOS のファンクションコール（INT 21h：ファイル・メモリ・時計）だけなので、
> BIOS ROM を持たないエミュレータでも、MS-DOS の機能さえあれば動きます。

PC-9801 シリーズの MS-DOS 上で動く、**N88日本語BASIC(86) 互換の BASIC インタプリタ**です。
単一の実行ファイル `P9PBASIC.EXE` だけで動きます。

## 特徴
- 引数なしで起動するとエディトモード（スクリーンエディタ）。画面上の行にカーソルを合わせて RETURN で取り込む、おなじみの操作です
- 引数に `.BAS`（ASCII 形式）を指定すると、そのまま実行します
- `GOTO *MAIN` のようなラベルに対応
- キー入力は自前の IRQ1 割り込みで 8251 から直接受け取ります。`INKEY$` のほか、
  `INP(&H41)`、キーマトリクス `INP(&HE0)`〜`INP(&HEC)`、キー状態テーブル `PEEK(&H52A)` に対応
- 640×400・16 色のグラフィック命令（LINE / CIRCLE / PAINT / GET@ / PUT@ など）
- シーケンシャルファイル、BSAVE / BLOAD、`MKS$` / `CVS` などの MBF 形式に対応
- Shift_JIS の漢字表示（テキスト VRAM に JIS コードで書き込み）

## 動作環境
- PC-9801 シリーズ（8086 / V30 以降）、または PC-98 エミュレータ（[PC98PLAYER](https://github.com/Imaha486/PC98PLAYER) など）
- MS-DOS 3.x 以降
- 16 色表示にはアナログパレット（PC-9801VM 以降の 16 色対応機）が必要です

## 使い方
```
P9PBASIC                 エディトモードで起動
P9PBASIC GAME            GAME.BAS を読み込んで実行（終了すると MS-DOS に戻る）
P9PBASIC GAME /K         実行後もエディトモードに残る
P9PBASIC GAME /R         INP(&H41) を実ポート直読みにする
P9PBASIC GAME /G         80h〜9Fh・E0h〜FFh を半角グラフィック文字として表示（PC-8801 用プログラム向け）
```
詳しくは [docs/MANUAL.md](docs/MANUAL.md) を見てください。

## ダウンロード
[Releases](../../releases) から次の 2 つを入手できます。
- `P9PBASIC_verYYMMDD.zip` … 実行ファイル、マニュアル、サンプル
- `P9PBASIC_verYYMMDD_src.zip` … ソース一式

## ビルド
[Open Watcom 2.0](https://github.com/open-watcom/open-watcom-v2) が必要です（16 ビット DOS、ラージモデル、8086 命令）。
- Windows：環境変数 `WATCOM` に Open Watcom のフォルダを設定して `build.bat`
- Linux：`./build.sh`

## フォルダ構成
| パス | 内容 |
|---|---|
| `src/` | インタプリタ本体（C 言語）。PC-98 ハードウェア層は `src/hal98.c` |
| `samples/` | サンプルプログラム（Shift_JIS、CR+LF） |
| `docs/MANUAL.md` | マニュアル |
| `tools/pc98sim.py` | 開発用の PC-98＋MS-DOS 最小試験台（Python、unicorn を使用） |
| `build.bat` / `build.sh` | ビルド用スクリプト |

## ライセンス
MIT License（[LICENSE](LICENSE)）。
実行ファイルには Open Watcom の C ランタイムライブラリが含まれます。

## 作者
Imaha486
