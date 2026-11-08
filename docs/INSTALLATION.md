# Installation — Jet Exhaust Unlimited

Guide pas à pas pour installer, vérifier et désinstaller le mod avec le loader
StormFabric.

---

## 1. Pré-requis

| Élément | Valeur |
|---|---|
| Jeu | Stormworks: Build and Rescue, Windows x64 |
| Builds supportés | **1.15.23** (`f9206d85…`) et **1.15.18** (`55914301…`) |
| Loader | StormFabric compilé (`iphlpapi.dll`) |
| DLL du mod | `jet_unlimited.dll` compilée en x64 |
| Accès | écriture dans le dossier du jeu |

Vérifier le SHA-256 de `stormworks64.exe` (PowerShell) :

```powershell
Get-FileHash "C:\Program Files (x86)\Steam\steamapps\common\Stormworks\stormworks64.exe" -Algorithm SHA256
```

Le hash doit correspondre à l'un des deux ci-dessus. Sinon, le loader refusera
tous les hooks : c'est volontaire.

---

## 2. Compiler le loader et la DLL du mod

Depuis le dépôt StormFabric :

```bat
cd /d "H:\Bordel 4TO NVME\StormFabric"
cmd /c ".\build.bat"
cmd /c "examples\build_examples.bat"
```

Résultats :

```text
StormFabric\build\iphlpapi.dll
StormFabric\build\examples\jet_unlimited\jet_unlimited.dll
StormFabric\build\examples\jet_unlimited\mod.json
StormFabric\build\examples\jet_unlimited\*.xml
StormFabric\build\examples\jet_unlimited\*.bin
```

La DLL est une couche SDK minimale : elle journalise et expose `SwModInit` /
`SwModUnload`. Elle ne contient ni adresse moteur ni patch. Le loader la charge
comme DLL principale du mod (sinon le mod est signalé `MISSING DLL`) ;
l'injection du composant est faite par le loader, pas par cette DLL.

---

## 3. Installer le loader

1. **Fermer Stormworks** (vérifier dans le gestionnaire de tâches qu'il ne reste
   pas de `stormworks64.exe`).
2. Localiser le dossier du jeu (celui qui contient `stormworks64.exe`).
3. Si un `iphlpapi.dll` existe déjà, le sauvegarder sous un autre nom.
4. Copier `StormFabric\build\iphlpapi.dll` à la racine du jeu.

Le loader est un proxy : `stormworks64.exe` importe `IPHLPAPI.DLL` ; Windows
charge la DLL du dossier de l'exécutable en premier, puis les forwarders
renvoient les exports vers la vraie `C:\Windows\System32\iphlpapi.dll`.

> Ne pas copier `iphlpapi.dll` dans `System32` ni dans un autre dossier : il doit
> être **à côté de `stormworks64.exe`**.

---

## 4. Installer le mod

Créer l'arborescence suivante :

```text
<Stormworks>\
├── stormworks64.exe
├── iphlpapi.dll
└── mods\
    ├── _modloader.ini
    └── jet_unlimited\
        ├── mod.json
        ├── jet_unlimited.dll
        ├── jet_engine_combustion_chamber_unlimited.xml
        ├── jet_engine_combustion_chamber_unlimited.bin   (asset historique, non requis)
        ├── jet_engine_exhaust_unlimited.xml
        └── README.md
```

Copier les fichiers depuis :

