# Shizuku Tune

[日本語](#日本語) | [English](#english)

Analysis-based, natural-sounding pitch correction for vocals, as an Audio Unit (AUv2) for macOS (Apple silicon and Intel).
It analyses the whole take first, then moves each note only as far as needed, keeping vibrato, scoops and the singer's phrasing.

---

## 日本語

歌のテイクを最初に通しで解析してから補正する、自然さ重視のピッチ補正プラグインです（macOS 用 Audio Unit）。

### できること

- **伴奏からキーを推定**（転調にも対応）し、歌の音程（pYIN）と音の区切りを解析します。
- **一音ずつ控えめに補正**: 音の中心の高さだけを動かし、ビブラート・しゃくり・語尾・音のつなぎは残します。声の響き（フォルマント）は保ちます。
- **正解のメロディーに合わせる**: Synthesizer V のプロジェクト（`.svp`）、MIDI（`.mid`）、ガイドの歌（`.wav`）を読み込むと、フレーズごとに一番合う正解を選んで寄せます（オクターブ違い・掛け合いの拍ずらしにも対応）。届ききらずに中途半端に移っている所は、正解の音符ごとに区切って目標の音程の線へ寄せます。
- **強さ・しきい値**: 補正の強さ（0〜100%）と、補正しない小さなずれ（セント）を調整できます。
- **手直し（コマンドライン）**: 音ごとの目標（1 セント単位）・補正しない・分割・結合を JSON で指定できます（`src/correction/NoteEdits.h`）。

### Logic Pro での使い方（取り込み方式）

Apple シリコンの Logic Pro はプラグインを別プロセスで動かすため ARA が使えません。そのため Melodyne の「転送」と同じく、**1 回再生して取り込む**方式で使います（Rosetta は不要です）。

1. 歌のトラックに **Shizuku Tune**（ARA ではない方）を差します。
2. プラグイン画面上部の **Side Chain** で伴奏のトラックを選びます（なくても動きますが、キーの推定には伴奏があると確実です）。
3. **取り込み** を押して、曲を再生します。停止すると自動で解析と補正が始まります。
4. 「準備完了」と出たら、再生すると補正した歌が鳴ります。バウンスでも同じ音になります。
5. 必要なら **正解を追加...** でガイドの `.svp` / `.mid` を読み込みます。
6. 歌のリージョンを動かしたり録り直したりしたら **取り込み直し** を押してください。

取り込んだ音と結果は `~/Library/Application Support/ShizukuTune/captures/` に保存され、プロジェクトを開き直しても取り込み直しは不要です。

Studio One や REAPER など ARA に対応したホストでは、ARA 版（Shizuku Tune (ARA)）として差すと、再生せずにトラック全体を解析できます。

### ビルド

```sh
git clone --recursive https://github.com/kojira/shizuku-tune.git
cd shizuku-tune
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ShizukuTune_AU
cp -R "build/ShizukuTune_artefacts/Release/AU/Shizuku Tune.component" ~/Library/Audio/Plug-Ins/Components/
auval -v aufx Shzt Kjra
```

必要なもの: macOS、Xcode、CMake 3.22 以上、Ninja。arm64 + x86_64 の両方入りでビルドされます。

---

## English

### Features

- **Key from the accompaniment** (chroma + Krumhansl–Kessler profiles, modulation-aware sections), **pitch** with pYIN, and note segmentation.
- **Gentle per-note correction**: only a note's centre is moved; vibrato, scoops, falls and connections stay. Formants are preserved (Signalsmith Stretch).
- **Reference melodies**: Synthesizer V `.svp`, MIDI `.mid` or a guide `.wav`. Per sung phrase the best-matching candidate is chosen (any octave, also shifted by whole beats for call-and-response). Where a reference exists, the sung line is aligned to its notes and corrected along a target pitch curve, so notes the singer never quite reaches are pulled onto the target while vibrato stays.
- **Strength / threshold** controls.
- **Hand edits** (command line): per-note target (1-cent precision), keep, split, merge, as JSON (`src/correction/NoteEdits.h`).

### Using it in Logic Pro (capture mode)

Logic Pro on Apple silicon hosts Audio Units out of process, which does not carry ARA. Shizuku Tune therefore offers a **play-once capture** mode, like Melodyne's "Transfer" (no Rosetta needed):

1. Insert **Shizuku Tune** (not the ARA variant) on the vocal track.
2. Choose the accompaniment track in the plug-in window's **Side Chain** menu (optional, but it makes the key detection reliable).
3. Press **取り込み** (Capture) and play the song. When the transport stops, analysis and correction run automatically.
4. When it shows 「準備完了」 (ready), playback and bounces use the corrected vocal.
5. Optionally add reference melodies with **正解を追加...** (Add reference).
6. After moving or re-recording the vocal, press **取り込み直し** (Capture again).

Captures are stored in `~/Library/Application Support/ShizukuTune/captures/`; the project keeps only an id and the settings.

In ARA hosts (Studio One, REAPER, …) insert **Shizuku Tune (ARA)** to analyse whole tracks without playing them.

### Building

```sh
git clone --recursive https://github.com/kojira/shizuku-tune.git
cd shizuku-tune
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ShizukuTune_AU
cp -R "build/ShizukuTune_artefacts/Release/AU/Shizuku Tune.component" ~/Library/Audio/Plug-Ins/Components/
auval -v aufx Shzt Kjra
```

Requires macOS, Xcode, CMake ≥ 3.22 and Ninja. The component is built for arm64 and x86_64.

### Command-line tools and tests

```sh
cmake -S . -B build-analysis -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHIZUKU_BUILD_PLUGIN=OFF
cmake --build build-analysis && (cd build-analysis && ctest --output-on-failure)

./build-analysis/shizuku-synth /tmp/out 9 minor voice            # synthetic test signals (known key / pitch)
./build-analysis/shizuku-analyze vocal.wav accompaniment.wav      # key and notes as JSON (either may be "-")
./build-analysis/shizuku-correct vocal.wav accompaniment.wav out.wav [--strength 100] [--threshold 25] \
    [--reference part.svp|part.mid|guide.wav[@offset]]... [--choose auto|N] [--bpm N] [--edits edits.json] [--report]
./build-analysis/shizuku-reference-error sung.wav part.svp[@offset] [--after corrected.wav] [--notes]
```

- `shizuku-edit-session`: keeps one take in memory and re-renders only around hand-edited notes (one JSON command per line on stdin), for interactive front ends.
- `shizuku-render-view`: draws the plug-in's pitch view to a PNG.
- `shizuku-au-capture-check` (plug-in build): loads the built component through AudioToolbox, feeds a vocal and a side-chain accompaniment, captures, and compares the corrected output with a file (e.g. `shizuku-correct`'s output).
- Tests: key detection over all 24 keys, correction accuracy and vibrato preservation, reference selection / alignment, hand edits, and an offline host simulation of the capture mode (`capture-session-test`).

`out.wav` is 32-bit float: pitch shifting changes the phase relation between harmonics, so the peak can rise by 1–2 dB while the level falls slightly; nothing is limited.

### Layout

- `src/analysis/` — JUCE-free analysis: key detection, pYIN pitch tracking, note segmentation, tempo, reference melodies (`.svp` / `.mid` / guide `.wav`) and their per-phrase selection and alignment.
- `src/correction/` — correction planning (per-note shifts, target pitch curves, connections) and rendering with Signalsmith Stretch; hand edits.
- `src/plugin/` — the Audio Unit: ARA document controller and playback renderer, capture mode (`CaptureSession`), editor and pitch view.
- `tools/`, `tests/` — command-line tools and tests.
- `external/` — JUCE (submodule, `8.0.17-security`), ARA SDK (`releases/2.3.0`, ARA_API and ARA_Library), Signalsmith Stretch (`1.4.0`) and Linear (`0.6.4`) (submodules).

### Licence

Shizuku Tune is licensed under the **GNU Affero General Public License v3.0** (see `LICENSE`), as required by its use of JUCE under the AGPLv3.
Third-party components keep their own licences: JUCE (AGPLv3 / commercial), ARA SDK (Apache 2.0, `external/ARA_SDK/*/LICENSE.txt`), Signalsmith Stretch and Linear (MIT).
ARA is a trademark of Celemony Software GmbH. Logic Pro and Audio Units are trademarks of Apple Inc.
