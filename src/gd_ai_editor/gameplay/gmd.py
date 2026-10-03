from __future__ import annotations

import base64
import math
import plistlib
import zlib
from pathlib import Path
from typing import Any

_SPEED_UNITS_PER_SECOND = {
    "slow": 8.371978759765625 * 30.0,
    "normal": 10.385991096496582 * 30.0,
    "fast": 12.914042472839355 * 30.0,
    "faster": 15.600034713745117 * 30.0,
    "fastest": 19.19999122619629 * 30.0,
}

_SPEED_BY_START_VALUE = {
    0: "normal",
    1: "slow",
    2: "fast",
    3: "faster",
    4: "fastest",
}

_SPEED_PORTAL_IDS = {
    200: "slow",
    201: "normal",
    202: "fast",
    203: "faster",
    1334: "fastest",
}

_OBJECT_TYPE_SEED: dict[int, tuple[str, str]] = {
    1: ("solid", "solid"),
    8: ("hazard", "hazard"),
    39: ("hazard", "hazard"),
    10: ("portal", "inverse_gravity_portal"),
    11: ("portal", "normal_gravity_portal"),
    12: ("portal", "cube_portal"),
    13: ("portal", "ship_portal"),
    35: ("pad", "yellow_pad"),
    36: ("orb", "yellow_orb"),
    45: ("portal", "mirror_on_portal"),
    46: ("portal", "mirror_off_portal"),
    47: ("portal", "ball_portal"),
    67: ("pad", "gravity_pad"),
    84: ("orb", "gravity_orb"),
    99: ("portal", "normal_size_portal"),
    101: ("portal", "mini_portal"),
    111: ("portal", "ufo_portal"),
    140: ("pad", "pink_pad"),
    141: ("orb", "pink_orb"),
    286: ("portal", "dual_portal"),
    287: ("portal", "solo_portal"),
    660: ("portal", "wave_portal"),
    745: ("portal", "robot_portal"),
    1022: ("orb", "green_orb"),
    1331: ("portal", "spider_portal"),
    1333: ("orb", "red_orb"),
    1931: ("portal", "swing_portal"),
}

_TRIGGER_IDS = {
    32,
    33,
    899,
    901,
    1006,
    1007,
    1049,
    1268,
    1346,
    1347,
    1520,
    1595,
    1611,
    1612,
    1613,
    1616,
    1811,
    1812,
    1814,
    1815,
    1817,
    1818,
    1819,
    1912,
    2067,
    2068,
    3032,
    3033,
    3614,
    3615,
    3617,
}


def build_object_catalog(levels: list[dict[str, Any]]) -> dict[int, tuple[str, str]]:
    """Build object-id -> (category, object_type) from trusted Geode exports."""

    votes: dict[int, dict[tuple[str, str], int]] = {}

    for level in levels:
        objects = level.get("objects")
        if not isinstance(objects, list):
            continue

        for item in objects:
            if not isinstance(item, dict):
                continue

            object_id = item.get("object_id")
            if not isinstance(object_id, int):
                continue

            key = (
                str(item.get("category", "other")),
                str(item.get("object_type", "unknown")),
            )
            object_votes = votes.setdefault(object_id, {})
            object_votes[key] = object_votes.get(key, 0) + 1

    catalog = dict(_OBJECT_TYPE_SEED)
    for object_id, object_votes in votes.items():
        catalog[object_id] = max(object_votes.items(), key=lambda pair: pair[1])[0]

    return catalog


def _find_level_dict(value: Any) -> dict[str, Any] | None:
    if isinstance(value, dict):
        if "k4" in value:
            return value

        root = value.get("root")
        if isinstance(root, dict) and "k4" in root:
            return root

        for child in value.values():
            found = _find_level_dict(child)
            if found is not None:
                return found

    if isinstance(value, list):
        for child in value:
            found = _find_level_dict(child)
            if found is not None:
                return found

    return None


