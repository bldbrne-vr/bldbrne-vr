#!/usr/bin/env python3
"""Summarize existing VR_DRAW_CTL `log N` output without changing the renderer.

This reports candidate UI draws, not proven widget ownership. Keep the original log
and screenshots: shared shader/texture identities cannot safely classify a widget.
"""
import argparse
import json
from pathlib import Path
import re

DRAW = re.compile(r"\bVRDRAWX\s+(.*)")
FIELD = re.compile(r"(\w+)=(\[[^\]]*\]|\S+)")
COMPOSITE_VS = 0x34F1F46B160


def inspect(text, composite_vs=COMPOSITE_VS):
    frames = {}
    malformed = 0
    for line in text.splitlines():
        match = DRAW.search(line)
        if not match:
            continue
        fields = dict(FIELD.findall(match[1]))
        try:
            frame, index, vs, cb = (int(fields[k], 0) for k in ("f", "i", "vs", "cb"))
            # Stable guest identity takes precedence over a compiled pipeline variant.
            if "guest_vs" in fields:
                vs = int(fields["guest_vs"], 0)
            int(fields["fs"], 0)
        except (ValueError, KeyError):
            malformed += 1
            continue
        frames.setdefault(frame, []).append((index, vs, cb, fields))
    result = []
    for frame, draws in frames.items():
        composites = [d for d in draws if d[1] == composite_vs]
        # Multiple composites or duplicate indices can indicate concatenated runs or a
        # different rendering path. Do not silently manufacture a single UI interval.
        if len(composites) != 1 or len({d[0] for d in draws}) != len(draws):
            result.append({"frame": frame, "status": "ambiguous_or_missing_composite",
                           "composites": len(composites), "recorded_draws": len(draws)})
            continue
        first, _, target, _ = composites[0]
        after = [d for d in draws if d[0] > first]
        result.append({
            "frame": frame,
            "status": "candidate_ui_interval",
            "composite_index": first,
            "composite_target": hex(target),
            "composite_identity": "guest" if "guest_vs" in composites[0][3] else "legacy_variant",
            "ui_target_draws": [d[3] for d in after if d[2] == target],
            "other_target_draws": [d[3] for d in after if d[2] != target],
        })
    return {"scope": "Draw metadata only; no widget classifier or performance verdict.",
            "malformed_lines": malformed, "frames": result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--composite-vs", type=lambda s: int(s, 0), default=COMPOSITE_VS)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = inspect(args.log.read_text(encoding="utf-8", errors="replace"), args.composite_vs)
    # Never overwrite an earlier capture's evidence.
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    count = sum(f["status"] == "candidate_ui_interval" for f in report["frames"])
    ambiguous = len(report["frames"]) - count
    print(f"{count} candidate UI frames; {ambiguous} ambiguous frames; "
          f"{report['malformed_lines']} malformed lines. {args.output}")
    return 0 if count and not ambiguous and not report["malformed_lines"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
