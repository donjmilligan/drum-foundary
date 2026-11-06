#!/usr/bin/env python3
# Drum Foundry. Copyright (c) 2026 Don Milligan.
# SPDX-License-Identifier: AGPL-3.0-only
# Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.
"""Render presets to disk and compare them with real one-shots from a sample library.

Dev tool, not shipped. Standard library only. The heavy lifting is DrumFoundryTools:
`measure-presets` renders each preset and measures it, and `measure` runs the same
analysis on the library, so both sides are measured by the same code.

The library is third-party material: it is only measured and linked to by path from the
local report. Never copy it into the repository or the plug-in.

Each preset gets:
  - defects: faults in the render itself (silent, flat-topped, DC, a click at the end).
  - fit: the share of its measurements inside the middle 90% of comparable samples
    (same instrument, same genre where the library has enough of it).
  - its nearest samples, to listen against.

A verdict is a screen, not a judgement of taste: FAIL means something is measurably wrong,
PASS means it isn't out of family, and only listening can say it sounds good.

Output, in <out>/<label>/: renders/*.wav, presets.csv, results.json, report.md (to read)
and report.html (to listen: each render next to its nearest samples).
"""

import argparse
import array
import csv
import html
import json
import math
import os
import re
import subprocess
import sys
import wave
from pathlib import Path
from statistics import median
from urllib.parse import quote

REPO = Path (__file__).resolve().parent.parent
DEFAULT_TOOL = REPO / "build-presets/DrumFoundryTools_artefacts/Release/DrumFoundryTools"
DEFAULT_LIBRARY = Path.home() / "Music/Ableton/User Library/Samples"
DEFAULT_OUT = Path.home() / "Music/Drum Foundry/Preset Auditions"

AUDIO_EXTENSIONS = {".wav", ".aif", ".aiff", ".flac"}
MIN_COHORT = 12          # fewer samples than this in a genre: compare with the whole instrument
PASS_FIT, FAIL_FIT = 0.75, 0.5
NEAREST = 3

# Measurements compared, from the measure CSV: (column, label, compare on a log scale).
METRICS = [
    ("f0Fft",         "body pitch Hz",        True),
    ("sweepSt",       "pitch sweep st",       False),
    ("bodyT20",       "body -20 dB ms",       True),
    ("wireT20",       "2-8 kHz -20 dB ms",    True),
    ("allT20",        "overall -20 dB ms",    True),
    ("allT40",        "overall -40 dB ms",    True),
    ("e<120",         "energy <120 Hz %",     False),
    ("e120-400",      "energy 120-400 Hz %",  False),
    ("e400-2k",       "energy 400-2k %",      False),
    ("e2k-8k",        "energy 2-8k %",        False),
    ("e>8k",          "energy >8k %",         False),
    ("centroidHz",    "centroid Hz",          True),
    ("flatness1k8k",  "noisiness 1-8k",       False),
    ("lowHigh0-10",   "low/high 0-10 ms dB",  False),
    ("lowHigh30-60",  "low/high 30-60 ms dB", False),
    ("crest",         "crest dB",             False),
]

# What a file in the library is: its file name decides when it says, else its folders.
NOT_ONE_SHOT = re.compile (r"loop|bpm|\bfill|break|\broll|groove|\bkit\b|sequence|pattern|stem", re.I)
INSTRUMENT_WORDS = [
    ("kick",  re.compile (r"kick|(^|[^a-z])bd([^a-z]|$)", re.I)),
    ("snare", re.compile (r"snare|(^|[^a-z])sd([^a-z]|$)", re.I)),
    ("rim",   re.compile (r"(^|[^a-z])rim|side.?stick", re.I)),
]
OTHER_DRUM = re.compile (r"clap|(^|[^a-z])(hat|hh|hihat|tom|cymbal|crash|ride|perc|shaker|snap|fx|vox|bass)([^a-z]|$)", re.I)

