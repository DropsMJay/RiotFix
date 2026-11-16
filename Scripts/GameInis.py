"""Writes RiotFix.PIH.ini and RiotFix.TTB.ini from RiotFix.ini.

RiotFix.ini is the one place where every option is explained. The two per-game files are the same file with the
values recommended for that game, without the options that do not apply to it, so they never fall behind the main
file's explanations again. Edit RiotFix.ini and the tables below, then run: python3 Scripts/GameInis.py
"""
import re
import sys
from pathlib import Path

Root = Path(__file__).resolve().parent.parent

Games = {
    "RiotFix.PIH.ini": {
        "Header": "; RiotFix - recommended configuration for THE SUFFERING (SUFFERING.EXE)",
        "Values": {
            ("Shadows", "Resolution"): "1024",
            ("LOD", "DistanceScale"): "2.0",
            ("Memory", "Model"): "64", ("Memory", "Texture"): "192", ("Memory", "Animation"): "48",
            ("Memory", "Sound"): "128", ("Memory", "Sequence"): "4", ("Memory", "String"): "2",
            ("Memory", "Lighting"): "16", ("Memory", "Waveform"): "4", ("Memory", "Font"): "2",
            ("Window", "Title"): "The Suffering",
        },
        "Drop": {("Memory", "ScreenSet")},
    },
    "RiotFix.TTB.ini": {
        "Header": "; RiotFix - recommended configuration for THE SUFFERING: TIES THAT BIND (SufferingTTB.exe)",
        "Values": {
            ("LOD", "DistanceScale"): "2.0",
            ("Memory", "Model"): "64", ("Memory", "Texture"): "192", ("Memory", "Animation"): "48",
            ("Memory", "Sound"): "128", ("Memory", "Sequence"): "4", ("Memory", "String"): "2",
            ("Memory", "Lighting"): "16", ("Memory", "Waveform"): "4", ("Memory", "Font"): "2",
            ("Memory", "ScreenSet"): "4",
            ("Window", "Title"): "The Suffering: Ties That Bind",
        },
        "Drop": {("Draw", "CullDistanceMultiplier"), ("Audio", "EngineReverb")},
    },
}

HeaderLines = [
    "; Rename this file to RiotFix.ini and put it next to dinput8.dll (or next to RiotFix.asi), with SDL3.dll.",
    "; Every run writes RiotFix.log.",
]


def Build(Lines, Game):
    Section, Kill, Used = None, set(), set()
    Out = list(Lines)
    for Index, Line in enumerate(Lines):
        Match = re.match(r"\[(\w+)\]", Line)
        if Match:
            Section = Match.group(1)
            continue
        Match = re.match(r"(;?)(\w+)=(.*)", Line)
        if not Match:
            continue
        Key = (Section, Match.group(2))
        if Key in Game["Drop"]:
            Kill.add(Index)
            Above = Index - 1
            while Above >= 0 and Lines[Above].startswith(";") and not re.match(r";\w+=", Lines[Above]):
                Kill.add(Above)
                Above -= 1
        elif Key in Game["Values"]:
            Out[Index] = f"{Match.group(1)}{Match.group(2)}={Game['Values'][Key]}"
            Used.add(Key)
    Missing = set(Game["Values"]) - Used
    if Missing:
        sys.exit(f"not in RiotFix.ini: {sorted(Missing)}")
    Body = [Line for Index, Line in enumerate(Out) if Index not in Kill]
    First = next(Index for Index, Line in enumerate(Body) if Line.startswith("["))
    return [Game["Header"], *HeaderLines, ""] + Body[First:]


def Main():
    Lines = (Root / "RiotFix.ini").read_text(encoding="utf-8").split("\n")
    for Name, Game in Games.items():
        (Root / Name).write_text("\n".join(Build(Lines, Game)), encoding="utf-8")
        print(f"wrote {Name}")


if __name__ == "__main__":
    Main()
