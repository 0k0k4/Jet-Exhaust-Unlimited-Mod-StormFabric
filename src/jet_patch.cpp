#include "jet_patch.h"
#include "jet_sdk.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace jet {
namespace {

struct PatchState {
    const SwModHost* host = nullptr;
    uint8_t* site = nullptr;
    void* stub = nullptr;
    SwHandle handle = 0;
    bool active = false;
};
PatchState g_patch;

// Petit émetteur : les déplacements RIP-relatifs sont calculés sur les offsets
// finaux, donc le bloc est identique et testable hors jeu.
size_t EmitStub(uint8_t* out, uint32_t signature, float hugeLimit, float vanillaLimit,
                void* resume) {
    constexpr size_t kVanilla = 71;
    constexpr size_t kHuge = 75;
    memset(out, 0x90, kStubBlockBytes);
    size_t o = 0;
    out[o++] = 0x50;                                           // push rax
    out[o++] = 0x41; out[o++] = 0x52;                          // push r10
    out[o++] = 0x48; out[o++] = 0x8B; out[o++] = 0x47;
    out[o++] = static_cast<uint8_t>(kDefinitionOffsetFromThis); // mov rax,[rdi+0x58]
    out[o++] = 0xF3; out[o++] = 0x0F; out[o++] = 0x10; out[o++] = 0x35;  // movss xmm6,[rip+vanilla]
    int32_t d = static_cast<int32_t>(kVanilla - (o + 4));
    memcpy(out + o, &d, 4); o += 4;
    out[o++] = 0x48; out[o++] = 0x85; out[o++] = 0xC0;         // test rax,rax
    out[o++] = 0x74; out[o++] = 0x17;                          // jz .go (+23)
    out[o++] = 0x41; out[o++] = 0xBA;                          // mov r10d, signature
    memcpy(out + o, &signature, 4); o += 4;
    out[o++] = 0x44; out[o++] = 0x39; out[o++] = 0x90;         // cmp [rax+0x2b4],r10d
    memcpy(out + o, &kDefinitionOffsetValue, 4); o += 4;
    out[o++] = 0x75; out[o++] = 0x08;                          // jne .go (+8)
    out[o++] = 0xF3; out[o++] = 0x0F; out[o++] = 0x10; out[o++] = 0x35;  // movss xmm6,[rip+huge]
    d = static_cast<int32_t>(kHuge - (o + 4));
    memcpy(out + o, &d, 4); o += 4;
    // .go : reproduit EXACTEMENT les instructions remplacées (XMM0 = RPS).
    out[o++] = 0xF3; out[o++] = 0x0F; out[o++] = 0x10;
    out[o++] = 0x87; out[o++] = 0xBC; out[o++] = 0x01; out[o++] = 0x00; out[o++] = 0x00;
    out[o++] = 0x0F; out[o++] = 0x2F; out[o++] = 0xC6;         // comiss xmm0,xmm6
    out[o++] = 0x41; out[o++] = 0x5A;                          // pop r10
    out[o++] = 0x58;                                           // pop rax
    out[o++] = 0xFF; out[o++] = 0x25; out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 0;
    const uint64_t target = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(resume));
    memcpy(out + o, &target, 8); o += 8;
    memcpy(out + kVanilla, &vanillaLimit, 4);
    memcpy(out + kHuge, &hugeLimit, 4);
    return kStubBlockBytes;
}
}  // namespace

void BuildSiteMask(uint8_t* mask) {
    if (!mask) return;
    memset(mask, 0xFF, kSiteSignatureSize);
    // movss xmm6,[rip+rel32] : opérande à +12..+15 ; jbe rel32 : opérande à +20..+23.
    memset(mask + 12, 0x00, 4);
    memset(mask + 20, 0x00, 4);
}

