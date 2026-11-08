# Jet Exhaust Unlimited — dossier du mod

Ce dossier est le contenu à copier dans `<Stormworks>\mods\jet_unlimited\`.
Il ne contient **aucun fichier de définition officiel** : le loader StormFabric
lit les assets et injecte le composant dans le catalogue du jeu **en mémoire**.

## Fichiers

| Fichier | Rôle |
|---|---|
| `mod.json` | manifeste du mod (id, DLL, `inject_definitions`, `inject_components`) |
| `jet_unlimited.cpp` | source de la DLL SDK minimale (journalisation) |
| `jet_engine_combustion_chamber_unlimited.xml` | définition fonctionnelle de la chambre sans limite de sur-régime |
| `jet_engine_combustion_chamber_unlimited.bin` | composant compilé ; sert de porte d'activation (`inject_components`) |
| `jet_engine_exhaust_unlimited.xml` | variante cosmétique d'échappement (non fonctionnelle pour le bypass) |
| `README.md` | ce fichier |

Il n'y a **pas de `mod.sig`**. La signature du composant est la constante
`value="500123"` du XML, recopiée dans le champ `value` de la définition et
testée par le stub machine du loader.

## Installation

1. Compiler la DLL depuis StormFabric :

   ```bat
   cd /d "H:\Bordel 4TO NVME\StormFabric"
   cmd /c "examples\build_examples.bat"
   ```

2. Copier ce dossier dans `<Stormworks>\mods\jet_unlimited\`, puis y copier
   `StormFabric\build\examples\jet_unlimited\jet_unlimited.dll`.

3. Vérifier `mods\_modloader.ini` :

   ```ini
   jet_unlimited=1
   ```

4. Lancer le jeu. Le composant **Jet Combustion Chamber Unlimited** apparaît
   dans **JET ENGINES**.

## Ce que fait le loader

- il clone la définition vanilla `jet_engine_combustion_chamber` en mémoire ;
- il remplace id/nom/catégorie/`value` et insère le clone dans les rings du
  catalogue ;
- il détourne le test `200.0f < RPS` uniquement pour `value == 500123` ;
- il restaure tout à la fermeture du jeu.

Le composant vanilla n'est pas modifié : il explose toujours au-delà de 200 RPS.

## Désactivation

- `SW_JET_CATALOGUE=0` : désactive l'injection catalogue (le composant
  n'apparaît pas) ;
- `jet_unlimited=0` dans `mods\_modloader.ini` : désactive tout le mod (aucun
  hook, aucun patch) ;
- fichier `.no_catalogue_injection` dans ce dossier : désactive uniquement
  l'insertion au catalogue, garde les hooks de diagnostic.

## Voir aussi

- `../../README.md` — vue d'ensemble et limites ;
- `../../docs/INSTALLATION.md` — guide complet ;
- `../../docs/FONCTIONNEMENT.md` — mécanismes internes.
