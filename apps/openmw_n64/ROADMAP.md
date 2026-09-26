# OpenMW on the Nintendo 64: Roadmap

> **Superseded.** [PLAN_ALL_ON_N64.md](PLAN_ALL_ON_N64.md) is the current plan:
> everything runs on the console, including OpenMW's own game code, using
> the flashcart's SDRAM as virtual memory. This PC-baker design is its
> fallback if the kill criteria there fire. Its §3 memory estimate for
> OpenMW's code (4.3–4.9 MB) was also too low: the measured linked size is
> 15.5 MB without Lua and 27.5 MB with it.

*File: `apps/openmw_n64/ROADMAP.md`. Status: superseded (see above); was the plan of record, revised after four independent reviews (RAM, CPU, completeness, data/tooling). Every number here is an estimate until a milestone measures it. When a measurement disagrees with this document, the measurement wins and this file gets updated.*

---

## 1. Verdict

### The honest summary

- **Most of OpenMW will not run on the N64. It will run on your PC.** Earlier drafts planned to compile OpenMW's gameplay code (mwworld, mwclass, mwmechanics, mwscript, mwdialogue) unchanged into the ROM. The reviews rebuilt the memory budget from measured object sizes. That design comes to about **11 MB of RAM in Balmora on an 8 MB console**, and the code alone would be about 4.3–4.9 MB (see §3). It does not fit. The problem is structural, not something tuning can fix:
  - OpenMW's record stores and cell stores use large per-record maps.
  - Record ids are interned strings, and the intern table only ever grows.
  - The ESM reader and writer pull in all of iostream and locale.
  - Every actor in the 3x3 grid gets a full simulation object.
- **The plan of record is therefore "OpenMW bakes, OpenMW judges, a small N64 engine plays":**
  - On your PC, OpenMW's own unmodified loaders, script compiler, NIF reader and collision code turn *your* Morrowind Data Files into compact N64 packs (the **baker**).
  - On the N64, a thin native engine runs those packs. It runs OpenMW's **script virtual machine (`components/interpreter`) unchanged**, executing bytecode from OpenMW's own compiler. Its game rules (combat, magic, AI, animation state machine, dialogue selection, levelling) are **copied from OpenMW with provenance comments** and each one is tested against the original on the PC.
  - Desktop OpenMW acts as the **oracle**: the reference implementation whose traces and saves the N64 must match.
- **OpenMW source edits stay near zero** (a cap of 300 lines outside the N64 folders), which meets your "modify as little as possible" goal. Code reuse happens on the PC and in the tests, not in the ROM.
- **Time is the biggest risk.** At 12 hobby hours a week, the vertical slice (boat → chargen → Seyda Neen → Balmora → Caius → save on real hardware) is roughly **10+ years** of work. The full main quest is about **20 years** (§8). At 25 hours a week those numbers roughly halve. The slice is the realistic product. Treat the full game as a stretch goal that needs contributors.
- **One cheap check on the old design stays.** M1 includes a 2-week **link probe** of the full-reuse design. If the real linked size somehow comes in under 2.6 MB, that design gets reconsidered. We expect it to fail. Its real job is to produce the list of OpenMW `.cpp` files that are cheap enough to link unchanged.

### What "fully playable" means here (acceptance test at the end of M12)

**Setup**
- A real N64 with an Expansion Pak and a SummerCart64 (the reference cart). EverDrive X7 or 64drive are verified second.
- The player runs the baker on their own PC against their own **English GOTY** `Morrowind.esm`. The baker hash-checks known builds and warns on anything else.
- The baker writes `personal.z64` (at most 0x03FF0000 bytes, about 63.9 MB) plus an `sd:/omw64/` folder.
- The player copies both to the SD card and boots through the flashcart menu (save type "none", 64DD off).
- Nothing derived from Morrowind is ever distributed.

**The test**
- One save lineage from a new game to the destruction of the Heart and the ending.
- No debug teleports, at least 20 save/load cycles, and a 4-hour soak with no crash.
- At 20 checkpoints, the N64 save is exported to `.omwsave` by the baker's converter, loaded in desktop OpenMW with vanilla `Morrowind.esm`, and its journal and quest state match.

**What works**
- Main menu and full character creation (the vanilla CharGen scripts run as baked bytecode).
- Every interior and exterior of Vvardenfell.
- Walk, run, sneak, jump, swim, levitate and water-walk.
- All travel: silt strider, boat, guild guides, propylons, Mark/Recall and both Interventions.
- Weather and day/night.
- Dialogue with persuasion and services; the journal, books and global map; all vanilla quests; crime, guards and jail.
- Melee, ranged and spells; potions, scrolls and enchanted items; diseases, Corprus and vampirism.
- Inventory, containers, barter, alchemy, enchanting, spellmaking, repair and recharge.
- Skill use, training and level-up.
- SFX, music, and voice with subtitles.

### What is cut or reduced, and why

| # | Cut / reduction | Why |
|---|---|---|
| 1 | **No Tribunal or Bloodmoon** (stretch goal after M13) | Morrowind.esm alone fills about 61 MB of the 64 MB ROM |
| 2 | **No Lua and no .esp mods** | Vanilla needs only MWScript. OpenMW's built-in Lua rules are reimplemented in C++. Real Lua costs about 2.3 MB of RAM and about 11 ms per frame |
| 3 | **Frame rate: 15 fps target, 12 fps floor** in towns and populated interiors; 20 fps only in small interiors | The CPU is the bottleneck (§3). Frames snap to vblank multiples: 20 / 15 / 12 fps |
| 4 | **Crowds:** at most **8 simulated actors** near the player (4 at full rate, 4 at half rate). Everyone else is **dormant**: frozen, with no AI or stats updates, until they come within about 2,048–3,072 units, when their timers catch up | VR4300 CPU time per actor. Distant guards and witnesses react late or not at all, and distant NPCs don't walk their routes. Followers, escorts, summons and combatants always get priority |
| 5 | **Exterior cell borders may pause briefly** (Xbox-style "Loading" icon, ≤0.7 s) if the incremental loader can't keep hitches under 100 ms (a kill criterion in M5) | Loading a town cell's actors is expensive, and libdragon threads cannot time-slice |
| 6 | **View distance:** full detail only in the player's cell, baked LOD for the 8 neighbours, fog at about 1 cell | Triangle budget (about 3,500 per frame) and RAM |
| 7 | **Graphics:** 320x240, 16 bpp, triple-buffered. Meshes decimated about 3–4x. Textures 64x64 CI4, 32x32 or 64x32 CI8, 32x32 RGBA16, 64x64 I4/IA4. Statics lit by baked vertex colour. 1 bone per vertex. **Terrain uses hard texture borders** (one LTEX tile per 4x4 block, softened by vertex colour). No shadows, reflections or post effects; particles capped at 192 | 4 KB TMEM, RSP/RDP throughput, ROM size |
| 8 | **UI:** controller only, full-screen screens. No hover tooltips (an info panel replaces them), no 3D inventory doll, no console, no in-text hyperlinks (topics are still learned). The local map is a text list with Detect markers | 320x240 and no mouse |
| 9 | **Physics:** actors are cylinders against baked floor, wall and ceiling polygons plus heightfields, using OpenMW's step, slope, gravity, fall-damage and swim rules. Animated collision is simplified to boxes. Pathgrid A* replaces the navmesh | No Bullet on the N64 |
| 10 | **Audio:** 16–24 voices, no reverb. SFX sample rate is chosen by the baker to fit ROM. Music and voice come from SD | ROM size, RSP time |
| 11 | **All Bink videos** (intro, ending, script `PlayBink`) become skippable still cards | No video decoder |
| 12 | **Saves require an SD flashcart** and use a compact native format. A PC tool converts them to and from `.omwsave` | Cartridge save media are 10–100x too small |
| 13 | **Some spell visuals are approximated.** Night Eye and Blind change ambient and fog colour, Light adds a point light, Chameleon uses alpha, Detect effects show on the map list | No full renderer |

