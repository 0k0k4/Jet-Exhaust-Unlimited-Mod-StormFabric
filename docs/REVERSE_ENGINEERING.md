# Reverse engineering — Jet Exhaust Unlimited

This document gathers the addresses, signatures and offsets needed to understand
and maintain the mod. The final reference is the code of `src/jet_patch.cpp` and
`src/jet_catalogue.cpp` (and, for the engine services, the StormFabric loader's
discovery profile): every value below must be findable there with a plain search.

---

## 1. Target binaries

| Build | File | SHA-256 |
|---|---|---|
| 1.15.23 | `stormworks64.exe` | `f9206d85c82f4d02fd0ac391781d19c5c68394a9ffc48accca0e1f5966db8699` |
| 1.15.18 | `stormworks64.exe` | `55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3` |

Default image base: `0x140000000` (RVA = VA − base). Both binaries are x64
executables; the RVAs below do not depend on ASLR.

---

## 2. Method

1. **Ghidra** on `stormworks64.exe` 1.15.23 (a local reference project of the
   game).
2. Locate the jet engine update (`FUN_140350f40`, slot 107 of the vtable
   `0x140b887c0`), then the `jet_engine_component_type == 2` branch (combustion
   chamber).
3. Identify the `200.0f < *(float*)(this + 0x1bc)` test and the
   `*(char*)(this + 0x4d9) == 0` guard.
4. Extract a 32-byte opcode signature around the site, with the `rel32`s as
   wildcards (they change from one build to the other).
5. Carry the signature over to 1.15.18 by comparing bytes (common mask), then
   check that it appears only once in the executable sections.
6. For the catalogue: identify `FindComponentDefinitionByName`,
   `ComponentRegistry::Clear`, the localisation table and `RingBuffer::Grow`,
   then extract 64/72-byte signatures with a mask (`call rel32` and
   rip-relative `lea` are masked).
7. Off-game validation: the tests map the PE image read-only and check the
   signatures, the constants and the decisions.

Tools:

- `tools/ghidra/Dump.java`: Ghidra function dump / decompilation;
- `tools/match_clear.py`: carries the `Clear` signature from 1.15.18 over to
  1.15.23 (and extracts the mask);
- `tools/gen_grow.py`: same method for `RingBuffer::Grow`;
- `tools/crash_dump.py`: minidump parser (modules, stacks, exception,
  symbolisation through the FunctionTable), used for the shutdown crash.

> The scripts `find_catalogue.py`, `gen_sigs*.py`, `compare_builds.py`,
> `read_const.py`, `match_funcs.py`, `match_helpers.py` and `binparse*.py`
> mentioned in some work plans **do not exist** in the repository. The scripts
> actually present and related to the jet are the ones listed above.

---

## 3. Over-rev site

### 3.1 RVA

| Build | Site RVA | Matches |
|---|---:|---:|
| 1.15.23 | `0x35352a` | 1 |
| 1.15.18 | `0x35428a` | 1 |

The site is in a non-discardable executable section. The loader's
`findSignature` finds it without a hardcoded RVA; the RVAs above are witnesses
for validation.

### 3.2 Signature (32 bytes)

```text
F3 0F 10 87 BC 01 00 00   movss xmm0,[rdi+0x1bc]      ; internal RPS
F3 0F 10 35 ?? ?? ?? ??   movss xmm6,[rip+rel32]      ; 200.0f (rel32 wildcard)
0F 2F C6                  comiss xmm0,xmm6
0F 86 ?? ?? ?? ??         jbe  rel32                   ; rel32 wildcard
44 38 BF D9 04 00 00      cmp  byte [rdi+0x4d9],0     ; explosion guard
```

Wildcard offsets in the signature:

| Offset | Size | Field |
|---:|---:|---|
| `12` | 4 | `rel32` of the `movss` to the constant |
| `20` | 4 | `rel32` of the `jbe` |

`kSiteSignatureSize = 32`, `kPatchBytes = 19` (`src/jet_patch.h`).

### 3.3 Resolving the constant

