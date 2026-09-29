from __future__ import annotations

import collections
import itertools
import math
import statistics
import typing


_TEMPLATE_BY_CATEGORIES = {
    ("hazard", "orb", "pad"): "hazard_orb_pad",
    ("hazard", "pad"): "hazard_pad",
    ("hazard", "orb"): "hazard_orb",
    ("hazard",): "hazard_only",
    ("pad",): "pad_only",
    ("orb",): "orb_only",
    (): "quiet",
}


def _normalize(counter: collections.Counter[str]) -> dict[str, float]:
    total = sum(counter.values())
    if total <= 0:
        return {}

    return {
        key: round(value / total, 6)
        for key, value in sorted(counter.items())
    }


def _cube_interaction_objects(level: dict[str, typing.Any]) -> list[dict[str, typing.Any]]:
    objects = level.get("objects")
    if not isinstance(objects, list):
        raise TypeError("Aligned gameplay export is missing an objects array")

    result: list[dict[str, typing.Any]] = []
    for item in objects:
        if not isinstance(item, dict):
            continue

        state = item.get("state_before")
        if not isinstance(state, dict) or state.get("mode") != "cube":
            continue

        beat = item.get("beat")
        if not isinstance(beat, int | float) or not math.isfinite(float(beat)):
            continue

        if item.get("category") not in {"hazard", "orb", "pad", "portal"}:
            continue

        result.append(item)

    return result


def build_motif_profile(levels: list[dict[str, typing.Any]]) -> dict[str, typing.Any]:
    source_levels: list[str] = []
    source_object_count = 0
    cube_object_count = 0

    template_counts: collections.Counter[str] = collections.Counter()
    interaction_counts: collections.Counter[str] = collections.Counter()
    beat_phase_counts: collections.Counter[str] = collections.Counter()
    rhythm_gap_counts: collections.Counter[str] = collections.Counter()
    phrase_cluster_counts: list[int] = []

    for level in levels:
        level_meta = level.get("level")
        if not isinstance(level_meta, dict):
            level_meta = {}

        source_levels.append(str(level_meta.get("name", "Unnamed level")))

        objects = level.get("objects")
        if isinstance(objects, list):
            source_object_count += len(objects)

        interactions = _cube_interaction_objects(level)
        cube_object_count += len(interactions)

        phrases: dict[int, list[dict[str, typing.Any]]] = collections.defaultdict(list)
        clusters: dict[float, list[dict[str, typing.Any]]] = collections.defaultdict(list)

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
            phrase_cluster_map: dict[int, int] = collections.Counter(
                math.floor(beat / 4.0) for beat in cluster_beats
            )
            phrase_cluster_counts.extend(phrase_cluster_map.values())

        for left, right in itertools.pairwise(cluster_beats):
            gap = right - left
            if gap <= 0.0 or gap > 1.0:
                continue

            sixteenth_steps = max(1, round(gap * 16.0))
            rhythm_gap_counts[str(sixteenth_steps)] += 1

    if not source_levels:
        raise ValueError("At least one aligned gameplay export is required")

    phrase_count = sum(template_counts.values())
    median_clusters = statistics.median(phrase_cluster_counts) if phrase_cluster_counts else 0.0

    # The raw editor exports contain stacked hitboxes and off-path hazards, so
    # the generator intentionally does not copy their raw density 1:1. This
    # compressed density becomes a useful prior for a playable first-pass layout.
    recommended_events = max(2, min(6, round(math.sqrt(max(1.0, median_clusters)))))

    return {
        "schema_version": 1,
        "profile_type": "cube_phrase_prior",
        "source_levels": source_levels,
        "source_level_count": len(source_levels),
        "source_object_count": source_object_count,
        "cube_interaction_object_count": cube_object_count,
        "cube_phrase_count": phrase_count,
        "template_weights": _normalize(template_counts),
        "interaction_weights": _normalize(interaction_counts),
        "quarter_beat_phase_weights": _normalize(beat_phase_counts),
        "rhythm_gap_sixteenth_weights": _normalize(rhythm_gap_counts),
        "median_interaction_clusters_per_phrase": round(float(median_clusters), 3),
        "recommended_max_events_per_phrase": recommended_events,
        "notes": [
            "Profile is learned from aligned editor exports, not replay inputs.",
            "Raw editor density is compressed before generation because exported levels include stacked/off-path collision objects.",
            "Replay capture will be needed before the model can learn exact player inputs and required interaction paths.",
        ],
    }
