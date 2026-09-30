# ruff: noqa: I001
from __future__ import annotations

import math
from collections import Counter, defaultdict
from itertools import pairwise
from statistics import median
from typing import Any


_TEMPLATE_BY_CATEGORIES = {
    ("hazard", "orb", "pad"): "hazard_orb_pad",
    ("hazard", "pad"): "hazard_pad",
    ("hazard", "orb"): "hazard_orb",
    ("hazard",): "hazard_only",
    ("pad",): "pad_only",
    ("orb",): "orb_only",
    (): "quiet",
}

_GAMEPLAY_MODES = (
    "cube",
    "ship",
    "ball",
    "ufo",
    "wave",
    "robot",
    "spider",
    "swing",
)

_DIFFICULTY_SCORES = {
    "Auto": 0.05,
    "Easy": 0.12,
    "Normal": 0.20,
    "Hard": 0.30,
    "Harder": 0.40,
    "Insane": 0.50,
    "Easy Demon": 0.60,
    "Medium Demon": 0.70,
    "Hard Demon": 0.80,
    "Insane Demon": 0.90,
    "Extreme Demon": 1.00,
}


def _normalize(counter: Counter[str]) -> dict[str, float]:
    total = sum(counter.values())
    if total <= 0:
        return {}

    return {
        key: round(value / total, 6)
        for key, value in sorted(counter.items())
    }


def _interaction_objects(
    level: dict[str, Any],
    *,
    mode: str | None = None,
) -> list[dict[str, Any]]:
    objects = level.get("objects")
    if not isinstance(objects, list):
        raise TypeError("Aligned gameplay export is missing an objects array")

    result: list[dict[str, Any]] = []
    for item in objects:
        if not isinstance(item, dict):
            continue

        state = item.get("state_before")
        if not isinstance(state, dict):
            continue

        item_mode = str(state.get("mode", ""))
        if mode is not None and item_mode != mode:
            continue

        beat = item.get("beat")
        if not isinstance(beat, int | float) or not math.isfinite(float(beat)):
            continue

        if item.get("category") not in {"hazard", "orb", "pad", "portal"}:
            continue

        result.append(item)

    return result


def _mode_profile(
    levels: list[dict[str, Any]],
    *,
    mode: str,
) -> dict[str, Any]:
    template_counts: Counter[str] = Counter()
    interaction_counts: Counter[str] = Counter()
    beat_phase_counts: Counter[str] = Counter()
    rhythm_gap_counts: Counter[str] = Counter()
    phrase_cluster_counts: list[int] = []
    interaction_object_count = 0

    # Keep level boundaries intact. Phrase index 0 from two different levels is
    # two training phrases, not one combined super-phrase.
    for level in levels:
        interactions = _interaction_objects(level, mode=mode)
        interaction_object_count += len(interactions)

        phrases: dict[int, list[dict[str, Any]]] = defaultdict(list)
        clusters: dict[float, list[dict[str, Any]]] = defaultdict(list)

        for item in interactions:
            category = str(item.get("category"))
            interaction_counts[category] += 1

            beat = float(item["beat"])
            phrase_index = math.floor(beat / 4.0)
            phrases[phrase_index].append(item)

            phase = beat % 1.0
            quarter = round(phase * 4.0) / 4.0
            if quarter >= 1.0:
                quarter = 0.0
            beat_phase_counts[f"{quarter:.2f}"] += 1

            cluster_beat = round(beat * 16.0) / 16.0
            clusters[cluster_beat].append(item)

        for phrase_objects in phrases.values():
            categories = tuple(
                sorted(
                    {
                        str(item.get("category"))
                        for item in phrase_objects
                        if item.get("category") != "portal"
                    }
                )
            )
            template = _TEMPLATE_BY_CATEGORIES.get(categories, "mixed")
            template_counts[template] += 1

        cluster_beats = sorted(clusters)
        if cluster_beats:
            phrase_cluster_map: dict[int, int] = Counter(
                math.floor(beat / 4.0) for beat in cluster_beats
            )
            phrase_cluster_counts.extend(phrase_cluster_map.values())

        for left, right in pairwise(cluster_beats):
            gap = right - left
            if gap <= 0.0 or gap > 1.0:
                continue

            sixteenth_steps = max(1, round(gap * 16.0))
            rhythm_gap_counts[str(sixteenth_steps)] += 1

    phrase_count = sum(template_counts.values())
    median_clusters = median(phrase_cluster_counts) if phrase_cluster_counts else 0.0

    recommended_events = max(
        2,
        min(12, round(max(2.0, float(median_clusters)))),
    )

    return {
        "interaction_object_count": interaction_object_count,
        "phrase_count": phrase_count,
        "template_weights": _normalize(template_counts),
        "interaction_weights": _normalize(interaction_counts),
        "quarter_beat_phase_weights": _normalize(beat_phase_counts),
        "rhythm_gap_sixteenth_weights": _normalize(rhythm_gap_counts),
        "median_interaction_clusters_per_phrase": round(float(median_clusters), 3),
        "recommended_max_events_per_phrase": recommended_events,
    }

