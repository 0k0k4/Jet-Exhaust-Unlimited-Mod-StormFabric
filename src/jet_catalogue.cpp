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

// Both hooks of this module are GENERIC ENGINE SERVICES: the StormFabric loader
// resolves them from its discovery profile (retained byte contract, validated
// before any write) and exposes them through the native API. The mod therefore
// contains NO executable address.
//
// Prologue sizes: derived from the certified instruction lengths, which
// guarantees an instruction is never cut in half (MOV RBX,RCX = 48 8B D9).
constexpr size_t kFindLens[] = {5, 5, 1, 1, 1, 1, 2};          // mov, mov, push x4, push r12
constexpr size_t kClearLens[] = {5, 5, 1, 4, 3};                // mov, mov, push, sub, mov
constexpr size_t Sum(const size_t* v, size_t n) {
    size_t t = 0; for (size_t i = 0; i < n; ++i) t += v[i]; return t;
}
constexpr size_t kFindPatch = Sum(kFindLens, sizeof(kFindLens) / sizeof(kFindLens[0]));
constexpr size_t kClearPatch = Sum(kClearLens, sizeof(kClearLens) / sizeof(kClearLens[0]));
static_assert(kFindPatch == 16, "Find prologue: instruction boundary changed");
static_assert(kClearPatch == 18, "Clear prologue: instruction boundary changed");

// Layout of a definition (0x2d08 bytes, 8 chained variants).
//
// SHUTDOWN CRASH (crash.dmp of 2026-11-08, AV 0xC0000005 reading 0x1FFFFFFFF8 at
// stormworks64.exe+0x9B2D5D, i.e. INSIDE the engine's string release): a single
// 0x2848 value was hardcoded, while the object size CHANGED between the two
// reviewed builds. On 1.15.23 the real size of a ComponentDefinition is 0x2d08,
// certified by THREE independent sites:
//   * 0x46EEC0 `mov ecx,0x2d08 ; call 0x958C28` -> operator new of a definition
//     by the component loader;
//   * 0x8B9FEE `mov edx,0x2d08 ; call 0x95881C` -> operator delete(ptr,size) of
//     EACH variant by the variants ring destructor;
//   * 0x87FDB0 (definition destructor) frees fields up to +0x2D00, so beyond
//     0x2848.
// With a 0x2848 stride on 1.15.23 our 8 variants OVERLAPPED by 0x4C0 bytes:
// destroying variant k destroyed/freed fields belonging to variant k+1 (double
// free, then a (len,cap) string pair read back as a pointer ->
// str_free(0x0000002000000000) -> read of [rcx-8] = 0x0000001FFFFFFFF8 -> AV).
// See tools/crash_dump.py (rebuilt stack) and docs/ENGINE_FUNCTION_MAPPING.md.
constexpr size_t kDefinitionBytesOld = 0x2848;   // 1.15.18 (historical reference)
constexpr size_t kDefinitionBytesNew = 0x2d08;   // 1.15.23 (certified, see above)
constexpr size_t kVariants = 8;
// EFFECTIVE size, set at initialisation from the detected build.
size_t g_definitionBytes = kDefinitionBytesNew;  // NOLINT: module state, like g_cat
static_assert(kDefinitionBytesNew >= 0x2d00 + 8, "a definition must cover its destroyed fields (+0x2D00)");
constexpr size_t kOffVariantIndex = 0x40;
// Offsets derived from the certified layout table (jet_catalogue.h): a single
// source of truth, so reading the source and writing the clone can never
// diverge.
constexpr size_t kOffId = kDefinitionLayout.idPtr;
constexpr size_t kOffIdLen = kDefinitionLayout.idLen;
constexpr size_t kOffIdCap = kDefinitionLayout.idCap;
constexpr size_t kOffName = kDefinitionLayout.namePtr;
constexpr size_t kOffNameLen = kDefinitionLayout.nameLen;
constexpr size_t kOffNameCap = kDefinitionLayout.nameCap;
constexpr size_t kOffCategory = kDefinitionLayout.category;
constexpr size_t kOffValue = kDefinitionLayout.value;
constexpr size_t kOffFlags = kDefinitionLayout.flags;
static_assert(kOffFlags == 0x2b8, "flags offset certified on 1.15.23");
static_assert(kDefinitionLayout.flagsBytes == sizeof(uint64_t),
              "flags must be written as 64 bits");
// Last field touched by the definition destructor (0x87FDB0 frees
// [def+0x2CE8] and [def+0x2CF8], then writes [def+0x2CF0] / [def+0x2D00]): this
// field MUST fit in a variant, otherwise the destruction spills onto the
// neighbouring variant (see JetCatalogueCloneGeometry).
constexpr size_t kOffDestroyedTailNew = 0x2d00;   // 1.15.23
constexpr size_t kOffDestroyedTailOld = 0x2840;   // 1.15.18 (size - 8)
// Registry rings.
constexpr size_t kOffVariants = 0xc8;
constexpr size_t kOffDescriptors = 0xe0;
constexpr size_t kOffRing = 0xf8;
constexpr size_t kOffBuckets = 0x110;
constexpr size_t kBucketStride = 24;

// Capacity requested when a never-allocated ring (cap == 0) must be ALLOCATED.
constexpr uint32_t kRingInitialCapacity = 1024;

constexpr char kComponentId[] = "jet_engine_combustion_chamber_unlimited";
constexpr char kComponentName[] = "Jet Combustion Chamber Unlimited";
constexpr char kSourceId[] = "jet_engine_combustion_chamber";
constexpr uint32_t kSignatureValue = 500123;
constexpr int32_t kCategory = 11;   // "JET ENGINES" (historical value, unused: the real category is read from the source)

using FindDefFn = void* (*)(void*, const void*);
using LocRegisterFn = void (*)(const SwEngineString*, const SwEngineString*);
using GrowFn = void (*)(void*, uint32_t);
using ClearFn = void (*)(void*);
void RegisterAllLocalization(LocRegisterFn fn);

// Clone geometry: PURE and testable off-game (see JetCatalogueCloneGeometry).
// A stride smaller than the size of a definition makes each variant spill onto
// the next one: any destruction by the engine then becomes fatal.
bool FieldInsideVariant(uint64_t stride, uint64_t offset, uint64_t size) {
    return stride != 0 && offset + size <= stride;
}

