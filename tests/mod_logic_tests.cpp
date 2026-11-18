// Off-game tests of the mod's logic (no game code runs).
//
// They ship with the code and no longer depend on loader internals. They check
// the PURE decisions (ring growth, on-demand injection, clone geometry) and the
// machine encoding of the over-rev stub.
#include "../src/jet_catalogue.h"
#include "../src/jet_patch.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

void TestSiteMask() {
    uint8_t mask[jet::kSiteSignatureSize];
    jet::BuildSiteMask(mask);
    for (size_t i = 0; i < jet::kSiteSignatureSize; ++i) {
        const bool wildcard = (i >= 12 && i < 16) || (i >= 20 && i < 24);
        assert(mask[i] == (wildcard ? 0x00 : 0xFF));
    }
    // The rel32s may change without invalidating the signature.
    uint8_t site[jet::kSiteSignatureSize];
    memcpy(site, jet::kSiteSignature, sizeof(site));
    site[13] = 0xAA; site[22] = 0xBB;
    for (size_t i = 0; i < sizeof(site); ++i)
        assert(!mask[i] || site[i] == jet::kSiteSignature[i]);
    // A changed fixed byte breaks the contract.
    site[2] ^= 0xFF;
    bool mismatch = false;
    for (size_t i = 0; i < sizeof(site); ++i)
        if (mask[i] && site[i] != jet::kSiteSignature[i]) mismatch = true;
    assert(mismatch);
    puts("PASS: site signature + rel32 mask");
}

void TestStubEncoding() {
    uint8_t stub[jet::kStubBlockBytes];
    uint8_t resume = 0;
    const float vanilla = 200.0f;
    const size_t size = jet::BuildJetStub(stub, sizeof(stub), 500123u, INFINITY, vanilla, &resume);
    assert(size == jet::kStubBlockBytes);
    // Header: push rax ; push r10 ; mov rax,[rdi+0x58]
    assert(stub[0] == 0x50 && stub[1] == 0x41 && stub[2] == 0x52);
    assert(stub[3] == 0x48 && stub[4] == 0x8B && stub[5] == 0x47 && stub[6] == 0x58);
    // Component signature encoded as imm32 right after 41 BA.
    assert(stub[20] == 0x41 && stub[21] == 0xBA);
    const uint32_t signature = *reinterpret_cast<const uint32_t*>(stub + 22);
    assert(signature == 500123u);
    // Comparison on definition + 0x2b4.
    assert(stub[26] == 0x44 && stub[27] == 0x39 && stub[28] == 0x90);
    assert(*reinterpret_cast<const uint32_t*>(stub + 29) == jet::kDefinitionOffsetValue);
    // Exact copy of the replaced code, then absolute jump to site + 19.
    assert(stub[43] == 0xF3 && stub[46] == 0x87 && stub[47] == 0xBC);
    assert(stub[51] == 0x0F && stub[52] == 0x2F && stub[53] == 0xC6);
    assert(stub[57] == 0xFF && stub[58] == 0x25);
    const uint64_t jump = *reinterpret_cast<const uint64_t*>(stub + 63);
    assert(jump == reinterpret_cast<uint64_t>(&resume));
    // Constants: the vanilla limit is copied, the unlimited limit is +inf.
    assert(*reinterpret_cast<const float*>(stub + 71) == vanilla);
    assert(std::isinf(*reinterpret_cast<const float*>(stub + 75)));
    // Both RIP-relative displacements point to these constants.
    const int32_t vanillaRel = *reinterpret_cast<const int32_t*>(stub + 11);
    assert(11 + 4 + vanillaRel == 71);
    const int32_t hugeRel = *reinterpret_cast<const int32_t*>(stub + 39);
    assert(39 + 4 + hugeRel == 75);
    // A block that is too small is refused (fail-closed).
    uint8_t tiny[16];
    assert(jet::BuildJetStub(tiny, sizeof(tiny), 500123u, INFINITY, vanilla, &resume) == 0);
    puts("PASS: over-rev stub (imm32, +0x2b4, constants, absolute jump)");
}