---

## 2. Architecture

### Who runs what

| Piece | Runs on | OpenMW code used | How |
|---|---|---|---|
| Record loading, merge, `validate()` fix-ups | PC baker | `mwworld/esmstore`, `components/esm3`, `esm`, `vfs`, `bsa` | Linked unmodified (openmw-lib) |
| Compact N64 records (dense ids) | PC → ROM | ESM3 structs as the source | New encoder plus a 100% round-trip checker against ESMStore |
| Script compilation | PC baker | `components/compiler` | Unmodified. Output is OpenMW `Interpreter::Program` bytecode |
| Script execution | **N64** | **`components/interpreter` (1,581 lines) unchanged** | N64 implements `Interpreter::Context` and the opcode handlers, with the same opcode numbers |
| Dialogue selection | N64 native evaluator on pre-decoded conditions | `mwdialogue/filter.cpp` as the **PC oracle** | Differential tests on thousands of (NPC, topic, state) cases |
| Mechanics formulas (hit, damage, armor, spell cost/chance, alchemy, enchant, levelled lists, persuasion, prices, skill progress, level-up, autocalc) | N64 | `mwmechanics/*.cpp` | **Copied with provenance.** Each gets a host test calling the original in openmw-lib |
| Animation state machine and CharacterController | N64 | `mwrender/animation.cpp`, `mwmechanics/character.cpp` | Copied and adapted to the native actor interface. Traces compared with desktop |
| Built-in Lua rules (onHit, magic effects, skill use, crimes, music) | N64 C++ | `files/data/scripts/omw/*.lua` as the spec | Reimplemented, trace-tested |
| NIF → Tiny3D meshes, LOD, vertex light | PC baker | `components/nif`, `nifosg`, `resource`, `sceneutil` | Unmodified (no GL context needed) |
| Collision | PC baker → N64 | `resource` / `nifbullet` (`forEachBulletObject`), `mwphysics` rules | Baker unmodified. Movement rules copied |
| Terrain | PC baker | `components/esmterrain` | Unmodified |
| KF animation sampling | PC baker | `nifosg` controllers, `keyframemanager` | Unmodified |
| Rendering (Tiny3D), UI (rdpq), audio (mixer/wav64), input | N64 | — | New |
| World model (refs, cells, 3x3 grid, dormant actors) | N64 | — | New, native, dense 32-bit ids (6-bit type + 26-bit index) |
| Saves | N64 native → PC converter | `components/esm3` ESMWriter plus save records | Converter linked unmodified on the PC |
| Behaviour oracle | PC | Desktop OpenMW (trace build on a separate branch) plus the **desktop twin** (the N64 runtime compiled for x86 with a null renderer) | Traces diffed in CI |

### PC side: `apps/n64baker`

This is a desktop CMake target in the fork, linking unmodified openmw-lib and components.

**Build**
- `BUILD_OPENCS/LAUNCHER/WIZARD=OFF`.
- Bullet comes from OpenMW's own `OPENMW_USE_SYSTEM_BULLET=OFF` path (verify it is double precision). Ubuntu's `libbullet-dev` is single precision and fails OpenMW's check.
- CI builds reuse the fork's `.github/workflows/push.yml` (Ubuntu) and `windows.yml` (prebuilt vcpkg deps).
- Release zips bundle Windows and Linux builds of `mksprite`, `audioconv64`, `mkdfs` and `n64tool` plus the engine ELF, so players never install libdragon.

**Stages** (each cached by content hash, each printing a budget report)
1. **Census.** Records per type; INFO count and text bytes; refs per cell; scripted refs per 3x3 grid in Balmora, Vivec and Ald'ruhn; running global scripts; opcodes used; NIF tris per cell; LAND bytes; SFX seconds; activators with keyframe controllers; `PlayGroup` on non-actors; PGRD sizes.
2. **Records.** Compact structs with dense ids, plus side tables:
   - marker table (divine, temple and prison markers);
   - exterior cell table and interior name list (for `ShowMap`, `FillMap` and named `PositionCell`);
   - key flags, autocalc spell answers, the soundgen map;
   - GMST array and fallbacks;
   - l10n strings;
   - RefNum map (keeps Morrowind.esm RefNums for save conversion).
3. **Scripts.** Every SCPT, and every INFO result script per speaker-script variant, compiled by `components/compiler`. Each program's string literals are pre-resolved to dense ids.
4. **Dialogue.**
   - Each INFO's conditions pre-decoded to about 32 B;
   - per-speaker-signature candidate lists (static filters only);
   - link spans from KeywordSearch;
   - LZ4 text.
5. **Cell packs.**
   - Statics merged per material into s16 Tiny3D chunks with a BVH, 2 LODs and baked vertex light.
   - Dynamic refs (actors, items, containers, doors, lights, activators, markers, and anything a script, dialogue or C++ names) as compact records.
   - Collision:
     - statics merged into a floor/wall/ceiling grid;
     - **per-dynamic-ref collision instances with owner ids**;
     - door boxes and pick bounds;
     - pathgrids.
6. **Terrain.**
   - 17x17 render mesh, one LTEX index per 4x4-quad block, draws grouped by LTEX (typically 3–8 per cell);
   - a 5x5 LOD mesh plus one 64x64 CI4 composite per cell for the LOD ring;
   - a 33x33 int16 collision grid, delta- and LZ4-compressed;
   - the WNAM global map.
7. **Actors and animation.**
   - Canonical skeletons, with BODY parts bound to them;
   - each part merged to a single material, and a per-race/sex far impostor;
   - 30 Hz quantized clips, text keys, velocity tables;
   - **ObjectAnimation tracks** for animated activators, and **box tracks** for animated collision.
8. **Textures.** DDS decoded, then `mksprite` into uniform 2 KB slots, alpha preserved. Icons 32x32 CI4.
9. **Audio.** SFX as VADPCM in ROM (baker-chosen rate); voice and music as ULC on SD, with an index.
10. **Pack.** `mkdfs` + `n64tool`. The ROM must be ≤ 0x03FF0000 so ares ISViewer logging still works. Pad to a power of two only for hardware release builds.
11. **Converter** (`n64baker save-export` / `save-import`). Native save ↔ `.omwsave` targeting vanilla `Morrowind.esm`.

**Budgets are enforced per cell:** player-cell geometry ≤ 64 KB, LOD ≤ 12 KB, collision ≤ 40 KB, live refs ≤ 160 per cell and ≤ 1,000 per 3x3 grid, ≤ 64 textures, ≤ 130 static draws and ≤ 2,000 static tris per viewpoint sample. An over-budget cell is decimated or dropped a tier, and the report records it.

### N64 side: `apps/openmw_n64`

- **Build:** C++20, libdragon preview 39d0d60, Tiny3D 73d822f, `-Os`, gc-sections, NDEBUG, **`-fno-exceptions`** (drops about 25% EH overhead), and no doubles in N64 translation units. Compat `<format>` (snprintf-based) replaces libstdc++'s. The link map is checked in CI: **no `locale_init.o` or `ios_init.o` may appear.**
- **Storage layer:**
  - `RomFs` (DFS over PI DMA; the main data path during play);
  - `SdFs` (FatFs; **only behind fades, in menus, and in bounded 8 KB music slices**, because libcart SD reads busy-poll the CPU);
  - `UsbLog` / null;
  - `HostFs` (desktop twin).
  - One **PI-bus owner lock** covers libcart and `dma_read`.
