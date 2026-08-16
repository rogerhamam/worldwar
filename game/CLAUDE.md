# World War engine — CLAUDE.md

This is the main C++ engine (`game/`): sim (`sim/`), client (`client/`),
render (`render/`), campaign data model (`campaign/`), tests
(`tests/sim_tests`), and tools (`tools/headless_runner`). The standalone
campaign editor lives in the sibling `editor/` folder with its own
`CLAUDE.md` — don't confuse the two.

Runtime assets/data are read from the repo-root `assets/` and `data/`
(compile-time `WW_ASSET_DIR`/`WW_DATA_DIR` for dev builds; a copy next to
the exe for published builds). Edit those, not the `dist/` or `editor/`
copies — `publish.bat` regenerates `dist/` from the repo root.

The current ongoing thread is **making the skirmish AI as strong as
possible via self-play**, without ever giving it stat/resource edges over
a human. Most of this file is about that.

## Building & running

**REBUILD `game/build` BEFORE ASKING THE PLAYER TO TEST ANYTHING.** There are
three client binaries and it is very easy to hand back a stale one — this has
already wasted two round-trips ("I thought we changed this" / "it still does
X"). `game/build` is what `build.bat` produces and what the player launches;
`game/build-release` is the tournament/test build; `dist/WorldWar/` is only
refreshed by `publish.bat`. Building only build-release means the player is
still running old code. Note the link **fails with "Permission denied" while
the game is open** — check `Get-Process worldwar_client` and rebuild after it
closes, don't assume the build succeeded.

- `build.bat` configures+builds `game/build` (and the editor) with CMake +
  Ninja (MSYS2 ucrt64 toolchain). **`game/build` has NO optimization**
  (`CMAKE_BUILD_TYPE` empty → `-O0`).
- For anything perf-sensitive (tournaments), use the **optimized build dir**
  `game/build-release`, configured with `-DCMAKE_BUILD_TYPE=Release
  -DCMAKE_CXX_FLAGS=-ffp-contract=off` (and `-DWW_BUILD_TESTS=ON` for the
  test target). `-ffp-contract=off` keeps floating-point bit-identical to
  the `-O0` build, so results (and the golden test) are unchanged — it's
  ~5× faster for free. Rebuild it after sim changes:
  `cmake --build game/build-release`.
- Tests are gated behind the `WW_BUILD_TESTS` CMake option.

## The AI self-play arena (`headless_runner --tournament`)

```
game/build-release/tools/headless_runner/headless_runner.exe \
    --tournament N --ticks 72000 --seed S --jobs 12 [--allied] [--out res.csv]
```

- `--ticks` is the per-match cap (72000 = 60 sim-minutes at the fixed
  1/20s dt; 24000 = 20 min). A match ends early the instant
  `Control::game_over` flips (unless `--allied`).
- `--jobs N` runs matches across N threads. Each match is fully
  self-contained (own Match/World/Control/Rng, seeded by index) → **results
  are byte-identical regardless of thread count**, just faster. Default =
  hardware concurrency.
- `--allied` puts both AIs on the SAME team so they never fight — a pure
  **economic-development** test (game_over fires immediately, so the loop
  runs the full `--ticks` cap). Use this to measure economy in isolation.
- Alternates which team index carries the candidate each match.
- Per-team output includes `industrial@M:SS(vils=N)` (when/at-what-economy
  it reached era 1) or `industrial@never(agebldgs=K)` (K = live age-
  qualifying buildings — <2 means it physically can't age), plus a summary
  line: `reached Industrial: X/Y (Z%) | avg advance M:SS, avg villagers V`.

**CRITICAL harness fact:** `new_skirmish` only turns on the map-derived
skirmish AI (`Team::ai_map_derive`) for teams `i != 0` (team 0 is the human
in a real game). The tournament forces team 0 to be AI too, so
`run_one_match` **explicitly sets `ai_map_derive = true` on BOTH teams** —
without that, team 0 silently runs the fallback (non-map-derive) AI and
every measured number is half garbage. (This bug polluted every metric for
a while; fixing it jumped allied industrial% from 62.5% → 97.5%.)

**Measurement caveat:** every change so far has been promoted
unconditionally, so both tournament teams are identical and **win-rate is
50/50 by construction — meaningless**. Judge changes by the *metrics*
(industrial%, advance time, peak_vil, peak_army, idle_tc, gathered
resources), not win-rate. To get a real win-rate delta, gate a change
behind `if (td.ai_variant == 1)` and run candidate-vs-baseline (the
`--candidate-variant` path); this hasn't been set up yet.

**Current economy health** (both teams map-derive, seed 7000, 20 matches,
`--allied`, 20-min cap): **100%** reach Industrial (40/40), avg **7:04**, ~15
vils, idle_tc 74s, War **90%** @ 17:03.

**60-min cap** (`--tournament 40 --ticks 72000 --seed 7000 --jobs 12 --allied`),
the live baseline: Industrial **100%** @ 7:15, War **100%** @ 18:33,
Scientific **96.2%** @ 29:21. (Before the movement fixes further down,
Scientific was 90% @ 31:57 — unsticking villagers and armies closed the gap.
Scientific reads 96-99% across runs; treat ~97% as the figure. Late `idle_tc` is
~4500s and harmless — it accrues *after* aging; see the "don't chase idle_tc"
note under Ageing.)

Progression of that same 20-min run across 2026-07-26's changes:

| after | Industrial | advance | idle_tc |
|---|---|---|---|
| AI economy (barracks/farms) | 92.5% | 9:20 | 88s |
| A* fixes + gather routing | 92.5% | 9:04 | 79s |
| delivery routing + replan ladder | **100%** | 9:10 | **21s** |
| build-order overhaul (below) | **100%** | **7:07** | 72s |

The 37/40 → 40/40 jump was the delivery stall: those 3 stragglers were
exactly the long-standing "last 1-2% never leave Victorian" cases.

The 9:10 → 7:07 came with **fewer villagers (21 → 15) and more idle TC (21s →
72s)**, and that is the intended trade, not a regression — it is the same
economy-size-vs-advance-time tension recorded under Ageing. War at the 20-min
cap moved 42.5% → **87.5%** on the way, which is the metric that actually
tracked the fix.

**Beware stale comparisons:** spawn positions are now randomised
(`spawn_points`, below), so every seed generates a *different map* than the
numbers recorded before 2026-07-26. Only compare runs from the same era of
the code.

Verify against `ww_sim_tests` too: **two pre-existing failures are expected
and unrelated** (`test_control.cpp` upgrade-chain, `test_aircraft.cpp`
landing). Any *other* failure is a real regression.

## Current skirmish-AI design (control_ai.cpp)

All of the below is skirmish-only, gated on `Team::ai_map_derive`. Campaign
AI (`new_from_level`) leaves that false and keeps its authored behaviour.

**Map assessment — `ai_assess_map` (once, first serviced tick).** Fills
`Team::ai_plan` from the geometry around the team's base:
- nearest-enemy-base distance → `closeness`; local water fraction + fish
  proximity → `can_fish`; a choke (narrowest natural gap with impassable
  blockers both sides of the base→enemy axis) → a `wall_planned` line.
- **playstyle** (with a small random nudge, replaces the old fully-random
  `ai_behavior`): `naval` (water+fish), `aggressive` (enemy close),
  `defensive` (a sealable choke + not too far), else `boom`. Maps onto the
  offensive-push `ai_behavior` + a villager goal + `ai_intensity_jitter`.

**Reserved production throttle (`ai_economy`/`ai_train`).** Instead of the
old binary "bank hard vs mass units", the AI spends only a *fraction*
(`ai_intensity` = playstyle base + per-team jitter + a bump when enemies
are near home) of its food *income* on military, accruing a food budget
(`ai_mil_budget`) that `ai_train` debits. So food net-climbs while some
army still trickles out; the average AI is reserved, a few (high jitter /
aggressive) go all-in. A small always-exempt defensive floor keeps it from
being defenceless; when spending hard it raises the villager goal to
recover surplus.

**Ageing (THE thing that was broken repeatedly).**
- `World::can_age_up(team)` — building prerequisite, enforced inside
  `World::enqueue` so human and AI are gated identically. Industrial needs
  any **2** of {barracks, academy, market, refinery, shipyard}; War needs
  2 of {factory, university, airbase}; Scientific needs 2 War-era
  buildings (base/fortress) OR 1 fortress. (tower/aa tower/house/farm/
  walls/outpost never count.)
- **Prerequisite buildings are the real advancement gate (not resources).**
  Once the age-up deadlock and oil scarcity were fixed, stuck teams were
  universally `can_age_up==false` while sitting on huge stockpiles: era-1
  teams missing 2 of {factory,university,airbase}; era-2 teams on 7k+ oil
  missing the Scientific prereq (2 of {base,fortress} OR 1 fortress). Two
  fixes: (1) `ai_build` pushes the *missing* prereqs HIGH the moment a team
  can't yet age (War prereqs at era 1; a 2nd base — cheaper, doubles as
  expansion — or a fortress at era 2), and (2) `ai_build_spot`'s generic
  placement now spirals rings 3→24 at 16 angles (was 3→7 at 8) because the
  64×64 prereq buildings couldn't find a clear 2×2 spot on a base packed with
  farms/houses/walls and silently failed to place. Lifted allied **Scientific
  ~20%→58%** (War ~73%). The `fortress_useful` civ-unit gate still guards the
  *lower-priority* fortress; the Sci-prereq push builds one regardless.
- **Age-push bank:** once a *working* economy exists (~20 villagers,
  `age_ready`) and the age is buildable but not yet affordable, briefly
  pause villager + military production so food reaches the cost fast, then
  age and resume. Without this the town centre spent every spare 80 food on
  another villager and never accumulated the ~500 age cost until the whole
  boom finished → reached Industrial absurdly late.
- **Age-ups ALWAYS enqueue at the FRONT of the base queue** (`priority=true`
  in `ai_economy`, was gated to `ai_variant==1`). This was not a "slow"
  issue — it was a hard **deadlock**: the AI keeps the base queue topped with
  civilians, and once the team hits its population cap a civilian at the
  *front* makes zero progress (`building_behavior.cpp` pop check) and
  strict-FIFO blocks the age-up queued behind it *forever*. Teams sat at
  Victorian the whole match with 18k food / 3k oil, `can_age_up` satisfied,
  and the War age *in the queue* but never advancing. Fixing it jumped allied
  **War 42.5%→80%, Scientific 15%→36%**. (Latent general bug: a pop-blocked
  unit at the front of *any* base queue also stalls a human's queue — e.g. a
  tank behind a maxed-pop villager.)
- **Aging speed dominates late economy.** Tried breaking the late-game
  pop-cap "plateau" (raise `vil_goal` 48→60 + house ceilings 12→24 so the TC
  never idles): `idle_tc` fell 1422s→278s but **War collapsed 80%→34%, Sci
  36%→2.5%**. A bigger economy diverts food/wood from the age cost, so teams
  bank each age *later*. The late idle_tc is harmless — it happens *after*
  aging. Reverted. Don't chase idle_tc; chase advance-time.

**Opening build order — economy first, NOT a muscateer rush (2026-07-26).**
`ai_build` used to push the first barracks 4th in the order, unconditionally,
above houses/academy/farms — so every AI regardless of plan had a barracks up
inside the first minute and made muscateers off three villagers. `ai_train`'s
boom-phase hold only ever capped the *rate*; it never stopped the building
going up first and soaking the opening's wood. Now gated on `army_time`:
- `rush_plan` (aggressive playstyle, or campaign `!ai_map_derive`) still opens
  with it — that IS the rush, by design.
- Everyone else waits for **12 villagers**, era ≥ 1, or `Team::ai_threat >= 2`
  (new field: enemy military within 24 tiles, computed in `ai_economy`, which
  runs immediately before `ai_build` in the same `ai_tick`). The threat term is
  what stops a boom plan being caught defenceless by someone else's rush.
  **It needs TWO enemy soldiers, not one** — every team starts with a cavalry
  and `ai_scout` sweeps a spare unit across the map in era 0, so at `> 0` a
  lone scout wandering past tripped the gate and bought the instant barracks
  straight back.
- **Measured** (seeds 300-315): non-aggressive plans now raise the barracks at
  **4-10 sim-minutes with 12-26 villagers**, never off the threat term. ~19% of
  seeds roll `aggressive` and still rush immediately, by design.
- The "second barracks" push is gated the same way — with zero barracks that
  condition is *also* true, so leaving it open just builds the first one there
  and walks around the gate.
- **Age-up cover:** Industrial needs 2 of {barracks, academy, market,
  refinery, shipyard} and refinery is era-1-gated, so a deferred barracks would
  quietly *become* the age gate. An era-0 `market` push (high priority, when
  `!can_age_up`) is the economic partner to the academy. Measured after: no
  ageing regression (92.5% Industrial, advance 9:56→9:20).

**Build-order overhaul — foundation spam & derelict foundations (2026-07-26,
from live play).** Five player-reported symptoms, one shared root: *nothing
counted work already in progress, and nothing watched whether it finished.*

- **`ai_build` returns on a placement that FAILED.** The single worst bug found
  here. The loop did `place_building(...); return;` unconditionally, but
  `place_building` has rejections `ai_build_spot` can't see — it re-checks
  affordability and `footprint_clear` (a unit may have wandered onto the spot),
  and **for team 0 it also requires the footprint to be explored**. So one
  candidate that found a spot but couldn't place jammed the whole build order
  behind it, every tick, forever. Measured: a team at 8 pop with 1500 banked
  wood, **zero houses and zero age-qualifying buildings for a full match**,
  re-picking the same doomed spot 20×/s. It now only returns on a placement that
  actually happened. This alone took Industrial 95% → **100%** and War 65% →
  **87.5%**. (This is the exact failure the ordered-candidate loop was written
  to fix, one layer further down — worth remembering as a shape.)
- **Work-in-progress cap.** `ai_build` places ≤1 building per call but is called
  every 0.05s on Hard, so "one per call" throttled nothing: five house
  foundations went down inside a second. Now ≤`clamp(cur_civs/5, 1, 3)`
  open foundations. Farms are naturally exempt — `place_building` spawns them
  already complete, so they are never foundations.
- **Pending houses count toward the AI's PLANNING figure (`cap_projected`).**
  `recompute` only credits a **complete** house, so while a house foundation
  stood unraised the team still read as pop-blocked and `cap - pop <= 1` fired
  again next tick, and the next. That is the five-houses-at-once bug. **The
  pop-cap mechanic is unchanged** — a foundation still grants zero population
  room to AI and human alike; `cap_projected` is a local in `ai_build` used only
  by the "should I queue another house?" predicates. The mechanic answers *how
  much room do I have now*; the build order needs *is more housing already on
  the way*, and answering the second with the first is the bug. Headroom is 2
  (not 1) so the TC isn't already capped by the time the house lands.
- **Housing is the one exemption from the WIP cap** — pop-blocking freezes
  villager production outright, and `cap_soon` is what bounds houses now.
- **Derelict-foundation watchdog** (`ai_tick`, `Building::ai_stall_t/
  ai_stall_con`). A foundation whose `construction` % stops moving is one nobody
  is raising. Past 15s it stops counting against the WIP cap (so a stuck
  foundation can't deadlock the build order); past 45s it is refunded pro-rata
  and poofed via the same `deleted = true` + `hurt` path the player's Delete
  uses. Walls exempt (segments legitimately wait their turn in the chain).
- **Wall crews capped at 1.** `ai_build_walls` deliberately hands ONE villager
  to a choke line and relies on wall chaining — but the generic foundation
  crewing loop then saw every *other* segment as uncrewed and pulled an idle
  villager onto each, so one wall order yanked ~10 villagers off the economy.
- **Farms are demand-gated, not count-gated.** `desired_farms` alone never
  worked because `food_workers` counts BERRY gatherers, so a berry-fed opening
  wanted 9 farms and built all 9 while the quotas kept those villagers on
  berries (**player saw 9 farms / 2 farmers**). Now also requires
  `free_farms < 2` — complete, unexhausted, `occupied_by`-free farms. Ahead-of-
  demand buffer is 1 spare (emergency paths allow 3). Self-correcting: as
  villagers occupy farms the count drops and the next farm goes up at once.
- **House spacing.** Every anchor term was deterministic, so consecutive houses
  resolved to the same berry vein and the same best-of-ring answer, landing in
  each other's shadow (**player: "all right next to each other, made no sense
  for gathering"**). Candidate spots within 5 tiles of an existing house are
  rejected, and an anchor already served by a house within 6 tiles is skipped
  entirely so the next house claims the next resource patch.
- **Refinery is placed ON the ore patch, and is no longer era-1-gated.** It was
  falling through to the generic base ring — i.e. planted next to the base,
  the very dropoff it exists to replace — while miners walked the full haul.
  Nothing in the catalog gates it; the era-0 weights already put 15-35% of the
  workforce on oil, whose only legal dropoff is the base (a house refuses carry
  types 2/3). Now built once `ore_workers >= 3`, anchored on the patch with the
  most miners, skipped if a refinery is already within 8 tiles of it.
  Metric-neutral on its own (7:33 vs 7:27) — this is a correctness fix.
- **Two builders per foundation** when ≥8 gatherers and ≤2 open foundations,
  and a carrying villager is pulled in when a foundation would otherwise have
  **zero** builders (carry survives the switch). Note `idle_civs` is every
  gatherer holding nothing *this instant*, i.e. most of the workforce — sizing
  crews off it directly cost 5 villagers and 60s of idle TC. Size off the
  workforce, not that list.

**Economy specifics.**
- **Shallow production queues (keep resources liquid).** Every `World::enqueue`
  pre-pays the item's full cost, so a deep queue *freezes* resources that could
  go to buildings/farms/age-ups. Base ≤2 (one building + one on deck — the
  on-deck slot starts the instant the prior unit finishes, so the TC never
  idles), barracks/academy/fortress ≤2 (academy ≤3 only when food floats).
  The **factory/airbase** previously had *no cap at all* and no throttle
  (tanks/artillery are oil+iron heavy with ~no food cost, so `train_mil`'s
  food throttle never fired) — they piled up siege and drained the oil
  Scientific needs. Now capped ≤2 AND held entirely while `ai_banking` (so
  banking for an age isn't sabotaged by the factory eating the oil). Effect on
  age rates is within seed noise (War ~74-82%, Sci ~25-31% across seeds).
- **Berries first, farms later.** No early farm-spam: farms only build once
  the berry vein near home is thinning (<5 nodes) OR the workforce is past
  ~10 (`need_farms`).
- **Farms track FOOD WORKERS, not headcount (2026-07-26).** A farm feeds
  exactly one villager (`Building::occupied_by` — a second one sent there
  bounces off), so a farm past the food workforce yields *nothing*: 40 wood
  and a build slot for zero income. `desired_farms` used to be `civs/2 + 2`,
  assuming ~half the workforce ends up on food — but nothing enforces that
  split, so when the resource weights put villagers on wood/oil/iron the farm
  count kept climbing with total headcount anyway. Seen in a real game: **~10
  farms with ONE occupied.** Now `clamp(food_workers + 2, 2, 24)`, with a
  separate `emergency_farms = clamp(food_workers + 4, 4, 24)` for the
  starvation paths (`food_emergency`/`food_at_risk`) — still anchored to the
  same workforce so it can't degenerate back into the old sprawl. Berry
  gatherers count as food workers, so the transition off a drying vein still
  ramps farms up *ahead* of demand. Campaign AI (`!ai_map_derive`) keeps its
  authored `3 + 2*era`.
- **House placement** prefers the berry vein (houses are food+wood
  dropoffs), picking the spot there nearest a woodline; falls back to
  woodline then generic.
- **Oil→army** (oil-poor land maps starve the oil-gated muscateer): a
  food-flush economy trains OIL-FREE academy units (cavalry/swordsman) —
  the academy is built during the boom and a 2nd is added when food floats;
  and surplus wood is traded → oil at the market when wood piles up.
- **Fishing:** naval/water plans build a shipyard and train fishing boats
  (food) before warships; `ai_economy` points idle fishing boats at the
  nearest shoal. (Fish DO spawn on any open water — `scenario.cpp`.)
- **Walls** (`ai_build_walls`): seals the one planned choke with a palisade
  line, but only after a working economy (n_civ ≥ 12), built by a SINGLE
  villager (the wall-chaining carries it segment→segment) so a crew doesn't
  get trapped on the far side.
- **Defense (`ai_defend_civilians`):** villagers only fight threats near
  home and disengage (return to work) once a threat retreats past a leash —
  no more chasing a scout across the map.
- **Scouting (`ai_scout`):** cosmetic only (the AI already sees the whole
  map — fog tracks team 0 only); one spare unit sweeps map waypoints in
  era 0.

## Map / resource generation (scenario.cpp)

- **Starting positions are randomised (`spawn_points`, 2026-07-26).** It used
  to be a fixed 8-entry table of map fractions with **no random input at all**:
  team 0 opened top-left and team 1 bottom-right in *every single skirmish*, so
  the human knew where the enemy was without scouting. It also capped every
  spawn at x ≤ 0.62 of the width regardless of map type, leaving the eastern
  third of a land map permanently unused. Now: teams spread **evenly around an
  ellipse inscribed in the map's usable land**, at a randomly rotated start
  angle. Only the rotation and ring radius are random — deliberately **no
  per-team angular wobble**, since even spacing is what keeps it fair (a wobble
  would put one player measurably closer to the enemy than another).
  - The land region is **measured** (bounding box of non-water tiles), not
    assumed: water maps flood a whole edge (the `random` toggle and normandy
    put a sea in the east, stalingrad the Volga, guam is a ringed island) *and*
    the coastline position is itself randomised, so any hardcoded fraction
    either wastes half the map or drops a base in the sea.
  - Minimum separation is enforced explicitly and scales with team count;
    layouts re-roll (bounded, best attempt kept) if a rotation bunches teams
    along a squeezed ellipse's short axis. The existing water/open-land nudge
    still runs afterwards as the final failsafe.
  - Consumes RNG draws, so **every seed now generates a different map** than
    before this landed.
- Map sizes bumped: `kMapSizeValues = {48,64,80,96}`, so **"Normal" is what
  "Huge" used to be** (64 → 128 cols) — small maps forced hyper-aggression.
  `area_scaled` keeps resource density constant across sizes (ref 80 cols).
- **Guaranteed home pods** per base (rolled once for the whole map so both
  players get the same supply): a berry vein (~6), a large oil pod (4-6), a
  small oil pod (2), a large iron pod (4-6), a small iron pod (2), spread
  to different sides. Plus small **neutral oil wells** ≥12 tiles from any
  base out in contested territory.
- A slightly bigger clear ring around the base (`NEAR = 6 tiles`) and
  near-base resources pushed out (~8-11 tiles) for build room.
- **Drop-off rules (`World::nearest_dropoff`):** base accepts everything;
  **house accepts only food+wood**; refinery only oil+iron. (Oil/iron with
  no refinery falls back to the base.)

## Movement / pathfinding (pathfind.cpp, unit_behavior.cpp)

Escaping a concavity means walking **away** from your goal — purely local,
reactive steering fundamentally cannot discover that, no matter how
sophisticated. Every fix has to come from the search layer. (An earlier
multi-session effort tried ~6 local-avoidance patches for this and reverted
all of them; see the pathfinding memory.)

**A\* (`pathfind.cpp`), all fixed 2026-07-26:**
- **Octile heuristic** (`1.4142*min + (max-min)`). Was Manhattan, which
  *overestimates* whenever a diagonal exists (a 10-tile diagonal really costs
  14.1; Manhattan claims 20). An inadmissible heuristic turns A* into near
  greedy-best-first — worst exactly in a pocket, where "head toward the goal"
  dives in and every tile of the climb out scores as a step backwards.
- **Closed set.** Nodes could be popped repeatedly, so `max_nodes` was eaten by
  tiles already visited. Consistent heuristic + closed set ⇒ `max_nodes` now
  counts DISTINCT tiles, a predictable budget.
- **`max_nodes` 4000 → 12000.** 4000 could be swallowed just filling a deep bay
  before finding the way around (Huge maps are 192×192).
- **PARTIAL PATHS.** On failure it used to return an empty path — and callers
  respond to that by steering straight at the destination, the worst possible
  move in a pocket. It now returns the route to the closest tile it did reach,
  so the unit leaves the pocket and re-paths from there. Only a path that
  genuinely arrived ends on the exact goal (a partial one must NOT be snapped).

**Rally (AI attack-move) was a FIFTH path with no route and no watcher — fixed
2026-07-26.** The whole implementation was `if (u.rally) { if (step_toward(u,
rally->x, rally->y, dt, world)) u.rally.reset(); }`. No A*, no stall detection,
no give-up — and `rally` is how every AI army crosses the map (offensive waves,
scout sweeps, defensive recalls; see control_ai.cpp's `rally =` assignments).
Greedy steering heads straight at the goal, so any concave pocket of trees or
buildings straddling the straight line swallowed the whole force: it pressed
into the back wall forever, because escaping a concavity means walking AWAY from
the goal and local steering cannot discover that. The player asked exactly the
right question — "wouldn't they just avoid that in the first place?" — and the
answer was that nothing was planning a route to avoid it with.

`advance_rally` now mirrors the gather walker: plan once per rally point, walk
the waypoints with the usual `kSeparation + 4` tolerance, watch NET progress
over a 2.5s window, two free replans, then drop the rally (the AI re-issues
waves on its own tick, so that's a retry, not a surrender). Keeps rally's own
semantics — normal ally blocking, unlike a player move order — so a charging
wave still spreads instead of stacking into a column. Regression test: a 4-unit
squad inside a U of trees, goal beyond the closed back. **0 of 4 escape without
the fix, 4 of 4 with it.**

**ALL FIVE movement paths now route and re-plan.** Each has a stall detector
and the same **replan-then-escalate ladder**: two free replans against the
current world, then escalate. Reactive rather than proactive on purpose — it
costs nothing when nothing is wrong (measured: A* is ~0.9% of sim time, 266
calls / 193 µs avg / 178 nodes avg over a 12-min match), at the price of eating
the detection window before reacting.

| path | detector | escalation after 2 replans |
|---|---|---|
| move order | `stuck_t` 1.5s + `stall_strikes` on path-distance | give up the order |
| build/repair | `approach_stalled` 2.5s window | drop the build target |
| gather approach | `gather_progress_check_t` 4s | switch resource |
| delivery | `approach_stalled` 2.5s window | switch to the next-best drop-off |
| rally (AI attack-move) | `rally_progress_t` 2.5s net-progress window | drop the rally; AI re-issues |

`approach_replans` is shared by the three approach kinds (mutually exclusive
per tick, each resets on arrival). **Repathing from the same spot often returns
the SAME path** — A* is unit-blind, so if a unit is the obstacle the new route
is identical. That's what the strike caps are for; any new repath site needs
one or it spins at 193 µs forever.

**Villager-vs-villager jamming — three fixes (2026-07-26, from live play).**
Player reports: a loaded villager "jittering back and forward forever, never
making it to the refinery"; another that "can never make it past the concavity
of the villager and the refinery". All three fixes below are covered by tests
that were confirmed to FAIL with the fix neutered.

- **`approach_stalled` measures NET displacement over a 2.5s window, not
  per-tick displacement.** THE keystone — both new regression tests fail without
  it. The old check asked "did the unit move <0.6px THIS tick?", which detects a
  unit standing perfectly still and nothing else. It is blind to **oscillation**:
  when step_toward's avoidance tiers all fail it falls through to the retreat
  tier, which moves a real distance every tick, and the goal pulls the unit
  straight back next tick. Per-tick displacement stays healthy forever while net
  progress is zero — so the timer never accumulated, the ladder never fired, and
  the unit jittered indefinitely. Threshold is 24px per 2.5s window; a villager
  runs 60px/s (~150px/window), so this is ~16% of nominal and cannot false-fire
  on a legitimate detour.
- **A replan always A*-routes, however close the target is.**
  `advance_to_building` skipped routing under 2 tiles and committed to the
  nearest perimeter point. Right for a first attempt, exactly wrong for a retry:
  a short approach only stalls when something is in the way, and the shortcut's
  answer is to keep pressing at it with no route. Repro geometry: a carrier 36px
  from the refinery door with two villagers 32px apart in front of it — each
  demanding 16.1px clearance, so the gap is ~0.2px too narrow to thread — sat at
  `path=0` cycling replans while one tile sideways was open the whole time.
- **`step_toward` phase-through tier (`Unit::wedged_ticks`).** After a full
  second with NO legal heading at all, a unit may phase through *allied* bodies
  (enemies still block absolutely) until it moves. Needed because the retreat
  tier averages only the directions to nearby **units**, so a pocket formed by
  one ally plus terrain — the player's "concavity of the villager and the
  refinery" — makes it flee directly into the terrain. With no legal heading the
  unit cannot take the first step of ANY route, so the ladders above are
  powerless no matter how well they replan. Same latitude move orders already
  have via `ignore_allies`; `resolve_overlap` un-stacks the result.

**Rejected: unit-aware A\* on replan.** A `settled_unit_at` predicate that made
retry plans treat parked bodies as terrain was written and measured. Every
movement test passed **identically with and without it** — unproven by this
file's own standard, and it costs a spatial query per tile examined. Removed.
The oscillation and phase-through fixes above carry those cases instead. If it
comes back, it needs a test that fails without it.

Effect on the 60-min allied baseline: **Scientific 90% → 98.8%**, avg advance
31:57 → **28:54**; War stays 100%, Industrial 100%.

**Gather and delivery previously called astar ZERO times** — both were bare
`step_toward`. Villagers do nearly all the walking in a match, so this was most
of the "units get stuck" seen in play; delivery in particular had no route AND
no watcher, which is the 55-minute `carry=10` freeze. Delivery now reuses
`advance_to_building` outright (a drop-off is just a building). Gather routes
beyond ~3 tiles; closer in the direct slide handles it. Two things that will
bite anyone touching this:
- It aims at a tile **beside** the target. A resource makes its own tile
  impassable (`resource_tiles_`), so routing *to* it fails the goal check every
  time and returns nothing. Same reason `advance_to_building` targets a
  building's perimeter.
- Replans are on a 1s cooldown (`Unit::gather_repath_t`) and drop stale routes
  when the target changes (`gather_path_for`). Without the cooldown an
  unreachable target runs a full-budget search every tick for every villager
  assigned to it.

**Villager bunching.** `kSeparation` = `kBodyRadius * 1.4` generally, but
gatherer-vs-gatherer pairs use `kGathererSeparation` = `* 1.15` so more fit
around one node. `blocked_by_unit` AND `resolve_overlap` must BOTH go through
`pair_separation()` — if only one did, it would shove villagers back out to a
gap the other was actively letting them close, and the pair jitters forever.
`resolve_overlap` ranks by *overlap depth*, not raw distance, since a nearer
pair is no longer necessarily the more overlapped one. The gather give-up
window ("this node is overcrowded, work elsewhere") is 4.0s (was 1.5s) — a
crowd around a node is transient, and a short fuse made villagers walk away
from a resource they'd have squeezed into a second later.

## Player-facing features (client)

- **Iron Wall** building (`data/catalog.json`): 5 iron, very high HP,
  `spr_grey_bricks`. Palisade re-sprited to `spr_wood_icon`. Palisade
  builds fast, iron wall very slow (`unit_behavior.cpp` per-name rate).
- **Click-drag walls** (orthogonal + diagonal): lays a connected segment
  line; villagers **auto-chain** along a wall (finish one → move to the
  nearest connected unfinished segment) so one order builds the whole run.
- Wall COLLISION rects are inflated a few px (world.cpp `rebuild_occupied`)
  so diagonally-adjacent segments seal the corner gap units could slip
  through; placement still uses the flush 32×32 footprint.
- **Walk-through foundations** (`Building::blocks_movement`): a foundation
  nobody has started work on (`construction == 0`) does NOT block movement —
  units path straight over it, so a freshly-placed foundation can't wall a
  villager off from its resource. It turns solid on the first hammer blow, and
  `unit_behavior.cpp` only lets that blow land while the footprint is clear of
  units, so nothing can ever be sealed inside. A builder that arrives *inside*
  the footprint steps off first (A* is free to route across a walk-through
  foundation, and `at_dropoff`'s buffer covers the whole interior, so it can
  stop dead in the middle). Teammates parked on it with nowhere to be get
  shoved off; enemies genuinely deny the build. Completion ejects anything
  still inside (the `blitz` cheat jumps 0→100 in one tick). Placement is
  UNAFFECTED — foundations stay in `all_building_rects_`.
- **Colour picker** (Random Map Setup): left-click the swatch cycles to the
  next *unused* colour, right-click cycles backwards; both skip colours other
  active rows hold. Right-click is dispatched via `handle_click(x, y, right)`
  and is **swallowed for every hit-rect except `cycle_colour`** — without that
  guard a right-click anywhere fires whatever button is under the cursor.
  `cycle_players` also de-duplicates when growing the roster (rows past
  `n_players_` don't reserve a colour, so they can collide on the way back up).
- **F1** toggles a per-team resource readout (`food/wood/oil/iron vN`) next
  to each leader in the bottom-right scoreboard, for diagnosing AI economy.
- Double-click select-all is a stricter manual check (250ms + same spot).
- Explosion SFX gain halved (`audio.cpp` `sfx_gain_table`).

## Villager productivity stall — CLOSED (2026-07-26)

All three failure modes from the original brief are fixed and measured. Kept
here because the *shape* of the bug is worth remembering, not as open work.

| original failure mode | fix |
|---|---|
| 2 villagers stuck "building" all game | unstarted foundations are walk-through; a builder inside one steps off; planning routes around them |
| villager can't reach its resource | `update_gather` A*-routes (previously never pathed at all) + replan ladder before abandoning the node |
| villager holding `carry=10`, can't DELIVER | delivery routes via `advance_to_building` + stall detection + escalate to the next-best drop-off |

**Result:** allied 20-match/20-min Industrial **92.5% → 100%**, idle_tc **88s →
21s**. The 3 teams that never left Victorian were exactly this.

**The lesson worth keeping.** Every one of these was "a code path that does
movement without a route, and nothing watching whether it works". When a unit
type seems stuck, check FIRST whether that particular branch calls `astar` at
all and whether any stall detector covers it — three of the four movement paths
in this file were bare `step_toward` with no watcher. Symptoms looked like
pathfinding quality; the cause was pathfinding *absence*.

**Never verified against the original repro** (seed 7024). Randomised spawns
mean that seed is now a different map. The regression tests build the failing
geometry directly instead, and each was confirmed to FAIL with its fix
temporarily neutered — do that when adding more, a movement test that passes
both ways is worthless.

## RESUME HERE — next up

1. ~~Re-baseline at the 60-min cap.~~ **DONE** — Industrial 100% @ 7:17, War
   100% @ 18:41, Scientific 90% @ 31:57 (see Current economy health). The old
   War ~77% / Sci ~60% figures in this file predate randomised spawns and are
   not comparable; the numbers above are the live baseline.
2. **`ai_variant` A/B gating** — still the single biggest process gap (see the
   measurement caveat near the top). Win-rate is 50/50 by construction until
   this exists, so every change is judged on metrics alone.
3. **Proactive path validation**, if reactive replanning still feels laggy in
   play: the ladder only reacts after a 3-4s detection window. Cheap version is
   a `World` obstacle-version counter (bump on building placed/completed/
   destroyed) + walking the remaining waypoints against `passable_planning`
   only when it changed — µs versus a 193 µs full search. Deliberately not
   built yet; the reactive ladder was enough to take Industrial to 100%.
4. Villager over-saturation on some maps (high `idle_vil`).
5. Per-age War/Scientific profiles (eras past Industrial are still thin).

**Diagnostic tooling already in place** (`tools/headless_runner/main.cpp`):
- `--dump-maps <dir>` — auto-writes a BMP of any match where a team ends at era 0
  (stuck base = red, healthy = white, farms = lime, berries = red, wood = dark
  green, oil = black, iron = grey, fish = cyan, water = blue). Convert to PNG:
  PowerShell `Add-Type -AssemblyName System.Drawing; ([System.Drawing.Image]::FromFile($bmp)).Save($png,[System.Drawing.Imaging.ImageFormat]::Png)`.
- `--trace-seed <seed>` — per-5-min trajectory (gathered food/wood, villager
  food/wood/build split) for that one match.
- `trace_stuck_villagers()` auto-prints each stuck civilian's target / carry /
  path / `stall_strikes` at match end.
- Repro: `headless_runner --tournament 40 --ticks 72000 --seed 7000 --jobs 12 --allied --trace-seed 7024`.

**Where today's session left the rates** (allied, seeds 7000/8000): Industrial
~99-100%, War ~77%, Scientific ~60-64%. Session wins, all landed & tested: (1)
age-ups always enqueue at the FRONT of the base queue (fixed a hard pop-cap
deadlock; War 42.5%→80%); (2) shallow production queues + factory/airbase oil-hold
while banking; (3) oil/iron nodes raised 200→800 per node (`world.cpp
resource_kinds`); (4) prereq-building pushes + wider `ai_build_spot` placement
(Sci ~20%→60%); (5) food-emergency now keeps building farms while starving
(`farm_count < desired_farms`, was `== 0`).

## Next areas

- **Set up `ai_variant` A/B gating** so tuning is measurable (see the
  measurement caveat above) — the single biggest process gap.
- Villager over-saturation on some maps (high `idle_vil`): the villager
  goal can exceed what local food sources employ.
- Per-age War/Scientific profiles (the eras past Industrial are still thin).
- Reactive counter-teching / scouting that actually informs decisions
  (currently the AI is omniscient but doesn't adapt composition to the
  enemy's).