```cpp
int32_t rel = 0;
memcpy(&rel, site + 12, 4);
memcpy(&constBits, site + 16 + rel, 4);   // rip after the movss (8 bytes)
```

Semantic validation: `constBits == 0x43480000` (`200.0f`). A different constant
→ no patch.

### 3.4 Fields

| Expression | Offset | Role |
|---|---:|---|
| `this + 0x58` | `0x58` | pointer to the definition |
| `this + 0x1bc` | `0x1bc` | internal RPS (float) |
| `this + 0x4d9` | `0x4d9` | "already exploded" guard (byte) |
| `def + 0x2b4` | `0x2b4` | `value` (uint32) — identity |
| `def + 0xed4` | `0xed4` | `jet_engine_component_type` (int) |

The field offsets are **identical** on 1.15.23 and 1.15.18.

### 3.5 Window and stub

- Patched window: 19 bytes (`movss` 8 + `movss` 8 + `comiss` 3).
- Patch: a jump to the stub written by the loader's `hookInline`.
- The original `jbe` (6 bytes, at `site+19` in the stream) stays intact.
- Stub: `0x50`-byte block, ends with `jmp [rip+0]` back to `site + 19`;
  constants at `0x47` (`200.0f`) and `0x4b` (`+inf`).
- Allocation: the loader's `codeAllocNear`, close to the site.

Full stub detail: `docs/HOW_IT_WORKS.md` §3.4.

---

## 4. Catalogue

### 4.1 RVAs used

| Role | 1.15.23 | 1.15.18 | Signature size |
|---|---:|---:|---:|
| `FindComponentDefinitionByName` | `0x46e900` | `0x46f380` | 64 |
| `ComponentRegistry::Clear` | `0x5f1ac0` | `0x5f1fd0` | 64 |
| Localisation registration | `0x1f3b30` | `0x1f5610` | 72 |
| `RingBuffer::Grow` | `0x8de0b0` | `0x8dd150` | 64 |

Since 2.0.0 these functions are resolved by the StormFabric loader's discovery
profile (roles `engine.component.lookup`, `engine.component.clear`,
`engine.localization.register`, `engine.component.grow`); the build is detected
by the loader too (`engineBuild`: 2 = 1.15.23, 1 = 1.15.18). The mod contains
none of these RVAs. Historical detection, from before 2.0.0:

```cpp
bool okNew = SigMatches(base + 0x46e900, kFindDefBytes, kFindDefMask, 64);
bool okOld = SigMatches(base + 0x46f380, kFindDefBytes, kFindDefMask, 64);
if (okNew == okOld) return 0;          // neither, or both
return okNew ? 2 : 1;                  // 2 = 1.15.23, 1 = 1.15.18
```

### 4.2 Old injection path (historical)

These RVAs are documented in `docs/jet_exhaust_unlimited.md` and were used for
the injection through a `.bin` file. The current code clones the vanilla
definition in memory and no longer calls them.

| Role | 1.15.23 | 1.15.18 |
|---|---:|---:|
| Definition/component loader | `0x46c180` | `0x46cf70` |
| Factory from a `.bin` | `0x314ce0` | `0x315a90` |
| De-duplication | `0x46e870` | `0x46f2f0` |
| Catalogue registration | `0x46ec30` | `0x46f6b0` |

### 4.3 Layout of a definition

| Item | 1.15.23 | 1.15.18 |
|---|---:|---:|
| Size of a variant | `0x2d08` | `0x2848` |
| Last destroyed field | `0x2d00` | `0x2840` |
| Number of variants | 8 | 8 |

Evidence for `0x2d08` on 1.15.23:

- `0x46EEC0`: `mov ecx,0x2d08 ; call 0x958C28` (operator new of a definition);
- `0x8B9FEE`: `mov edx,0x2d08 ; call 0x95881C` (operator delete(ptr,size) in the
  variants ring destructor);
- `0x87FDB0`: definition destructor, frees fields up to `+0x2D00`.

