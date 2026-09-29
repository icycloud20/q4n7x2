# GD AI Editor

Early research build for music-aware Geometry Dash gameplay generation.

## Current build

This first in-game build focuses on the foundation:

- adds a GD AI button to the level editor
- reads the song directly from the currently opened level
- resolves Geometry Dash's downloaded song file automatically
- caches analysis per song file
- runs the bundled audio-analysis backend without requiring a manual song path
- writes beat/BPM/onset/energy/section analysis for later gameplay training

Actual gameplay generation comes after the audio and level representations are proven reliable.
