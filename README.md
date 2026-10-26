# Sq_Repextend

After Effects 用エフェクトプラグインです。Nuke の EdgeExtend のように、レイヤーのアルファ領域を広げて、広げた部分を最寄りのエッジピクセルと同じ色で埋めます。広げる量は CC Repetile と同じ感覚の Expand Right / Left / Down / Up で指定します。

## 動作環境

- Windows 10 / 11
- After Effects 2025 以降

## インストール

1. [Releases](https://github.com/Sqkurq/Sq_Repextend/releases) から `Sq_Repextend.aex` をダウンロードします
2. After Effects を終了し、次のフォルダにコピーします（管理者権限が必要です）

   ```
   C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\
   ```

3. After Effects を起動し、**エフェクト > Sq > Sq_Repextend** から適用します

## パラメーター

| 名前 | 内容 |
|---|---|
| Expand Right / Left / Down / Up | 各方向へ伸ばす距離（px） |
| Expand All | 4方向すべてに加算する距離 |
| Shape | 伸ばす形（Square / Circle / Diamond） |
| Alpha Threshold | このアルファ値以上のピクセルを色の元として扱う |
| Output | Original Over Extension：元画像を伸長部分の上に合成<br>Replace Below Threshold：しきい値未満のピクセルを置き換え<br>Extension Only：伸長部分だけを出力 |
| Opaque Fill | 伸長部分を不透明にする |
| Expand Layer Bounds | レイヤー範囲の外まで広げる |

## ソースからのビルド

Visual Studio の C++ ビルドツールと [After Effects SDK](https://developer.adobe.com/after-effects/)（同梱していません）が必要です。SDK をこのフォルダの `SDK\` に展開して `build.bat` を実行すると、`build\Sq_Repextend.aex` ができます。

## ライセンス

(C) 2026 Sqkurq. All rights reserved.
