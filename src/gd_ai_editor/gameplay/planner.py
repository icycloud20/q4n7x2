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

ACTION_SCHEMA: dict[str, Any] = PLAN_SCHEMA["properties"]["actions"]["items"]

LAYOUT_PLAN_SCHEMA: dict[str, Any] = {
    "type": "object",
    "properties": {
        "difficulty": {"type": "string"},
        "sections": {
            "type": "array",
            "minItems": 1,
            "maxItems": 16,
            "items": {
                "type": "object",
                "properties": {
                    "index": {"type": "integer", "minimum": 0, "maximum": 15},
                    "mode": {"type": "string", "enum": list(SUPPORTED_MODES)},
                    "concept": {"type": "string", "minLength": 1},
                    "intensity": {"type": "number", "minimum": 0, "maximum": 1},
                    "actions": {
                        "type": "array",
                        "minItems": 3,
                        "maxItems": 24,
                        "items": ACTION_SCHEMA,
                    },
                },
                "required": ["index", "mode", "concept", "intensity", "actions"],
                "additionalProperties": False,
            },
        },
    },
    "required": ["difficulty", "sections"],
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



def _analysis_sections(
    analysis: dict[str, Any],
    *,
    section_beats: int = 8,
    maximum_sections: int = 15,
) -> list[dict[str, Any]]:
    beats = analysis.get("beats")
    if not isinstance(beats, list):
        raise TypeError("Song analysis does not contain a beats array")

    usable = [beat for beat in beats if isinstance(beat, dict)]
    if len(usable) < 12:
        raise ValueError("Song analysis does not contain enough beats")

    # Match the Geode generator's four-beat lead-in.
    usable = usable[4:]
    sections: list[dict[str, Any]] = []

    for index in range(0, len(usable) - section_beats + 1, section_beats):
        if len(sections) >= maximum_sections:
            break

        group = usable[index : index + section_beats]
        energy = [
            value
            for beat in group
            if (value := _finite_number(beat.get("energy"))) is not None
        ]
        onset = [
            value
            for beat in group
            if (
                value := _finite_number(
                    beat.get("onset_strength", beat.get("onset", 0.0))
                )
            ) is not None
        ]

        sections.append(
            {
                "index": len(sections),
                "beats": section_beats,
                "energy": round(mean(energy), 4) if energy else 0.0,
                "onset": round(mean(onset), 4) if onset else 0.0,
                "start_time": round(
                    float(_finite_number(group[0].get("time")) or 0.0),
                    4,
                ),
                "end_time": round(
                    float(_finite_number(group[-1].get("time")) or 0.0),
                    4,
                ),
            }
        )

    return sections


def _layout_reference_set(
    chunks: list[dict[str, Any]],
    *,
    difficulty: str,
    average_energy: float,
) -> list[dict[str, Any]]:
    references: list[dict[str, Any]] = []

    for mode in SUPPORTED_MODES:
        request = PlannerRequest(
            mode=mode,
            difficulty=difficulty,
            beats=4.0,
            energy=average_energy,
            onset=average_energy,
            entry_speed="fast",
        )
        references.extend(retrieve_reference_chunks(chunks, request, limit=2))

    return references


def build_layout_prompt(
    analysis: dict[str, Any],
    chunks: list[dict[str, Any]],
    *,
    difficulty: str,
) -> tuple[str, int]:
    sections = _analysis_sections(analysis)
    if not sections:
        raise ValueError("No song sections were available for planning")

    average_energy = mean(section["energy"] for section in sections)
    references = _layout_reference_set(
        chunks,
        difficulty=difficulty,
        average_energy=average_energy,
    )

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

    prompt = (
        "You are the gameplay director for a Geometry Dash level. Plan the whole "
        "level as action intent only; never output raw x/y object coordinates. "
        f"Target difficulty is {difficulty}. There are exactly {len(sections)} "
        "eight-beat sections, and your output must contain exactly that many "
        "sections with indices 0 through "
        f"{len(sections) - 1} in order. "
        "Use the song energy/onset contour to create escalation, contrast, and "
        "breathing moments while staying at the target difficulty. Use all five "
        "supported modes across the full plan when the level is long enough, but "
        "do not rotate through them mechanically. Avoid repeating the same concept "
        "or evenly-spaced obstacle rhythm in adjacent sections. Every orb or pad "
        "must be route-required; optional interactions are forbidden. "
        "Cube actions may use jump/land and required orbs/pads. Ship uses hold and "
        "release timing. Ball uses gravity_flip and land intent. UFO uses ufo_click. "
        "Wave uses wave_hold and wave_release. A section may start with mode_portal "
        "when its mode differs from the previous section. Keep action beat values "
        "between 0 and 8. height_delta is in 30-unit gameplay steps, relative to "
        "the current route, not an absolute coordinate. Use human references as "
        "style/rhythm examples but do not copy one literally. "
        "Return only data matching the supplied JSON schema.\n\n"
        f"SONG SECTIONS:\n{json.dumps(sections, separators=(',', ':'))}\n\n"
        "HUMAN REFERENCE CHUNKS:\n"
        f"{json.dumps(compact_references, separators=(',', ':'))}"
    )

    return prompt, len(sections)


def request_openai_layout(
    analysis: dict[str, Any],
    chunks: list[dict[str, Any]],
    *,
    difficulty: str = "Hard Demon",
    model: str = "gpt-6-luna",
    reasoning_effort: str = "low",
    timeout_seconds: float = 90.0,
) -> dict[str, Any]:
    api_key = os.environ.get("OPENAI_API_KEY", "").strip()
    if not api_key:
        raise RuntimeError(
            "OPENAI_API_KEY is not set. Set it in your environment before using the LLM planner."
        )

    prompt, expected_sections = build_layout_prompt(
        analysis,
        chunks,
        difficulty=difficulty,
    )

    body = {
        "model": model,
        "reasoning": {"effort": reasoning_effort},
        "input": prompt,
        "max_output_tokens": 12000,
        "text": {
            "format": {
                "type": "json_schema",
                "name": "geometry_dash_level_plan",
                "strict": True,
                "schema": LAYOUT_PLAN_SCHEMA,
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

    plan = json.loads(_extract_response_text(payload))
    sections = plan.get("sections")
    if not isinstance(sections, list) or len(sections) != expected_sections:
        raise RuntimeError(
            "Planner returned the wrong number of sections: "
            f"expected {expected_sections}, got "
            f"{len(sections) if isinstance(sections, list) else 0}"
        )

    for expected_index, section in enumerate(sections):
        if not isinstance(section, dict) or section.get("index") != expected_index:
            raise RuntimeError("Planner returned sections out of order")

    return plan


def build_or_refresh_reference_library(
    training_directory: Path,
    cache_path: Path,
) -> list[dict[str, Any]]:
    aligned_files = sorted(training_directory.glob("*-aligned.json"))
    if not aligned_files:
        raise FileNotFoundError(
            f"No aligned gameplay exports were found in {training_directory}"
        )

    newest_source = max(path.stat().st_mtime_ns for path in aligned_files)
    source_signature = {
        "files": len(aligned_files),
        "newest_mtime_ns": newest_source,
    }

    if cache_path.exists():
        try:
            cached = json.loads(cache_path.read_text(encoding="utf-8"))
            if cached.get("source_signature") == source_signature:
                chunks = cached.get("chunks")
                if isinstance(chunks, list):
                    return [chunk for chunk in chunks if isinstance(chunk, dict)]
        except (OSError, json.JSONDecodeError):
            pass

    levels = [
        json.loads(path.read_text(encoding="utf-8"))
        for path in aligned_files
    ]
    chunks = extract_reference_chunks(levels)
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    cache_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "source_signature": source_signature,
                "source_level_count": len(levels),
                "chunk_count": len(chunks),
                "chunks": chunks,
            },
            separators=(",", ":"),
        )
        + "\n",
        encoding="utf-8",
    )
    return chunks


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
