#include "jet_catalogue.h"
#include "jet_sdk.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

namespace jet {
namespace {

// Les deux hooks du module sont des SERVICES MOTEUR GÉNÉRIQUES : le loader
// StormFabric les résout depuis son profil de découverte (contrat d'octets
// retenu, validé avant tout écriture) et les expose via l'API native. Le mod ne
// contient donc AUCUNE adresse d'exécutable.
//
// Tailles de prologue : dérivées des longueurs d'instruction certifiées, ce qui
// garantit qu'on ne coupe jamais une instruction en deux (MOV RBX,RCX = 48 8B D9).
constexpr size_t kFindLens[] = {5, 5, 1, 1, 1, 1, 2};          // mov, mov, push x4, push r12
constexpr size_t kClearLens[] = {5, 5, 1, 4, 3};                // mov, mov, push, sub, mov
constexpr size_t Sum(const size_t* v, size_t n) {
    size_t t = 0; for (size_t i = 0; i < n; ++i) t += v[i]; return t;
}
constexpr size_t kFindPatch = Sum(kFindLens, sizeof(kFindLens) / sizeof(kFindLens[0]));
constexpr size_t kClearPatch = Sum(kClearLens, sizeof(kClearLens) / sizeof(kClearLens[0]));
static_assert(kFindPatch == 16, "prologue Find: frontiere d'instruction changee");
static_assert(kClearPatch == 18, "prologue Clear: frontiere d'instruction changee");

// Disposition d'une définition (0x2d08 octets, 8 variantes chaînées).
//
// CRASH AU SHUTDOWN (crash.dmp du 2026-11-08, AV 0xC0000005 lecture de
// 0x1FFFFFFFF8 a stormworks64.exe+0x9B2D5D, c.-a-d. DANS la liberation de chaine
// du moteur) : une seule valeur 0x2848 était codée en dur, alors que la taille de
// l'objet a CHANGÉ entre les deux builds reviewés. Sur 1.15.23 la taille réelle
// d'une ComponentDefinition est 0x2d08, certifiée par TROIS sites indépendants :
//   * 0x46EEC0 `mov ecx,0x2d08 ; call 0x958C28` -> operator new d'une définition
//     par le chargeur de composants ;
//   * 0x8B9FEE `mov edx,0x2d08 ; call 0x95881C` -> operator delete(ptr,size) de
//     CHAQUE variante par le destructeur du ring des variantes ;
//   * 0x87FDB0 (destructeur de définition) libère des champs jusqu'à +0x2D00,
//     donc au-delà de 0x2848.
// Avec un pas de 0x2848 sur 1.15.23 nos 8 variantes SE CHEVAUCHAIENT de 0x4C0
// octets : détruire la variante k détruisait/libérait des champs appartenant à la
// variante k+1 (double free, puis relecture d'une paire (len,cap) de chaîne comme
// s'il s'agissait d'un pointeur -> str_free(0x0000002000000000) -> lecture de
// [rcx-8] = 0x0000001FFFFFFFF8 -> AV).
// Voir build/tests/crash_dump.py (pile reconstituée) et docs/ENGINE_FUNCTION_MAPPING.md.
constexpr size_t kDefinitionBytesOld = 0x2848;   // 1.15.18 (référence historique)
constexpr size_t kDefinitionBytesNew = 0x2d08;   // 1.15.23 (certifié, voir ci-dessus)
constexpr size_t kVariants = 8;
// Taille EFFECTIVE, fixée à l'initialisation selon le build détecté.
size_t g_definitionBytes = kDefinitionBytesNew;  // NOLINT : état de module, comme g_cat
static_assert(kDefinitionBytesNew >= 0x2d00 + 8, "une definition doit couvrir ses champs detruits (+0x2D00)");
constexpr size_t kOffVariantIndex = 0x40;
constexpr size_t kOffId = 0x288, kOffIdLen = 0x290, kOffIdCap = 0x294;
constexpr size_t kOffName = 0x298, kOffNameLen = 0x2a0, kOffNameCap = 0x2a4;
constexpr size_t kOffCategory = 0x2a8;
constexpr size_t kOffValue = 0x2b4;
constexpr size_t kOffFlags = 0x2b8;
// Dernier champ touché par le destructeur de définition (0x87FDB0 libère
// [def+0x2CE8] et [def+0x2CF8], puis écrit [def+0x2CF0] / [def+0x2D00]) : ce
// champ DOIT tenir dans une variante, sinon la destruction déborde sur la
// variante voisine (cf. JetCatalogueCloneGeometry).
constexpr size_t kOffDestroyedTailNew = 0x2d00;   // 1.15.23
constexpr size_t kOffDestroyedTailOld = 0x2840;   // 1.15.18 (taille - 8)
// Rings du registre.
constexpr size_t kOffVariants = 0xc8;
constexpr size_t kOffDescriptors = 0xe0;
constexpr size_t kOffRing = 0xf8;
constexpr size_t kOffBuckets = 0x110;
constexpr size_t kBucketStride = 24;

// Capacité demandée quand on doit ALLOUER un ring jamais alloué (cap == 0).
constexpr uint32_t kRingInitialCapacity = 1024;

constexpr char kComponentId[] = "jet_engine_combustion_chamber_unlimited";
constexpr char kComponentName[] = "Jet Combustion Chamber Unlimited";
constexpr char kSourceId[] = "jet_engine_combustion_chamber";
constexpr uint32_t kSignatureValue = 500123;
constexpr int32_t kCategory = 11;   // « JET ENGINES » (valeur historique, non utilisée : on lit la catégorie réelle de la source)

using FindDefFn = void* (*)(void*, const void*);
using LocRegisterFn = void (*)(const SwEngineString*, const SwEngineString*);
using GrowFn = void (*)(void*, uint32_t);
using ClearFn = void (*)(void*);
void RegisterAllLocalization(LocRegisterFn fn);

// Géométrie du clone : PURE et testable hors jeu (voir JetCatalogueCloneGeometry).
// Un pas plus petit que la taille d'une définition fait empiéter chaque variante
// sur la suivante : toute destruction par le moteur devient alors fatale.
bool FieldInsideVariant(uint64_t stride, uint64_t offset, uint64_t size) {
    return stride != 0 && offset + size <= stride;
}

struct State {
    // hook FindComponentDefinitionByName
    uint8_t* findTarget = nullptr;
    uint8_t* findTramp = nullptr;
    FindDefFn originalFind = nullptr;
    SwHandle findHandle = 0;      // poignée du loader (sw_hook_restore)
    // hook ComponentRegistry::Clear
    uint8_t* clearTarget = nullptr;
    uint8_t* clearTramp = nullptr;
    ClearFn originalClear = nullptr;
    SwHandle clearHandle = 0;
    LocRegisterFn locRegister = nullptr;
    GrowFn grow = nullptr;
    std::string xmlPath;
    void* registry = nullptr;     // registre dans lequel on a injecté
    void* definition = nullptr;   // variante 0 (entrée des rings global/catégorie)
    // Chaque variante est une allocation PROPRE de la taille réelle de l'objet
    // moteur : le destructeur du ring des variantes libère chaque entrée par
    // operator delete(ptr, 0x2d08). Un bloc unique découpé en 8 (ancien schéma)
    // rendait toute destruction par le moteur fatale (8 delete sur un bloc).
    void* variants[kVariants] = {};
    void* builtFor = nullptr;     // registre pour lequel `definition` a été construite
    int32_t builtCategory = -1;   // catégorie (bucket) lue sur la définition source
    void* descriptor = nullptr;   // descripteur XML 32 o (construit UNE fois)
    SwEngineString descriptorId = {}, descriptorXml = {};
    bool registered = false;      // insérée dans les rings (ne jamais re-construire)
    bool failed = false;
    bool clearing = false;        // B1/M6 : Clear en cours -> ne pas injecter
    bool shuttingDown = false;    // DllMain(PROCESS_DETACH) : plus aucune écriture
    int attempts = 0;             // M3 : échecs transitoires bornés (pas de "failed" définitif)
    bool inInject = false;        // réentrance : injection lancée depuis le hook de lookup
    bool injectDisabled = false;  // bissection : marqueur <mod>/.no_catalogue_injection
    bool shapesLogged = false;    // diagnostic des rings déjà journalisé
    int validateFails = 0;        // validations consécutives refusées (diagnostic)
    int pushFails = 0;            // insertions de ring refusées (diagnostic, voir RingPushLogged)
    uint64_t lastAttemptMs = 0;   // cadencement des tentatives faites depuis le lookup
    // Injection à la demande : provenance du clone et âge de celui-ci.
    void* sourceUsed = nullptr;   // définition source COPIÉE dans le clone courant
    uint32_t epoch = 0;           // incrémenté à chaque ComponentRegistry::Clear
    uint32_t cloneEpoch = 0;      // valeur de `epoch` au moment de la construction
    uint32_t lazyHits = 0;        // lookups de notre id déjà servis par l'entrée existante
    uint32_t lazyMisses = 0;      // lookups de notre id non servis (fail-closed)
    uint32_t nativeResolved = 0;  // lookups où le MOTEUR a retrouvé notre entrée
};
State g_cat;
// B1 : sérialise injection / Clear / écriture des rings. CRITICAL_SECTION est
// réentrant (SRWLOCK ne l'est pas) : le hook peut être rappelé par le trampoline.
CRITICAL_SECTION g_lock;
bool g_lockInit = false;
inline void LockAcquire() { if (g_lockInit) EnterCriticalSection(&g_lock); }
inline void LockRelease() { if (g_lockInit) LeaveCriticalSection(&g_lock); }

bool IsComponentId(const uint8_t* p, uint32_t len) {
    return p && len == sizeof(kComponentId) - 1 && _strnicmp(reinterpret_cast<const char*>(p), kComponentId, len) == 0;
}

// Lit l'identifiant d'une entrée selon le type de ring.
//   définitions (variantes / global / bucket) : id à +0x288 (ptr) / +0x290 (len)
//   descripteurs (+0xe0)                     : id à +0x00 (ptr) / +0x08 (len)
bool EntryIsOurs(void* entry, bool descriptor) {
    if (!entry) return false;
    __try {
        auto* e = static_cast<uint8_t*>(entry);
        const uint8_t* p = descriptor ? *reinterpret_cast<const uint8_t**>(e + 0x00)
                                      : *reinterpret_cast<const uint8_t**>(e + kOffId);
        uint32_t len = descriptor ? *reinterpret_cast<uint32_t*>(e + 0x08)
                                  : *reinterpret_cast<uint32_t*>(e + kOffIdLen);
        return IsComponentId(p, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Identité des blocs que NOUS avons publiés (clone + descripteur). C'est le
// critère de retrait le plus sûr : il ne dépend d'aucun champ de l'objet, donc
// il fonctionne même si le moteur (ou une écriture concurrente) a corrompu l'id.
// Le crash du shutdown venait précisément d'une entrée laissée dans les rings :
// le moteur la détruisait avec SES allocateurs et sa taille d'objet.
bool IsPublishedEntry(const void* entry) {
    if (!entry) return false;
    if (entry == g_cat.descriptor) return true;
    for (size_t k = 0; k < kVariants; ++k)
        if (entry == g_cat.variants[k]) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Vue d'un ring du registre : {buf +0, cap +8, head +12, count +16} (stride 24).
// Toutes les gardes sur les rings sont concentrées ici : aucune autre fonction
// ne lit/écrit un ring sans passer par ces helpers.
struct Ring {
    void** buf = nullptr;
    uint32_t cap = 0;
    int32_t head = 0;
    uint32_t count = 0;
};

bool ReadRing(void* reg, size_t offset, Ring* out) {
    if (!reg || !out) return false;
    __try {
        auto* b = static_cast<uint8_t*>(reg) + offset;
        out->buf = *reinterpret_cast<void***>(b + 0);
        out->cap = *reinterpret_cast<uint32_t*>(b + 8);
        out->head = *reinterpret_cast<int32_t*>(b + 12);
        out->count = *reinterpret_cast<uint32_t*>(b + 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// `head` n'est PAS garanti borné : le moteur ne le replie que dans la croissance
// de ring (FUN_1408de0b0 remet head=0) ; un compteur brut (head >= cap, voire
// négatif) est parfaitement exploitable puisque les index sont calculés modulo
// `cap`. On le NORMALISE donc au lieu de rejeter le registre : rejeter faisait
// échouer TOUTE l'injection en silence (aucune trace dans le log).
int32_t NormHead(int32_t head, uint32_t cap) {
    if (!cap) return 0;
    int64_t m = static_cast<int64_t>(head) % static_cast<int64_t>(cap);
    if (m < 0) m += static_cast<int64_t>(cap);
    return static_cast<int32_t>(m);
}

// M4 : refuse un objet qui n'a pas la géométrie d'un ComponentRegistry.
// `reason` (optionnel) reçoit un code de refus journalisable une seule fois.
enum class Why { kOk = 0, kRingCap, kRingCount, kRingBuf, kRingHead, kRingUncommitted };

bool ValidateRegistryEx(void* reg, Why* reason) {
    if (reason) *reason = Why::kRingCap;
    if (!reg) return false;
    __try {
        auto* r = static_cast<uint8_t*>(reg);
        auto okRing = [reason](uint8_t* b, uint32_t* outCap, uint32_t* outCount) -> bool {
            void** buf = *reinterpret_cast<void***>(b + 0);
            uint32_t cap = *reinterpret_cast<uint32_t*>(b + 8);
            int32_t head = *reinterpret_cast<int32_t*>(b + 12);
            uint32_t count = *reinterpret_cast<uint32_t*>(b + 16);
            if (outCap) *outCap = cap;
            if (outCount) *outCount = count;
            if (cap > (1u << 20)) { if (reason) *reason = Why::kRingCap; return false; }
            if (count > cap) { if (reason) *reason = Why::kRingCount; return false; }
            if (!cap) return true;            // ring vide : (buf=null, head=0, count=0) est valide
            if (!buf) { if (reason) *reason = Why::kRingBuf; return false; }
            // head hors bornes = toléré (normalisé à l'usage), mais bornage dur ci-dessous.
            if (head == INT32_MIN) { if (reason) *reason = Why::kRingHead; return false; }
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(buf, &mbi, sizeof(mbi))) { if (reason) *reason = Why::kRingBuf; return false; }
            if (mbi.State != MEM_COMMIT) { if (reason) *reason = Why::kRingUncommitted; return false; }
            return true;
        };
        if (!okRing(r + kOffVariants, nullptr, nullptr)) return false;
        if (!okRing(r + kOffDescriptors, nullptr, nullptr)) return false;
        if (!okRing(r + kOffRing, nullptr, nullptr)) return false;
        if (reason) *reason = Why::kOk;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { if (reason) *reason = Why::kRingBuf; return false; }
}

// (ValidateRegistryEx est le seul point d'entrée : plus de variante sans raison.)

const char* WhyText(Why w) {
    switch (w) {
        case Why::kOk: return "ok";
        case Why::kRingCap: return "capacite de ring aberrante";
        case Why::kRingCount: return "count > cap";
        case Why::kRingBuf: return "buffer de ring illisible";
        case Why::kRingHead: return "head invalide";
        case Why::kRingUncommitted: return "buffer de ring non commite";
    }
    return "?";
}

// Journalise une seule fois la géométrie complete des rings : indispensable pour
// diagnostiquer un refus (le log precedent n'affichait jamais ces valeurs).
void LogRingShapes(void* reg) {
    if (g_cat.shapesLogged || !reg) return;
    g_cat.shapesLogged = true;
    Ring v, d, a;
    if (!ReadRing(reg, kOffVariants, &v) || !ReadRing(reg, kOffDescriptors, &d) ||
        !ReadRing(reg, kOffRing, &a)) {
        LogWarn("Jet[diag] registre %p illisible", reg);
        return;
    }
    LogInfo("Jet[diag] registre=%p | variantes cap=%u head=%d count=%u | xml cap=%u head=%d count=%u | "
            "global cap=%u head=%d count=%u",
            reg, v.cap, v.head, v.count, d.cap, d.head, d.count, a.cap, a.head, a.count);
}

size_t RemoveOursFromRing(void* reg, size_t offset, bool descriptor);

bool RingHasId(void* reg, size_t offset, bool descriptor) {
    if (!reg) return false;
    __try {
        Ring r;
        if (!ReadRing(reg, offset, &r)) return false;
        if (!r.buf || !r.cap || r.count > 100000 || r.count > r.cap) return false;
        const int32_t head = NormHead(r.head, r.cap);
        for (uint32_t i = 0; i < r.count; ++i)
            if (EntryIsOurs(r.buf[static_cast<uint32_t>((static_cast<int64_t>(head) + i) % r.cap)], descriptor))
                return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return false;
}

// Retire nos entrées d'un ring en le compactant.
// B4 : la copie DOIT passer par un buffer temporaire — écrire buf[kept] pendant
// qu'on lit buf[(head+i)%cap] écrase des entrées non encore lues dès que head!=0
// (doublons => le moteur détruit deux fois la même variante => corruption de tas).
size_t RemoveOursFromRing(void* reg, size_t offset, bool descriptor) {
    size_t removed = 0;
    if (!reg) return 0;
    __try {
        auto* base = static_cast<uint8_t*>(reg) + offset;
        Ring r;
        if (!ReadRing(reg, offset, &r)) return 0;
        if (!r.buf || !r.cap || r.cap > (1u << 20) || r.count > r.cap) return 0;
        const int32_t head = NormHead(r.head, r.cap);
        void** buf = r.buf;
        const uint32_t cap = r.cap;
        const uint32_t count = r.count;
        if (cap > (1u << 16)) {
            // Ring énorme : pas de buffer temporaire de 512 Ko par thread. On
            // retire par ÉCHANGE avec la dernière entrée logique (l'ordre d'un
            // ring n'a pas de sens pour le moteur, qui le parcourt comme un
            // ensemble). Ne JAMAIS laisser nos entrées en place : c'est ce qui
            // faisait détruire nos définitions par le moteur au shutdown.
            uint32_t remaining = count;
            uint32_t i = 0;
            while (i < remaining) {
                uint32_t idx = static_cast<uint32_t>((static_cast<int64_t>(head) + i) % cap);
                void* e = buf[idx];
                if (!EntryIsOurs(e, descriptor) && !IsPublishedEntry(e)) { ++i; continue; }
                uint32_t last = static_cast<uint32_t>((static_cast<int64_t>(head) + remaining - 1) % cap);
                buf[idx] = buf[last];
                buf[last] = nullptr;
                --remaining;
                ++removed;
            }
            if (!removed) return 0;
            *reinterpret_cast<int32_t*>(base + 12) = head;
            *reinterpret_cast<uint32_t*>(base + 16) = remaining;
            return removed;
        }
        static __declspec(thread) void* keep[1 << 16];   // 512 Ko par thread, hors pile
        uint32_t kept = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t idx = static_cast<uint32_t>((static_cast<int64_t>(head) + i) % cap);
            void* e = buf[idx];
            if (EntryIsOurs(e, descriptor) || IsPublishedEntry(e)) { ++removed; continue; }
            keep[kept++] = e;
        }
        if (!removed) return 0;                       // rien a nous : ne rien reecrire
        for (uint32_t i = 0; i < kept; ++i) buf[i] = keep[i];
        for (uint32_t i = kept; i < count; ++i) buf[i] = nullptr;
        *reinterpret_cast<int32_t*>(base + 12) = 0;   // head
        *reinterpret_cast<uint32_t*>(base + 16) = kept;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return removed; }
    return removed;
}

// Pourquoi une insertion a été refusée. Distinguer « ring plein » de « ring pas
// encore alloué » est le cœur du diagnostic : le 2026-11-08 le log annonçait
// « ring des descripteurs plein » alors que ce ring était à `cap=0 head=0 count=0`,
// c'est-à-dire VIDE (jamais alloué). Le message masquait la vraie cause du report
// d'injection pendant tout le chargement de monde.
enum class PushResult { kOk = 0, kUnallocated, kFull, kInvalid };

const char* PushText(PushResult r) {
    switch (r) {
        case PushResult::kOk: return "ok";
        case PushResult::kUnallocated: return "ring non alloue (cap=0), allocation impossible";
        case PushResult::kFull: return "ring plein, croissance interdite dans une frame du jeu";
        case PushResult::kInvalid: return "geometrie de ring invalide";
    }
    return "?";
}

// Insère un pointeur dans un ring (buf, cap, head, count).
// `allowGrow` = false => jamais de REALLOC : le moteur peut parcourir le ring dans
// la même frame et garder un pointeur sur l'ancien buffer, qu'une croissance
// viendrait de libérer. C'est OBLIGATOIRE quand on injecte depuis une frame du jeu
// (hook de lookup).
// SEULE EXCEPTION (2026-11-08) : un ring JAMAIS ALLOUÉ (cap == 0, buf == nul) n'a
// aucun buffer à invalider — personne ne peut l'itérer — et FUN_1408de0b0 gère
// explicitement ce cas (`test rax,rax` sur l'ancien buffer avant la copie). On
// l'alloue donc même depuis une frame : c'est la seule façon d'injecter PENDANT le
// chargement, puisque la pulsation HUD (qui autorise la croissance) n'y tourne pas.
PushResult RingPush(void* reg, size_t offset, void* value, bool allowGrow) {
    // Jamais de pointeur nul ni "petit" (clone+k*0x2848 sur un clone nul) dans un ring du jeu.
    if (!reg || !value || reinterpret_cast<uintptr_t>(value) < 0x10000ull) return PushResult::kInvalid;
    __try {
        auto* base = static_cast<uint8_t*>(reg) + offset;
        Ring r;
        if (!ReadRing(reg, offset, &r)) return PushResult::kInvalid;
        if (r.cap > (1u << 20) || r.count > 100000 || r.count > r.cap) return PushResult::kInvalid;
        const RingGrowth growth = JetCatalogueRingGrowth(r.cap, r.count, allowGrow, g_cat.grow != nullptr);
        if (growth != RingGrowth::kNone) {
            g_cat.grow(base, r.cap > 0 ? r.cap * 2 : kRingInitialCapacity);
            if (!ReadRing(reg, offset, &r)) return PushResult::kInvalid;
            if (r.cap > (1u << 20) || r.count > r.cap) return PushResult::kInvalid;
        }
        if (!r.buf || !r.cap || r.count >= r.cap)
            return (r.cap == 0 || !r.buf) ? PushResult::kUnallocated : PushResult::kFull;
        const int32_t head = NormHead(r.head, r.cap);
        r.buf[static_cast<uint32_t>((static_cast<int64_t>(head) + r.count) % r.cap)] = value;
        *reinterpret_cast<uint32_t*>(base + 16) = r.count + 1;
        return PushResult::kOk;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return PushResult::kInvalid; }
}

// Une insertion refusée n'est PAS une injection abandonnée : on réessaiera (le
// chargement peut encore allouer le ring). Le log dit exactement pourquoi, une
// fois, puis un refus sur 200 pour rester lisible.
bool RingPushLogged(void* reg, size_t offset, void* value, bool allowGrow, const char* what) {
    const PushResult r = RingPush(reg, offset, value, allowGrow);
    if (r == PushResult::kOk) return true;
    if (++g_cat.pushFails == 1 || g_cat.pushFails % 200 == 0)
        LogWarn("Jet: insertion '%s' differee (%s) ; %d refus cumules", what, PushText(r),
                g_cat.pushFails);
    return false;
}

// Recherche de la définition SOURCE à cloner.
// L'identifiant interne n'est pas forcément exactement "jet_engine_combustion_chamber"
// (le jeu peut le stocker avec une autre casse, ou avec un suffixe). On fait donc
// trois passes, de la plus stricte à la plus large, et on renvoie l'id réellement
// trouvé pour qu'il soit journalisé :
//   0. id exact (insensible à la casse)
//   1. id commençant par l'id demandé (suffixe éventuel)
//   2. id contenant "combustion_chamber" et pas "unlimited" (la chambre vanilla)
bool IdContainsNoCase(const char* p, uint32_t len, const char* needle) {
    const size_t n = strlen(needle);
    if (!p || n > len) return false;
    for (uint32_t i = 0; i + n <= len; ++i)
        if (_strnicmp(p + i, needle, n) == 0) return true;
    return false;
}

void* FindInRegistryEx(void* reg, const char* id, char* outId, size_t outSize) {
    if (!reg || !id) return nullptr;
    void* best = nullptr;
    int bestScore = 99;
    const size_t wantLen = strlen(id);
    __try {
        Ring r;
        if (!ReadRing(reg, kOffRing, &r)) return nullptr;
        if (!r.buf || !r.cap || !r.count || r.count > 20000 || r.count > r.cap) return nullptr;
        const int32_t head = NormHead(r.head, r.cap);
        for (uint32_t i = 0; i < r.count; ++i) {
            auto* e = static_cast<uint8_t*>(r.buf[static_cast<uint32_t>((static_cast<int64_t>(head) + i) % r.cap)]);
            if (!e) continue;
            const char* p = *reinterpret_cast<const char* const*>(e + kOffId);
            uint32_t len = *reinterpret_cast<const uint32_t*>(e + kOffIdLen);
            if (!p || !len || len >= 128) continue;
            int score = 99;
            if (len == wantLen && _strnicmp(p, id, len) == 0) score = 0;
            else if (len > wantLen && _strnicmp(p, id, wantLen) == 0) score = 1;
            else if (!IdContainsNoCase(p, len, "unlimited") &&
                     IdContainsNoCase(p, len, "combustion_chamber")) score = 2;
            if (score >= bestScore) continue;
            bestScore = score;
            best = e;
            if (outId && outSize) {
                const uint32_t n = len < outSize - 1 ? len : static_cast<uint32_t>(outSize - 1);
                memcpy(outId, p, n);
                outId[n] = '\0';
            }
            if (score == 0) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return best;
}

// Ancien point d'entrée (id exact) : conservé par FindInRegistryEx, qui couvre
// désormais l'id exact, les suffixes et la recherche par sous-chaîne.

// Ring des variantes (+0xc8) : les 8 sous-définitions du clone.
// M2 : le XML est lu et validé AVANT toute insertion dans les rings.
std::string g_xml;

bool PrepareDescriptor() {
    g_xml.clear();
    if (g_cat.xmlPath.empty()) return false;
    std::wstring wide = Utf8ToWide(g_cat.xmlPath);
    HANDLE f = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        LogError("Jet: XML du composant introuvable (%s)", g_cat.xmlPath.c_str());
        return false;
    }
    char buf[8192];
    DWORD read = 0;
    bool truncated = false;
    while (ReadFile(f, buf, sizeof(buf), &read, nullptr) && read) {
        g_xml.append(buf, read);
        if (g_xml.size() > 256u * 1024u) { truncated = true; break; }
    }
    CloseHandle(f);
    if (truncated) {
        LogError("Jet: XML du composant trop volumineux ; refuse");
        g_xml.clear();
        return false;
    }
    if (g_xml.empty() || g_xml.find(char(0)) != std::string::npos) {
        LogError("Jet: XML du composant invalide (vide ou NUL interne) ; refuse");
        g_xml.clear();
        return false;
    }
    return true;
}

// Ring des variantes (+0xc8) : les 8 sous-définitions du clone.
bool EnsureVariants(void* reg, bool allowGrow) {
    if (!reg || !g_cat.definition) return false;
    if (RingHasId(reg, kOffVariants, false)) return true;
    for (size_t k = 0; k < kVariants; ++k) {
        if (!g_cat.variants[k]) return false;
        if (!RingPushLogged(reg, kOffVariants, g_cat.variants[k], allowGrow, "variantes"))
            return false;
    }
    return true;
}

// Ring des descripteurs (+0xe0) : { id(ptr), len, cap, xml(ptr), xmlLen, xmlCap }.
// id/xml alloués par l'allocateur aligné du jeu (_aligned_free compatible),
// desc par l'operator new du jeu (operator delete(desc,32) compatible).
// Construit UNE seule fois : une insertion refusée ne doit ni reconstruire les
// 18 Ko de XML ni fuiter les chaînes déjà allouées (l'API du jeu n'expose pas
// l'operator delete correspondant).
bool EnsureDescriptorStruct() {
    if (g_cat.descriptor) return true;
    if (g_xml.empty() || !g_engine.native) return false;
    if (!g_cat.descriptorId.data) StrMake(g_cat.descriptorId, kComponentId);
    if (!g_cat.descriptorXml.data) StrMake(g_cat.descriptorXml, g_xml.c_str());
    if (!g_cat.descriptorId.data || !g_cat.descriptorXml.data) return false;
    auto* desc = static_cast<uint8_t*>(g_engine.native->gameAlloc(g_engine.host, 32));
    if (!desc) return false;
    memset(desc, 0, 32);
    *reinterpret_cast<char**>(desc + 0x00) = g_cat.descriptorId.data;
    *reinterpret_cast<uint32_t*>(desc + 0x08) = g_cat.descriptorId.len;
    *reinterpret_cast<uint32_t*>(desc + 0x0c) = g_cat.descriptorId.cap;
    *reinterpret_cast<char**>(desc + 0x10) = g_cat.descriptorXml.data;
    *reinterpret_cast<uint32_t*>(desc + 0x18) = static_cast<uint32_t>(g_cat.descriptorXml.len);
    *reinterpret_cast<uint32_t*>(desc + 0x1c) = g_cat.descriptorXml.cap;
    g_cat.descriptor = desc;
    return true;
}

bool PushDescriptor(void* reg, bool allowGrow) {
    if (!reg) return false;
    if (RingHasId(reg, kOffDescriptors, true)) return true;
    if (!EnsureDescriptorStruct()) return false;
    return RingPushLogged(reg, kOffDescriptors, g_cat.descriptor, allowGrow, "descripteurs XML");
}

// Catégorie/type/valeur/flags effectifs de la définition SOURCE (clone vanilla).
struct SourceInfo {
    char id[128] = {};
    int32_t category = -1;
    uint32_t value = 0;
    uint64_t flags = 0;
    bool valid = false;
};

bool ReadSourceInfo(void* source, SourceInfo* out) {
    if (!source || !out) return false;
    __try {
        auto* s = static_cast<uint8_t*>(source);
        out->category = *reinterpret_cast<int32_t*>(s + kOffCategory);
        out->value = *reinterpret_cast<uint32_t*>(s + kOffValue);
        out->flags = *reinterpret_cast<uint64_t*>(s + kOffFlags);
        const char* p = *reinterpret_cast<const char* const*>(s + kOffId);
        const uint32_t len = *reinterpret_cast<const uint32_t*>(s + kOffIdLen);
        if (p && len && len < sizeof(out->id)) {
            memcpy(out->id, p, len);
            out->id[len] = '\0';
        }
        out->valid = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// Oublie le clone (sans rien libérer : l'API du jeu n'expose pas operator delete).
// Les sous-objets copiés depuis la définition vanilla appartiennent au moteur.
void ForgetDefinition() {
    g_cat.definition = nullptr;
    for (size_t k = 0; k < kVariants; ++k) g_cat.variants[k] = nullptr;
    g_cat.builtFor = nullptr;
    g_cat.builtCategory = -1;
    g_cat.sourceUsed = nullptr;   // provenance du clone : plus aucun clone vivant
}

// M2 : (1) CONSTRUCTION — clone des 8 variantes + descripteur XML, SANS toucher aux
// rings du jeu. Construite UNE seule fois par registre : une insertion refusée
// (ring non alloué, ring plein) ne doit ni refaire 8 x 0x2d08 octets à chaque
// tentative de lookup, ni fuiter ce qui est déjà alloué.
//
// Chaque variante est une ALLOCATION PROPRE à la taille réelle de l'objet moteur
// (0x2d08). Deux garanties en découlent, toutes deux nécessaires au shutdown :
//   1. aucun chevauchement -> détruire une variante ne touche jamais sa voisine ;
//   2. jamais « 8 delete sur un bloc unique » si le moteur détruit nos entrées.
//
// Le résultat DISTINGUE « source absente » (transitoire pendant le chargement : on
// réessaiera) de « refus » (géométrie/XML inexploitables) : le hook de lookup doit
// pouvoir journaliser la cause exacte quand le jeu demande notre identifiant.
enum class PrepareResult { kReady = 0, kNoSource, kRefused };

PrepareResult PrepareDefinition(void* reg) {
    if (g_cat.definition && g_cat.builtFor == reg) return PrepareResult::kReady;
    SourceInfo info;
    void* source = FindInRegistryEx(reg, kSourceId, info.id, sizeof(info.id));
    if (!source) {
        static int miss = 0;
        if (++miss == 1 || miss % 200 == 0)
            LogWarn("Jet: definition source '%s' absente du registre (essai %d)", kSourceId, miss);
        return PrepareResult::kNoSource;       // M3 : source pas encore chargée -> réessayer
    }
    // RÉUTILISATION SÛRE du clone : il n'est valable que si la source est EXACTEMENT la
    // définition dont il a été copié ET si aucun ComponentRegistry::Clear n'a eu lieu
    // entre-temps (Clear vide les rings et rend caducs les buffers de la source). En jeu,
    // deux registres vivants (monde et atelier) partagent la MÊME définition source : on
    // réinsère alors le clone existant au lieu de refaire 8 x 0x2d08 octets (et de fuiter
    // le précédent, que l'API du jeu ne permet pas de libérer) à chaque changement.
    if (g_cat.definition && g_cat.sourceUsed == source && g_cat.cloneEpoch == g_cat.epoch) {
        g_cat.builtFor = reg;
        return PrepareResult::kReady;
    }
    if (g_cat.definition) {                    // construit pour un AUTRE registre :
        ForgetDefinition();                    // son clone référence des buffers d'un
    }                                          // registre mort -> on reconstruit
    if (!ReadSourceInfo(source, &info)) return PrepareResult::kRefused;
    // Catégorie RÉELLE de la source. Les buckets vont de +0x110 à +0x290, soit
    // exactement 16 entrées de 24 octets (certifié par ComponentRegistry::Clear,
    // 0x5f1ac0 : base +0x110, stride 0x18, 16 éléments). Hors bornes => on refuse.
    if (info.category < 0 || info.category >= 16) {
        LogWarn("Jet: categorie source inattendue (%d) ; injection refusee", info.category);
        return PrepareResult::kRefused;
    }
    const int32_t category = info.category;
    // Flags : on CONSERVE ceux de la source, sauf 0x80 / 0x800 qui, dans
    // FUN_14046ec30 (0x46ec30), font sauter l'enregistrement dans le bucket de
    // catégorie — c'est-à-dire la présence au catalogue de l'atelier.
    const uint64_t flags = info.flags & ~static_cast<uint64_t>(0x880);
    LogInfo("Jet: source='%s' ptr=%p cat=%d value=%u flags=0x%llx -> clone cat=%d value=%u flags=0x%llx",
            info.id, source, info.category, info.value, static_cast<unsigned long long>(info.flags),
            category, kSignatureValue, static_cast<unsigned long long>(flags));
    // M2 : le XML du descripteur est lu et validé AVANT toute allocation puis insertion.
    if (!PrepareDescriptor()) return PrepareResult::kRefused;
    void* variants[kVariants] = {};
    for (size_t k = 0; k < kVariants; ++k) {
        variants[k] = g_engine.native->gameAlloc(g_engine.host, g_definitionBytes);
        if (!variants[k]) break;
        __try { memset(variants[k], 0, g_definitionBytes); }
        __except (EXCEPTION_EXECUTE_HANDLER) { variants[k] = nullptr; break; }
    }
    bool ok = variants[kVariants - 1] != nullptr;
    __try {
        for (size_t k = 0; k < kVariants && ok; ++k) {
            void* src = reinterpret_cast<void**>(source)[k];
            uint8_t* dst = static_cast<uint8_t*>(variants[k]);
            memcpy(dst, src ? src : source, g_definitionBytes);
            for (size_t j = 0; j < kVariants; ++j)
                reinterpret_cast<void**>(dst)[j] = variants[j];
            SwEngineString id = {}, name = {};
            StrMake(id, kComponentId);
            StrMake(name, kComponentName);
            if (!id.data || !name.data) { ok = false; break; }
            *reinterpret_cast<char**>(dst + kOffId) = id.data;
            *reinterpret_cast<uint32_t*>(dst + kOffIdLen) = id.len;
            *reinterpret_cast<uint32_t*>(dst + kOffIdCap) = id.cap;
            *reinterpret_cast<char**>(dst + kOffName) = name.data;
            *reinterpret_cast<uint32_t*>(dst + kOffNameLen) = name.len;
            *reinterpret_cast<uint32_t*>(dst + kOffNameCap) = name.cap;
            *reinterpret_cast<uint8_t*>(dst + kOffVariantIndex) = static_cast<uint8_t>(k);
            *reinterpret_cast<int32_t*>(dst + kOffCategory) = category;
            *reinterpret_cast<uint32_t*>(dst + kOffValue) = kSignatureValue;
            *reinterpret_cast<uint64_t*>(dst + kOffFlags) = flags;
        }
        if (ok) {
            g_cat.builtFor = reg;
            g_cat.builtCategory = category;
            // Provenance du clone : sert à décider d'une réutilisation sans re-clonage
            // quand un AUTRE registre demande le composant (voir plus haut).
            g_cat.sourceUsed = source;
            g_cat.cloneEpoch = g_cat.epoch;
            // Chaque pointeur publié est mémorisé : le retrait des rings se fait
            // aussi PAR IDENTITÉ (IsPublishedEntry), pas seulement par id.
            for (size_t k = 0; k < kVariants; ++k) g_cat.variants[k] = variants[k];
            g_cat.definition = variants[0];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok ? PrepareResult::kReady : PrepareResult::kRefused;
}

// M1 : (2) INSERTION — chaque écriture de ring est vérifiée et, au premier refus,
// TOUTES nos entrées sont retirées : jamais d'état partiel dans le registre vanilla.
// Un refus n'abandonne pas : le clone et le descripteur restent construits pour la
// tentative suivante (le chargement peut encore allouer le ring).
bool InsertDefinition(void* reg, bool allowGrow) {
    void* clone = g_cat.definition;             // variante 0 : ring global + bucket
    if (!reg || !clone || !g_cat.variants[0] || g_cat.builtCategory < 0) return false;
    const size_t bucket = kOffBuckets + static_cast<size_t>(g_cat.builtCategory) * kBucketStride;
    if (EnsureVariants(reg, allowGrow) && PushDescriptor(reg, allowGrow) &&
        RingPushLogged(reg, kOffRing, clone, allowGrow, "global") &&
        RingPushLogged(reg, bucket, clone, allowGrow, "categorie")) {
        g_cat.registered = true;
        return true;
    }
    RemoveOursFromRing(reg, kOffDescriptors, true);
    RemoveOursFromRing(reg, kOffVariants, false);
    RemoveOursFromRing(reg, kOffRing, false);
    RemoveOursFromRing(reg, bucket, false);
    return false;
}

// Une seule tentative d'injection, dans le registre déjà capturé.
// ATTENTION : l'appelant doit tenir g_lock (CRITICAL_SECTION réentrante).
// `allowGrow` = false quand on est appelé depuis une frame du jeu : on n'y
// provoque JAMAIS de realloc de ring (le moteur peut en parcourir un).
// `otherRegistry` = true quand le registre courant n'est PAS celui déjà servi (le monde et
// l'atelier ont chacun le leur) : l'entrée existe ailleurs mais pas ici, donc il faut la
// publier dans CE registre aussi. C'est le cas mesuré en jeu : après le chargement, le jeu
// résout les composants d'un véhicule dans un registre où notre entrée n'était pas.
bool TryInjectCurrentRegistry(bool allowGrow, bool otherRegistry = false) {
    if (!g_engine.native) return false;
    if (g_cat.shuttingDown) return false;       // PROCESS_DETACH : plus aucune ecriture
    if ((g_cat.registered && !otherRegistry) || g_cat.failed || g_cat.clearing || g_cat.inInject)
        return false;
    if (g_cat.injectDisabled) {
        static bool once = false;
        if (!once) { once = true; LogInfo("Jet: insertion au catalogue desactivee (marqueur) ; hooks de diagnostic seuls"); }
        return false;
    }
    void* reg = g_cat.registry;
    if (!reg) return false;
    Why why = Why::kRingCap;
    if (!ValidateRegistryEx(reg, &why)) {
        // Diagnostic borné : avant, un refus ici ne produisait AUCUN log.
        if (g_cat.validateFails < 3 || g_cat.validateFails % 300 == 0)
            LogWarn("Jet: registre %p refuse par validation (%s) essai %d",
                    reg, WhyText(why), g_cat.validateFails);
        ++g_cat.validateFails;
        return false;
    }
    g_cat.validateFails = 0;
    LogRingShapes(reg);
    g_cat.inInject = true;
    bool injected = false;
    __try {
        injected = PrepareDefinition(reg) == PrepareResult::kReady && InsertDefinition(reg, allowGrow);
    } __except (EXCEPTION_EXECUTE_HANDLER) { injected = false; }
    g_cat.inInject = false;
    if (injected) {
        g_cat.attempts = 0;
        __try {
            if (g_cat.locRegister) RegisterAllLocalization(g_cat.locRegister);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        LogInfo("Jet: composant '%s' injecte au catalogue (memoire, value=%u, registre=%p%s)",
                kComponentId, kSignatureValue, reg, otherRegistry ? ", second registre" : "");
        return true;
    }
    if (++g_cat.attempts % 600 == 0)
        LogWarn("Jet: injection catalogue encore en attente (%d tentatives)", g_cat.attempts);
    return false;
}

// Cadencement de la tentative faite par la pulsation UI (`JetCatalogueTick`), hors frame
// de jeu donc avec croissance de ring autorisée. Le hook de lookup, lui, n'est plus jamais
// temporisé à 250 ms : il tente à la première demande (voir kReinjectMinPeriodMs).
constexpr uint64_t kLookupAttemptPeriodMs = 250;

// Ré-injection déclenchée par un lookup : cadencement purement DÉFENSIF (on ne refait pas
// 8 x 0x2d08 pour chaque lookup d'un chargement). 10 ms = la première opportunité réelle,
// sans marteler — et surtout SANS le sondage de 250 ms qui laissait une fenêtre pendant
// laquelle le jeu ne trouvait plus notre composant.
constexpr uint64_t kReinjectMinPeriodMs = 10;

// RÉ-INJECTION AU PREMIER LOOKUP (quel qu'il soit).
//
// Mesuré en jeu le 2026-11-08 : `ComponentRegistry::Clear` retire nos entrées (obligatoire),
// puis l'atelier désérialise un véhicule SANS jamais appeler FindComponentDefinitionByName
// avec notre identifiant — la preuve : le véhicule a perdu exactement notre composant
// (`data/vehicles/tmp/autosave.xml` : 43 -> 42 composants, seul
// 'jet_engine_combustion_chamber_unlimited' a disparu) alors que le hook de lookup n'a
// jamais reçu ce nom. Attendre une demande explicite de notre id ne suffit donc PAS : on
// réinjecte dès le PREMIER lookup venu (le jeu en fait des centaines pendant un chargement :
// inventaires, prefabs…), donc bien avant la désérialisation des véhicules.
//
// `allowGrow` reste false : on est dans une frame du jeu, jamais de realloc de ring.
void MaybeReinjectOnLookup(void* registry) {
    if (!g_engine.native || !registry) return;
    if (g_cat.shuttingDown || g_cat.clearing || g_cat.inInject) return;
    if (g_cat.injectDisabled) return;
    if (!JetCatalogueNeedsInjection(g_cat.registered, g_cat.builtFor == registry)) return;
    const uint64_t now = GetTickCount64();
    if (g_cat.lastAttemptMs && now - g_cat.lastAttemptMs < kReinjectMinPeriodMs) return;
    LockAcquire();
    if (!g_cat.clearing && !g_cat.inInject) {
        Why why = Why::kRingCap;
        g_cat.lastAttemptMs = now;
        if (ValidateRegistryEx(registry, &why)) {
            const bool other = g_cat.registered;
            g_cat.registry = registry;
            TryInjectCurrentRegistry(/*allowGrow=*/false, other);
        } else if (g_cat.validateFails < 3 || g_cat.validateFails % 300 == 0) {
            LogWarn("Jet: registre %p non valide au lookup (%s) essai %d", registry, WhyText(why),
                    g_cat.validateFails);
            ++g_cat.validateFails;
        }
    }
    LockRelease();
}

// Bissection « est-ce notre injection qui bloque le chargement ? » : un fichier
// marqueur à côté du XML du composant (<mod>/.no_catalogue_injection) désactive
// UNIQUEMENT l'insertion au catalogue, sans toucher au reste du loader : un seul
// fichier à créer ou supprimer pour trancher.
// Activation de l'injection du catalogue : ACTIVE PAR DÉFAUT.
// Le composant « Jet Combustion Chamber Unlimited » doit réapparaître dans l'atelier sans
// variable d'environnement. Les deux hooks de ce module (`FindComponentDefinitionByName`,
// appelé en boucle par le jeu, et `ComponentRegistry::Clear`) s'exécutent PENDANT le
// chargement ; le blocage historique a été corrigé (croissance de ring limitée au seul cas
// `cap == 0`, clone construit une fois, rollback intégral). Si un blocage revenait avec
// l'injection active, `SW_JET_CATALOGUE=0` la désactive sans toucher au reste du loader.
bool JetCatalogueInjectionRequested() {
    wchar_t buffer[16] = {};
    const DWORD length = GetEnvironmentVariableW(L"SW_JET_CATALOGUE", buffer, 16);
    if (length == 0 || length >= 16) return true;   // absente (ou illisible) : ACTIF
    const wchar_t c = buffer[0];
    // Toute valeur explicitement négative désactive ; tout le reste (1, y, oui…) active.
    return !(c == L'0' || c == L'n' || c == L'N' || c == L'f' || c == L'F');
}

bool InjectionDisabledByMarker() {
    if (g_cat.xmlPath.empty()) return false;
    std::string marker = g_cat.xmlPath;
    const size_t slash = marker.find_last_of("\\/");
    marker = (slash == std::string::npos) ? std::string() : marker.substr(0, slash + 1);
    marker += ".no_catalogue_injection";
    const std::wstring wide = Utf8ToWide(marker);
    return GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Le nom demandé par le jeu est-il NOTRE identifiant ? (lecture SEH de l'EStr
// {ptr,len,cap} ; aucune écriture).
bool IsOurLookupName(const void* name) {
    if (!name) return false;
    __try {
        const auto* s = static_cast<const uint8_t*>(name);
        const char* data = *reinterpret_cast<const char* const*>(s + 0);
        const uint32_t len = *reinterpret_cast<const uint32_t*>(s + 8);
        return data && len && len <= 120 &&
               IsComponentId(reinterpret_cast<const uint8_t*>(data), len);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------------------
// INJECTION À LA DEMANDE — le correctif du composant qui disparaissait du build.
//
// Appelé UNIQUEMENT quand le jeu demande `jet_engine_combustion_chamber_unlimited`,
// avec le registre qu'il vient de passer en RCX. À cet instant précis la source
// vanilla est disponible (le jeu est justement en train de résoudre les identifiants du
// véhicule), donc le clone est construit et inséré MAINTENANT, avant que le jeu ne
// conclue « composant inconnu » et ne retire le composant du build.
//
// Anti-doublon : si nos pointeurs sont déjà dans les rings de CE registre, on renvoie
// l'existant sans re-cloner. Fail-closed : sans source, on renvoie null APRÈS avoir
// journalisé la cause exacte (jamais un null silencieux).
void* LookupOurComponent(void* registry) {
    const uint64_t started = GetTickCount64();
    // Lecture bon marché du ring global : nos 8 variantes + notre entrée globale y sont
    // publiées ensemble (InsertDefinition est tout-ou-rien).
    const bool published = registry != nullptr && RingHasId(registry, kOffRing, false);
    const LazyResult plan = JetCatalogueLazyPlan(true, published);
    if (plan == LazyResult::kPresent && g_cat.definition) {
        const uint32_t hits = ++g_cat.lazyHits;
        if (hits == 1 || hits % 500 == 0)
            LogInfo("Jet: lookup '%s' -> deja present (registre=%p, %u lookup(s) servis)",
                    kComponentId, registry, hits);
        return g_cat.definition;
    }
    if (g_cat.shuttingDown || g_cat.clearing) return nullptr;   // aucune écriture
    if (g_cat.injectDisabled) {
        static bool once = false;
        if (!once) {
            once = true;
            LogInfo("Jet: insertion au catalogue desactivee (marqueur) ; "
                    "le lookup '%s' restera non servi", kComponentId);
        }
        return nullptr;
    }
    if (!registry) {
        LogWarn("Jet: lookup '%s' mais registre nul -> clone impossible", kComponentId);
        return nullptr;
    }

    LazyResult result = LazyResult::kRefused;
    const char* whyText = "registre refuse par validation";
    LockAcquire();
    // Re-vérification sous verrou : un autre thread a pu injecter entre-temps.
    if (g_cat.definition && g_cat.builtFor == registry && RingHasId(registry, kOffRing, false)) {
        result = LazyResult::kPresent;
    } else {
        Why why = Why::kRingCap;
        if (!ValidateRegistryEx(registry, &why)) {
            whyText = WhyText(why);
            if (g_cat.validateFails < 3)
                LogWarn("Jet: registre %p non valide au lookup (%s)", registry, whyText);
            ++g_cat.validateFails;
        } else {
            g_cat.validateFails = 0;
            LogRingShapes(registry);
            // Le registre demandé devient le registre courant : la pulsation UI peut
            // ensuite réinjecter sans attendre un nouveau lookup.
            g_cat.registry = registry;
            g_cat.inInject = true;
            PrepareResult prepared = PrepareResult::kRefused;
            bool inserted = false;
            __try {
                prepared = PrepareDefinition(registry);
                inserted = prepared == PrepareResult::kReady &&
                           InsertDefinition(registry, /*allowGrow=*/false);
            } __except (EXCEPTION_EXECUTE_HANDLER) { inserted = false; }
            g_cat.inInject = false;
            // `allowGrow=false` : on est dans une frame du jeu, jamais de realloc de ring.
            result = JetCatalogueLazyOutcome(prepared == PrepareResult::kNoSource, inserted);
        }
    }
    const uint64_t elapsed = GetTickCount64() - started;
    void* served = nullptr;
    if (result == LazyResult::kInjected) {
        g_cat.attempts = 0;
        g_cat.lazyMisses = 0;
        __try {
            if (g_cat.locRegister) RegisterAllLocalization(g_cat.locRegister);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        // PREUVE attendue au prochain essai : le lookup de notre identifiant est servi
        // à la demande, donc AVANT la conclusion « composant inconnu » du jeu.
        LogInfo("Jet: lookup '%s' -> injecte a la demande (%llu ms, registre=%p)",
                kComponentId, static_cast<unsigned long long>(elapsed), registry);
        served = g_cat.definition;
    } else if (result == LazyResult::kPresent && g_cat.definition) {
        const uint32_t hits = ++g_cat.lazyHits;
        if (hits == 1 || hits % 500 == 0)
            LogInfo("Jet: lookup '%s' -> deja present (registre=%p, %u lookup(s) servis)",
                    kComponentId, registry, hits);
        served = g_cat.definition;
    } else {
        // FAIL-CLOSED : null, mais jamais silencieux.
        const uint32_t misses = ++g_cat.lazyMisses;
        if (misses <= 5 || misses % 200 == 0) {
            if (result == LazyResult::kSourceMissing)
                LogWarn("Jet: lookup '%s' mais source absente -> clone impossible "
                        "(registre=%p, %u demande(s) non servie(s))",
                        kComponentId, registry, misses);
            else
                LogWarn("Jet: lookup '%s' non servi (%s : %s) registre=%p -> null "
                        "(fail-closed, %u demande(s))",
                        kComponentId, JetCatalogueLazyResultText(result), whyText, registry,
                        misses);
        }
    }
    LockRelease();
    return served;
}

// Hook de `FindComponentDefinitionByName(registry, name)` : le registre arrive en RCX.
//
// L'injection n'est plus pilotée par sondage : elle est déclenchée PAR LA DEMANDE, au
// moment exact où le jeu résout NOTRE identifiant (désérialisation de véhicule, atelier,
// catalogue). C'est ce qui garantit la disponibilité du composant au moment où le jeu
// décide de le conserver ou de le RETIRER du build.
void* HookedFindDef(void* registry, const void* name) {
    void* (*original)(void*, const void*) = g_cat.originalFind;
    if (g_cat.shuttingDown) return original ? original(registry, name) : nullptr;

    if (IsOurLookupName(name)) {
        void* ours = LookupOurComponent(registry);
        if (!ours) return original ? original(registry, name) : nullptr;   // fail-closed
        // Preuve mesurable : le MOTEUR lui-même retrouve désormais l'entrée dans SES rings
        // (le clone est bien un composant de son catalogue, pas seulement un pointeur rendu).
        void* native = original ? original(registry, name) : nullptr;
        if (native) {
            const uint32_t ok = ++g_cat.nativeResolved;
            if (ok == 1 || ok % 500 == 0)
                LogInfo("Jet: lookup '%s' -> resolu par le moteur (%p, %u fois)",
                        kComponentId, native, ok);
            return native;
        }
        // Diagnostic borné : si le moteur ne voit pas l'entrée, c'est une information
        // capitale (le clone est servi, mais absent de SES rings) — on ne la répète pas
        // indéfiniment pour ne pas noyer le journal pendant la désérialisation.
        static uint32_t unresolved = 0;
        if (++unresolved == 1 || unresolved % 500 == 0)
            LogWarn("Jet: lookup '%s' -> injecte mais le moteur ne le resout pas encore "
                    "(registre=%p, %u fois) ; definition renvoyee pour ne pas perdre le "
                    "composant",
                    kComponentId, registry, unresolved);
        return ours;    }

    void* result = original ? original(registry, name) : nullptr;
    // Ré-injection « au premier lookup » : c'est elle qui garantit que le composant est
    // présent dans les rings AVANT la désérialisation d'un véhicule (l'atelier ne demande
    // jamais notre identifiant par ce hook — mesuré).
    MaybeReinjectOnLookup(registry);
    return result;
}

// Retire NOS entrées de TOUS les rings d'un registre. Idempotent : un ring sans
// nos entrées n'est que réécrit à l'identique. N'écrit que si le registre a la
// géométrie d'un ComponentRegistry (ValidateRegistryEx).
// Renvoie le nombre d'entrées retirées (0 = rien à nous).
size_t SweepRegistry(void* reg) {
    if (!reg) return 0;
    Why why = Why::kRingCap;
    if (!ValidateRegistryEx(reg, &why)) {
        LogWarn("Jet: balayage ignore, registre %p non valide (%s)", reg, WhyText(why));
        return 0;
    }
    size_t removed = 0;
    removed += RemoveOursFromRing(reg, kOffDescriptors, true);
    removed += RemoveOursFromRing(reg, kOffVariants, false);
    removed += RemoveOursFromRing(reg, kOffRing, false);
    // Le bucket est balayé sur les 16 catégories : on ne suppose pas que la
    // catégorie de la source est celle de la constante par défaut.
    for (size_t cat = 0; cat < 16; ++cat)
        removed += RemoveOursFromRing(reg, kOffBuckets + cat * kBucketStride, false);
    return removed;
}

// Avant que le moteur ne détruise/libère le contenu des rings, on retire nos
// entrées (sinon : destruction de nos définitions par les allocateurs du moteur).
//
// CAUSE RACINE DU CRASH AU SHUTDOWN (2026-11-08) : le retrait était conditionné à
// `g_cat.registry == registry`. Quand le moteur appelle Clear sur un registre
// DIFFÉRENT de celui capturé au lookup (l'atelier et le monde ont chacun le leur,
// et le monde est recréé à chaque chargement), le retrait était SILENCIEUXEMENT
// SAUTÉ : nos entrées restaient dans les rings et le moteur les détruisait avec
// ses allocateurs (preuve dans le log : la 2e initialisation de monde à 11:31:45
// réarme l'injection — donc Clear a bien été appelé — sans la ligne « entrees
// retirees »). Le retrait est désormais INCONDITIONNEL sur le registre reçu, et
// aussi tenté sur le registre mémorisé s'il est encore valide.
void HookedClear(void* registry) {
    LockAcquire();                             // B1 : pas d'injection concurrente
    g_cat.clearing = true;                     // M6 : HookedFindDef refuse d'injecter
    const void* injectedInto = g_cat.registry;
    const bool same = registry && registry == injectedInto;
    size_t removed = SweepRegistry(registry);
    // Le registre mémorisé n'est balayé que si l'on croit y avoir des entrées :
    // sinon ce pointeur peut désigner un objet déjà libéré (monde détruit).
    if (!same && injectedInto && g_cat.registered) removed += SweepRegistry(g_cat.registry);
    if (removed || g_cat.registered || !same)
        LogInfo("Jet: entrees retirees avant ComponentRegistry::Clear "
                "(registre=%p, injection=%p, identiques=%d, %zu entree(s))",
                registry, injectedInto, same ? 1 : 0, removed);
    if (g_cat.originalClear) g_cat.originalClear(registry);
    // Le moteur vient de vider les rings. Le registre vit DANS l'objet monde
    // (monde+0xbb670) : au CHARGEMENT d'un monde, Clear() est appelé puis les
    // définitions sont rechargées — garder le pointeur permet donc à la
    // pulsation HUD (en jeu) de réinjecter sans attendre un lookup. Conserver le
    // pointeur reste sûr même si Clear accompagne un démontage : l'injection exige
    // de trouver la définition SOURCE complète dans le ring global, ce qui n'est
    // possible que dans un registre réellement repeuplé.
    ForgetDefinition();
    // Le descripteur a peut-être été libéré par le moteur (DestructXmlDescriptors)
    // s'il n'a pas pu être retiré du ring juste avant Clear : on oublie nos
    // poignées au lieu de risquer un usage après libération. Il est reconstruit à
    // la prochaine injection (fuite bornée : 1 descripteur par Clear).
    g_cat.descriptor = nullptr;
    g_cat.descriptorId = SwEngineString{};
    g_cat.descriptorXml = SwEngineString{};
    // Les rings viennent d'être vidés : le clone ne doit JAMAIS être réutilisé tel quel
    // (ses buffers copiés viennent d'un état du registre qui n'existe plus). `epoch`
    // invalide toute réutilisation ; la ré-injection aura lieu au PREMIER lookup de notre
    // identifiant (injection à la demande), pas après un sondage.
    ++g_cat.epoch;
    g_cat.sourceUsed = nullptr;
    g_cat.lazyHits = 0;
    g_cat.lazyMisses = 0;
    g_cat.nativeResolved = 0;
    g_cat.pushFails = 0;
    g_cat.registered = false;
    g_cat.failed = false;                      // un AV passé ne condamne pas la session
    g_cat.attempts = 0;
    g_cat.validateFails = 0;
    g_cat.lastAttemptMs = 0;                   // autorise une tentative immédiate
    g_cat.clearing = false;
    if (registry && ValidateRegistryEx(registry, nullptr)) g_cat.registry = registry;
    else g_cat.registry = nullptr;
    LockRelease();
}

void RegisterLoc(LocRegisterFn fn, const char* key, const char* value) {
    if (!fn || !key || !value) return;
    SwEngineString k = {}, v = {};
    StrMake(k, key);
    StrMake(v, value);
    __try { fn(&k, &v); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    StrRelease(k);
    StrRelease(v);
}

void RegisterAllLocalization(LocRegisterFn fn) {
    char key[160];
    RegisterLoc(fn, "DEF_JET_ENGINE_COMBUSTION_CHAMBER_UNLIMITED_NAME", kComponentName);
    snprintf(key, sizeof(key), "def_%s_name", kComponentId);
    RegisterLoc(fn, key, kComponentName);
    snprintf(key, sizeof(key), "def_%s_desc", kComponentId);
    RegisterLoc(fn, key,
                "A combustion chamber that burns jet fuel to sustain a jet engine. "
                "This one is not limited by the over-revolution protection (added by StormFabric).");
    snprintf(key, sizeof(key), "def_%s_s_desc", kComponentId);
    RegisterLoc(fn, key, "Unlimited RPS combustion chamber");
    static const char* kNodeLabels[5] = {"Fuel", "Throttle", "Air Pressure", "Temperature", "RPS"};
    static const char* kNodeDesc[5] = {
        "Jet fuel input required for combustion.",
        "Throttle control between 0 and 1.",
        "The pressure of air transferred by the chamber.",
        "The temperature of air transferred by the chamber.",
        "The rotations per second generated by the chamber."};
    for (int i = 0; i < 5; ++i) {
        snprintf(key, sizeof(key), "def_%s_node_%d_label", kComponentId, i);
        RegisterLoc(fn, key, kNodeLabels[i]);
        snprintf(key, sizeof(key), "def_%s_node_%d_desc", kComponentId, i);
        RegisterLoc(fn, key, kNodeDesc[i]);
    }
}

// Les écritures de code passent EXCLUSIVEMENT par l'API native du loader :
// validation VirtualQuery, PAGE_EXECUTE_READWRITE, restauration de la protection
// d'origine, FlushInstructionCache, et refus si les octets du prologue ne
// correspondent pas au contrat validé. Le mod ne contient plus aucune écriture
// directe dans le code du jeu.

// Hook inline d'un prologue de `patchSize` octets (frontière d'instruction
// garantie par kFindLens / kClearLens). Le trampoline appartient au loader et
// sert d'original : aucun octet n'est copié ni libéré par le mod.
bool InstallHook(uint8_t* target, void* hook, size_t patchSize, SwHandle* handleOut,
                 void** originalOut) {
    if (!g_engine.native || !g_engine.host || !target || !hook || !handleOut || !originalOut) return false;
    if (patchSize < 5 || patchSize > 32) return false;
    uint8_t expected[32] = {};
    if (g_engine.native->readMemory(g_engine.host, target, expected, patchSize) != SW_OK) return false;
    void* trampoline = nullptr;
    SwHandle handle = 0;
    if (g_engine.native->hookInline(g_engine.host, target, hook, expected, patchSize,
                                    &trampoline, &handle) != SW_OK) return false;
    *originalOut = trampoline;
    *handleOut = handle;
    return true;
}

}  // namespace

size_t JetCatalogueBuildJump(uint8_t* out, size_t patchSize, void* target);

RingGrowth JetCatalogueRingGrowth(uint32_t cap, uint32_t count, bool allowGrow, bool hasGrowFn) {
    if (count < cap) return RingGrowth::kNone;      // reste de la place : rien à faire
    if (!hasGrowFn) return RingGrowth::kNone;       // aucune croissance disponible
    // cap == 0 : le ring n'a JAMAIS été alloué (aucun buffer existant) -> allouer
    // n'invalide rien et reste sûr même depuis une frame du jeu. C'est la SEULE
    // croissance autorisée : « croissance de ring autorisée uniquement pour
    // l'allocation d'un ring jamais alloué (cap==0) ».
    if (cap == 0) return RingGrowth::kAllocate;
    // cap > 0 : un vrai realloc invaliderait le buffer que le moteur peut parcourir.
    // Refus systématique, même hors frame : la protection est conservée telle quelle.
    (void)allowGrow;
    return RingGrowth::kNone;
}

// ---------------------------------------------------------------------------
// Décisions PURES de l'injection à la demande (testables hors du jeu).
//
// Étape 1 : que faire avant toute écriture ? Le seul cas qui autorise une écriture est
// `kInject` ; `kPresent` est l'anti-doublon (aucun re-clonage, aucun rejeu de 8 x 0x2d08)
// et `kNotOurs` laisse le lookup du jeu strictement inchangé.
LazyResult JetCatalogueLazyPlan(bool isOurs, bool publishedInThisRegistry) {
    if (!isOurs) return LazyResult::kNotOurs;
    if (publishedInThisRegistry) return LazyResult::kPresent;
    return LazyResult::kInject;
}

// Étape 2 : issue d'une tentative. La source absente n'est JAMAIS confondue avec un
// succès : c'est le cas « le jeu demande notre id et nous ne pouvons pas le fournir »,
// qui doit renvoyer null et être journalisé (fail-closed).
LazyResult JetCatalogueLazyOutcome(bool sourceMissing, bool insertOk) {
    if (sourceMissing) return LazyResult::kSourceMissing;
    if (!insertOk) return LazyResult::kRefused;
    return LazyResult::kInjected;
}

bool JetCatalogueNeedsInjection(bool registered, bool builtForThisRegistry) {
    return !(registered && builtForThisRegistry);
}

const char* JetCatalogueLazyResultText(LazyResult result) {
    switch (result) {
        case LazyResult::kNotOurs: return "nom etranger";
        case LazyResult::kPresent: return "deja present";
        case LazyResult::kInject: return "injection requise";
        case LazyResult::kInjected: return "injecte a la demande";
        case LazyResult::kSourceMissing: return "source absente";
        case LazyResult::kRefused: return "injection refusee";
    }
    return "inconnu";
}

size_t JetCatalogueBuildJump(uint8_t* out, size_t patchSize, void* target) {
    if (!out || !target || patchSize < 14 || patchSize > 24) return 0;
    memset(out, 0x90, patchSize);
    out[0] = 0xFF; out[1] = 0x25;                  // jmp qword ptr [rip+disp32]
    *reinterpret_cast<int32_t*>(out + 2) = 0;      // disp32 = 0 (obligatoire)
    *reinterpret_cast<uint64_t*>(out + 6) = reinterpret_cast<uint64_t>(target);
    return patchSize;
}

bool InitJetCatalogueInjection(const SwModHost* host, const char* xmlPath) {
    if (g_cat.findTarget) return true;
    if (!g_engine.native || !g_engine.host || !host) return false;
    // ACTIF PAR DÉFAUT : seuls les hooks de catalogue sont posés.
    if (!JetCatalogueInjectionRequested()) {
        LogInfo("Jet: injection catalogue desactivee (SW_JET_CATALOGUE=0) ; hooks non poses");
        return false;
    }
    const uint32_t build = g_engine.build;
    if (!build) {
        LogError("Jet: build moteur non certifie ; injection catalogue annulee");
        return false;
    }
    // Disposition de l'objet « définition » PROPRE AU BUILD (0x2d08 / 0x2848).
    // L'identité de build vient du profil certificaté du loader, jamais d'une RVA.
    g_definitionBytes = static_cast<size_t>(JetCatalogueDefinitionBytes(static_cast<int>(build)));
    LogInfo("Jet: disposition definition build %u : %zu o par variante (dernier champ detruit +0x%llX)",
            build, g_definitionBytes,
            static_cast<unsigned long long>(JetCatalogueDestroyedTail(static_cast<int>(build))));
    // Services moteur génériques résolus par le profil de découverte du loader.
    auto service = [host](const char* role) -> uint8_t* {
        return static_cast<uint8_t*>(
            const_cast<void*>(g_engine.native->findSymbol(host, role)));
    };
    uint8_t* findTarget = service("engine.component.lookup");
    uint8_t* clearTarget = service("engine.component.clear");
    uint8_t* locRegister = service("engine.localization.register");
    uint8_t* grow = service("engine.component.grow");
    if (!findTarget || !clearTarget || !locRegister || !grow) {
        LogError("Jet: services catalogue non resolus par le profil (lookup=%p clear=%p loc=%p grow=%p) "
                 "; aucun hook installe",
                 (void*)findTarget, (void*)clearTarget, (void*)locRegister, (void*)grow);
        return false;
    }
    if (!g_lockInit) { InitializeCriticalSection(&g_lock); g_lockInit = true; }
    g_cat.locRegister = reinterpret_cast<LocRegisterFn>(locRegister);
    g_cat.grow = reinterpret_cast<GrowFn>(grow);
    if (xmlPath && *xmlPath) g_cat.xmlPath = xmlPath;
    g_cat.injectDisabled = InjectionDisabledByMarker();
    if (g_cat.injectDisabled)
        LogWarn("Jet: marqueur .no_catalogue_injection present -> composant NON injecte "
                "(hooks de diagnostic conserves) ; le catalogue reste 100%% vanilla");
    // Hook du lookup de définitions.
    if (!InstallHook(findTarget, reinterpret_cast<void*>(&HookedFindDef), kFindPatch,
                     &g_cat.findHandle, reinterpret_cast<void**>(&g_cat.originalFind))) {
        LogError("Jet: hook FindComponentDefinitionByName refuse par l'API native");
        return false;
    }
    g_cat.findTarget = findTarget;
    // Hook de ComponentRegistry::Clear (nettoyage avant destruction par le moteur).
    if (!InstallHook(clearTarget, reinterpret_cast<void*>(&HookedClear), kClearPatch,
                     &g_cat.clearHandle, reinterpret_cast<void**>(&g_cat.originalClear))) {
        g_engine.native->hookRestore(g_engine.host, g_cat.findHandle);
        g_cat.findHandle = 0;
        g_cat.findTarget = nullptr;
        LogError("Jet: hook ComponentRegistry::Clear refuse ; injection annulee");
        return false;
    }
    g_cat.clearTarget = clearTarget;
    LogInfo("Jet: hooks catalogue installes (build %u) : lookup + ComponentRegistry::Clear", build);
    return true;
}

// Injection proactive, appelée depuis les pulsations UI (menu principal) ET depuis
// la mise à jour du HUD / de l'éditeur (donc EN JEU, là où le registre existe).
// La croissance de ring est autorisée ici : on n'est pas dans une frame où le
// moteur parcourt les rings du registre.
void JetCatalogueTick() {
    if (g_cat.shuttingDown) return;            // PROCESS_DETACH : plus aucune ecriture
    if (!g_cat.findTarget || g_cat.registered || g_cat.failed || g_cat.clearing) return;
    if (!g_engine.native) return;
    if (!g_cat.registry) {
        static bool once = false;
        if (!once) { once = true; LogInfo("Jet[tick] en attente du registre (findTarget=1 api=1)"); }
        return;
    }
    LockAcquire();
    if (!g_cat.registered && !g_cat.failed && !g_cat.clearing) {
        const uint64_t now = GetTickCount64();
        if (g_cat.lastAttemptMs == 0 || now - g_cat.lastAttemptMs >= kLookupAttemptPeriodMs) {
            g_cat.lastAttemptMs = now;
            TryInjectCurrentRegistry(true);
        }
    }
    LockRelease();
}

// Démontage (DllMain / DLL_PROCESS_DETACH). IDEMPOTENT et sans danger :
//   1. on désarme l'injection (plus AUCUNE écriture dans les rings du jeu) ;
//   2. on retire nos entrées des rings — AVANT de rendre les hooks au moteur, pour
//      que le Clear qui suit ne puisse pas détruire nos définitions ;
//   3. on restaure les octets d'origine des deux prologues patchés ;
//   4. on oublie l'état. On ne libère JAMAIS de mémoire du jeu (ni nos chaînes,
//      ni nos clones) : c'est le prix d'un démontage qui ne peut pas fauter.
// Les trampolines ne sont volontairement PAS libérés : un thread du jeu peut
// encore les exécuter au moment du PROCESS_DETACH (le processus meurt juste
// après, les fuites sont bornées et sans conséquence).
void ShutdownJetCatalogueInjection() {
    if (!g_cat.findTarget && !g_cat.clearTarget) return;   // déjà démonté
    LockAcquire();
    g_cat.shuttingDown = true;
    g_cat.clearing = true;
    const size_t removed = g_cat.registered ? SweepRegistry(g_cat.registry) : 0;
    if (removed)
        LogInfo("Jet: arret - %zu entree(s) retiree(s) du catalogue avant restauration des hooks", removed);
    LockRelease();
    // Ordre inverse de l'installation ; sw_hook_restore est idempotent.
    if (g_cat.clearHandle) g_engine.native->hookRestore(g_engine.host, g_cat.clearHandle);
    if (g_cat.findHandle) g_engine.native->hookRestore(g_engine.host, g_cat.findHandle);
    g_cat.clearHandle = 0;
    g_cat.findHandle = 0;
    g_cat.findTarget = nullptr;
    g_cat.clearTarget = nullptr;
    g_cat.findTramp = nullptr;
    g_cat.clearTramp = nullptr;
    g_cat.originalFind = nullptr;
    g_cat.originalClear = nullptr;
    g_cat.registered = false;
    ForgetDefinition();
    g_cat.descriptor = nullptr;
    g_cat.descriptorId = SwEngineString{};
    g_cat.descriptorXml = SwEngineString{};
    LogInfo("Jet: hooks catalogue restaures (demontage)");
}

// ---------------------------------------------------------------------------
// API PURE (testable hors jeu) : géométrie du clone et cohérence des chaînes.
// Un pas de variante inférieur à la taille réelle de l'objet moteur fait empiéter
// chaque variante sur la suivante : détruire la variante k libère alors des
// champs de la variante k+1 -> double free et pointeur bidon dans _aligned_free
// (crash au shutdown). C'est exactement ce que testent ces deux fonctions.
CloneGeometry JetCatalogueCloneGeometry(uint64_t variantBytes, uint32_t variants,
                                       uint64_t destroyedTail) {
    CloneGeometry g{};
    g.variantBytes = variantBytes;
    g.variants = variants;
    g.blockBytes = variantBytes * static_cast<uint64_t>(variants);
    // Le destructeur moteur libère le champ situé à `destroyedTail` (0x2D00 sur
    // 1.15.23) : s'il ne tient pas dans une variante, la destruction déborde sur
    // la variante voisine -> double free / pointeur bidon.
    g.overlapping = !FieldInsideVariant(variantBytes, destroyedTail, 8);
    return g;
}

// Taille d'une ComponentDefinition et décalage du dernier champ détruit, PAR
// BUILD (purs, testables). La disposition a changé entre 1.15.18 (0x2848) et
// 1.15.23 (0x2d08) ; coder une seule valeur en dur a produit le crash.
uint64_t JetCatalogueDefinitionBytes(int build) {
    return build == 1 ? kDefinitionBytesOld : kDefinitionBytesNew;
}

uint64_t JetCatalogueDestroyedTail(int build) {
    return build == 1 ? kOffDestroyedTailOld : kOffDestroyedTailNew;
}

// Une chaîne du moteur est {ptr, len, cap}. Le destructeur du jeu fait, pour un
// objet situé à l'adresse `self` : si (len != 0) { si (ptr != 0) free(ptr) }.
// Est cohérent : cap >= 32 (capacité minimale du moteur), len <= cap, et
// ptr non nul dès que len > 0. Une paire (len, cap) lue à la place d'un pointeur
// (cas du chevauchement de variantes) est donc détectée : c'est 0x0000002000000000
// pour len=0 / cap=0x20.
bool JetCatalogueStringCoherent(uint64_t ptr, uint32_t len, uint32_t cap) {
    if (cap < 32) return false;
    if (len > cap) return false;
    if (len && !ptr) return false;
    return true;
}

}  // namespace jet
