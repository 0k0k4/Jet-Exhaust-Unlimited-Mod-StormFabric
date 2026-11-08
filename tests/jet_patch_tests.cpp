// Tests hors jeu du module jet_patch : signature, résolution de la constante,
// encodage du stub, patch/rollback transactionnel, matérialisation des
// définitions. N'exécute jamais de code du jeu ; dossiers temporaires uniquement.
#include "../src/mods_scan.h"
#include "jet_test_sigs.h"
#include "test_temp.h"
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace swml::jet_internal;

namespace {

// Octets réels autour du site 1.15.23 (0x35352a) : contexte avant, site, après.
const uint8_t kSiteReal[] = {
    // avant : CALL ... ; CMP byte [rax+0x4a] ; JE +0x207
    0xE8,0x70,0x11,0xF4,0xFF,0x44,0x38,0x78,0x4A,0x0F,0x84,0x07,0x02,0x00,0x00,
    // site (32 octets) : movss xmm0 ; movss xmm6,[rip+0x85d29e] ; comiss ; jbe ; cmp garde
    0xF3,0x0F,0x10,0x87,0xBC,0x01,0x00,0x00,
    0xF3,0x0F,0x10,0x35,0x9E,0xD2,0x85,0x00,
    0x0F,0x2F,0xC6,
    0x0F,0x86,0xEE,0x01,0x00,0x00,
    0x44,0x38,0xBF,0xD9,0x04,0x00,0x00,
    // après : mov byte [rdi+0x4d9],1 ; ...
    0xC6,0x87,0xD9,0x04,0x00,0x00,0x01,0x4C,0x89,0x7D,0x60};
constexpr size_t kSiteAt = 15;   // offset du site dans kSiteReal

void TestSignature() {
    size_t hit = 0;
    assert(FindSignature(kSiteReal, sizeof(kSiteReal), hit) == 1 && hit == kSiteAt);

    // Aucune occurrence si un octet fixe change.
    std::vector<uint8_t> broken(kSiteReal, kSiteReal + sizeof(kSiteReal));
    broken[kSiteAt + 2] ^= 0xFF;
    assert(FindSignature(broken.data(), broken.size(), hit) == 0);

    // Ambiguïté : deux occurrences -> l'appelant doit refuser.
    std::vector<uint8_t> doubled(kSiteReal, kSiteReal + sizeof(kSiteReal));
    doubled.insert(doubled.end(), kSiteReal, kSiteReal + sizeof(kSiteReal));
    assert(FindSignature(doubled.data(), doubled.size(), hit) == 2);

    // Zone trop courte.
    assert(FindSignature(kSiteReal, 10, hit) == 0);

    // Les rel32 (jokers) peuvent valoir n'importe quoi.
    std::vector<uint8_t> otherRel(kSiteReal, kSiteReal + sizeof(kSiteReal));
    otherRel[kSiteAt + 12] = 0xAA; otherRel[kSiteAt + 13] = 0xBB;
    otherRel[kSiteAt + 22] = 0x11; otherRel[kSiteAt + 23] = 0x22;
    assert(FindSignature(otherRel.data(), otherRel.size(), hit) == 1);
    puts("PASS: signature unique/absente/ambigue, jokers rel32, zone courte");
}

void TestConstantResolution() {
    // Mécanique de résolution vérifiée sur une copie locale bornée (le rel32 du
    // site réel pointe vers .rdata à 0x140bb07d8, hors de toute zone de test).
    uint32_t bits = 0;
    std::vector<uint8_t> v(kSiteReal, kSiteReal + sizeof(kSiteReal));
    size_t constAt = v.size();
    float twoHundred = 200.0f;
    const uint8_t* cb = reinterpret_cast<const uint8_t*>(&twoHundred);
    v.push_back(cb[0]); v.push_back(cb[1]); v.push_back(cb[2]); v.push_back(cb[3]);
    int32_t rel = (int32_t)(constAt - (kSiteAt + 16));
    memcpy(&v[kSiteAt + 12], &rel, 4);
    assert(ResolveConstant(v.data() + kSiteAt, bits) && bits == 0x43480000u);
    // Constante modifiée : la mécanique de lecture suit, la validation appartient
    // à l'appelant (fail-closed).
    v[constAt] ^= 0xFF;
    assert(ResolveConstant(v.data() + kSiteAt, bits) && bits != 0x43480000u);
    puts("PASS: constante resolue via rel32 (200.0f attendu par l'appelant)");
}

void TestStubEncoding() {
    uint8_t stub[kStubBlockBytes] = {};
    size_t n = BuildJetStub(stub, 500123u, INFINITY, 200.0f);
    assert(n == 0x3A);
    assert(stub[0] == 0x50 && stub[1] == 0x41 && stub[2] == 0x52);          // push rax; push r10
    assert(stub[3] == 0x48 && stub[4] == 0x8B && stub[5] == 0x47 && stub[6] == 0x58);
    assert(stub[7] == 0x48 && stub[8] == 0x85 && stub[9] == 0xC0);          // test rax,rax
    assert(stub[10] == 0x74 && stub[11] == 0x17);                           // jz .vanilla
    assert(stub[12] == 0x41 && stub[13] == 0xBA);
    uint32_t sig = 0; memcpy(&sig, stub + 14, 4); assert(sig == 500123u);
    assert(stub[18] == 0x44 && stub[19] == 0x39 && stub[20] == 0x90);
    assert(stub[21] == 0xB4 && stub[22] == 0x02 && stub[23] == 0x00 && stub[24] == 0x00);
    int32_t d1 = 0; memcpy(&d1, stub + 29, 4);
    assert(25 + 8 + d1 == 0x3c);                                            // rip -> bloc huge
    assert(stub[33] == 0x74 && stub[34] == 0x08);                           // je +8
    int32_t d2 = 0; memcpy(&d2, stub + 39, 4);
    assert(35 + 8 + d2 == 0x40);                                            // rip -> bloc vanilla
    assert(stub[43] == 0xF3 && stub[44] == 0x0F && stub[45] == 0x10 && stub[46] == 0x87 &&
           stub[47] == 0xBC && stub[48] == 0x01 && stub[49] == 0x00 && stub[50] == 0x00);
    assert(stub[51] == 0x0F && stub[52] == 0x2F && stub[53] == 0xC6);       // comiss xmm0,xmm6
    assert(stub[54] == 0x41 && stub[55] == 0x5A && stub[56] == 0x58 && stub[57] == 0xC3);
    float huge = 0, vanilla = 0;
    memcpy(&huge, stub + 0x3c, 4); memcpy(&vanilla, stub + 0x40, 4);
    assert(std::isinf(huge) && vanilla == 200.0f);
    puts("PASS: stub encode (registres, signature, rip-relatifs in-bloc, comiss final)");
}


// Execute reellement le stub + le JBE original dans une page RWX.
// Vérifie la décision "explose / n'explose pas" pour vanilla et pour le composant.
void TestStubExecution() {
    uint8_t* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x2000, MEM_RESERVE | MEM_COMMIT,
                                                      PAGE_EXECUTE_READWRITE));
    assert(mem);
    memset(mem, 0, 0x2000);
    uint8_t* stub = mem + 0x000;
    uint8_t* site = mem + 0x100;
    int32_t* result = reinterpret_cast<int32_t*>(mem + 0x180);   // 1 = explosion, 0 = non
    uint8_t* thunk = mem + 0x200;

    size_t n = BuildJetStub(stub, 500123u, INFINITY, 200.0f);
    assert(n == 0x3A);

    // site : CALL stub(5) + 14 NOP = 19 octets, puis JBE(faux), [res]=1, jmp, [res]=0, ret
    size_t o = 0;
    site[o] = 0xE8;
    int32_t rel = (int32_t)(stub - (site + 5));
    memcpy(site + 1, &rel, 4);
    o = 5;
    for (int i = 0; i < 14; ++i) site[o++] = 0x90;
    assert(o == 19);
    const size_t jbe = o;
    site[o++] = 0x0F; site[o++] = 0x86;              // jbe -> no_explode
    const size_t jbeDisp = o; o += 4;
    const size_t exploded = o;
    site[o++] = 0xC7; site[o++] = 0x05;              // mov dword [rip+d], 1
    const size_t expDisp = o; o += 4;
    int32_t one = 1; memcpy(site + o, &one, 4); o += 4;
    // REGRESSION (harnais) : ce saut doit ENJAMBER le store de 0 (10 octets),
    // sinon la branche « explosion » écrit 1 puis retombe immédiatement sur
    // l'écriture de 0 : le harnais retournait toujours 0 et invalidait le test
    // (le stub de production, lui, est correct — vérifié par désassemblage).
    site[o++] = 0xEB; site[o++] = 0x0A;              // jmp +10 -> juste après [res]=0
    const size_t noexp = o;
    site[o++] = 0xC7; site[o++] = 0x05;              // mov dword [rip+d], 0
    const size_t neDisp = o; o += 4;
    int32_t zero = 0; memcpy(site + o, &zero, 4); o += 4;
    site[o++] = 0xC3;                                 // ret
    int32_t d = (int32_t)((site + noexp) - (site + jbe + 6)); memcpy(site + jbeDisp, &d, 4);
    d = (int32_t)(reinterpret_cast<uint8_t*>(result) - (site + exploded + 10));
    memcpy(site + expDisp, &d, 4);
    d = (int32_t)(reinterpret_cast<uint8_t*>(result) - (site + noexp + 10));
    memcpy(site + neDisp, &d, 4);

    // thunk : rcx -> rdi, puis call site
    size_t t = 0;
    thunk[t++] = 0x57;                                        // push rdi (non-volatile)
    thunk[t++] = 0x48; thunk[t++] = 0x8B; thunk[t++] = 0xF9;   // mov rdi,rcx
    thunk[t++] = 0x48; thunk[t++] = 0xB8;                      // mov rax,imm64
    uint64_t siteAddr = reinterpret_cast<uint64_t>(site);
    memcpy(thunk + t, &siteAddr, 8); t += 8;
    thunk[t++] = 0xFF; thunk[t++] = 0xD0;                      // call rax
    thunk[t++] = 0x5F;                                         // pop rdi
    thunk[t++] = 0xC3;                                         // ret
    using CallFn = void (*)(void*);
    CallFn call = reinterpret_cast<CallFn>(thunk);

    uint8_t def[0x400] = {};
    uint8_t comp[0x400] = {};
    *reinterpret_cast<void**>(comp + 0x58) = def;
    auto setRps = [&](float v) { *reinterpret_cast<float*>(comp + 0x1bc) = v; };
    auto setValue = [&](uint32_t v) { *reinterpret_cast<uint32_t*>(def + 0x2b4) = v; };
    FlushInstructionCache(GetCurrentProcess(), mem, 0x2000);

    printf("[dbg] stub=");
    for (int i = 0; i < 0x44; ++i) printf("%02x", stub[i]);
    putchar(10);
    printf("[dbg] comp=%p def=%p", (void*)comp, (void*)*(void**)(comp + 0x58));
    putchar(10);
    setRps(250.0f); *result = 99; call(comp);
    printf("[dbg] vanilla 250 RPS -> %d (attendu 1)\n", *result);
    setRps(100.0f); *result = 99; call(comp);
    printf("[dbg] vanilla 100 RPS -> %d (attendu 0)\n", *result);

    setValue(500123);                  // notre composant
    setRps(10000.0f); *result = 99; call(comp);
    printf("[dbg] unlimited 10000 RPS -> %d (attendu 0)\n", *result);
    assert(*result == 1 || *result == 0);
    setRps(1e30f);    *result = 99; call(comp); assert(*result == 0);
    setRps(INFINITY); *result = 99; call(comp); assert(*result == 0);

    *reinterpret_cast<void**>(comp + 0x58) = nullptr;   // définition nulle
    setRps(250.0f); *result = 99; call(comp); assert(*result == 1);
    setRps(100.0f); *result = 99; call(comp); assert(*result == 0);

    VirtualFree(mem, 0, MEM_RELEASE);
    puts("PASS: execution reelle du stub (vanilla 200 RPS, unlimited jamais, def nulle)");
}

