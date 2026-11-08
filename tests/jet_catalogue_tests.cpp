// Tests hors jeu du module d'injection catalogue : validation des signatures
// d'octets sur les deux exécutables reviewés et encodage du détour.
#include "../src/jet_catalogue.h"
#include "../src/log.h"
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace swml {
void LogError(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); puts(""); va_end(a); }
void LogWarn(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); puts(""); va_end(a); }
void LogInfo(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); puts(""); va_end(a); }
void Log(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); puts(""); va_end(a); }
void LogDebug(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); puts(""); va_end(a); }
void LogLevelMsg(LogLevel, const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); puts(""); va_end(a); }
void LogDiscoveryProgress(const char*, size_t, size_t, const char*, bool) {}
// Stubs des helpers de chaîne du moteur (non utilisés par ces tests).
}

namespace sw {
void StrMake(const Api&, EStr& s, const char*) { s = {}; }
void StrRelease(const Api&, EStr& s) { s = {}; }
}

namespace {

std::vector<uint8_t> MapImage(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (raw.size() < sizeof(IMAGE_DOS_HEADER)) return {};
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return {};
    std::vector<uint8_t> image(nt->OptionalHeader.SizeOfImage, 0);
    memcpy(image.data(), raw.data(), nt->OptionalHeader.SizeOfHeaders);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if ((size_t)s[i].PointerToRawData + s[i].SizeOfRawData > raw.size()) continue;
        if ((size_t)s[i].VirtualAddress + s[i].SizeOfRawData > image.size()) continue;
        memcpy(image.data() + s[i].VirtualAddress, raw.data() + s[i].PointerToRawData, s[i].SizeOfRawData);
    }
    return image;
}

void TestJumpEncoding() {
    uint8_t out[24];
    // 16 octets (FindComponentDefinitionByName)
    assert(swml::JetCatalogueBuildJump(out, 16, reinterpret_cast<void*>(0x1122334455667788ull)) == 16);
    assert(out[0] == 0xFF && out[1] == 0x25);
    int32_t disp = -1; memcpy(&disp, out + 2, 4);
    assert(disp == 0);   // REGRESSION : disp32 non nul => saut vers une adresse arbitraire
    uint64_t tgt = 0; memcpy(&tgt, out + 6, 8);
    assert(tgt == 0x1122334455667788ull);
    for (int i = 14; i < 16; ++i) assert(out[i] == 0x90);
    // 18 octets (ComponentRegistry::Clear)
    assert(swml::JetCatalogueBuildJump(out, 18, reinterpret_cast<void*>(0xAABBCCDD11223344ull)) == 18);
    memcpy(&disp, out + 2, 4); assert(disp == 0);
    memcpy(&tgt, out + 6, 8); assert(tgt == 0xAABBCCDD11223344ull);
    for (int i = 14; i < 18; ++i) assert(out[i] == 0x90);
    // refus
    assert(swml::JetCatalogueBuildJump(out, 13, reinterpret_cast<void*>(1)) == 0);
    assert(swml::JetCatalogueBuildJump(out, 25, reinterpret_cast<void*>(1)) == 0);
    assert(swml::JetCatalogueBuildJump(nullptr, 16, reinterpret_cast<void*>(1)) == 0);
    puts("PASS: encodage du saut absolu (disp32 nul, imm64, NOP, refus)");
}

// Régression 2026-11-08 : un ring JAMAIS ALLOUÉ (cap=0) doit être alloué même quand
// la croissance est interdite dans la frame courante. Confondre ce cas avec un
// ring « plein » faisait échouer toute l'injection pendant le chargement de monde
// (« ring des descripteurs plein » alors que count=cap=0).
void TestRingGrowth() {
    using swml::JetCatalogueRingGrowth;
    using swml::RingGrowth;
    assert(JetCatalogueRingGrowth(8192, 6072, false, true) == RingGrowth::kNone);
    // VIDE / jamais alloué : allocation autorisée depuis une frame du jeu.
    assert(JetCatalogueRingGrowth(0, 0, false, true) == RingGrowth::kAllocate);
    assert(JetCatalogueRingGrowth(0, 0, true, true) == RingGrowth::kAllocate);
    // Réellement plein (cap>0) : realloc TOUJOURS refusé, même hors frame.
    // La seule croissance autorisée est l'allocation d'un ring jamais alloué (cap==0).
    assert(JetCatalogueRingGrowth(16, 16, false, true) == RingGrowth::kNone);
    assert(JetCatalogueRingGrowth(16, 16, true, true) == RingGrowth::kNone);
    // Aucune fonction de croissance : rien à faire dans tous les cas.
    assert(JetCatalogueRingGrowth(0, 0, true, false) == RingGrowth::kNone);
    assert(JetCatalogueRingGrowth(16, 16, true, false) == RingGrowth::kNone);
    puts("PASS: croissance de ring (allocation d'un ring vide seule autorisee, realloc toujours interdite)");
}

