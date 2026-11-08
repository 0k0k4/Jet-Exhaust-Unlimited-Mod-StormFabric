#include "jet_patch.h"
#include "mods_scan.h"
#include "log.h"
#include <windows.h>
#include <shlobj.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace swml {
namespace jet_internal {

// Signature d'opcodes du site de sur-régime (voir docs/jet_exhaust_unlimited.md) :
//   movss xmm0,[rdi+0x1bc]           ; RPS interne de la chambre
//   movss xmm6,[rip+rel32]           ; constante 200.0f (rel32 = joker, spécifique au build)
//   comiss xmm0,xmm6
//   jbe rel32                        ; saut « pas d'explosion » (rel32 = joker)
//   cmp byte [rdi+0x4d9],0           ; garde « déjà explosé »
// Une seule occurrence par exécutable reviewé : 0x35352a (v1.15.23), 0x35428a (v1.15.18).
constexpr uint8_t kSiteSig[] = {
    0xF3,0x0F,0x10,0x87,0xBC,0x01,0x00,0x00,   // movss xmm0,[rdi+0x1bc]
    0xF3,0x0F,0x10,0x35,0x00,0x00,0x00,0x00,  // movss xmm6,[rip+rel32]  (joker)
    0x0F,0x2F,0xC6,                            // comiss xmm0,xmm6
    0x0F,0x86,0x00,0x00,0x00,0x00,             // jbe rel32               (joker)
    0x44,0x38,0xBF,0xD9,0x04,0x00,0x00         // cmp byte [rdi+0x4d9],0
};
constexpr size_t kSiteSigSize = sizeof(kSiteSig);
constexpr size_t kMovssConstOperand = 12;   // offset du rel32 pointant la constante
constexpr size_t kJbeOperand = 20;          // offset du rel32 du jbe
constexpr size_t kPatchSize = 19;           // movss+movss+comiss : remplacés par CALL stub + NOPs
constexpr uint32_t kExpectedConstBits = 0x43480000u; // 200.0f

// Identité du composant « unlimited » : champ value (prix) de la définition.
// déf+0x2b4 (uint32), déf accessible via this+0x58. Zéro effet gameplay.
constexpr uint32_t kSignatureValue = 500123u;      // « value » exigé dans le XML du mod
constexpr uint32_t kDefOffsetValue = 0x2b4;
constexpr uint32_t kDefOffsetFromThis = 0x58;
// +inf : couvre aussi +inf et les NaN via COMISS (1e30f ne le garantissait pas).
constexpr float kHugeLimit = INFINITY;

namespace {

bool IsWildcardOffset(size_t off) {
    for (size_t j = kMovssConstOperand; j < kMovssConstOperand + 4; ++j) if (j == off) return true;
    for (size_t j = kJbeOperand; j < kJbeOperand + 4; ++j) if (j == off) return true;
    return false;
}

// PAGE_EXECUTE_READWRITE (jamais PAGE_READWRITE sur du code) + échec de
// restauration traité. Publication ordonnée par l'appelant.
bool WriteProtected(void* addr, const void* data, size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(addr, data, size);
    FlushInstructionCache(GetCurrentProcess(), addr, size);
    DWORD ignored = 0;
    if (!VirtualProtect(addr, size, old, &ignored)) {
        LogError("Jet: restauration de protection echouee a %p", addr);
        return false;
    }
    return true;
}

}  // namespace

// Comparaison en ignorant les offsets jokers (rel32 spécifiques au build).
bool MatchesSignature(const uint8_t* data, const uint8_t* expected, size_t size) {
    if (size > kSiteSigSize) return false;
    for (size_t j = 0; j < size; ++j) {
        if (IsWildcardOffset(j)) continue;
        if (data[j] != expected[j]) return false;
    }
    return true;
}

// Recherche de la signature. Retourne le nombre d'occurrences ; hitOffset reçoit
// la dernière position trouvée. L'appelant exige exactement une occurrence.
size_t FindSignature(const uint8_t* data, size_t size, size_t& hitOffset) {
    size_t count = 0;
    if (size < kSiteSigSize) return 0;
    for (size_t i = 0; i + kSiteSigSize <= size; ++i) {
        bool ok = true;
        for (size_t j = 0; j < kSiteSigSize; ++j) {
            if (!IsWildcardOffset(j) && data[i+j] != kSiteSig[j]) { ok = false; break; }
        }
        if (ok) { hitOffset = i; ++count; }
    }
    return count;
}

// Lit le rel32 du movss et la constante pointée. La validation sémantique
// (constBits == 200.0f) appartient à l'appelant, fail-closed.
bool ResolveConstant(const uint8_t* site, uint32_t& constBits) {
    int32_t rel = 0;
    memcpy(&rel, site + kMovssConstOperand, 4);
    memcpy(&constBits, site + 16 + rel, 4);  // rip après le movss (8 octets)
    return true;
}

// Encodage du stub (code 0x32 octets, constantes à 0x34/0x38 dans le même bloc :
// les RIP-relatifs sont donc fixes et testables hors jeu).
//   push rax ; push r10
//   mov rax,[rdi+0x58]                    ; définition
//   mov r10d, <signature value>
//   cmp dword [rax+0x2b4], r10d
//   movss xmm6,[rip+d1]                   ; limite quasi infinie
//   je +8                                 ; garde la limite illimitée
//   movss xmm6,[rip+d2]                   ; limite vanilla (copie de la constante)
//   movss xmm1,[rdi+0x1bc]
//   comiss xmm1,xmm6                      ; flags pour le jbe original (ret les préserve)
//   pop r10 ; pop rax ; ret
constexpr size_t kStubHugeOff = 0x3c;      // float limite illimitée
constexpr size_t kStubVanillaOff = 0x40;   // float limite originale
constexpr size_t kStubBlockBytes = 0x68;   // assez pour le stub de diagnostic
constexpr size_t kDiagVanilla = 0x64;   // float 200.0f du stub de diagnostic
static_assert(kDefOffsetValue < 0x10000, "encodage disp32 : offset sur 2 octets");

// Stub de DIAGNOSTIC : enregistre this/definition/RPS/value dans le bloc (à
// portée RIP-relative) puis reproduit EXACTEMENT l'original (limite 200.0f).
// Aucun changement de comportement : on peut l'installer sans risque.
constexpr size_t kDiagDef = 0x50, kDiagRps = 0x58, kDiagVal = 0x5c, kDiagCount = 0x60;
constexpr size_t kDiagBlockBytes = 0x68;

size_t BuildJetDiagStub(uint8_t* out, size_t blockBytes, float vanillaLimit) {
    if (!out || blockBytes < kDiagBlockBytes) return 0;
    auto* base = out;
    auto rip = [&](size_t off, size_t target) {
        int32_t d = (int32_t)((intptr_t)target - (intptr_t)(off + 4));
        return d;
    };
    size_t o = 0;
    out[o++] = 0x50;                                        // push rax
    out[o++] = 0x41; out[o++] = 0x52;                       // push r10
    out[o++] = 0x48; out[o++] = 0x8B; out[o++] = 0x47; out[o++] = 0x58;  // mov rax,[rdi+0x58]
    out[o++] = 0x48; out[o++] = 0x89; out[o++] = 0x05;      // mov [rip+kDiagDef], rax
    { int32_t d = rip(o + 4, kDiagDef); memcpy(out + o, &d, 4); } o += 4;
    out[o++] = 0x44; out[o++] = 0x8B; out[o++] = 0x97; out[o++] = 0xBC; out[o++] = 0x01;
    out[o++] = 0x00; out[o++] = 0x00;                       // mov r10d,[rdi+0x1bc] (RPS)
    out[o++] = 0x44; out[o++] = 0x89; out[o++] = 0x15;      // mov [rip+kDiagRps], r10d
    { int32_t d = rip(o + 4, kDiagRps); memcpy(out + o, &d, 4); } o += 4;
    out[o++] = 0x48; out[o++] = 0x85; out[o++] = 0xC0;      // test rax,rax
    out[o++] = 0x74; out[o++] = 0x0E;                       // jz +14 -> 47
    out[o++] = 0x44; out[o++] = 0x8B; out[o++] = 0x90;
    out[o++] = 0xB4; out[o++] = 0x02; out[o++] = 0x00; out[o++] = 0x00;  // mov r10d,[rax+0x2b4]
    out[o++] = 0x44; out[o++] = 0x89; out[o++] = 0x15;      // mov [rip+kDiagVal], r10d
    { int32_t d = rip(o + 4, kDiagVal); memcpy(out + o, &d, 4); } o += 4;
    assert(o == 47);
    out[o++] = 0xFF; out[o++] = 0x05;                        // inc dword [rip+kDiagCount]
    { int32_t d = rip(o + 4, kDiagCount); memcpy(out + o, &d, 4); } o += 4;
    out[o++] = 0xF3; out[o++] = 0x0F; out[o++] = 0x10; out[o++] = 0x35;  // movss xmm6,[rip+200]
    { int32_t d = rip(o + 4, kDiagVanilla); memcpy(out + o, &d, 4); } o += 4;
    out[o++] = 0xF3; out[o++] = 0x0F; out[o++] = 0x10;
    out[o++] = 0x87; out[o++] = 0xBC; out[o++] = 0x01; out[o++] = 0x00; out[o++] = 0x00;
    out[o++] = 0x0F; out[o++] = 0x2F; out[o++] = 0xC6;       // comiss xmm0,xmm6
    out[o++] = 0x41; out[o++] = 0x5A;                        // pop r10
    out[o++] = 0x58;                                         // pop rax
    out[o++] = 0xC3;                                         // ret
    memcpy(out + kDiagVanilla, &vanillaLimit, 4);
    return kDiagBlockBytes;
}

size_t BuildJetStub(uint8_t* out, uint32_t signature, float hugeLimit, float vanillaLimit) {
    size_t o = 0;
    out[o++] = 0x50;                                          // push rax
    out[o++] = 0x41; out[o++] = 0x52;                         // push r10
    out[o++] = 0x48; out[o++] = 0x8B; out[o++] = 0x47; out[o++] = 0x58;  // mov rax,[rdi+0x58]
    // Garde-fou : définition pas encore affectée => limite vanilla (jamais de
    // déréférencement nul dans le stub, qui n'a pas de SEH).
    out[o++] = 0x48; out[o++] = 0x85; out[o++] = 0xC0;        // test rax,rax
    out[o++] = 0x74; out[o++] = 0x17;                         // jz .vanilla (offset 35)
    out[o++] = 0x41; out[o++] = 0xBA;                         // mov r10d, imm32
    memcpy(out + o, &signature, 4); o += 4;
    out[o++] = 0x44; out[o++] = 0x39; out[o++] = 0x90;        // cmp dword [rax+disp32], r10d
    out[o++] = kDefOffsetValue & 0xFF; out[o++] = (kDefOffsetValue >> 8) & 0xFF;
    out[o++] = 0; out[o++] = 0;
    out[o++] = 0xF3; out[o++] = 0x0F; out[o++] = 0x10; out[o++] = 0x35;   // movss xmm6,[rip+d1]
    int32_t d1 = (int32_t)(kStubHugeOff - (o + 4));
    memcpy(out + o, &d1, 4); o += 4;
    out[o++] = 0x74; out[o++] = 0x08;                         // je +8
    out[o++] = 0xF3; out[o++] = 0x0F; out[o++] = 0x10; out[o++] = 0x35;   // movss xmm6,[rip+d2]
    int32_t d2 = (int32_t)(kStubVanillaOff - (o + 4));
    memcpy(out + o, &d2, 4); o += 4;
    // IMPORTANT : le code d'origine charge le RPS dans XMM0 et le jeu relit XMM0
    // ensuite ; on doit donc écrire XMM0 (et non XMM1).
    out[o++] = 0xF3; out[o++] = 0x0F; out[o++] = 0x10;       // movss xmm0,[rdi+0x1bc]
    out[o++] = 0x87; out[o++] = 0xBC; out[o++] = 0x01; out[o++] = 0x00; out[o++] = 0x00;
    out[o++] = 0x0F; out[o++] = 0x2F; out[o++] = 0xC6;        // comiss xmm0,xmm6
    out[o++] = 0x41; out[o++] = 0x5A;                         // pop r10
    out[o++] = 0x58;                                          // pop rax
    out[o++] = 0xC3;                                          // ret
    memcpy(out + kStubHugeOff, &hugeLimit, 4);
    memcpy(out + kStubVanillaOff, &vanillaLimit, 4);
    return o;
}

// Patch transactionnel : vérifie les octets originaux, sauvegarde, écrit
// CALL rel32 vers le stub + NOPs. Le jbe original (6 octets) reste en place.
bool InstallPatch(uint8_t* site, const uint8_t* stub, const uint8_t* expectedOriginal,
                  uint8_t* savedOriginal, size_t patchSize) {
    if (patchSize != kPatchSize) return false;
    if (!MatchesSignature(site, expectedOriginal, patchSize)) return false;
    uint8_t patch[kPatchSize];
    patch[0] = 0xE8;
    int32_t call = (int32_t)(reinterpret_cast<uintptr_t>(stub) -
                             reinterpret_cast<uintptr_t>(site + 5));
    memcpy(patch + 1, &call, 4);
    memset(patch + 5, 0x90, patchSize - 5);
    memcpy(savedOriginal, site, patchSize);
    return WriteProtected(site, patch, patchSize);
}

bool RevertPatch(uint8_t* site, const uint8_t* savedOriginal, size_t patchSize) {
    if (patchSize != kPatchSize) return false;
    return WriteProtected(site, savedOriginal, patchSize);
}

// Aligne le site sur une page, puis essaie des candidats en descendant, puis en
// montant, tous dans la portée d'un CALL rel32 (±2 Go).
uint8_t* AllocateNear(uint8_t* site) {
    const uintptr_t granularity = 0x10000ull;
    uintptr_t base = reinterpret_cast<uintptr_t>(site) & ~(granularity - 1);
    for (int step = 1; step <= 64; ++step) {
        for (int sign : {-1, 1}) {
            uintptr_t candidate = base + sign * (uintptr_t)step * 0x400000ull; // 4 Mo
            if (candidate < granularity) continue;
            void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), kStubBlockBytes,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return static_cast<uint8_t*>(p);
        }
    }
    return nullptr;
}