struct State {
    // FindComponentDefinitionByName hook
    uint8_t* findTarget = nullptr;
    uint8_t* findTramp = nullptr;
    FindDefFn originalFind = nullptr;
    SwHandle findHandle = 0;      // loader handle (sw_hook_restore)
    // ComponentRegistry::Clear hook
    uint8_t* clearTarget = nullptr;
    uint8_t* clearTramp = nullptr;
    ClearFn originalClear = nullptr;
    SwHandle clearHandle = 0;
    LocRegisterFn locRegister = nullptr;
    GrowFn grow = nullptr;
    std::string xmlPath;
    void* registry = nullptr;     // registry we injected into
    void* definition = nullptr;   // variant 0 (entry of the global/category rings)
    // Each variant is its OWN allocation of the engine object's real size: the
    // variants ring destructor frees every entry with operator delete(ptr,
    // 0x2d08). A single block cut into 8 (old scheme) made any destruction by the
    // engine fatal (8 deletes on one block).
    void* variants[kVariants] = {};
    void* builtFor = nullptr;     // registry `definition` was built for
    int32_t builtCategory = -1;   // category (bucket) read from the source definition
    void* descriptor = nullptr;   // 32-byte XML descriptor (built ONCE)
    SwEngineString descriptorId = {}, descriptorXml = {};
    bool registered = false;      // inserted into the rings (never rebuild)
    bool failed = false;
    bool clearing = false;        // B1/M6: Clear in progress -> do not inject
    bool shuttingDown = false;    // DllMain(PROCESS_DETACH): no more writes
    int attempts = 0;             // M3: bounded transient failures (no permanent "failed")
    bool inInject = false;        // re-entrancy: injection started from the lookup hook
    bool injectDisabled = false;  // bisection: <mod>/.no_catalogue_injection marker
    bool shapesLogged = false;    // ring diagnostics already logged
    int validateFails = 0;        // consecutive refused validations (diagnostics)
    int pushFails = 0;            // refused ring insertions (diagnostics, see RingPushLogged)
    uint64_t lastAttemptMs = 0;   // pacing of the attempts made from the lookup
    // On-demand injection: where the clone comes from and how old it is.
    void* sourceUsed = nullptr;   // source definition COPIED into the current clone
    uint32_t epoch = 0;           // incremented on every ComponentRegistry::Clear
    uint32_t cloneEpoch = 0;      // value of `epoch` when the clone was built
    uint32_t lazyHits = 0;        // lookups of our id already served by the existing entry
    uint32_t lazyMisses = 0;      // lookups of our id not served (fail-closed)
    uint32_t nativeResolved = 0;  // lookups where the ENGINE found our entry
};
State g_cat;
// B1: serialises injection / Clear / ring writes. CRITICAL_SECTION is
// re-entrant (SRWLOCK is not): the hook can be called again through the
// trampoline.
CRITICAL_SECTION g_lock;
bool g_lockInit = false;
inline void LockAcquire() { if (g_lockInit) EnterCriticalSection(&g_lock); }
inline void LockRelease() { if (g_lockInit) LeaveCriticalSection(&g_lock); }

bool IsComponentId(const uint8_t* p, uint32_t len) {
    return p && len == sizeof(kComponentId) - 1 && _strnicmp(reinterpret_cast<const char*>(p), kComponentId, len) == 0;
}

// Reads the id of an entry according to the ring type.
//   definitions (variants / global / bucket): id at +0x288 (ptr) / +0x290 (len)
//   descriptors (+0xe0)                      : id at +0x00 (ptr) / +0x08 (len)
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

// Identity of the blocks WE published (clone + descriptor). This is the safest
// removal criterion: it depends on no field of the object, so it works even if
// the engine (or a concurrent write) corrupted the id. The shutdown crash came
// precisely from an entry left in the rings: the engine destroyed it with ITS
// allocators and its object size.
bool IsPublishedEntry(const void* entry) {
    if (!entry) return false;
    if (entry == g_cat.descriptor) return true;
    for (size_t k = 0; k < kVariants; ++k)
        if (entry == g_cat.variants[k]) return true;
    return false;
}

// ---------------------------------------------------------------------------
// View of a registry ring: {buf +0, cap +8, head +12, count +16} (stride 24).
// All the ring guards are gathered here: no other function reads/writes a ring
// without going through these helpers.
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

// `head` is NOT guaranteed to be in range: the engine only wraps it in the ring
// growth (FUN_1408de0b0 resets head=0); a raw counter (head >= cap, even
// negative) is perfectly usable since the indices are computed modulo `cap`. We
// therefore NORMALISE it instead of rejecting the registry: rejecting made the
// WHOLE injection fail silently (no trace in the log).
int32_t NormHead(int32_t head, uint32_t cap) {
    if (!cap) return 0;
    int64_t m = static_cast<int64_t>(head) % static_cast<int64_t>(cap);
    if (m < 0) m += static_cast<int64_t>(cap);
    return static_cast<int32_t>(m);
}

// M4: refuses an object that does not have the geometry of a ComponentRegistry.
// `reason` (optional) receives a refusal code that can be logged once.
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
            if (!cap) return true;            // empty ring: (buf=null, head=0, count=0) is valid
            if (!buf) { if (reason) *reason = Why::kRingBuf; return false; }
            // out-of-range head = tolerated (normalised when used), but hard bound below.
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

// (ValidateRegistryEx is the only entry point: no variant without a reason.)

const char* WhyText(Why w) {
    switch (w) {
        case Why::kOk: return "ok";
        case Why::kRingCap: return "absurd ring capacity";
        case Why::kRingCount: return "count > cap";
        case Why::kRingBuf: return "unreadable ring buffer";
        case Why::kRingHead: return "invalid head";
        case Why::kRingUncommitted: return "ring buffer not committed";
    }
    return "?";
}

// Logs the full ring geometry once: essential to diagnose a refusal (the old
// log never showed these values).
void LogRingShapes(void* reg) {
    if (g_cat.shapesLogged || !reg) return;
    g_cat.shapesLogged = true;
    Ring v, d, a;
    if (!ReadRing(reg, kOffVariants, &v) || !ReadRing(reg, kOffDescriptors, &d) ||
        !ReadRing(reg, kOffRing, &a)) {
        LogWarn("Jet[diag] registry %p unreadable", reg);
        return;
    }
    LogInfo("Jet[diag] registry=%p | variants cap=%u head=%d count=%u | xml cap=%u head=%d count=%u | "
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

// Removes our entries from a ring by compacting it.
// B4: the copy MUST go through a temporary buffer — writing buf[kept] while
// reading buf[(head+i)%cap] overwrites entries not read yet as soon as head!=0
// (duplicates => the engine destroys the same variant twice => heap corruption).
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
            // Huge ring: no 512 KB temporary buffer per thread. Remove by SWAPPING
            // with the last logical entry (the order of a ring means nothing to
            // the engine, which walks it as a set). NEVER leave our entries in
            // place: that is what made the engine destroy our definitions on
            // shutdown.
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
        static __declspec(thread) void* keep[1 << 16];   // 512 KB per thread, off the stack
        uint32_t kept = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t idx = static_cast<uint32_t>((static_cast<int64_t>(head) + i) % cap);
            void* e = buf[idx];
            if (EntryIsOurs(e, descriptor) || IsPublishedEntry(e)) { ++removed; continue; }
            keep[kept++] = e;
        }
        if (!removed) return 0;                       // nothing of ours: rewrite nothing
        for (uint32_t i = 0; i < kept; ++i) buf[i] = keep[i];
        for (uint32_t i = kept; i < count; ++i) buf[i] = nullptr;
        *reinterpret_cast<int32_t*>(base + 12) = 0;   // head
        *reinterpret_cast<uint32_t*>(base + 16) = kept;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return removed; }
    return removed;
}

