# How it works — Jet Exhaust Unlimited

This document describes the mod's actual implementation, as it exists in
`src/jet_patch.cpp` and `src/jet_catalogue.cpp`. Values in parentheses refer to
constants in the code; they can be checked with a plain search.

---

## 1. Overview

The mod adds a combustion chamber component without the automatic destruction on
over-rev. It replaces no game file and does not modify the vanilla definition.

Two separate modules, compiled into **the mod's DLL** (the loader knows nothing
about them):

| Module | Role | Entry point |
|---|---|---|
| `jet_catalogue` | clones a vanilla definition and inserts it into the game's registry, in memory | `InitJetCatalogueInjection()` |
| `jet_patch` | diverts the "RPS > 200.0f" test for the `value=500123` component only | `InitOverrevPatch()` |

Every address comes from the loader's discovery profile (engine roles) or from a
signature owned by the mod and resolved by `sw_engine_signature_find`. Code
writes go through `sw_hook_inline`, which validates the prologue, provides the
trampoline and restores the prologue exactly.

The link between the two is the definition's `value` field:

```
Mod XML: value="500123"
        │
        ├──► catalogue: the clone gets value = 500123
        │
        └──► patch: the stub tests [def+0x2b4] == 500123
```

Without the catalogue, the patch never sees a `value=500123` component: it is
armed but has no effect. Without the patch, the component exists but explodes
like a vanilla chamber. Both are needed for the full behaviour.

---

## 2. Catalogue injection

### 2.1 Why an in-memory clone

The game's inventory catalogue is built from internal data and compiled
components, not just by listing `rom/data/definitions`. Adding an XML to `rom/`
is not enough: the name may show up in a tooltip, but the catalogue entry stays a
placeholder.

The chosen solution is to **clone an already loaded definition** (the vanilla
combustion chamber) and insert it into the structures the game walks. The game
then makes no difference between this clone and an official definition: same
native class, same meshes, same surfaces, same logic nodes.

### 2.2 Registry structures

The component registry is a world object (`world + 0xbb670`). The rings are
24-byte `{ void** buf; uint32_t cap; int32_t head; uint32_t count; }` structures
(`stride 0x18`):

| Offset | Ring | Content |
|---:|---|---|
| `+0xc8` | variants | the 8 variants of every definition |
| `+0xe0` | XML descriptors | 32-byte `{id, len, cap, xml, xmlLen, xmlCap}` structures |
| `+0xf8` | global | variant 0 of every component |
| `+0x110 + cat*0x18` | category buckets | 16 buckets, read by the workbench catalogue |

A `ComponentDefinition` is allocated as 8 variants (indices 0..7, symmetry
combinations). Fields used:

| Offset | Type | Role |
|---:|---|---|
| `+0x40` | `uint8_t` | variant index |
| `+0x288` | `char*` | technical id |
| `+0x290` / `+0x294` | `uint32_t` | id length / capacity |
| `+0x298` | `char*` | display name |
| `+0x2a0` / `+0x2a4` | `uint32_t` | name length / capacity |
| `+0x2a8` | `int32_t` | category (11 = JET ENGINES) |
| `+0x2b4` | `uint32_t` | `value` (price) — identity of the bypass |
| `+0x2b8` | `uint64_t` | flags |

### 2.3 Hooks

`InitJetCatalogueInjection()` first reads the engine build certified by the
loader (it picks the definition size, `0x2d08` or `0x2848`), then asks the
loader's discovery profile for four engine services through `findSymbol`:

| Role | Function | RVA 1.15.23 / 1.15.18 |
|---|---|---|
| `engine.component.lookup` | `FindComponentDefinitionByName` | `0x46e900` / `0x46f380` |
| `engine.component.clear` | `ComponentRegistry::Clear` | `0x5f1ac0` / `0x5f1fd0` |
| `engine.localization.register` | localisation table insert | `0x1f3b30` / `0x1f5610` |
| `engine.component.grow` | `RingBuffer::Grow` | `0x8de0b0` / `0x8dd150` |

The RVAs are given for reference only: the loader validates them against its
byte contracts; the mod never contains them. If one service is missing, no hook
is installed.

It then installs two hooks with `hookInline`:

1. **`FindComponentDefinitionByName`**: 16-byte prologue (the instruction
   boundaries are checked by `static_assert`). The registry comes in `RCX`, the
   name in `RDX`. The hook reads the name, captures the registry and tries a
   reinjection.
