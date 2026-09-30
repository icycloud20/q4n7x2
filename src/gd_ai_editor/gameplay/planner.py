from __future__ import annotations

import json
import math
import os
import urllib.error
import urllib.request
from collections import Counter
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path
from statistics import mean
from typing import Any

SUPPORTED_MODES = ("cube", "ship", "ball", "ufo", "wave")
INTERACTION_CATEGORIES = {"hazard", "orb", "pad", "portal"}

PLAN_SCHEMA: dict[str, Any] = {
    "type": "object",
    "properties": {
        "mode": {"type": "string", "enum": list(SUPPORTED_MODES)},
        "difficulty": {"type": "string"},
        "beats": {"type": "number", "minimum": 1, "maximum": 16},
        "concept": {"type": "string", "minLength": 1},
        "actions": {
            "type": "array",
            "minItems": 2,
            "maxItems": 32,
            "items": {
                "type": "object",
                "properties": {
                    "beat": {"type": "number", "minimum": 0, "maximum": 16},
                    "action": {
                        "type": "string",
                        "enum": [
                            "jump",
                            "land",
                            "orb_yellow",
                            "orb_pink",
                            "orb_blue",
                            "orb_green",
                            "pad_yellow",
                            "pad_pink",
                            "gravity_flip",
                            "hold",
                            "release",
                            "ufo_click",
                            "wave_hold",
                            "wave_release",
                            "mode_portal",
                        ],
                    },
                    "required": {"type": "boolean"},
                    "height_delta": {"type": "integer", "minimum": -6, "maximum": 6},
                    "intensity": {"type": "number", "minimum": 0, "maximum": 1},
                    "note": {"type": "string"},
                },
                "required": [
                    "beat",
                    "action",
                    "required",
                    "height_delta",
                    "intensity",
                    "note",
                ],
                "additionalProperties": False,
            },
        },
    },
    "required": ["mode", "difficulty", "beats", "concept", "actions"],
    "additionalProperties": False,
}


@dataclass(frozen=True)
class PlannerRequest:
    mode: str
    difficulty: str
    beats: float
    energy: float
    onset: float
    entry_gravity: str = "normal"
    entry_speed: str = "normal"
    entry_mini: bool = False
    previous_mode: str = "cube"


def _finite_number(value: Any) -> float | None:
    if not isinstance(value, int | float):
        return None

    result = float(value)
    return result if math.isfinite(result) else None


def _difficulty_label(level: dict[str, Any]) -> str:
    metadata = level.get("level")
    if not isinstance(metadata, dict):
        return "Unknown"

    label = metadata.get("difficulty_label")
    if isinstance(label, str) and label:
        return label

    return "Unknown"


def _iter_mode_objects(
    level: dict[str, Any],
    *,
    mode: str,
    start_beat: float,
    end_beat: float,
) -> Iterable[dict[str, Any]]:
    objects = level.get("objects")
    if not isinstance(objects, list):
        return

    for item in objects:
        if not isinstance(item, dict):
            continue

        beat = _finite_number(item.get("beat"))
        if beat is None or beat < start_beat or beat >= end_beat:
            continue

        state = item.get("state_before")
        if not isinstance(state, dict) or state.get("mode") != mode:
            continue

        yield item


def _state_for_chunk(objects: list[dict[str, Any]], fallback_mode: str) -> dict[str, Any]:
    for item in objects:
        state = item.get("state_before")
        if isinstance(state, dict):
            return {
                "mode": str(state.get("mode", fallback_mode)),
                "gravity": str(state.get("gravity", "normal")),
                "mini": bool(state.get("mini", False)),
                "speed": str(state.get("speed", "normal")),
            }

    return {
        "mode": fallback_mode,
        "gravity": "normal",
        "mini": False,
        "speed": "normal",
    }


