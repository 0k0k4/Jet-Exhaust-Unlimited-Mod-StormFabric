# "Jet Exhaust Unlimited" mode — plan and RE evidence

> **Note (2026-11-09) — target architecture reached.** The code of this plan is
> now compiled into the **mod's DLL** (`src/jet_*.cpp`, `build_mod.bat`), and the
> StormFabric loader no longer references the component: it only exposes the
> generic native primitives (`sw_hook_inline`, `sw_engine_symbol_find`,
> `sw_memory_*`, `sw_game_alloc`, `sw_engine_string_make`). The engine service
> addresses come from the loader's discovery profile; the over-rev site is
> resolved by signature.
>
> **Note on this copy (2026-11-08).** This document is the historical work log,
> not the reference for the current state. Two points changed since: the stub
> uses `INFINITY` (`+inf`), not `1e30f`, and it writes the RPS into `XMM0` (not
> `XMM1`). The active path injects the component **in memory** (clone of the
> vanilla definition); the sections about materialising files in `rom/`
> describe the old path, which is no longer used. See `README.md`,
> `docs/HOW_IT_WORKS.md` and `docs/REVERSE_ENGINEERING.md` for the current state.

Goal: a **new** jet exhaust component without the over-rev limit (explosion at
~200 RPS), **without changing** the behaviour of the vanilla component.
Target: StormFabric (loader) + an SDK mod, reviewed builds 1.15.18 and newer.

## 1. Why it explodes (found, Ghidra evidence)

Project: a local Ghidra project of stormworks64.exe 1.15.23 (SHA-256
`f9206d85…`, base `0x140000000`).

- All the modular jet engine components share **a single native class**:
  `c_scene_vehicle_component_jet_engine` (vtable `0x140b887c0`), hierarchy
  `c_scene_vehicle_component_extended_base -> dynamic_base -> base`.
- The per-tick update is **slot 107** of the vtable: `FUN_140350f40`
  (RVA `0x350f40`, ~42 KB, switching on `jet_engine_component_type`).
- Types: 0=intake, 1=compressor, **2=combustion chamber**, 3=turbine,
  4=exhaust (basic/afterburner/rotating), 5=duct.
- In the `type == 2` branch (**combustion chamber**):

```c
state = FUN_140294520();                    // game state singleton
if (state[0x35] != 0 &&                     // damage enabled flag (to confirm)
    state[0x4a] != 0 &&                     // explosions flag (to confirm)
    DAT_140bb07d8 < *(float*)(this + 0x1bc) &&   // internal RPS > 200.0f HARDCODED
    *(char*)(this + 0x4d9) == 0) {          // "already exploded" guard
    *(char*)(this + 0x4d9) = 1;
    FUN_140288f70(..., DAT_140bb0740, 200.0f, ...);  // spawn the explosion
}
```

- The `200.0f` constant lives in `.rdata` (`0x140bb07d8`) and is referenced by
  **50+ functions**: **never** patch the constant, only the branch site.
- **Important trap**: the `+0x4d9` guard is also used by the "destroyed by
  damage" explosion (end of `FUN_140350f40`, health test then the same flag).
  Presetting the flag would remove both explosions — rejected. Only the over-rev
  branch must be neutralised.
- The XML files in `rom/data/definitions/` contain **no** RPS limit
  (`jet_engine_exhaust_basic.xml` checked): the limit is 100% native.
- Definitions are loaded by **listing the folder** (strings `"definitions/"`,
  `".xml"`, wide `"rom/data/..."`, xrefs `FUN_14046ed20`, `FUN_140206400`,
  `FUN_140871570`, `FUN_140872440`): adding a new XML file adds a new component
  without touching the vanilla ones.

## 2. Further RE results (2026-11-06)

### Patch site certified on both builds

Opcode signature unique per binary (32 bytes, rel32 as wildcards):

```
F3 0F 10 87 BC 01 00 00   movss xmm0,[rdi+0x1bc]      ; internal RPS
F3 0F 10 35 ?? ?? ?? ??   movss xmm6,[rip+…]          ; 200.0f (build-specific rel32)
0F 2F C6                  comiss xmm0,xmm6
0F 86 ?? ?? ?? ??         jbe skip
44 38 BF D9 04 00 00      cmp byte [rdi+0x4d9],0      ; explosion guard
```

