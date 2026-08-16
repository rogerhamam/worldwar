# Golden fixture 1

A fixed (non-procedural) starting scenario, spawned in this exact order
by both the Python recorder (`golden_record.py`, part of the retired
Python prototype -- not present in this repo, kept only as historical
context for how this fixture was validated) and the C++ replayer
(`headless_runner --fixture 1 ...`), so entity ids line up (both sides
auto-increment ids from 1 in spawn order):

World: 60x60 tiles, `map_type="random"`, `water=False`, `n_players=2`.
Team 0: civ=0 (UK), `is_ai=False`. Team 1: civ=2 (Germany), `is_ai=False`
(AI is deliberately off on both teams -- this fixture isolates the
deterministic gameplay mechanics, not AI decision-making or procedural
generation, which can't be bit-matched across languages/RNGs anyway;
see the note below).

Spawn order (id assigned = order). NOTE: the base's footprint is 4x4
tiles @ 24px = 96x96px, so a base at (600,600) occupies roughly
[552,648] on both axes -- keep every other spawn point clear of that
box on at least one axis, or a unit spawned inside it can never step
out (confirmed by hand while building this fixture: a civilian placed
at (640,600), inside the box, was permanently stuck -- a fixture bug,
not a sim bug, fixed by moving it to (750,600)):
1. `base`, team 0, (600, 600)
2. `civilian`, team 0, (750, 600)
3. `tree` resource, (790, 600)
4. `rifleman`, team 0, (600, 750)
5. `base`, team 1, (1200, 600)
6. `rifleman`, team 1, (1200, 750)

Then `World::prime()` / `world.grid.rebuild(...)` + `control.recompute(...)`
before tick 0.

Commands are in `fixture1.commands.csv` (`tick,cmd,args...`), applied
immediately before that tick's `update()` call.

## Running the comparison

The Python side (`golden_record.py`) no longer exists in this repo (the
Python prototype it belonged to was retired) so this comparison can't be
re-run today. It's kept here as a record of the last verified parity
result. The C++ side alone can still be replayed:

```
game/build/tools/headless_runner/headless_runner.exe \
    --fixture game/tests/golden/fixture1.commands.csv --ticks 1200 --snapshot-every 40 \
    > /tmp/cpp_out.csv
```

As of 2026-07-10 this matched byte-for-byte over 1200 ticks (a full
simulated minute) against the Python recording, including gathering,
movement, building placement, production queueing, tech research (with
its stat-bonus application), and a scripted attack command that plays
out to an actual kill.

**Why not compare full `scenario.new_skirmish` output?** Its terrain and
resource placement are driven by the match RNG, and Python's
`random.Random` and the C++ port's `xoshiro128**` are different
algorithms -- "the same seed" does NOT produce the same draw sequence
across languages, so procedurally-generated state can never be bit-exact
between the two. What CAN and should match exactly is everything that
doesn't touch the RNG: movement speed/pathing, combat damage, gather
rates, construction/production timing, resource costs, tech gating --
i.e. the actual ported game logic, which is exactly what this fixture
isolates by using a hand-placed (RNG-free) starting layout.
