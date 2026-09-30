from __future__ import annotations

from gd_ai_editor.gameplay import (
    PlannerRequest,
    extract_reference_chunks,
    retrieve_reference_chunks,
)


def _object(
    beat: float,
    category: str,
    mode: str,
    *,
    y: float = 105.0,
    object_id: int = 1,
    strength: float = 0.8,
) -> dict:
    return {
        "beat": beat,
        "category": category,
        "object_id": object_id,
        "y": y,
        "nearest_onset_strength": strength,
        "state_before": {
            "mode": mode,
            "gravity": "normal",
            "mini": False,
            "speed": "fast",
        },
    }


def test_reference_chunks_are_mode_aware_and_retrievable() -> None:
    hard_demon = {
        "level": {
            "name": "Human Hard Demon",
            "difficulty_label": "Hard Demon",
        },
        "objects": [
            _object(0.0, "solid", "cube"),
            _object(0.5, "hazard", "cube", object_id=8),
            _object(1.0, "solid", "cube", y=135.0),
            _object(1.5, "orb", "cube", y=195.0, object_id=36),
            _object(2.0, "solid", "cube", y=165.0),
            _object(2.5, "hazard", "cube", y=165.0, object_id=8),
            _object(4.0, "solid", "wave"),
            _object(4.5, "hazard", "wave", y=255.0, object_id=8),
            _object(5.0, "solid", "wave", y=285.0),
            _object(5.5, "hazard", "wave", y=315.0, object_id=8),
            _object(6.0, "solid", "wave", y=345.0),
        ],
    }
    unknown = {
        "level": {"name": "Old Reference", "difficulty_label": "Unknown"},
        "objects": [
            _object(0.0, "solid", "cube", strength=0.2),
            _object(0.5, "hazard", "cube", object_id=8, strength=0.2),
            _object(1.0, "solid", "cube", strength=0.2),
            _object(1.5, "hazard", "cube", object_id=8, strength=0.2),
        ],
    }

    chunks = extract_reference_chunks(
        [hard_demon, unknown],
        chunk_beats=4.0,
        stride_beats=2.0,
    )

    assert any(chunk["mode"] == "cube" for chunk in chunks)
    assert any(chunk["mode"] == "wave" for chunk in chunks)
    assert all(chunk["mode"] != "ship" for chunk in chunks)

    request = PlannerRequest(
        mode="cube",
        difficulty="Hard Demon",
        beats=4.0,
        energy=0.8,
        onset=0.8,
        entry_speed="fast",
    )
    references = retrieve_reference_chunks(chunks, request, limit=2)

    assert references
    assert references[0]["level"] == "Human Hard Demon"
    assert all(reference["mode"] == "cube" for reference in references)
    assert references[0]["events"]
    assert references[0]["geometry_cells"]
    assert "entry_y_step" in references[0]
    assert "exit_y_step" in references[0]
    assert any(
        cell["category"] == "solid"
        for cell in references[0]["geometry_cells"]
    )


def test_reference_extraction_rejects_invalid_window_sizes() -> None:
    try:
        extract_reference_chunks([], chunk_beats=0)
    except ValueError as error:
        assert "positive" in str(error)
    else:
        raise AssertionError("Expected invalid chunk size to raise ValueError")
