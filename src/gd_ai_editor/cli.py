from __future__ import annotations

import argparse
import json
from pathlib import Path

from gd_ai_editor.audio import analyze_audio, write_beat_preview
from gd_ai_editor.gameplay import (
    PlannerRequest,
    align_gameplay_export,
    build_motif_profile,
    extract_reference_chunks,
    load_json,
    load_reference_library,
    request_openai_plan,
    retrieve_reference_chunks,
    write_json,
    write_reference_library,
)


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

    gameplay_parser = commands.add_parser("gameplay", help="Gameplay dataset commands")
    gameplay_commands = gameplay_parser.add_subparsers(dest="gameplay_command", required=True)

    align_parser = gameplay_commands.add_parser(
        "align",
        help="Align an exported Geometry Dash gameplay timeline to analyzed song beats",
    )
    align_parser.add_argument("gameplay_file", type=Path)
    align_parser.add_argument("analysis_file", type=Path)
    align_parser.add_argument(
        "--out",
        type=Path,
        required=True,
        help="Path to the aligned gameplay JSON file",
    )
    align_parser.add_argument(
        "--window-beats",
        type=float,
        default=8.0,
        help="Training-window width in beats (default: 8)",
    )
    align_parser.add_argument(
        "--stride-beats",
        type=float,
        default=4.0,
        help="Training-window stride in beats (default: 4)",
    )

    profile_parser = gameplay_commands.add_parser(
        "profile",
        help="Learn phrase-level gameplay priors from aligned level exports",
    )
    profile_parser.add_argument(
        "aligned_files",
        nargs="+",
        type=Path,
        help="One or more aligned gameplay JSON exports",
    )
    profile_parser.add_argument(
        "--out",
        type=Path,
        required=True,
        help="Path to the learned motif-profile JSON file",
    )

    chunks_parser = gameplay_commands.add_parser(
        "chunks",
        help="Build a compact human-reference chunk library from aligned exports",
    )
    chunks_parser.add_argument("aligned_files", nargs="+", type=Path)
    chunks_parser.add_argument("--out", type=Path, required=True)
    chunks_parser.add_argument("--chunk-beats", type=float, default=4.0)
    chunks_parser.add_argument("--stride-beats", type=float, default=2.0)

    plan_parser = gameplay_commands.add_parser(
        "plan",
        help="Plan one gameplay section with retrieved human examples + GPT-6 Luna",
    )
    plan_parser.add_argument("reference_library", type=Path)
    plan_parser.add_argument("--mode", choices=("cube", "ship", "ball", "ufo", "wave"), required=True)
    plan_parser.add_argument("--difficulty", default="Hard Demon")
    plan_parser.add_argument("--beats", type=float, default=4.0)
    plan_parser.add_argument("--energy", type=float, default=0.8)
    plan_parser.add_argument("--onset", type=float, default=0.8)
    plan_parser.add_argument("--entry-gravity", default="normal")
    plan_parser.add_argument("--entry-speed", default="normal")
    plan_parser.add_argument("--entry-mini", action="store_true")
    plan_parser.add_argument("--previous-mode", default="cube")
    plan_parser.add_argument("--references", type=int, default=5)
    plan_parser.add_argument("--model", default="gpt-6-luna")
    plan_parser.add_argument("--reasoning-effort", default="low")
    plan_parser.add_argument("--out", type=Path, required=True)
    plan_parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Write retrieved examples/request metadata without calling OpenAI",
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

    if arguments.command == "gameplay" and arguments.gameplay_command == "align":
        gameplay = load_json(arguments.gameplay_file)
        analysis = load_json(arguments.analysis_file)
        aligned = align_gameplay_export(
            gameplay,
            analysis,
            window_beats=arguments.window_beats,
            stride_beats=arguments.stride_beats,
        )
        output = write_json(arguments.out, aligned)
        print(f"Wrote {output}")
        print(f"Aligned objects: {len(aligned.get('objects', []))}")
        print(f"Training windows: {len(aligned.get('windows', []))}")
        return

    if arguments.command == "gameplay" and arguments.gameplay_command == "profile":
        levels = [load_json(path) for path in arguments.aligned_files]
        profile = build_motif_profile(levels)
        output = write_json(arguments.out, profile)
        print(f"Wrote {output}")
        print(f"Source levels: {profile['source_level_count']}")
        print(f"Cube phrases: {profile['cube_phrase_count']}")
        print(f"Difficulty distribution: {profile.get('difficulty_distribution', {})}")
        for mode, mode_profile in profile.get("mode_profiles", {}).items():
            print(
                f"{mode}: {mode_profile.get('phrase_count', 0)} phrases / "
                f"{mode_profile.get('interaction_object_count', 0)} interactions"
            )
        return

    if arguments.command == "gameplay" and arguments.gameplay_command == "chunks":
        levels = [load_json(path) for path in arguments.aligned_files]
        chunks = extract_reference_chunks(
            levels,
            chunk_beats=arguments.chunk_beats,
            stride_beats=arguments.stride_beats,
        )
        output = write_reference_library(arguments.out, chunks)
        print(f"Wrote {output}")
        print(f"Reference chunks: {len(chunks)}")
        return

    if arguments.command == "gameplay" and arguments.gameplay_command == "plan":
        request = PlannerRequest(
            mode=arguments.mode,
            difficulty=arguments.difficulty,
            beats=arguments.beats,
            energy=arguments.energy,
            onset=arguments.onset,
            entry_gravity=arguments.entry_gravity,
            entry_speed=arguments.entry_speed,
            entry_mini=arguments.entry_mini,
            previous_mode=arguments.previous_mode,
        )
        chunks = load_reference_library(arguments.reference_library)
        references = retrieve_reference_chunks(
            chunks,
            request,
            limit=max(1, min(arguments.references, 8)),
        )

        if arguments.dry_run:
            payload = {
                "request": request.__dict__,
                "reference_count": len(references),
                "references": references,
            }
        else:
            plan = request_openai_plan(
                request,
                references,
                model=arguments.model,
                reasoning_effort=arguments.reasoning_effort,
            )
            payload = {
                "planner": "openai",
                "model": arguments.model,
                "request": request.__dict__,
                "reference_ids": [reference.get("id") for reference in references],
                "plan": plan,
            }

        output = write_json(arguments.out, payload)
        print(f"Wrote {output}")
        print(f"Retrieved references: {len(references)}")
        if not arguments.dry_run:
            print(f"Planned actions: {len(payload['plan'].get('actions', []))}")
        return

    parser.error("Unknown command")


if __name__ == "__main__":
    main()
