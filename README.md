# OBS Composite TV

https://github.com/user-attachments/assets/1efc4153-d88c-4719-909a-3fdd8eb9da68

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

ビルド生成物（`composite-tv.dll` / `.so` / `.plugin` と `data/` フォルダ）を OBS の
プラグインディレクトリへ配置します。

- Windows（ポータブル配置）: `obs-studio/obs-plugins/64bit/composite-tv.dll` と
  `obs-studio/data/obs-plugins/composite-tv/`
- Linux: `~/.config/obs-studio/plugins/composite-tv/bin/64bit/` と `.../data/`

> CI（GitHub Actions）用のワークフローもテンプレート由来で同梱しています。
> リポジトリを GitHub へ push すると Windows / macOS / Linux 向けの配布物を自動生成できます。

### インストーラ (Windows)

手動コピーの代わりに、**ワンクリックのインストーラ (.exe)** を作れます。

**作る側（要 [Inno Setup 6](https://jrsoftware.org/isdl.php)・無料）:**

いちばん簡単なのは **`installer\build-installer.bat` をダブルクリック**するだけ
（初回だけ先に `cmake --preset windows-x64` で構成しておく）。PowerShell から直接でも可:

```powershell
cmake --preset windows-x64          # 未構成のときだけ
powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1
```

どちらも `release\composite-tv-<version>-windows-x64.exe` を生成します（ビルド →
`cmake --install` → Inno Setup コンパイルまで自動）。

**使う側（エンドユーザー）:** その `.exe` をダブルクリック。OBS のインストール先を
自動検出して `obs-plugins\64bit\composite-tv.dll` と `data\obs-plugins\composite-tv\` を配置します
（見つからなければ手動でフォルダ選択）。インストール後 OBS を（再）起動してください。
アンインストールは「アプリと機能」または同梱のアンインストーラから。

> 署名していないため、初回は SmartScreen の警告が出ることがあります。
> 「**詳細情報 → 実行**」で進めてください。OBS 起動中は上書きできないので閉じてから実行します。

## 使い方

1. OBS で任意のソースを右クリック →「フィルタ」。
2. エフェクトフィルタに「**コンポジットTV (Composite TV)**」を追加。
3. パラメータを調整します。

### 画面全体（シーン）に適用する

OBS ではシーン自体もソースなので、**シーンにこのフィルタを追加すると、その
シーンの合成結果全体**（カメラ・オーバーレイ・テキストなどすべて）に NTSC 効果が
かかります。「最終的な出力にかける」場合はこの方法を使います。

1. ソース一覧の空白を右クリック →「フィルタ」、または対象シーンを選んでフィルタを開く。
   （シーンのフィルタは、シーンを選択した状態で「フィルタ」から開けます）
2. エフェクトフィルタに「**コンポジットTV**」を追加。

> **全シーン共通にしたいとき（マスターシーン方式）**
> 「マスター」シーンを 1 つ作り、その中に他のシーンを**シーンソースとして入れ子**で配置し、
> マスターシーンにフィルタを付けます。マスターを本番シーンとして使えば、中身を差し替えても
> 常に NTSC 効果がかかります。
>
> なお OBS には「エンコード後の最終ミックスそのもの（シーン切替やトランジションを含む
> 放送出力）」にフィルタを差す仕組みはありません。シーン単位が実質的な最上位です。

**コンポジットTV ドック**の「映像ソース」ドロップダウンには、フィルタを持つソースに加えて
**シーンも表示**されるので、シーン全体の砂嵐量や電源もドックからライブ操作できます
（一覧が古い場合は「更新」を押してください）。

### 音声（TV スピーカー音）

無信号時の「サー」というノイズや、映像時の弱いインターキャリア音は、**独立した
音声フィルタ「コンポジットTV 音声」**で再現します。映像フィルタは映像ソースに付いていて音声を
持たないため、音声は別途、音を出すソースに付けます。

1. 音を出したいソース（**マイク**／**デスクトップ音声**など）を右クリック →「フィルタ」。
2. **音声フィルタ**に「**コンポジットTV 音声**」を追加。
3. ドックで「**音声ソース**」にそのソースを選ぶと、**電界強度スライダー・電源ボタンが
   映像と音声の両方を同時に制御**します。

電界強度を下げるほど元の音声がダッキング（小さく）され、砂嵐のサー音が大きくなり、
「声 → 砂嵐」へ連続的にフェードします。音量・インターキャリア音はフィルタのプロパティで
個別に調整できます。

### グリッチ（受信不良の演出）

フィルタ下部の「**グリッチ**」グループを ON にすると、受信機モデルの各段に破綻を
差し込めます（OFF のときは完全に元の見え方に戻ります）。

| 項目 | 差し込む段 | 見え方 |
|---|---|---|
| ゴースト強度 / 遅延 | 検波前 | 遅れて届いた反射波による多重像。Y/C 分離前なので色にじみを伴う。遅延を**負**にすると先行ゴースト |
| 干渉ビート 強度 / 周波数 | エンコード | 流れる斜め縞。3.58MHz 付近では虹色のクロスカラー |
| 垂直ロール速度 / 帰線帯 | 表示 | 画面が上下に流れ、継ぎ目に黒い帯（V-Hold 不良） |
| 水平同期ジッタ | 表示 | 行ごとに横へ揺れ、左端がギザギザに |
| フラッギング | 表示 | 画面**上部だけ**が横に曲がる（VTR の定番） |
| ヘッドスイッチング | 表示 | 画面**下部**数ラインが千切れる（VHS） |
| ドロップアウト / 補償 | 表示 | テープ傷で信号が欠落。1〜3 ライン分が白飛びし、後ろが一瞬モノクロに。補償 ON で直前ラインを複製する実機の DOC 動作 |

**一時発動**: ドックの「**グリッチ発動！**」ボタン、または 設定 → ホットキー の
「**コンポジットTV: グリッチ発動**」で、数百 ms だけ一気に暴れて自然に収まります
（長さは「一時発動の長さ」で調整）。グループが OFF でもボタンだけは効きます。

**おすすめの組み合わせ**

- **古いアンテナ・弱電界**: ゴースト 0.25 / 遅延 30 / 水平ジッタ 0.15 / 電界強度 0.6
- **VHS 再生**: ヘッドスイッチング 0.5 / ドロップアウト 0.3 / フラッギング 0.4
- **チャンネル調整中**: 垂直ロール 0.3 / 帰線帯 12 / 干渉ビート 0.15

### 消磁（デガウス）

実機のブラウン管が電源投入時に「ボーン」と鳴って画面が一瞬うねる、あの動作です。
消磁コイルに流れる**減衰する交流**をモデル化しており、次の3つが同時に起きて約1秒で収まります。

- 画面全体が数回**うねる**（ビームが磁界で振られる）
- 赤と青が左右にずれる**ミスコンバージェンス**（周辺ほど強い）
- 周辺に**虹色の色ムラ**（純度エラー）が浮いて消える

**発動方法:**
- ドックの「**消磁**」ボタン（映像と音が同時に発動）
- 設定 → ホットキー の「**コンポジットTV: 消磁**」「**コンポジットTV 音声: 消磁**」
  （**同じキーを両方に割り当てる**と、キー一発で映像と音が揃って発動します）

**調整:** 「消磁の長さ」（0.3〜3.0秒）と「消磁の強さ」（0〜1）。派手にするなら 2.0秒／1.0、
控えめなら 0.5秒／0.3 あたり。消磁していない間は処理をスキップするので**負荷は増えません**。

### ホットキー

設定 → ホットキー を開くと、フィルタを付けたソースの欄に次の項目が並びます
（フィルタを追加したソースごとに個別に割り当てられます）。

| 表示名 | 内部名 | 動作 |
|---|---|---|
| コンポジットTV: 電源 ON/OFF | `composite_tv.power` | 電源トグル（起動・シャットダウンのアニメーション付き） |
| コンポジットTV: グリッチ発動 | `composite_tv.glitch` | 数百 ms のグリッチバースト |
| コンポジットTV: 消磁 | `composite_tv.degauss` | 消磁（映像側） |
| コンポジットTV: 電源 ON/OFF ※音声フィルタ側 | `composite_tv_audio.power` | 電源トグル（音声側） |
| コンポジットTV: 消磁 ※音声フィルタ側 | `composite_tv_audio.degauss` | 消磁音 |

映像と音声で同じ表示名の項目が（それぞれのソースの欄に）現れるので、**同じキーを
両方に割り当てる**と、キー一発で映像と音が揃って動きます（電源・消磁とも）。

Stream Deck などの左手デバイスは、標準の「ホットキー」アクションでここに割り当てた
キーを送れば操作できます（OBS のホットキーはウィンドウが非アクティブでも効きます）。

### obs-websocket からの操作

OBS 28 以降に内蔵の obs-websocket (5.x) から、ホットキーを名前で直接叩けます。
`contextName` には**フィルタを付けているソース名**（シーンに付けた場合はシーン名）を
指定します（`contextName` 対応は obs-websocket 5.4 / OBS 30.2 以降）。

```json
{
  "requestType": "TriggerHotkeyByName",
  "requestData": {
    "hotkeyName": "composite_tv.glitch",
    "contextName": "対象ソース名（シーン名）"
  }
}
```

`hotkeyName` は上の表の内部名（`composite_tv.power` / `composite_tv.glitch` /
`composite_tv.degauss` / `composite_tv_audio.power` / `composite_tv_audio.degauss`）。
ホットキーはフィルタを付けたソースごとに登録されるので、複数のソースで使っている
場合は `contextName` のソース名で対象を選びます。

電源をトグルではなく **ON/OFF どちらかに確定**させたい場合や、電界強度などの
パラメータを動かしたい場合は `SetSourceFilterSettings` を使います
（`sourceName` はフィルタが付いている**ソース側**の名前です）。

```json
{
  "requestType": "SetSourceFilterSettings",
  "requestData": {
    "sourceName": "対象ソース名",
    "filterName": "コンポジットTV",
    "filterSettings": { "power": false, "field_strength": 0.5 }
  }
}
```

設定キーは保存済みシーンコレクション（JSON）や [docs/parameters.md](docs/parameters.md) で
確認できます。グリッチ／消磁をこの経路で発動したいときは、`glitch_pulse` /
`degauss_pulse` に**前回と違う整数**を書き込むと 1 回発火します（ドックと同じ仕組み）。

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
| CRT | 走査線数(486/243/162), 1ライン飛ばし, インターレース, 蛍光体残光, ビーム径(V/H), 走査線, シャドウマスク(スロット/ドット/アパーチャーグリル), 画面曲率, 周辺光量落ち, オーバースキャン | 表示管の再現 |

> **全項目の詳しい解説は [docs/parameters.md](docs/parameters.md) にあります。**
> 既定値・可動範囲・動かすと何が起きるかに加え、「走査線が見えない」「色相ドリフトが効かない」
> といった**効いていないように見えるときの原因**もまとめてあります。

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

# OBS Composite TV (English)

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
the **Composite TV** effect filter to any source.

### Windows installer

Instead of copying files by hand you can build a one-click installer. With
[Inno Setup 6](https://jrsoftware.org/isdl.php) installed:

```powershell
cmake --preset windows-x64          # first time only
powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1
```

This produces `release\composite-tv-<version>-windows-x64.exe`. Double-clicking it detects the
OBS install directory and places `composite-tv.dll` and the `data` folder in the right spots; an
uninstaller is registered. The build is unsigned, so SmartScreen may warn — choose
"More info → Run anyway", and close OBS before installing.

### Applying to the whole picture

A Scene is itself a source, so **adding the filter to a Scene** applies NTSC to the entire
scene composite (camera, overlays, text — everything). That is the way to "filter the final
output". For a look shared across all scenes, nest your scenes inside one master scene and
filter the master. OBS has no way to filter the post-mix broadcast output itself; a scene is
the effective top level. The dock's target drop-down lists scenes too, so field strength and
power can be driven live for a whole scene.

### Glitches

Switch on the **Glitches** group for reception faults injected at the physically correct
stage: multipath **ghosting** (before Y/C separation, so it smears colour; a negative delay
gives a leading ghost), an **interference beat** (herringbone plus rainbow cross-colour),
**vertical roll** with a blanking bar, **horizontal sync jitter**, **flagging** (top-of-picture
bend), VHS **head-switching** tear and tape **dropout** streaks. The dock's **Glitch!** button
and the *Composite TV: trigger glitch* hotkey fire a momentary burst that settles on its own.

### Degauss

The dock's **Degauss** button (or the *Composite TV: degauss* / *Composite TV Audio: degauss* hotkeys)
runs the demagnetising coil: a decaying AC field ripples the raster, pulls red and blue apart
worse towards the edges, floats rainbow purity blotches over the picture, and sounds the
low "boing" — all settling in about a second. Length and strength are adjustable, and the
whole path is skipped when idle, so it costs nothing the rest of the time.

### Audio

The no-signal hiss and faint intercarrier tone are a **separate "Composite TV Audio" filter** (the
video filter sits on a video source that carries no audio). Add it to an audio-producing
source (mic, desktop audio), then pick that source under the dock's **Audio source**: the one
field-strength slider and power button now drive both picture and sound, ducking the source
audio into full static as field strength falls.

### Hotkeys

Settings → Hotkeys shows these entries in the section of each source the filter is
attached to (each filtered source gets its own bindings):

| Display name | Internal name | Action |
|---|---|---|
| Composite TV: toggle power | `composite_tv.power` | Power toggle (with the warm-up / shutdown animation) |
| Composite TV: trigger glitch | `composite_tv.glitch` | Momentary glitch burst (a few hundred ms) |
| Composite TV: degauss | `composite_tv.degauss` | Degauss (video side) |
| Composite TV: toggle power — audio filter | `composite_tv_audio.power` | Power toggle (audio side) |
| Composite TV: degauss — audio filter | `composite_tv_audio.degauss` | Degauss thump |

The video and audio entries share display names but appear under their own sources;
**bind the same key to both** and one keypress fires picture and sound together
(works for power and degauss alike).

A Stream Deck (or similar) can drive them with its stock "Hotkey" action sending the
key you bound here — OBS hotkeys work even while the window is unfocused.

### Controlling over obs-websocket

With the obs-websocket built into OBS 28+ (5.x), trigger the hotkeys directly by name.
`contextName` is the **name of the source the filter is attached to** (the scene name
when filtering a scene); it needs obs-websocket 5.4 / OBS 30.2 or later.

```json
{
  "requestType": "TriggerHotkeyByName",
  "requestData": {
    "hotkeyName": "composite_tv.glitch",
    "contextName": "your source (scene) name"
  }
}
```

`hotkeyName` is an internal name from the table above (`composite_tv.power` /
`composite_tv.glitch` / `composite_tv.degauss` / `composite_tv_audio.power` /
`composite_tv_audio.degauss`). Hotkeys are registered per filtered source, so when
several sources carry the filter, `contextName` picks which one fires.

To set power to a **definite state** instead of toggling — or to drive any parameter
such as field strength — use `SetSourceFilterSettings` (`sourceName` is again the
**parent source's** name):

```json
{
  "requestType": "SetSourceFilterSettings",
  "requestData": {
    "sourceName": "your source (scene) name",
    "filterName": "Composite TV",
    "filterSettings": { "power": false, "field_strength": 0.5 }
  }
}
```

Setting keys can be found in a saved scene collection (JSON) or in
[docs/parameters.md](docs/parameters.md). To fire the glitch / degauss this way,
write **an integer different from the last one** to `glitch_pulse` / `degauss_pulse`
and it triggers once (the same mechanism the dock uses).

### License

GPL-2.0-or-later (links libobs). Algorithms from [abanum/ZAA](https://github.com/abanum/ZAA)
(MIT); scaffolding from obs-plugintemplate.
