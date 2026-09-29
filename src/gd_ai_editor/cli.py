from __future__ import annotations

import argparse
import json
from pathlib import Path

from gd_ai_editor.audio import analyze_audio, write_beat_preview


def _write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def _print_summary(analysis) -> None:
    print(f"Source: {analysis.source_name}")
    print(f"Duration: {analysis.duration:.2f}s")
    print(f"Estimated BPM: {analysis.bpm:.3f}")
    print(f"Beats: {len(analysis.beats)}")
    print(f"Onsets: {len(analysis.onsets)}")
    print(f"Sections: {len(analysis.sections)}")

    if analysis.sections:
        print("\nSections:")
        for section in analysis.sections:
            print(
                f"  {section.index:02d}  "
                f"{section.start:7.2f}s -> {section.end:7.2f}s  "
                f"intensity={section.intensity:.2f}  "
                f"{section.label}"
            )

    if analysis.beats:
        print("\nFirst beats:")
        for beat in analysis.beats[:16]:
            print(
                f"  #{beat.index:03d}  {beat.time:8.3f}s  "
                f"onset={beat.onset_strength:.2f}  energy={beat.energy:.2f}"
            )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="gd-ai",
        description="Research tooling for music-aware Geometry Dash gameplay generation.",
    )
    commands = parser.add_subparsers(dest="command", required=True)

    audio_parser = commands.add_parser("audio", help="Audio analysis commands")
    audio_commands = audio_parser.add_subparsers(dest="audio_command", required=True)

    analyze_parser = audio_commands.add_parser(
        "analyze",
        help="Analyze song timing and musical intensity",
    )
    analyze_parser.add_argument("audio_file", type=Path)
    analyze_parser.add_argument(
        "--out",
        type=Path,
        required=True,
        help="Path to the output JSON file",
    )
    analyze_parser.add_argument(
        "--summary",
        action="store_true",
        help="Print a readable summary after analysis",
    )
    analyze_parser.add_argument(
        "--beat-preview",
        type=Path,
        help="Optional WAV path containing the song with clicks on detected beats",
    )
    analyze_parser.add_argument(
        "--sample-rate",
        type=int,
        default=22050,
        help="Analysis sample rate (default: 22050)",
    )
    analyze_parser.add_argument(
        "--hop-length",
        type=int,
        default=512,
        help="Analysis hop length (default: 512)",
    )

    return parser


def main() -> None:
    parser = _build_parser()
    arguments = parser.parse_args()

    if arguments.command == "audio" and arguments.audio_command == "analyze":
        analysis = analyze_audio(
            arguments.audio_file,
            sample_rate=arguments.sample_rate,
            hop_length=arguments.hop_length,
        )
        _write_json(arguments.out, analysis.to_dict())
        print(f"Wrote {arguments.out}")

        if arguments.beat_preview:
            preview = write_beat_preview(
                arguments.audio_file,
                [beat.time for beat in analysis.beats],
                arguments.beat_preview,
                beat_strengths=[beat.onset_strength for beat in analysis.beats],
            )
            print(f"Wrote {preview}")

        if arguments.summary:
            _print_summary(analysis)
        return

    parser.error("Unknown command")


if __name__ == "__main__":
    main()
