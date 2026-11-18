# Jet Exhaust Unlimited — mod folder

This folder is what goes into `<Stormworks>\mods\jet_unlimited\`.
It contains **no official definition file**: the mod's DLL clones the vanilla
definition **in memory** and inserts it into the game's catalogue.

## Files

| File | Role |
|---|---|
| `mod.json` | mod manifest (id, DLL, enabled by default) |
| `jet_unlimited.dll` | the mod's DLL, **self-contained** (built by `build_mod.bat`) |
| `jet_engine_combustion_chamber_unlimited.xml` | working definition of the chamber without the over-rev limit (read by the mod for the descriptor) |
| `jet_engine_combustion_chamber_unlimited.bin` | historical compiled component — **not needed** by the in-memory injection |
| `jet_engine_exhaust_unlimited.xml` | cosmetic exhaust variant (plays no part in the bypass) |
| `README.md` | this file |

There is **no `mod.sig`**. The component's signature is the `value="500123"`
constant of the XML, copied into the definition's `value` field and tested by
the mod's machine-code stub.

## Installation

1. Build the mod's DLL from the mod repository:

   ```bat
   cd /d "<path to the Jet Exhaust Unlimited repository>"
   cmd /c ".\build_mod.bat"
   ```

2. Copy this folder to `<Stormworks>\mods\jet_unlimited\` (the DLL is already in it).
3. Start the game. The mod is enabled by default; it is turned on or off in the
   game's **MODS** menu. The **Jet Combustion Chamber Unlimited** component shows
   up in **JET ENGINES**.

## What the mod does

- it clones the vanilla `jet_engine_combustion_chamber` definition in memory;
- it replaces id/name/category/`value` and inserts the clone into the catalogue
  rings **on demand**, in the engine's lookup hook;
- it re-arms the injection after `ComponentRegistry::Clear` (back to the workbench);
- it diverts the `200.0f < RPS` test only for `value == 500123`;
- it restores everything on exit (`SwModUnload`, then the DLL is unloaded).

The vanilla component is not modified: it still explodes above 200 RPS.
The StormFabric loader contains no code from this mod: the mod uses its generic
native API (`sw_hook_inline`, `sw_engine_symbol_find`, `sw_memory_*`,
`sw_game_alloc`, `sw_engine_string_make`, …).

## Disabling

- `SW_JET_CATALOGUE=0`: turns off the catalogue injection (the component does
  not show up);
- the game's **MODS** menu: turns off the whole mod (no hook, no patch);
- a `.no_catalogue_injection` file in this folder: turns off only the catalogue
  insertion.

## See also

- `../../README.md` — overview, building, installation;
- `../../docs/INSTALLATION.md` — full guide;
- `../../docs/HOW_IT_WORKS.md` — internals.