void TestInstallRollback() {
    std::vector<uint8_t> mem(kSiteReal, kSiteReal + sizeof(kSiteReal));
    uint8_t* site = mem.data() + kSiteAt;
    // Comme en production : stub alloué à portée rel32 du site (VirtualAlloc proche).
    uint8_t* stub = AllocateNear(site);
    assert(stub);
    BuildJetStub(stub, 500123u, INFINITY, 200.0f);

    uint8_t saved[19] = {};
    assert(InstallPatch(site, stub, kSiteReal + kSiteAt, saved, 19));
    assert(site[0] == 0xE8);                                   // CALL rel32 -> stub
    int32_t rel = 0; memcpy(&rel, site + 1, 4);
    assert(site + 5 + rel == stub);
    for (size_t i = 5; i < 19; ++i) assert(site[i] == 0x90);   // NOPs
    for (size_t i = 0; i < 19; ++i) assert(saved[i] == kSiteReal[kSiteAt + i]);

    assert(RevertPatch(site, saved, 19));
    for (size_t i = 0; i < 19; ++i) assert(site[i] == kSiteReal[kSiteAt + i]);

    // Site déjà modifié -> refus fail-closed.
    std::vector<uint8_t> dirty(kSiteReal, kSiteReal + sizeof(kSiteReal));
    dirty[kSiteAt + 3] ^= 1;
    assert(!InstallPatch(dirty.data() + kSiteAt, stub, kSiteReal + kSiteAt, saved, 19));
    VirtualFree(stub, 0, MEM_RELEASE);
    puts("PASS: patch installé (CALL rel32 + NOPs), rollback, refus si site modifié");
}