- **Frame loop:** input → scripts → actors (8 simulated, the rest dormant) → physics → animation → world update → render (Tiny3D) → HUD (rdpq). Audio runs on a kthread woken by the AI interrupt. libdragon's kernel does switch to a higher-priority thread on an interrupt wake-up; M0 verifies this.
- **Incremental loader:** a main-thread job queue with at most 2 ms per frame. It DMAs compact refs from ROM and instantiates at most 1 actor per frame. It never touches SD.
- **Overlays (DSOs):** none by default. They are only a lever if code overruns (§5). If used, they load into a buffer reserved at boot through a `dlopen` wrapper, and the save writer always stays resident.

---

## 3. Budgets

### RAM, worst case (8,192 KB)

The worst case is Balmora exterior 3x3 at noon, in a fight with one summon, rain, music, and a grid shift under way. CI also runs three named peak scenarios with the same gate: a save in Balmora, barter with a book merchant, and combat with summons during a grid shift.

The **Path A** column is the old full-reuse design, re-baselined by the RAM review with its own fixes applied (typed RefIds, baked GMST/DIAL). It is shown so you can see why it was dropped.

| Item | Path A (reuse), KB | **Path B (plan), KB** | Notes |
|---|---:|---:|---|
| System, .bss, malloc headers, EH pool | 240 | **160** | Measure the EH pool at boot with `sys_get_heap_stats` |
| Code: platform (libdragon, FatFs/libcart, Tiny3D, mixer/VADPCM/ULC, libstdc++/newlib) | 900 | **540** | A keeps iostream/locale because ESMReader/Writer are built on streams |
| Code: OpenMW compiled unchanged | 3,150 | **120** | B: `components/interpreter`, `esm` RefId, misc pieces from the M1 probe list |
| Code: new N64 code | 700 | **1,940** | B: native world, opcodes, copied rules, renderer, physics, UI, audio, saves |
| DSO slot + main symbol table | 506 | **0** | B uses DSOs only as a lever |
| Framebuffers 3 x 320x240x16 + Z-buffer | 600 | **600** | Triple buffering lets the CPU run ahead of the RDP |
| rspq/rdpq + Tiny3D matrices; stacks (64 + 16 + 8) | 168 | **168** | |
| Audio buffers + resident SFX bank | 208 | **176** | |
| Records: tables + id index + record cache | 1,146 | **448** | B: compact structs, arena per cell, no string interning of record ids |
| Dialogue window + script bytecode LRU, locals, interned script literals | 330 | **224** | B: 32 B condition records. Book and INFO text are paged from ROM, never kept |
| Live refs (3x3 grid + 1 cached interior) + actors | 964 | **224** | B: about 64 B per ref; 8 simulated actors at about 6 KB each; dormant actors about 256 B each |
| Skeletons (8 drawn, about 18 KB) + clip cache | 256 | **256** | |
| Static geometry (player cell + LOD ring) + actor/item mesh cache | 624 | **624** | An interior gets the whole 400 KB static line |
| Texture slots (128 x 2,080 B) | 272 | **272** | |
| Terrain + collision + pathgrids | 376 | **376** | Includes per-ref collision instances |
| Sky, water, particles; UI fonts and icons; streaming, LZ4 and save buffers | 240 | **240** | The save record buffer shares the streaming buffers |
| Enforced fragmentation reserve | 384 | **384** | CI fails if heap high-water exceeds 7,808 KB |
| **Total** | **≈ 11,064 (does not fit)** | **6,752** | |
| **Unallocated** | — | **1,440** | First use: code overrun (the new-code estimate may be 50% low). Caches may grow only after the M9 soak |

### Frame budget

**Targets.** Frame time is the **serial CPU total, rounded up to the next vblank multiple** (≤50 ms → 20 fps, ≤66.7 ms → 15 fps, ≤83 ms → 12 fps). The RSP and RDP overlap the CPU but must stay below it.

| Scene | Target | Floor |
|---|---|---|
| Small interiors (Census Office class) | 20 fps | 15 fps |
| Populated interiors (Vivec plazas, guild halls) | 15 fps | 12 fps |
| Wilderness | 15 fps | 12 fps |
| Towns (Balmora, Vivec Foreign Quarter exterior) | 15 fps | 12 fps |
| Any hitch | ≤100 ms (test failure above) | Exterior border pause ≤0.7 s only if the M5 kill criterion fires |

**CPU (VR4300 93.75 MHz), Balmora exterior at noon, one fight, rain.** These are estimates; M0 re-baselines them from the measured ares-to-hardware ratio for C++ workloads.

| Work | ms |
|---|---:|
| Input and loop | 0.5 |
| Scripts: every active-cell script, far refs (>3,072 units) at 5 Hz, O(1) explicit-ref lookup via baked id → (cell, ref) | 6.0 |
| Player controller, 1st-person rig, stats | 1.5 |
| Actors: 4 full-rate x 1.5 + 4 half-rate x 0.75 + dormant bookkeeping | 9.3 |
| Path requests: ≤1 binary-heap A* per frame, queued | 1.0 |
| LOS: ≤4 rays per frame, cache with a conservative default on miss | 2.0 |
| Physics: 8 movers + sub-steps (more steps as fps drops) | 3.5 |
| Skeleton sampling and bone matrices, 8 x 0.4 | 3.2 |
| Render submission: about 210 draws (below) | 12.0 |
| HUD and text | 1.0 |
| Audio: 3D params, mixer_poll wait, music SD slice (amortised) | 4.0 |
| Weather, sky, particles | 2.5 |
| Incremental loader (ROM DMA, LZ4, ref and actor instantiation) | 2.0 |
| World update, doors, heap | 1.5 |
| **Total** | **≈ 50** |

On paper that is 20 fps. We **target 15** because every row is extrapolated and the reviews found the earlier draft about 1.5–2x optimistic. Small interiors come to about 25–30 ms.

**Draw budget by category** (the baker enforces the static line; the runtime counts the rest):

| Category | Draws |
|---|---:|
| Statics (player cell + LOD ring) | ≤130 |
| Terrain (grouped by LTEX, plus 8 LOD composites) | ≤24 |
| Actors (2 near x ≤6 merged parts, 6 far x ≤2) | ≤24 |
| Sky, water, weather, particles | ≤16 |
| First-person arms and weapon | ≤4 |
| HUD | ~10 |
| **Total** | **≈210** |

**RSP** (≈21 ms): 3,500 tris x 3.8 µs = 13.3, skinning 1.5, mixer 16–24 channels 3.0, ULC music 1.5, particles 1.0, HUD 1.0.

**RDP** (≈28 ms):
- opaque fill at 2.5x overdraw and about 15 Mpx/s: 12.8;
- blended rain, particles and water: about 6;
- triangle setup: 3.5;
- about 200 TMEM/TLUT loads: 4;
- Z clear and HUD: 2.

**Menus** (dialogue, inventory, journal) pause the world. A topic click must refresh within 300 ms (goal 150). A greeting must appear within 500 ms (goal 300); the voice line is read whole from SD during that time.

**Transitions.**
- Door: behind a 0.5–1.5 s fade (SD cell-state reads and writes happen only here).
- Exterior grid shift: incremental, at most 2 ms of CPU per frame.
- Save: measured in M0/M9; the target is ≤10 s, the kill threshold 20 s.

### ROM budget (provisional until the M2 census; each line is a baker-enforced cap)