def _difficulty_label(level: dict[str, Any]) -> str:
    level_meta = level.get("level")
    if not isinstance(level_meta, dict):
        return "Unknown"

    explicit = level_meta.get("difficulty_label")
    if isinstance(explicit, str) and explicit:
        return explicit

    stars = level_meta.get("stars")
    demon = level_meta.get("is_demon")
    demon_difficulty = level_meta.get("demon_difficulty")

    if demon or stars == 10:
        return {
            3: "Easy Demon",
            4: "Medium Demon",
            0: "Hard Demon",
            5: "Insane Demon",
            6: "Extreme Demon",
        }.get(demon_difficulty, "Hard Demon")

    if not isinstance(stars, int | float):
        return "Unknown"

    stars = int(stars)
    if stars <= 0:
        return "Unknown"
    if stars <= 1:
        return "Auto"
    if stars == 2:
        return "Easy"
    if stars == 3:
        return "Normal"
    if stars <= 5:
        return "Hard"
    if stars <= 7:
        return "Harder"
    if stars <= 9:
        return "Insane"

    return "Unknown"


def _profiles_for_levels(levels: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    return {
        mode: _mode_profile(levels, mode=mode)
        for mode in _GAMEPLAY_MODES
    }


def build_motif_profile(levels: list[dict[str, Any]]) -> dict[str, Any]:
    if not levels:
        raise ValueError("At least one aligned gameplay export is required")

    source_levels: list[str] = []
    source_object_count = 0
    difficulty_counts: Counter[str] = Counter()
    difficulty_scores: list[float] = []
    levels_by_difficulty: dict[str, list[dict[str, Any]]] = defaultdict(list)

    for level in levels:
        level_meta = level.get("level")
        if not isinstance(level_meta, dict):
            level_meta = {}

        source_levels.append(str(level_meta.get("name", "Unnamed level")))

        objects = level.get("objects")
        if isinstance(objects, list):
            source_object_count += len(objects)

        label = _difficulty_label(level)
        difficulty_counts[label] += 1
        levels_by_difficulty[label].append(level)

        score = _DIFFICULTY_SCORES.get(label)
        if score is not None:
            difficulty_scores.append(score)

    mode_profiles = _profiles_for_levels(levels)
    cube = mode_profiles["cube"]

    difficulty_profiles = {
        label: {
            "source_level_count": len(group),
            "mode_profiles": _profiles_for_levels(group),
        }
        for label, group in sorted(levels_by_difficulty.items())
        if label != "Unknown"
    }

    median_difficulty = (
        round(float(median(difficulty_scores)), 3)
        if difficulty_scores
        else None
    )

    return {
        "schema_version": 2,
        "profile_type": "mode_aware_phrase_prior",
        "source_levels": source_levels,
        "source_level_count": len(source_levels),
        "source_object_count": source_object_count,
        "difficulty_distribution": dict(sorted(difficulty_counts.items())),
        "median_source_difficulty_score": median_difficulty,
        "mode_profiles": mode_profiles,
        "difficulty_profiles": difficulty_profiles,

        # Backwards-compatible cube keys so old generator builds still load the
        # new profile while the C++ side migrates to mode-aware priors.
        "cube_interaction_object_count": cube["interaction_object_count"],
        "cube_phrase_count": cube["phrase_count"],
        "template_weights": cube["template_weights"],
        "interaction_weights": cube["interaction_weights"],
        "quarter_beat_phase_weights": cube["quarter_beat_phase_weights"],
        "rhythm_gap_sixteenth_weights": cube["rhythm_gap_sixteenth_weights"],
        "median_interaction_clusters_per_phrase": (
            cube["median_interaction_clusters_per_phrase"]
        ),
        "recommended_max_events_per_phrase": (
            cube["recommended_max_events_per_phrase"]
        ),
        "notes": [
            "Profile is learned from aligned editor exports, not replay inputs.",
            "All gameplay modes are profiled separately instead of discarding non-cube data.",
            "Difficulty-specific subprofiles allow generation to target Hard Demon without blending every source level together.",
            "Exact required inputs still need replay/trajectory solving; this profile learns editor structure, rhythm and density.",
        ],
    }