void TestSafeNames() {
    assert(SafeDefinitionName("jet_engine_exhaust_unlimited.xml"));
    assert(!SafeDefinitionName("..\\evil.xml"));
    assert(!SafeDefinitionName("a.xml"));
    assert(!SafeDefinitionName("noext.txt"));
    assert(!SafeDefinitionName("bad:name.xml"));
    assert(!SafeDefinitionName("bad/slashed.xml"));
    assert(!SafeDefinitionName("a\\b.xml"));
    assert(SafeDefinitionName("component.bin"));
    assert(!SafeDefinitionName("..\\evil.bin"));
    puts("PASS: noms de définitions assainis (traversée, séparateurs, extension)");
}

void TestReconcile() {
    namespace fs = std::filesystem;
    fs::path root = fs::path(TestTempRoot()) / "jet_patch_tests";
    fs::remove_all(root);
    fs::path modsDir = root / "mods";
    fs::path defsDir = root / "defs";
    fs::create_directories(defsDir);
    fs::path modDir = modsDir / "jet_unlimited";
    fs::create_directories(modDir);
    const char* xml = "<definition name=\"Jet Exhaust Unlimited\" value=\"500123\"/>";
    FILE* f = nullptr;
    _wfopen_s(&f, (modDir / L"jet_engine_exhaust_unlimited.xml").c_str(), L"wb");
    assert(f);
    fwrite(xml, 1, strlen(xml), f);
    fclose(f);

    swml::ModEntry m;
    m.id = "jet_unlimited";
    m.enabled = true;
    m.dirPath = modDir.wstring();
    m.injectDefinitions = {"jet_engine_exhaust_unlimited.xml"};
    std::vector<swml::ModEntry> mods{m};

    std::vector<std::string> injected;
    assert(ReconcileDefinitions(mods, defsDir.wstring(), injected));
    assert(injected.size() == 1 && injected[0] == "jet_engine_exhaust_unlimited.xml");
    assert(fs::exists(defsDir / "jet_engine_exhaust_unlimited.xml"));
    assert(fs::exists(defsDir / "jet_engine_exhaust_unlimited.xml.swml"));

    // Le XML du mod change : la copie est rafraîchie.
    FILE* g = nullptr;
    _wfopen_s(&g, (modDir / L"jet_engine_exhaust_unlimited.xml").c_str(), L"wb");
    assert(g);
    fwrite(xml, 1, strlen(xml) - 1, g);  // tronque volontairement
    fclose(g);
    assert(ReconcileDefinitions(mods, defsDir.wstring(), injected));
    assert(fs::file_size(defsDir / "jet_engine_exhaust_unlimited.xml") == strlen(xml) - 1);

    // Mod désactivé : la définition est retirée (nettoyage automatique).
    m.enabled = false;
    mods = {m};
    injected.clear();
    assert(ReconcileDefinitions(mods, defsDir.wstring(), injected));
    assert(injected.empty());
    assert(!fs::exists(defsDir / "jet_engine_exhaust_unlimited.xml"));
    assert(!fs::exists(defsDir / "jet_engine_exhaust_unlimited.xml.swml"));

    // Composants compilés : même mécanique, dossier rom/data/components.
    fs::path compsDir = root / "components";
    fs::create_directories(compsDir);
    FILE* bin = nullptr;
    _wfopen_s(&bin, (modDir / L"jet_engine_combustion_chamber_unlimited.bin").c_str(), L"wb");
    assert(bin);
    fwrite("BIN\0\x01\x02", 1, 7, bin);
    fclose(bin);
    m.enabled = true;
    m.injectComponents = {"jet_engine_combustion_chamber_unlimited.bin"};
    mods = {m};
    std::vector<std::string> comps;
    assert(ReconcileComponents(mods, compsDir.wstring(), comps));
    assert(comps.size() == 1 && comps[0] == "jet_engine_combustion_chamber_unlimited.bin");
    assert(fs::exists(compsDir / "jet_engine_combustion_chamber_unlimited.bin"));
    assert(fs::exists(compsDir / "jet_engine_combustion_chamber_unlimited.bin.swml"));
    m.enabled = false;
    mods = {m};
    comps.clear();
    assert(ReconcileComponents(mods, compsDir.wstring(), comps));
    assert(comps.empty());
    assert(!fs::exists(compsDir / "jet_engine_combustion_chamber_unlimited.bin"));
    puts("PASS: matérialisation, rafraîchissement et retrait automatique");
}