def extract_reference_chunks(
    levels: list[dict[str, Any]],
    *,
    chunk_beats: float = 4.0,
    stride_beats: float = 2.0,
) -> list[dict[str, Any]]:
    if chunk_beats <= 0 or stride_beats <= 0:
        raise ValueError("chunk_beats and stride_beats must be positive")

    chunks: list[dict[str, Any]] = []

    for level_index, level in enumerate(levels):
        objects = level.get("objects")
        if not isinstance(objects, list):
            continue

        valid_beats = [
            beat
            for item in objects
            if isinstance(item, dict)
            if (beat := _finite_number(item.get("beat"))) is not None
        ]
        if not valid_beats:
            continue

        level_name = str(level.get("level", {}).get("name", f"level-{level_index}"))
        difficulty = _difficulty_label(level)
        first_beat = math.floor(min(valid_beats) / stride_beats) * stride_beats
        last_beat = max(valid_beats)

        chunk_index = 0
        start_beat = first_beat
        while start_beat + chunk_beats <= last_beat + 1e-6:
            end_beat = start_beat + chunk_beats

            for mode in SUPPORTED_MODES:
                mode_objects = list(
                    _iter_mode_objects(
                        level,
                        mode=mode,
                        start_beat=start_beat,
                        end_beat=end_beat,
                    )
                )
                if len(mode_objects) < 3:
                    continue

                interactions = [
                    item
                    for item in mode_objects
                    if item.get("category") in INTERACTION_CATEGORIES
                ]
                solids = [
                    item
                    for item in mode_objects
                    if item.get("category") == "solid"
                ]

                if not interactions and len(solids) < 4:
                    continue

                all_y = [
                    y
                    for item in mode_objects
                    if (y := _finite_number(item.get("y"))) is not None
                ]
                anchor_y = min(all_y) if all_y else 0.0

                events: list[dict[str, Any]] = []
                for item in interactions[:24]:
                    beat = _finite_number(item.get("beat"))
                    y = _finite_number(item.get("y"))
                    if beat is None:
                        continue

                    events.append(
                        {
                            "beat": round(beat - start_beat, 3),
                            "category": str(item.get("category", "unknown")),
                            "object_id": int(item.get("object_id", 0) or 0),
                            "relative_y": round((y or anchor_y) - anchor_y, 1),
                        }
                    )

                bins: dict[int, list[float]] = {}
                for item in solids:
                    beat = _finite_number(item.get("beat"))
                    y = _finite_number(item.get("y"))
                    if beat is None or y is None:
                        continue

                    bin_index = max(
                        0,
                        min(
                            int(chunk_beats * 2) - 1,
                            int((beat - start_beat) * 2),
                        ),
                    )
                    bins.setdefault(bin_index, []).append(y - anchor_y)

                solid_profile = [
                    {
                        "half_beat": bin_index,
                        "count": len(values),
                        "min_y": round(min(values), 1),
                        "max_y": round(max(values), 1),
                    }
                    for bin_index, values in sorted(bins.items())
                ]

                onset_values = [
                    value
                    for item in mode_objects
                    if (
                        value := _finite_number(item.get("nearest_onset_strength"))
                    ) is not None
                ]

                state = _state_for_chunk(mode_objects, mode)
                chunks.append(
                    {
                        "id": f"{level_index}:{chunk_index}:{mode}",
                        "level": level_name,
                        "difficulty": difficulty,
                        "mode": mode,
                        "start_beat": round(start_beat, 3),
                        "beats": chunk_beats,
                        "entry": state,
                        "interaction_count": len(interactions),
                        "solid_count": len(solids),
                        "intensity": round(mean(onset_values), 4) if onset_values else 0.0,
                        "events": events,
                        "solid_profile": solid_profile[:16],
                    }
                )

            chunk_index += 1
            start_beat += stride_beats

    return chunks


def _reference_score(chunk: dict[str, Any], request: PlannerRequest) -> float:
    if chunk.get("mode") != request.mode:
        return -1_000_000.0

    score = 8.0

    if chunk.get("difficulty") == request.difficulty:
        score += 4.0
    elif chunk.get("difficulty") == "Unknown":
        score += 0.5

    chunk_intensity = float(chunk.get("intensity", 0.0) or 0.0)
    score += max(0.0, 2.0 - abs(chunk_intensity - request.energy) * 3.0)

    entry = chunk.get("entry")
    if isinstance(entry, dict):
        if entry.get("gravity") == request.entry_gravity:
            score += 0.8
        if bool(entry.get("mini", False)) == request.entry_mini:
            score += 0.5
        if entry.get("speed") == request.entry_speed:
            score += 0.4

    event_count = int(chunk.get("interaction_count", 0) or 0)
    score += min(event_count, 16) * 0.08
    if event_count > 28:
        score -= (event_count - 28) * 0.04

    return score