| Content | MB |
|---|---:|
| Engine ELF | 2.7 |
| Compact records, id tables, cell/marker tables | 3.0 |
| Dialogue (text LZ4, conditions, candidates, links) | 4.0 |
| Books and scrolls text | 1.2 |
| Script bytecode and literal tables | 0.6 |
| Cell refs (dynamic only) | 2.5 |
| Static meshes and LODs | 12.0 |
| Actor, creature and item meshes | 4.0 |
| Textures | 9.0 |
| Icons | 1.5 |
| Collision | 7.0 |
| Terrain (LTEX tiles, block indices, heights, LOD composites) | 5.0 |
| Animation | 3.0 |
| SFX (VADPCM, rate chosen to fit) | 5.0 |
| Fonts and UI | 0.5 |
| **Total** (hard cap 63.9) | **61.0** |

On SD: voice about 34–50 MB, music about 8–15 MB, and saves (one preallocated file per slot with an internal cell-delta index, atomic commit via `f_rename`). Tell players to **format the card as exFAT**, because FatFs here has no fast-seek.

---

## 4. Milestones

| # | Milestone | Weeks | Cumulative | Decisive measurement |
|---|---|---:|---:|---|
| M0 | Hardware truth, harness, storage layer | 4 | 4 | Tris/ms, fill, SD stall, CPU ratio |
| M1 | Fit test, native core, first Tiny3D picture | 6 | 10 | Path A size; Balmora CPU proxy |
| M2 | Baker v0: census, compact records, round-trip | 6 | 16 | Census; ROM fit |
| M3 | Renderer from baked slice packs | 8 | 24 | Hardware fps, statics only |
| M4 | Walk the slice: collision and movement | 7 | 31 | Replay accuracy; fall damage |
| M5 | Live world: refs, scripts, doors, incremental loading | 14 | 45 | Grid-shift hitch; script CPU |
| M6 | Actors: animation, AI, scheduling | 14 | 59 | Per-actor CPU; animation fidelity |
| M7 | Talk and create a character | 13 | 72 | Dialogue oracle match; latency |
| M8 | Fight, cast, loot, trade, rest, hear it | 12 | 84 | 50-hit trace match |
| M9 | Saves and Balmora: **vertical slice done** | 9 | 93 | Heap soak; desktop save match |
| M10 | All of Vvardenfell | 16 | 109 | Pack ≤63.9 MB; worst cells ≥12 fps |
| M11 | Every system | 22 | 131 | Scenario suite |
| M12 | Main quest and hardening | 18 | 149 | Acceptance test |
| M13 | Release packaging | 6 | 155 | A stranger builds and plays |

A playable "walking simulator" of the slice exists at week 31. It is a good motivation checkpoint and fine to publish (engine only, no data).

---

### M0: Hardware truth, test harness, storage layer (4 weeks)

**Goal.** Measured numbers and automated gates before any engine work.

**Work**
- **Hardware setup:** SummerCart64, Expansion Pak, SD card, UNFLoader USB logging (`main.cpp` already calls `debug_init_usblog`).
- **`VERSIONS` file, checked at build time:** OpenMW 3ee798e9 + port commit, libdragon 39d0d60, Tiny3D 73d822f, ares af4cbb04, gopher64 (pin a commit).
- **Dev container:** extend `docker/Dockerfile.ubuntu` with libdragon, Tiny3D, the pinned ares/gopher64, and OpenMW deps with bundled Bullet. Add a one-page setup guide.
- **Microbenchmark ROM**, results committed to `docs/hardware.md`:
  - Tiny3D textured, fogged, Z-buffered tris/ms **and CPU µs per draw** at 130 and 300 draws;
  - RDP fill at 1x, 2.5x and 4x overdraw, with and without blended sprites;
  - PI DMA MB/s for 4 KB and 64 KB reads;
  - SD read/write KB/s **and the stall per 8 KB chunk** on every cart you can borrow;
  - mixer (16–24 channels) + ULC RSP time and **mixer_poll blocking time**;
  - that a kthread woken by the AI interrupt preempts the main loop;
  - a CPU-bound C++ workload (map churn, virtual calls) timed in ares and on hardware, giving **the ares-to-hardware ratio for CPU**, not only for triangles;
  - the existing 400-crate cell.
- **Harness:** port Pak's `tcl/tools/ares_test.tcl` to `tools/n64test/ares_run.py` (about 250 lines: Xvfb, `SDL_AUDIODRIVER=dummy`, stable-frame detection, pixel probes, boot timeout, ImageMagick RMSE goldens).
- **Log protocol:** `OMW64 READY/FRAME <cpu_us> <rsp_us> <rdp_us> <tris> <draws> <heap_hw> <largest_free>/ASSERT/TRACE/DONE`, a hitch detector, and a scripted input timeline.
- **Storage interface** (`RomFs`, `SdFs`, `UsbLog`, `HostFs`) plus the PI-bus lock.
- **SD emulation:** ares has no flashcart or SD model. Use **pinned gopher64 (which emulates SC64 + SD)** for SD tests, or add a minimal SC64 SD register model to the pinned ares. Every later exit criterion names its emulator or "hardware".
- **Two CI tiers:**
  - **Public:** CC0 example-suite, engine and baker builds, ledger gate, `openmw_tests`.
  - **Private:** a self-hosted runner on your machine, with Data Files, packs, goldens, traces and saves in `~/omw64-private`. Nothing from it is ever committed or uploaded.
- **Desktop OpenMW trace build** on a separate `trace-oracle` branch, never shipped. It logs positions, globals, locals, journal indices, chosen INFO ids, rolls with a fixed `Misc::Rng` seed, and text keys.

**OpenMW reused:** the existing viewer (as test subject); desktop OpenMW.

**New code:** about 1,500 lines.

**Exit criteria**
- `make test` boots the viewer headless in ares, parses FRAME lines and passes an example-suite golden.
- An SD file write/read round-trip passes in gopher64 and on hardware.
- `docs/hardware.md` holds SC64 numbers for every benchmark.
- The M0 kill criteria (§5) are evaluated in writing.
- A 5-minute oracle trace of a Morrowind new game is recorded locally.

### M1: Fit test, native core, first Tiny3D picture (6 weeks, three tracks)

**Goal.** Settle the Path A vs Path B question with a real link, measure gameplay CPU on hardware early, and have something on screen.

**Work**
- **Track A, link probe (time-boxed to 2 weeks).**
  - Link mwworld, mwclass, mwmechanics, mwscript and mwdialogue with abort() stubs at release flags.
  - Report the **real ELF** size (text + rodata + eh_frame + gcc_except_table + data) per archive and per file.
  - Write the **"linkable unchanged" list**: OpenMW files whose link closure does not drag in Ptr, ESMStore or streams.
  - **Rule:** Path A is reconsidered only if its resident code is ≤ 2,600 KB. Otherwise Path B is confirmed, which we expect.
- **Track B, native core.**
  - Dense id scheme and compact structs for the slice's record types.
  - `components/interpreter` linked unchanged with an N64 `Interpreter::Context`.
  - Compat `<format>` covers `miscopcodes.hpp`. `defines.cpp` uses `std::ostringstream`: shim it or copy it.
  - Bounded interning of script literals.
  - Write `docs/runtime-design.md` (world model, ids, dormant actors, loader, save format).