GENRE_WORDS = {
    "dnb":      re.compile (r"dnb|d_b|drum.?(and|n|&).?bass|neuro|jungle|molecular", re.I),
    "fg":       re.compile (r"future.?garage|burial|dusk|chillstep|(^|[^a-z])fg([^a-z]|$)", re.I),
    "808":      re.compile (r"808|trap", re.I),
    "909":      re.compile (r"909", re.I),
    "hiphop":   re.compile (r"hip.?hop|(^|[^a-z])bang([^a-z]|$)|prestige|lo.?fi", re.I),
    "club":     re.compile (r"techno|house|edm|dubstep", re.I),
    "hard":     re.compile (r"hardstyle|hardcore|gabber|(^|[^a-z])hard([^a-z]|$)", re.I),
    "acoustic": re.compile (r"acoustic|(^|[^a-z])live([^a-z]|$)|ludwig|(^|[^a-z])real([^a-z]|$)|ghost", re.I),
}

# Which samples a preset should sit among: the first pattern matching its name wins.
PRESET_COHORTS = [
    (r"FG .*(Rim|Knock|Wood)$|Rimshot|FG Rim Ring|FG Knock Room", "rim",   "fg"),
    (r"^FG |Future Garage",                                        "snare", "fg"),
    (r"^DnB .*Kick",                                               "kick",  "dnb"),
    (r"^DnB ",                                                     "snare", "dnb"),
    (r"808 Kick|Trap 808|Sub Kick",                                "kick",  "808"),
    (r"909 Kick",                                                  "kick",  "909"),
    (r"Hardstyle|Gabber",                                          "kick",  "hard"),
    (r"Acoustic Kick|Live Kick",                                   "kick",  "acoustic"),
    (r"Techno|House|Psy|Punch",                                    "kick",  "club"),
    (r"kick",                                                      "kick",  None),
    (r"808 Snare|Trap Crack",                                      "snare", "808"),
    (r"909 Snare",                                                 "snare", "909"),
    (r"Lo-Fi",                                                     "snare", "hiphop"),
    (r"Acoustic|Tight Studio|Piccolo|Ghost|Backbeat|Deep Wood|Brush", "snare", "acoustic"),
    (r".",                                                         "snare", None),
]


def classify_sample (path, library):
    """(instrument, genres) for a file in the library, or None if it isn't a usable one-shot."""
    relative = str (path.relative_to (library))
    if NOT_ONE_SHOT.search (path.name) or NOT_ONE_SHOT.search (path.parent.name):
        return None

    def words_in (text):
        return [name for name, pattern in INSTRUMENT_WORDS if pattern.search (text)]

    in_name = words_in (path.stem)
    if len (in_name) > 1:
        return None
    if in_name:
        instrument = in_name[0]
    else:
        if OTHER_DRUM.search (path.stem):
            return None
        in_folder = words_in (path.parent.name)
        if len (in_folder) != 1:
            return None
        instrument = in_folder[0]
    return instrument, [g for g, pattern in GENRE_WORDS.items() if pattern.search (relative)]


def to_float (text):
    try:
        v = float (text)
    except (TypeError, ValueError):
        return None
    return v if math.isfinite (v) else None


def read_measurements (csv_path):
    rows = []
    with open (csv_path, newline="", encoding="utf-8", errors="replace") as f:
        for row in csv.DictReader (f):
            values = {column: to_float (row.get (column)) for column, _, _ in METRICS}
            peak, rms = to_float (row.get ("peakDb")), to_float (row.get ("rms200Db"))
            values["crest"] = peak - rms if peak is not None and rms is not None else None
            rows.append ({"name": row["name"], "path": row.get ("path", ""), "peak": peak, "rms": rms,
                          "group": row.get ("group", ""), "values": values})
    return rows


def run_tool (arguments, log_path):
    with open (log_path, "w", encoding="utf-8") as log:
        code = subprocess.run (arguments, stdout=log, stderr=subprocess.STDOUT).returncode
    if code != 0:
        sys.exit ("%s failed (exit %d), see %s" % (Path (arguments[1]).name, code, log_path))


