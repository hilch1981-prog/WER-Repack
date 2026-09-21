# mod-wowlegends tests

Standalone harnesses. Not wired into CMake — they are compiled by hand when the
logic they cover changes. Each one is deliberately **generated from the shipped
source** rather than hand-written, so it can never drift into testing a
reimplementation instead of the real thing.

## camp_alloc_test.cpp — Warband Camp phase-bit allocator

Covers the load-bearing claim of the Warband Camp redesign: that 31 phase bits
are enough for an entire world because a bit is only "taken" by camps within
`PHASE_REUSE_RADIUS`.

The constants, `struct Camp`, `Dist2D` and `PickPhaseBit` are extracted verbatim
out of `src/wowlegends_warbandcamp.cpp`. If you change any of them, regenerate
rather than editing this file.

```bash
cl /nologo /EHsc /std:c++20 /W4 camp_alloc_test.cpp /Fe:camp_alloc_test.exe && camp_alloc_test.exe
```

Last run 2026-08-06: **10/10 pass**.

What it pins down:

| Assertion | Why it matters |
|---|---|
| Just **inside** the reuse radius, a new camp must not share its neighbour's bit | Two camps sharing a bit within view of each other would show each other's tents |
| Just **outside** it, bit 1 is reused | This is the entire argument for placing camps in the real world instead of on one dedicated map. If it fails, the scrapped map-37 design was right |
| 31 co-located camps exhaust the pool and the 32nd is **refused** | v1 had a bug in the same shape that permanently bricked claiming after 18 camps. A refusal is correct; a wedge is not |
| The returned bit is always 1..31 | `1u << 32` is undefined behaviour |
| `CAMP_MIN_SEPARATION > 2 * CAMP_RADIUS` | Otherwise a player can stand inside two camps and "which camp am I in" is a coin toss |
| `PHASE_REUSE_RADIUS > MAX_VISIBILITY_DISTANCE + CAMP_RADIUS` | Guarantees two camps sharing a bit can never both be on screen |

⚠️ **A failing assertion here is not automatically a code bug.** The first run
failed one case and the *test* was wrong: the probe point sat 591 yards from the
nearest of 31 camps spread over 300 yards, so it was still inside the reuse
radius and refusing was correct. Check the geometry before changing the source.