// ---- Définitions injectées -----------------------------------------------------------

bool SafeDefinitionName(const std::string& name) {
    if (name.size() < 6 || name.size() > 128) return false;
    const std::string lower = LowerAscii(name);
    const bool isXml = lower.rfind(".xml") == name.size() - 4;
    const bool isBin = lower.rfind(".bin") == name.size() - 4;
    if (!isXml && !isBin) return false;
    for (char c : name) {
        if ((unsigned char)c < 0x20 || c == 0x7F) return false;
        if (c == '/' || c == '\\' || c == ':' || c == '"' || c == '<' ||
            c == '>' || c == '|' || c == '?' || c == '*') return false;
    }
    if (name.find("..") != std::string::npos) return false;
    return true;
}

using WantedList = std::vector<std::pair<std::string, std::wstring>>; // (fichier, chemin source)

// Rassemble les demandes des mods actifs pour un champ donné, noms assainis et dédupliqués.
void CollectWanted(const std::vector<ModEntry>& mods, const std::vector<std::string> ModEntry::*field,
                   const char* what, WantedList& out) {
    for (const auto& m : mods) {
        if (!m.enabled) continue;
        for (const auto& file : m.*field) {
            if (!SafeDefinitionName(file)) {
                LogWarn("Jet: nom de %s refuse pour le mod '%s' : '%s'", what, m.id.c_str(), file.c_str());
                continue;
            }
            bool dup = false;
            for (const auto& w : out) if (w.first == file) dup = true;
            if (!dup) out.push_back({file, m.dirPath + L"\\" + Utf8ToWide(file)});
        }
    }
}