def load_references (tool, library, cache_dir, refresh):
    """Every kick, snare and rim in the library, measured once and cached (it takes minutes)."""
    cache = cache_dir / "references.csv"
    cache_dir.mkdir (parents=True, exist_ok=True)
    if refresh or not cache.exists():
        paths = [p for p in library.rglob ("*")
                 if p.suffix.lower() in AUDIO_EXTENSIONS and p.is_file() and classify_sample (p, library)]
        if not paths:
            sys.exit ("no kicks, snares or rims found under %s" % library)
        listing = cache_dir / "references.txt"
        listing.write_text ("\n".join (map (str, paths)) + "\n", encoding="utf-8")
        print ("measuring %d samples from %s ..." % (len (paths), library), flush=True)
        run_tool ([str (tool), "measure", "--list", str (listing), "--csv", str (cache)], cache_dir / "references.log")

    references = []
    for row in read_measurements (cache):
        path = Path (row["path"])
        try:
            kind = classify_sample (path, library)
        except ValueError:
            continue
        # Samples at the wrong end of the level range are fades or stems, not usable one-shots.
        if kind and row["peak"] is not None and row["peak"] > -24.0:
            row["instrument"], row["genres"] = kind
            references.append (row)
    return references


def preset_cohort (name):
    for pattern, instrument, genre in PRESET_COHORTS:
        if re.search (pattern, name, re.I):
            return instrument, genre
    return "snare", None


def percentile (sorted_values, p):
    if not sorted_values:
        return None
    position = (len (sorted_values) - 1) * p
    low = int (position)
    high = min (low + 1, len (sorted_values) - 1)
    return sorted_values[low] + (sorted_values[high] - sorted_values[low]) * (position - low)


def scaled (value, is_log):
    if value is None:
        return None
    return math.log (max (value, 1e-3)) if is_log else value


class Cohort:
    """Per-metric spread of a set of samples."""

    def __init__ (self, samples):
        self.samples = samples
        self.spread = {}
        for column, _, is_log in METRICS:
            values = sorted (v for v in (scaled (s["values"].get (column), is_log) for s in samples) if v is not None)
            if len (values) < 5:
                continue
            mid = median (values)
            mad = median (abs (v - mid) for v in values) * 1.4826
            self.spread[column] = {"p5": percentile (values, 0.05), "p95": percentile (values, 0.95), "mid": mid,
                                   "scale": max (mad, 1e-6), "count": len (values)}

    def compare (self, values):
        """The fit, and the measurements outside the middle 90%, worst first."""
        inside, outside = 0, []
        for column, label, is_log in METRICS:
            spread = self.spread.get (column)
            value = scaled (values.get (column), is_log)
            if spread is None or value is None:
                continue
            if spread["p5"] <= value <= spread["p95"]:
                inside += 1
                continue
            z = (value - spread["mid"]) / spread["scale"]
            unscale = (lambda v: math.exp (v)) if is_log else (lambda v: v)
            outside.append ({"metric": label, "value": round (values[column], 2), "z": round (z, 1),
                             "low": round (unscale (spread["p5"]), 2), "high": round (unscale (spread["p95"]), 2)})
        total = inside + len (outside)
        outside.sort (key=lambda o: -abs (o["z"]))
        return (inside / total if total else 0.0), outside

    def nearest (self, values, count):
        scored = []
        for sample in self.samples:
            total, used = 0.0, 0
            for column, _, is_log in METRICS:
                spread = self.spread.get (column)
                a, b = scaled (values.get (column), is_log), scaled (sample["values"].get (column), is_log)
                if spread is None or a is None or b is None:
                    continue
                total += min (((a - b) / spread["scale"]) ** 2, 25.0)  # one wild metric can't decide it
                used += 1
            if used >= len (METRICS) // 2:
                scored.append ((math.sqrt (total / used), sample))
        scored.sort (key=lambda item: item[0])
        return [{"distance": round (d, 2), "path": s["path"], "name": s["name"]} for d, s in scored[:count]]


