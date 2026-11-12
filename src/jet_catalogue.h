#pragma once
// Jet Exhaust Unlimited — injection du composant dans le CATALOGUE du jeu, en
// mémoire (aucun fichier écrit hors du dossier du mod).
//
// Ce module est AUTONOME : il ne dépend que du SDK public de StormFabric
// (sw_mod_sdk.h, API native). Toutes les adresses d'exécutable sont résolues par
// le profil de découverte du loader ; aucune RVA n'est codée en dur ici.
//
// Principe :
//   1. hooks de `FindComponentDefinitionByName` et `ComponentRegistry::Clear`
//      posés via `sw_hook_inline` (contrat d'octets validé, fail-closed) ;
//   2. à la demande — au moment EXACT où le jeu demande notre identifiant, ou au
//      premier lookup venu après un Clear — on clone la définition vanilla et on
//      l'insère dans les rings du registre reçu du moteur ;
//   3. avant que le moteur ne détruise les rings, nos entrées sont retirées ;
//      le démontage restaure les prologues à l'identique (sw_hook_restore).
#include "sw_mod_sdk.h"

#include <cstdint>
#include <string>

namespace jet {

bool InitJetCatalogueInjection(const SwModHost* host, const char* xmlPath);
// Injection proactive hors frame de jeu (croissance de ring autorisée).
void JetCatalogueTick();
// Démontage idempotent : retire nos entrées puis restaure les deux hooks.
void ShutdownJetCatalogueInjection();

// ---------------------------------------------------------------------------
// Décisions PURES (testables hors jeu).
//
// Croissance d'un ring : `kAllocate` uniquement quand le ring n'a JAMAIS été
// alloué (cap == 0 : aucun buffer à invalider) ; `kRealloc` est toujours refusé
// (la croissance réelle n'est autorisée que pour cap == 0).
enum class RingGrowth { kNone = 0, kAllocate, kRealloc };
RingGrowth JetCatalogueRingGrowth(uint32_t cap, uint32_t count, bool allowGrow, bool hasGrowFn);

enum class LazyResult {
    kNotOurs = 0,    // le nom demandé n'est pas le nôtre
    kPresent,        // nos entrées sont déjà dans CE registre
    kInject,         // rien dans ce registre : construire + insérer MAINTENANT
    kInjected,       // injection réalisée à la demande
    kSourceMissing,  // source vanilla absente : null (fail-closed) + log
    kRefused,        // registre/insertion refusés : null (fail-closed) + log
};
LazyResult JetCatalogueLazyPlan(bool isOurs, bool publishedInThisRegistry);
LazyResult JetCatalogueLazyOutcome(bool sourceMissing, bool insertOk);
const char* JetCatalogueLazyResultText(LazyResult result);
bool JetCatalogueNeedsInjection(bool registered, bool builtForThisRegistry);

// Géométrie du clone comparée à la taille RÉELLE d'une ComponentDefinition du
// moteur (0x2d08 en 1.15.23, 0x2848 en 1.15.18).
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
// Disposition VÉRIFIÉE des champs du clone sur 1.15.23 (preuves :
// docs/REVERSE_ENGINEERING.md §4.4 et docs/LAYOUT_DEFINITION.md).
//
// Chaque largeur est celle de l'instruction du moteur qui écrit/lit le champ,
// pas un espacement déduit :
//   id       string_t {ptr 8, len 4, cap 4}   @0x288
//   name     string_t {ptr 8, len 4, cap 4}   @0x298
//   category int32                            @0x2a8
//   value    int32                            @0x2b4
//   flags    uint64                           @0x2b8  (`mov [r15],rax`)
//
// `flags` fait 8 octets : une écriture 32 bits n'écrase pas le voisin, mais
// laisse les 4 octets hauts (0x2bc..0x2bf) dans leur état précédent. Le clone
// doit donc recopier la valeur 64 bits de la source, pas seulement son dword
// bas. Les static_assert ci-dessous figent cette disposition.
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
static_assert(kDefinitionLayout.idPtr + 8 == kDefinitionLayout.idLen, "id: ptr 8 o puis len");
static_assert(kDefinitionLayout.idLen + 4 == kDefinitionLayout.idCap, "id: len puis cap");
static_assert(kDefinitionLayout.idCap + 4 == kDefinitionLayout.namePtr, "id puis name contigus");
static_assert(kDefinitionLayout.namePtr + 8 == kDefinitionLayout.nameLen, "name: ptr 8 o puis len");
static_assert(kDefinitionLayout.nameLen + 4 == kDefinitionLayout.nameCap, "name: len puis cap");
static_assert(kDefinitionLayout.nameCap + 4 == kDefinitionLayout.category, "name puis category contigus");
static_assert(kDefinitionLayout.value + 4 == kDefinitionLayout.flags, "value 4 o juste avant flags");
static_assert(kDefinitionLayout.flagsBytes == 8, "flags fait 8 octets (mov [r15],rax)");
static_assert(kDefinitionLayout.flags + kDefinitionLayout.flagsBytes == 0x2c0,
              "flags occupe 0x2b8..0x2c0");
static_assert(kDefinitionLayout.flags + kDefinitionLayout.flagsBytes <= kEngineDefinitionBytes,
              "flags tient dans une definition");

// Valeurs écrites dans le clone. Les largeurs sont portées par les types :
// `flags` est un uint64_t, jamais un uint32_t.
struct CloneFieldValues {
    const char* id;
    uint32_t idLen, idCap;
    const char* name;
    uint32_t nameLen, nameCap;
    int32_t category;
    uint32_t value;
    uint64_t flags;
};
// Écrit les champs du clone à leurs offsets/largeurs certifiés. Fonction PURE
// (aucun accès moteur) : les tests hors jeu vérifient qu'aucun octet voisin
// n'est touché et que `flags` est bien écrit sur 8 octets.
void JetCatalogueWriteCloneFields(uint8_t* dst, const CloneFieldValues& values);

// Taille d'une définition et décalage du dernier champ détruit, par build
// (1 ou 2, identité fournie par le profil du loader). Les deux builds n'ont PAS
// la même disposition : coder une seule valeur en dur produit un crash.
uint64_t JetCatalogueDefinitionBytes(int build);
uint64_t JetCatalogueDestroyedTail(int build);

// Une chaîne moteur {ptr,len,cap} est-elle destructible sans faute ?
bool JetCatalogueStringCoherent(uint64_t ptr, uint32_t len, uint32_t cap);
// Encode `jmp [rip+0]` (FF 25 00000000 <imm64>) + NOP jusqu'à patchSize.
size_t JetCatalogueBuildJump(uint8_t* out, size_t patchSize, void* target);
}  // namespace jet
