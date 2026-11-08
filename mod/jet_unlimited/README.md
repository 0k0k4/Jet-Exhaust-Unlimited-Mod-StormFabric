# Jet Exhaust Unlimited — dossier du mod

Ce dossier est le contenu à copier dans `<Stormworks>\mods\jet_unlimited\`.
Il ne contient **aucun fichier de définition officiel** : la DLL du mod clone la
définition vanilla **en mémoire** et l'insère dans le catalogue du jeu.

## Fichiers

| Fichier | Rôle |
|---|---|
| `mod.json` | manifeste du mod (id, DLL, activation par défaut) |
| `jet_unlimited.dll` | DLL du mod, **autonome** (produite par `build_mod.bat`) |
| `jet_engine_combustion_chamber_unlimited.xml` | définition fonctionnelle de la chambre sans limite de sur-régime (lue par le mod pour le descripteur) |
| `jet_engine_combustion_chamber_unlimited.bin` | composant compilé historique — **non requis** par l'injection en mémoire |
| `jet_engine_exhaust_unlimited.xml` | variante cosmétique d'échappement (non fonctionnelle pour le bypass) |
| `README.md` | ce fichier |

Il n'y a **pas de `mod.sig`**. La signature du composant est la constante
`value="500123"` du XML, recopiée dans le champ `value` de la définition et
testée par le stub machine du mod.

## Installation

1. Compiler la DLL du mod depuis le dépôt du mod :

   ```bat
   cd /d "H:\Bordel 4TO NVME\Jet Exhaust Unlimited Mod StormFabric"
   cmd /c ".\build_mod.bat"
   ```

2. Copier ce dossier dans `<Stormworks>\mods\jet_unlimited\` (la DLL y est déjà).
3. Vérifier `mods\_modloader.ini` :

   ```ini
   jet_unlimited=1
   ```

4. Lancer le jeu. Le composant **Jet Combustion Chamber Unlimited** apparaît
   dans **JET ENGINES**.

## Ce que fait le mod

- il clone la définition vanilla `jet_engine_combustion_chamber` en mémoire ;
- il remplace id/nom/catégorie/`value` et insère le clone dans les rings du
  catalogue **à la demande**, dans le hook de lookup du moteur ;
- il ré-arme l'injection après `ComponentRegistry::Clear` (retour à l'atelier) ;
- il détourne le test `200.0f < RPS` uniquement pour `value == 500123` ;
- il restaure tout à la fermeture (`SwModUnload`, puis déchargement de la DLL).

Le composant vanilla n'est pas modifié : il explose toujours au-delà de 200 RPS.
Le loader StormFabric ne contient aucun code de ce mod : le mod utilise son API
native générique (`sw_hook_inline`, `sw_engine_symbol_find`, `sw_memory_*`,
`sw_game_alloc`, `sw_engine_string_make`, …).

## Désactivation

- `SW_JET_CATALOGUE=0` : désactive l'injection catalogue (le composant
  n'apparaît pas) ;
- `jet_unlimited=0` dans `mods\_modloader.ini` : désactive tout le mod (aucun
  hook, aucun patch) ;
- fichier `.no_catalogue_injection` dans ce dossier : désactive uniquement
  l'insertion au catalogue.

## Voir aussi

- `../../README.md` — vue d'ensemble, compilation, installation ;
- `../../docs/INSTALLATION.md` — guide complet ;
- `../../docs/FONCTIONNEMENT.md` — mécanismes internes.
