# gd-ai-editor

Experimental tooling for learning and generating **music-synced Geometry Dash gameplay**.

The project is intentionally split into deterministic systems and learned systems:

```text
song audio + existing gameplay context
            ↓
      feature extraction
            ↓
   learned gameplay generator
            ↓
     candidate gameplay plans
            ↓
 physics / rules validation
            ↓
   candidate scoring + ranking
            ↓
    Geometry Dash editor bridge
```

The first milestone is deliberately smaller: prove that we can turn a real song into stable, useful timing features that can later be paired with Geometry Dash gameplay for training.

## Milestone 0 — audio analysis

The current prototype analyzes an audio file and writes a compact JSON document containing:

- estimated BPM
- beat timestamps
- onset/transient timestamps and strengths
- RMS energy over time
- automatically estimated musical sections
- per-beat feature summaries for future training examples

This is the first half of the future dataset pipeline:

```text
song + GD level
   ↓
audio features + gameplay events
   ↓
aligned training windows
```

## Geode mod

There is now an actual Windows Geode mod in `geode/`.

When you open a level in the Geometry Dash editor it adds a music-note button to the editor controls. Pressing it:

1. reads the current level's song ID / audio track,
2. resolves the already-downloaded audio file from Geometry Dash automatically,
3. checks the mod's per-song analysis cache,
4. runs the bundled standalone analyzer if the song has not been analyzed yet,
5. stores JSON analysis plus a beat-preview WAV for later training and debugging.

You do **not** manually browse for the song. For a custom song, it only needs to have been downloaded normally inside Geometry Dash.

### Get a ready-made `.geode` build

The repository builds a self-contained Windows package through GitHub Actions.

Go to **Actions → GD AI Editor - Windows → latest successful run → Artifacts → `gd-ai-editor-windows`**.

The artifact contains the packaged `.geode` mod, including the standalone Python audio backend. Put the `.geode` file in your Geode mods folder or use Geode's manual-install flow.

### Build locally

Install the Geode CLI/SDK once, then from the repository root run:

```powershell
.\scripts\build-geode.ps1
```

That builds the standalone backend first, bundles it into the mod resources, then runs the Geode build.

If you are only changing C++ and already have `geode/resources/gd-ai-backend.exe`, you can skip rebuilding the backend:

```powershell
.\scripts\build-geode.ps1 -SkipBackend
```

## Python-only setup

Use Python 3.11 or newer.

```bash
git clone https://github.com/icycloud20/gd-ai-editor.git
cd gd-ai-editor

python -m venv .venv
.venv\Scripts\activate

python -m pip install -U pip
pip install -e ".[dev]"
```

On macOS/Linux, activate the environment with:

```bash
source .venv/bin/activate
```

## Analyze a song

```bash
gd-ai audio analyze "path/to/song.mp3" --out artifacts/song-analysis.json
```

To also print a readable summary:

```bash
gd-ai audio analyze "path/to/song.mp3" --out artifacts/song-analysis.json --summary
```

For the most useful first test, also create a listenable beat preview:

```bash
gd-ai audio analyze "path/to/song.mp3" \
  --out artifacts/song-analysis.json \
  --beat-preview artifacts/beat-preview.wav \
  --summary
```

The preview is the original song with short clicks placed on every detected beat. Listen through the whole track and check whether the clicks stay locked to the rhythm instead of slowly drifting.

## What to test first

Use a song you know extremely well.

1. Run the audio analyzer.
2. Check whether the estimated BPM is correct or a sensible half/double-time equivalent.
3. Open the generated JSON.
4. Compare the first 20-30 beat timestamps against obvious kicks/snares in the song.
5. Check whether section boundaries roughly line up with intro / buildup / drop / break changes.
6. Keep the JSON even if some detections are wrong; failures are useful examples for improving the analyzer.

For a Geometry Dash song, the most important early test is **timing stability**. If beats drift away from the music later in the song, do not build generation on top of it yet.

## Roadmap

### M0 — music representation
- [x] audio loading
- [x] BPM / beat detection
- [x] onset detection
- [x] energy envelope
- [x] basic section estimation
- [x] JSON schema + CLI

### M1 — gameplay representation
- [ ] parse/export gameplay-relevant GD objects
- [ ] normalize portals, orbs, pads, hazards, solids
- [ ] represent gameplay in beat-relative coordinates
- [ ] derive local mode/speed/gravity state
- [ ] create aligned audio + gameplay windows

### M2 — deterministic validation
- [ ] basic reachability checks
- [ ] per-gamemode physics
- [ ] replay/input timeline representation
- [ ] candidate validity scoring

### M3 — learned generation
- [ ] tokenizer / event representation
- [ ] cube-only baseline model
- [ ] conditioning on audio features
- [ ] autoregressive candidate generation
- [ ] candidate ranker

### M4 — editor integration
- [x] initial Geode bridge
- [x] automatic current-song discovery
- [x] per-song analysis cache
- [x] bundled analyzer launch
- [ ] select timeline range
- [ ] generate multiple candidates
- [ ] preview / accept / reject
- [ ] record feedback for later training

## Design rule

The learned model should reason about **gameplay events, rhythm, trajectory, and structure**. Deterministic code should own exact Geometry Dash coordinates, object serialization, collision rules, and physics wherever possible.

That keeps the model focused on the part we actually want it to learn: what makes gameplay flow with music.