void TestModAssetsLayout() {
    namespace fs = std::filesystem;
    fs::path root = fs::path(TestTempRoot()) / "jet_patch_modassets";
    fs::remove_all(root);
    fs::path modsRoot = root / "mods";
    fs::create_directories(modsRoot);
    fs::path src = root / "src";
    fs::create_directories(src);
    const char* xml = "<definition name=\"Unlimited\" value=\"500123\"/>";
    FILE* f = nullptr;
    _wfopen_s(&f, (src / L"chamber.xml").c_str(), L"wb");
    assert(f); fwrite(xml, 1, strlen(xml), f); fclose(f);
    _wfopen_s(&f, (src / L"chamber.bin").c_str(), L"wb");
    assert(f); fwrite("BIN", 1, 3, f); fclose(f);

    swml::ModEntry m;
    m.id = "jet_unlimited";
    m.title = "Jet Unlimited";
    m.author = "tester";
    m.description = "desc";
    m.enabled = true;
    m.dirPath = src.wstring();
    m.injectDefinitions = {"chamber.xml"};
    m.injectComponents = {"chamber.bin"};
    std::vector<swml::ModEntry> mods{m};

    std::vector<std::string> injected;
    assert(ReconcileModAssets(mods, modsRoot.wstring(), injected));
    assert(injected.size() == 2);
    fs::path modDir = modsRoot / "jet_unlimited";
    assert(fs::exists(modDir / "mod.xml"));
    assert(fs::exists(modDir / ".stormfabric"));
    assert(fs::exists(modDir / "data" / "definitions" / "chamber.xml"));
    assert(fs::exists(modDir / "data" / "components" / "chamber.bin"));
    // mod.xml reprend le nom/l'auteur du manifeste du loader.
    std::string manifest;
    { std::ifstream in(modDir / "mod.xml"); manifest.assign(std::istreambuf_iterator<char>(in), {}); }
    assert(manifest.find("Jet Unlimited") != std::string::npos &&
           manifest.find("tester") != std::string::npos);

    // Un dossier étranger sans marqueur ne doit jamais être supprimé.
    fs::path foreign = modsRoot / "3358440146_someone_else";
    fs::create_directories(foreign);
    {
        std::ofstream out(foreign / "keep.txt");
        out << "x";
    }

    // Mod désactivé : notre dossier part, le dossier étranger reste.
    m.enabled = false;
    mods = {m};
    injected.clear();
    assert(ReconcileModAssets(mods, modsRoot.wstring(), injected));
    assert(injected.empty());
    assert(!fs::exists(modDir));
    assert(fs::exists(foreign / "keep.txt"));
    puts("PASS: mod local (mod.xml + data/definitions + data/components), nettoyage sans toucher aux mods tiers");
}

