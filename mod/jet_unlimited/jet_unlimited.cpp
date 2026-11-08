// Jet Exhaust Unlimited — DLL du mod (StormFabric SDK natif).
//
// Le mod est AUTONOME : il porte toute sa logique et n'utilise que les primitives
// génériques du loader (résolution de symboles par profil de découverte, accès
// mémoire borné, hooks inline avec trampoline et restauration, allocateur et
// chaînes du moteur, mémoire exécutable). Aucune adresse d'exécutable n'est
// codée en dur et rien n'est écrit hors du dossier du mod.
//
//   * src/jet_patch.cpp     : contournement du sur-régime pour le composant dont
//                             la définition porte value == 500123 ;
//   * src/jet_catalogue.cpp : injection du composant dans le catalogue, à la
//                             demande, dans le hook de lookup du moteur.
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
                      "ERREUR: API native StormFabric indisponible ou build moteur non certifie ; "
                      "mod inactif (aucun hook, aucune ecriture)");
        return false;
    }
    g_shutdownDone = false;
    const std::string xml = DefinitionPath(host);
    const bool catalogue = jet::InitJetCatalogueInjection(host, xml.c_str());
    const bool overrev = jet::InitOverrevPatch(host);
    jet::LogInfo("Jet Exhaust Unlimited actif (build %u) : catalogue %s, sur-regime %s",
                 jet::g_engine.build, catalogue ? "injecte a la demande" : "inactif",
                 overrev ? "sans limite pour value=500123" : "inactif (vanilla conserve)");
    // Le mod reste charge meme si une capacite est indisponible : chaque
    // primitive est fail-closed et a journalise la cause exacte.
    return true;
}

SW_MOD_EXPORT void SwModTick(void*) {
    jet::JetCatalogueTick();
}

SW_MOD_EXPORT void SwModUnload(void) {
    ShutdownAll();
}

// Filet de securite : Windows decharge les DLL dans l'ordre inverse du chargement,
// donc le mod avant le loader. Nos prologues sont restaures ici si SwModUnload
// n'a pas ete appele. Idempotent.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        ShutdownAll();
    }
    return TRUE;
}