- 1.15.23 (SHA `f9206d85…`): **RVA 0x35352a** (a single match)
- 1.15.18 (SHA `55914301…`): **RVA 0x35428a** (a single match)
- Field offsets **identical** on both builds (`+0x1bc` RPS, `+0x4d9` guard).

Patch plan (19-byte window = movss+movss+comiss; the original 6-byte `jbe` stays
in place and consumes the stub's flags):

```
CALL stub (E8 rel32, 5 bytes) + 14 NOP
stub: push rax/r10 ; def=[rdi+0x58]
      cmp def, [rip+g_unlimitedDef]      ; pointer captured at registration
      movss xmm6,[rip+g_200]             ; address of the original constant (per build)
      jne .vanilla
      movss xmm6,[rip+g_huge]            ; ~+inf for our component
.vanilla:
      movss xmm1,[rdi+0x1bc]
      comiss xmm1,xmm6                   ; final flags for the original jbe
      pop r10/rax ; ret                  ; ret does not change the flags
```

The stub must live within ±2 GB of the site (allocation near the image base).
The original XMM6 is dead after the comiss (reloaded before the explosion spawn
at 140352bda on 1.15.23) — checked.

### Instance -> definition identity

- `this+0x58` -> parsed definition (struct > 0x2900 bytes).
- `def+0xed4` = `jet_engine_component_type` (int) — confirmed: the definition
  parser `FUN_1403168d0` calls the generic attribute parser with
  `(int*)(def+0xed4)` for `"jet_engine_component_type"`.
- `def+0x298` = `name` (string).
- Chosen strategy: **capture the definition pointer at registration**
  (injection -> parse -> capture), exact pointer comparison in the stub: no
  string comparison per tick.

### Definition injection (option A)

- `FUN_14046c180` (1.15.23) scans `L"rom/data/definitions/"` (folder iterator
  `FUN_1401ec620`), builds the list of names, then `FUN_140206400` loads each
  `<name>.xml` (`FUN_140183d60` = read, `FUN_1402067e0` = parse + registration)
  and registers them **by name** in a hash map.
- Injection point: the result of the folder iteration (the loader adds the name
  of its virtual XML to the name container after the scan), then capture of the
  definition pointer after `FUN_1402067e0`.

### Left before implementation (minor)

1. Confirm the writer of `this+0x58` (ctor chain `FUN_14033b320` / creation site
   `FUN_14045dd20`) — almost certain given `def[+0xed4]` = type.
2. Pin down the name container in `FUN_14046c180` (exact insertion point) and the
   capture site after registration.
3. Exact meaning of the `state[0x35]`/`state[0x4a]` flags (probably
   damage/explosion settings) — informative, the stub leaves them untouched.

## 3. Development plan

### Remaining RE phase (before any code)

Solved — see section 2 (site certified on 2 builds, identity, injection).

**Target correction (2026-11-06, after a first in-game test)**: the
`200.0f < RPS` test is in the `jet_engine_component_type == 2` branch
(**combustion chamber**) of `FUN_140350f40`. The stub compares the `value` of the
definition of the component **being updated** — so the `500123` signature must
be carried by a new **combustion chamber**
(`jet_engine_combustion_chamber_unlimited.xml`, vanilla copy with
`value="500123"`). The "Jet Exhaust Unlimited" stays injected as a cosmetic
variant, but the chamber carries the unlock.

4. **Hook choice**: inline patch at the branch site with a loader trampoline:
   - if the component's definition pointer == the captured pointer -> skip the
     explosion block (vanilla behaviour otherwise);
   - transactional installation, check of the original bytes, rollback,
     conflict detection — same discipline as the existing hooks.
   Rejected alternative: hook slot 107 + re-emulate the logic (too fragile).

### Loader development phase

5. New reviewed binding (2 builds) + hook transaction for the branch site.
6. New minimal SDK surface (append-only): e.g. `api->components` or a dedicated
   "register jet overrev bypass(signature)" function. Mods stay free of engine
   addresses.
7. Logs and diagnostics: explicit refusals if the bytes are unexpected
   (read-only).

### Mod development phase

8. New component: `jet_engine_exhaust_unlimited.xml` — copy of
   `jet_engine_exhaust_basic.xml` named "Jet Exhaust Unlimited", vanilla meshes
   reused, and a **distinctive signature** in an attribute without gameplay
   effect (to be chosen at step RE-2) for runtime identification.
9. `jet_unlimited` mod (SDK DLL + manifest): registers the signature with the
   loader. The installation copies the XML into `rom/data/definitions/` (with a
   clean backup/uninstall, game closed).
10. The vanilla component stays intact and keeps exploding above 200 RPS.

### Validation

11. Offline tests: bypass decision logic (fake backend), binding validation on
    both binaries, regression of the existing suites.
12. In-game validation: go above 200 RPS -> the new component does not explode,
    a vanilla engine still explodes; the "destroyed by damage" explosion works on
    both; clean uninstall.

## 4. Implementation (2026-11-06) — DONE

### Major finding: the catalogue is baked, mod components are COMPILED

Two facts established by RE + in-game observation:

1. **The inventory catalogue is baked into the executable**: the names of the
   vanilla definitions (`jet_engine_exhaust_basic`, `water_jet`, …) are present
   as literals in `stormworks64.exe`. An XML added to `rom/data/definitions` is
   *parsed* (the name shows up in the 3D tooltip) but does not enter the
   catalogue → *placeholder* entry (`DEF_<FILE>_XML_NAME`, `$100`, `mass 1`,
   `tags: blocks`, cube mesh).
2. **Mod components go through an official local mod**:
   `%APPDATA%\Stormworks\data\mods\<id>\` with `mod.xml` at the root and the
   components **compiled** in `data/components/*.bin` (`FUN_14020f950` reads
   `<modroot>/mod.xml`). Observed reference: an installed Workshop mod
   (`..._more_monitors_mod_v2`) with 39 XML + 49 `.bin`.

`.bin` format: `[uint32 size][content]` (compiled by
`<game>/sdk/component_mod_compiler.com`).

Our pipeline (at the time): the `.bin` is compiled at build time with the
compiler **of the target game** (format tied to the version), shipped in the mod
folder, then the loader installs/refreshes an **official local mod** under
`%APPDATA%\Stormworks\data\mods\<id>\` (generated `mod.xml`, `data/definitions`
+ `data/components`, `.stormfabric` marker), with automatic cleanup when the mod
is disabled — never touching third-party mods (checked by a test).

| Component | Role |
|---|---|
| `src/jet_patch.cpp` | Definition reconciliation (`mods/*/inject_definitions` -> `rom/data/definitions`, `.xml.swml` marker, automatic removal), signature search in the executable sections (requires exactly 1 match), constant validation (200.0f required), stub allocation within ±2 GB, transactional CALL+NOPs patch (19 bytes), clean rollback |
| Stub (50 bytes + 2 floats in the block) | `def=[rdi+0x58]`; if `def+0x2b4 == 500123` (value field of the mod's XML) then limit `1e30f`, else the copied original constant; final `comiss` for the preserved original `jbe`. No string comparison per tick |
| `src/mods_scan.*` | `inject_definitions` (XML -> rom/data/definitions) and `inject_components` (`.bin` -> rom/data/components) keys in mod.json, exposed on `ModEntry` |
| `examples/jet_unlimited/` | `mod.json` (inject_definitions), minimal SDK DLL, `jet_engine_combustion_chamber_unlimited.xml` (the working component: vanilla copy of the chamber, `value="500123"`) + `jet_engine_exhaust_unlimited.xml` (cosmetic variant) |
| `tests/jet_patch_tests.cpp` | Signature (unique/missing/ambiguous/wildcards), constant resolution, stub encoding, fail-closed patch/rollback, sanitised names, materialisation/refresh/removal; optional real binary check (mapped image) |

Results: 14 suites PASS; real binary check on both reviewed executables: site
`0x35352A` (1.15.23) and `0x35428A` (1.15.18), one match each, constant
`0x43480000` (200.0f). In-game validation left: component present in the
workbench, above 200 RPS without explosion, vanilla engine still limited,
disabling the mod -> file removed at the next start.

## 4bis. In-memory catalogue injection (2026-11-06) — DONE

Goal: **the C++ loader alone**, without an official mod and **without any file
written outside `mods/`**.

### Native registration path (RE)

| Role | Function | 1.15.23 | 1.15.18 |
|---|---|---|---|
| Definition/component loader (called once at init) | `FUN_14046c180` | `0x46c180` | `0x46cf70` |
| Builds a component from a `.bin` | `FUN_140314ce0` | `0x314ce0` | `0x315a90` |
| De-duplication | `FUN_14046e870` | `0x46e870` | `0x46f2f0` |
| **Catalogue registration** (ring `manager+0xF8`) | `FUN_14046ec30` | `0x46ec30` | `0x46f6b0` |

The component manager is at `app + 0xbb670`. The factory opens the file through
`_wfopen_s` on a path **relative to the game root** → our `.bin` stays in
`mods/jet_unlimited/`. The expected allocator and string type are **already**
the loader's (`op_new` 0x957478, `str_reserve` 0x17cb00, `str_free` 0x9b15a0).

### Implementation (`src/jet_catalogue.cpp`, at the time)

1. **Entry hook** on `FUN_14046c180`: 50-byte detour that saves the 5 arguments
   (4 registers + `[rsp+0x28]`) into a block allocated within ±2 GB, runs the
   stolen instruction (`mov [rsp+0x20],r9`, the only one in the first 5 bytes)
   and resumes execution. No call, so the stack is untouched.
2. **Deferred registration** (`JetCatalogueTick`, called from the already hooked
   UI pulse): `StrMake("mods/jet_unlimited/<file>.bin")` → `FUN_140314ce0` →
   `FUN_14046e870` → `FUN_14046ec30`. Only once, on the game thread.
3. **Fail-closed**: the 4 functions are validated by a 64-byte signature (mask
   common to both builds); any difference → no hook.

The component then shows up as a real catalogue component (name, price, mesh),
**without any mod** and without any file added to the game.

Follow-up: these 4 identities were certified in the module (the Ghidra project of
the 1.15.18 reference no longer exists); eventually, move them into the central
reviewed bindings regenerated from both projects.

## 4bis (continued). Chosen solution: hook `FindComponentDefinitionByName`

Decisive finding: the old **CustomDisplay** project (removed from the repository)
already had a **working** catalogue injection
(`custom_display_vehicle_observer.cpp`). Its technique is reused:

| Role | Detail |
|---|---|
| Hook | `FindComponentDefinitionByName(registry, name)` — the **registry comes in RCX** — 1.15.23 `0x46e900` / 1.15.18 `0x46f380` |
| Hook | `FF 25 00 00 00 00 <imm64>` + 2 NOP (16 bytes), trampoline = copied prologue + absolute jump to `target+16` |
| Registry | global ring `+0xF8` (cap `+0x100`, head `+0x104`, count `+0x108`); category buckets `+0x110 + category*24` |
| Definition | `0x2d08`-byte object on 1.15.23 (`0x2848` on 1.15.18) × 8 variants, each in ITS OWN allocation; id `+0x288`, name `+0x298`, category `+0x2a8`, **value `+0x2b4`**, flags `+0x2b8` |

Our implementation (`src/jet_catalogue.cpp`):

1. On the **first** definition lookup, clone the vanilla combustion chamber
   (`jet_engine_combustion_chamber`), set the identity
   `jet_engine_combustion_chamber_unlimited` / "Jet Combustion Chamber Unlimited",
   category 11, `value = 500123` (the signature of the 200 RPS bypass) and flags 0.
2. Insert the clone into the global ring **and** into the category bucket — that
   is what the workbench catalogue reads.
3. Return the clone when the game looks up our id.

No reallocation of an internal structure: if a ring is full, we refuse
(fail-closed). 64-byte signature validated on both builds; any difference → no
hook.

## 5. Risks / open points

- **Multiplayer**: an extra definition may desynchronise vanilla clients
  (component checksums) — to be tested; possibly single player/host only.
- The exact meaning of the `state[0x35]`/`state[0x4a]` flags (probably "damage" /
  "explosions" settings) to confirm, so as not to disable more than intended.
- The hook is an inline patch (more invasive than a vtable slot): certified
  instruction window required, fail-closed refusal otherwise.

## 4ter. Fixing how the injection is driven (2026-11-07)

The component never showed up: `JetCatalogueTick()` was only called from the
`default_view.init` / `set_alpha` / `interface.update` hooks, which **only exist
in the main menu**. But the component registry is only filled when a world / the
workbench loads: the tick never ran with a captured registry (and
`ValidateRegistry` could also silently reject the registry on an out-of-range
`head`, without any log).

Fixes:

1. **Driven from `HookedFindDef`**: the registry comes in RCX, it is captured,
   then the injection is tried **immediately**, without ring growth
   (`RingPush(..., allowGrow=false)`) so that a ring the engine may walk in the
   same frame is never reallocated. Pacing bounded to 250 ms.
2. **Driven from the HUD / editor update** (`src/ingame_notifications.cpp`)
   through `JetCatalogueTick()`: these updates run in game, where the registry
   exists (growth allowed there).
3. **`head` normalised** (`NormHead`) instead of rejecting the registry; the
   whole ring geometry is logged once (`Jet[diag] ...`) and every validation
   refusal is logged with its reason (`Jet: registry %p rejected by validation`).
4. **Category/flags read from the source**: the buckets span `+0x110` to `+0x290`
   (16 × 24 bytes, certified by `ComponentRegistry::Clear` 0x5f1ac0); so the real
   category of the vanilla chamber is reused, and its flags are kept except bits
   `0x80`/`0x800` (which, in `FUN_14046ec30`, skip the registration in the
   category bucket).
5. **Tolerant source search**: exact id, then prefix, then any id containing
   `combustion_chamber` without `unlimited`; the id actually found is logged.

## 4quater. "descriptors ring full" = EMPTY ring never allocated (2026-11-08)

Symptom: `grep -c "descripteurs plein" StormworksModLoader.log` → 5, with the
world load never finishing (`c_game_state_spawning` never destroyed, the game's
counter oscillating 24↔25).

Evidence in the session log (verbatim; the log messages were in French at the time):

```
[00:46:58.658] Jet[diag] registre=... | variantes cap=8192 head=0 count=6072 |
               xml cap=0 head=0 count=0 | global cap=1024 head=0 count=759
[00:46:58.660] Jet: ring des descripteurs plein ; injection annulee (croissance=0)
```

The XML descriptors ring (`registry + 0xe0`) was **not full**: it was
`cap=0 head=0 count=0`, i.e. **never allocated**. In `RingPush`, the
`count >= cap` test is true by construction when `cap == 0`: the code concluded
"full" and, since `allowGrow=false` on the lookup hook path (no reallocating a
ring the engine may walk in the same frame), the injection failed **on every
attempt for the whole load**. `JetCatalogueTick()` (the only path allowing
growth) is not called during the loading screen: the injection only succeeded at
`00:47:08.603`, 110 ms **after** the end of the spawn phase — too late for a
prefab that needs the component.

Fix (`src/jet_catalogue.cpp`):

1. `JetCatalogueRingGrowth()` (pure, tested) tells **allocation** (`cap == 0`, no
   buffer to invalidate ⇒ safe even inside a game frame, and `FUN_1408de0b0`
   explicitly handles the null old buffer) from **realloc** (`cap > 0`, always
   forbidden inside a game frame).
2. The clone (8 variants, `sizeof(ComponentDefinition)` per variant depending on
   the build: `0x2d08` on 1.15.23, `0x2848` on 1.15.18; separate allocations) and
   the 32-byte descriptor are built **only once** (`PrepareDefinition` /
   `EnsureDescriptorStruct`): a refused insertion no longer rebuilds 82 KB nor
   leaks its strings; only the insertion is replayed (`InsertDefinition`, full
   rollback on the first refusal).
3. The log no longer lies: `ring not allocated (cap=0)` / `ring full` /
   `invalid geometry`, once and then 1 refusal in 200.
4. **Load diagnostics**: every id the game looks up without finding it during a
   load is logged once (`Jet: lookup missed during load: '<id>'`) — this is what
   tells whether a prefab cannot be instantiated (cause of a spawn retried in a
   loop).
5. **One-file bisection**: creating `<mod>/.no_catalogue_injection` (that is
   `mods/jet_unlimited/.no_catalogue_injection`) turns off only the catalogue
   insertion, keeping the diagnostic hooks. Disabling the mod in the game's
   **MODS** menu turns off the whole mod (no hook).

Bisection procedure (game closed, then started):

| Step | Action | Verdict in `StormworksModLoader.log` |
|---|---|---|
| A | start, load the world | `Jet: component '...' injected into the catalogue` (or `Jet: lookup '...' -> injected on demand`) during the load, then the load finishes without blocking ⇒ the injection is not the cause |
| B | create `.no_catalogue_injection`, restart | if the load finishes **without** injection ⇒ the block comes from the injection; if it is still blocked ⇒ the block is in the world (the `Jet:` lines name the missing component) |