// Why an insertion was refused. Telling "ring full" from "ring not allocated
// yet" is the heart of the diagnostics: on 2026-11-08 the log reported
// "descriptors ring full" while that ring was at `cap=0 head=0 count=0`, i.e.
// EMPTY (never allocated). The message hid the real cause of the injection
// being postponed during the whole world load.
enum class PushResult { kOk = 0, kUnallocated, kFull, kInvalid };

const char* PushText(PushResult r) {
    switch (r) {
        case PushResult::kOk: return "ok";
        case PushResult::kUnallocated: return "ring not allocated (cap=0), allocation impossible";
        case PushResult::kFull: return "ring full, growth forbidden inside a game frame";
        case PushResult::kInvalid: return "invalid ring geometry";
    }
    return "?";
}

// Inserts a pointer into a ring (buf, cap, head, count).
// `allowGrow` = false => never a REALLOC: the engine may walk the ring in the
// same frame and keep a pointer to the old buffer, which a growth would free.
// This is MANDATORY when injecting from a game frame (lookup hook).
// ONLY EXCEPTION (2026-11-08): a NEVER-ALLOCATED ring (cap == 0, buf == null)
// has no buffer to invalidate — nobody can iterate it — and FUN_1408de0b0
// explicitly handles that case (`test rax,rax` on the old buffer before the
// copy). It is therefore allocated even from a frame: that is the only way to
// inject DURING the load, since the HUD pulse (which allows growth) does not run
// then.
PushResult RingPush(void* reg, size_t offset, void* value, bool allowGrow) {
    // Never a null or "small" pointer (clone+k*0x2848 on a null clone) in a game ring.
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

// A refused insertion is NOT an abandoned injection: we will retry (the load may
// still allocate the ring). The log says exactly why, once, then one refusal in
// 200 to stay readable.
bool RingPushLogged(void* reg, size_t offset, void* value, bool allowGrow, const char* what) {
    const PushResult r = RingPush(reg, offset, value, allowGrow);
    if (r == PushResult::kOk) return true;
    if (++g_cat.pushFails == 1 || g_cat.pushFails % 200 == 0)
        LogWarn("Jet: insertion into '%s' postponed (%s); %d refusals so far", what, PushText(r),
                g_cat.pushFails);
    return false;
}

// Looking up the SOURCE definition to clone.
// The internal id is not necessarily exactly "jet_engine_combustion_chamber"
// (the game may store it with another case, or with a suffix). So there are
// three passes, from the strictest to the widest, and the id actually found is
// returned so it can be logged:
//   0. exact id (case-insensitive)
//   1. id starting with the requested id (possible suffix)
//   2. id containing "combustion_chamber" and not "unlimited" (the vanilla chamber)
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

// Old entry point (exact id): covered by FindInRegistryEx, which now handles the
// exact id, suffixes and the substring search.

// Variants ring (+0xc8): the 8 sub-definitions of the clone.
// M2: the XML is read and validated BEFORE any insertion into the rings.
std::string g_xml;

bool PrepareDescriptor() {
    g_xml.clear();
    if (g_cat.xmlPath.empty()) return false;
    std::wstring wide = Utf8ToWide(g_cat.xmlPath);
    HANDLE f = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        LogError("Jet: component XML not found (%s)", g_cat.xmlPath.c_str());
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
        LogError("Jet: component XML too large; refused");
        g_xml.clear();
        return false;
    }
    if (g_xml.empty() || g_xml.find(char(0)) != std::string::npos) {
        LogError("Jet: invalid component XML (empty or embedded NUL); refused");
        g_xml.clear();
        return false;
    }
    return true;
}

// Variants ring (+0xc8): the 8 sub-definitions of the clone.
bool EnsureVariants(void* reg, bool allowGrow) {
    if (!reg || !g_cat.definition) return false;
    if (RingHasId(reg, kOffVariants, false)) return true;
    for (size_t k = 0; k < kVariants; ++k) {
        if (!g_cat.variants[k]) return false;
        if (!RingPushLogged(reg, kOffVariants, g_cat.variants[k], allowGrow, "variants"))
            return false;
    }
    return true;
}

// Descriptors ring (+0xe0): { id(ptr), len, cap, xml(ptr), xmlLen, xmlCap }.
// id/xml allocated by the game's aligned allocator (_aligned_free compatible),
// desc by the game's operator new (operator delete(desc,32) compatible).
// Built ONCE: a refused insertion must neither rebuild the 18 KB of XML nor leak
// the strings already allocated (the game API does not expose the matching
// operator delete).
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
    return RingPushLogged(reg, kOffDescriptors, g_cat.descriptor, allowGrow, "XML descriptors");
}

// Effective category/type/value/flags of the SOURCE definition (vanilla clone).
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

// Forgets the clone (without freeing anything: the game API does not expose
// operator delete). The sub-objects copied from the vanilla definition belong to
// the engine.
void ForgetDefinition() {
    g_cat.definition = nullptr;
    for (size_t k = 0; k < kVariants; ++k) g_cat.variants[k] = nullptr;
    g_cat.builtFor = nullptr;
    g_cat.builtCategory = -1;
    g_cat.sourceUsed = nullptr;   // where the clone came from: no live clone any more
}

// M2: (1) BUILDING — clone of the 8 variants + XML descriptor, WITHOUT touching
// the game's rings. Built only ONCE per registry: a refused insertion (ring not
// allocated, ring full) must neither redo 8 x 0x2d08 bytes on every lookup
// attempt nor leak what is already allocated.
//
// Each variant is its OWN ALLOCATION of the engine object's real size (0x2d08).
// Two guarantees follow, both needed on shutdown:
//   1. no overlap -> destroying a variant never touches its neighbour;
//   2. never "8 deletes on a single block" if the engine destroys our entries.
//
// The result TELLS "source missing" (transient during the load: we will retry)
// from "refused" (unusable geometry/XML): the lookup hook must be able to log
// the exact cause when the game asks for our id.
enum class PrepareResult { kReady = 0, kNoSource, kRefused };

