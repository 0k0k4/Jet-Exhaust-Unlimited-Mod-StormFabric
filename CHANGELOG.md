# Changelog — Jet Exhaust Unlimited

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Mod version: `2.0.1` (the one in `mod/jet_unlimited/mod.json`).

The mod is **self-contained**: all the hook code is compiled into
`jet_unlimited.dll` (sources `src/jet_*.cpp`). The StormFabric loader no longer
contains any code specific to this mod; it exposes generic native primitives.

---

## [Unreleased]

## [2.0.1] — 2026-11-18

### Changed

- All documentation, code comments and log messages are now in English.
  `docs/FONCTIONNEMENT.md` is renamed `docs/HOW_IT_WORKS.md`.

### Fixed

- **The component disappeared from the workbench** when a vehicle placed on the
  map was brought back to it (no error; reloading brought it back). The game
  then clears a secondary component registry, and the `ComponentRegistry::Clear`
  hook also removed our entries from the main registry, which was still alive.
  Only the registry actually being cleared is swept now, and the injection state
  is kept while our entries are still in the main registry.

### Verified / hardened

- **Width of `flags` (`+0x2b8`) certified as 8 bytes** on 1.15.23: written by
  `mov qword ptr [r15],rax` (`0x1408efb48`), read as 64 bits in the bucket
  registration (`0x14046ec9c`), low dword read as 32 bits in
  `ComponentRegistry::Clear` (`0x14046e7d6`). Evidence and instructions in
  `docs/LAYOUT_DEFINITION.md`.
- Clone field layout centralised in `kDefinitionLayout` (`src/jet_catalogue.h`)
  with a `static_assert` on every offset/width; fields are written through
  `JetCatalogueWriteCloneFields` (pure).
- `TestCloneFieldWidths` test: sentinel on a `0x2d08`-byte buffer, checks that
  `flags` writes 8 bytes, that `+0x2c0` stays intact and that a 32-bit store
  would leave `+0x2bc..+0x2bf` unchanged.

## [2.0.0] — 2026-11-09

### Architecture

- **The mod is self-contained.** `jet_unlimited.dll` carries the catalogue
  injection and the over-rev patch. The loader no longer references the component.
- The mod only uses the loader's generic native API (`api->native` extension):
  `findSymbol` (engine roles resolved by the discovery profile),
  `findSignature` (signature owned by the mod, single occurrence),
  `readMemory`/`writeMemory` (bounded, protection restored),
  `hookInline`/`hookRestore` (byte contract, loader trampoline, idempotent
  restore), `gameAlloc`, `stringMake`/`stringFree`, `codeAllocNear`,
  `engineBuild`.
- No executable RVA left in the mod: engine services come from the loader's
  profile; the over-rev site comes from a validated signature.
- Hooks are restored before the DLL is unloaded (`SwModUnload` then
  `DllMain(PROCESS_DETACH)`), in reverse installation order.

### Added

- `build_mod.bat`: builds the mod against the StormFabric SDK, produces
  `mod/jet_unlimited/jet_unlimited.dll`.
- `tests/run_tests.bat` + `tests/mod_logic_tests.cpp`: self-contained off-game
  tests (signature mask, machine stub, ring growth, on-demand injection, clone
  geometry, strings, absolute jump).

### Removed

- `tools/sync_to_stormfabric.bat`: no longer needed, the mod no longer copies its
  sources into the loader.
- The test copies that depended on loader internals.

### Fixed

- The component **stays on the vehicle after a round trip to the workbench**:
  the injection is triggered on demand in the lookup hook and re-armed after
  `ComponentRegistry::Clear`, without timed polling.

---

## [1.2.0] — 2026-11-08

### Added

- Standalone repository for the mod: documentation, assets, reference copies of
  the hook code and the tests.
- `docs/FONCTIONNEMENT.md` (now `docs/HOW_IT_WORKS.md`): full architecture
  (catalogue + patch).
