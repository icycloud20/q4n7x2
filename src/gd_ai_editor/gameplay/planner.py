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
from statistics import mean, median
from typing import Any

SUPPORTED_MODES = ("cube", "ship", "ball", "ufo", "wave")
INTERACTION_CATEGORIES = {"hazard", "orb", "pad", "portal"}
RENDER_CATEGORIES = {"solid", "hazard", "orb", "pad"}

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
                    "reference_ids": {
                        "type": "array",
                        "minItems": 2,
                        "maxItems": 2,
                        "items": {"type": "string", "minLength": 1},
                    },
                    "actions": {
                        "type": "array",
                        "minItems": 3,
                        "maxItems": 24,
                        "items": ACTION_SCHEMA,
                    },
                },
                "required": [
                    "index",
                    "mode",
                    "concept",
                    "intensity",
                    "reference_ids",
                    "actions",
                ],
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
                solid_y = [
                    y
                    for item in mode_objects
                    if item.get("category") == "solid"
                    if (y := _finite_number(item.get("y"))) is not None
                ]
                anchor_source = solid_y if solid_y else all_y
                anchor_y = float(median(anchor_source)) if anchor_source else 0.0

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

                geometry_cells: list[dict[str, Any]] = []
                seen_geometry: set[tuple[Any, ...]] = set()

                for item in mode_objects:
                    category = str(item.get("category", ""))
                    if category not in RENDER_CATEGORIES:
                        continue

                    beat = _finite_number(item.get("beat"))
                    y = _finite_number(item.get("y"))
                    if beat is None or y is None:
                        continue

                    beat_eighth = max(
                        0,
                        min(
                            int(round(chunk_beats * 8)),
                            int(round((beat - start_beat) * 8)),
                        ),
                    )
                    y_step = int(round((y - anchor_y) / 15.0))
                    object_id = int(item.get("object_id", 0) or 0)
                    rotation_value = _finite_number(item.get("rotation")) or 0.0
                    rotation = int(round(rotation_value / 45.0) * 45)

                    # Collapse duplicate collision solids at the same normalized
                    # location while preserving distinct hazards/orbs/pads.
                    key = (
                        beat_eighth,
                        y_step,
                        category,
                        object_id,
                        rotation,
                    )
                    if key in seen_geometry:
                        continue
                    seen_geometry.add(key)

                    geometry_cells.append(
                        {
                            "beat_eighth": beat_eighth,
                            "y_step": y_step,
                            "category": category,
                            "object_id": object_id,
                            "rotation": rotation,
                        }
                    )

                geometry_cells.sort(
                    key=lambda item: (
                        int(item["beat_eighth"]),
                        int(item["y_step"]),
                        str(item["category"]),
                    )
                )

                # Keep a detailed but bounded collision representation. This is
                # what lets the LLM/compiler see actual human micro-structure
                # instead of only min/max solid summaries.
                non_solids = [
                    item for item in geometry_cells if item["category"] != "solid"
                ][:48]
                solid_cells = [
                    item for item in geometry_cells if item["category"] == "solid"
                ]
                if len(solid_cells) > 128:
                    last = len(solid_cells) - 1
                    solid_cells = [
                        solid_cells[round(index * last / 127)]
                        for index in range(128)
                    ]

                geometry_cells = sorted(
                    solid_cells + non_solids,
                    key=lambda item: (
                        int(item["beat_eighth"]),
                        int(item["y_step"]),
                        str(item["category"]),
                    ),
                )

                entry_cells = [
                    int(item["y_step"])
                    for item in geometry_cells
                    if int(item["beat_eighth"]) <= 8
                    and item["category"] == "solid"
                ]
                exit_cells = [
                    int(item["y_step"])
                    for item in geometry_cells
                    if int(item["beat_eighth"]) >= int(chunk_beats * 8) - 8
                    and item["category"] == "solid"
                ]

                entry_y_step = int(round(median(entry_cells))) if entry_cells else 0
                exit_y_step = int(round(median(exit_cells))) if exit_cells else 0
                vertical_span_steps = (
                    max(int(item["y_step"]) for item in geometry_cells)
                    - min(int(item["y_step"]) for item in geometry_cells)
                    if geometry_cells
                    else 0
                )

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
                        "geometry_cells": geometry_cells,
                        "entry_y_step": entry_y_step,
                        "exit_y_step": exit_y_step,
                        "vertical_span_steps": vertical_span_steps,
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

    vertical_span = int(chunk.get("vertical_span_steps", 0) or 0)
    if vertical_span > 36:
        score -= (vertical_span - 36) * 0.08
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
            "id": reference.get("id"),
            "level": reference.get("level"),
            "difficulty": reference.get("difficulty"),
            "mode": reference.get("mode"),
            "entry": reference.get("entry"),
            "interaction_count": reference.get("interaction_count"),
            "solid_count": reference.get("solid_count"),
            "intensity": reference.get("intensity"),
            "entry_y_step": reference.get("entry_y_step"),
            "exit_y_step": reference.get("exit_y_step"),
            "vertical_span_steps": reference.get("vertical_span_steps"),
            "events": reference.get("events"),
            "geometry_cells": reference.get("geometry_cells"),
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