def read_wav (path):
    """(channels as float lists, sample rate). DrumFoundryTools writes 24-bit PCM."""
    with wave.open (str (path), "rb") as w:
        channels, width, rate, frames = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes (frames)
    if width == 3:
        samples = [int.from_bytes (raw[i:i + 3], "little", signed=True) / 8388608.0 for i in range (0, len (raw), 3)]
    elif width == 2:
        samples = [v / 32768.0 for v in array.array ("h", raw)]
    else:
        raise ValueError ("unexpected %d-byte samples in %s" % (width, path))
    return [samples[c::channels] for c in range (channels)], rate


def find_defects (path):
    """Faults in the render itself, whatever it's meant to sound like."""
    channels, rate = read_wav (path)
    defects = []
    peak = max ((max (map (abs, c), default=0.0) for c in channels), default=0.0)
    if peak < 0.01:
        return ["silent (peak %.1f dBFS)" % (20 * math.log10 (max (peak, 1e-9)))]
    if peak > 1.0:
        defects.append ("over full scale (peak %+.1f dBFS)" % (20 * math.log10 (peak)))
    for c, data in enumerate (channels):
        # Consecutive samples held at the very top: a flat top, not a peak passing through.
        run = longest = 0
        ceiling = peak * 0.9995
        for v in data:
            run = run + 1 if abs (v) >= ceiling else 0
            longest = max (longest, run)
        if longest >= max (8, int (rate * 0.0004)) and peak > 0.97:
            defects.append ("flat-topped for %.1f ms" % (1000.0 * longest / rate))
            break
    window = int (rate * 0.2)
    dc = max (abs (sum (c[:window]) / max (1, len (c[:window]))) for c in channels)
    if dc > 0.02:
        defects.append ("DC offset %.3f in the first 200 ms" % dc)
    tail = max (abs (c[-1]) for c in channels if c)
    if tail > 0.003:
        defects.append ("cut off: the last sample is at %.1f dBFS (a click)" % (20 * math.log10 (tail)))
    return defects


def wav_name (preset):
    """The file measure-presets writes (juce::File::createLegalFileName, spaces to underscores)."""
    return re.sub (r"[\"#@,;:<>*^|?\\/]", "", preset)[:128].replace (" ", "_") + ".wav"


def evaluate (preset, renders, references):
    instrument, genre = preset_cohort (preset["name"])
    of_instrument = [r for r in references if r["instrument"] == instrument]
    in_genre = [r for r in of_instrument if genre in r["genres"]] if genre else []
    samples, used_genre = (in_genre, genre) if len (in_genre) >= MIN_COHORT else (of_instrument, None)

    render = renders / wav_name (preset["name"])
    defects = find_defects (render) if render.exists() else ["no render on disk"]
    if not samples:
        return {"name": preset["name"], "verdict": "NO REFERENCES", "fit": None, "instrument": instrument, "wanted_genre": genre,
                "genre": None, "cohort_size": 0, "defects": defects, "outside": [], "nearest": [], "render": str (render),
                "peak": preset["peak"], "rms": preset["rms"]}

    cohort = Cohort (samples)
    fit, outside = cohort.compare (preset["values"])
    verdict = "FAIL" if defects or fit < FAIL_FIT else "PASS" if fit >= PASS_FIT else "REVIEW"
    return {"name": preset["name"], "verdict": verdict, "fit": round (fit, 2), "instrument": instrument, "wanted_genre": genre,
            "genre": used_genre, "cohort_size": len (samples), "defects": defects, "outside": outside,
            "nearest": cohort.nearest (preset["values"], NEAREST), "render": str (render),
            "peak": preset["peak"], "rms": preset["rms"]}


def baseline_results (argument, out):
    if not argument:
        return {}
    path = Path (argument)
    if not path.suffix:
        path = (out / argument / "results.json") if not path.is_dir() else path / "results.json"
    if not path.exists():
        sys.exit ("baseline not found: %s" % path)
    return {r["name"]: r for r in json.loads (path.read_text (encoding="utf-8"))}


