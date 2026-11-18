#pragma once
// Jet Exhaust Unlimited — injection of the component into the game's CATALOGUE,
// in memory (no file written outside the mod folder).
//
// This module is SELF-CONTAINED: it only depends on StormFabric's public SDK
// (sw_mod_sdk.h, native API). Every executable address is resolved by the
// loader's discovery profile; no RVA is hardcoded here.
//
// Principle:
//   1. hooks on `FindComponentDefinitionByName` and `ComponentRegistry::Clear`
//      installed through `sw_hook_inline` (byte contract validated, fail-closed);
//   2. on demand — at the EXACT moment the game asks for our id, or on the first
//      lookup after a Clear — the vanilla definition is cloned and inserted into
//      the rings of the registry received from the engine;
//   3. before the engine destroys a registry's rings, our entries are removed
//      from that registry; teardown restores the prologues exactly
//      (sw_hook_restore).
#include "sw_mod_sdk.h"

#include <cstdint>
#include <string>

namespace jet {

bool InitJetCatalogueInjection(const SwModHost* host, const char* xmlPath);
// Proactive injection outside a game frame (ring growth allowed).
void JetCatalogueTick();
// Idempotent teardown: removes our entries, then restores both hooks.
void ShutdownJetCatalogueInjection();

// ---------------------------------------------------------------------------
// PURE decisions (testable off-game).
//
// Ring growth: `kAllocate` only when the ring was NEVER allocated (cap == 0: no
// buffer to invalidate); `kRealloc` is always refused (real growth is only
// allowed for cap == 0).
enum class RingGrowth { kNone = 0, kAllocate, kRealloc };
RingGrowth JetCatalogueRingGrowth(uint32_t cap, uint32_t count, bool allowGrow, bool hasGrowFn);

enum class LazyResult {
    kNotOurs = 0,    // the requested name is not ours
    kPresent,        // our entries are already in THIS registry
    kInject,         // nothing in this registry: build + insert NOW
    kInjected,       // injection done on demand
    kSourceMissing,  // vanilla source missing: null (fail-closed) + log
    kRefused,        // registry/insertion refused: null (fail-closed) + log
};
LazyResult JetCatalogueLazyPlan(bool isOurs, bool publishedInThisRegistry);
LazyResult JetCatalogueLazyOutcome(bool sourceMissing, bool insertOk);
const char* JetCatalogueLazyResultText(LazyResult result);
bool JetCatalogueNeedsInjection(bool registered, bool builtForThisRegistry);

// Clone geometry compared with the REAL size of an engine ComponentDefinition
// (0x2d08 on 1.15.23, 0x2848 on 1.15.18).
struct CloneGeometry {
    uint64_t variantBytes;
    uint32_t variants;
    uint64_t blockBytes;
    bool overlapping;
};
CloneGeometry JetCatalogueCloneGeometry(uint64_t variantBytes, uint32_t variants,
                                        uint64_t destroyedTail);
constexpr uint64_t kEngineDefinitionBytes = 0x2d08;      // build 2
constexpr uint64_t kEngineDefinitionBytesOld = 0x2848;   // build 1
constexpr uint64_t kEngineDestroyedTail = 0x2d00;        // build 2
constexpr uint64_t kEngineDestroyedTailOld = 0x2840;     // build 1

// ---------------------------------------------------------------------------
// VERIFIED layout of the clone fields on 1.15.23 (evidence:
// docs/REVERSE_ENGINEERING.md §4.4 and docs/LAYOUT_DEFINITION.md).
//
// Each width is the one of the engine instruction that writes/reads the field,
// not a spacing guessed from neighbours:
//   id       string_t {ptr 8, len 4, cap 4}   @0x288
//   name     string_t {ptr 8, len 4, cap 4}   @0x298
//   category int32                            @0x2a8
//   value    int32                            @0x2b4
//   flags    uint64                           @0x2b8  (`mov [r15],rax`)
//
// `flags` is 8 bytes: a 32-bit write does not overwrite the neighbour, but
// leaves the 4 high bytes (0x2bc..0x2bf) in their previous state. The clone must
// therefore copy the source's 64-bit value, not only its low dword. The
// static_asserts below pin this layout down.
struct DefinitionFieldLayout {
    uint32_t idPtr, idLen, idCap;
    uint32_t namePtr, nameLen, nameCap;
    uint32_t category;
    uint32_t value;
    uint32_t flags;
    uint32_t flagsBytes;
};
constexpr DefinitionFieldLayout kDefinitionLayout = {
    0x288, 0x290, 0x294,   // id   : string_t
    0x298, 0x2a0, 0x2a4,   // name : string_t
    0x2a8,                 // category : int32
    0x2b4,                 // value    : int32
    0x2b8, 8,              // flags    : uint64
};
static_assert(kDefinitionLayout.idPtr + 8 == kDefinitionLayout.idLen, "id: 8-byte ptr then len");
static_assert(kDefinitionLayout.idLen + 4 == kDefinitionLayout.idCap, "id: len then cap");
static_assert(kDefinitionLayout.idCap + 4 == kDefinitionLayout.namePtr, "id and name are contiguous");
static_assert(kDefinitionLayout.namePtr + 8 == kDefinitionLayout.nameLen, "name: 8-byte ptr then len");
static_assert(kDefinitionLayout.nameLen + 4 == kDefinitionLayout.nameCap, "name: len then cap");
static_assert(kDefinitionLayout.nameCap + 4 == kDefinitionLayout.category, "name and category are contiguous");
static_assert(kDefinitionLayout.value + 4 == kDefinitionLayout.flags, "4-byte value right before flags");
static_assert(kDefinitionLayout.flagsBytes == 8, "flags is 8 bytes (mov [r15],rax)");
static_assert(kDefinitionLayout.flags + kDefinitionLayout.flagsBytes == 0x2c0,
              "flags spans 0x2b8..0x2c0");
static_assert(kDefinitionLayout.flags + kDefinitionLayout.flagsBytes <= kEngineDefinitionBytes,
              "flags fits in a definition");

// Values written into the clone. The widths are carried by the types: `flags`
// is a uint64_t, never a uint32_t.
struct CloneFieldValues {
    const char* id;
    uint32_t idLen, idCap;
    const char* name;
    uint32_t nameLen, nameCap;
    int32_t category;
    uint32_t value;
    uint64_t flags;
};
// Writes the clone fields at their certified offsets/widths. PURE function (no
// engine access): the off-game tests check that no neighbouring byte is touched
// and that `flags` is written on 8 bytes.
void JetCatalogueWriteCloneFields(uint8_t* dst, const CloneFieldValues& values);

// Size of a definition and offset of the last destroyed field, per build (1 or
// 2, identity provided by the loader's profile). The two builds do NOT share the
// same layout: hardcoding a single value causes a crash.
uint64_t JetCatalogueDefinitionBytes(int build);
uint64_t JetCatalogueDestroyedTail(int build);

// Can an engine string {ptr,len,cap} be destroyed without faulting?
bool JetCatalogueStringCoherent(uint64_t ptr, uint32_t len, uint32_t cap);
// Encodes `jmp [rip+0]` (FF 25 00000000 <imm64>) + NOP up to patchSize.
size_t JetCatalogueBuildJump(uint8_t* out, size_t patchSize, void* target);
}  // namespace jet
