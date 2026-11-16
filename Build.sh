#!/bin/sh
# Builds RiotFix with MinGW-w64 (i686). Output: Build/dinput8.dll and Build/RiotFix.asi (same file).
# Pass "debug" to keep symbols (used to check hook calling conventions with objdump).
set -e
cd "$(dirname "$0")"
mkdir -p Build
Strip=-s
[ "$1" = "debug" ] && Strip=
# The build stamp goes into the version string, so two builds of one version can be told apart. It is written in
# the time zone the project is worked in, with the offset shown, so it lines up with a tester's own clock and
# with the times in RiotFix.log. Override with RiotFixTimeZone when building elsewhere.
Stamp=$(TZ=${RiotFixTimeZone:-America/Sao_Paulo} date "+%Y-%m-%d %H:%M %z")
i686-w64-mingw32-gcc -O2 -Wall -Wextra -shared $Strip -static-libgcc -DRiotFixBuild="\"$Stamp\"" -o Build/dinput8.dll \
    Source/RiotFix.c Source/RiotCore.c Source/RiotMemory.c Source/RiotRendering.c Source/RiotSystem.c Source/RiotDisplay.c Source/RiotAccessibility.c Source/RiotWindow.c Source/RiotAudio.c Source/RiotSdl.c Source/RiotInput.c Source/RiotPad.c Source/RiotTiming.c Source/RiotDiagnostics.c Source/dinput8.def -lole32 -luuid
cp Build/dinput8.dll Build/RiotFix.asi
