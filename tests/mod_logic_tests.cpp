// Tests hors jeu de la logique du mod (aucune exécution de code du jeu).
//
// Portés avec le code : ils ne dépendent plus des internes du loader. Ils
// vérifient les décisions PURES (croissance de ring, injection à la demande,
// géométrie du clone) et l'encodage machine du stub de sur-régime.
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
    // Les rel32 peuvent changer sans invalider la signature.
    uint8_t site[jet::kSiteSignatureSize];
    memcpy(site, jet::kSiteSignature, sizeof(site));
    site[13] = 0xAA; site[22] = 0xBB;
    for (size_t i = 0; i < sizeof(site); ++i)
        assert(!mask[i] || site[i] == jet::kSiteSignature[i]);
    // Un octet fixe modifié casse le contrat.
    site[2] ^= 0xFF;
    bool mismatch = false;
    for (size_t i = 0; i < sizeof(site); ++i)
        if (mask[i] && site[i] != jet::kSiteSignature[i]) mismatch = true;
    assert(mismatch);
    puts("PASS: signature du site + masque des rel32");
}

void TestStubEncoding() {
    uint8_t stub[jet::kStubBlockBytes];
    uint8_t resume = 0;
    const float vanilla = 200.0f;
    const size_t size = jet::BuildJetStub(stub, sizeof(stub), 500123u, INFINITY, vanilla, &resume);
    assert(size == jet::kStubBlockBytes);
    // En-tête : push rax ; push r10 ; mov rax,[rdi+0x58]
    assert(stub[0] == 0x50 && stub[1] == 0x41 && stub[2] == 0x52);
    assert(stub[3] == 0x48 && stub[4] == 0x8B && stub[5] == 0x47 && stub[6] == 0x58);
    // Signature du composant encodée en imm32 immédiatement après 41 BA.
    assert(stub[20] == 0x41 && stub[21] == 0xBA);
    const uint32_t signature = *reinterpret_cast<const uint32_t*>(stub + 22);
    assert(signature == 500123u);
    // Comparaison sur definition + 0x2b4.
    assert(stub[26] == 0x44 && stub[27] == 0x39 && stub[28] == 0x90);
    assert(*reinterpret_cast<const uint32_t*>(stub + 29) == jet::kDefinitionOffsetValue);
    // Copie fidèle du code remplacé puis saut absolu vers le site + 19.
    assert(stub[43] == 0xF3 && stub[46] == 0x87 && stub[47] == 0xBC);
    assert(stub[51] == 0x0F && stub[52] == 0x2F && stub[53] == 0xC6);
    assert(stub[57] == 0xFF && stub[58] == 0x25);
    const uint64_t jump = *reinterpret_cast<const uint64_t*>(stub + 63);
    assert(jump == reinterpret_cast<uint64_t>(&resume));
    // Constantes : la limite vanilla est recopiée, la limite illimitée est +inf.
    assert(*reinterpret_cast<const float*>(stub + 71) == vanilla);
    assert(std::isinf(*reinterpret_cast<const float*>(stub + 75)));
    // Les deux déplacements RIP-relatifs pointent bien sur ces constantes.
    const int32_t vanillaRel = *reinterpret_cast<const int32_t*>(stub + 11);
    assert(11 + 4 + vanillaRel == 71);
    const int32_t hugeRel = *reinterpret_cast<const int32_t*>(stub + 39);
    assert(39 + 4 + hugeRel == 75);
    // Un bloc trop petit est refusé (fail-closed).
    uint8_t tiny[16];
    assert(jet::BuildJetStub(tiny, sizeof(tiny), 500123u, INFINITY, vanilla, &resume) == 0);
    puts("PASS: stub de sur-regime (imm32, +0x2b4, constantes, saut absolu)");
}

void TestRingGrowth() {
    using jet::RingGrowth;
    // Place disponible : rien à faire.
    assert(jet::JetCatalogueRingGrowth(8, 3, true, true) == RingGrowth::kNone);
    // Ring jamais alloué : allouer n'invalide aucun buffer.
    assert(jet::JetCatalogueRingGrowth(0, 0, false, true) == RingGrowth::kAllocate);
    // Ring réellement plein : realloc TOUJOURS refusé.
    assert(jet::JetCatalogueRingGrowth(64, 64, true, true) == RingGrowth::kNone);
    // Sans fonction de croissance, rien n'est possible.
    assert(jet::JetCatalogueRingGrowth(0, 0, true, false) == RingGrowth::kNone);
    puts("PASS: croissance de ring (allocation cap==0 seulement, realloc refuse)");
}

void TestLazyInjection() {
    using jet::LazyResult;
    assert(jet::JetCatalogueLazyPlan(false, false) == LazyResult::kNotOurs);
    assert(jet::JetCatalogueLazyPlan(true, true) == LazyResult::kPresent);
    assert(jet::JetCatalogueLazyPlan(true, false) == LazyResult::kInject);
    assert(jet::JetCatalogueLazyOutcome(true, false) == LazyResult::kSourceMissing);
    assert(jet::JetCatalogueLazyOutcome(false, false) == LazyResult::kRefused);
    assert(jet::JetCatalogueLazyOutcome(false, true) == LazyResult::kInjected);
    // Ré-injection requise après un Clear ou pour un autre registre.
    assert(jet::JetCatalogueNeedsInjection(false, false));
    assert(jet::JetCatalogueNeedsInjection(true, false));
    assert(!jet::JetCatalogueNeedsInjection(true, true));
    puts("PASS: injection a la demande (plan, issue, re-injection apres Clear)");
}

