from __future__ import annotations

from gd_ai_editor.gameplay import build_motif_profile


def _object(
    beat: float,
    category: str,
    *,
    mode: str = "cube",
) -> dict:
    return {
        "beat": beat,
        "category": category,
        "state_before": {"mode": mode},
    }


def test_build_motif_profile_learns_mode_and_difficulty_profiles() -> None:
    level_a = {
        "level": {
            "name": "A",
            "difficulty_label": "Hard Demon",
            "stars": 10,
            "is_demon": True,
            "demon_difficulty": 0,
        },
        "objects": [
            _object(0.0, "hazard"),
            _object(0.25, "orb"),
            _object(0.50, "pad"),
            _object(1.0, "portal"),
            _object(4.0, "hazard"),
            _object(4.25, "hazard"),
            _object(4.50, "hazard"),
            _object(4.75, "hazard"),
            _object(8.0, "hazard", mode="ship"),
            _object(8.25, "hazard", mode="ship"),
            _object(8.50, "portal", mode="ship"),
        ],
    }
    level_b = {
        "level": {
            "name": "B",
            "difficulty_label": "Insane",
            "stars": 9,
            "is_demon": False,
        },
        "objects": [
            _object(0.0, "hazard"),
            _object(0.125, "pad"),
            _object(0.25, "portal"),
            _object(4.0, "hazard"),
            _object(4.125, "orb"),
            _object(8.0, "hazard", mode="wave"),
            _object(8.125, "hazard", mode="wave"),
        ],
    }

    profile = build_motif_profile([level_a, level_b])

    assert profile["schema_version"] == 2
    assert profile["profile_type"] == "mode_aware_phrase_prior"
    assert profile["source_levels"] == ["A", "B"]
    assert profile["source_level_count"] == 2
    assert profile["source_object_count"] == 18
    assert profile["cube_phrase_count"] == 4

    weights = profile["template_weights"]
    assert weights["hazard_only"] == 0.25
    assert weights["hazard_orb"] == 0.25
    assert weights["hazard_orb_pad"] == 0.25
    assert weights["hazard_pad"] == 0.25

    assert profile["mode_profiles"]["ship"]["interaction_object_count"] == 3
    assert profile["mode_profiles"]["wave"]["interaction_object_count"] == 2
    assert profile["mode_profiles"]["ufo"]["interaction_object_count"] == 0

    assert profile["difficulty_distribution"] == {
        "Hard Demon": 1,
        "Insane": 1,
    }
    hard_demon = profile["difficulty_profiles"]["Hard Demon"]
    assert hard_demon["source_level_count"] == 1
    assert hard_demon["mode_profiles"]["ship"]["interaction_object_count"] == 3

    assert profile["interaction_weights"]["hazard"] > profile["interaction_weights"]["orb"]
    assert profile["rhythm_gap_sixteenth_weights"]
    assert 2 <= profile["recommended_max_events_per_phrase"] <= 12
