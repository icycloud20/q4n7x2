from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


@dataclass(slots=True)
class BeatFeature:
    index: int
    time: float
    interval: float | None
    onset_strength: float
    energy: float


@dataclass(slots=True)
class OnsetFeature:
    time: float
    strength: float


@dataclass(slots=True)
class EnergyFrame:
    time: float
    energy: float


@dataclass(slots=True)
class SectionFeature:
    index: int
    start: float
    end: float
    intensity: float
    onset_density: float
    label: str


@dataclass(slots=True)
class AudioAnalysis:
    source: str
    duration: float
    sample_rate: int
    hop_length: int
    bpm: float
    beats: list[BeatFeature]
    onsets: list[OnsetFeature]
    energy: list[EnergyFrame]
    sections: list[SectionFeature]

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @property
    def source_name(self) -> str:
        return Path(self.source).name