def _decode_level_string(value: Any) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(".gmd does not contain a usable k4 level string")

    if ";" in value and "," in value:
        return value

    encoded = value.strip().replace("-", "+").replace("_", "/")
    encoded += "=" * (-len(encoded) % 4)

    try:
        compressed = base64.b64decode(encoded)
    except ValueError as exc:
        raise ValueError("Could not decode the .gmd k4 level string") from exc

    for window_bits in (15 | 32, 15, -15):
        try:
            return zlib.decompress(compressed, window_bits).decode("utf-8")
        except (zlib.error, UnicodeDecodeError):
            continue

    raise ValueError("Could not decompress the .gmd k4 level string")


def _pairs(segment: str) -> dict[str, str]:
    parts = segment.split(",")
    return {
        parts[index]: parts[index + 1]
        for index in range(0, len(parts) - 1, 2)
    }


def _as_int(value: Any, default: int = 0) -> int:
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


def _as_float(value: Any, default: float = 0.0) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError):
        return default

    return result if math.isfinite(result) else default


def _as_bool(value: Any) -> bool:
    return str(value) in {"1", "true", "True"}


def _groups(properties: dict[str, str]) -> list[int]:
    raw = properties.get("57", "")
    if not raw:
        single = _as_int(properties.get("33"), 0)
        return [single] if single > 0 else []

    result: list[int] = []
    for item in raw.split("."):
        group_id = _as_int(item, 0)
        if group_id > 0:
            result.append(group_id)
    return result


def _classify_object(
    object_id: int,
    catalog: dict[int, tuple[str, str]],
) -> tuple[str, str]:
    if object_id in _SPEED_PORTAL_IDS:
        return "modifier", "modifier"

    if object_id in catalog:
        return catalog[object_id]

    if object_id in _TRIGGER_IDS:
        return "modifier", "modifier"

    return "other", "unknown"


def _object_scale(properties: dict[str, str]) -> tuple[float, float]:
    uniform = _as_float(properties.get("32"), 1.0)
    scale_x = _as_float(properties.get("128"), uniform)
    scale_y = _as_float(properties.get("129"), uniform)

    if _as_bool(properties.get("4")):
        scale_x *= -1.0
    if _as_bool(properties.get("5")):
        scale_y *= -1.0

    return scale_x, scale_y


def _level_times(
    objects: list[dict[str, Any]],
    *,
    start_speed: str,
) -> dict[int, float]:
    ordered = sorted(
        enumerate(objects),
        key=lambda pair: (float(pair[1]["x"]), pair[0]),
    )
    current_speed = start_speed
    previous_x = 0.0
    elapsed = 0.0
    result: dict[int, float] = {}

    for original_index, item in ordered:
        x = max(previous_x, float(item["x"]))
        speed_units = _SPEED_UNITS_PER_SECOND.get(
            current_speed,
            _SPEED_UNITS_PER_SECOND["normal"],
        )

        elapsed += (x - previous_x) / speed_units
        result[original_index] = elapsed
        previous_x = x

        next_speed = _SPEED_PORTAL_IDS.get(int(item.get("object_id", 0) or 0))
        if next_speed is not None:
            current_speed = next_speed

    return result