def retrieve_reference_chunks(
    chunks: list[dict[str, Any]],
    request: PlannerRequest,
    *,
    limit: int = 5,
) -> list[dict[str, Any]]:
    scored = [
        (_reference_score(chunk, request), chunk)
        for chunk in chunks
    ]
    scored.sort(key=lambda item: item[0], reverse=True)

    result: list[dict[str, Any]] = []
    used_levels: Counter[str] = Counter()

    for score, chunk in scored:
        if score < -1000:
            continue

        level_name = str(chunk.get("level", ""))
        if used_levels[level_name] >= 2:
            continue

        result.append(chunk)
        used_levels[level_name] += 1

        if len(result) >= limit:
            break

    return result


def build_planner_prompt(
    request: PlannerRequest,
    references: list[dict[str, Any]],
) -> str:
    compact_references = [
        {
            "level": reference.get("level"),
            "difficulty": reference.get("difficulty"),
            "mode": reference.get("mode"),
            "entry": reference.get("entry"),
            "interaction_count": reference.get("interaction_count"),
            "solid_count": reference.get("solid_count"),
            "intensity": reference.get("intensity"),
            "events": reference.get("events"),
            "solid_profile": reference.get("solid_profile"),
        }
        for reference in references
    ]

    return (
        "Design one Geometry Dash gameplay section as ACTION INTENT, not raw object "
        "coordinates. The deterministic compiler will place geometry later. "
        "Use the human reference chunks for rhythm/style inspiration without copying "
        "one chunk literally. Every orb/pad must be required by the intended route; "
        "omit interactions that are optional or decorative. Make the section match the "
        "requested mode and difficulty, avoid repetitive evenly-spaced patterns, and "
        "include clear entry-to-exit progression. For ship/wave use hold/release intent; "
        "for UFO use click timing; for ball use gravity flips; for cube use jumps, "
        "landings, and only necessary orb/pad interactions. "
        "Return only data matching the supplied JSON schema.\n\n"
        f"REQUEST:\n{json.dumps(request.__dict__, indent=2)}\n\n"
        f"HUMAN REFERENCES:\n{json.dumps(compact_references, separators=(',', ':'))}"
    )


def _extract_response_text(payload: dict[str, Any]) -> str:
    for output_item in payload.get("output", []):
        if not isinstance(output_item, dict):
            continue

        for content in output_item.get("content", []):
            if not isinstance(content, dict):
                continue

            if content.get("type") == "output_text":
                text = content.get("text")
                if isinstance(text, str) and text:
                    return text

    raise RuntimeError("OpenAI response did not contain output text")


def request_openai_plan(
    request: PlannerRequest,
    references: list[dict[str, Any]],
    *,
    model: str = "gpt-6-luna",
    reasoning_effort: str = "low",
    timeout_seconds: float = 45.0,
) -> dict[str, Any]:
    api_key = os.environ.get("OPENAI_API_KEY", "").strip()
    if not api_key:
        raise RuntimeError(
            "OPENAI_API_KEY is not set. Set it in your environment before using the LLM planner."
        )

    body = {
        "model": model,
        "reasoning": {"effort": reasoning_effort},
        "input": build_planner_prompt(request, references),
        "max_output_tokens": 3000,
        "text": {
            "format": {
                "type": "json_schema",
                "name": "geometry_dash_gameplay_plan",
                "strict": True,
                "schema": PLAN_SCHEMA,
            }
        },
    }

    http_request = urllib.request.Request(
        "https://api.openai.com/v1/responses",
        data=json.dumps(body).encode("utf-8"),
        method="POST",
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
    )

    try:
        with urllib.request.urlopen(http_request, timeout=timeout_seconds) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        details = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"OpenAI API returned HTTP {exc.code}: {details}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Could not reach the OpenAI API: {exc.reason}") from exc

    return json.loads(_extract_response_text(payload))


def write_reference_library(path: Path, chunks: list[dict[str, Any]]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema_version": 1,
        "chunk_count": len(chunks),
        "chunks": chunks,
    }
    path.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    return path


def load_reference_library(path: Path) -> list[dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    chunks = payload.get("chunks")
    if not isinstance(chunks, list):
        raise TypeError("Reference library does not contain a chunks array")
    return [chunk for chunk in chunks if isinstance(chunk, dict)]
