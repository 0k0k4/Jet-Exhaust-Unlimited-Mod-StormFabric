# Installation — Jet Exhaust Unlimited

Step-by-step guide to install, check and uninstall the mod with the StormFabric
loader.

---

## 1. Requirements

| Item | Value |
|---|---|
| Game | Stormworks: Build and Rescue, Windows x64 |
| Supported builds | **1.15.23** (`f9206d85…`) and **1.15.18** (`55914301…`) |
| Loader | built StormFabric (`iphlpapi.dll`) |
| Mod DLL | `jet_unlimited.dll`, built for x64 |
| Access | write access to the game folder |

Check the SHA-256 of `stormworks64.exe` (PowerShell):

```powershell
Get-FileHash "C:\Program Files (x86)\Steam\steamapps\common\Stormworks\stormworks64.exe" -Algorithm SHA256
```

The hash must match one of the two above. Otherwise the loader refuses every
hook: this is on purpose.

---

## 2. Building the mod's DLL

From this repository (MSVC x64 required; the StormFabric SDK is expected in a
sibling `StormFabric` checkout):

```bat
build_mod.bat
```

or, with the SDK somewhere else:

```bat
build_mod.bat "<path to StormFabric>\include"
```

Result: `mod\jet_unlimited\jet_unlimited.dll`. A ready-made zip of the mod
folder is attached to each GitHub release.

The loader itself is built from the StormFabric repository (`build.bat`, result
`build\iphlpapi.dll`).

---

## 3. Installing the loader

1. **Close Stormworks** (check in the Task Manager that no `stormworks64.exe` is
   left).
2. Find the game folder (the one that holds `stormworks64.exe`).
3. If an `iphlpapi.dll` already exists there, save it under another name.
4. Copy StormFabric's `build\iphlpapi.dll` to the game root.

The loader is a proxy: `stormworks64.exe` imports `IPHLPAPI.DLL`; Windows loads
the DLL from the executable's folder first, then the forwarders send the exports
on to the real `C:\Windows\System32\iphlpapi.dll`.

> Do not copy `iphlpapi.dll` into `System32` or any other folder: it must sit
> **next to `stormworks64.exe`**.

---

## 4. Installing the mod

Create this tree:

```text
<Stormworks>\
├── stormworks64.exe
├── iphlpapi.dll
└── mods\
    └── jet_unlimited\
        ├── mod.json
        ├── jet_unlimited.dll
        ├── jet_engine_combustion_chamber_unlimited.xml
        ├── jet_engine_combustion_chamber_unlimited.bin   (historical asset, not required)
        └── README.md
```

Copy the content of `mod\jet_unlimited\` from this repository (or extract the
release zip into `<Stormworks>\mods\`).

The mod reads `jet_engine_combustion_chamber_unlimited.xml` to build the
component's XML descriptor. Without it, the log shows
`Jet: component XML not found` and the component is not injected.

The mod is enabled by default (`enabled_by_default: true` in `mod.json`). It is
turned on or off in the game's **MODS** menu.

---

## 5. First start

1. Start Stormworks.
2. Wait for the main menu, then check `StormworksModLoader.log` (next to the
   executable).
3. Open **MODS**: `Jet Exhaust Unlimited` must be listed and enabled.
4. Go to the workbench, **JET ENGINES** category: the
   **Jet Combustion Chamber Unlimited** component must show up.
5. Put the component on a vehicle, connect it to the engine, go above ~200 RPS:
   it must not explode.
6. Put a **vanilla combustion chamber** next to it and go above 200 RPS: it must
   explode as usual.

### 5.1 Expected log lines

```text
Jet: definition layout for build 2: 11528 bytes per variant (last destroyed field +0x2D00)
Jet: catalogue hooks installed (build 2): lookup + ComponentRegistry::Clear
Over-rev: patch installed (site=..., stub=..., vanilla limit 200.0)
Jet: source='jet_engine_combustion_chamber' ptr=... cat=11 value=... flags=... -> clone ...
Jet: component 'jet_engine_combustion_chamber_unlimited' injected into the catalogue (memory, value=500123, registry=...)
```

The injection can also be reported on demand:

```text
Jet: lookup 'jet_engine_combustion_chamber_unlimited' -> injected on demand
```

This is normal: the game resolves a vehicle's ids during deserialization,
before the UI pulses.

---

## 6. Disabling

### 6.1 Disable only the catalogue injection

```bat
set SW_JET_CATALOGUE=0
```

Then restart the game. The mod does not install the catalogue hooks; the
component no longer shows up. The over-rev patch stays armed but finds no
`value=500123` component, so it has no visible effect.

The variable is read at startup. Missing or unknown value: injection on. A value
starting with `0`, `n`, `N`, `f`, `F` turns it off.

### 6.2 Disable the whole mod

Untick the mod in the game's **MODS** menu, then restart. The mod then installs
neither the catalogue hooks nor the patch.

### 6.3 Bisection

Create an empty file:

```text
mods\jet_unlimited\.no_catalogue_injection
```

The catalogue insertion is turned off; the diagnostic hooks stay installed.
Delete the file to turn it back on. Useful to tell whether a loading block comes
from the injection or from the world.

---

## 7. Uninstalling

1. Close the game.
2. Delete `mods\jet_unlimited\`.
3. To remove the loader: delete `iphlpapi.dll` from the game folder, or restore
   the saved file.
4. Do not touch `rom/`, `%APPDATA%\Stormworks` or `stormworks64.exe`.

Neither the mod nor the loader modifies any game file; uninstalling is just
deleting the added files.

---

## 8. Regression checks

After an update of the loader or the mod, rebuild, run the tests, then
reinstall:

```bat
build_mod.bat
tests\run_tests.bat
```

Expected:

```text
Build successful:
All mod logic tests passed.
```

The loader's own tests are run from the StormFabric repository
(`tests\run_tests.bat`).

---

## 9. Troubleshooting

| Symptom | Check |
|---|---|
| The game no longer starts | `iphlpapi.dll` is x64, game version, conflict with another proxy |
| No `Jet:` line in the log | the loader is not loaded (wrong folder, unrecognised binary) |
| Component missing | `SW_JET_CATALOGUE=0`? `.no_catalogue_injection` marker? mod enabled in **MODS**? XML present? |
| Component present but explodes | patch not installed: look for `Over-rev: site signature ...` or `Over-rev: unexpected site constant` |
| Component disappears from a vehicle | check `Jet: lookup ... -> injected on demand` and the `Clear` lines (`registry=` vs `injection=`); see 2.0.1 in the CHANGELOG |
| `LoadLibrary failed` | 32-bit DLL, missing dependency, or wrong `dll` name in `mod.json` |
| Crash on exit | outdated loader or mod; do not mix copies |

The log is the first source: `StormworksModLoader.log`, look for `Jet:`,
`Jet[diag]` and `Over-rev:`.
