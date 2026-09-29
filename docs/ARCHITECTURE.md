# Architecture

## Goal

Learn **playable, music-synced Geometry Dash gameplay**, not raw decorated level strings.

The model should learn rhythm, flow, transitions, click density, trajectory, and gameplay structure. Exact object serialization and physics should remain deterministic whenever possible.

## Target pipeline

```text
                         ┌─────────────────────┐
song ──► audio analyzer ─►│ aligned feature grid│
                         └──────────┬──────────┘
                                    │
GD level ─► gameplay parser ────────┤
                                    ▼
                         ┌─────────────────────┐
                         │ training windows    │
                         │ audio + context     │
                         │ -> target gameplay  │
                         └──────────┬──────────┘
                                    │
                                    ▼
                         ┌─────────────────────┐
                         │ gameplay generator  │
                         └──────────┬──────────┘
                                    │ N candidates
                                    ▼
                         ┌─────────────────────┐
                         │ physics validator   │
                         └──────────┬──────────┘
                                    │ valid only
                                    ▼
                         ┌─────────────────────┐
                         │ learned ranker      │
                         └──────────┬──────────┘
                                    │ best candidate
                                    ▼
                         ┌─────────────────────┐
                         │ deterministic GD    │
                         │ object builder      │
                         └──────────┬──────────┘
                                    ▼
                              Geode editor
```

## Why not train directly on raw level objects?

A normal rated level contains huge amounts of decoration and trigger data unrelated to the movement the player performs. Training directly on all objects would force the model to spend most of its capacity learning visual construction rather than gameplay.

We instead want a gameplay-focused intermediate representation.

## Planned gameplay representation

A parsed level becomes a timeline of state changes and gameplay events.

Example:

```json
{
  "start_time": 31.2,
  "mode": "cube",
  "speed": 2.0,
  "gravity": 1,
  "events": [
    {
      "beat": 0.0,
      "kind": "jump"
    },
    {
      "beat": 0.5,
      "kind": "yellow_orb",
      "relative_height": 1.8
    },
    {
      "beat": 1.5,
      "kind": "landing"
    }
  ]
}
```

Raw GD objects are still retained separately so a generated abstract sequence can be converted back into a real level.

## Training windows

Training should use overlapping musical windows instead of entire levels.

Example:

```text
context: beats 0-16
target:  beats 16-24

context: beats 4-20
target:  beats 20-28

context: beats 8-24
target:  beats 24-32
```

Each example can contain:

- audio features
- prior gameplay events
- mode / speed / gravity state
- difficulty metadata
- target future gameplay events
- optional replay/input events
- optional player trajectory

This lets one level produce many examples.

## Learned systems

### Generator

First serious model: a relatively small Transformer trained autoregressively over gameplay events.

Conditioning:

- beat-relative audio features
- prior gameplay
- current player state
- requested difficulty / intensity
- optional preferred gamemodes

Output:

- future abstract gameplay events
- mode changes
- rhythmic input intentions
- trajectory targets

The first model should be cube-only. Multimode generation comes after the representation and validation pipeline work.

### Ranker

A second model scores candidate gameplay.

Positive examples:

- real human-created gameplay windows
- accepted generations

Negative examples:

- rhythm-shifted copies
- excessive repetition
- missing inputs
- awkward mode transitions
- deliberately broken geometry
- rejected generations

Generation should eventually sample multiple candidates, validate them, then rank the survivors.

## Deterministic systems

These should not be delegated to an LLM or learned model when exact code can do them better:

- audio timestamp conversion
- GD X/time conversion
- object serialization
- portal state tracking
- collision rules
- gameplay physics
- reachability
- candidate validity
- editor communication

## LLM role

An LLM is optional and sits above the specialized generator.

Useful jobs:

- convert natural-language requests into control parameters
- plan broad song sections
- choose desired modes / intensity curves
- critique several valid generated candidates

Bad jobs:

- manually output thousands of object coordinates
- simulate physics
- enforce collision correctness
- calculate exact beat positions

## Current milestone

M0 is deliberately only the audio side.

Before downloading thousands of levels or training anything, verify that timing extraction is stable on songs used by Geometry Dash. If our beat grid drifts, every future training pair will be mislabeled.

After M0 passes, M1 is the level/gameplay parser.


## Phrase Generator v2

The in-editor cube generator now keeps a persistent path height across four-beat phrases.
Raised platforms carry into following phrases, quiet phrases can descend toward the ground,
and spikes / pads / orbs are positioned relative to the current gameplay surface.

Phrase selection remains dataset-informed by the bundled cube phrase profile learned from
the current aligned training exports. The beat and micro-onset lanes remain temporary debug
visuals while playability and phrase continuity are being validated.
