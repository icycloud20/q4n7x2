from __future__ import annotations

import json
from pathlib import Path


def test_bundled_learned_profile_is_valid() -> None:
    path = Path("geode/resources/learned-profile-v1.json")
    profile = json.loads(path.read_text(encoding="utf-8"))

    assert profile["schema_version"] == 2
    assert profile["profile_type"] == "mode_aware_phrase_prior"
    assert profile["source_level_count"] == 12
    assert profile["difficulty_distribution"]["Hard Demon"] == 8
    assert profile["cube_phrase_count"] >= 300

    weights = profile["template_weights"]
    assert abs(sum(weights.values()) - 1.0) < 1e-5
    assert 2 <= profile["recommended_max_events_per_phrase"] <= 12

    for mode in ("cube", "ship", "ball", "ufo", "wave"):
        mode_profile = profile["mode_profiles"][mode]
        assert mode_profile["phrase_count"] > 0
        assert 2 <= mode_profile["recommended_max_events_per_phrase"] <= 12

    hard_demon = profile["difficulty_profiles"]["Hard Demon"]
    assert hard_demon["source_level_count"] == 8
    sequence = hard_demon["mode_sequence_profile"]
    assert sequence["section_count"] > 300
    assert sequence["recommended_section_chunks"] == 1
    assert sequence["transition_weights"]["cube"]["ball"] > 0


def test_bundled_structural_motifs_are_valid() -> None:
    path = Path("geode/resources/learned-motifs-v2.json")
    profile = json.loads(path.read_text(encoding="utf-8"))

    assert profile["v"] == 2
    assert profile["n"] == 4
    assert profile["c"] == len(profile["motifs"])
    assert profile["c"] >= 8

    assert any(motif["g0"] != motif["g1"] for motif in profile["motifs"])
    assert any(
        event[0] == 1
        for motif in profile["motifs"]
        for event in motif["e"]
    )
    assert any(
        event[0] == 5
        for motif in profile["motifs"]
        for event in motif["e"]
    )