def _read_openai_api_key() -> str:
    key = os.environ.get("OPENAI_API_KEY", "").strip()
    if key:
        return key

    # Windows child processes inherit their parent's environment. If Steam was
    # already running when OPENAI_API_KEY was added, GD may not inherit it even
    # though Windows has saved it correctly. Read the persisted user variable
    # directly as a fallback.
    if os.name == "nt":
        try:
            import winreg

            with winreg.OpenKey(
                winreg.HKEY_CURRENT_USER,
                "Environment",
                0,
                winreg.KEY_READ,
            ) as key_handle:
                value, _ = winreg.QueryValueEx(key_handle, "OPENAI_API_KEY")

            if isinstance(value, str) and value.strip():
                return value.strip()
        except (FileNotFoundError, OSError):
            pass

    return ""


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
    api_key = _read_openai_api_key()
    if not api_key:
        raise RuntimeError(
            "OPENAI_API_KEY is not available in the process environment or Windows user variables."
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
    song_offset: float = 0.0,
    section_beats: int = 8,
    maximum_sections: int = 15,
) -> list[dict[str, Any]]:
    beats = analysis.get("beats")
    if not isinstance(beats, list):
        raise TypeError("Song analysis does not contain a beats array")

    usable = [
        beat
        for beat in beats
        if isinstance(beat, dict)
        and (_finite_number(beat.get("time")) or 0.0) >= song_offset + 1.25
    ]
    if len(usable) < 12:
        raise ValueError("Song analysis does not contain enough beats after the song offset")

    # Match the Geode generator's four-beat lead-in after its offset filter.
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
    used_ids: set[str] = set()

    energy_targets = (
        max(0.25, average_energy - 0.25),
        average_energy,
        min(1.0, average_energy + 0.20),
    )

    for mode in SUPPORTED_MODES:
        mode_references: list[dict[str, Any]] = []

        for target_energy in energy_targets:
            request = PlannerRequest(
                mode=mode,
                difficulty=difficulty,
                beats=4.0,
                energy=target_energy,
                onset=target_energy,
                entry_speed="fast",
            )

            for chunk in retrieve_reference_chunks(chunks, request, limit=3):
                chunk_id = str(chunk.get("id", ""))
                if not chunk_id or chunk_id in used_ids:
                    continue
                used_ids.add(chunk_id)
                mode_references.append(chunk)

                if len(mode_references) >= 5:
                    break

            if len(mode_references) >= 5:
                break

        references.extend(mode_references)

    return references

