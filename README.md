# OBS NTSC Snow

NTSC 方式のテレビ受像機の信号処理系をモデル化して、OBS の任意のソースに
「ブラウン管越しの映像」と「無信号の砂嵐」を実時間で与えるビデオフィルタです。

WebGL2 アプリ **[NTSC Snow Simulator](https://github.com/abanum/ZAA)**（MIT）の
処理チェーンを、OBS のネイティブ C プラグイン（多段レンダリング）へ移植したものです。

- 乱数テクスチャを貼るのではなく、**受信機の信号処理系**を再現します。
  帯域制限した複素ガウス IF 雑音を包絡線検波するため、砂嵐の輝度は一様乱数ではなく
  **レイリー分布**になります（NTSC は負変調なので大振幅ほど暗い）。
- **電界強度**スライダー 1 本で「クリアな映像 → 弱電界のざらつき → 完全な砂嵐」まで
  連続的に遷移します（砂嵐は電界強度ゼロの極限）。
- **Y/C 分離方式**（バンドパス／2 ラインコム／3 ラインコム）を切り替えて、
  クロスカラー（レインボー）やドットクロールの増減を確認できます。
- **CRT 表示**：インターレース走査、ビームスポット径、蛍光体残光、走査線、画面曲率、周辺光量落ち。

## 処理チェーン

```
ソース(RGB)
  └▶ Encode : RGB→YIQ→帯域制限(Y4.2 / I1.3 / Q0.4 MHz)→3.58MHz 変調 = コンポジット
      └▶ Detect : ライス検波（搬送波 + 帯域制限した複素ガウス雑音 → 包絡線）
          └▶ Decode : Y/C 分離 + 同期検波 → YIQ→RGB（1 フィールド）
              └▶ Display : 2 フィールドをインターレース織り合わせ + CRT 表示
```

4fsc（14.318 MHz = 3.579545×4）サンプリングで、副搬送波位相が 1 サンプルあたり π/2
進む性質を利用しています。副搬送波位相を絶対サンプル位置と 4 フィールドシーケンスから
積算するため、ドットクロールとクロスカラーがアーティファクトとして自然に発生します。

## 導入方法

### ビルド（付属の CMake プリセット）

このリポジトリは公式の
[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) をベースにしており、
CMake プリセットが OBS の開発用依存物（libobs 等）を自動でダウンロードします。

**Windows（Visual Studio 2022 + CMake 3.28 以降）**

```bash
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

**macOS**

```bash
cmake --preset macos
cmake --build --preset macos --config RelWithDebInfo
```

**Linux（Ubuntu, Ninja）**

```bash
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

ビルド生成物（`ntsc-snow.dll` / `.so` / `.plugin` と `data/` フォルダ）を OBS の
プラグインディレクトリへ配置します。

- Windows（ポータブル配置）: `obs-studio/obs-plugins/64bit/ntsc-snow.dll` と
  `obs-studio/data/obs-plugins/ntsc-snow/`
- Linux: `~/.config/obs-studio/plugins/ntsc-snow/bin/64bit/` と `.../data/`

> CI（GitHub Actions）用のワークフローもテンプレート由来で同梱しています。
> リポジトリを GitHub へ push すると Windows / macOS / Linux 向けの配布物を自動生成できます。

## 使い方

1. OBS で任意のソースを右クリック →「フィルタ」。
2. エフェクトフィルタに「**NTSC 砂嵐 (NTSC Snow)**」を追加。
3. パラメータを調整します。

### 試すと分かりやすい設定

- **電界強度**をゆっくり下げると、カラー映像 → ざらつき → 白黒の砂嵐へ連続的に遷移します。
- **Y/C 分離**を「バンドパス」にすると色の縦エッジにレインボーが出て、
  「2 ラインコム」で消えます（安物 TV と高級機の差）。
- **カラーキラー** ON で砂嵐がモノクロになります。

## パラメータ

| グループ | パラメータ | 内容 |
|---|---|---|
| 信号 | 電界強度 / 最小雑音 | 砂嵐↔映像の遷移、残る粒の量 |
| エンコーダ | I/Q 帯域, 変調クロマゲイン, アスペクト処理 | コンポジット化の帯域・色にじみ |
| RF/検波 | 検波レベル(AGC), AGCハンチング, IF帯域幅 | 砂嵐の粒立ち・帯域 |
| 映像デコーダ | Y/C分離, 映像帯域, クロマゲイン, カラーキラー, 位相ドリフト, コントラスト, ブライトネス | 復調と絵作り |
| CRT | インターレース, 蛍光体残光, ビーム径(V/H), 走査線, 画面曲率, 周辺光量落ち, オーバースキャン | 表示管の再現 |

## 実装上の注意 / 制限

- 内部処理は NTSC-M の固定ジオメトリ（1 ライン 754 有効サンプル × 243 有効走査線/フィールド、
  4fsc）で行い、Display 段で出力解像度へ拡大します。走査線の見えは
  「1 走査線あたり 2〜3 画素以上」の解像度でないと標本化定理上表現できないため、自動でフェードします。
- 元アプリの音声（無信号 FM 雑音）と、電源投入ウォームアップ／消灯つぶれ演出は
  フィルタとしては省略しています。
- 主対象は Windows（Direct3D 11）です。ノイズ生成はビット演算を避けた浮動小数ハッシュに
  しているため OpenGL バックエンドでも動作する想定ですが、検証は D3D11 中心です。

## ライセンスと謝辞

- 本プラグインは libobs とリンクするため **GPL-2.0-or-later**（`LICENSE` 参照）。
- アルゴリズムと処理チェーンは
  [abanum/ZAA — NTSC Snow Simulator](https://github.com/abanum/ZAA)（MIT）に基づきます。
- 足場は [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) を使用しています。

---

# OBS NTSC Snow (English)

An OBS video filter that models the NTSC receiver signal chain to give any source a
real-time "through-a-CRT" look and no-signal snow. It is a native C port (multi-pass
rendering) of the WebGL2 [NTSC Snow Simulator](https://github.com/abanum/ZAA) (MIT).

The receiver chain — **Encode** (RGB→YIQ→band-limit→3.58 MHz modulation), **Detect**
(Rician envelope detection of the carrier plus band-limited complex Gaussian IF noise),
**Decode** (Y/C separation + synchronous demodulation), **Display** (interlaced CRT) — is
reproduced rather than faked, so snow luminance is Rayleigh-distributed and a single
*field strength* slider moves continuously from a clean picture to full static.

### Build

Based on the official obs-plugintemplate; the CMake presets fetch libobs automatically.

```bash
cmake --preset windows-x64      # or: macos / ubuntu-x86_64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

Copy the resulting module and its `data/` folder into your OBS plugins directory, then add
the **NTSC Snow** effect filter to any source.

### License

GPL-2.0-or-later (links libobs). Algorithms from [abanum/ZAA](https://github.com/abanum/ZAA)
(MIT); scaffolding from obs-plugintemplate.