void TestCloneGeometry() {
    // 1.15.23 : 0x2d08 par variante, dernier champ détruit à 0x2d00.
    const auto good = jet::JetCatalogueCloneGeometry(jet::kEngineDefinitionBytes, 8,
                                                     jet::kEngineDestroyedTail);
    assert(good.blockBytes == jet::kEngineDefinitionBytes * 8);
    assert(!good.overlapping);
    // Un pas de 0x2848 sur un objet 0x2d08 fait empiéter les variantes.
    const auto bad = jet::JetCatalogueCloneGeometry(jet::kEngineDefinitionBytesOld, 8,
                                                    jet::kEngineDestroyedTail);
    assert(bad.overlapping);
    // Tailles par build.
    assert(jet::JetCatalogueDefinitionBytes(1) == jet::kEngineDefinitionBytesOld);
    assert(jet::JetCatalogueDefinitionBytes(2) == jet::kEngineDefinitionBytes);
    assert(jet::JetCatalogueDestroyedTail(1) == jet::kEngineDestroyedTailOld);
    assert(jet::JetCatalogueDestroyedTail(2) == jet::kEngineDestroyedTail);
    puts("PASS: geometrie du clone (non-chevauchement, tailles par build)");
}

void TestStringCoherence() {
    assert(jet::JetCatalogueStringCoherent(0x1000, 5, 32));
    assert(jet::JetCatalogueStringCoherent(0x1000, 0, 32));
    assert(!jet::JetCatalogueStringCoherent(0x1000, 40, 32));   // len > cap
    assert(!jet::JetCatalogueStringCoherent(0, 5, 32));         // ptr nul, len > 0
    assert(!jet::JetCatalogueStringCoherent(0x1000, 0xFFFFFFFFu, 64));
    // La paire (len=0, cap=0x20) relue comme un pointeur vaut 0x0000002000000000 :
    // c'est la geometrie du clone (pas cette fonction) qui interdit ce decalage.
    uint8_t raw[24] = {};
    const uint32_t len = 0, cap = 0x20;
    memcpy(raw + 8, &len, 4);
    memcpy(raw + 12, &cap, 4);
    uint64_t shifted = 0;
    memcpy(&shifted, raw + 8, 8);
    assert(shifted == 0x0000002000000000ull);
    puts("PASS: coherence des chaines moteur");
}

void TestCloneFieldWidths() {
    // Disposition certifiée sur 1.15.23 (preuves : docs/LAYOUT_DEFINITION.md).
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
    v.flags = 0x4000ull;   // flags vanilla de la source (16384)
    jet::JetCatalogueWriteCloneFields(buf, v);

    auto u32 = [&](size_t off) { uint32_t x; memcpy(&x, buf + off, 4); return x; };
    auto u64 = [&](size_t off) { uint64_t x; memcpy(&x, buf + off, 8); return x; };
    auto ptr = [&](size_t off) { const char* x; memcpy(&x, buf + off, 8); return x; };

    // string_t id / name : pointeur 8 o, len et cap 4 o.
    assert(ptr(L.idPtr) == id);
    assert(u32(L.idLen) == v.idLen && u32(L.idCap) == v.idCap);
    assert(ptr(L.namePtr) == name);
    assert(u32(L.nameLen) == v.nameLen && u32(L.nameCap) == v.nameCap);

    // category : 4 octets ; le champ suivant (0x2ac, non utilisé par le clone)
    // reste intact.
    assert(static_cast<int32_t>(u32(L.category)) == 11);
    for (size_t i = L.category + 4; i < L.value; ++i) assert(buf[i] == 0xA5);

    // value : 4 octets, immédiatement suivi de flags.
    assert(u32(L.value) == 500123u);

    // flags : 8 octets écrits, y compris les 4 octets hauts (0x2bc..0x2bf).
    assert(u64(L.flags) == 0x4000ull);
    for (size_t i = 0; i < 8; ++i)
        assert(buf[L.flags + i] == static_cast<uint8_t>((0x4000ull >> (8 * i)) & 0xFF));
    // Le voisin immédiat (0x2c0) n'est pas touché.
    for (size_t i = L.flags + L.flagsBytes; i < L.flags + L.flagsBytes + 16; ++i)
        assert(buf[i] == 0xA5);

    // RÉGRESSION : un store 32 bits laisserait 0x2bc..0x2bf à leur valeur
    // précédente (ici la sentinelle). C'est exactement ce que ce test interdit.
    uint8_t partial[jet::kEngineDefinitionBytes];
    memset(partial, 0xA5, sizeof(partial));
    const uint32_t low = 0x4000u;
    memcpy(partial + L.flags, &low, 4);
    for (size_t i = 4; i < 8; ++i) assert(partial[L.flags + i] == 0xA5);

    // Une valeur dont les octets hauts sont non nuls est recopiée intégralement.
    v.flags = 0x1122334455667788ull;
    jet::JetCatalogueWriteCloneFields(buf, v);
    assert(u64(L.flags) == 0x1122334455667788ull);
    puts("PASS: largeurs des champs du clone (flags 8 o, value/category 4 o, voisins intacts)");
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
    puts("PASS: encodage du saut absolu");
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