def build_layout_prompt(
    analysis: dict[str, Any],
    chunks: list[dict[str, Any]],
    *,
    difficulty: str,
    song_offset: float = 0.0,
) -> tuple[str, int, list[dict[str, Any]]]:
    sections = _analysis_sections(analysis, song_offset=song_offset)
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
            "id": reference.get("id"),
            "level": reference.get("level"),
            "difficulty": reference.get("difficulty"),
            "mode": reference.get("mode"),
            "entry": reference.get("entry"),
            "interaction_count": reference.get("interaction_count"),
            "solid_count": reference.get("solid_count"),
            "intensity": reference.get("intensity"),
            "entry_y_step": reference.get("entry_y_step"),
            "exit_y_step": reference.get("exit_y_step"),
            "vertical_span_steps": reference.get("vertical_span_steps"),
            "events": reference.get("events"),
            "geometry_cells": reference.get("geometry_cells"),
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
        "breathing moments while staying at the target difficulty. Every section "
        "must select exactly two reference_ids from HUMAN REFERENCE CHUNKS with "
        "the same mode. Those are real four-beat human gameplay chunks and the "
        "local compiler will stitch/adapt their actual collision geometry. Pick "
        "references whose intensity and structure fit the song section, avoid "
        "reusing the same pair in adjacent sections, and mix source levels when "
        "possible. The action list is high-level intent; reference geometry is "
        "the primary visual/gameplay structure. Use all five "
        "supported modes across the full plan when the level is long enough, but "
        "do not rotate through them mechanically. Avoid repeating the same concept "
        "or evenly-spaced obstacle rhythm in adjacent sections. Every orb or pad "
        "must be route-required; optional interactions are forbidden. "
        "Cube actions may use jump/land plus required yellow/pink orbs/pads; do not "
        "use blue/green orbs in this v1 compiler. Ship uses hold and release timing. "
        "Ball uses gravity_flip. UFO uses ufo_click. Wave uses wave_hold and wave_release. "
        "A section may start with mode_portal "
        "when its mode differs from the previous section. Keep action beat values "
        "between 0 and 8. height_delta is in 30-unit gameplay steps, relative to "
        "the current route, not an absolute coordinate. Use human references as "
        "style/rhythm examples but do not copy one literally. "
        "Return only data matching the supplied JSON schema.\n\n"
        f"SONG SECTIONS:\n{json.dumps(sections, separators=(',', ':'))}\n\n"
        "HUMAN REFERENCE CHUNKS:\n"
        f"{json.dumps(compact_references, separators=(',', ':'))}"
    )

    return prompt, len(sections), references