- **Track B, Balmora CPU proxy ROM** (the CPU review's key request).
  - 8 simulated + 40 dormant synthetic actors running copied AiWander steering, hit chance, and binary-heap A* on a synthetic 200-node grid (the real Balmora PGRD after M2).
  - 60 running scripts.
  - Timed with `get_ticks` on SC64.
- **Track C, viewer.** Port the viewer from libdragon GL to Tiny3D using example-suite NIFs converted on the PC (T3DMData → writeT3DM), a 128-slot texture LRU and Tiny3D fog. This removes the s10.5 quarter-scale workaround and the GL_FOG bug.

**OpenMW reused:** `components/interpreter`, `esm` RefId, `misc` (N64); `compiler`, `nif`, `nifosg` (PC); the whole gameplay tree for the probe.

**New code:** about 4,000 lines.

**Exit criteria**
- (a) The size report and Path A/B decision are committed.
- (b) A script compiled by desktop OpenMW runs in the ROM, and its globals trace matches desktop exactly.
- (c) The CPU proxy runs in ≤25 ms per frame on SC64.
- (d) The Tiny3D viewer renders an example-suite interior matching its golden in ares, and prints its fps on hardware.
- (e) The link map contains no `locale_init.o`.

### M2: Baker v0: census, compact records, round-trip (6 weeks)

**Goal.** Faithful N64 data from the player's files, and every estimate replaced with a measured count.

**Work**
- Create the `apps/n64baker` target (see the build notes in §2).
- Load through the real ESMStore (load, setUp, validate).
- Hash-check the Data Files.
- Stage 0 census (the full list in §2). Commit the numbers only, never the data.
- Update §3's RAM and ROM tables from the census.
- Compact record encoder with a **round-trip checker**: decode every compact record and compare it field by field with ESMStore.
- Side tables: marker, exterior cell, interior names, key flags, autocalc, soundgen, GMST array, `esmfallbacks.lua` ported to C++, the mwiniimporter fallback table, l10n, the RefNum map.
- Dynamic-only cell refs; markers and anything named by scripts, dialogue or C++ are always kept.
- Personal ROM builder (cap 0x03FF0000; pad only release builds).
- **N64 side:** `RomFs` reader, a 32 KB resident id index with leaves paged from ROM, and the record cache.

**OpenMW reused:** openmw-lib `esmstore`/`store`, `components/esm3`, `esm`, `vfs`, `bsa`, `files`; `apps/mwiniimporter`; `mwmechanics/autocalcspell` (PC).

**New code:** about 5,000 lines.

**Exit criteria**
- (a) The round-trip check passes on 100% of records (Morrowind privately, example-suite publicly).
- (b) An N64 ROM loads 1,000 random records, and its dump is byte-identical to the host dump.
- (c) The census is committed, the M2 kill criteria are evaluated, and the projected pack is ≤ 63.9 MB at some quality tier.

### M3: Renderer from baked slice packs (8 weeks)

**Goal.** Every slice cell renders from baked data at target speed on hardware.

**Work**
- **Baker, meshes:** NIF → merged static chunks per material, BVH, meshoptimizer decimation into 2 LODs, vertex light from AMBI/LIGH and the sun.
- **Baker, textures:** tiers as in §1, via mksprite.
- **Baker, terrain:** the new scheme (LTEX per 4x4 block, 5x5 LOD with 64x64 CI4 composite, 33x33 heights).
- **Baker:** per-cell budget enforcement and report.
- **Baker, hero surfaces:** port the chroma64 UV-page clipper (about 100 lines of C++) for a handful of surfaces where 64x64 is visibly too blurry.
- **Slice cells:** prison ship, Census Office (both floors), Seyda Neen 3x3, Arrille's Tradehouse, Balmora 3x3, Caius Cosades' house. Add **Vivec Foreign Quarter Plaza** as a stress cell.
- **N64:** model cache, texture LRU, BVH and frustum culling, sort by texture, camera-relative matrices, LOD ring, the incremental loader skeleton. Delete the GL code.

**OpenMW reused:** `nif`, `nifosg`, `resource`, `sceneutil`, `esmterrain` (PC).

**New code:** about 6,500 lines (replacing about 1,080 lines of GL code).

**Exit criteria** (SC64 over USB, free-fly camera, statics only)
- Census Office ≥ 25 fps.
- Seyda Neen exterior ≥ 20 fps.
- Balmora ≥ 15 fps.
- Vivec plaza ≥ 15 fps.
- The slice pack is ≤ 12 MB.
- 6 goldens pass in private CI and 3 example-suite goldens pass in public CI.

### M4: Walk the slice: collision, movement, doors (7 weeks)

**Goal.** The player walks the slice with vanilla-like step, slope, jump, swim and fall rules.

**Work**
- **Baker:** collision via `Resource::forEachBulletObject`:
  - static triangles merged and classified into a 256-unit grid;
  - **per-dynamic-ref instances tagged with the owning ref**;
  - door boxes, pick bounds, actor half-extents.
- **N64 CollisionWorld:**
  - floor ray, cylinder push-out with step-up 34, slope 46°, ceiling clamp;
  - rays against polygons, heightfields and boxes;
  - sub-steps whenever displacement exceeds the actor radius;
  - enable, disable and move for dynamic instances.
- **Movement rules** copied from `movementsolver`, `stepper`, `handleJump`, **and the `mtphysics.cpp` sync block**:
  - fall-height accumulation and landing;
  - standing-on / on-ground / on-slope / walking-on-water;
  - inertia.
- **Input:** stick = look, C = move/strafe, A = activate, B = jump, Z = use, R = weapon/magic, L = sneak, D-pad = quick keys, Start = menu.
- First-person camera.

**OpenMW reused:** `nifbullet`, `resource/bulletshape` (PC); `mwphysics` rules (copied); `misc/constants.hpp`.

**New code:** about 4,000 lines.

**Exit criteria**
- A scripted replay from the ship deck to the Seyda Neen lighthouse hits every waypoint within 64 units of the desktop path in 10 of 10 runs (ares).
- A scripted drop takes fall damage within 5% of desktop.
- Landing from levitation causes no damage.
- Player physics costs ≤ 2 ms per frame on hardware.

### M5: Live world: refs, scripts, doors, incremental loading (14 weeks)

**Goal.** The slice runs as a world. Scripts run from baked OpenMW bytecode.

**Work**
- **Native world model:** cells, the 3x3 grid, refs, the dormant actor state, and the per-cell arena. A Ptr-like handle is (type, index, generation).
- **Incremental loader:** grid shifts are spread over frames (≤ 2 ms each, ≤ 1 actor instantiated per frame). SD is only used behind fades.
- **Opcode handlers** for **every opcode used in the slice** (from the census list), with the same numbers and semantics as `apps/openmw/mwscript/*extensions.cpp`.
  - Explicit refs go through a baked id → (cell, ref) map with a negative cache keyed by grid generation.
  - Scripts on refs beyond 3,072 units run at 5 Hz with accumulated `dt`.
- **World features:**
  - activation, doors (including the CharGen scripted doors), containers, pickups;
  - **marker lookup** via the marker table;
  - `ShowMap`, `FillMap` and named `PositionCell` via the cell tables;
  - `GetStandingPC/Actor`, `GetCollidingPC/Actor`, `HurtStandingActor` and `HurtCollidingActor` via collision owner ids;
  - `PlayBink` shows a still card.
- **NativeLuaManager part 1:** activation and use queue, UI pause.
- **Desktop twin:** the runtime built for x86 Linux with a null renderer.

**OpenMW reused:** `components/interpreter` (N64); `compiler` (PC); opcode semantics copied from `mwscript`.

**New code:** about 12,000 lines.

**Exit criteria**
- Doors, containers and pickups work (inventory dump in the log), and CharGen reaches its menu gates.
- A lava activator using `HurtStandingActor` and a disabled scripted barrier behave as on desktop.
- The twin's globals and locals trace matches desktop over a scripted 5-minute run.
- On hardware, the worst grid shift in Seyda Neen and Balmora causes a hitch of ≤ 100 ms, and a door takes ≤ 1.5 s.
- Script CPU in Balmora is ≤ 6 ms per frame.
- Heap is within the §3 lines for every slice cell.

### M6: Actors: animation, AI, scheduling (14 weeks)

**Goal.** NPCs and creatures animate, wander, path, fight and collide with vanilla timing, within the CPU budget.

**Work**
- **Baker, animation:**
  - skeletons and 30 Hz clips per group and blend mask;
  - the Bip01 root at high precision;
  - text keys and velocity tables (including the AshVampire quirk);
  - the 1st-person rig;
  - BODY parts with left mirroring, merged to ≤ 6 materials per near actor, plus far impostors;
  - **activator/object keyframe tracks and animated-collision box tracks.**
- **N64 animation:** the Animation state machine (from `animation.cpp`) and CharacterController (from `character.cpp`), copied with provenance, over a random-access sampler with 4 blend masks. ObjectAnimation handles `PlayGroup` / `LoopGroup` on activators.
- **AI:** wander, travel, follow, escort, combat, pursue and flee, copied from `ai*.cpp`, with binary-heap A* behind a queue of ≤ 1 request per frame.
- **Scheduler:** 8 simulated (4 full-rate, 4 half-rate), everyone else dormant with timer and spell catch-up on wake. Actors following, escorting or summoned by the player, and actors in combat with the player, always come first.
- **Skinned Tiny3D drawing:** 8 drawn, distance-sorted, 2 LODs.

**OpenMW reused:** `mwmechanics` character, AI, pathgrid, stats and `animation.cpp` logic (copied); `nifosg` controllers and `keyframemanager` (PC).

**New code:** about 10,000 lines.

**Exit criteria**
- (a) For 25 groups, the twin's (group, key, time) traces match desktop within one sample, and root-motion speed is within 2%.
- (b) Fargoth, Arrille and the guards wander and greet, and mudcrabs roam.
- (c) On hardware, average per-actor CPU is ≤ 1.5 ms, 8 drawn actors cost ≤ 8 ms of RSP, and Balmora holds ≥ 12 fps (target 15).
- (d) A follower passes through a load door and keeps following.

### M7: Talk and create a character (13 weeks)

**Goal.** Complete vanilla CharGen, talk to NPCs, and record quest updates.

**Work**
- **rdpq UI toolkit:** panel, label, list, icon grid, tabs, bars, spinner, modal box, paged text, on-screen keyboard, info panel, toasts. OFL fonts via mkfont.
- **N64WindowManager:** GuiMode stack, interactive message boxes, fades, and a HUD.
- **Chargen screens:** name, race/sex, class (list, quiz or custom), birthsign, review. They are driven by the unchanged CharGen scripts' GUI opcodes.
- **Native dialogue evaluator** over the 32 B condition records, using the candidate lists, baked result-script lookup per (INFO, speaker script), link spans and a known-topic bitset.
- **Voice** read whole from SD during the topic delay.
- **Journal and books:** entries stored as (topic, info, day) indices; text paged from ROM.

**OpenMW reused:** `mwdialogue/filter.cpp` and `selectwrapper.cpp` (PC oracle, and copied semantics); `interpreter/defines.cpp`; `mwgui` `BookTextParser` and `journalviewmodel` logic (copied); the chargen question table.

**New code:** about 8,000 lines.

**Exit criteria**
- On hardware, with no debug help: new game → Jiub → every chargen menu → Sellus Gravius → leave the Census Office → Fargoth's ring quest appears in the journal.
- **2,000 sampled (NPC, topic, state) cases pick the same INFO id as desktop's Filter.**
- A topic click takes ≤ 300 ms and a greeting ≤ 500 ms on hardware.

### M8: Fight, cast, loot, trade, rest, hear it (12 weeks)

**Goal.** The core RPG loop, with vanilla numbers.

**Work**
- **NativeLuaManager part 2**, using `files/data/scripts/omw/*.lua` as the spec:
  - onHit: armor mitigation, armor skill use, condition wear, difficulty, stagger/knockdown, hit sounds;
  - magic effects: AoE, reflect, Lock/Open with trespass;
  - projectile hits;
  - skill use, level-up, jail;
  - the crimes wrapper;
  - actor death.
- **Projectiles:** swept spheres over CollisionWorld.
- **Inventory with a single `useItem` entry point** (used by quick keys too). It copies `InventoryWindow::useItem`: OnPCEquip/PCSkipEquip, broken-item refusal, beast-race boots and helmets, books, ingredients, repair tools.
- **Screens:** containers, pickpocketing, companion share, barter with haggling, magic list, quick keys, stats.
- **RestController**, copied from `WaitDialog`: sleep ambush roll and spawn, `GetPCSleep`/`WakeUpPC`, level-up after sleep.
- **Sound:** mixer backend (channel free-list, CPU gain and pan), the baked SFX pack, the soundgen table, footstep text keys.
- Minimal hit and spell particles, and a temporary point light.

**OpenMW reused:** `mwmechanics` combat, spellcasting, activespells, spelleffects, levelledlist and npcstats (copied, each with a host test); built-in Lua (spec); tradewindow haggle logic (copied).

**New code:** about 8,000 lines.

**Exit criteria**
- A scripted scenario (mudcrab kill, skill gain, loot, sell to Arrille, potion, Fireball, Open, caught stealing and fined, sleep, level-up) passes in the twin and in ares, and is repeated by hand on hardware.
- With a fixed seed, 50 consecutive hits match desktop's traces.
- An OnPCEquip-scripted item behaves as on desktop.
- A sleep ambush fires.
- A 3-actor fight holds ≥ 12 fps on hardware with no audio stutter.

### M9: Saves and Balmora: vertical slice complete (9 weeks)

**Goal.** The whole slice on real hardware, in one sitting.

**Work**
- **Native save:** one preallocated file per slot with an internal cell-delta index. Written only in menus or behind fades, with a progress screen, committed atomically with `f_rename`. The save writer stays resident.
- **PC converter:** `save-export` / `save-import` via ESMWriter, targeting vanilla `Morrowind.esm` RefNums.
- **Cell-state paging** into the slot file behind fades.
- **Travel:** silt strider, and the Seyda Neen → Pelagiad → Balmora road corridor (about 40 cells).
- **Soak test:** 30 transitions and 10 save/load cycles. Run the SD paths in gopher64 and on hardware.

**OpenMW reused:** `components/esm3` ESMWriter and save records (PC converter); travel pricing (copied).

**New code:** about 5,000 lines.

**Exit criteria** (on SC64)
- New game → chargen → Fargoth's ring → mudcrab fight and loot → sell at Arrille's → silt strider to Balmora (and separately the road on foot) → deliver the package to Caius (the A1_1 entry appears) → save → power off → load → keep playing.
- The exported save loads in desktop OpenMW with vanilla Morrowind.esm and gives the same state hash (stats, inventory, journal, globals, touched cells).
- Heap high-water is ≤ 7,808 KB in all four named peak scenarios.
- Growth is < 50 KB per transition, and the largest free block stays ≥ 512 KB.
- Balmora ≥ 12 fps (target 15), interiors ≥ 15 fps.
- Save and load each take ≤ 20 s (target 10).

**This is a finished, publishable product.**

### M10: All of Vvardenfell (16 weeks)

**Goal.** Every cell of Morrowind.esm within the pack, RAM and frame budgets.

**Work**
- **Full bake** with caps. Heavy-cell passes on the Vivec cantons, Ald'ruhn, Sadrith Mora, Ghostgate, Red Mountain and Dagoth Ur's citadel.
- **Streaming** prefetch by movement direction; fog per region.
- **Sky and weather:** logic copied from `weather.cpp`, plus the sky dome, clouds, sun, moons (phases kept for scripts), rain/ash/blight particles capped by screen area, and lightning.
- **Water:** the water plane, underwater fog, levitation, water walking.
- **Music:** playlists (from `music.lua`) streamed from SD in bounded 8 KB main-thread slices through a large RDRAM ring.
- **Global map:** from WNAM, with markers.

**OpenMW reused:** `esmterrain`, `terrain` (PC); weather and globalmap colour logic (copied).

**New code:** about 6,000 lines.

**Exit criteria**
- An automated teleport tour of every cell runs in ares (SD stubbed) with no ASSERT.
- A 200-cell subset runs with real SD paging in gopher64.
- The 20 worst cells hold ≥ 12 fps on hardware.
- The pack is ≤ 0x03FF0000.
- A 10-minute Balmora walk with music and voice shows no underrun and no SD stall over 15 ms.
- A rainy road walk shows no hitch over 100 ms.

### M11: Every system (22 weeks)

**Goal.** Everything vanilla quests need.

**Work**
- **Opcodes:** the remaining opcodes, until 100% of census opcodes are implemented.
- **Social systems:** persuasion; crime, bounty, guards and jail; faction ranks.
- **Services:** training, boats, guides, propylons, spell buying, merchant repair.
- **Crafting:** alchemy, self-repair, recharge, soul gems, enchanting, spellmaking with D-pad editors.
- **Magic and status:** Mark/Recall and **Interventions from exteriors** using the marker table; diseases, Corprus, vampirism; levelled restock.
- **Spell visuals:** Detect effects on the map list; Night Eye, Blind, Light and Chameleon visuals.
- **Menus:** the options menu, and PlayBink cards everywhere.

**OpenMW reused:** `mwmechanics` alchemy, enchanting, repair, recharge, security, pickpocket, spellutil, and mechanicsmanagerimp barter/persuasion/crime (all copied and host-tested).

**New code:** about 10,000 lines.

**Exit criteria**
- Every GuiMode vanilla uses is reachable, or listed as cut with a reason.
- 60 scripted scenarios pass in the twin and in the emulator, including an outdoor Intervention and an outdoor arrest, both with a heap check.
- Crafting results match desktop for 30 fixed cases.
- Resident code is ≤ the §5 gate.

### M12: Main quest and hardening (18 weeks)

**Goal.** Pass the acceptance test (§1).

**Work**
- A full main-quest playthrough on hardware, keeping a fix log.
- Checkpoint saves become **private** CI regressions, each round-tripped through desktop.
- A 4-hour soak.
- Fragmentation fixes.
- Work on the citadel cells' frame rate.
- A second flashcart model.

**OpenMW reused:** everything above; desktop as the oracle.

**New code:** about 3,000 lines of fixes.

**Exit criteria**
- The acceptance test in §1 passes.
- 30+ checkpoint saves pass in private CI.
- Two cart models pass the slice replay.

### M13: Release packaging (6 weeks)

**Goal.** A stranger with a legal copy can build and play without the developer's help.

**Work**
- A one-command baker for Linux and Windows with bundled libdragon tools, a language and build check, a progress display, and a size report.
- Documentation: Expansion Pak, SD layout (exFAT), cart settings (save type none, 64DD off), controls, saves, the converter, USB crash logs.
- Legal notice: only the engine ROM, baker and their source are distributed.

**New code:** about 1,500 lines.

**Exit criteria:** a tester on a fresh PC follows only the documentation and finishes character creation on real hardware.

---

## 5. Kill criteria and early measurements

| When | Measurement | Threshold | Fallback |
|---|---|---|---|
| M0 (wk 4) | Tiny3D on SC64 | < 3,000 tris per 66 ms at 210 draws, or CPU per draw > 50 µs | Caps: 2,500 tris, fog 0.5 cell, 6 drawn actors. Below 1,800 tris: renegotiate the definition as interiors + simplified hubs |
| M0 | ares-to-hardware ratio for C++ CPU work | > 1.5x | Re-baseline every CPU row. Cut to 3 full-rate / 6 simulated actors |
| M0 | SD per 8 KB chunk | > 15 ms stall, or < 300 KB/s | Music moves to ROM at a low bitrate (a subset of tracks). If a voice line can't load within 200 ms, drop voice (subtitles only) |
| M0 | Interrupt-woken audio thread | Does not preempt | mixer_poll stays in the main loop; add its blocking time to the CPU budget |
| M1 (wk 10) | Path A linked resident code | > 2,600 KB (expected) | Path B confirmed (plan of record) |
| M1 | Balmora CPU proxy on SC64 | > 25 ms | 3 full-rate / 6 simulated actors. Above 40 ms: towns get "4 simulated + everyone else dormant" |
| Every milestone | Path B resident code | > 3,200 KB | 1) DSO overlays for chargen, crafting, services, map/stats, journal (loaded into a buffer reserved at boot; the save writer stays resident); 2) cut UI polish; 3) shrink caches |
| M2 (wk 16) | Projected pack at the lowest quality tier | > 63.9 MB | SFX → SD (loaded behind fades), 32x32 textures everywhere, stronger decimation, simplified collision hulls, drop book art. **There is no SDRAM overflow tier** (cart SDRAM *is* the ROM space) |
| M2 | Record cache for Balmora | > 256 KB | Arena per cell; slimmer NPC structs |
| M3 (wk 24) | Hardware fps, statics only | Census < 20, Balmora or Vivec plaza < 12 | 288x216, LOD ring off (terrain only), 1,500 tris per cell |
| M4 | Movement replay | > 1 failure in 10, or > 5% of waypoints unreachable | Port `stepper.cpp` faithfully for the player on a swept-AABB tracer (about 1–2.5 ms per frame). NPCs keep the simple mover |
| M5 (wk 45) | Grid-shift hitch on hardware | > 150 ms after the incremental loader | Xbox-style pause with a "Loading" icon (≤ 0.7 s) at exterior borders, stated in the definition |
| M5 | Script CPU in Balmora | > 8 ms | Baker classifies poll-only scripts, which run every 4th frame; the far-script radius shrinks to 2,048 |
| M6 (wk 59) | Per-actor CPU | > 2 ms average | 3 full-rate / 6 simulated, dormant radius 1,536. Document the behaviour loss |
| M6 | Animation traces | Not within 1 sample / 2% | Stop. Bake event times from desktop trace recordings instead of the KF sampling |
| M7 | Dialogue | Topic > 1 s, or any main-quest topic mismatches Filter | Fix before continuing (no fallback: a wrong INFO breaks quests). Add per-signature topic-availability tables |
| M8/M10 | RSP audio + Tiny3D | Frame below the floor | VADPCM only, music 16 kHz mono, 16 channels, no voice while music plays |
| M9 (wk 93) | Heap soak | High-water > 7,808 KB, > 50 KB per transition, or largest free block < 512 KB | Texture slots 128 → 96, cached interior 0, 6 simulated actors, save-and-reload compaction behind interior fades |
| M9 | Saves | > 20 s, frame stalls, or desktop round-trip mismatch | Save only at beds and menus with a progress screen; dirty cells only; support SC64 only if another cart's SD is unusable |
| Schedule | Effort | M0–M5 take > 2x plan (> 90 wk), or the slice takes > 2x plan (> 186 wk) | Declare the vertical slice the product and publish it with the baker |

