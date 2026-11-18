// Jet Exhaust Unlimited — the mod's DLL (StormFabric native SDK).
//
// The mod is SELF-CONTAINED: it carries all of its logic and only uses the
// loader's generic primitives (symbol resolution through the discovery profile,
// bounded memory access, inline hooks with trampoline and restore, the engine's
// allocator and strings, executable memory). No executable address is hardcoded
// and nothing is written outside the mod folder.
//
//   * src/jet_patch.cpp     : over-rev bypass for the component whose
//                             definition has value == 500123;
//   * src/jet_catalogue.cpp : injection of the component into the catalogue, on
//                             demand, in the engine's lookup hook.
#include "sw_mod_sdk.h"

#include "jet_catalogue.h"
#include "jet_patch.h"
#include "jet_sdk.h"

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <string>

namespace {

constexpr const char* kDefinitionXml = "jet_engine_combustion_chamber_unlimited.xml";

std::string DefinitionPath(const SwModHost* host) {
    if (!host || !host->modDir || !*host->modDir) return std::string();
    std::string path(host->modDir);
    if (path.back() != '\\' && path.back() != '/') path += '\\';
    path += kDefinitionXml;
    return path;
}

bool g_shutdownDone = false;

void ShutdownAll() {
    if (g_shutdownDone) return;
    g_shutdownDone = true;
    jet::ShutdownJetCatalogueInjection();
    jet::ShutdownOverrevPatch();
}

}  // namespace

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!jet::InitEngine(host)) {
        if (host && host->log)
            host->log(host->modId,
                      "ERROR: StormFabric native API unavailable or engine build not certified; "
                      "mod inactive (no hook, no write)");
        return false;
    }
    g_shutdownDone = false;
    const std::string xml = DefinitionPath(host);
    const bool catalogue = jet::InitJetCatalogueInjection(host, xml.c_str());
    const bool overrev = jet::InitOverrevPatch(host);
    jet::LogInfo("Jet Exhaust Unlimited active (build %u): catalogue %s, over-rev %s",
                 jet::g_engine.build, catalogue ? "injected on demand" : "inactive",
                 overrev ? "unlimited for value=500123" : "inactive (vanilla kept)");
    // The mod stays loaded even if a capability is unavailable: every primitive
    // fails closed and has logged the exact cause.
    return true;
}

SW_MOD_EXPORT void SwModTick(void*) {
    jet::JetCatalogueTick();
}

SW_MOD_EXPORT void SwModUnload(void) {
    ShutdownAll();
}

// Safety net: Windows unloads DLLs in reverse load order, so the mod before the
// loader. Our prologues are restored here if SwModUnload was not called.
// Idempotent.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        ShutdownAll();
    }
    return TRUE;
}
