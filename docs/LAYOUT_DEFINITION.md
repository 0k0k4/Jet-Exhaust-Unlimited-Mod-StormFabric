# Layout of a `ComponentDefinition` (Stormworks 1.15.23) — evidence

This document pins down the **offsets and widths** of the fields the mod's clone
writes. It answers one precise question: is `flags` (`+0x2b8`) 4 or 8 bytes wide?

**Conclusion: 8 bytes.** The mod already writes a `uint64_t`; this document and
the `static_assert`s in `src/jet_catalogue.h` prevent a regression to a 32-bit
store.

## Method

Static analysis of `stormworks64.exe` (1.15.23, image base `0x140000000`) with
Ghidra 12.1.4 headless, on a local Ghidra project of the game, script
`StormFabric/tools/ghidra/Dump.java` (commands `A`/`I`/`D`).
The addresses below are **absolute addresses** (RVA + `0x140000000`).

## Certified table

| Field | Offset | Width | Evidence (engine instruction) |
|---|---:|---:|---|
| variant index | `+0x40` | 1 | `uint8_t` written by the clone; not read back by the engine at the checked sites |
| `id` pointer | `+0x288` | 8 | `0x14046ed20`: `FUN_14017cbb0(pcVar5+0x288, len)` writes the pointer; `0x14046e550`: `MOV RDX,[RBX+0x288]` |
| `id` length | `+0x290` | 4 | `0x14046ed20`: `MOV dword ptr [pcVar5+0x290], len`; `0x14046e550`: `MOV [..+0x290]` (uint32) |
| `id` capacity | `+0x294` | 4 | `FUN_14017cbb0`: `MOV dword ptr [RDI+0xc], EBX` (string_t = `{ptr,len,cap}`) |
| `name` pointer | `+0x298` | 8 | `0x140190c40`: `MOV RCX,R14` then `FUN_14017cbb0(R14, len)`; `R14 = &def->name` |
| `name` length | `+0x2a0` | 4 | `0x140190c40`: `MOV dword ptr [R14+0x8], EAX` |
| `name` capacity | `+0x2a4` | 4 | `FUN_14017cbb0`: `MOV dword ptr [RDI+0xc], EBX` |
| `category` | `+0x2a8` | 4 | `0x14093d1d0`: `MOV dword ptr [R15], EAX`; read at `0x14046e7e4`: `MOVSXD RAX,dword ptr [RSI+0x2a8]` |
| neighbour field (unused) | `+0x2ac` | 4 | `0x14093d410`: `MOV dword ptr [R15], EAX` |
| `mass` (unused) | `+0x2b0` | 4 | `0x1408efd50`: `MOVSS dword ptr [R15], XMM1` |
| `value` | `+0x2b4` | 4 | `0x1408ef630`: `MOV dword ptr [R15], EAX` (after `CALL 0x1409b3b94`) |
| **`flags`** | **`+0x2b8`** | **8** | **written by `0x1408efaf0`: `MOV qword ptr [R15], RAX` at `0x1408efb48`** |
| end of `flags` | `+0x2c0` | — | the next named field (`reward_tier`) is at `+0x2e0` |

## Independent evidence for `flags`

### 1. Write — parsing helper `0x1408efaf0`

```
1408efb43  CALL 0x1409b3b94          ; XML string -> 64-bit integer (RAX)
1408efb48  MOV qword ptr [R15],RAX   ; 8-byte store into def->flags
```

`R15` receives `R9`, i.e. the `&this->flags` pointer passed by `0x1403168d0`
(`LEA R9,[RSI+0x2b8]` at `0x140316b26`). The store is **64 bits**.

### 2. 64-bit read — bucket registration `0x14046ec30`

```
14046ec9c  MOV RAX,qword ptr [RBX+0x2b8]   ; 8-byte read
14046eca3  TEST RAX,0x80
14046ecab  TEST RAX,0x800
```

This is the site that decides whether the definition goes into its category
bucket (bits `0x80`/`0x800` skip it). It reads **8 bytes**.

### 3. 32-bit read of the low dword — `ComponentRegistry::Clear` `0x14046e750`

```
14046e7d6  MOV EAX,dword ptr [RSI+0x2b8]   ; low dword only
14046e7dc  TEST RAX,0x20000000
```

The engine tests a bit of the **low dword** with a 32-bit read. This is
consistent with a 64-bit field whose useful value fits in the low 32 bits: both
sites read the same value, at different widths.

### 4. `value` right before it is 32 bits

```
1408ef683  CALL 0x1409b3b94
1408ef688  MOV dword ptr [R15],EAX   ; value = 4 bytes
```

`value` (`+0x2b4`) and `flags` (`+0x2b8`) are therefore two separate, contiguous
fields: `value` on 4 bytes, `flags` on 8.

## Expected value

The vanilla definition `rom/data/definitions/jet_engine_combustion_chamber.xml`
has `flags="16384"` = `0x4000`. The high bytes of the 64-bit field are therefore
zero; the clone still copies the full **64-bit** value (`info.flags & ~0x880`),
not only its low dword.

## Consequences for the clone

- `flags` must be written with a 64-bit store. A 32-bit store would not overwrite
  the neighbour (`+0x2bc` is still inside the field), but would leave the 4 high
  bytes in their previous state — a non-zero 64-bit value would be truncated.
- `value` and `category` stay 32-bit stores.
- `id`/`name` are `string_t` `{ptr 8, len 4, cap 4}`.

The `TestCloneFieldWidths` test (`tests/mod_logic_tests.cpp`) fills a
`0x2d08`-byte buffer with a sentinel, writes the fields through
`JetCatalogueWriteCloneFields`, then checks that the 8 bytes of `flags` are
written, that `+0x2c0` is not touched, and that a 32-bit store would leave
`+0x2bc..+0x2bf` at the sentinel.
