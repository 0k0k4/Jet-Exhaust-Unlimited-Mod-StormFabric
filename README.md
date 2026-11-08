# Jet Exhaust Unlimited

Une chambre de combustion pour Stormworks qui **n'explose pas au sur-régime**.

Le composant vanilla explose vers 200 RPS. Celui-ci monte aussi haut que tu veux.

Le mod est **autonome** : il porte toute sa logique et s'installe comme n'importe
quel mod StormFabric. Le loader ne contient aucun code propre à ce mod ; il expose
seulement des **primitives natives génériques** (résolution de symboles par profil
de découverte, accès mémoire borné, hooks inline avec trampoline et restauration,
allocateur et chaînes du moteur).

---

## Ce que ça fait

- Ajoute **Jet Combustion Chamber Unlimited** dans l'onglet *JET ENGINES* de l'atelier.
- Aucune limite de sur-régime pour **ce composant uniquement** (identifié par `value = 500123`).
- **Le Jet Exhaust vanilla n'est pas modifié** : il explose toujours à 200 RPS.
- Rien n'est écrit dans le jeu : tout est injecté **en mémoire**, à la demande.
- Le composant **reste sur le véhicule après un aller-retour à l'atelier** :
  l'injection se fait dans le hook de lookup du moteur, à l'instant exact où le
  jeu résout les identifiants du véhicule.

## Ce que ça ne fait pas

- Pas de nouvelles textures ni de nouveau modèle : le composant réutilise le mesh vanilla.
- Ne fonctionne que sur les builds d'exécutable **revus par StormFabric** (le loader
  refuse tout autre binaire ; le mod reste alors inactif, sans aucune écriture).

---

## Prérequis

Le loader **StormFabric** (`iphlpapi.dll`). Le mod n'est pas un mod officiel
Stormworks : c'est la DLL du mod qui crée le composant à la volée, via le SDK natif
du loader.

## Compiler

Depuis ce dépôt (SDK StormFabric dans un checkout voisin) :

```bat
build_mod.bat
```

ou, si le SDK est ailleurs :

```bat
build_mod.bat "H:\chemin\vers\StormFabric\include"
```

Produit `mod/jet_unlimited/jet_unlimited.dll`. Aucune adresse d'exécutable n'est
codée en dur : le mod compile sans copie des sources du loader.

## Installation

1. Copier `iphlpapi.dll` (build de StormFabric) dans le dossier du jeu :
   `...\Stormworks\iphlpapi.dll`
2. Copier le dossier `mod/jet_unlimited/` dans :
   `...\Stormworks\mods\jet_unlimited\`
3. Vérifier `...\Stormworks\mods\_modloader.ini` :

   ```ini
   jet_unlimited=1
   ```

4. Lancer le jeu. Le composant apparaît dans l'atelier, onglet **JET ENGINES**.

Le dossier `mod/jet_unlimited/` contient `mod.json`, `jet_unlimited.dll`, le XML de
la définition et le `.bin` historique (conservé comme asset de référence ; il n'est
plus nécessaire à l'injection, qui clone la définition vanilla en mémoire).

## Désinstallation

- Supprime `mods/jet_unlimited/` et la ligne `jet_unlimited=1`.
- Pour couper seulement l'injection du composant, sans rien supprimer :

  ```bat
  set SW_JET_CATALOGUE=0
  ```

Rien d'autre à nettoyer : aucun fichier n'est créé dans `rom/` ni dans `%APPDATA%`.

---

## Comment ça marche (en bref)

1. **Injection du composant** — le mod clone la définition vanilla
   (`jet_engine_combustion_chamber`) et l'ajoute au catalogue du jeu en
   l'identifiant par son prix : `value = 500123`. L'injection est déclenchée par
   le hook de `FindComponentDefinitionByName` (à la demande) et ré-armée après
   `ComponentRegistry::Clear` : le composant survit au rechargement de l'atelier.
2. **Suppression de la limite** — un stub machine compare la définition du
   composant ; si son `value` vaut `500123`, la limite de sur-régime devient
   `+inf`. Sinon, comportement vanilla inchangé (la constante `200.0f` d'origine
   est recopiée dans le stub).

Toutes les adresses viennent du profil de découverte du loader, ou d'une
signature propre au mod résolue par `sw_engine_signature_find`. Le mod n'écrit
jamais dans le code du jeu directement : il passe par `sw_hook_inline`, qui
valide les octets du prologue, fournit le trampoline et restaure à l'identique.

Détails techniques : [`docs/FONCTIONNEMENT.md`](docs/FONCTIONNEMENT.md)
Rétro-ingénierie : [`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md)

## Tests

```bat
tests\run_tests.bat
```

Les tests hors jeu vérifient la logique pure (croissance de ring, injection à la
demande, géométrie du clone, cohérence des chaînes) et l'encodage machine du stub
de sur-régime. Les tests des primitives du loader (contrat de signature,
hook/restore, refus d'écriture) sont dans StormFabric : `tests\run_tests.bat`.

## Dépannage

**Le composant n'apparaît pas dans l'atelier**
Vérifie `StormworksModLoader.log` : cherche
`composant '...unlimited' injecte au catalogue`. Si la ligne est absente, le log
indique la cause (services moteur non résolus, signature absente, build non certifié).

**Le composant disparaît de mon véhicule après un passage à l'atelier**
Ne devrait plus arriver : l'injection est déclenchée par le lookup du moteur et
ré-armée avant la désérialisation. Cherche `injecte a la demande` dans le log.

**Le jeu plante à la fermeture**
Vérifie qu'aucun `crash.dmp` n'est régénéré. Le mod restaure ses prologues dans
`SwModUnload`, puis Windows décharge le mod avant le loader.

---

## Licence

Voir [`LICENSE`](LICENSE).
