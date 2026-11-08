#pragma once
#include "game.h"
#include <cstdint>
#include <string>

namespace swml {

// Injection du composant dans le CATALOGUE du jeu, en mémoire (aucun fichier écrit
// hors de mods\). Voir docs/jet_exhaust_unlimited.md.
//
// Principe :
//   1. On pose un hook d'entrée sur FUN_14046c180 (chargeur de définitions/composants
//      appelé une fois à l'init). Le hook ne fait que SAUVEGARDER ses arguments
//      (manager, contexte fichiers, 3 paramètres) puis reprend l'exécution normale.
//   2. Sur la première pulsation UI (jeu initialisé), on enregistre notre composant
//      compilé (.bin) dans le catalogue via les fonctions natives du jeu :
//      fabrique FUN_140314ce0 -> anti-doublon FUN_14046e870 -> enregistrement FUN_14046ec30.
//   Le composant apparaît alors comme un vrai composant, sans mod officiel.
//
// Fail-closed : les 4 fonctions sont validées par signature d'octets sur les deux
// builds reviewés ; toute divergence => aucun hook, aucun enregistrement.
//
// OPT-IN DEPUIS LA CHASSE AU BLOCAGE DE CHARGEMENT : sans la variable d'environnement
// `SW_JET_CATALOGUE=1`, cette fonction ne pose AUCUN hook et n'injecte rien (elle journalise
// « Jet: injection catalogue desactivee par defaut »). Ses deux hooks s'exécutent pendant le
// chargement d'un monde ; ils sont donc écartés par défaut pour que le jeu charge dans tous
// les cas. Le composant du mod n'apparaît alors pas dans l'atelier.
bool InitJetCatalogueInjection(const sw::Api& api, const std::string& relativeXmlPath);
// Conservé pour compatibilité : l'injection est désormais paresseuse (au premier
// lookup de notre identifiant par le jeu).
void JetCatalogueTick();
void ShutdownJetCatalogueInjection();

// Validation pure (testable hors jeu) : renvoie 2 (build 1.15.23), 1 (1.15.18) ou 0.
int JetCatalogueBuild(const uint8_t* base, size_t size);

// Décision d'insertion dans un ring du registre, pure et testable hors jeu.
//   kNone      : il reste de la place (rien à faire) ou aucune croissance possible ;
//   kAllocate  : ring JAMAIS alloué (cap == 0) -> allouer n'est pas un realloc :
//                il n'existe aucun buffer que le moteur parcourt, donc c'est sûr
//                même depuis une frame du jeu ;
//   kRealloc   : ring réellement plein -> realloc TOUJOURS refusé (protection
//                conservée) : la croissance n'est autorisée que pour cap==0.
// Confondre kAllocate et kRealloc est ce qui a bloqué l'injection pendant le
// chargement de monde du 2026-11-08 (log : « ring des descripteurs plein » avec
// un ring à cap=0 count=0, c'est-à-dire vide).
enum class RingGrowth { kNone = 0, kAllocate, kRealloc };
RingGrowth JetCatalogueRingGrowth(uint32_t cap, uint32_t count, bool allowGrow, bool hasGrowFn);

// ---------------------------------------------------------------------------
// INJECTION À LA DEMANDE (correctif « le composant disparaît de mon build »).
//
// Le jeu résout les identifiants de composants d'un véhicule PENDANT la désérialisation
// (retour à l'atelier, chargement d'une sauvegarde), donc AVANT toute pulsation UI : une
// injection pilotée par sondage arrivait trop tard, le lookup de
// `jet_engine_combustion_chamber_unlimited` renvoyait null et le jeu RETIRAIT le composant
// du build. L'injection est désormais déclenchée par le hook de lookup, à l'instant EXACT
// où le jeu demande notre identifiant, dans le registre qu'il a passé en RCX.
//
// Étape 1 — plan (pure, testable) : que faire AVANT toute écriture ?
enum class LazyResult {
    kNotOurs = 0,    // le nom demandé n'est pas le nôtre : le lookup du jeu décide seul
    kPresent,        // nos entrées sont déjà dans CE registre : renvoyer l'existante
    kInject,         // rien dans ce registre : construire + insérer MAINTENANT
    kInjected,       // injection réalisée à la demande
    kSourceMissing,  // source vanilla absente à cet instant : null (fail-closed) + log
    kRefused,        // registre invalide / XML / insertion refusée : null (fail-closed) + log
};
// Anti-doublon : (isOurs && publishedInThisRegistry) => kPresent, jamais un re-clonage.
LazyResult JetCatalogueLazyPlan(bool isOurs, bool publishedInThisRegistry);
// Étape 2 — issue de la tentative (pure, testable) : source absente => kSourceMissing,
// insertion refusée => kRefused, insertion réussie => kInjected.
LazyResult JetCatalogueLazyOutcome(bool sourceMissing, bool insertOk);
// Texte journalisé pour une issue (jamais nullptr) : c'est la preuve lisible dans le log.
const char* JetCatalogueLazyResultText(LazyResult result);
// Étape 0 (pure) : faut-il (ré)injecter dans le registre reçu par le hook de lookup ?
//   - entrée déjà publiée DANS CE registre (registered && builtForThisRegistry) -> non ;
//   - après ComponentRegistry::Clear (registered == false)                     -> oui ;
//   - registre DIFFÉRENT de celui déjà servi (monde vs atelier)                -> oui.
// C'est cette décision qui réinjecte « au premier lookup », sans aucun sondage temporisé :
// mesuré en jeu, l'atelier désérialise un véhicule sans jamais demander notre identifiant.
bool JetCatalogueNeedsInjection(bool registered, bool builtForThisRegistry);

// ---------------------------------------------------------------------------
// Géométrie du clone comparée à la taille RÉELLE d'une ComponentDefinition du
// moteur (0x2d08, certifiée par operator new / operator delete(ptr,size) et par
// le destructeur 0x87FDB0 — voir jet_catalogue.cpp).
//   blockBytes  : taille totale des variantes ;
//   overlapping : une variante empiète-t-elle sur la suivante ? (fatal : le
//                 destructeur moteur libère des champs jusqu'à +0x2D00)
struct CloneGeometry {
    uint64_t variantBytes;
    uint32_t variants;
    uint64_t blockBytes;
    bool overlapping;
};
CloneGeometry JetCatalogueCloneGeometry(uint64_t variantBytes, uint32_t variants,
                                       uint64_t destroyedTail);
constexpr uint64_t kEngineDefinitionBytes = 0x2d08;      // 1.15.23 : sizeof(ComponentDefinition)
constexpr uint64_t kEngineDefinitionBytesOld = 0x2848;   // 1.15.18 (reference historique)
constexpr uint64_t kEngineDestroyedTail = 0x2d00;        // 1.15.23 : dernier champ detruit
constexpr uint64_t kEngineDestroyedTailOld = 0x2840;     // 1.15.18

// Taille d'une definition et decalage du dernier champ detruit, par build
// (1 = 1.15.18, 2 = 1.15.23). Les deux builds n'ont PAS la meme disposition :
// coder une seule valeur en dur faisait empieter nos variantes (crash au shutdown).
uint64_t JetCatalogueDefinitionBytes(int build);
uint64_t JetCatalogueDestroyedTail(int build);

// Une chaîne moteur {ptr,len,cap} est-elle destructible sans faute ?
// (cap >= 32, len <= cap, ptr non nul si len > 0). La paire (len,cap) lue comme
// un pointeur — 0x0000002000000000 pour len=0 / cap=0x20 — est refusée.
bool JetCatalogueStringCoherent(uint64_t ptr, uint32_t len, uint32_t cap);
// Encode `jmp [rip+0]` (FF 25 00000000 <imm64>) + NOP jusqu'à patchSize.
size_t JetCatalogueBuildJump(uint8_t* out, size_t patchSize, void* target);
}