// Ramène un dossier cible (définitions XML ou composants compilés .bin) à l'état
// demandé : copie/rafraîchit les fichiers réclamés (+ marqueur de propriété),
// retire ceux qui ne le sont plus.
bool ReconcileFolder(const WantedList& wanted, const std::wstring& targetDir,
                     std::vector<std::string>& injected) {
    injected.clear();
    // Le dossier cible peut ne pas exister (rom/data/components absent du jeu de base).
    if (!wanted.empty()) CreateDirectoryW(targetDir.c_str(), nullptr);

    // 1. Retire les marqueurs dont le fichier n'est plus réclamé.
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((targetDir + L"\\*.swml").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring marker = fd.cFileName;               // "<fichier>.swml"
            std::string owned = WideToUtf8(marker.substr(0, marker.size() - 5));
            bool stillWanted = false;
            for (const auto& w : wanted) if (LowerAscii(w.first) == LowerAscii(owned)) stillWanted = true;
            if (!stillWanted) {
                DeleteFileW((targetDir + L"\\" + marker).c_str());
                DeleteFileW((targetDir + L"\\" + Utf8ToWide(owned)).c_str());
                LogInfo("Jet: fichier injecte retire (plus demande) : %s", owned.c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    // 2. Copie/rafraîchit les fichiers demandés + marqueur de propriété.
    for (const auto& w : wanted) {
        if (!CopyFileW(w.second.c_str(), (targetDir + L"\\" + Utf8ToWide(w.first)).c_str(), FALSE)) {
            LogError("Jet: copie impossible : %s", w.first.c_str());
            injected.clear();
            return false;
        }
        HANDLE f = CreateFileW((targetDir + L"\\" + Utf8ToWide(w.first) + L".swml").c_str(),
                               GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const char* text = "stormfabric injected asset";
            DWORD written = 0;
            WriteFile(f, text, (DWORD)strlen(text), &written, nullptr);
            CloseHandle(f);
        }
        injected.push_back(w.first);
        LogInfo("Jet: fichier materialise : %s", w.first.c_str());
    }
    return true;
}

bool ReconcileDefinitions(const std::vector<ModEntry>& mods, const std::wstring& defsDir,
                          std::vector<std::string>& injected) {
    WantedList wanted;
    CollectWanted(mods, &ModEntry::injectDefinitions, "definition", wanted);
    return ReconcileFolder(wanted, defsDir, injected);
}

bool ReconcileComponents(const std::vector<ModEntry>& mods, const std::wstring& componentsDir,
                         std::vector<std::string>& injected) {
    WantedList wanted;
    CollectWanted(mods, &ModEntry::injectComponents, "composant", wanted);
    return ReconcileFolder(wanted, componentsDir, injected);
}

// ---- Mod local Stormworks (%APPDATA%\Stormworks\data\mods\<id>) ------------------
// Le catalogue d'inventaire du jeu est construit à partir de la liste bakée dans
// l'exécutable et des composants COMPILÉS des mods locaux. Un XML ajouté à
// rom/data/definitions n'y entre pas (entrée placeholder). On installe donc un
// dossier de mod local officiel : mod.xml + data/definitions + data/components.

std::wstring UserStormworksModsDir() {
    wchar_t path[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path))) return L"";
    return std::wstring(path) + L"\\Stormworks\\data\\mods";
}

bool WriteTextFile(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, text.data(), (DWORD)text.size(), &written, nullptr);
    CloseHandle(f);
    return ok != 0;
}

