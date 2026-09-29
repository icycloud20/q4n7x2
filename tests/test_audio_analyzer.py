from __future__ import annotations

import numpy as np
import soundfile as sf

from gd_ai_editor.audio import analyze_audio


def _write_click_track(path, bpm: float = 120.0, duration: float = 12.0) -> None:
    sample_rate = 22050
    waveform = np.zeros(int(duration * sample_rate), dtype=np.float32)

    seconds_per_beat = 60.0 / bpm
    beat_times = np.arange(0.5, duration - 0.5, seconds_per_beat)

    click_length = int(0.02 * sample_rate)
    window = np.hanning(click_length).astype(np.float32)

    for beat_time in beat_times:
        start = int(beat_time * sample_rate)
        end = min(start + click_length, waveform.size)
        waveform[start:end] += window[: end - start]

    sf.write(path, waveform, sample_rate)


def test_analyze_audio_finds_timing_features(tmp_path) -> None:
    audio_path = tmp_path / "clicks.wav"
    _write_click_track(audio_path)

    analysis = analyze_audio(audio_path)

    assert 11.9 <= analysis.duration <= 12.1
    assert analysis.sample_rate == 22050
    assert len(analysis.beats) >= 10
    assert len(analysis.onsets) >= 10
    assert len(analysis.energy) >= 100
    assert len(analysis.sections) >= 1

    # Beat trackers may report an integer multiple/divisor of the musical tempo.
    # All of these represent the same grid for this synthetic click track.
    valid_tempos = (60.0, 120.0, 240.0)
    assert min(abs(analysis.bpm - tempo) for tempo in valid_tempos) < 5.0

    payload = analysis.to_dict()
    assert payload["beats"][0]["time"] >= 0.0
    assert payload["sections"][0]["start"] == 0.0