def _validate_layout_plan(
    plan: dict[str, Any],
    expected_sections: int,
    references_by_id: dict[str, dict[str, Any]],
) -> None:
    sections = plan.get("sections")
    if not isinstance(sections, list) or len(sections) != expected_sections:
        raise RuntimeError(
            "Planner returned the wrong number of sections: "
            f"expected {expected_sections}, got "
            f"{len(sections) if isinstance(sections, list) else 0}"
        )

    allowed_by_mode = {
        "cube": {
            "jump",
            "land",
            "orb_yellow",
            "orb_pink",
            "pad_yellow",
            "pad_pink",
            "mode_portal",
        },
        "ship": {"hold", "release", "mode_portal"},
        "ball": {"gravity_flip", "mode_portal"},
        "ufo": {"ufo_click", "mode_portal"},
        "wave": {"wave_hold", "wave_release", "mode_portal"},
    }
    minimum_primary_actions = {
        "cube": 4,
        "ship": 4,
        "ball": 4,
        "ufo": 4,
        "wave": 5,
    }

    previous_reference_pair: tuple[str, str] | None = None

    for expected_index, section in enumerate(sections):
        if not isinstance(section, dict) or section.get("index") != expected_index:
            raise RuntimeError("Planner returned sections out of order")

        mode = str(section.get("mode", ""))
        if mode not in allowed_by_mode:
            raise RuntimeError(f"Planner returned unsupported mode {mode!r}")

        reference_ids = section.get("reference_ids")
        if not isinstance(reference_ids, list) or len(reference_ids) != 2:
            raise RuntimeError(
                f"Section {expected_index} must select exactly two human references"
            )

        reference_pair = (str(reference_ids[0]), str(reference_ids[1]))
        if reference_pair[0] == reference_pair[1]:
            raise RuntimeError(
                f"Section {expected_index} selected the same human chunk twice"
            )
        if previous_reference_pair == reference_pair:
            raise RuntimeError(
                f"Section {expected_index} repeated the previous human chunk pair"
            )
        previous_reference_pair = reference_pair

        for reference_id in reference_ids:
            reference = references_by_id.get(str(reference_id))
            if reference is None:
                raise RuntimeError(
                    f"Section {expected_index} selected unknown reference {reference_id!r}"
                )
            if reference.get("mode") != mode:
                raise RuntimeError(
                    f"Section {expected_index} selected a {reference.get('mode')} "
                    f"reference for {mode} gameplay"
                )

        actions = section.get("actions")
        if not isinstance(actions, list):
            raise TypeError(f"Section {expected_index} has no action list")

        primary_actions = 0
        previous_beat = -1.0

        for action in actions:
            if not isinstance(action, dict):
                raise TypeError(f"Section {expected_index} contains an invalid action")

            action_name = str(action.get("action", ""))
            beat = _finite_number(action.get("beat"))
            if beat is None or beat < 0.0 or beat > 8.0:
                raise RuntimeError(
                    f"Section {expected_index} has an action outside its 8-beat window"
                )

            if beat < previous_beat:
                raise RuntimeError(f"Section {expected_index} actions are not time-ordered")
            previous_beat = beat

            if action_name not in allowed_by_mode[mode]:
                raise RuntimeError(
                    f"Section {expected_index} ({mode}) contains incompatible "
                    f"action {action_name!r}"
                )

            if action_name != "mode_portal":
                primary_actions += 1

            if action_name.startswith(("orb_", "pad_")) and not bool(
                action.get("required", False)
            ):
                raise RuntimeError(
                    f"Section {expected_index} contains an optional orb/pad"
                )

        if primary_actions < minimum_primary_actions[mode]:
            raise RuntimeError(
                f"Section {expected_index} ({mode}) is too sparse: "
                f"{primary_actions} primary actions"
            )