// Régression du CRASH AU SHUTDOWN (2026-11-08).
// Le moteur détruit une définition en libérant des champs jusqu'à +0x2D00, et le
// destructeur du ring des variantes libère CHAQUE variante avec sa taille réelle
// (operator delete(ptr, 0x2d08)). Un pas de 0x2848 faisait donc empiéter chaque
// variante sur la suivante : détruire la variante k libérait un champ de la
// variante k+1 (double free) et pouvait relire une paire (len,cap) comme un
// pointeur -> _aligned_free(0x0000002000000000) -> AV lecture de
// 0x0000001FFFFFFFF8 (crash.dmp : stormworks64.exe+0x9B2D5D).
void TestCloneGeometry() {
    using swml::JetCatalogueCloneGeometry;
    using swml::JetCatalogueDefinitionBytes;
    using swml::JetCatalogueDestroyedTail;
    // Build détecté -> taille de l'objet et dernier champ détruit.
    assert(JetCatalogueDefinitionBytes(2) == swml::kEngineDefinitionBytes);   // 1.15.23
    assert(JetCatalogueDefinitionBytes(1) == swml::kEngineDefinitionBytesOld);// 1.15.18
    assert(JetCatalogueDestroyedTail(2) == swml::kEngineDestroyedTail);
    assert(JetCatalogueDestroyedTail(1) == swml::kEngineDestroyedTailOld);

    // 1.15.23 : une variante par 0x2D08 -> le dernier champ détruit (+0x2D00)
    // tient dans la variante, aucun chevauchement, destructible par le moteur.
    auto good = JetCatalogueCloneGeometry(JetCatalogueDefinitionBytes(2), 8, JetCatalogueDestroyedTail(2));
    assert(good.variantBytes == 0x2d08);
    assert(good.variants == 8);
    assert(good.blockBytes == 8ull * 0x2d08);
    assert(!good.overlapping);

    // RÉGRESSION DU CRASH : le pas 0x2848 appliqué à un objet 1.15.23 (0x2D08)
    // déborde sur la variante suivante -> la destruction moteur libère un champ
    // de la voisine, et relit une paire (len,cap) comme un pointeur.
    auto legacy = JetCatalogueCloneGeometry(0x2848, 8, JetCatalogueDestroyedTail(2));
    assert(legacy.overlapping);
    assert(legacy.blockBytes == 8ull * 0x2848);
    assert(!(0x2d00 + 8 <= legacy.variantBytes));

    // Sur 1.15.18 en revanche, 0x2848 est la bonne taille : cohérent.
    auto old2024 = JetCatalogueCloneGeometry(JetCatalogueDefinitionBytes(1), 8, JetCatalogueDestroyedTail(1));
    assert(!old2024.overlapping);
    puts("PASS: geometrie du clone (0x2d08 par variante en 1.15.23, chevauchement 0x2848 detecte)");
}

