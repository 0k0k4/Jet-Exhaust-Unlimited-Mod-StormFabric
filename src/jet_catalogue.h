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
