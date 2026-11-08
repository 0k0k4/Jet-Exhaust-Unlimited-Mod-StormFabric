#pragma once
// Jet Exhaust Unlimited — contournement du sur-régime (over-rev) pour le SEUL
// composant identifié par sa constante `value`.
//
// Le site est localisé par une signature d'octets propre au mod (le loader ne
// connaît pas cette signature) ; le patch est posé via `sw_hook_inline` :
// contrat d'octets vérifié, trampoline fourni par le loader, restauration
// idempotente. Aucune adresse d'exécutable n'est codée en dur.
#include "sw_mod_sdk.h"

#include <cstddef>
#include <cstdint>

namespace jet {

bool InitOverrevPatch(const SwModHost* host);
void ShutdownOverrevPatch();
bool OverrevPatchActive();

// ---- Interne pur, testable hors jeu ---------------------------------------
// Signature du site (30 octets) : movss xmm0,[rdi+0x1bc] ; movss xmm6,[rip+rel32] ;
// comiss ; jbe rel32 ; cmp byte [rdi+0x4d9],0. Les deux rel32 sont des jokers
// (spécifiques au build) ; tous les autres octets sont fixes.
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
constexpr uint32_t kSignatureValue = 500123u;         // champ `value` de notre XML
constexpr uint32_t kDefinitionOffsetValue = 0x2b4;    // (definition + 0x2b4) == value
constexpr uint32_t kDefinitionOffsetFromThis = 0x58;  // this->definition

// Remplit `mask` (même longueur) : 0xFF pour les octets fixes, 0x00 pour les
// deux opérandes rel32.
void BuildSiteMask(uint8_t* mask);
// Stub machine autonome (aucun trampoline nécessaire) :
//   limite vanilla par défaut, +inf si definition->value == signature, puis
//   reproduit movss/comiss et saute en `resume` (= site + kPatchBytes).
size_t BuildJetStub(uint8_t* out, size_t capacity, uint32_t signature, float hugeLimit,
                    float vanillaLimit, void* resume);

}  // namespace jet