PrepareResult PrepareDefinition(void* reg) {
    if (g_cat.definition && g_cat.builtFor == reg) return PrepareResult::kReady;
    SourceInfo info;
    void* source = FindInRegistryEx(reg, kSourceId, info.id, sizeof(info.id));
    if (!source) {
        static int miss = 0;
        if (++miss == 1 || miss % 200 == 0)
            LogWarn("Jet: source definition '%s' missing from the registry (attempt %d)", kSourceId, miss);
        return PrepareResult::kNoSource;       // M3: source not loaded yet -> retry
    }
    // SAFE REUSE of the clone: it is only valid if the source is EXACTLY the
    // definition it was copied from AND no ComponentRegistry::Clear happened in
    // between (Clear empties the rings and makes the source's buffers stale). In
    // game, two live registries (world and workbench) share the SAME source
    // definition: the existing clone is then reinserted instead of redoing
    // 8 x 0x2d08 bytes (and leaking the previous one, which the game API cannot
    // free) on every change.
    if (g_cat.definition && g_cat.sourceUsed == source && g_cat.cloneEpoch == g_cat.epoch) {
        g_cat.builtFor = reg;
        return PrepareResult::kReady;
    }
    if (g_cat.definition) {                    // built for ANOTHER registry:
        ForgetDefinition();                    // its clone refers to buffers of a
    }                                          // dead registry -> rebuild
    if (!ReadSourceInfo(source, &info)) return PrepareResult::kRefused;
    // REAL category of the source. The buckets span +0x110 to +0x290, that is
    // exactly 16 entries of 24 bytes (certified by ComponentRegistry::Clear,
    // 0x5f1ac0: base +0x110, stride 0x18, 16 elements). Out of range => refused.
    if (info.category < 0 || info.category >= 16) {
        LogWarn("Jet: unexpected source category (%d); injection refused", info.category);
        return PrepareResult::kRefused;
    }
    const int32_t category = info.category;
    // Flags: the source's flags are KEPT, except 0x80 / 0x800 which, in
    // FUN_14046ec30 (0x46ec30), skip the registration in the category bucket —
    // that is, the presence in the workbench catalogue.
    const uint64_t flags = info.flags & ~static_cast<uint64_t>(0x880);
    LogInfo("Jet: source='%s' ptr=%p cat=%d value=%u flags=0x%llx -> clone cat=%d value=%u flags=0x%llx",
            info.id, source, info.category, info.value, static_cast<unsigned long long>(info.flags),
            category, kSignatureValue, static_cast<unsigned long long>(flags));
    // M2: the descriptor XML is read and validated BEFORE any allocation then insertion.
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
            CloneFieldValues fields{};
            fields.id = id.data;
            fields.idLen = id.len;
            fields.idCap = id.cap;
            fields.name = name.data;
            fields.nameLen = name.len;
            fields.nameCap = name.cap;
            fields.category = category;
            fields.value = kSignatureValue;
            fields.flags = flags;   // uint64_t: the engine field is 8 bytes
            JetCatalogueWriteCloneFields(dst, fields);
            *reinterpret_cast<uint8_t*>(dst + kOffVariantIndex) = static_cast<uint8_t>(k);
        }
        if (ok) {
            g_cat.builtFor = reg;
            g_cat.builtCategory = category;
            // Where the clone comes from: used to decide on a reuse without
            // re-cloning when ANOTHER registry asks for the component (see above).
            g_cat.sourceUsed = source;
            g_cat.cloneEpoch = g_cat.epoch;
            // Every published pointer is remembered: removal from the rings also
            // works BY IDENTITY (IsPublishedEntry), not only by id.
            for (size_t k = 0; k < kVariants; ++k) g_cat.variants[k] = variants[k];
            g_cat.definition = variants[0];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok ? PrepareResult::kReady : PrepareResult::kRefused;
}

// M1: (2) INSERTION — every ring write is checked and, on the first refusal,
// ALL our entries are removed: never a partial state in the vanilla registry.
// A refusal does not give up: the clone and the descriptor stay built for the
// next attempt (the load may still allocate the ring).
bool InsertDefinition(void* reg, bool allowGrow) {
    void* clone = g_cat.definition;             // variant 0: global ring + bucket
    if (!reg || !clone || !g_cat.variants[0] || g_cat.builtCategory < 0) return false;
    const size_t bucket = kOffBuckets + static_cast<size_t>(g_cat.builtCategory) * kBucketStride;
    if (EnsureVariants(reg, allowGrow) && PushDescriptor(reg, allowGrow) &&
        RingPushLogged(reg, kOffRing, clone, allowGrow, "global") &&
        RingPushLogged(reg, bucket, clone, allowGrow, "category")) {
        g_cat.registered = true;
        return true;
    }
    RemoveOursFromRing(reg, kOffDescriptors, true);
    RemoveOursFromRing(reg, kOffVariants, false);
    RemoveOursFromRing(reg, kOffRing, false);
    RemoveOursFromRing(reg, bucket, false);
    return false;
}