// Les chaînes du clone sont construites par l'allocateur du JEU : len/cap doivent
// rester cohérents, sinon leur destruction au shutdown lit un pointeur bidon.
void TestEngineStringCoherence() {
    using swml::JetCatalogueStringCoherent;
    // Chaîne du moteur valide : 39 octets, cap 64.
    assert(JetCatalogueStringCoherent(0x000001A7E853E000ull, 39, 64));
    // Chaîne vide mais réservée (cas réel du moteur) : ptr nul, len 0, cap 32.
    assert(JetCatalogueStringCoherent(0, 0, 32));
    // Incohérences refusées.
    assert(!JetCatalogueStringCoherent(0x1000, 64, 39));    // len > cap
    assert(!JetCatalogueStringCoherent(0, 39, 64));         // ptr nul avec len > 0
    assert(!JetCatalogueStringCoherent(0x1000, 0, 31));     // cap < 32 (minimum moteur)
    assert(!JetCatalogueStringCoherent(0x1000, 0xFFFFFFFFu, 64)); // len aberrant
    // Valeur fautive du dump : 0x0000002000000000 = la paire (len=0, cap=0x20)
    // RELUE COMME UN POINTEUR quand la structure est lue 8 octets trop loin.
    // C'est ce que produisait le chevauchement des variantes ; l'arithmétique est
    // figée ici pour que la régression reste lisible, la géométrie (test
    // précédent) étant ce qui interdit désormais ce décalage.
    uint8_t raw[24] = {};
    const uint32_t len = 0, cap = 0x20;
    memcpy(raw + 8, &len, 4);
    memcpy(raw + 12, &cap, 4);
    uint64_t shifted = 0;
    memcpy(&shifted, raw + 8, 8);
    assert(shifted == 0x0000002000000000ull);
    puts("PASS: coherence des chaines moteur (len/cap, pointeur, capacite min, valeur fautive du dump)");
}

// ---------------------------------------------------------------------------
// RÉGRESSION « le composant disparaît de mon build à chaque retour à l'atelier ».
//
// Preuve du log de référence (StormworksModLoader.log) :
//   [14:39:00.829] entrees retirees avant ComponentRegistry::Clear (11 entree(s))
//   [14:39:30.138] definition source 'jet_engine_combustion_chamber' absente du
//                  registre (essai 200)
//   [14:39:37.550] source=... -> clone -> injecte
// => pendant ~37 s après Clear, le lookup de 'jet_engine_combustion_chamber_unlimited'
//    (désérialisation du véhicule) ne trouvait RIEN : le jeu retirait le composant.
// Le remède est que le lookup lui-même serve la demande, sans attendre un sondage.
void TestLazyInjectionDecision() {
    using swml::LazyResult;
    // Nom étranger : le lookup du jeu décide seul, aucune écriture de notre part.
    assert(swml::JetCatalogueLazyPlan(false, false) == LazyResult::kNotOurs);
    assert(swml::JetCatalogueLazyPlan(false, true) == LazyResult::kNotOurs);

    // Notre identifiant, absent du registre reçu en RCX : injecter MAINTENANT.
    assert(swml::JetCatalogueLazyPlan(true, false) == LazyResult::kInject);
    // Anti-doublon / idempotence : déjà publié dans CE registre => renvoyer l'existant
    // (aucun re-clonage de 8 x 0x2d08, aucune deuxième entrée dans les rings).
    assert(swml::JetCatalogueLazyPlan(true, true) == LazyResult::kPresent);

    // Deux demandes successives : la 1re injecte, la 2e voit l'entrée et ne réinjecte pas.
    bool published = false;
    const LazyResult first = swml::JetCatalogueLazyPlan(true, published);
    assert(first == LazyResult::kInject);
    published = true;   // ce que l'insertion vient de rendre vrai
    const LazyResult second = swml::JetCatalogueLazyPlan(true, published);
    assert(second == LazyResult::kPresent && second != LazyResult::kInject);

    // Après ComponentRegistry::Clear (rings vidés) le PREMIER lookup doit réinjecter :
    // c'est exactement la fenêtre où le jeu désérialisait le véhicule sans le composant.
    published = false;
    assert(swml::JetCatalogueLazyPlan(true, published) == LazyResult::kInject);
    puts("PASS: injection a la demande (plan) - idempotente, anti-doublon, reinjection apres Clear");
}