Applying `0x2848` to 1.15.23 makes each variant overlap the next by `0x4C0`
bytes: destroying variant *k* frees fields of variant *k+1* (double free), then
reads a `(len,cap)` pair back as a pointer (`0x0000002000000000` → `str_free`
reads `[rcx-8]` = `0x0000001FFFFFFFF8`). This is the shutdown crash analysed
with `tools/crash_dump.py`.

### 4.4 Fields of a definition

Widths checked on 1.15.23 (with the instructions in
`docs/LAYOUT_DEFINITION.md`): `flags` is **8 bytes** (`mov [r15],rax` at
`0x1408efb48`), `value` and `category` 4 bytes, `id`/`name` are
`string_t {ptr 8, len 4, cap 4}`. The table below is the source of truth copied
into `kDefinitionLayout` (`src/jet_catalogue.h`), with `static_assert`s.

| Offset | Type | Role |
|---:|---|---|
| `+0x40` | `uint8_t` | variant index |
| `+0x288` | `char*` | id |
| `+0x290` | `uint32_t` | id length |
| `+0x294` | `uint32_t` | id capacity |
| `+0x298` | `char*` | name |
| `+0x2a0` | `uint32_t` | name length |
| `+0x2a4` | `uint32_t` | name capacity |
| `+0x2a8` | `int32_t` | category |
| `+0x2ac` | `int32_t` | neighbour field, not used by the clone |
| `+0x2b0` | `float` | `mass`, not used by the clone |
| `+0x2b4` | `uint32_t` | `value` (price) |
| `+0x2b8` | `uint64_t` | flags (8 bytes, not 4) |

### 4.5 Registry rings

| Offset | Ring | Element |
|---:|---|---|
| `+0xc8` | variants | `ComponentDefinition*` |
| `+0xe0` | XML descriptors | 32-byte structure |
| `+0xf8` | global | `ComponentDefinition*` (variant 0) |
| `+0x110 + cat*0x18` | buckets | `ComponentDefinition*` |

A ring is `{buf +0, cap +8, head +12, count +16}` (24 bytes). `head` can be out
of range; it is normalised modulo `cap`.

The game has at least two live registries (seen on 2026-11-18): the main one,
where the mod injects, and a secondary one that the game clears when a vehicle
is brought back from the map to the workbench. Clearing the secondary one must
not touch the main one.

The 32-byte XML descriptor is:

| Offset | Type | Role |
|---:|---|---|
| `+0x00` | `char*` | id |
| `+0x08` | `uint32_t` | length |
| `+0x0c` | `uint32_t` | capacity |
| `+0x10` | `char*` | XML |
| `+0x18` | `uint32_t` | length |
| `+0x1c` | `uint32_t` | capacity |

---

## 5. Catalogue signatures

The `kFindDefBytes`, `kFindDefMask`, `kClearBytes`, `kClearMask`, `kLocBytes`,
`kLocMask`, `kGrowBytes` and `kGrowMask` tables now live in the StormFabric
loader's discovery profile. They are reproduced in appendix A for reference.

Mask rules:

- `0xFF`: fixed byte, compared;
- `0x00`: wildcard byte (rel32, disp32, absolute address, jump offset).
- The comparison is protected by `__try/__except`: an invalid read returns
  `false` instead of faulting.

---

## 6. Component signature

The runtime identity is not a `mod.sig` file: it is the `500123` constant
(`kSignatureValue`), written into the definition's `value` field.

- XML: `value="500123"`;
- clone: `*(uint32_t*)(dst + 0x2b4) = 500123`;
- stub: `cmp dword [rax+0x2b4], 500123`.

The `value` field is also the component's price. This is deliberate: it serves as
a stable identity, without any string comparison per tick.

---

## 7. Shutdown crash (2026-11-08)

Symptom: `AV 0xC0000005` reading `0x1FFFFFFFF8` at `stormworks64.exe+0x9B2D5D`,
in the engine's string release.

Causal chain:

1. variant size `0x2848` hardcoded on a 1.15.23 build (`0x2d08`);
2. the clone's 8 variants overlap by `0x4C0` bytes;
3. the engine destructor destroys variant *k*, which frees fields of variant
   *k+1*;