// A single injection attempt, in the registry already captured.
// WARNING: the caller must hold g_lock (re-entrant CRITICAL_SECTION).
// `allowGrow` = false when called from a game frame: a ring realloc is NEVER
// caused there (the engine may be walking one).
// `otherRegistry` = true when the current registry is NOT the one already served
// (the world and the workbench each have their own): the entry exists elsewhere
// but not here, so it must be published in THIS registry too. This is the case
// measured in game: after loading, the game resolves a vehicle's components in a
// registry where our entry was not.
bool TryInjectCurrentRegistry(bool allowGrow, bool otherRegistry = false) {
    if (!g_engine.native) return false;
    if (g_cat.shuttingDown) return false;       // PROCESS_DETACH: no more writes
    if ((g_cat.registered && !otherRegistry) || g_cat.failed || g_cat.clearing || g_cat.inInject)
        return false;
    if (g_cat.injectDisabled) {
        static bool once = false;
        if (!once) { once = true; LogInfo("Jet: catalogue insertion disabled (marker); diagnostic hooks only"); }
        return false;
    }
    void* reg = g_cat.registry;
    if (!reg) return false;
    Why why = Why::kRingCap;
    if (!ValidateRegistryEx(reg, &why)) {
        // Bounded diagnostics: before, a refusal here produced NO log at all.
        if (g_cat.validateFails < 3 || g_cat.validateFails % 300 == 0)
            LogWarn("Jet: registry %p rejected by validation (%s) attempt %d",
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
        LogInfo("Jet: component '%s' injected into the catalogue (memory, value=%u, registry=%p%s)",
                kComponentId, kSignatureValue, reg, otherRegistry ? ", second registry" : "");
        return true;
    }
    if (++g_cat.attempts % 600 == 0)
        LogWarn("Jet: catalogue injection still pending (%d attempts)", g_cat.attempts);
    return false;
}

// Pacing of the attempt made by the UI pulse (`JetCatalogueTick`), outside a
// game frame, so with ring growth allowed. The lookup hook itself is never
// throttled to 250 ms any more: it tries on the first request (see
// kReinjectMinPeriodMs).
constexpr uint64_t kLookupAttemptPeriodMs = 250;

// Reinjection triggered by a lookup: purely DEFENSIVE pacing (8 x 0x2d08 is not
// redone for every lookup of a load). 10 ms = the first real opportunity,
// without hammering — and above all WITHOUT the 250 ms polling that left a
// window during which the game no longer found our component.
constexpr uint64_t kReinjectMinPeriodMs = 10;

// REINJECTION ON THE FIRST LOOKUP (whatever it is).
//
// Measured in game on 2026-11-08: `ComponentRegistry::Clear` removes our entries
// (mandatory), then the workbench deserializes a vehicle WITHOUT ever calling
// FindComponentDefinitionByName with our id — the proof: the vehicle lost
// exactly our component (`data/vehicles/tmp/autosave.xml`: 43 -> 42 components,
// only 'jet_engine_combustion_chamber_unlimited' disappeared) while the lookup
// hook never received that name. Waiting for an explicit request of our id is
// therefore NOT enough: we reinject on the FIRST lookup that comes (the game
// does hundreds of them during a load: inventories, prefabs…), so well before
// the vehicles are deserialized.
//
// `allowGrow` stays false: we are inside a game frame, never a ring realloc.
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
            LogWarn("Jet: registry %p invalid at lookup (%s) attempt %d", registry, WhyText(why),
                    g_cat.validateFails);
            ++g_cat.validateFails;
        }
    }
    LockRelease();
}

