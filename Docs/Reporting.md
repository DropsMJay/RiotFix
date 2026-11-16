# Reporting a problem

What a report needs so the cause can be found by reading evidence instead of by guessing. Everything here comes
from reports that worked and from one that cost an hour because a piece was missing.

## The short version

Say which build, what you did, and what happened. Then send `RiotFix.log`. For a crash, send `RiotFix.dmp` too.
If the game folder is connected to the session, say so and name nothing: the files can be fetched from there,
including the ones you did not think to send.

## What to say in words

Four things, and the first two matter more than any file:

- **Which build.** The first line of the log has it, stamp included (`RiotFix 0.4.4 (2026-11-15 09:58 -0300)`).
  Two builds of one version exist often enough for the stamp to matter.
- **The last build where it was right.** A fault that appeared between two builds is nearly solved already: what
  changed between them is a short list, and the list can be read. Without it, every patch ever written is a
  suspect.
- **Where it happens, and whether it happens again at the same place.** "Crashes" and "crashes every time at the
  lift after the yard" are different problems. A fault that repeats at one spot can be walked through
  instruction by instruction; one that wanders cannot.
- **What is different about your setup**, if anything: options you changed, another mod, an overlay (RivaTuner
  and the like take a module slot and move where the game's own libraries land).

Say what you saw, not what you think it was. A wrong guess sends the search the wrong way, and yours carries
weight because you are the one looking at it.

## Files, and what each one answers

| file | when | what it gives |
|---|---|---|
| `RiotFix.log` | always | which patches applied, every count of the session, and the crash's registers |
| the log of a run that was **right** | anything that used to work | the comparison. Two logs side by side show the one line that differs |
| `RiotFix.dmp` | any crash | the real call stack, the objects, the memory |
| the same with `FullCrashDump=1` | a crash that has to be *explained* rather than located | the code and the read-only data, which is where every class's table of methods lives |
| screenshots | anything visual | two of them: the wrong moment and the right one, from the same spot |

### The log's stack list is a guess, and the dump's is not

At a crash the log prints a list under "stack (likely return addresses)". That list is a **sweep**: it walks the
stack and prints whatever looks like code, so leftovers from calls that finished long ago sit in it next to the
real ones, in no reliable order. It is enough to see roughly where a crash is. It is not enough to say which
function called which, and reading it as if it were a call stack costs hours.

The `.dmp` has the real thing. Send it for any crash; it is a few megabytes.

### When the ordinary dump is not enough

The ordinary dump holds the stacks and the writable data. It leaves out the code and the read-only data — and
that is where vtables live, so a crash can be followed to an object but not to the function that was about to
run. When a crash needs explaining rather than locating, put this in `[Debug]`:

```
FullCrashDump=1
```

The file grows to the size of the game's memory, a few hundred megabytes. Over 400 MB it cannot be sent as it is:
right-click → **Send to** → **Compressed (zipped) folder** first. A full dump is mostly empty pages and
compresses to about a quarter.

## Do not relaunch more than once before reporting

On every start the files rotate: `RiotFix.log` becomes `RiotFix.1.log`, that one becomes `.2`, and `.3` is lost.
The dump keeps **one** older copy: `RiotFix.dmp` becomes `RiotFix.1.dmp`, and the one that was `.1` is gone.

So a single relaunch is safe — the crash is still there as `RiotFix.1.log` and `RiotFix.1.dmp`. A second
relaunch loses the dump. If you have crashed twice chasing the same fault, say which run is which.

## By kind of problem

**Crash.** Log and dump. Say whether it survives `NativeDevice=0`, and whether it survives starting the level
again from the same save — a fault that needs a particular approach to a place is different from one that waits
there for anybody.

**Something looks wrong.** Two screenshots from the same spot: the wrong moment and a right one. For anything
that changes during a transition, one *during* and one *after* — that pair is what made the HUD's fade problem
readable at all. Full screen, unscaled; a photograph of the monitor loses the pixel positions that carry the
answer.

**Sound.** The log's own counters at the end answer most of it: how many buffers were made, how many asked for
effects, how many were moved to software, how many plays failed. If a particular sound is the problem, give its
size in bytes from SurrealEd's Audio tab and set `[Debug] WatchSound` to it; every event of that one buffer is
then written out.

**Controller.** Which layout, which pad, and whether the keyboard still answers. `[Debug] LogBindings=1` reports
how often the engine read the pad, what each action answered, and how often the rumble mixer reached us. A pad
that does nothing and a pad the engine never reads look identical on screen and are opposite problems.

**Slow.** The frame rate with the patch and without it, at the same place, and whether the limiter is on.

## What is not needed

The whole folder. A description of the code — the source is here. An apology for the report being short: four
lines and a log are a good report. And no need to work out the cause before writing; the time it takes to find
it from evidence is shorter than the time lost to a theory that has to be unwound.
