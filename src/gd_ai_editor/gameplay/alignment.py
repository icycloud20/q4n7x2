from __future__ import annotations

import bisect
import copy
import json
import math
from pathlib import Path
from typing import Any


def load_json(path: str | Path) -> dict[str, Any]:
    source = Path(path)
    with source.open("r", encoding="utf-8") as handle:
        value = json.load(handle)

    if not isinstance(value, dict):
        raise ValueError(f"Expected a JSON object in {source}")

    return value


def write_json(path: str | Path, payload: dict[str, Any]) -> Path:
    output = Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return output


def _beat_times(analysis: dict[str, Any]) -> list[float]:
    beats = analysis.get("beats")
    if not isinstance(beats, list):
        raise ValueError("Audio analysis is missing a beats array")

    result: list[float] = []
    for beat in beats:
        if not isinstance(beat, dict):
            continue

        time_value = beat.get("time")
        if isinstance(time_value, int | float) and math.isfinite(float(time_value)):
            result.append(float(time_value))

    result.sort()

    if len(result) < 2:
        raise ValueError("Audio analysis needs at least two detected beats")

    return result


def _onsets(analysis: dict[str, Any]) -> tuple[list[float], list[float]]:
    raw_onsets = analysis.get("onsets")
    if not isinstance(raw_onsets, list):
        return [], []

    pairs: list[tuple[float, float]] = []
    for onset in raw_onsets:
        if not isinstance(onset, dict):
            continue

        time_value = onset.get("time")
        strength_value = onset.get("strength", 0.0)

        if not isinstance(time_value, int | float):
            continue

        time_float = float(time_value)
        if not math.isfinite(time_float):
            continue

        strength_float = (
            float(strength_value)
            if isinstance(strength_value, int | float) and math.isfinite(float(strength_value))
            else 0.0
        )
        pairs.append((time_float, strength_float))

    pairs.sort(key=lambda pair: pair[0])
    return [pair[0] for pair in pairs], [pair[1] for pair in pairs]


def _beat_position(time_seconds: float, beat_times: list[float]) -> float:
    """Return a continuous zero-based beat coordinate for an audio timestamp."""
    if time_seconds <= beat_times[0]:
        interval = beat_times[1] - beat_times[0]
        if interval <= 0:
            return 0.0
        return (time_seconds - beat_times[0]) / interval

    if time_seconds >= beat_times[-1]:
        interval = beat_times[-1] - beat_times[-2]
        if interval <= 0:
            return float(len(beat_times) - 1)
        return (len(beat_times) - 1) + (time_seconds - beat_times[-1]) / interval

    right_index = bisect.bisect_right(beat_times, time_seconds)
    left_index = right_index - 1

    left_time = beat_times[left_index]
    right_time = beat_times[right_index]
    interval = right_time - left_time

    if interval <= 0:
        return float(left_index)

    fraction = (time_seconds - left_time) / interval
    return left_index + fraction


def _nearest_index(values: list[float], target: float) -> int | None:
    if not values:
        return None

    right = bisect.bisect_left(values, target)
    candidates: list[int] = []

    if right < len(values):
        candidates.append(right)
    if right > 0:
        candidates.append(right - 1)

    return min(candidates, key=lambda index: abs(values[index] - target))


def _build_windows(
    objects: list[dict[str, Any]],
    *,
    window_beats: float = 8.0,
    stride_beats: float = 4.0,
) -> list[dict[str, Any]]:
    beat_values = [
        float(item["beat"])
        for item in objects
        if isinstance(item.get("beat"), int | float) and math.isfinite(float(item["beat"]))
    ]

    if not beat_values:
        return []

    first = math.floor(min(beat_values) / stride_beats) * stride_beats
    last = max(beat_values)

    windows: list[dict[str, Any]] = []
    start = first
    window_index = 0

    while start <= last:
        end = start + window_beats
        indices = [
            index
            for index, item in enumerate(objects)
            if isinstance(item.get("beat"), int | float)
            and start <= float(item["beat"]) < end
        ]

        if indices:
            categories: dict[str, int] = {}
            for index in indices:
                category = str(objects[index].get("category", "other"))
                categories[category] = categories.get(category, 0) + 1

            windows.append(
                {
                    "index": window_index,
                    "start_beat": round(start, 6),
                    "end_beat": round(end, 6),
                    "object_indices": indices,
                    "object_count": len(indices),
                    "category_counts": categories,
                }
            )
            window_index += 1

        start += stride_beats

    return windows


def align_gameplay_export(
    gameplay: dict[str, Any],
    analysis: dict[str, Any],
    *,
    window_beats: float = 8.0,
    stride_beats: float = 4.0,
) -> dict[str, Any]:
    beat_times = _beat_times(analysis)
    onset_times, onset_strengths = _onsets(analysis)

    raw_objects = gameplay.get("objects")
    if not isinstance(raw_objects, list):
        raise ValueError("Gameplay export is missing an objects array")

    result = copy.deepcopy(gameplay)
    aligned_objects: list[dict[str, Any]] = []

    for raw_object in raw_objects:
        if not isinstance(raw_object, dict):
            continue

        item = copy.deepcopy(raw_object)
        audio_time = item.get("audio_time_seconds")

        if not isinstance(audio_time, int | float) or not math.isfinite(float(audio_time)):
            aligned_objects.append(item)
            continue

        audio_time_float = float(audio_time)
        beat = _beat_position(audio_time_float, beat_times)
        nearest_beat_index = _nearest_index(beat_times, audio_time_float)

        item["beat"] = round(beat, 6)

        if nearest_beat_index is not None:
            nearest_beat_time = beat_times[nearest_beat_index]
            item["nearest_beat_index"] = nearest_beat_index
            item["nearest_beat_time"] = round(nearest_beat_time, 6)
            item["nearest_beat_error_seconds"] = round(audio_time_float - nearest_beat_time, 6)

        nearest_onset_index = _nearest_index(onset_times, audio_time_float)
        if nearest_onset_index is not None:
            nearest_onset_time = onset_times[nearest_onset_index]
            item["nearest_onset_time"] = round(nearest_onset_time, 6)
            item["nearest_onset_error_seconds"] = round(audio_time_float - nearest_onset_time, 6)
            item["nearest_onset_strength"] = round(onset_strengths[nearest_onset_index], 6)

        aligned_objects.append(item)

    aligned_objects.sort(
        key=lambda item: (
            float(item.get("beat", math.inf)),
            float(item.get("x", math.inf)),
            int(item.get("unique_id", 0)),
        )
    )

    result["objects"] = aligned_objects
    result["alignment"] = {
        "schema_version": 1,
        "beat_count": len(beat_times),
        "first_detected_beat_seconds": round(beat_times[0], 6),
        "last_detected_beat_seconds": round(beat_times[-1], 6),
        "window_beats": window_beats,
        "stride_beats": stride_beats,
    }
    result["windows"] = _build_windows(
        aligned_objects,
        window_beats=window_beats,
        stride_beats=stride_beats,
    )

    return result