---

## 6. How Pak and chroma64 are used

### Pak (`/home/user/Pak`): use the tooling and the knowledge, not the language

**Use**
- **`tools/build_ares.sh`:** the pinned ares build (af4cbb04, installed at `/opt/pak-ares`). Distro builds lack the N64 core.
- **`tcl/tools/ares_test.tcl` logic:** ported in M0 to `tools/n64test/ares_run.py`, so Tcl is not a dependency. It becomes the gate for every milestone.
- **`N64_HARDWARE.md` and `IDIOMS.md`:** code-review checklists for the page cache, texture slots and pack DMA:
  - cache writeback → DMA → wait → invalidate;
  - 16-byte alignment;
  - KSEG1 addresses for RDP data;
  - init order.
  - Note: this port chooses triple buffering, which matches the canonical `display.init(0, 2, 3, 0, 1)`.
- **`docs/ipl3-emulator-matrix.md`:** explains why stock mupen64plus cannot test libdragon ROMs. The oracles are ares + gopher64 + hardware.
- **The `n64rom.tcl` padding note:** applied only to hardware release ROMs. Test ROMs stay ≤ 0x03FF0000 so ISViewer works.
- **Optional:** small throwaway Pak ROMs (mixer/wav64) as quick hardware experiments while learning. They never go into the engine.