- `docs/REVERSE_ENGINEERING.md`: RVAs of both builds, signatures, offsets,
  method and analysis of the shutdown crash.
- `docs/INSTALLATION.md`: installation, checking, disabling, uninstalling.
- `tools/sync_to_stormfabric.bat`: resynchronises the 4 source files and the
  3 test files into a StormFabric checkout.
- `tools/README.md`: scope of the analysis scripts actually present.

### Fixed (loader side, carried over into this copy)

- **On-demand injection**: the `FindComponentDefinitionByName` hook reinjects
  on the first lookup, after `ComponentRegistry::Clear` or in a second registry.
  Fixes the component disappearing while a vehicle is deserialized.
- **Ring growth**: a `cap=0,count=0` ring (never allocated) is now allocated
  instead of being mistaken for a full ring. Reallocating a really full ring is
  still forbidden inside a game frame.
- **Clone geometry**: variant size chosen per build (`0x2d08` on 1.15.23,
  `0x2848` on 1.15.18) and a check that the last destroyed field
  (`+0x2d00` / `+0x2840`) fits in the variant. Fixes the shutdown crash caused by
  overlapping variants / double free.
- **Teardown**: our entries are removed from the rings before `Clear`, prologues
  are restored, and the stub is kept if the restore fails.
- **Over-rev stub**: `+inf` limit instead of `1e30f`, to cover `+inf` and NaN;
  RPS loaded into `XMM0` (not `XMM1`) so the game reads back the right value;
  final `comiss` for the original `jbe`.
- **Transactional patch**: the 19 original bytes are checked, saved, replaced by
  `CALL rel32` + 14 NOP, and restored.

### Changed

- The working component is the **combustion chamber**
  `jet_engine_combustion_chamber_unlimited` (`value=500123`); the
  `jet_engine_exhaust_unlimited.xml` file stays a cosmetic variant.
- The catalogue is injected **entirely in memory**: the active path writes no
  file to `rom/` or `%APPDATA%`. The historical file reconciliation code is still
  compiled but no longer called.
- `mod.json` description updated for the in-memory injection (it still mentioned
  `rom/data/components`).

### Known

- The `value=500123` field is also the component's price.
- Only builds 1.15.23 and 1.15.18 are supported.
- Multiplayer not certified.
- Historical "bypass disabled" comments in `jet_patch.cpp` / `mods_ui.cpp`: the
  `kOverrevPatchEnabled` constant is `true`.
- `tests/jet_test_sigs.h` declares `kStubBlockBytes = 0x40` while the production
  block is `0x68` (the test passes, but the constant should be aligned).

---

## [1.1.0] — 2026-11-07

### Added

- Component injection into the catalogue through hooks on
  `FindComponentDefinitionByName` and `ComponentRegistry::Clear`.
- Clone of the vanilla definition in 8 variants, with id/name/category/value and
  an XML descriptor.
- Validation of the catalogue signatures on both builds (64/72 bytes).
- Off-game tests: jump encoding, ring growth, clone geometry, string coherence,
  on-demand injection decisions.

### Fixed

- The injection is no longer driven only by the main menu UI pulses (which do not
  exist while a world loads).
- The registry is no longer silently rejected for an out-of-range `head`: it is
  normalised and refusals are logged.

---

## [1.0.0] — 2026-11-06

### Added

- Over-rev site certified on 1.15.23 (`0x35352a`) and 1.15.18 (`0x35428a`).
- 32-byte opcode signature, `200.0f` constant (`0x43480000`) resolved through
  `rel32`.
- 19-byte patch (`CALL` + NOP), stub within ±2 GB, identity through `value=500123`.
- Initial catalogue injection and mod assets (chamber XML, cosmetic exhaust XML,
  compiled `.bin`).
- Signature, stub, patch/rollback and materialisation tests.

---

## Maintenance notes

- Every new game version must be preceded by a full RE pass: the signatures are
  not compatibility guarantees.
- Never patch the global `200.0f` constant: it is shared by many engine functions.