// Bisection "is our injection what blocks the load?": a marker file next to the
// component XML (<mod>/.no_catalogue_injection) turns off ONLY the catalogue
// insertion, without touching the rest of the loader: a single file to create or
// delete to decide.
//
// Catalogue injection switch: ON BY DEFAULT.
// The "Jet Combustion Chamber Unlimited" component must show up in the workbench
// without any environment variable. Both hooks of this module
// (`FindComponentDefinitionByName`, called in a loop by the game, and
// `ComponentRegistry::Clear`) run DURING the load; the historical deadlock was
// fixed (ring growth limited to the `cap == 0` case, clone built once, full
// rollback). If a deadlock came back with the injection on,
// `SW_JET_CATALOGUE=0` turns it off without touching the rest of the loader.
bool JetCatalogueInjectionRequested() {
    wchar_t buffer[16] = {};
    const DWORD length = GetEnvironmentVariableW(L"SW_JET_CATALOGUE", buffer, 16);
    if (length == 0 || length >= 16) return true;   // missing (or unreadable): ON
    const wchar_t c = buffer[0];
    // Any explicitly negative value turns it off; anything else (1, y, yes…) turns it on.
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

// Is the name requested by the game OUR id? (SEH read of the EStr
// {ptr,len,cap}; no write.)
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
// ON-DEMAND INJECTION — the fix for the component that disappeared from the build.
//
// Called ONLY when the game asks for `jet_engine_combustion_chamber_unlimited`,
// with the registry it just passed in RCX. At that exact moment the vanilla
// source is available (the game is precisely resolving the vehicle's ids), so
// the clone is built and inserted NOW, before the game concludes "unknown
// component" and removes the component from the build.
//
// De-duplication: if our pointers are already in THIS registry's rings, the
// existing entry is returned without re-cloning. Fail-closed: without a source,
// null is returned AFTER logging the exact cause (never a silent null).
void* LookupOurComponent(void* registry) {
    const uint64_t started = GetTickCount64();
    // Cheap read of the global ring: our 8 variants + our global entry are
    // published together (InsertDefinition is all-or-nothing).
    const bool published = registry != nullptr && RingHasId(registry, kOffRing, false);
    const LazyResult plan = JetCatalogueLazyPlan(true, published);
    if (plan == LazyResult::kPresent && g_cat.definition) {
        const uint32_t hits = ++g_cat.lazyHits;
        if (hits == 1 || hits % 500 == 0)
            LogInfo("Jet: lookup '%s' -> already present (registry=%p, %u lookup(s) served)",
                    kComponentId, registry, hits);
        return g_cat.definition;
    }
    if (g_cat.shuttingDown || g_cat.clearing) return nullptr;   // no write
    if (g_cat.injectDisabled) {
        static bool once = false;
        if (!once) {
            once = true;
            LogInfo("Jet: catalogue insertion disabled (marker); "
                    "the lookup of '%s' will not be served", kComponentId);
        }
        return nullptr;
    }
    if (!registry) {
        LogWarn("Jet: lookup of '%s' with a null registry -> cannot clone", kComponentId);
        return nullptr;
    }

    LazyResult result = LazyResult::kRefused;
    const char* whyText = "registry rejected by validation";
    LockAcquire();
    // Check again under the lock: another thread may have injected meanwhile.
    if (g_cat.definition && g_cat.builtFor == registry && RingHasId(registry, kOffRing, false)) {
        result = LazyResult::kPresent;
    } else {
        Why why = Why::kRingCap;
        if (!ValidateRegistryEx(registry, &why)) {
            whyText = WhyText(why);
            if (g_cat.validateFails < 3)
                LogWarn("Jet: registry %p invalid at lookup (%s)", registry, whyText);
            ++g_cat.validateFails;
        } else {
            g_cat.validateFails = 0;
            LogRingShapes(registry);
            // The requested registry becomes the current one: the UI pulse can
            // then reinject without waiting for a new lookup.
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
            // `allowGrow=false`: we are inside a game frame, never a ring realloc.
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
        // Expected PROOF on the next run: the lookup of our id is served on
        // demand, so BEFORE the game concludes "unknown component".
        LogInfo("Jet: lookup '%s' -> injected on demand (%llu ms, registry=%p)",
                kComponentId, static_cast<unsigned long long>(elapsed), registry);
        served = g_cat.definition;
    } else if (result == LazyResult::kPresent && g_cat.definition) {
        const uint32_t hits = ++g_cat.lazyHits;
        if (hits == 1 || hits % 500 == 0)
            LogInfo("Jet: lookup '%s' -> already present (registry=%p, %u lookup(s) served)",
                    kComponentId, registry, hits);
        served = g_cat.definition;
    } else {
        // FAIL-CLOSED: null, but never silent.
        const uint32_t misses = ++g_cat.lazyMisses;
        if (misses <= 5 || misses % 200 == 0) {
            if (result == LazyResult::kSourceMissing)
                LogWarn("Jet: lookup of '%s' but the source is missing -> cannot clone "
                        "(registry=%p, %u request(s) not served)",
                        kComponentId, registry, misses);
            else
                LogWarn("Jet: lookup of '%s' not served (%s: %s) registry=%p -> null "
                        "(fail-closed, %u request(s))",
                        kComponentId, JetCatalogueLazyResultText(result), whyText, registry,
                        misses);
        }
    }
    LockRelease();
    return served;
}

// Hook of `FindComponentDefinitionByName(registry, name)`: the registry comes in
// RCX.
//
// The injection is no longer driven by polling: it is triggered BY THE REQUEST,
// at the exact moment the game resolves OUR id (vehicle deserialization,
// workbench, catalogue). That is what guarantees the component is available when
// the game decides whether to keep it or REMOVE it from the build.
void* HookedFindDef(void* registry, const void* name) {
    void* (*original)(void*, const void*) = g_cat.originalFind;
    if (g_cat.shuttingDown) return original ? original(registry, name) : nullptr;

    if (IsOurLookupName(name)) {
        void* ours = LookupOurComponent(registry);
        if (!ours) return original ? original(registry, name) : nullptr;   // fail-closed
        // Measurable proof: the ENGINE itself now finds the entry in ITS rings (the
        // clone really is a component of its catalogue, not just a returned
        // pointer).
        void* native = original ? original(registry, name) : nullptr;
        if (native) {
            const uint32_t ok = ++g_cat.nativeResolved;
            if (ok == 1 || ok % 500 == 0)
                LogInfo("Jet: lookup '%s' -> resolved by the engine (%p, %u times)",
                        kComponentId, native, ok);
            return native;
        }
        // Bounded diagnostics: if the engine does not see the entry, that is key
        // information (the clone is served, but missing from ITS rings) — it is not
        // repeated forever so the log is not flooded during deserialization.
        static uint32_t unresolved = 0;
        if (++unresolved == 1 || unresolved % 500 == 0)
            LogWarn("Jet: lookup '%s' -> injected but the engine does not resolve it yet "
                    "(registry=%p, %u times); definition returned so the component is "
                    "not lost",
                    kComponentId, registry, unresolved);
        return ours;    }

    void* result = original ? original(registry, name) : nullptr;
    // Reinjection "on the first lookup": this is what guarantees the component is
    // in the rings BEFORE a vehicle is deserialized (the workbench never asks for
    // our id through this hook — measured).
    MaybeReinjectOnLookup(registry);
    return result;
}

// Removes OUR entries from ALL the rings of a registry. Idempotent: a ring
// without our entries is just rewritten as it was. Only writes if the registry
// has the geometry of a ComponentRegistry (ValidateRegistryEx).
// Returns the number of entries removed (0 = nothing of ours).
size_t SweepRegistry(void* reg) {
    if (!reg) return 0;
    Why why = Why::kRingCap;
    if (!ValidateRegistryEx(reg, &why)) {
        LogWarn("Jet: sweep skipped, registry %p invalid (%s)", reg, WhyText(why));
        return 0;
    }
    size_t removed = 0;
    removed += RemoveOursFromRing(reg, kOffDescriptors, true);
    removed += RemoveOursFromRing(reg, kOffVariants, false);
    removed += RemoveOursFromRing(reg, kOffRing, false);
    // The bucket is swept over the 16 categories: we do not assume the source's
    // category is the one of the default constant.
    for (size_t cat = 0; cat < 16; ++cat)
        removed += RemoveOursFromRing(reg, kOffBuckets + cat * kBucketStride, false);
    return removed;
}

// Before the engine destroys/frees the content of a registry's rings, our
// entries are removed from it (otherwise: our definitions destroyed with the
// engine's allocators).
//
// ROOT CAUSE OF THE SHUTDOWN CRASH (2026-11-08): the removal was conditioned on
// `g_cat.registry == registry`. When the engine called Clear on a registry
// DIFFERENT from the one captured at lookup (the workbench and the world each
// have their own, and the world is recreated on every load), the removal was
// SILENTLY SKIPPED: our entries stayed in the rings and the engine destroyed
// them with its allocators (evidence in the log: the 2nd world initialisation at
// 11:31:45 re-arms the injection — so Clear was called — without the "entries
// removed" line). The removal is now UNCONDITIONAL on the registry received.
void HookedClear(void* registry) {
    LockAcquire();                             // B1: no concurrent injection
    g_cat.clearing = true;                     // M6: HookedFindDef refuses to inject
    const void* injectedInto = g_cat.registry;
    const bool same = registry && registry == injectedInto;
    // Only the registry ACTUALLY being cleared is swept. Also sweeping the
    // remembered registry (old behaviour) removed the component from the LIVE
    // registry when the game cleared a secondary one: when a vehicle goes from the
    // map back to the workbench, the game clears another registry, then reads the
    // vehicle back in the main registry without a lookup of our id -> the
    // component disappeared from the build (measured on 2026-11-18:
    // Clear(…09470) removed 11 entries from …CC570).
    const size_t removed = SweepRegistry(registry);
    if (removed || g_cat.registered || !same)
        LogInfo("Jet: entries removed before ComponentRegistry::Clear "
                "(registry=%p, injection=%p, same=%d, %zu entr(ies))",
                registry, injectedInto, same ? 1 : 0, removed);
    if (g_cat.originalClear) g_cat.originalClear(registry);
    // A secondary registry was cleared while our injection registry still holds
    // our entries: the injection state stays valid, nothing is forgotten (the
    // clone and the descriptor are still published in that registry). Read-only,
    // under SEH.
    if (!same && injectedInto && g_cat.registered && RingHasId(g_cat.registry, kOffRing, false)) {
        if (g_cat.builtFor == registry) g_cat.builtFor = g_cat.registry;
        LogInfo("Jet: Clear of a secondary registry (%p); component kept in %p",
                registry, injectedInto);
        g_cat.clearing = false;
        LockRelease();
        return;
    }
    // The engine just emptied the rings. The registry lives INSIDE the world
    // object (world+0xbb670): when a world LOADS, Clear() is called and then the
    // definitions are reloaded — keeping the pointer therefore lets the HUD pulse
    // (in game) reinject without waiting for a lookup. Keeping the pointer stays
    // safe even if Clear comes with a teardown: the injection requires finding the
    // complete SOURCE definition in the global ring, which is only possible in a
    // registry that was really filled again.
    ForgetDefinition();
    // The descriptor may have been freed by the engine (DestructXmlDescriptors)
    // if it could not be removed from the ring right before Clear: our handles are
    // forgotten instead of risking a use after free. It is rebuilt at the next
    // injection (bounded leak: 1 descriptor per Clear).
    g_cat.descriptor = nullptr;
    g_cat.descriptorId = SwEngineString{};
    g_cat.descriptorXml = SwEngineString{};
    // The rings were just emptied: the clone must NEVER be reused as is (its
    // copied buffers come from a registry state that no longer exists). `epoch`
    // invalidates any reuse; the reinjection happens on the FIRST lookup of our id
    // (on-demand injection), not after polling.
    ++g_cat.epoch;
    g_cat.sourceUsed = nullptr;
    g_cat.lazyHits = 0;
    g_cat.lazyMisses = 0;
    g_cat.nativeResolved = 0;
    g_cat.pushFails = 0;
    g_cat.registered = false;
    g_cat.failed = false;                      // a past AV does not doom the session
    g_cat.attempts = 0;
    g_cat.validateFails = 0;
    g_cat.lastAttemptMs = 0;                   // allows an immediate attempt
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

// Code writes go EXCLUSIVELY through the loader's native API: VirtualQuery
// validation, PAGE_EXECUTE_READWRITE, restore of the original protection,
// FlushInstructionCache, and refusal if the prologue bytes do not match the
// validated contract. The mod no longer contains any direct write into the
// game's code.

// Inline hook of a `patchSize`-byte prologue (instruction boundary guaranteed by
// kFindLens / kClearLens). The trampoline belongs to the loader and serves as
// the original: no byte is copied or freed by the mod.
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
    if (count < cap) return RingGrowth::kNone;      // room left: nothing to do
    if (!hasGrowFn) return RingGrowth::kNone;       // no growth available
    // cap == 0: the ring was NEVER allocated (no existing buffer) -> allocating
    // invalidates nothing and stays safe even from a game frame. This is the ONLY
    // growth allowed: "ring growth allowed only to allocate a never-allocated ring
    // (cap==0)".
    if (cap == 0) return RingGrowth::kAllocate;
    // cap > 0: a real realloc would invalidate the buffer the engine may be
    // walking. Always refused, even outside a frame: the protection is kept as is.
    (void)allowGrow;
    return RingGrowth::kNone;
}

// ---------------------------------------------------------------------------
// PURE decisions of the on-demand injection (testable off-game).
//
// Step 1: what to do before any write? The only case that allows a write is
// `kInject`; `kPresent` is the de-duplication (no re-cloning, no replay of
// 8 x 0x2d08) and `kNotOurs` leaves the game's lookup strictly unchanged.
LazyResult JetCatalogueLazyPlan(bool isOurs, bool publishedInThisRegistry) {
    if (!isOurs) return LazyResult::kNotOurs;
    if (publishedInThisRegistry) return LazyResult::kPresent;
    return LazyResult::kInject;
}

// Step 2: outcome of an attempt. A missing source is NEVER mistaken for a
// success: it is the "the game asks for our id and we cannot provide it" case,
// which must return null and be logged (fail-closed).
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
        case LazyResult::kNotOurs: return "foreign name";
        case LazyResult::kPresent: return "already present";
        case LazyResult::kInject: return "injection needed";
        case LazyResult::kInjected: return "injected on demand";
        case LazyResult::kSourceMissing: return "source missing";
        case LazyResult::kRefused: return "injection refused";
    }
    return "unknown";
}

