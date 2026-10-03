from __future__ import annotations

import plistlib

from gd_ai_editor.gameplay import (
    align_gameplay_export,
    build_object_catalog,
    load_gmd_gameplay,
)


def _analysis() -> dict:
    beats = [
        {"time": 1.5 + index * 0.5, "onset_strength": 0.8, "energy": 0.7}
        for index in range(10)
    ]
    return {
        "beats": beats,
        "onsets": [
            {"time": beat["time"], "strength": beat["onset_strength"]}
            for beat in beats
        ],
    }


def test_load_gmd_gameplay_parses_plist_level_string(tmp_path) -> None:
    level_string = (
        "kA1,0,kA2,0,kA3,0,kA4,0,kA8,0,kA13,1.5;"
        "1,1,2,0,3,105,6,0;"
        "1,36,2,155.79,3,165,6,0;"
        "1,202,2,311.58,3,150;"
        "1,8,2,505.29,3,105,32,0.5;"
    )
    payload = {
        "root": {
            "k1": 123,
            "k2": "GMD Test",
            "k5": "Creator",
            "k45": 999,
            "k4": level_string,
        }
    }
    path = tmp_path / "test.gmd"
    path.write_bytes(plistlib.dumps(payload))

    gameplay = load_gmd_gameplay(path, difficulty_label="Hard Demon")

    assert gameplay["source"] == "gmd-import"
    assert gameplay["level"]["name"] == "GMD Test"
    assert gameplay["level"]["difficulty_label"] == "Hard Demon"
    assert gameplay["level"]["start_speed"] == 0
    assert len(gameplay["objects"]) == 4

    block, orb, speed, hazard = gameplay["objects"]
    assert block["category"] == "solid"
    assert orb["category"] == "orb"
    assert speed["object_id"] == 202
    assert speed["trigger"]["name"] == "speed_fast"
    assert hazard["category"] == "hazard"
    assert hazard["scale_x"] == 0.5
    assert hazard["scale_y"] == 0.5

    assert abs(speed["level_time_seconds"] - 1.0) < 0.01
    assert hazard["level_time_seconds"] > speed["level_time_seconds"]


def test_gmd_catalog_uses_trusted_geode_object_types(tmp_path) -> None:
    aligned = {
        "objects": [
            {
                "object_id": 7777,
                "category": "solid",
                "object_type": "slope",
            },
            {
                "object_id": 8888,
                "category": "hazard",
                "object_type": "animated_hazard",
            },
        ]
    }
    catalog = build_object_catalog([aligned])

    level_string = (
        "kA2,0,kA4,0;"
        "1,7777,2,30,3,120;"
        "1,8888,2,60,3,120;"
    )
    path = tmp_path / "catalog.gmd"
    path.write_bytes(
        plistlib.dumps(
            {
                "k2": "Catalog",
                "k4": level_string,
            }
        )
    )

    gameplay = load_gmd_gameplay(path, object_catalog=catalog)

    assert gameplay["objects"][0]["category"] == "solid"
    assert gameplay["objects"][0]["object_type"] == "slope"
    assert gameplay["objects"][1]["category"] == "hazard"
    assert gameplay["objects"][1]["object_type"] == "animated_hazard"


def test_gmd_can_flow_through_existing_alignment_pipeline(tmp_path) -> None:
    level_string = (
        "kA2,0,kA4,0,kA13,1.5;"
        "1,1,2,0,3,105;"
        "1,36,2,155.79,3,165;"
        "1,12,2,311.58,3,150;"
        "1,1,2,467.37,3,135;"
    )
    path = tmp_path / "aligned.gmd"
    path.write_bytes(plistlib.dumps({"k2": "Aligned GMD", "k4": level_string}))

    gameplay = load_gmd_gameplay(path, difficulty_label="Hard Demon")
    aligned = align_gameplay_export(gameplay, _analysis())

    assert aligned["objects"]
    assert aligned["objects"][0]["beat"] == 0.0
    assert aligned["objects"][0]["state_before"]["mode"] == "cube"
    assert aligned["player_state"]["initial"]["mode"] == "cube"
    assert aligned["windows"]
