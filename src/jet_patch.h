#pragma once
// Jet Exhaust Unlimited — over-rev bypass for the ONE component identified by
// its `value` constant.
//
// The site is found by a byte signature owned by the mod (the loader does not
// know this signature); the patch is installed through `sw_hook_inline`: byte
// contract checked, trampoline provided by the loader, idempotent restore. No
// executable address is hardcoded.
#include "sw_mod_sdk.h"

#include <cstddef>
#include <cstdint>

namespace jet {

bool InitOverrevPatch(const SwModHost* host);
void ShutdownOverrevPatch();
bool OverrevPatchActive();

// ---- Pure internals, testable off-game ------------------------------------
// Site signature (32 bytes): movss xmm0,[rdi+0x1bc] ; movss xmm6,[rip+rel32] ;
// comiss ; jbe rel32 ; cmp byte [rdi+0x4d9],0. Both rel32s are wildcards
// (build-specific); every other byte is fixed.
constexpr uint8_t kSiteSignature[] = {
    0xF3, 0x0F, 0x10, 0x87, 0xBC, 0x01, 0x00, 0x00,   // movss xmm0,[rdi+0x1bc]
    0xF3, 0x0F, 0x10, 0x35, 0x00, 0x00, 0x00, 0x00,   // movss xmm6,[rip+rel32]
    0x0F, 0x2F, 0xC6,                                 // comiss xmm0,xmm6
    0x0F, 0x86, 0x00, 0x00, 0x00, 0x00,               // jbe rel32
    0x44, 0x38, 0xBF, 0xD9, 0x04, 0x00, 0x00,         // cmp byte [rdi+0x4d9],0
};
constexpr size_t kSiteSignatureSize = sizeof(kSiteSignature);
constexpr size_t kPatchBytes = 19;       // movss + movss + comiss -> jmp (14) + NOP
constexpr size_t kStubBlockBytes = 0x50;
constexpr uint32_t kExpectedLimitBits = 0x43480000u;  // 200.0f
constexpr uint32_t kSignatureValue = 500123u;         // `value` field of our XML
constexpr uint32_t kDefinitionOffsetValue = 0x2b4;    // (definition + 0x2b4) == value
constexpr uint32_t kDefinitionOffsetFromThis = 0x58;  // this->definition

// Fills `mask` (same length): 0xFF for the fixed bytes, 0x00 for the two rel32
// operands.
void BuildSiteMask(uint8_t* mask);
// Self-contained machine stub (no trampoline needed):
//   vanilla limit by default, +inf if definition->value == signature, then
//   replays movss/comiss and jumps to `resume` (= site + kPatchBytes).
size_t BuildJetStub(uint8_t* out, size_t capacity, uint32_t signature, float hugeLimit,
                    float vanillaLimit, void* resume);

}  // namespace jet
