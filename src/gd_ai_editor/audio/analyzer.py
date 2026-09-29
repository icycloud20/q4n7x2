from __future__ import annotations

from itertools import pairwise
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf
from scipy.ndimage import gaussian_filter1d
from scipy.signal import find_peaks

from gd_ai_editor.schema import (
    AudioAnalysis,
    BeatFeature,
    EnergyFrame,
    OnsetFeature,
    SectionFeature,
)


def _normalize(values: np.ndarray) -> np.ndarray:
    values = np.asarray(values, dtype=np.float64)
    if values.size == 0:
        return values

    minimum = float(np.min(values))
    maximum = float(np.max(values))
    span = maximum - minimum
    if span <= 1e-12:
        return np.zeros_like(values)

    return (values - minimum) / span


def _nearest_value(values: np.ndarray, index: int) -> float:
    if values.size == 0:
        return 0.0

    index = max(0, min(index, values.size - 1))
    return float(values[index])


def _resample_feature(
    source_times: np.ndarray,
    values: np.ndarray,
    target_times: np.ndarray,
) -> np.ndarray:
    if source_times.size == 0 or values.size == 0:
        return np.zeros_like(target_times, dtype=np.float64)

    return np.interp(target_times, source_times, values)


def _estimate_sections(
    duration: float,
    frame_times: np.ndarray,
    energy: np.ndarray,
    onset_strength: np.ndarray,
    spectral_centroid: np.ndarray,
) -> list[SectionFeature]:
    if duration <= 0:
        return []

    step = 0.10
    timeline = np.arange(0.0, duration + step, step)

    energy_curve = _resample_feature(frame_times, _normalize(energy), timeline)
    onset_curve = _resample_feature(frame_times, _normalize(onset_strength), timeline)
    centroid_curve = _resample_feature(
        frame_times,
        _normalize(spectral_centroid),
        timeline,
    )

    smoothed_energy = gaussian_filter1d(energy_curve, sigma=4)
    smoothed_onset = gaussian_filter1d(onset_curve, sigma=4)
    smoothed_centroid = gaussian_filter1d(centroid_curve, sigma=4)

    lag_frames = max(1, round(1.0 / step))

    def lagged_change(curve: np.ndarray) -> np.ndarray:
        change = np.zeros_like(curve)
        if curve.size > lag_frames:
            change[lag_frames:] = np.abs(curve[lag_frames:] - curve[:-lag_frames])
        return change

    novelty = (
        0.50 * lagged_change(smoothed_energy)
        + 0.30 * lagged_change(smoothed_onset)
        + 0.20 * lagged_change(smoothed_centroid)
    )
    novelty = gaussian_filter1d(novelty, sigma=2)

    minimum_section_seconds = 6.0
    peak_distance = max(1, round(minimum_section_seconds / step))
    prominence = max(0.02, float(np.percentile(novelty, 75)) * 0.55)

    peaks, properties = find_peaks(
        novelty,
        distance=peak_distance,
        prominence=prominence,
    )

    maximum_internal_boundaries = max(1, min(15, int(duration // 10)))
    if peaks.size > maximum_internal_boundaries:
        prominences = properties.get("prominences", novelty[peaks])
        strongest = np.argsort(prominences)[-maximum_internal_boundaries:]
        peaks = np.sort(peaks[strongest])

    boundaries = [0.0]
    boundaries.extend(float(timeline[index]) for index in peaks)
    boundaries.append(float(duration))

    cleaned_boundaries: list[float] = []
    for boundary in boundaries:
        boundary = min(max(boundary, 0.0), duration)
        if not cleaned_boundaries or boundary - cleaned_boundaries[-1] >= minimum_section_seconds:
            cleaned_boundaries.append(boundary)
        elif boundary == duration:
            cleaned_boundaries[-1] = boundary

    if cleaned_boundaries[-1] != duration:
        cleaned_boundaries.append(duration)

    sections: list[SectionFeature] = []
    for section_index, (start, end) in enumerate(pairwise(cleaned_boundaries)):
        mask = (timeline >= start) & (timeline < end)
        if not np.any(mask):
            mean_energy = 0.0
            mean_onset = 0.0
        else:
            mean_energy = float(np.mean(smoothed_energy[mask]))
            mean_onset = float(np.mean(smoothed_onset[mask]))

        intensity = float(np.clip(0.70 * mean_energy + 0.30 * mean_onset, 0.0, 1.0))
        if intensity < 0.33:
            label = "low"
        elif intensity < 0.66:
            label = "medium"
        else:
            label = "high"

        sections.append(
            SectionFeature(
                index=section_index,
                start=round(start, 6),
                end=round(end, 6),
                intensity=round(intensity, 6),
                onset_density=round(mean_onset, 6),
                label=label,
            )
        )

    return sections


def write_beat_preview(
    source_path: str | Path,
    beat_times: list[float] | np.ndarray,
    output_path: str | Path,
    *,
    beat_strengths: list[float] | np.ndarray | None = None,
    click_gain: float = 0.62,
    accent_gain: float = 0.90,
) -> Path:
    """Write a clearly audible beat-check preview over the original song.

    Regular beats use a short bright tick. Every four-beat group also gets one
    lower, louder accent on the strongest detected beat in that group. This is
    only a debugging preview; it is not an input/hold prediction.
    """
    source = Path(source_path).expanduser().resolve()
    output = Path(output_path).expanduser().resolve()

    waveform, sample_rate = librosa.load(source, sr=None, mono=True)
    if waveform.size == 0:
        raise ValueError(f"Audio file is empty: {source}")

    peak = float(np.max(np.abs(waveform)))
    if peak > 0:
        # Leave headroom for the click track while keeping the music easy to hear.
        waveform = waveform / peak * 0.64

    times = np.asarray(beat_times, dtype=np.float64)
    times = times[np.isfinite(times)]
    times = times[(times >= 0.0) & (times < waveform.size / sample_rate)]

    regular_clicks = librosa.clicks(
        times=times,
        sr=sample_rate,
        length=waveform.size,
        click_freq=2400.0,
        click_duration=0.032,
    )

    if beat_strengths is None:
        strengths = np.ones(times.size, dtype=np.float64)
    else:
        strengths = np.asarray(beat_strengths, dtype=np.float64)
        strengths = strengths[: times.size]
        if strengths.size < times.size:
            strengths = np.pad(strengths, (0, times.size - strengths.size), constant_values=1.0)

    accent_indices: list[int] = []
    for start in range(0, times.size, 4):
        end = min(start + 4, times.size)
        if end <= start:
            continue

        local = strengths[start:end]
        accent_indices.append(start + int(np.argmax(local)))

    accent_times = times[np.asarray(accent_indices, dtype=np.int64)] if accent_indices else np.array([])
    accent_clicks = librosa.clicks(
        times=accent_times,
        sr=sample_rate,
        length=waveform.size,
        click_freq=1050.0,
        click_duration=0.045,
    )

    # A tiny local duck around each beat makes the debug ticks readable even
    # through dense drops without making the entire song much quieter.
    duck = np.ones(waveform.size, dtype=np.float32)
    duck_half_width = max(1, int(sample_rate * 0.018))
    for beat_time in times:
        center = int(round(beat_time * sample_rate))
        start = max(0, center - duck_half_width)
        end = min(waveform.size, center + duck_half_width)
        if end <= start:
            continue

        window = np.hanning((end - start) * 2)[end - start :]
        duck[start:end] *= (1.0 - 0.12 * window.astype(np.float32))

    preview = np.clip(
        waveform * duck
        + regular_clicks * click_gain
        + accent_clicks * accent_gain,
        -1.0,
        1.0,
    )

    output.parent.mkdir(parents=True, exist_ok=True)
    sf.write(output, preview, sample_rate)
    return output


def analyze_audio(
    path: str | Path,
    *,
    sample_rate: int = 22050,
    hop_length: int = 512,
    energy_step_seconds: float = 0.10,
) -> AudioAnalysis:
    source = Path(path).expanduser().resolve()
    if not source.exists():
        raise FileNotFoundError(f"Audio file does not exist: {source}")

    if sample_rate <= 0:
        raise ValueError("sample_rate must be positive")
    if hop_length <= 0:
        raise ValueError("hop_length must be positive")
    if energy_step_seconds <= 0:
        raise ValueError("energy_step_seconds must be positive")

    waveform, loaded_sample_rate = librosa.load(
        source,
        sr=sample_rate,
        mono=True,
    )

    if waveform.size == 0:
        raise ValueError(f"Audio file is empty: {source}")

    duration = float(librosa.get_duration(y=waveform, sr=loaded_sample_rate))

    onset_envelope = librosa.onset.onset_strength(
        y=waveform,
        sr=loaded_sample_rate,
        hop_length=hop_length,
    )
    normalized_onset = _normalize(onset_envelope)

    tempo, beat_frames = librosa.beat.beat_track(
        onset_envelope=onset_envelope,
        sr=loaded_sample_rate,
        hop_length=hop_length,
        units="frames",
        sparse=True,
    )
    tempo_value = float(np.asarray(tempo).reshape(-1)[0]) if np.size(tempo) else 0.0

    beat_frames = np.asarray(beat_frames, dtype=np.int64)
    beat_times = librosa.frames_to_time(
        beat_frames,
        sr=loaded_sample_rate,
        hop_length=hop_length,
    )

    onset_frames = librosa.onset.onset_detect(
        onset_envelope=onset_envelope,
        sr=loaded_sample_rate,
        hop_length=hop_length,
        units="frames",
        backtrack=False,
        normalize=True,
    )
    onset_frames = np.asarray(onset_frames, dtype=np.int64)
    onset_times = librosa.frames_to_time(
        onset_frames,
        sr=loaded_sample_rate,
        hop_length=hop_length,
    )

    rms = librosa.feature.rms(
        y=waveform,
        frame_length=2048,
        hop_length=hop_length,
    )[0]
    normalized_rms = _normalize(rms)

    spectral_centroid = librosa.feature.spectral_centroid(
        y=waveform,
        sr=loaded_sample_rate,
        hop_length=hop_length,
    )[0]

    frame_count = min(
        onset_envelope.size,
        rms.size,
        spectral_centroid.size,
    )
    frame_times = librosa.frames_to_time(
        np.arange(frame_count),
        sr=loaded_sample_rate,
        hop_length=hop_length,
    )

    normalized_onset_for_sections = normalized_onset[:frame_count]
    normalized_rms_for_sections = normalized_rms[:frame_count]
    centroid_for_sections = spectral_centroid[:frame_count]

    beats: list[BeatFeature] = []
    for beat_index, (frame, time_value) in enumerate(zip(beat_frames, beat_times, strict=True)):
        previous_time = float(beat_times[beat_index - 1]) if beat_index > 0 else None
        interval = None if previous_time is None else float(time_value) - previous_time

        beats.append(
            BeatFeature(
                index=beat_index,
                time=round(float(time_value), 6),
                interval=None if interval is None else round(interval, 6),
                onset_strength=round(_nearest_value(normalized_onset, int(frame)), 6),
                energy=round(_nearest_value(normalized_rms, int(frame)), 6),
            )
        )

    onsets = [
        OnsetFeature(
            time=round(float(time_value), 6),
            strength=round(_nearest_value(normalized_onset, int(frame)), 6),
        )
        for frame, time_value in zip(onset_frames, onset_times, strict=True)
    ]

    energy_times = np.arange(0.0, duration + energy_step_seconds, energy_step_seconds)
    rms_times = librosa.frames_to_time(
        np.arange(normalized_rms.size),
        sr=loaded_sample_rate,
        hop_length=hop_length,
    )
    sampled_energy = _resample_feature(rms_times, normalized_rms, energy_times)
    energy = [
        EnergyFrame(
            time=round(float(time_value), 6),
            energy=round(float(energy_value), 6),
        )
        for time_value, energy_value in zip(energy_times, sampled_energy, strict=True)
        if time_value <= duration
    ]

    sections = _estimate_sections(
        duration,
        frame_times,
        normalized_rms_for_sections,
        normalized_onset_for_sections,
        centroid_for_sections,
    )

    return AudioAnalysis(
        source=str(source),
        duration=round(duration, 6),
        sample_rate=int(loaded_sample_rate),
        hop_length=int(hop_length),
        bpm=round(tempo_value, 6),
        beats=beats,
        onsets=onsets,
        energy=energy,
        sections=sections,
    )
