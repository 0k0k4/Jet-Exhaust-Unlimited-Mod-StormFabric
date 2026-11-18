# Mod tests

`mod_logic_tests.cpp` checks only the mod's **pure logic**: no game code runs,
no real hook is installed.

Coverage:

- mask of the over-rev site signature (rel32 wildcards, fixed bytes);
- machine encoding of the stub (`value` as imm32, `definition+0x2b4`, `200.0f` /
  `+inf` constants, absolute jump to `site+19`);
- ring growth (`kAllocate` only for `cap == 0`, realloc refused);
- on-demand injection (`LazyPlan` / `LazyOutcome` / reinjection after Clear);
- clone geometry (overlap forbidden, sizes per build);
- clone field widths (`flags` 8 bytes, `value`/`category` 4 bytes, neighbouring
  bytes intact — see `docs/LAYOUT_DEFINITION.md`);
- engine string coherence;
- absolute jump encoding.

## Running

```bat
cd /d "<path to the Jet Exhaust Unlimited repository>"
cmd /c ".\tests\run_tests.bat"
```

Expected: `All mod logic tests passed.`

The StormFabric SDK must be in a sibling checkout (`..\StormFabric\include`) or
passed as an argument:

```bat
cmd /c ".\tests\run_tests.bat" "<path to StormFabric>\include"
```

The **loader** primitives (signature contract found/missing/ambiguous,
hook/restore, write refusal) are tested in StormFabric: `tests\run_tests.bat`
(`native_prims_tests` suite).
