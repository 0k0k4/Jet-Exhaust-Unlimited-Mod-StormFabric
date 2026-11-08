#pragma once

namespace swml {
// « Jet Exhaust Unlimited » : nouveau composant sans la limite de sur-régime.
//
// Principe (docs/jet_exhaust_unlimited.md) :
//   1. Les mods déclarent "inject_definitions": ["xxx.xml"] dans mod.json. Init
//      matérialise ces fichiers dans rom\data\definitions (avant le scan du jeu)
//      avec un marqueur de propriété, et retire les fichiers devenus inutiles.
//   2. Le site natif qui explose la chambre au-delà de 200.0f de RPS interne est
//      repéré par une signature d'opcodes unique par build (aucune RVA codée en
//      dur). Un stub loader-owned remplace la constante 200.0f par une valeur
//      quasi infinie UNIQUEMENT pour la définition signée (champ value).
//   3. Sans mod éligible ou sans signature validée : aucun hook, aucun fichier.
bool InitJetExhaustUnlimited();
void JetDiagTick();   // lecture des valeurs capturees (mode diagnostic)
void ShutdownJetExhaustUnlimited();
}