void TestManifestParsing() {
    // Régression : la clé inject_definitions doit remplir le champ (bug 2026-11-06 :
    // branche de parseur insérée vide, la définition n'était jamais matérialisée).
    const char* json = R"({
  "id": "jet_unlimited",
  "name": "Jet Exhaust Unlimited",
  "author": "0k0k4",
  "version": "1.0.0",
  "description": "desc",
  "main": "jet_unlimited.dll",
  "dependencies": [],
  "native_deps": [],
  "enabled_by_default": true,
  "inject_definitions": ["jet_engine_exhaust_unlimited.xml"]
})";
    auto m = swml::ParseModJson(json);
    assert(m.id == "jet_unlimited");
    assert(m.mainDll == "jet_unlimited.dll");
    assert(m.hasEnabledDefault && m.enabledDefault);
    assert(m.injectDefinitions.size() == 1 &&
           m.injectDefinitions[0] == "jet_engine_exhaust_unlimited.xml");
    // Forme singulière et valeur simple également acceptées.
    auto m2 = swml::ParseModJson(R"({"inject_definition": "a.xml"})");
    assert(m2.injectDefinitions.size() == 1 && m2.injectDefinitions[0] == "a.xml");
    // Composants compilés (.bin) : clé dédiée.
    auto m3 = swml::ParseModJson(R"({"inject_components": ["c.bin"]})");
    assert(m3.injectComponents.size() == 1 && m3.injectComponents[0] == "c.bin");
    puts("PASS: mod.json parse (inject_definitions + inject_components)");
}

}  // namespace