size_t BuildJetStub(uint8_t* out, size_t capacity, uint32_t signature, float hugeLimit,
                    float vanillaLimit, void* resume) {
    if (!out || capacity < kStubBlockBytes || !resume) return 0;
    return EmitStub(out, signature, hugeLimit, vanillaLimit, resume);
}

bool InitOverrevPatch(const SwModHost* host) {
    if (g_patch.active) return true;   // idempotent
    if (!host || !g_engine.native || !g_engine.base) return false;
    const SwNativeApi* native = g_engine.native;

    uint8_t mask[kSiteSignatureSize];
    BuildSiteMask(mask);
    const void* found = nullptr;
    const SwResult search = native->findSignature(host, kSiteSignature, mask,
                                                 kSiteSignatureSize, &found);
    if (search != SW_OK || !found) {
        LogError("Sur-regime: signature du site %s ; aucun patch",
                 search == SW_CONFLICT ? "ambigue (plusieurs occurrences)" : "absente");
        return false;
    }
    uint8_t* site = const_cast<uint8_t*>(static_cast<const uint8_t*>(found));

    // Constante 200.0f réellement pointée par le movss : refuse un site dont la
    // sémantique aurait changé (fail-closed, avant toute écriture).
    uint8_t operand[4];
    if (native->readMemory(host, site + 12, operand, sizeof(operand)) != SW_OK) return false;
    int32_t displacement = 0;
    memcpy(&displacement, operand, sizeof(displacement));
    uint32_t limitBits = 0;
    if (native->readMemory(host, site + 16 + displacement, &limitBits, sizeof(limitBits)) != SW_OK)
        return false;
    if (limitBits != kExpectedLimitBits) {
        LogError("Sur-regime: constante du site inattendue (0x%08X) ; aucun patch", limitBits);
        return false;
    }
    float vanillaLimit = 0.0f;
    memcpy(&vanillaLimit, &limitBits, sizeof(vanillaLimit));

    void* stub = native->codeAllocNear(host, site, kStubBlockBytes);
    if (!stub) {
        LogError("Sur-regime: allocation du stub executable impossible ; aucun patch");
        return false;
    }
    uint8_t code[kStubBlockBytes];
    if (BuildJetStub(code, sizeof(code), kSignatureValue, INFINITY, vanillaLimit,
                     site + kPatchBytes) != kStubBlockBytes ||
        native->writeMemory(host, stub, code, kStubBlockBytes) != SW_OK) {
        native->codeFree(host, stub);
        LogError("Sur-regime: stub refuse ; aucun patch");
        return false;
    }

    // Contrat exact : les 19 octets lus sont ceux que le hook va remplacer.
    uint8_t expected[kPatchBytes];
    if (native->readMemory(host, site, expected, sizeof(expected)) != SW_OK) {
        native->codeFree(host, stub);
        return false;
    }
    void* trampoline = nullptr;
    SwHandle handle = 0;
    if (native->hookInline(host, site, stub, expected, kPatchBytes, &trampoline, &handle) != SW_OK) {
        native->codeFree(host, stub);
        LogError("Sur-regime: pose du hook refusee par l'API native ; aucun patch");
        return false;
    }
    g_patch.host = host;
    g_patch.site = site;
    g_patch.stub = stub;
    g_patch.handle = handle;
    g_patch.active = true;
    LogInfo("Sur-regime: patch installe (site=%p, stub=%p, seuil vanilla %.1f)",
            (void*)site, stub, vanillaLimit);
    return true;
}

void ShutdownOverrevPatch() {
    if (!g_patch.active) return;
    if (g_engine.native && g_engine.host)
        g_engine.native->hookRestore(g_engine.host, g_patch.handle);
    if (g_engine.native && g_engine.host && g_patch.stub)
        g_engine.native->codeFree(g_engine.host, g_patch.stub);
    g_patch = PatchState{};
    LogInfo("Sur-regime: prologue original restaure");
}

bool OverrevPatchActive() { return g_patch.active; }

}  // namespace jet
