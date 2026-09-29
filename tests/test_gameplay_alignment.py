from __future__ import annotations

from gd_ai_editor.gameplay import align_gameplay_export


def test_align_gameplay_export_interpolates_beats_and_windows() -> None:
    analysis = {
        "beats": [
            {"time": 1.0},
            {"time": 1.5},
            {"time": 2.0},
            {"time": 2.5},
            {"time": 3.0},
            {"time": 3.5},
            {"time": 4.0},
            {"time": 4.5},
            {"time": 5.0},
            {"time": 5.5},
        ],
        "onsets": [
            {"time": 1.25, "strength": 0.4},
            {"time": 2.0, "strength": 0.9},
        ],
    }

    gameplay = {
        "schema_version": 1,
        "objects": [
            {
                "unique_id": 1,
                "category": "solid",
                "x": 100.0,
                "audio_time_seconds": 1.0,
            },
            {
                "unique_id": 2,
                "category": "orb",
                "x": 200.0,
                "audio_time_seconds": 1.75,
            },
            {
                "unique_id": 3,
                "category": "hazard",
                "x": 300.0,
                "audio_time_seconds": 5.75,
            },
        ],
    }

    aligned = align_gameplay_export(gameplay, analysis)

    assert aligned["objects"][0]["beat"] == 0.0
    assert aligned["objects"][1]["beat"] == 1.5
    assert aligned["objects"][2]["beat"] == 9.5

    assert aligned["objects"][1]["nearest_beat_error_seconds"] == -0.25
    assert aligned["objects"][1]["nearest_onset_time"] == 2.0
    assert aligned["objects"][1]["nearest_onset_strength"] == 0.9

    assert aligned["alignment"]["beat_count"] == 10
    assert aligned["windows"]
    assert all(window["object_count"] > 0 for window in aligned["windows"])