void TestRingGrowth() {
    using jet::RingGrowth;
    // Room left: nothing to do.
    assert(jet::JetCatalogueRingGrowth(8, 3, true, true) == RingGrowth::kNone);
    // Ring never allocated: allocating invalidates no buffer.
    assert(jet::JetCatalogueRingGrowth(0, 0, false, true) == RingGrowth::kAllocate);
    // Really full ring: realloc ALWAYS refused.
    assert(jet::JetCatalogueRingGrowth(64, 64, true, true) == RingGrowth::kNone);
    // Without a grow function, nothing is possible.
    assert(jet::JetCatalogueRingGrowth(0, 0, true, false) == RingGrowth::kNone);
    puts("PASS: ring growth (allocation for cap==0 only, realloc refused)");
}

void TestLazyInjection() {
    using jet::LazyResult;
    assert(jet::JetCatalogueLazyPlan(false, false) == LazyResult::kNotOurs);
    assert(jet::JetCatalogueLazyPlan(true, true) == LazyResult::kPresent);
    assert(jet::JetCatalogueLazyPlan(true, false) == LazyResult::kInject);
    assert(jet::JetCatalogueLazyOutcome(true, false) == LazyResult::kSourceMissing);
    assert(jet::JetCatalogueLazyOutcome(false, false) == LazyResult::kRefused);
    assert(jet::JetCatalogueLazyOutcome(false, true) == LazyResult::kInjected);
    // Reinjection needed after a Clear or for another registry.
    assert(jet::JetCatalogueNeedsInjection(false, false));
    assert(jet::JetCatalogueNeedsInjection(true, false));
    assert(!jet::JetCatalogueNeedsInjection(true, true));
    puts("PASS: on-demand injection (plan, outcome, reinjection after Clear)");
}

void TestCloneGeometry() {
    // 1.15.23: 0x2d08 per variant, last destroyed field at 0x2d00.
    const auto good = jet::JetCatalogueCloneGeometry(jet::kEngineDefinitionBytes, 8,
                                                     jet::kEngineDestroyedTail);
    assert(good.blockBytes == jet::kEngineDefinitionBytes * 8);
    assert(!good.overlapping);
    // A 0x2848 stride on a 0x2d08 object makes the variants overlap.
    const auto bad = jet::JetCatalogueCloneGeometry(jet::kEngineDefinitionBytesOld, 8,
                                                    jet::kEngineDestroyedTail);
    assert(bad.overlapping);
    // Sizes per build.
    assert(jet::JetCatalogueDefinitionBytes(1) == jet::kEngineDefinitionBytesOld);
    assert(jet::JetCatalogueDefinitionBytes(2) == jet::kEngineDefinitionBytes);
    assert(jet::JetCatalogueDestroyedTail(1) == jet::kEngineDestroyedTailOld);
    assert(jet::JetCatalogueDestroyedTail(2) == jet::kEngineDestroyedTail);
    puts("PASS: clone geometry (no overlap, sizes per build)");
}

void TestStringCoherence() {
    assert(jet::JetCatalogueStringCoherent(0x1000, 5, 32));
    assert(jet::JetCatalogueStringCoherent(0x1000, 0, 32));
    assert(!jet::JetCatalogueStringCoherent(0x1000, 40, 32));   // len > cap
    assert(!jet::JetCatalogueStringCoherent(0, 5, 32));         // null ptr, len > 0
    assert(!jet::JetCatalogueStringCoherent(0x1000, 0xFFFFFFFFu, 64));
    // The (len=0, cap=0x20) pair read back as a pointer is 0x0000002000000000:
    // the clone geometry (not this function) is what forbids that shift.
    uint8_t raw[24] = {};
    const uint32_t len = 0, cap = 0x20;
    memcpy(raw + 8, &len, 4);
    memcpy(raw + 12, &cap, 4);
    uint64_t shifted = 0;
    memcpy(&shifted, raw + 8, 8);
    assert(shifted == 0x0000002000000000ull);
    puts("PASS: engine string coherence");
}