def _attach_reference_render_sections(
    plan: dict[str, Any],
    chunks_by_id: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    render_sections: list[dict[str, Any]] = []

    sections = plan.get("sections")
    if not isinstance(sections, list):
        return plan

    for section in sections:
        if not isinstance(section, dict):
            continue

        reference_ids = [
            str(value)
            for value in section.get("reference_ids", [])
            if isinstance(value, str)
        ][:2]
        if len(reference_ids) != 2:
            continue

        render_objects: list[dict[str, Any]] = []
        entry_y_step = 0
        exit_y_step = 0
        source_levels: list[str] = []

        for half_index, reference_id in enumerate(reference_ids):
            reference = chunks_by_id.get(reference_id)
            if not isinstance(reference, dict):
                continue

            source_levels.append(str(reference.get("level", "")))
            if half_index == 0:
                entry_y_step = int(reference.get("entry_y_step", 0) or 0)
            else:
                exit_y_step = int(reference.get("exit_y_step", 0) or 0)

            for item in reference.get("geometry_cells", []):
                if not isinstance(item, dict):
                    continue

                beat_eighth = int(item.get("beat_eighth", 0) or 0)
                beat = half_index * 4.0 + beat_eighth / 8.0
                if beat < half_index * 4.0 or beat > (half_index + 1) * 4.0:
                    continue

                render_objects.append(
                    {
                        "beat": round(beat, 3),
                        "y_step": int(item.get("y_step", 0) or 0),
                        "category": str(item.get("category", "")),
                        "object_id": int(item.get("object_id", 0) or 0),
                        "rotation": int(item.get("rotation", 0) or 0),
                    }
                )

        # Do not preserve duplicate collision cells created by editor layering.
        unique: dict[tuple[Any, ...], dict[str, Any]] = {}
        for item in render_objects:
            key = (
                round(float(item["beat"]) * 8),
                int(item["y_step"]),
                str(item["category"]),
                int(item["object_id"]),
                int(item["rotation"]),
            )
            unique.setdefault(key, item)

        render_objects = sorted(
            unique.values(),
            key=lambda item: (
                float(item["beat"]),
                int(item["y_step"]),
                str(item["category"]),
            ),
        )

        render_sections.append(
            {
                "index": int(section.get("index", 0) or 0),
                "mode": str(section.get("mode", "cube")),
                "entry_y_step": entry_y_step,
                "exit_y_step": exit_y_step,
                "source_chunk_ids": reference_ids,
                "source_levels": source_levels,
                "objects": render_objects,
            }
        )

    plan["render_strategy"] = "human_chunk_adaptation_v1"
    plan["render_sections"] = render_sections
    return plan


def request_openai_layout(
    analysis: dict[str, Any],
    chunks: list[dict[str, Any]],
    *,
    difficulty: str = "Hard Demon",
    model: str = "gpt-6-luna",
    reasoning_effort: str = "low",
    song_offset: float = 0.0,
    timeout_seconds: float = 90.0,
) -> dict[str, Any]:
    api_key = _read_openai_api_key()
    if not api_key:
        raise RuntimeError(
            "OPENAI_API_KEY is not available in the process environment or Windows user variables."
        )

    prompt, expected_sections, candidate_references = build_layout_prompt(
        analysis,
        chunks,
        difficulty=difficulty,
        song_offset=song_offset,
    )
    references_by_id = {
        str(reference.get("id")): reference
        for reference in candidate_references
        if reference.get("id")
    }

    def perform_request(prompt_text: str) -> dict[str, Any]:
        body = {
            "model": model,
            "reasoning": {"effort": reasoning_effort},
            "input": prompt_text,
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
            with urllib.request.urlopen(
                http_request,
                timeout=timeout_seconds,
            ) as response:
                payload = json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            details = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(
                f"OpenAI API returned HTTP {exc.code}: {details}"
            ) from exc
        except urllib.error.URLError as exc:
            raise RuntimeError(
                f"Could not reach the OpenAI API: {exc.reason}"
            ) from exc

        return json.loads(_extract_response_text(payload))

    plan = perform_request(prompt)
    try:
        _validate_layout_plan(plan, expected_sections, references_by_id)
        return _attach_reference_render_sections(plan, references_by_id)
    except (RuntimeError, TypeError) as error:
        repair_prompt = (
            prompt
            + "\n\nYOUR PREVIOUS PLAN FAILED THE LOCAL SEMANTIC VALIDATOR. "
            "Return a complete corrected plan, not a patch. "
            f"VALIDATION ERROR: {error}\n"
            "PREVIOUS PLAN:\n"
            + json.dumps(plan, separators=(",", ":"))
        )
        repaired = perform_request(repair_prompt)
        _validate_layout_plan(repaired, expected_sections, references_by_id)
        return _attach_reference_render_sections(repaired, references_by_id)


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

    chosen_levels: dict[str, tuple[dict[str, Any], int, bool]] = {}

    for source_path in aligned_files:
        level = json.loads(source_path.read_text(encoding="utf-8"))
        metadata = level.get("level")
        if not isinstance(metadata, dict):
            metadata = {}

        level_name = str(metadata.get("name", source_path.stem))
        labeled = str(metadata.get("difficulty_label", "Unknown")) != "Unknown"
        modified = source_path.stat().st_mtime_ns

        previous = chosen_levels.get(level_name)
        if previous is None:
            chosen_levels[level_name] = (level, modified, labeled)
            continue

        _, previous_modified, previous_labeled = previous
        if (labeled and not previous_labeled) or (
            labeled == previous_labeled and modified > previous_modified
        ):
            chosen_levels[level_name] = (level, modified, labeled)

    levels = [
        value[0]
        for _, value in sorted(chosen_levels.items())
    ]
    chunks = extract_reference_chunks(levels)
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    cache_path.write_text(
        json.dumps(
            {
                "schema_version": 2,
                "source_signature": source_signature,
                "source_level_count": len(levels),
                "source_file_count": len(aligned_files),
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
        "schema_version": 2,
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
