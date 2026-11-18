# Jet Exhaust Unlimited

A combustion chamber for Stormworks that **does not explode when it over-revs**.

The vanilla component explodes at about 200 RPS. This one goes as high as you want.

The mod is **self-contained**: it carries all of its logic and installs like any
other StormFabric mod. The loader contains no code specific to this mod; it only
exposes **generic native primitives** (symbol resolution through the discovery
profile, bounded memory access, inline hooks with trampoline and restore, the
engine's allocator and strings).

---

## What it does

- Adds **Jet Combustion Chamber Unlimited** to the *JET ENGINES* tab of the workbench.
- No over-rev limit for **this component only** (identified by `value = 500123`).
- **The vanilla Jet Exhaust is not modified**: it still explodes at 200 RPS.
- Nothing is written to the game: everything is injected **in memory**, on demand.
- The component **stays on the vehicle when it goes back to the workbench**: the
  injection happens in the engine's lookup hook, at the exact moment the game
  resolves the vehicle's component ids.

## What it does not do

- No new textures or model: the component reuses the vanilla mesh.
- Only works on the game executables **reviewed by StormFabric** (the loader
  refuses any other binary; the mod then stays inactive and writes nothing).

---

## Requirements

The **StormFabric** loader (`iphlpapi.dll`). This is not an official Stormworks
mod: the mod's DLL creates the component on the fly, through the loader's native
SDK.

## Building

From this repository (StormFabric SDK in a sibling checkout):

```bat
build_mod.bat
```

or, if the SDK is somewhere else:

```bat
build_mod.bat "H:\path\to\StormFabric\include"
```

Produces `mod/jet_unlimited/jet_unlimited.dll`. No executable address is
hardcoded: the mod builds without a copy of the loader's sources.

## Installation

1. Copy `iphlpapi.dll` (StormFabric build) into the game folder:
   `...\Stormworks\iphlpapi.dll`
2. Copy the `mod/jet_unlimited/` folder to:
   `...\Stormworks\mods\jet_unlimited\`
3. Start the game. The mod is enabled by default; it is turned on or off in the
   game's **MODS** menu. The component shows up in the workbench, **JET ENGINES**
   tab.

The `mod/jet_unlimited/` folder holds `mod.json`, `jet_unlimited.dll`, the
definition XML and the historical `.bin` (kept as a reference asset; the
injection no longer needs it, since it clones the vanilla definition in memory).

## Uninstalling

- Disable the mod in the **MODS** menu, or delete `mods/jet_unlimited/`.
- To turn off only the component injection, without deleting anything:

  ```bat
  set SW_JET_CATALOGUE=0
  ```

Nothing else to clean up: no file is created in `rom/` or `%APPDATA%`.

---

## How it works (short version)

1. **Component injection**: the mod clones the vanilla definition
   (`jet_engine_combustion_chamber`) and adds it to the game's catalogue,
   identified by its price: `value = 500123`. The injection is triggered by the
   `FindComponentDefinitionByName` hook (on demand) and re-armed after
   `ComponentRegistry::Clear`: the component survives a workbench reload.
2. **Removing the limit**: a machine-code stub checks the component's
   definition; if its `value` is `500123`, the over-rev limit becomes `+inf`.
   Otherwise the vanilla behaviour is unchanged (the original `200.0f` constant
   is copied into the stub).

Every address comes from the loader's discovery profile, or from a signature
owned by the mod and resolved by `sw_engine_signature_find`. The mod never
writes to the game's code directly: it goes through `sw_hook_inline`, which
validates the prologue bytes, provides the trampoline and restores them exactly.

Technical details: [`docs/HOW_IT_WORKS.md`](docs/HOW_IT_WORKS.md)
Reverse engineering: [`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md)

## Tests

```bat
tests\run_tests.bat
```

The off-game tests check the pure logic (ring growth, on-demand injection, clone
geometry, string coherence) and the machine encoding of the over-rev stub. The
tests of the loader primitives (signature contract, hook/restore, write refusal)
live in StormFabric: `tests\run_tests.bat`.

## Troubleshooting

**The component does not show up in the workbench**
Check `StormworksModLoader.log` for `component '...unlimited' injected into the
catalogue`. If the line is missing, the log gives the cause (engine services not
resolved, signature not found, uncertified build).

**The component disappears from my vehicle after a trip to the workbench**
Should no longer happen: the injection is triggered by the engine's lookup and
re-armed before deserialization, and clearing a secondary registry no longer
removes it from the main one (2.0.1). Look for `injected on demand` in the log.

**The game crashes on exit**
Check that no `crash.dmp` is generated again. The mod restores its prologues in
`SwModUnload`, then Windows unloads the mod before the loader.

---

## License

See [`LICENSE`](LICENSE).
