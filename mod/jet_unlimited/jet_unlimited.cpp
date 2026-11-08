// Jet Exhaust Unlimited : le travail est fait par le loader (injection de la
// definition + contournement du sur-regime signe par le champ value). Cette DLL
// ne fait que journaliser et exposer un point d'entree SDK propre.
#include "../../include/sw_mod_sdk.h"

static const SwModHost* g_host = nullptr;

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2) return false;
    g_host = host;
    host->log(host->modId,
              "Jet Exhaust Unlimited actif : composant 'Jet Exhaust Unlimited' "
              "disponible dans l'atelier (value=500123, sans limite de sur-regime). "
              "Le Jet Exhaust vanilla n'est pas modifie.");
    return true;
}

SW_MOD_EXPORT void SwModUnload(void) { g_host = nullptr; }