// RÉ-INJECTION « AU PREMIER LOOKUP » : la définition d'un véhicule est résolue par le jeu
// SANS passer par FindComponentDefinitionByName avec notre id (mesuré en jeu : le véhicule a
// perdu exactement notre composant alors que le hook n'a jamais vu ce nom). L'injection doit
// donc repartir au premier lookup venu, dès que l'entrée n'est pas publiée DANS CE registre.
void TestNeedsInjectionDecision() {
    // Déjà publiée dans CE registre : ne rien refaire (idempotent, aucun re-clonage).
    assert(!swml::JetCatalogueNeedsInjection(true, true));
    // Après ComponentRegistry::Clear (registered retombe à false) : réinjecter.
    assert(swml::JetCatalogueNeedsInjection(false, false));
    // Registre DIFFÉRENT (monde vs atelier) : l'entrée existe ailleurs, il faut la publier ici.
    assert(swml::JetCatalogueNeedsInjection(true, false));
    assert(swml::JetCatalogueNeedsInjection(false, true));
    // Séquence vécue : chargement -> injection -> Clear -> désérialisation -> réinjection.
    bool registered = false, builtForThis = false;
    assert(swml::JetCatalogueNeedsInjection(registered, builtForThis));   // 1er lookup
    registered = true; builtForThis = true;                               // injecté
    assert(!swml::JetCatalogueNeedsInjection(registered, builtForThis));  // lookups suivants
    registered = false; builtForThis = false;                             // Clear
    assert(swml::JetCatalogueNeedsInjection(registered, builtForThis));   // réinjection
    puts("PASS: reinjection au premier lookup (apres Clear et sur un second registre)");
}

// La source absente ne doit JAMAIS passer pour un succès : null + log (fail-closed).
void TestLazyInjectionOutcome() {
    using swml::LazyResult;
    assert(swml::JetCatalogueLazyOutcome(true, false) == LazyResult::kSourceMissing);
    assert(swml::JetCatalogueLazyOutcome(true, true) == LazyResult::kSourceMissing);
    assert(swml::JetCatalogueLazyOutcome(false, false) == LazyResult::kRefused);
    assert(swml::JetCatalogueLazyOutcome(false, true) == LazyResult::kInjected);

    // Les textes journalisés portent la cause exacte (preuve lisible dans le log).
    assert(swml::JetCatalogueLazyResultText(LazyResult::kInjected) != nullptr);
    assert(strstr(swml::JetCatalogueLazyResultText(LazyResult::kInjected), "a la demande") != nullptr);
    assert(strstr(swml::JetCatalogueLazyResultText(LazyResult::kPresent), "deja present") != nullptr);
    assert(strstr(swml::JetCatalogueLazyResultText(LazyResult::kSourceMissing), "source absente") != nullptr);
    for (int i = 0; i <= static_cast<int>(LazyResult::kRefused); ++i) {
        const char* text = swml::JetCatalogueLazyResultText(static_cast<LazyResult>(i));
        assert(text != nullptr && text[0] != '\0');
    }
    puts("PASS: injection a la demande (issue) - source absente => fail-closed + cause journalisee");
}

void TestSignaturesOnRealBinaries(int argc, char** argv) {
    if (argc < 2) {
        puts("SKIP: executables non fournis (passer 1.15.23 puis 1.15.18)");
        return;
    }
    auto newer = MapImage(argv[1]);
    assert(!newer.empty());
    int build = swml::JetCatalogueBuild(newer.data(), newer.size());
    printf("[Jet] %s -> build %d\n", argv[1], build);
    assert(build == 2);

    if (argc >= 3) {
        auto older = MapImage(argv[2]);
        assert(!older.empty());
        int b2 = swml::JetCatalogueBuild(older.data(), older.size());
        printf("[Jet] %s -> build %d\n", argv[2], b2);
        assert(b2 == 1);
    }

    // Un octet modifié dans la signature => refus (fail-closed).
    auto broken = newer;
    broken[0x46e900 + 33] ^= 0xFF;
    assert(swml::JetCatalogueBuild(broken.data(), broken.size()) == 0);
    if (argc >= 3) {
        auto brokenOld = MapImage(argv[2]);
        brokenOld[0x46f380 + 26] ^= 0xFF;
        assert(swml::JetCatalogueBuild(brokenOld.data(), brokenOld.size()) == 0);
    }
    puts("PASS: signature catalogue validee sur binaire reel + refus si octet altere");
}

}  // namespace

int main(int argc, char** argv) {
    TestJumpEncoding();
    TestRingGrowth();
    TestCloneGeometry();
    TestEngineStringCoherence();
    TestLazyInjectionDecision();
    TestNeedsInjectionDecision();
    TestLazyInjectionOutcome();
    TestSignaturesOnRealBinaries(argc, argv);
    puts("PASS: jet_catalogue (signature FindComponentDefinitionByName, 2 builds, fail-closed)");
    return 0;
}
