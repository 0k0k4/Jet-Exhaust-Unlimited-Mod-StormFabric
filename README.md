# Jet Exhaust Unlimited

Une chambre de combustion pour Stormworks qui **n'explose pas au sur-régime**.

Le composant vanilla explose vers 200 RPS. Celui-ci monte aussi haut que tu veux.

---

## Ce que ça fait

- Ajoute **Jet Combustion Chamber Unlimited** dans l'onglet *JET ENGINES* de l'atelier.
- Aucune limite de sur-régime.
- **Le Jet Exhaust vanilla n'est pas modifié** : il explose toujours à 200 RPS.
- Rien n'est écrit dans le jeu : tout est injecté **en mémoire**.

## Ce que ça ne fait pas

- Pas de nouvelles textures ni de nouveau modèle : le composant réutilise le mesh vanilla.
- Ne fonctionne que sur les builds **1.15.23** et **1.15.18**.

---

## Prérequis

Le loader **StormFabric** (`iphlpapi.dll`). Le mod n'est pas un mod officiel Stormworks : c'est le loader qui crée le composant à la volée.

## Installation

1. Copie `iphlpapi.dll` (build de StormFabric) dans le dossier du jeu :
   `...\Stormworks\iphlpapi.dll`
2. Copie le dossier `mod/jet_unlimited/` dans :
   `...\Stormworks\mods\jet_unlimited\`
3. Dans `...\Stormworks\mods\_modloader.ini`, ajoute :

   ```ini
   jet_unlimited=1
   ```

4. Lance le jeu. Le composant apparaît dans l'atelier, onglet **JET ENGINES**.

## Désinstallation

- Supprime `mods/jet_unlimited/` et la ligne `jet_unlimited=1`.
- Pour couper seulement l'injection du composant, sans rien supprimer :

  ```bat
  set SW_JET_CATALOGUE=0
  ```

Rien d'autre à nettoyer : aucun fichier n'est créé dans `rom/` ni dans `%APPDATA%`.

---

## Comment ça marche (en bref)

Deux mécanismes, tous deux en mémoire :

1. **Injection du composant** — le loader clone la définition vanilla
   (`jet_engine_combustion_chamber`) et l'ajoute au catalogue du jeu en
   l'identifiant par son prix : `value = 500123`.
2. **Suppression de la limite** — un petit patch de code compare le régime du
   composant ; si son `value` vaut `500123`, la limite est remplacée par
   l'infini. Sinon, comportement vanilla inchangé.

Détails techniques : [`docs/FONCTIONNEMENT.md`](docs/FONCTIONNEMENT.md)
Rétro-ingénierie et adresses : [`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md)

## Compatibilité

| Build | Version | État |
|---|---|---|
| 2 | 1.15.23 | ✅ testé |
| 1 | 1.15.18 | ✅ testé |

Toute autre version : le loader refuse de s'installer (aucune modification du jeu).

---

## Relation avec StormFabric

Ce dépôt est le **projet du mod** : documentation, assets, et copie de travail des
sources. Le code compilé vit dans **StormFabric** :

```
StormFabric/src/jet_patch.cpp      -> patch du sur-régime
StormFabric/src/jet_catalogue.cpp  -> injection du composant
```

Pour resynchroniser après une modification ici :

```bat
tools\sync_to_stormfabric.bat
```

## Tests

Depuis StormFabric :

```bat
tests\run_tests.bat
```

Les suites du jet vérifient notamment que le site du patch est **unique** dans le
binaire, que la constante voisine vaut bien `200.0f`, et que le stub encode les
bons registres — puis elles exécutent réellement le stub dans une page mémoire
exécutable.

## Dépannage

**Le composant n'apparaît pas dans l'atelier**
Vérifie le log du loader (`StormworksModLoader.log`) : cherche
`Jet: composant '...unlimited' injecte au catalogue`. Si la ligne est absente,
l'injection n'a pas eu lieu — envoie le log.

**Le composant disparaît de mon véhicule**
Connu et en cours de correction : le catalogue est vidé puis reconstruit au
chargement, et le composant doit être réinjecté à temps.

**Le jeu plante à la fermeture**
Vérifie qu'aucun `crash.dmp` n'est régénéré. Le loader restaure les prologues
modifiés au démontage.

---

## Licence

Voir [`LICENSE`](LICENSE).