void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*" ).c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring child = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(child);
            else DeleteFileW(child.c_str());
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

std::string XmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

bool ReconcileModAssets(const std::vector<ModEntry>& mods, const std::wstring& modsRoot,
                        std::vector<std::string>& injected) {
    injected.clear();
    if (modsRoot.empty()) return false;

    // 1. Mods actifs déclarant des assets à injecter.
    std::vector<const ModEntry*> active;
    for (const auto& m : mods) {
        if (!m.enabled || m.id.empty()) continue;
        if (m.injectDefinitions.empty() && m.injectComponents.empty()) continue;
        active.push_back(&m);
    }

    // 2. Retire les dossiers de mods locaux créés par nous et plus demandés.
    std::vector<std::wstring> activeIds;
    for (const ModEntry* m : active) activeIds.push_back(Utf8ToWide(LowerAscii(m->id)));
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((modsRoot + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring modDir = modsRoot + L"\\" + fd.cFileName;
            if (GetFileAttributesW((modDir + L"\\.stormfabric").c_str()) == INVALID_FILE_ATTRIBUTES)
                continue; // jamais touché : ce n'est pas notre dossier
            bool wanted = false;
            for (const std::wstring& id : activeIds) if (_wcsicmp(id.c_str(), fd.cFileName) == 0) wanted = true;
            if (!wanted) {
                DeleteTree(modDir);
                LogInfo("Jet: mod local retire (plus demande) : %ls", fd.cFileName);
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    // 3. Installe/rafraîchit chaque mod actif (mod.xml + assets).
    for (const ModEntry* m : active) {
        std::wstring modDir = modsRoot + L"\\" + Utf8ToWide(LowerAscii(m->id));
        std::wstring defsDir = modDir + L"\\data\\definitions";
        std::wstring compsDir = modDir + L"\\data\\components";
        CreateDirectoryW(modsRoot.c_str(), nullptr);
        CreateDirectoryW(modDir.c_str(), nullptr);
        CreateDirectoryW((modDir + L"\\data").c_str(), nullptr);
        CreateDirectoryW(defsDir.c_str(), nullptr);
        CreateDirectoryW(compsDir.c_str(), nullptr);
        std::string manifest = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<mod name=\"" +
            XmlEscape(m->title.empty() ? m->id : m->title) + "\" author=\"" +
            XmlEscape(m->author) + "\" desc=\"" + XmlEscape(m->description) + "\"/>\n";
        WriteTextFile(modDir + L"\\mod.xml", manifest);
        WriteTextFile(modDir + L"\\.stormfabric", "managed by StormFabric; do not edit\n");

        std::vector<std::string> defs, comps;
        WantedList defList, compList;
        std::vector<ModEntry> single{*m};
        CollectWanted(single, &ModEntry::injectDefinitions, "definition", defList);
        CollectWanted(single, &ModEntry::injectComponents, "composant", compList);
        if (!ReconcileFolder(defList, defsDir, defs)) return false;
        if (!ReconcileFolder(compList, compsDir, comps)) return false;
        for (const auto& d : defs) injected.push_back(d);
        for (const auto& c : comps) injected.push_back(c);
        LogInfo("Jet: mod local installe : %ls (%zu definition(s), %zu composant(s))",
                modDir.c_str(), defs.size(), comps.size());
    }
    return true;
}

}  // namespace jet_internal

namespace {

std::wstring GameRoot() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring p(path);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : p.substr(0, slash);
}

struct PatchState {
    uint8_t* site = nullptr;
    uint8_t* stub = nullptr;
    uint8_t original[jet_internal::kPatchSize] = {};
    bool installed = false;
    bool diag = false;
};
PatchState g_jet;

}  // namespace

// Le patch de survitesse est DÉSACTIVÉ : il modifiait le comportement de TOUS les
// réacteurs (vanilla compris) en jeu, alors que le modèle exécuté hors jeu se
// comporte correctement. Tant que la cause n'est pas établie, on ne patche pas le
// code du jeu : le composant reste injecté au catalogue, sans contournement.
constexpr bool kOverrevPatchEnabled = true;

bool InitJetExhaustUnlimited() {
    if (g_jet.installed) return true;   // idempotent
    constexpr bool kDiagMode = !kOverrevPatchEnabled;
    std::vector<ModEntry> mods = ScanMods();
    // Le composant est injecté au catalogue en mémoire (voir jet_catalogue) :
    // aucun fichier n'est écrit hors de mods\. Ce module ne fait plus qu'armer
    // le contournement de sur-régime quand un mod actif déclare un composant.
    bool wanted = false;
    for (const auto& m : mods) {
        if (m.enabled && !m.injectComponents.empty()) wanted = true;
    }
    if (!wanted) {
        LogInfo("Jet: aucun composant declare par les mods actifs ; aucun patch");
        return false;
    }

    // Localise la signature dans les sections exécutables de l'image chargée.
    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);
    size_t hits = 0;
    size_t siteOffset = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        if (section[i].Characteristics & IMAGE_SCN_MEM_DISCARDABLE) continue;
        size_t offset = 0;
        size_t count = jet_internal::FindSignature(base + section[i].VirtualAddress,
                                                   section[i].Misc.VirtualSize, offset);
        if (count) { hits += count; siteOffset = section[i].VirtualAddress + offset; }
    }
    if (hits != 1) {
        LogError("Jet: signature du site de sur-regime %s (%zu occurrences) ; aucun patch",
                 hits == 0 ? "absente" : "ambigue", hits);
        return false;
    }

    // Résout la constante pointée par movss et exige 200.0f : refuse un site
    // dont la sémantique aurait changé.
    uint8_t* site = base + siteOffset;
    uint32_t constBits = 0;
    jet_internal::ResolveConstant(site, constBits);
    if (constBits != jet_internal::kExpectedConstBits) {
        LogError("Jet: constante du site inattendue (0x%08X) ; aucun patch", constBits);
        return false;
    }

    uint8_t* stub = jet_internal::AllocateNear(site);
    if (!stub) {
        LogError("Jet: allocation du trampoline a ±2Go impossible ; aucun patch");
        return false;
    }
    uint8_t code[jet_internal::kStubBlockBytes] = {};
    if (kDiagMode) {
        if (jet_internal::BuildJetDiagStub(code, sizeof(code),
                                           *reinterpret_cast<const float*>(&constBits)) == 0) {
            VirtualFree(stub, 0, MEM_RELEASE);
            LogError("Jet: encodage du stub de diagnostic refuse");
            return false;
        }
    } else {
        jet_internal::BuildJetStub(code, jet_internal::kSignatureValue, jet_internal::kHugeLimit,
                                   *reinterpret_cast<const float*>(&constBits));
    }
    memcpy(stub, code, jet_internal::kStubBlockBytes);
    g_jet.diag = kDiagMode;

    if (!jet_internal::InstallPatch(site, stub, jet_internal::kSiteSig,
                                    g_jet.original, jet_internal::kPatchSize)) {
        LogError("Jet: ecriture du patch impossible (site modifie ?) ; aucun hook installe");
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    g_jet.site = site;
    g_jet.stub = stub;
    g_jet.installed = true;
    char siteTxt[32], stubTxt[32];
    snprintf(siteTxt, sizeof(siteTxt), "%p", (void*)site);
    snprintf(stubTxt, sizeof(stubTxt), "%p", (void*)stub);
    LogInfo("Jet: patch sur-regime installe en mode %s (site %s, stub %s)",
            g_jet.diag ? "DIAGNOSTIC PASSIF" : "BYPASS", siteTxt, stubTxt);
    return true;
}