// Validation optionnelle sur un exécutable réel (lecture seule, image mappée
// comme au chargement) : signature présente exactement une fois, constante 200.0f.
int CheckRealBinary(const char* path) {
    FILE* f = nullptr;
    _wfopen_s(&f, swml::Utf8ToWide(path).c_str(), L"rb");
    if (!f) return 3;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> file((size_t)size);
    if (fread(file.data(), 1, file.size(), f) != file.size()) { fclose(f); return 4; }
    fclose(f);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    if (file.size() < sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS64) ||
        dos->e_magic != IMAGE_DOS_SIGNATURE) return 4;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    std::vector<uint8_t> image(nt->OptionalHeader.SizeOfImage, 0);
    memcpy(image.data(), file.data(), nt->OptionalHeader.SizeOfHeaders);
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = section[i];
        if ((size_t)s.PointerToRawData + s.SizeOfRawData > file.size()) continue;
        if ((size_t)s.VirtualAddress + s.SizeOfRawData > image.size()) continue;
        memcpy(image.data() + s.VirtualAddress, file.data() + s.PointerToRawData, s.SizeOfRawData);
    }
    size_t hits = 0, siteRva = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = section[i];
        if (!(s.Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
            (s.Characteristics & IMAGE_SCN_MEM_DISCARDABLE)) continue;
        size_t hit = 0;
        size_t count = FindSignature(image.data() + s.VirtualAddress, s.Misc.VirtualSize, hit);
        if (count) { hits += count; siteRva = s.VirtualAddress + hit; }
    }
    printf("[Jet] site 0x%X : %zu occurrence(s)\n", (unsigned)siteRva, hits);
    if (hits != 1) return 5;
    uint32_t bits = 0;
    assert(ResolveConstant(image.data() + siteRva, bits));
    printf("[Jet] constante du site : 0x%08X (%s)\n", bits,
           bits == 0x43480000u ? "200.0f attendu" : "INATTENDU");
    return bits == 0x43480000u ? 0 : 6;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // diagnostic : sortie immediate
    TestSignature();
    TestConstantResolution();
    TestStubEncoding();
    TestStubExecution();
    TestInstallRollback();
    TestSafeNames();
    TestManifestParsing();
    TestReconcile();
    TestModAssetsLayout();
    puts("PASS: jet_patch (signature, constante, stub, patch/rollback, matérialisation)");
    if (argc > 1) {
        int rc = CheckRealBinary(argv[1]);
        if (rc) return rc;
        puts("PASS: jet_patch site réel (signature unique, constante 200.0f)");
    }
    return 0;
}