size_t JetCatalogueBuildJump(uint8_t* out, size_t patchSize, void* target) {
    if (!out || !target || patchSize < 14 || patchSize > 24) return 0;
    memset(out, 0x90, patchSize);
    out[0] = 0xFF; out[1] = 0x25;                  // jmp qword ptr [rip+disp32]
    *reinterpret_cast<int32_t*>(out + 2) = 0;      // disp32 = 0 (mandatory)
    *reinterpret_cast<uint64_t*>(out + 6) = reinterpret_cast<uint64_t>(target);
    return patchSize;
}

bool InitJetCatalogueInjection(const SwModHost* host, const char* xmlPath) {
    if (g_cat.findTarget) return true;
    if (!g_engine.native || !g_engine.host || !host) return false;
    // ON BY DEFAULT: only the catalogue hooks are installed.
    if (!JetCatalogueInjectionRequested()) {
        LogInfo("Jet: catalogue injection disabled (SW_JET_CATALOGUE=0); hooks not installed");
        return false;
    }
    const uint32_t build = g_engine.build;
    if (!build) {
        LogError("Jet: engine build not certified; catalogue injection cancelled");
        return false;
    }
    // Layout of the "definition" object SPECIFIC TO THE BUILD (0x2d08 / 0x2848).
    // The build identity comes from the loader's certified profile, never from an RVA.
    g_definitionBytes = static_cast<size_t>(JetCatalogueDefinitionBytes(static_cast<int>(build)));
    LogInfo("Jet: definition layout for build %u: %zu bytes per variant (last destroyed field +0x%llX)",
            build, g_definitionBytes,
            static_cast<unsigned long long>(JetCatalogueDestroyedTail(static_cast<int>(build))));
    // Generic engine services resolved by the loader's discovery profile.
    auto service = [host](const char* role) -> uint8_t* {
        return static_cast<uint8_t*>(
            const_cast<void*>(g_engine.native->findSymbol(host, role)));
    };
    uint8_t* findTarget = service("engine.component.lookup");
    uint8_t* clearTarget = service("engine.component.clear");
    uint8_t* locRegister = service("engine.localization.register");
    uint8_t* grow = service("engine.component.grow");
    if (!findTarget || !clearTarget || !locRegister || !grow) {
        LogError("Jet: catalogue services not resolved by the profile (lookup=%p clear=%p loc=%p grow=%p)"
                 "; no hook installed",
                 (void*)findTarget, (void*)clearTarget, (void*)locRegister, (void*)grow);
        return false;
    }
    if (!g_lockInit) { InitializeCriticalSection(&g_lock); g_lockInit = true; }
    g_cat.locRegister = reinterpret_cast<LocRegisterFn>(locRegister);
    g_cat.grow = reinterpret_cast<GrowFn>(grow);
    if (xmlPath && *xmlPath) g_cat.xmlPath = xmlPath;
    g_cat.injectDisabled = InjectionDisabledByMarker();
    if (g_cat.injectDisabled)
        LogWarn("Jet: .no_catalogue_injection marker present -> component NOT injected "
                "(diagnostic hooks kept); the catalogue stays 100%% vanilla");
    // Hook of the definition lookup.
    if (!InstallHook(findTarget, reinterpret_cast<void*>(&HookedFindDef), kFindPatch,
                     &g_cat.findHandle, reinterpret_cast<void**>(&g_cat.originalFind))) {
        LogError("Jet: FindComponentDefinitionByName hook refused by the native API");
        return false;
    }
    g_cat.findTarget = findTarget;
    // Hook of ComponentRegistry::Clear (cleanup before the engine destroys the rings).
    if (!InstallHook(clearTarget, reinterpret_cast<void*>(&HookedClear), kClearPatch,
                     &g_cat.clearHandle, reinterpret_cast<void**>(&g_cat.originalClear))) {
        g_engine.native->hookRestore(g_engine.host, g_cat.findHandle);
        g_cat.findHandle = 0;
        g_cat.findTarget = nullptr;
        LogError("Jet: ComponentRegistry::Clear hook refused; injection cancelled");
        return false;
    }
    g_cat.clearTarget = clearTarget;
    LogInfo("Jet: catalogue hooks installed (build %u): lookup + ComponentRegistry::Clear", build);
    return true;
}

