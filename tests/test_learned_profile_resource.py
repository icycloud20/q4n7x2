from __future__ import annotations

import json
from pathlib import Path


def test_bundled_learned_profile_is_valid() -> None:
    path = Path("geode/resources/learned-profile-v1.json")
    profile = json.loads(path.read_text(encoding="utf-8"))

    assert profile["schema_version"] == 1
    assert profile["profile_type"] == "cube_phrase_prior"
    assert profile["source_level_count"] == 2
    assert profile["cube_phrase_count"] > 0

    weights = profile["template_weights"]
    assert abs(sum(weights.values()) - 1.0) < 1e-5
    assert weights["hazard_orb_pad"] > weights["hazard_orb"]
    assert 2 <= profile["recommended_max_events_per_phrase"] <= 6
