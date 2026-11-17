# P8PBASIC 設計方針

作成: 2026-11-17（MA2026A.D88 の実物解析とデトークナイズ完了を受けて）

根拠資料: `docs/MA2026-ANALYSIS.md`（ディスク形式）、
`docs/TOKEN-FORMAT.md`（言語仕様の実測）、`docs/HOST-CLI.md`（動作形態）。

## 目標

MA2026A.D88 が、P88SR + PC88.ROM なしで動くこと。
土台は P9PBASIC のインタプリタ本体、形態はまず Mac のヘッドレス CLI。

## 言語仕様として実装する範囲（実測ベース）

MA2026 の 2 プログラム（MENU.ALP / menu、全 334 行）が使うものだけを
対象にする。「PC-88 BASIC の全部」にはしない。

- 文: FOR/NEXT、IF/THEN/ELSE、WHILE/WEND、GOTO/GOSUB/RETURN（ラベル対応）、
  ON STOP GOSUB / STOP ON、READ/DATA/RESTORE（行番号・ラベル両方）、
  DIM、DEFSNG、PRINT、LOCATE、COLOR（通常形＋パレット形）、CLS [n]、
  SCREEN、LINE（BF・ラインパターン含む）、CLEAR ,上限アドレス、
  BLOAD "file",R、RUN "file"、END、POKE、OUT
- 関数: INP、PEEK、LEN、STR$、VAL、HEX$、RIGHT$（推定）、INT（推定）
- ラベル: 定義 `*名前`、参照は GOTO/GOSUB/RESTORE
- コメント: `'`（8F E9 形式）と REM
- 数値形式: トークン内の定数エンコーディング（0x0F/0x11–1A/0x1C/0x1D/0x1F）

P9PBASIC 本体との差分は主に:
1. トークナイザ／プログラム格納形式が PC-88 形式（リンク＋行番号）
2. ラベル対応（本体に無い）
3. WHILE/WEND、LOCATE、COLOR パレット形、CLS 引数、SCREEN の意味
4. ファイルが PC-88 Disk BASIC 形式ディスク上にあること

## メモリとデバイスのモデル

プログラムは 64KB 空間を PEEK/POKE/OUT/INP で直接触る。実機の
メモリマップを再現するのではなく、プログラムが実際に使う領域だけを
意味のある状態として持つ:

| 領域 | 役割 |
|---|---|
| &HC800– | 曲テーブル（4 バイト/曲）。MENU.ALP が構築、menu が参照 |
| &HCA00 / &H5500 / &H2000 / &H1000 / &HAA00 | FFDE で読み込まれるデータ置き場（MML オブジェクト、PCM、画像、PMD 本体相当） |
| &HFFF8 / &HFFF9 | 曲選択の状態（プログラム間共有） |
| &HFFFC / &HFFFD | ドライバモード（&HB1=YM2203 / &HB2=SB2） |
| &HAA20 / &HAA21 | PMD 再生状態（演奏中・ループ検出） |
| テキスト VRAM | TXR=&HF418 起点の直書き（1 行 120 バイト計算）を受ける |
| INP(0..10) | キーボードマトリクス（アクティブ Low） |
| OUT/INP ポート | SB 判定プローブ（&H44/&H45、&HA8/&HA9）と OUT 83,1（0x53） |

## NEOMSC と拡張命令の扱い（設計上の核心）

NEOMSC は Z80 機械語ドライバで、BLOAD ,R で実行され、FFDD/FFDE/FFDF の
拡張命令と PMD 再生を提供しているとみられる。P8PBASIC では:

- **Z80 はエミュレートしない**（当面）。BLOAD "NEOMSC",R は「ドライバ
  組み込み済み」として受理し、拡張命令 3 つをインタプリタに
  ネイティブ実装する。
- FFDE（ディスク直接読み＋ロール表示）は、D88 イメージのセクタ読みと
  テキスト表示として実装する。旧 RBYTE 形式の意味論と突き合わせて確定。
- FFDF（音楽制御）はネイティブな PMD 互換プレイヤへの命令として実装。
- FFDD（表示サービス）は引数の形から順に実装する（当面は曲情報登録と
  文字列表示を優先）。

## 音楽再生（PMD 互換）

- 演奏データは PMD のコンパイル済み MML オブジェクト（&HCA00 等に配置）。
  互換プレイヤを新規に書くのが最大の作業量。FM/SSG/ADPCM の発音までが範囲。
- 音源合成は YM2608（OPNA）相当: FM/SSG は ymfm を使う想定
  （RetroDOS コアで実績あり）。
- リズム音（ADPCM-A）は当面 `2608_rhythm.zip` の WAV 6 点を前提とし、
  供給部を差し替え可能な分離点にする（`docs/MA2026-ANALYSIS.md` §5）。
  WAV 自体は権利確認までリポジトリに入れない。
- ADPCM-B（PCM データ）は &H5500 配置のデータをそのまま使う。

## 段階計画

1. **言語コア**: PC-88 トークン形式のロード、ラベル、不足文・関数の追加。
   CLI で MENU.ALP/menu が文法エラーなく読み込めること。
2. **起動〜メニュー**: ディスク読み（FFDE 含む）、画面描画、キー入力で
   曲選択 UI が操作できること（無音でよい）。
3. **演奏**: FFDF 制御＋ PMD 互換プレイヤで FM/SSG が鳴ること。
4. **リズム／ADPCM**: リズム音（暫定 WAV）と PCM を含めて MA2026 が
   実機相当に演奏できること。

各段階は CLI の画面ダンプ／キー入力スクリプトで検証する
（`docs/HOST-CLI.md` の仕組みを使う）。
