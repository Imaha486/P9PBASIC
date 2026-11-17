# PC-88 N88-BASIC トークン形式（MA2026A.D88 の実物から実証）

作成: 2026-11-17 / 対象: MENU.ALP（69 行）、menu（265 行）を全行デトークナイズして検証

PC-98（N88-BASIC(86)）版とはコード表が異なる。以下はすべて実物との
突き合わせで確認したもの。デトークナイザは `tools/p88_detok.py`、
抽出器は `tools/p88_extract.py`。

## 行形式

```
[link u16 LE][行番号 u16 LE][本体][0x00]
```

- link は次行のファイル先頭からのオフセット。ただし実物では ±1
  ずれる行があるため、パーサは link±2 の範囲で妥当な次ヘッダを探す。
- 本体は MS-BASIC 系トークン列。変数名は素の ASCII（型接尾辞 $ % ! # 付き）。

## 数値・記号

| コード | 意味 |
|---|---|
| 0x0B | &O + u16 LE |
| 0x0C | &H + u16 LE |
| 0x0E | 行番号参照 + u16 LE |
| 0x0F | 1 バイト整数 |
| 0x11–0x1A | 数字 0–9 |
| 0x1C | int16 LE |
| 0x1D | 単精度（MBF 4 バイト） |
| 0x1F | 倍精度（MBF 8 バイト） |
| 0xF0–0xFE | 演算子: > = < + - * / ^ AND OR XOR EQV IMP MOD \ |
| 0xF5 | 式中では * （乗算）。GOTO/GOSUB/RESTORE の引数位置ではラベル参照マーカー（直後にラベル名 ASCII） |

## キーワード（1 バイト、抜粋ではなく実測で埋まった分）

GW-BASIC の並びが土台で、PC-88 拡張が C0–FE に入る。

- 0x81 END / 0x82 FOR / 0x83 NEXT / 0x84 DATA / 0x85 INPUT / 0x86 DIM /
  0x87 READ / 0x88 LET / 0x89 GOTO / 0x8A RUN / 0x8B IF / 0x8C RESTORE /
  0x8D GOSUB / 0x8E RETURN / 0x90 STOP / 0x91 PRINT / 0x92 CLEAR /
  0x95 ON / 0x97 DEF / 0x98 POKE / 0x9A OUT / 0x9D CONSOLE / 0x9E WIDTH /
  0x9F ELSE / 0xAB DEFSNG
- 0x8F 0xE9 = アポストロフィコメント（行末まで素テキスト）
- 0xAE LINE / 0xAF WHILE / 0xB0 WEND
- 0xCB COLOR（`COLOR 7,0` 形式と `COLOR=(n,c)` パレット形式の両方） /
  0xCE CLS（引数付きあり） / 0xD3 SCREEN / 0xD4 BLOAD / 0xD6 LOCATE /
  0xDC TO / 0xDD THEN

## 関数（0xFF + 1 バイト）

| コード | 意味 | 根拠 |
|---|---|---|
| FF82 | RIGHT$（推定） | `RIGHT$("  "+STR$(I),3)` の形で番号を右詰め表示 |
| FF90 | INP | キーボードマトリクス（0–10）とポート読みに使用 |
| FF92 | LEN | タイトル右寄せ計算 `79-LEN(A$)` |
| FF93 | STR$ | 数値を文字列化して連結 |
| FF94 | VAL | `VAL("&h"+PT$)` |
| FF97 | PEEK | &HFFF8 等の状態バイト、曲テーブル &HC800 読み |
| FF9A | HEX$ | 数値→16 進文字列（VAL と対で使用） |
| FF85 | INT（推定） | 中央寄せ計算 `(80-LEN(A$)*X)/2` の結果 |

## ラベル

- 定義: 行頭に `*名前`（素の ASCII）。例: `*STP` `*BGMP` `*SELECT` `*STAFF`
- 参照: GOTO / GOSUB / RESTORE の引数に `*名前`（0xF5 + 名前で格納）
- P9PBASIC 本体はラベル非対応（Rance 対応時に行番号へ変換して回避した）。
  P8PBASIC では本体にラベル対応が要る。

## 拡張命令（NEOMSC ドライバ提供とみられる 3 命令）

いずれも 0xFF + 1 バイトの文トークン。名前は不明のためコードで呼ぶ。
引数の形だけ実測で分かっている。意味の確定は実装時の課題。

| コード | 引数の形（実測） | 推定される役割 |
|---|---|---|
| FFDD | 先頭が機能番号のことが多い: (9,addr) (8,addr) (6,1,1,24,140,"題") (5,3,1,41,6; A-1,0) (7,3,0,x,y; A-1,3) (1,3,1,x,y; A-1,3) (4,3,0,74,17,"ended ") / 例外 (C,T,B,X,Y,A$) | 画面パターン・文字表示系サービス。MENU 162 行の (C,T,B,X,Y,A$) は曲情報の登録か |
| FFDE | (0,drv,trk,sec,cnt,addr) (1,0,ST,RT) (2,addr,len,addr) / 機能 3 系: (3,10) (3,11,n) (3,9,addr) (3,8) (3,col,文字列) | ディスク直接読み（旧コードのコメントでは RBYTE 相当）と、機能 3 = スタッフロール文字表示 |
| FFDF | 1–2 文字のコマンド名 + 任意引数: S / P / F,速度 / E,12 / E,15 / ES,0 / R,addr / K,addr | 音楽ドライバ制御。P=再生、S=停止、F=フェードアウトは文脈からほぼ確実 |

## MA2026 プログラムの構造（読み解けた全体像）

- 起動は MENU.ALP: メモリ初期化（CLEAR ,&H97FF）、画面初期化、
  `BLOAD "NEOMSC",R` で Z80 ドライバを読み込み実行。
- サウンドボード判定: OUT/INP で SB2（YM2608）を探し、無ければ
  YM2203 モード（SB=1）。結果は POKE で &HFFFD に &HB1/&HB2 を書く。
- PMD データ読み: FFDE で予約領域から直接セクタ読み
  （&HAA00 へ PMD 本体相当、&HCA00 へ MML オブジェクト、&H5500 へ PCM）。
  旧方式（BLOAD "pmd" / RBYTE）はコメントアウトされて残っている。
- 曲テーブルは &HC800 に 4 バイト/曲（トラック、セクタ、セクタ数、予備）。
  MENU.ALP が DATA 文から組み立て、menu と共有する。
- menu が UI 本体: 曲リスト表示（LOCATE/PRINT）、キーボードマトリクス
  （INP(0..10)）による選択・再生操作、FFDF でドライバ制御、
  スタッフロール（RESTORE *STAFF + FFDE 機能 3）。
- 状態共有はメモリ直書き: &HFFF8 = 次に再生する曲、&HFFF9 = 現在の曲、
  &HFFFC/&HFFFD = ドライバモード、&HAA20/&HAA21 = PMD 再生状態。
- 画面はテキスト VRAM 直書き（TXR=&HF418、1 行 120 バイト計算）や
  LINE によるグラフィック描画（タイトル帯）を併用する。