void TestCloneFieldWidths() {
    // Layout certified on 1.15.23 (evidence: docs/LAYOUT_DEFINITION.md).
    const auto& L = jet::kDefinitionLayout;
    assert(L.idPtr == 0x288 && L.idLen == 0x290 && L.idCap == 0x294);
    assert(L.namePtr == 0x298 && L.nameLen == 0x2a0 && L.nameCap == 0x2a4);
    assert(L.category == 0x2a8);
    assert(L.value == 0x2b4);
    assert(L.flags == 0x2b8 && L.flagsBytes == 8);

    uint8_t buf[jet::kEngineDefinitionBytes];
    memset(buf, 0xA5, sizeof(buf));
    const char id[] = "jet_engine_combustion_chamber_unlimited";
    const char name[] = "Jet Combustion Chamber Unlimited";
    jet::CloneFieldValues v{};
    v.id = id; v.idLen = sizeof(id) - 1; v.idCap = 64;
    v.name = name; v.nameLen = sizeof(name) - 1; v.nameCap = 64;
    v.category = 11;
    v.value = 500123;
    v.flags = 0x4000ull;   // vanilla flags of the source (16384)
    jet::JetCatalogueWriteCloneFields(buf, v);

    auto u32 = [&](size_t off) { uint32_t x; memcpy(&x, buf + off, 4); return x; };
    auto u64 = [&](size_t off) { uint64_t x; memcpy(&x, buf + off, 8); return x; };
    auto ptr = [&](size_t off) { const char* x; memcpy(&x, buf + off, 8); return x; };

    // string_t id / name: 8-byte pointer, 4-byte len and cap.
    assert(ptr(L.idPtr) == id);
    assert(u32(L.idLen) == v.idLen && u32(L.idCap) == v.idCap);
    assert(ptr(L.namePtr) == name);
    assert(u32(L.nameLen) == v.nameLen && u32(L.nameCap) == v.nameCap);

    // category: 4 bytes; the next field (0x2ac, not used by the clone) stays
    // intact.
    assert(static_cast<int32_t>(u32(L.category)) == 11);
    for (size_t i = L.category + 4; i < L.value; ++i) assert(buf[i] == 0xA5);

    // value: 4 bytes, immediately followed by flags.
    assert(u32(L.value) == 500123u);

    // flags: 8 bytes written, including the 4 high bytes (0x2bc..0x2bf).
    assert(u64(L.flags) == 0x4000ull);
    for (size_t i = 0; i < 8; ++i)
        assert(buf[L.flags + i] == static_cast<uint8_t>((0x4000ull >> (8 * i)) & 0xFF));
    // The immediate neighbour (0x2c0) is not touched.
    for (size_t i = L.flags + L.flagsBytes; i < L.flags + L.flagsBytes + 16; ++i)
        assert(buf[i] == 0xA5);

    // REGRESSION: a 32-bit store would leave 0x2bc..0x2bf at their previous
    // value (here the sentinel). That is exactly what this test forbids.
    uint8_t partial[jet::kEngineDefinitionBytes];
    memset(partial, 0xA5, sizeof(partial));
    const uint32_t low = 0x4000u;
    memcpy(partial + L.flags, &low, 4);
    for (size_t i = 4; i < 8; ++i) assert(partial[L.flags + i] == 0xA5);

    // A value with non-zero high bytes is copied in full.
    v.flags = 0x1122334455667788ull;
    jet::JetCatalogueWriteCloneFields(buf, v);
    assert(u64(L.flags) == 0x1122334455667788ull);
    puts("PASS: clone field widths (flags 8 bytes, value/category 4 bytes, neighbours intact)");
}

void TestBuildJump() {
    uint8_t patch[24];
    uint8_t target = 0;
    assert(jet::JetCatalogueBuildJump(patch, sizeof(patch), &target) == 24);
    assert(patch[0] == 0xFF && patch[1] == 0x25);
    assert(*reinterpret_cast<const int32_t*>(patch + 2) == 0);
    assert(*reinterpret_cast<const uint64_t*>(patch + 6) ==
           reinterpret_cast<uint64_t>(&target));
    assert(jet::JetCatalogueBuildJump(patch, 8, &target) == 0);
    puts("PASS: absolute jump encoding");
}

}  // namespace

int main() {
    TestSiteMask();
    TestStubEncoding();
    TestRingGrowth();
    TestLazyInjection();
    TestCloneGeometry();
    TestStringCoherence();
    TestCloneFieldWidths();
    TestBuildJump();
    puts("PASS: mod logic suites");
    return 0;
}