def load_gmd_gameplay(
    path: str | Path,
    *,
    difficulty_label: str = "Unknown",
    object_catalog: dict[int, tuple[str, str]] | None = None,
) -> dict[str, Any]:
    """Parse a .gmd into the same raw gameplay schema used by Geode exports."""

    source = Path(path)

    try:
        plist = plistlib.loads(source.read_bytes())
    except Exception as exc:
        raise ValueError(f"Could not parse {source.name} as a .gmd plist") from exc

    level_dict = _find_level_dict(plist)
    if level_dict is None:
        raise ValueError(f"{source.name} does not contain a Geometry Dash level dictionary")

    level_string = _decode_level_string(level_dict.get("k4"))
    segments = [segment for segment in level_string.split(";") if segment]
    if not segments:
        raise ValueError(f"{source.name} contains an empty Geometry Dash level string")

    header = _pairs(segments[0])
    start_mode = _as_int(header.get("kA2"), 0)
    start_speed_value = _as_int(header.get("kA4"), 0)
    start_speed = _SPEED_BY_START_VALUE.get(start_speed_value, "normal")
    song_offset = _as_float(header.get("kA13"), 0.0)

    catalog = dict(_OBJECT_TYPE_SEED)
    if object_catalog:
        catalog.update(object_catalog)

    objects: list[dict[str, Any]] = []

    for unique_id, segment in enumerate(segments[1:], start=1):
        properties = _pairs(segment)
        object_id = _as_int(properties.get("1"), 0)
        if object_id <= 0:
            continue

        category, object_type = _classify_object(object_id, catalog)
        scale_x, scale_y = _object_scale(properties)

        item: dict[str, Any] = {
            "unique_id": unique_id,
            "object_id": object_id,
            "object_type_id": -1,
            "object_type": object_type,
            "category": category,
            "x": _as_float(properties.get("2"), 0.0),
            "y": _as_float(properties.get("3"), 0.0),
            "rotation": _as_float(properties.get("6"), 0.0),
            "scale_x": scale_x,
            "scale_y": scale_y,
            "no_touch": _as_bool(properties.get("121")),
            "passable": False,
            "hidden": False,
            "high_detail": False,
            "editor_layer": _as_int(properties.get("20"), 0),
            "editor_layer_2": _as_int(properties.get("61"), 0),
            "groups": _groups(properties),
            "gmd_properties": properties,
        }

        if object_id in _SPEED_PORTAL_IDS:
            item["trigger"] = {"name": f"speed_{_SPEED_PORTAL_IDS[object_id]}"}

        objects.append(item)

    level_times = _level_times(objects, start_speed=start_speed)
    for index, item in enumerate(objects):
        level_time = level_times.get(index, 0.0)
        item["level_time_seconds"] = round(level_time, 6)
        item["audio_time_seconds"] = round(level_time + song_offset, 6)

    objects.sort(
        key=lambda item: (
            float(item.get("x", 0.0)),
            float(item.get("y", 0.0)),
            int(item.get("unique_id", 0)),
        )
    )

    category_counts: dict[str, int] = {}
    for item in objects:
        category = str(item["category"])
        category_counts[category] = category_counts.get(category, 0) + 1

    return {
        "schema_version": 1,
        "source": "gmd-import",
        "source_file": source.name,
        "level": {
            "id": _as_int(level_dict.get("k1"), 0),
            "name": str(level_dict.get("k2") or source.stem),
            "creator": str(level_dict.get("k5") or ""),
            "song_id": _as_int(level_dict.get("k45"), 0),
            "audio_track": _as_int(
                header.get("kA1"),
                _as_int(level_dict.get("k8"), 0),
            ),
            "song_offset_seconds": song_offset,
            "platformer": _as_bool(header.get("kA22")),
            "start_mode": start_mode,
            "start_speed": start_speed_value,
            "start_mini": _as_bool(header.get("kA3")),
            "start_dual": _as_bool(header.get("kA8")),
            "start_mirror": False,
            "reverse_gameplay": False,
            "stars": 0,
            "is_demon": "Demon" in difficulty_label,
            "demon_difficulty": 0,
            "difficulty_enum": 0,
            "difficulty_label": difficulty_label,
            "difficulty_label_source": (
                "gmd_import_override"
                if difficulty_label != "Unknown"
                else "gmd_metadata"
            ),
        },
        "summary": {
            "total_editor_objects": len(objects),
            "exported_gameplay_objects": len(objects),
            "category_counts": category_counts,
            "catalog_classified_objects": sum(
                1 for item in objects if item["category"] != "other"
            ),
            "unknown_objects": sum(
                1 for item in objects if item["category"] == "other"
            ),
        },
        "objects": objects,
        "gmd": {
            "schema_version": 1,
            "header": header,
            "raw_object_count": len(segments) - 1,
        },
    }