**Do not use**
- The Pak language, for gameplay or scripts. Scripts stay OpenMW bytecode, and a third language hurts a beginner.
- The standalone MIPS backend (no C++, no-op `free`, 2 MB code window).
- The RSP microcode backend.
- The Tiny3D bindings and fetch scripts. They are pinned to a trunk revision that is incompatible with libdragon preview 39d0d60. The port uses Tiny3D 73d822f directly.
- The save modules (unimplemented).

### chroma64 (`/home/user/chroma64`): use the ideas, not the encoder

**Do not use it as the texture baker, because it:**
- forces alpha to 255 (foliage, hair and grates would turn opaque);
- reads only PNG without ffmpeg;
- takes about 3.5 s per 256x256 texture;
- produces full-sheet output that doesn't fit in RDRAM.

`mksprite` does the job instead.

**Reuse these ideas**
1. **Block-aligned texturing.** Terrain binds one LTEX tile per 4x4-quad block of the 17x17 mesh, so texturing adds zero triangles. This is chroma64's alignment idea, applied to shared tiles instead of per-cell pages (per-cell pages would cost about 45 MB of ROM).
2. **The UV-page clipper** (`gen_rdp_mesh.py` `clip_axis`/`split_uv`), about 100 lines of C++, used only for hero surfaces because it multiplies triangles 2–15x.
3. **The TMEM residency pattern** from `church.pk64` (fixed 2 KB records, aligned DMA, KSEG1 handoff) becomes the 128-slot texture cache and the icon cache, plus the LRU eviction it lacked.
4. **The distance-tier model** (`TIER_SPECS`/`pickTier`) chooses texture tiers and mesh LODs.

