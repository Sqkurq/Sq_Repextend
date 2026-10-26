# Sq_Repextend

After Effects 用エフェクトプラグインです。Nuke の EdgeExtend のように、レイヤーのアルファ領域を広げて、広げた部分を最寄りのエッジピクセルと同じ色で埋めます。広げる量は CC Repetile と同じ感覚の Expand Right / Left / Down / Up で指定します。

[English](#english)

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

---

## English

An effect plug-in for After Effects. Like Nuke's EdgeExtend, it grows the alpha area of a layer and fills the new pixels with the color of the nearest edge pixel. The amount is set with Expand Right / Left / Down / Up, the same way as CC Repetile.

### Requirements

- Windows 10 / 11
- After Effects 2025 or later

### Installation

1. Download `Sq_Repextend.aex` from [Releases](https://github.com/Sqkurq/Sq_Repextend/releases)
2. Quit After Effects and copy the file into this folder (administrator rights are required)

   ```
   C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\
   ```

3. Start After Effects and apply it from **Effect > Sq > Sq_Repextend**

### Parameters

| Name | Description |
|---|---|
| Expand Right / Left / Down / Up | Distance to extend in each direction (px) |
| Expand All | Distance added to all four directions |
| Shape | Shape of the extension (Square / Circle / Diamond) |
| Alpha Threshold | Pixels at or above this alpha are used as color sources |
| Output | Original Over Extension: composite the original over the extension<br>Replace Below Threshold: replace pixels below the threshold<br>Extension Only: output only the extended area |
| Opaque Fill | Make the extended area fully opaque |
| Expand Layer Bounds | Extend beyond the layer bounds |

### Building from source

Requires the Visual Studio C++ build tools and the [After Effects SDK](https://developer.adobe.com/after-effects/) (not included). Extract the SDK into `SDK\` in this folder and run `build.bat` to get `build\Sq_Repextend.aex`.

### License

(C) 2026 Sqkurq. All rights reserved.