def cohort_text (r):
    if r["genre"]:
        return "%s %s (%d)" % (r["genre"], r["instrument"], r["cohort_size"])
    if r["wanted_genre"]:
        return "all %ss (%d; too few %s)" % (r["instrument"], r["cohort_size"], r["wanted_genre"])
    return "all %ss (%d)" % (r["instrument"], r["cohort_size"])


def change_text (r, base):
    if r["name"] not in base or r["fit"] is None or base[r["name"]]["fit"] is None:
        return ""
    return "%s %.0f%% -> " % (base[r["name"]]["verdict"], 100 * base[r["name"]]["fit"])


def write_markdown (results, base, label, path):
    counts = {v: sum (1 for r in results if r["verdict"] == v) for v in ("PASS", "REVIEW", "FAIL", "NO REFERENCES")}
    lines = ["# Preset comparison: %s" % label, "",
             "%d presets: %d pass, %d review, %d fail, %d with no references." % (len (results), counts["PASS"], counts["REVIEW"],
                                                                                  counts["FAIL"], counts["NO REFERENCES"]),
             "", "Fit is the share of measurements inside the middle 90%% of the comparison set. PASS >= %d%%, FAIL < %d%% or any defect. "
             "This is a screen: listen before calling anything good." % (100 * PASS_FIT, 100 * FAIL_FIT), "",
             "| Preset | Verdict | Fit | Compared with | Peak / RMS dB | Furthest out |", "|---|---|---|---|---|---|"]
    order = {"FAIL": 0, "REVIEW": 1, "NO REFERENCES": 2, "PASS": 3}
    for r in sorted (results, key=lambda r: (order[r["verdict"]], r["fit"] or 0)):
        furthest = "; ".join ("%s %g (usual %g to %g)" % (o["metric"], o["value"], o["low"], o["high"]) for o in r["outside"][:3])
        fit = "%s%.0f%%" % (change_text (r, base), 100 * r["fit"]) if r["fit"] is not None else "-"
        problem = ("**" + "; ".join (r["defects"]) + "** " if r["defects"] else "") + furthest
        lines.append ("| %s | %s | %s | %s | %s / %s | %s |" % (r["name"], r["verdict"], fit, cohort_text (r), r["peak"], r["rms"], problem))
    lines += ["", "## Nearest samples", ""]
    for r in sorted (results, key=lambda r: r["name"]):
        lines.append ("- **%s**: %s" % (r["name"], ", ".join ("%s (%.1f)" % (n["name"], n["distance"]) for n in r["nearest"]) or "none"))
    path.write_text ("\n".join (lines) + "\n", encoding="utf-8")