2. **`ComponentRegistry::Clear`**: 18-byte prologue. Before the engine clears the
   registry, the hook removes all our entries from its rings (by id and by
   identity of the published block). This is essential: otherwise the engine
   would destroy our definitions with its own allocators and the `0x2d08` /
   `0x2848` size. Only the registry being cleared is swept: the game also clears
   a secondary registry (vehicle brought back from the map to the workbench) while
   the main one stays alive, and our entries must stay in the main one (2.0.1).

The loader writes the jumps, checks that the prologue bytes match what the mod
read, provides the trampoline used as the original function, and restores the
bytes on `hookRestore`.

### 2.4 Building the clone

`PrepareDefinition()`:

1. looks for the `jet_engine_combustion_chamber` source in the registry (exact
   search, then a tolerant one);
2. reads its category, `value` and `flags`;
3. reads and validates the mod's XML (max 256 KB, no embedded NUL);
4. allocates **8 separate variants** with the game's allocator
   (`native->gameAlloc(size)`) where `size` is
   `0x2d08` (1.15.23) or `0x2848` (1.15.18);
5. copies the matching source variant into each variant;
6. rewrites the 8 variant pointers so they all point to the set of clones;
7. replaces id, name, variant index, category, `value`, `flags`;
8. builds the 32-byte XML descriptor (id + XML read from the mod's file);
9. publishes the clone into the rings.

The id/name strings are allocated by `StrMake` (the loader's `stringMake`), so
by the game's allocator (`_aligned_free` compatible). The descriptor is
allocated by `gameAlloc(32)` and can be released by the game's
`operator delete(ptr, 32)`.

The category and flags are **read from the source**:

```cpp
category = info.category;                       // 11 for the chamber
flags    = info.flags & ~0x880ull;              // 0x80/0x800 block the bucket
```

Bits `0x80` and `0x800` are cleared because, in the registration function, they
skip the insertion into the category bucket — and so the presence in the
workbench catalogue. The other flags are kept.

### 2.5 Insertion and de-duplication

`InsertDefinition()` publishes the clone into:

1. the variants ring (`+0xc8`): the 8 variants;
2. the descriptors ring (`+0xe0`): the XML descriptor;
3. the global ring (`+0xf8`): variant 0;
4. the category bucket (`+0x110 + cat*0x18`): variant 0.

Every insertion is checked. On the first refusal, **all** entries already
published are removed: never a partial state in the vanilla rings. The clone and
the descriptor stay built for a later attempt (the game may allocate the ring
later).

De-duplication is twofold:

- `JetCatalogueLazyPlan(true, published)` returns `kPresent` if our entries are
  already in **this** registry;
- `IsPublishedEntry()` compares the pointer with our published blocks, whatever
  their fields (so even if the id was corrupted).

### 2.6 Ring growth

`RingPush` handles three cases (`JetCatalogueRingGrowth`):

| `cap` | `count` | Decision |
|---:|---:|---|
| `0` | `0` | **allocation** allowed, even inside a game frame |
| `>0` | `< cap` | nothing to do |
| `>0` | `>= cap` | `realloc` **always refused** |

A `cap=0` ring was never allocated: there is no buffer the engine could walk in
the same frame, so allocating it is safe. A really full ring (`cap>0`) is never
reallocated from a game-frame hook: we wait for a UI pulse that allows growth.

This distinction fixes the historical deadlock: the code mistook
`cap=0,count=0` (empty, never allocated ring) for "full ring", and refused the
injection during the whole load.

### 2.7 On-demand injection and reinjection

The game resolves a vehicle's ids **during deserialization** (back to the
workbench, loading a save), before any UI pulse. A polling-driven injection
arrives too late: the lookup returns null and the game removes the component from
the build.

The lookup hook therefore triggers the injection **on the first lookup that
comes** as soon as `JetCatalogueNeedsInjection(registered, builtForThisRegistry)`
is true:

- `registered == false` (after `Clear`) → reinject;
- `builtForThisRegistry == false` (second registry: world vs workbench) →
  reinject;
- already published in this registry → do nothing.

The lookup hook **never** causes a ring `realloc` (`allowGrow=false`). The
defensive pacing is 10 ms (`kReinjectMinPeriodMs`), so that 8 × the definition
size (`0x2d08` on 1.15.23, `0x2848` on 1.15.18) is not redone on every lookup,
without bringing back the 250 ms window.

### 2.8 Localisation

After a successful insertion, `RegisterAllLocalization()` registers the keys:

```text
DEF_JET_ENGINE_COMBUSTION_CHAMBER_UNLIMITED_NAME
def_jet_engine_combustion_chamber_unlimited_name
def_jet_engine_combustion_chamber_unlimited_desc
def_jet_engine_combustion_chamber_unlimited_s_desc
def_jet_engine_combustion_chamber_unlimited_node_<n>_label
def_jet_engine_combustion_chamber_unlimited_node_<n>_desc
```

The texts are ASCII English. The game's font does not render some accented
characters such as `É` correctly, which is why display names avoid accents.

### 2.9 Teardown

`ShutdownJetCatalogueInjection()`:

1. disarms the injection (`shuttingDown = true`);
2. removes our entries from the rings before the engine's `Clear`;
3. restores the prologues of both hooks;
4. forgets the state.

The mod **never** frees game memory (neither our strings nor our clones). The
leaks are bounded and deliberate: a teardown that cannot fault is better than a
`free` with the wrong allocator. The trampolines are not freed for the same
reason (a game thread may still run them at `PROCESS_DETACH` time).

---

## 3. Over-rev patch

### 3.1 Semantics of the original code

In the `jet_engine_component_type == 2` branch of the jet engine update:

```c
if (state[0x35] != 0 &&
    state[0x4a] != 0 &&
    DAT_140bb07d8 < *(float*)(this + 0x1bc) &&   // 200.0f < RPS
    *(char*)(this + 0x4d9) == 0) {
    *(char*)(this + 0x4d9) = 1;
    /* explosion */
}
```

The `DAT_140bb07d8` constant is `200.0f` (`0x43480000`) and is shared by more
than 50 functions. The `+0x4d9` guard is also used by the "destroyed by damage"
explosion: presetting it would remove both explosions. The mod therefore does
**not** touch the guard; it only neutralises the RPS test.

### 3.2 Signature

32 bytes, `rel32` as wildcards (`kSiteSignature`, `src/jet_patch.h`):

```text
F3 0F 10 87 BC 01 00 00   movss xmm0,[rdi+0x1bc]
F3 0F 10 35 ?? ?? ?? ??   movss xmm6,[rip+rel32]
0F 2F C6                  comiss xmm0,xmm6
0F 86 ?? ?? ?? ??         jbe  rel32
44 38 BF D9 04 00 00      cmp  byte [rdi+0x4d9],0
```

The search is done by the loader's `findSignature`, which requires **exactly
one** match (`SW_CONFLICT` when there are several). The wildcard offsets are
(`BuildSiteMask`):

- `12..15`: `rel32` of the `movss` to the constant;
- `20..23`: `rel32` of the `jbe`.

`InitOverrevPatch()` then reads the `rel32` at offset 12 and the constant at
`site + 16 + rel` (`rip` is after the 8-byte `movss` instruction). It requires
`0x43480000` (`200.0f`). A different constant means the site changed meaning:
no patch.

### 3.3 Patch window

The window is 19 bytes (`kPatchBytes`): `movss` (8) + `movss` (8) +
`comiss` (3). The original `jbe` (6 bytes) is not included; it stays in place.

```text
site + 0   : jump to the stub (written by hookInline, NOP padding)
site + 19  : 0F 86 <rel32>              ; original jbe, untouched
```

The stub block (`kStubBlockBytes = 0x50`) is allocated by the loader's
`codeAllocNear` next to the site, filled by `BuildJetStub` and written with
`writeMemory`. The mod reads the 19 original bytes and passes them to
`hookInline` as the expected contract: if the bytes differ, the loader refuses
the hook. Any failure frees the stub and leaves the game untouched.

### 3.4 Stub

The constants are in the same block, at `0x47` (`200.0f`) and `0x4b` (`+inf`),
so the `rip`-relative offsets are fixed and can be tested off-game.

| Offset | Bytes | Instruction |
|---:|---|---|
| `0x00` | `50` | `push rax` |
| `0x01` | `41 52` | `push r10` |
| `0x03` | `48 8B 47 58` | `mov rax,[rdi+0x58]` (definition) |
| `0x07` | `F3 0F 10 35 <d>` | `movss xmm6,[rip+d]` → `200.0f` |
| `0x0f` | `48 85 C0` | `test rax,rax` |
| `0x12` | `74 17` | `jz .go` |
| `0x14` | `41 BA <imm32>` | `mov r10d,500123` |
| `0x1a` | `44 39 90 B4 02 00 00` | `cmp dword [rax+0x2b4],r10d` |
| `0x21` | `75 08` | `jne .go` |
| `0x23` | `F3 0F 10 35 <d>` | `movss xmm6,[rip+d]` → `+inf` |
| `0x2b` | `F3 0F 10 87 BC 01 00 00` | `.go: movss xmm0,[rdi+0x1bc]` |
| `0x33` | `0F 2F C6` | `comiss xmm0,xmm6` |
| `0x36` | `41 5A` | `pop r10` |
| `0x38` | `58` | `pop rax` |
| `0x39` | `FF 25 00 00 00 00 <imm64>` | `jmp [rip+0]` → `site + 19` |
| `0x47` | float | `200.0f` |
| `0x4b` | float | `+inf` |

Key points:

- **XMM0**, not XMM1: the original code loads the RPS into `XMM0` and the game
  reads it back after the site. Writing `XMM1` would leave `XMM0` stale.
- `pop` and `jmp` do not change the flags: the original `jbe` sees the result of
  the stub's `comiss`.
- `test rax,rax` / `jz` avoids any null dereference if the definition is not set
  yet: the vanilla limit applies. The stub has no SEH.
- The stub compares no string per tick: a single integer comparison.

### 3.5 `+inf` and NaN

The original `jbe` is taken if `CF=1` or `ZF=1`. With `xmm6 = +inf`:

| `xmm0` (RPS) | `comiss xmm0, +inf` | `jbe` |
|---|---|---|
| finite | `CF=1`, `ZF=0` (RPS < +inf) | taken |
| `+inf` | `CF=0`, `ZF=1` (equal) | taken |
| `NaN` | `CF=1`, `ZF=1`, `PF=1` (unordered) | taken |

A finite `1e30f` constant covered NaN (`CF=1`) but **not** `+inf`
(`+inf > 1e30f` → `CF=0`, `ZF=0`, so `jbe` not taken → explosion). That is why
the limit is `INFINITY`: `+inf` covers both `+inf` and NaN.

### 3.6 Teardown

`ShutdownOverrevPatch()` restores the 19 original bytes with `hookRestore`,
frees the stub with `codeFree` and resets the state.

---

## 4. Full life cycle

```text
SwModInit
  ├─ InitJetCatalogueInjection(host, xmlPath)
  │    ├─ engine build -> definition size (0x2d08 or 0x2848)
  │    ├─ engine services resolved by the loader profile
  │    └─ hooks FindComponentDefinitionByName + ComponentRegistry::Clear
  └─ InitOverrevPatch(host)
       ├─ FindSignature() -> 1 match
       ├─ ResolveConstant() == 200.0f
       ├─ AllocateNear() -> stub
       └─ InstallPatch(): CALL + NOP, 19 bytes saved

In game
  ├─ lookup -> on-demand injection / reinjection after Clear
  ├─ chamber tick -> patched site -> stub -> +inf or 200.0f
  └─ SwModTick -> JetCatalogueTick(): injection attempt if needed

SwModUnload (then DllMain(PROCESS_DETACH) as a safety net)
  ├─ ShutdownJetCatalogueInjection(): removal from the rings, then hooks
  └─ ShutdownOverrevPatch(): restore the 19 bytes, then the stub
```

---

## 5. Concurrency and re-entrancy

- `g_lock` is a **re-entrant** `CRITICAL_SECTION`: the hook can be called again
  through the trampoline. An `SRWLOCK` would not be re-entrant.
- `g_cat.inInject` prevents re-entrancy during building/insertion.
- `g_cat.clearing` prevents injecting during `ComponentRegistry::Clear`.
- `g_cat.shuttingDown` prevents any write after teardown starts.
- The lookup hook never reallocates a ring (`allowGrow=false`); only the UI pulse
  allows growth, and only for `cap=0`.
- `RemoveOursFromRing()` uses a temporary buffer (or a swap with the last entry
  for rings > 65536): the copy never overwrites itself when `head != 0`.

---

## 6. Disk access

The active path reads:

- `mods/jet_unlimited/jet_engine_combustion_chamber_unlimited.xml` (descriptor);
- `mods/jet_unlimited/.no_catalogue_injection` (existence only, optional marker).

It writes:

- log lines in `StormworksModLoader.log` (through the loader);
- nothing else.

---

## 7. Invariants

1. The vanilla component is never modified.
2. The global `200.0f` constant is never written.
3. The patch site is validated by signature **and** by the constant's value
   before any write.
4. The clone has 8 variants of the exact engine size, without overlap.
5. Our catalogue entries are removed from a registry before the engine clears it.
6. Any anomaly fails closed: no hook, no write, an explicit log line.
7. The patch and the hooks are restored on exit.