// Proactive injection, called from the UI pulses (main menu) AND from the HUD /
// editor update (so IN GAME, where the registry exists). Ring growth is allowed
// here: we are not inside a frame where the engine walks the registry's rings.
void JetCatalogueTick() {
    if (g_cat.shuttingDown) return;            // PROCESS_DETACH: no more writes
    if (!g_cat.findTarget || g_cat.registered || g_cat.failed || g_cat.clearing) return;
    if (!g_engine.native) return;
    if (!g_cat.registry) {
        static bool once = false;
        if (!once) { once = true; LogInfo("Jet[tick] waiting for the registry (findTarget=1 api=1)"); }
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

// Teardown (DllMain / DLL_PROCESS_DETACH). IDEMPOTENT and harmless:
//   1. the injection is disarmed (NO more writes into the game's rings);
//   2. our entries are removed from the rings — BEFORE the hooks are given back
//      to the engine, so the Clear that follows cannot destroy our definitions;
//   3. the original bytes of both patched prologues are restored;
//   4. the state is forgotten. Game memory is NEVER freed (neither our strings
//      nor our clones): that is the price of a teardown that cannot fault.
// The trampolines are deliberately NOT freed: a game thread may still run them
// at PROCESS_DETACH time (the process dies right after; the leaks are bounded
// and harmless).
void ShutdownJetCatalogueInjection() {
    if (!g_cat.findTarget && !g_cat.clearTarget) return;   // already torn down
    LockAcquire();
    g_cat.shuttingDown = true;
    g_cat.clearing = true;
    const size_t removed = g_cat.registered ? SweepRegistry(g_cat.registry) : 0;
    if (removed)
        LogInfo("Jet: shutdown - %zu entr(ies) removed from the catalogue before restoring the hooks", removed);
    LockRelease();
    // Reverse installation order; sw_hook_restore is idempotent.
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
    LogInfo("Jet: catalogue hooks restored (teardown)");
}

// ---------------------------------------------------------------------------
// PURE API (testable off-game): clone geometry and string coherence.
// A variant stride smaller than the engine object's real size makes each variant
// spill onto the next: destroying variant k then frees fields of variant k+1 ->
// double free and a bogus pointer in _aligned_free (shutdown crash). That is
// exactly what these two functions test.
void JetCatalogueWriteCloneFields(uint8_t* dst, const CloneFieldValues& v) {
    if (!dst) return;
    const DefinitionFieldLayout& L = kDefinitionLayout;
    // string_t id: {ptr 8, len 4, cap 4}
    *reinterpret_cast<const char**>(dst + L.idPtr) = v.id;
    *reinterpret_cast<uint32_t*>(dst + L.idLen) = v.idLen;
    *reinterpret_cast<uint32_t*>(dst + L.idCap) = v.idCap;
    // string_t name: {ptr 8, len 4, cap 4}
    *reinterpret_cast<const char**>(dst + L.namePtr) = v.name;
    *reinterpret_cast<uint32_t*>(dst + L.nameLen) = v.nameLen;
    *reinterpret_cast<uint32_t*>(dst + L.nameCap) = v.nameCap;
    // category / value: int32 (the engine reads them with MOVSXD / MOV dword).
    *reinterpret_cast<int32_t*>(dst + L.category) = v.category;
    *reinterpret_cast<uint32_t*>(dst + L.value) = v.value;
    // flags: uint64. The engine writes this field with `mov [r15],rax` (8 bytes)
    // and reads it with `mov rax,[def+0x2b8]` (0x46ec30); a 32-bit store would
    // leave 0x2bc..0x2bf unchanged. So the 64-bit value is copied.
    *reinterpret_cast<uint64_t*>(dst + L.flags) = v.flags;
}

CloneGeometry JetCatalogueCloneGeometry(uint64_t variantBytes, uint32_t variants,
                                       uint64_t destroyedTail) {
    CloneGeometry g{};
    g.variantBytes = variantBytes;
    g.variants = variants;
    g.blockBytes = variantBytes * static_cast<uint64_t>(variants);
    // The engine destructor frees the field at `destroyedTail` (0x2D00 on
    // 1.15.23): if it does not fit in a variant, the destruction spills onto the
    // neighbouring variant -> double free / bogus pointer.
    g.overlapping = !FieldInsideVariant(variantBytes, destroyedTail, 8);
    return g;
}

// Size of a ComponentDefinition and offset of the last destroyed field, PER
// BUILD (pure, testable). The layout changed between 1.15.18 (0x2848) and
// 1.15.23 (0x2d08); hardcoding a single value caused the crash.
uint64_t JetCatalogueDefinitionBytes(int build) {
    return build == 1 ? kDefinitionBytesOld : kDefinitionBytesNew;
}

uint64_t JetCatalogueDestroyedTail(int build) {
    return build == 1 ? kOffDestroyedTailOld : kOffDestroyedTailNew;
}

// An engine string is {ptr, len, cap}. The game's destructor does, for an object
// at address `self`: if (len != 0) { if (ptr != 0) free(ptr) }.
// Coherent means: cap >= 32 (minimum engine capacity), len <= cap, and ptr
// non-null as soon as len > 0. A (len, cap) pair read in place of a pointer (the
// overlapping variants case) is therefore detected: it is 0x0000002000000000 for
// len=0 / cap=0x20.
bool JetCatalogueStringCoherent(uint64_t ptr, uint32_t len, uint32_t cap) {
    if (cap < 32) return false;
    if (len > cap) return false;
    if (len && !ptr) return false;
    return true;
}

}  // namespace jet
