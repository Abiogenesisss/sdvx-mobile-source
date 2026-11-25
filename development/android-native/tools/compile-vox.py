"""Compile a SOUND VOLTEX VOX chart and verified judgment tables for Android."""
import argparse
from bisect import bisect_right
from hashlib import sha256
import json
from math import lcm
from pathlib import Path
import struct
from xml.etree import ElementTree


TICKS_PER_QUARTER = 48


class VoxClock:
    def __init__(self, beats, tempos, resolution=TICKS_PER_QUARTER):
        self.meters = []
        self.meter_bars = []
        rows = [row.split() for row in beats]
        # Gate of Atlantis has a 1/36 measure, which is fractional at 48 PPQN.
        self.scale = lcm(4 * resolution, *(int(row[2]) for row in rows)) // (4 * resolution)
        self.ticks_per_quarter = resolution * self.scale
        for position, numerator, denominator in rows:
            bar, beat, offset = map(int, position.split(","))
            numerator, denominator = int(numerator), int(denominator)
            if beat != 1 or offset or numerator <= 0 or denominator <= 0:
                raise ValueError(f"Unsupported meter change {position} {numerator}/{denominator}")
            start = self.tick(position) if self.meters else 0
            self.meter_bars.append(bar)
            self.meters.append({"bar": bar, "tick": start, "numerator": numerator,
                                "denominator": denominator})

        self.tempos = []
        self.tempo_ticks = []
        time_ms = 0.0
        previous_tick = 0
        previous_bpm = None
        for row in tempos:
            position, bpm, *flags = row.split()
            tick = self.tick(position)
            bpm = float(bpm)
            if previous_bpm is not None:
                time_ms += (tick - previous_tick) / self.ticks_per_quarter * (60_000 / previous_bpm)
            self.tempo_ticks.append(tick)
            self.tempos.append({"tick": tick, "time_ms": time_ms, "bpm": bpm, "flags": flags})
            previous_tick, previous_bpm = tick, bpm

    def tick(self, position):
        bar, beat, offset = map(int, position.split(","))
        meter = self.meters[bisect_right(self.meter_bars, bar) - 1]
        beats = (bar - meter["bar"]) * meter["numerator"] + beat - 1
        return meter["tick"] + beats * (4 * self.ticks_per_quarter // meter["denominator"]) + offset * self.scale

    def milliseconds(self, tick):
        tempo = self.tempos[bisect_right(self.tempo_ticks, tick) - 1]
        value = tempo["time_ms"] + (tick - tempo["tick"]) / self.ticks_per_quarter * (60_000 / tempo["bpm"])
        # Fumen stores the result of the double-precision clock as a float.
        return struct.unpack("<f", struct.pack("<f", value))[0]


def compile_chart(raw, judgment, metadata):
    sections = {}
    section = None
    # Some later charts have UTF-8 author comments in otherwise ASCII/CP932
    # VOX data (1751 EXCEED lecture). Comments do not belong to the grammar.
    for raw_line in raw.splitlines():
        line = raw_line.split(b"//", 1)[0].decode("cp932").strip()
        if not line:
            continue
        if line == "#END":
            section = None
        elif line.startswith("#"):
            section = sections.setdefault(line[1:], [])
        elif section is not None:
            section.append(line)
    resolution, = sections.get("BEAT RESOLUTION", [str(TICKS_PER_QUARTER)])
    clock = VoxClock(sections["BEAT INFO"], sections["BPM INFO"], int(resolution))
    version = int(sections["FORMAT VERSION"][0])

    events = []
    for track in range(1, 9):
        for row in sections.pop(f"TRACK{track}", []):
            position, *fields = row.split()
            start = clock.tick(position)
            step = (9 if version >= 13 else 8) if track in (1, 8) else 2
            if clock.scale != 1 and len(fields) > step:
                fields[step] = str(int(fields[step]) * clock.scale)
            event = {"track": track, "position": position, "tick": start,
                     "time_ms": clock.milliseconds(start), "fields": fields}
            if 2 <= track <= 7 and int(fields[0]) > 0:
                end = start + int(fields[0]) * clock.scale
                event["hold_end"] = {"tick": end, "time_ms": clock.milliseconds(end)}
            events.append(event)
    events.sort(key=lambda event: event["tick"])

    controls = []
    for row in sections.pop("SPCONTROLER", []) + sections.pop("LOCKED_SPCONTROLER", []):
        position, name, mode, duration, *parameters = row.split()
        start = clock.tick(position)
        controls.append({"name": name, "position": position, "tick": start,
                         "time_ms": clock.milliseconds(start), "mode": int(mode),
                         "duration_ticks": int(duration) * clock.scale, "parameters": parameters})
    # Automation Paradise splices these controller curves into a new chart.
    # Regular charts read the complete original POSTEFFECT rows on Android.
    for row in sections.get("POSTEFFECT", []):
        position, mode, duration, name, _, _, label, *values = row.split()
        if (name, label) not in {("ChromaticAbberation", "Strength"), ("RadicalBlur", "Strength"),
                                 ("CrtMonitorEffect", "Active"), ("CrtMonitorEffect", "Distortion")}:
            continue
        start = clock.tick(position)
        first, last = values[-2:]
        controls.append({"name": name + label, "position": position, "tick": start,
                         "time_ms": clock.milliseconds(start), "mode": int(mode),
                         "duration_ticks": 0 if duration.endswith("ms") else int(duration) * clock.scale,
                         "parameters": [first.removesuffix("f"), last.removesuffix("f")]})
    controls.sort(key=lambda event: event["tick"])

    end_position, = sections["END POSITION"]
    end_tick = clock.tick(end_position)
    meter = clock.meters[0]
    return {
        **metadata, "source_sha256": sha256(raw).hexdigest(),
        "ticks_per_quarter": clock.ticks_per_quarter, "bpm": clock.tempos[0]["bpm"],
        "meter": [meter["numerator"], meter["denominator"]],
        "tempo_map": clock.tempos, "meter_map": clock.meters,
        "chart_end": {"position": end_position, "tick": end_tick, "time_ms": clock.milliseconds(end_tick)},
        "events": events, "controllers": controls, "other_vox_sections": sections,
        "judgment": judgment["judgment"], "offsets": judgment["offsets"],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("vox", type=Path)
    parser.add_argument("judgment_fixture", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--song-id", type=int, required=True)
    parser.add_argument("--title", required=True)
    parser.add_argument("--artist", required=True)
    parser.add_argument("--difficulty", required=True)
    parser.add_argument("--music-db", type=Path, help="Original music_db.xml for the chart's maximum EX score")
    args = parser.parse_args()
    metadata = {"song_id": args.song_id, "title": args.title, "artist": args.artist,
                "difficulty": args.difficulty, "source_vox": args.vox.name}
    if args.music_db:
        database = ElementTree.fromstring(args.music_db.read_bytes().decode("cp932"))
        entry = database.find(f"music[@id='{args.song_id}']/difficulty/{args.difficulty.lower()}")
        if entry is None:
            parser.error(f"No chart for {args.song_id} {args.difficulty} in music_db.xml")
        metadata["max_exscore"] = int(entry.findtext("max_exscore"))
        metadata["level_tenths"] = int(entry.findtext("difnum"))
    original = json.loads(args.judgment_fixture.read_text(encoding="utf-8"))
    chart = compile_chart(args.vox.read_bytes(), original, metadata)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(chart, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    print(f"{len(chart['events'])} note rows, {len(chart['controllers'])} controller rows, "
          f"chart end {chart['chart_end']['time_ms']:.3f} ms")


if __name__ == "__main__":
    main()