4. double free, then `(len,cap)` read back as a pointer (`0x0000002000000000`);
5. `str_free` reads `[rcx-8]` = `0x0000001FFFFFFFF8` → AV.

Fixes:

- `JetCatalogueDefinitionBytes(build)` picks `0x2d08` or `0x2848`;
- `JetCatalogueCloneGeometry` checks that the last destroyed field fits in a
  variant;
- `ShutdownJetCatalogueInjection` removes our entries from the rings before
  `Clear`;
- `RemoveOursFromRing` compacts correctly (temporary buffer or swap with the last
  entry) so it never duplicates an entry when `head != 0`.

The `tools/crash_dump.py` parser replays the analysis: modules, stacks,
exception, memory and symbolisation through the FunctionTable.

---

## 8. Limits of the certification

- Only the two SHA-256 above are certified. A different binary may contain the
  same byte patterns with a different meaning: the mod also requires `200.0f` at
  the site, and the loader validates the catalogue signatures.
- The exact meaning of the `state[0x35]` / `state[0x4a]` flags (probably
  damage/explosions) is not needed by the stub: it does not change them.
- Multiplayer is not certified: an extra definition may desynchronise vanilla
  clients.
- The historical function addresses (section 4.2) are no longer used by the
  current code; do not bring them back without a full RE pass.

---

## Appendix A — Signature tables

```cpp
constexpr uint8_t kFindDefBytes[64] = {0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4C,0x24,0x08,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x58,0x48,0x8B,0xF2,0x4C,0x8B,0xC1,0x33,0xED,0x8B,0xFD,0x89,0x6C,0x24,0x20,0x44,0x8B,0xFD,0x8B,0x81,0x08,0x01,0x00,0x00,0x89,0x84,0x24,0xB0,0x00,0x00,0x00,0x0F,0x1F,0x84,0x00,0x00,0x00,0x00,0x00};
constexpr uint8_t kFindDefMask[64] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF};
constexpr uint8_t kClearBytes[64] = {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x4C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x81,0xC1,0x10,0x01,0x00,0x00,0x4C,0x8D,0x0D,0x00,0x6D,0x2A,0x00,0xBA,0x18,0x00,0x00,0x00,0x44,0x8D,0x42,0xF8,0xE8,0xFA,0x6F,0x36,0x00,0x90,0x33,0xFF,0x39,0xBB,0x00,0x01,0x00,0x00,0x74,0x19,0x48,0x8B,0x8B,0xF8,0x00,0x00,0x00};
constexpr uint8_t kClearMask[64] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF};
constexpr uint8_t kLocBytes[72] = {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xDA,0x4C,0x8B,0xF1,0xE8,0x6D,0xFE,0xFF,0xFF,0x48,0x8D,0xB8,0x48,0xFF,0x00,0x00,0x8B,0x4F,0x10,0x44,0x8B,0x47,0x08,0x41,0x3B,0xC8,0x75,0x1D,0x43,0x8D,0x14,0x00,0xB8,0x20,0x00,0x00,0x00,0x3B,0xD0,0x0F,0x42,0xD0,0x48,0x8B,0xCF,0xE8};
constexpr uint8_t kLocMask[72] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
constexpr uint8_t kGrowBytes[64] = {0x40,0x56,0x57,0x48,0x83,0xEC,0x28,0x48,0x8B,0xF9,0x8B,0xF2,0x3B,0x51,0x08,0x0F,0x84,0xE1,0x00,0x00,0x00,0x48,0x89,0x6C,0x24,0x48,0x85,0xD2,0x0F,0x84,0xBF,0x00,0x00,0x00,0x8B,0xCE,0xBA,0x10,0x00,0x00,0x00,0x48,0xC1,0xE1,0x03,0xE8,0x8A,0x4C,0x0D,0x00,0x48,0x8B,0xE8,0x48,0x8B,0x07,0x48,0x85,0xC0,0x0F,0x84,0xAA,0x00,0x00};
constexpr uint8_t kGrowMask[64] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
```