def write_html (results, base, label, path):
    def player (file_path, caption):
        return ("<div class=p><span>%s</span><audio controls preload=none src=\"file://%s\"></audio></div>"
                % (html.escape (caption), quote (file_path)))

    order = {"FAIL": 0, "REVIEW": 1, "NO REFERENCES": 2, "PASS": 3}
    cards = []
    for r in sorted (results, key=lambda r: (order[r["verdict"]], r["fit"] or 0)):
        fit = "fit %s%.0f%%" % (change_text (r, base), 100 * r["fit"]) if r["fit"] is not None else "no fit"
        issues = "".join ("<li class=d>%s</li>" % html.escape (d) for d in r["defects"])
        issues += "".join ("<li>%s: %g (usual %g to %g)</li>" % (html.escape (o["metric"]), o["value"], o["low"], o["high"])
                           for o in r["outside"][:6])
        players = player (r["render"], "This preset") + "".join (player (n["path"], n["name"]) for n in r["nearest"])
        cards.append ("<section><h2>%s <b class=%s>%s</b></h2><p>%s &middot; compared with %s</p>%s<ul>%s</ul></section>"
                      % (html.escape (r["name"]), r["verdict"].split()[0], r["verdict"], html.escape (fit),
                         html.escape (cohort_text (r)), players, issues))
    path.write_text ("""<!doctype html><meta charset=utf-8><title>Preset Comparison</title>
<style>:root{color-scheme:light dark}body{font:15px/1.45 system-ui;max-width:880px;margin:24px auto;padding:0 16px}
section{border-top:1px solid #8884;padding:12px 0}h2{font-size:17px;margin:0}p{margin:4px 0 8px;opacity:.75}
b{font-size:12px;padding:2px 6px;border-radius:4px;color:#fff;margin-left:6px}.FAIL{background:#b3362b}.REVIEW{background:#a06a00}
.PASS{background:#2b7a3d}.NO{background:#666}.p{display:flex;gap:10px;align-items:center;margin:3px 0}
.p span{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}audio{height:30px;width:280px;flex:none}
ul{margin:8px 0 0;padding-left:18px;font-size:13px}.d{color:#b3362b;font-weight:600}</style>
<h1>Preset comparison: %s</h1>%s""" % (html.escape (label), "\n".join (cards)), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser (description="Render presets and compare them with one-shots in a sample library.")
    parser.add_argument ("--label", required=True, help="name of this run, e.g. before or after")
    parser.add_argument ("--kind", default="all", choices=["all", "kick", "snare"])
    parser.add_argument ("--preset", action="append", default=[], help="only presets whose name contains this (repeatable)")
    parser.add_argument ("--source", default="factory", choices=["factory", "user"], help="factory (in the code) or user (saved files)")
    parser.add_argument ("--overrides", help="measure-presets overrides file: try changes with no rebuild")
    parser.add_argument ("--note", type=int, default=60, help="MIDI note to play (60 = C3, where Tune is the pitch)")
    parser.add_argument ("--velocity", type=float, default=1.0, help="velocity to play, 0 to 1")
    parser.add_argument ("--baseline", help="label or results.json of an earlier run, to show what changed")
    parser.add_argument ("--library", type=Path, default=DEFAULT_LIBRARY, help="sample folder to compare with")
    parser.add_argument ("--out", type=Path, default=DEFAULT_OUT, help="where runs are saved")
    parser.add_argument ("--tool", type=Path, default=DEFAULT_TOOL, help="the DrumFoundryTools binary")
    parser.add_argument ("--refresh-references", action="store_true", help="measure the library again")
    args = parser.parse_args()

    if not args.tool.exists():
        sys.exit ("%s not found. Build it:\n  cmake -S . -B build-presets -G Ninja -DDRUMFOUNDRY_COPY_PLUGINS=OFF\n"
                  "  cmake --build build-presets --target DrumFoundryTools" % args.tool)
    if not args.library.is_dir():
        sys.exit ("sample library not found: %s" % args.library)

    base = baseline_results (args.baseline, args.out)
    run_dir = args.out / args.label
    renders = run_dir / "renders"
    renders.mkdir (parents=True, exist_ok=True)
    for stale in renders.glob ("*.wav"):
        stale.unlink()  # only ever this script's own renders of this label

    references = load_references (args.tool, args.library, args.out / "_references", args.refresh_references)

    command = [str (args.tool), "measure-presets", args.kind, "--source", args.source,
               "--csv", str (run_dir / "presets.csv"), "--wav", str (renders),
               "--note", str (args.note), "--velocity", str (args.velocity)]
    for name in args.preset:
        command += ["--preset", name]
    if args.overrides:
        command += ["--overrides", str (Path (args.overrides).resolve())]
    run_tool (command, run_dir / "measure-presets.log")

    results = [evaluate (p, renders, references) for p in read_measurements (run_dir / "presets.csv")]
    if not results:
        sys.exit ("no presets matched")
    (run_dir / "results.json").write_text (json.dumps (results, indent=1), encoding="utf-8")
    write_markdown (results, base, args.label, run_dir / "report.md")
    write_html (results, base, args.label, run_dir / "report.html")

    counts = {v: sum (1 for r in results if r["verdict"] == v) for v in ("PASS", "REVIEW", "FAIL")}
    print ("%d presets against %d samples: %d pass, %d review, %d fail" % (len (results), len (references), counts["PASS"],
                                                                         counts["REVIEW"], counts["FAIL"]))
    print ("report: %s\nlisten: %s" % (run_dir / "report.md", run_dir / "report.html"))


if __name__ == "__main__":
    main()