void JetDiagTick() {
    static bool logged = false;
    if (!g_jet.installed || !g_jet.diag || logged || !g_jet.stub) return;
    auto* b = g_jet.stub;
    uint32_t count = 0;
    memcpy(&count, b + jet_internal::kDiagCount, 4);
    if (!count) return;
    logged = true;
    uint64_t def = 0; uint32_t rps = 0, value = 0;
    memcpy(&def, b + jet_internal::kDiagDef, 8);
    memcpy(&rps, b + jet_internal::kDiagRps, 4);
    memcpy(&value, b + jet_internal::kDiagVal, 4);
    float rpsF = 0; memcpy(&rpsF, &rps, 4);
    LogInfo("[DIAG] chambre: appels=%u def=%p value(+0x2b4)=%u rps(+0x1bc)=%.1f",
            count, (void*)def, value, rpsF);
}

void ShutdownJetExhaustUnlimited() {
    if (!g_jet.installed) return;
    // B2 : ne liberer le stub que si les octets d'origine sont reellement restaures.
    if (jet_internal::RevertPatch(g_jet.site, g_jet.original, jet_internal::kPatchSize)) {
        LogInfo("Jet: patch original restaure");
        if (g_jet.stub) VirtualFree(g_jet.stub, 0, MEM_RELEASE);
        g_jet = PatchState{};
    } else {
        LogError("Jet: restauration impossible ; stub conserve (evite un saut dans le vide)");
        g_jet.installed = false;
    }
}

}  // namespace swml