- `Jet Exhaust Unlimited Mod StormFabric\mod\jet_unlimited\` (mod.json, XML,
  `.bin`, README, source `.cpp`) ;
- `StormFabric\build\examples\jet_unlimited\jet_unlimited.dll` (DLL compilée).

Le `.bin` et le XML doivent avoir le **même radical** :
`jet_engine_combustion_chamber_unlimited`. C'est ce que vérifie le loader pour
armer le mod :

- `inject_components` contient le `.bin` ;
- `inject_definitions` contient le `.xml` correspondant.

Sans le `.bin` ou sans le XML, le loader journalise
`Jet: composant declare absent` / `descripteur XML introuvable` et n'installe
rien.

### 4.1 `mods\_modloader.ini`

Le fichier contient l'état d'activation des mods, une ligne `nom_dossier=0|1` :

```ini
jet_unlimited=1
```

- `1` : mod actif ;
- `0` : mod désactivé (aucun hook, aucun patch) ;
- clé absente : la valeur par défaut de `mod.json` (`enabled_by_default: true`)
  s'applique.

Le nom de la clé est le **nom du dossier** (`jet_unlimited`). Si vous renommez le
dossier, renommez la clé.

Le menu **MODS** du loader écrit ce fichier. On peut aussi éditer le fichier à la
main, jeu fermé.

---

## 5. Premier lancement

1. Lancer Stormworks par Steam.
2. Attendre le menu principal, puis vérifier `StormworksModLoader.log` (à côté de
   l'exécutable).
3. Ouvrir **MODS** : `jet_unlimited` doit être listé et activé.
4. Entrer dans l'atelier, catégorie **JET ENGINES** : le composant
   **Jet Combustion Chamber Unlimited** doit apparaître.
5. Placer le composant sur un véhicule, le relier au réacteur, dépasser
   ~200 RPS : il ne doit pas exploser.
6. Placer une **chambre de combustion vanilla** à côté et dépasser 200 RPS :
   elle doit exploser normalement.

### 5.1 Lignes de log attendues

```text
Jet: disposition definition build 2 : 11528 o par variante (dernier champ detruit +0x2D00)
Jet: hooks catalogue installes (build 2) : lookup + ComponentRegistry::Clear
Jet: source='jet_engine_combustion_chamber' ptr=... cat=11 value=... flags=... -> clone ...
Jet: composant 'jet_engine_combustion_chamber_unlimited' injecte au catalogue (memoire, value=500123, registre=...)
Jet: patch sur-regime installe en mode BYPASS (site ..., stub ...)
```

L'injection peut aussi être annoncée à la demande :

```text
Jet: lookup 'jet_engine_combustion_chamber_unlimited' -> injecte a la demande
```

C'est normal : le jeu résout les identifiants d'un véhicule pendant la
désérialisation, avant les pulsations UI.

---

## 6. Désactivation

### 6.1 Désactiver l'injection catalogue seulement

```bat
set SW_JET_CATALOGUE=0
```

Puis relancer le jeu. Le mod ne pose pas les hooks de catalogue ; le composant
n'apparaît plus. Le patch de sur-régime reste armé mais ne trouve aucun composant
`value=500123`, donc il est sans effet visible.

La variable est lue au démarrage. Absente ou valeur non reconnue : injection
active. Une valeur commençant par `0`, `n`, `N`, `f`, `F` désactive.

### 6.2 Désactiver tout le mod

Dans `mods\_modloader.ini` :

```ini
jet_unlimited=0
```

Ou décocher le mod dans **MODS**, puis redémarrer. Le mod ne pose alors ni hook
catalogue ni patch.

### 6.3 Bissection

Créer un fichier vide :

```text
mods\jet_unlimited\.no_catalogue_injection
```

L'insertion au catalogue est désactivée ; les hooks de diagnostic restent posés.
Supprimer le fichier pour réactiver. Utile pour déterminer si un blocage de
chargement vient de l'injection ou du monde.

---

## 7. Désinstallation

1. Fermer le jeu.
2. Supprimer `mods\jet_unlimited\`.
3. Mettre `jet_unlimited=0` dans `mods\_modloader.ini` (ou supprimer la ligne).
4. Pour retirer le loader : supprimer `iphlpapi.dll` du dossier du jeu, ou
   restaurer le fichier sauvegardé.
5. Ne pas toucher à `rom/`, `%APPDATA%\Stormworks` ni à `stormworks64.exe`.

Aucun fichier du jeu n'est modifié par le mod ou par le loader ; la
désinstallation est donc une simple suppression de fichiers ajoutés.

---

## 8. Vérifications de non-régression

Après une mise à jour du loader ou du mod, reconstruire puis réinstaller :

```bat
cd /d "H:\Bordel 4TO NVME\StormFabric"
cmd /c ".\build.bat"
cmd /c ".\tests\run_tests.bat"
```

Attendu :

```text
Build successful: ...\build\iphlpapi.dll
All available test suites passed successfully!
```

Tests ciblés :

```bat
build\tests\jet_patch_tests.exe "H:\Jeux\StormworksV1.15.23\Stormworks\stormworks64.exe"
build\tests\jet_catalogue_tests.exe "H:\Jeux\StormworksV1.15.23\Stormworks\stormworks64.exe" "H:\Jeux\Stormworks\stormworks64.exe"
```

---

## 9. Dépannage

| Symptôme | Vérifier |
|---|---|
| Le jeu ne démarre plus | `iphlpapi.dll` x64, version du jeu, conflit avec un autre proxy |
| Aucun log `Jet:` | le loader n'est pas chargé (mauvais dossier, jeu non lancé par Steam, binaire non reconnu) |
| Composant absent | `SW_JET_CATALOGUE=0` ? marqueur `.no_catalogue_injection` ? mod actif ? `.bin` et `.xml` présents ? |
| Composant présent mais explose | patch non installé : chercher `signature ... absente/ambigue` ou `constante ... inattendue` |
| Composant disparaît d'un véhicule | réinjection trop tardive : vérifier `Jet: lookup ... -> injecte a la demande` et recharger |
| `LoadLibrary failed` | DLL 32 bits, dépendance manquante, ou nom `dll` incorrect dans `mod.json` |
| Crash au shutdown | version de loader trop ancienne (correctif `ShutdownJetCatalogueInjection`) ; ne pas mélanger des copies |

Le journal est la première source : `StormworksModLoader.log`, rechercher `Jet:`
et `Jet[diag]`.
