# Gameplay dataset format

The Geode mod can now export gameplay-relevant objects from the currently open editor level.

The export intentionally starts with **geometry and gameplay objects**, not decoration. Pure
`GameObjectType::Decoration` objects are ignored. Solids, hazards, portals, pads, orbs,
collectibles, collision objects, modifiers/triggers, and other non-decoration object types are
kept so future dataset passes can decide what is useful.

## Raw editor export

Press the editor button with the edit icon after the song has been analyzed.

The Geode mod writes a raw JSON file containing:

```json
{
  "schema_version": 1,
  "source": "gd-ai-editor-geode",
  "level": {
    "id": 0,
    "name": "Example",
    "song_id": 123456,
    "audio_track": 0,
    "song_offset_seconds": 0.0,
    "platformer": false
  },
  "summary": {
    "total_editor_objects": 1200,
    "exported_gameplay_objects": 214,
    "category_counts": {
      "solid": 120,
      "hazard": 42,
      "orb": 24,
      "portal": 12
    }
  },
  "objects": []
}
```

Each exported object currently includes:

- Geometry Dash object ID and `GameObjectType`
- stable editor unique ID for that editor session
- normalized category
- x / y position
- rotation and x/y scale
- Geometry Dash level time for its x position
- song-offset-adjusted audio time
- no-touch / passable / hidden / high-detail flags
- editor layers
- groups
- current object hitbox rectangle

The important timing fields are:

```text
level_time_seconds
audio_time_seconds = level_time_seconds + song_offset_seconds
```

The mod uses Geometry Dash's own `LevelEditorLayer::timeForXPos` conversion, so speed changes
inside the level are handled by the game rather than recreated with guessed constants.

## Beat-aligned export

After writing the raw export, the bundled backend aligns every object against the cached song
analysis.

Each object then gets fields such as:

```json
{
  "audio_time_seconds": 42.125,
  "beat": 103.42,
  "nearest_beat_index": 103,
  "nearest_beat_time": 41.96,
  "nearest_beat_error_seconds": 0.165,
  "nearest_onset_time": 42.13,
  "nearest_onset_error_seconds": -0.005,
  "nearest_onset_strength": 0.91
}
```

`beat` is continuous, not rounded. A value such as `103.5` means the object occurs halfway
between detected beats 103 and 104.

The aligned file also contains overlapping training-window metadata. The initial defaults are:

```text
window width: 8 beats
stride:       4 beats
```

Windows currently store indices into the aligned object array plus basic category counts. They
are deliberately lightweight because the exact model input format will change while the dataset
pipeline is being validated.

## Why export modifiers too?

A moving platform may be physically meaningful because of a move trigger or another gameplay
modifier. Dropping every trigger at export time would destroy that information permanently.

The current rule is therefore conservative:

```text
pure decoration -> discard
everything else -> preserve and classify
```

Later preprocessing can remove visual-only modifiers once we can classify them safely.

## Next dataset milestone

Before training the first model:

1. export several known-good levels,
2. inspect whether object categories and hitboxes look correct,
3. verify objects stay aligned to music through speed changes,
4. identify which modifier object IDs actually affect gameplay,
5. add player-state reconstruction (mode, gravity, size, speed) over the timeline,
6. then generate cube-only training sequences.


## Learned gameplay profile

Aligned exports are compressed into a reusable mode-aware prior:

```text
gd-ai gameplay profile level-a-aligned.json level-b-aligned.json --out learned-profile.json
```

The current profile learns separately for cube, ship, ball, UFO, wave, robot, spider, and swing:

- interaction density and four-beat phrase density,
- hazard / orb / pad / portal ratios,
- quarter-beat phase usage,
- common interaction-cluster gaps quantized to 1/16-beat steps,
- reconstructed mode-section lengths,
- mode-to-mode transition probabilities,
- difficulty-specific subprofiles when a difficulty label is available.

The bundled runtime profile is currently retrained from 12 aligned levels: 8 manually
labeled Hard Demons plus the 4 original reference exports (Absolute Zero 2, Sakura 2,
Seven Seas Layout, and Moonman Layout). The original Seven Seas / Moonman structural
motifs remain style anchors while the larger dataset drives Hard Demon density and mode
cadence.

This is still **dataset-informed generation**, not a replay-trained policy. Editor exports
can teach structure, timing, state changes, density, and creator habits, but they cannot
prove which exact orb press, hold, or release was mandatory. A trajectory/input solver is
still required before generated gameplay can be validated from actual player actions.