---

## 7. Policy for touching OpenMW code

1. **Pin and freeze.** Stay on fork base 3ee798e9 (0.52-dev). Rebase only deliberately, at most once per major milestone. Each rebase needs green differential tests, a re-bake, a re-check of the built-in Lua spec files, and the provenance-diff script (below).
2. **Preference ladder**, most preferred first:
   - **(a) Link unchanged on the PC.** The baker, converter and tests link unmodified openmw-lib and components. If the baker needs internals, it copies them. It never patches them.
   - **(b) Link unchanged on the N64** only for files on the M1 "linkable unchanged" list (`components/interpreter`, `esm` RefId, selected `misc`).
   - **(c) Copy instead of forking.** Copied logic lives in `apps/openmw_n64` with `// from <file>:<lines> @<commit>` comments. A script lists every comment whose source changed after a rebase. **Every copied rule gets a host test that calls the original in openmw-lib side by side** (same inputs and seed, identical outputs).
   - **(d) Edit OpenMW source,** only as:
     - upstreamable portability fixes (for example `int32_t` vs `long` casts, the ESM big-endian fixes already made) submitted to OpenMW's GitLab; or
     - `#ifdef OPENMW_N64` hunks when there is truly no alternative.
3. **Budget and bookkeeping.**
   - At most **300 changed lines** in `apps/openmw`, `components` and `extern`, counting the existing ~65. This excludes `apps/openmw_n64`, `apps/n64baker`, and the PC-only `trace-oracle` branch.
   - `PATCHES.md` lists every hunk with file, line count, reason and upstream status.
   - CI compares `git diff --stat <pin>` with the ledger and fails on unledgered growth.
   - CI builds desktop OpenMW from the same tree and runs `openmw_tests`, so any hunk keeps desktop behaviour identical.
4. **Instrumentation stays off the N64.** The trace oracle and dialogue decision logging live on a separate branch and are never compiled into shipped code.
5. **If Path A were ever revived** (M1 link ≤ 2,600 KB), the older policy would apply instead: interfaces and shadow headers first, and a 1,000-line ledger cap.

---

## 8. Total effort and the next three tasks

### Effort

A "week" means **40 focused hours** by a strong hobbyist working with an AI assistant.

| Scope | Planned weeks | + 30% contingency | x1.5 beginner factor | Hours | At 12 h/week | At 25 h/week |
|---|---:|---:|---:|---:|---:|---:|
| Vertical slice (M0–M9) | 93 | 121 | ~180 | ~7,200 | **~11–12 years** | ~5.5 years |
| Full main quest (M0–M13) | 155 | 202 | ~300 | ~12,000 | **~19 years** | ~9 years |

**Code volume**
- About **85k lines** of new or copied code: baker about 15k; N64 runtime about 62k (world and opcodes about 17k, copied rules about 14k, UI about 13k, renderer about 7k, physics about 4k, saves about 3k, audio about 2k, tests and harness about 2k); save converter about 3k.
- **≤ 300 changed lines** in OpenMW itself.
- Hundreds of thousands of unchanged OpenMW lines used as baker, compiler and oracle.

**The honest recommendation:** commit to the **vertical slice** (week 93 of plan). It is a real, shippable result: Morrowind's opening hours on a real N64. Recruit contributors before widening to M10–M13.

**Go/no-go checkpoints** (in plan weeks)
- **4:** hardware numbers.
- **10:** code path and gameplay CPU.
- **16:** census and pack size.
- **24:** frame rate.
- **45:** grid-shift hitch and script CPU.
- **59:** actor CPU and animation fidelity.
- **93:** the whole slice on hardware.

The three biggest unknowns (code size, frame rate, gameplay CPU) all have a first hardware measurement by week 10.

### The very next three tasks

1. **Make `make test` exist (about 3 days).** Port Pak's `tcl/tools/ares_test.tcl` into `tools/n64test/ares_run.py`. Add the `OMW64 FRAME ...` log line to the existing viewer. Make `make test AUTOPLAY=1` boot it headless in the pinned ares, parse one FRAME line and compare one example-suite screenshot against a golden with RMSE.
2. **Run the Path A link probe (hard cap: 2 weeks).** Add a `probe-reuse` target to `apps/openmw_n64` that links mwworld, mwclass, mwmechanics, mwscript and mwdialogue with abort() stubs at release flags. Print text, rodata, eh_frame, gcc_except_table and data per archive and per `.cpp` from the real ELF. Record the Path A/B decision and the "linkable unchanged" file list in `docs/runtime-design.md`.
3. **Write the first two hardware microbenchmarks (about 1 week).** Clone Tiny3D at 73d822f. Build a ROM that measures:
   - (a) textured, fogged, Z-buffered Tiny3D tris/ms and CPU µs per draw at 130 and 300 draws;
   - (b) SD read KB/s and the stall per 8 KB chunk through the new `SdFs` storage layer.

   Run it in ares and gopher64 now. Run it on the SummerCart64 as soon as you have one, and commit the numbers to `docs/hardware.md`.