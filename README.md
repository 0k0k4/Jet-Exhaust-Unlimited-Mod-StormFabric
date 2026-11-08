# Jet Exhaust Unlimited — mod pour StormFabric

**Version du mod :** 1.2.0  
**Loader :** StormFabric (proxy `iphlpapi.dll`)  
**Jeu :** Stormworks: Build and Rescue, Windows x64, builds 1.15.23 et 1.15.18  
**Composant fonctionnel :** `jet_engine_combustion_chamber_unlimited`

---

## Sommaire

1. [Quoi](#1-quoi)
2. [Comment](#2-comment)
3. [Compatibilité](#3-compatibilité)
4. [Installation](#4-installation)
5. [Sécurité et réversibilité](#5-sécurité-et-réversibilité)
6. [Relation avec StormFabric](#6-relation-avec-stormfabric)
7. [Tests](#7-tests)
8. [Limites connues](#8-limites-connues)
9. [Contenu du dépôt](#9-contenu-du-dépôt)
10. [Dépannage](#10-dépannage)

---

## 1. Quoi

« Jet Exhaust Unlimited » ajoute une **nouvelle chambre de combustion**,
`jet_engine_combustion_chamber_unlimited`, dans la catégorie **JET ENGINES**, qui
**n'explose pas au sur-régime** (~200 RPS) contrairement au composant vanilla.
Le loader StormFabric l'injecte **entièrement en mémoire** dans le catalogue du
jeu et neutralise le test de sur-régime **uniquement pour ce composant**, identifié
par son champ `value = 500123`.
La chambre de combustion vanilla, elle, conserve exactement son comportement
d'origine : au-delà de 200 RPS, elle explode toujours.

Ce que le mod **ne fait pas** :

- il ne remplace pas le composant vanilla et ne modifie aucun de ses octets ;
- il n'écrit rien dans `rom/` ni dans `%APPDATA%` ;
- il ne patche pas la constante globale `200.0f` (elle est référencée par plus de
  50 fonctions du moteur) : seul le **site de branche** de la chambre est détourné ;
- il ne contourne pas l'explosion « destruction par dégâts » : la garde
  `+0x4d9` du moteur reste intacte ;
- il ne fonctionne pas sans le loader StormFabric : la DLL du mod seule ne fait
  que journaliser.

> **Note de nommage.** Le dépôt et le mod s'appellent « Jet Exhaust Unlimited »,
> mais le composant qui porte effectivement le déverrouillage est la **chambre de
> combustion** (`jet_engine_combustion_chamber_unlimited`). Le fichier
> `jet_engine_exhaust_unlimited.xml` est une variante cosmétique d'échappement,
> conservée comme asset historique ; il n'est pas le composant fonctionnel.

---

## 2. Comment

Le mod repose sur **deux mécanismes indépendants**, tous deux exécutés par le
loader dans le processus du jeu. Ils se rejoignent sur une seule identité :
`value = 500123`.

```
                         mods/jet_unlimited/
                    ┌──────────────────────────┐
                    │ mod.json                 │
                    │  inject_components: .bin │──► porte d'activation
                    │  inject_definitions:.xml │──► descripteur XML lu
                    └──────────────────────────┘
                                │
                                ▼
   catalogue du jeu (mémoire)                    code du jeu (mémoire)
   ┌──────────────────────────────┐   value    ┌───────────────────────────┐
   │ clone x8 de la chambre       │  500123    │ site de sur-régime        │
   │ vanilla + id/nom/value       │ ─────────► │ 19 octets -> CALL stub    │
   │ rings + bucket + descriptor  │            │ stub: +inf ou 200.0f      │
   └──────────────────────────────┘            └───────────────────────────┘
```

### 2.1 Injection du composant au catalogue (mémoire)

Le catalogue d'inventaire du jeu n'est pas un simple dossier XML : il est construit
à partir des définitions vanilla et des composants **compilés** des mods. Le loader
n'utilise ni `rom/data/definitions`, ni `%APPDATA%\Stormworks\data\mods` : il
**clone une définition déjà chargée** et l'insère dans les structures du registre.

Chemin d'injection (`src/jet_catalogue.cpp`) :

1. **Hook de `FindComponentDefinitionByName`** (le registre arrive en `RCX`) :
   détour de 16 octets `FF 25 00000000 <imm64>` + 2 NOP.
   RVA : `0x46e900` en 1.15.23, `0x46f380` en 1.15.18.
2. **Hook de `ComponentRegistry::Clear`** : détour de 18 octets, pour retirer nos
   entrées **avant** que le moteur ne détruise le contenu des rings avec ses
   propres allocateurs.
   RVA : `0x5f1ac0` en 1.15.23, `0x5f1fd0` en 1.15.18.
3. Au premier lookup (ou après un `Clear`, ou dans un second registre), le loader :
   - cherche la définition source `jet_engine_combustion_chamber` dans le registre ;
   - clone ses **8 variantes** (une allocation moteur par variante, pas un bloc
     unique) ;
   - remplace l'identité : id `jet_engine_combustion_chamber_unlimited`, nom
     `Jet Combustion Chamber Unlimited`, catégorie lue sur la source (11 = JET
     ENGINES), `value = 500123`, `flags` de la source sans les bits `0x80`/`0x800` ;
   - construit un descripteur XML de 32 octets à partir du fichier
     `jet_engine_combustion_chamber_unlimited.xml` ;
   - insère le clone dans le ring des variantes (`+0xc8`), le ring des
     descripteurs (`+0xe0`), le ring global (`+0xf8`) et le bucket de catégorie
     (`+0x110 + catégorie * 24`) ;
   - enregistre les chaînes de localisation (`def_<id>_name`, `_desc`, `_s_desc`,
     labels de nœuds) via la table de localisation du jeu.
4. **Injection à la demande** : le hook de lookup réinjecte au premier lookup venu
   dès que l'entrée n'est pas publiée dans **ce** registre. C'est le correctif du
   symptôme « le composant disparaît du véhicule au retour à l'atelier » : le jeu
   résout les identifiants d'un véhicule pendant la désérialisation, avant toute
   pulsation UI.
5. **`ComponentRegistry::Clear`** : nos entrées sont retirées des rings avant le
   nettoyage moteur. Le clone et ses chaînes ne sont jamais libérés par le loader
   (l'API du jeu n'expose pas l'`operator delete` correspondant) : les fuites sont
   bornées et volontaires, pour ne jamais fauter au démontage.

Taille d'une `ComponentDefinition` :

| Build | Taille par variante | Dernier champ détruit |
|---|---:|---:|
| 1.15.23 | `0x2d08` | `+0x2d00` |
| 1.15.18 | `0x2848` | `+0x2840` |

La taille est choisie **par build** (`JetCatalogueDefinitionBytes`). Utiliser
`0x2848` sur 1.15.23 fait empiéter chaque variante sur la suivante : le
destructeur moteur libère alors des champs de la variante voisine, ce qui a
produit un crash au shutdown (double free + `str_free` d'une paire `(len,cap)`
relue comme un pointeur). La géométrie est vérifiée par
`JetCatalogueCloneGeometry` et par les tests.

Champs utilisés (identiques sur les deux builds) :

| Emplacement | Rôle |
|---|---|
| `this + 0x58` | pointeur vers la définition du composant |
| `this + 0x1bc` | RPS interne de la chambre |
| `this + 0x4d9` | garde « déjà explosé » |
| `def + 0x288` | id de la définition |
| `def + 0x298` | nom affiché |
| `def + 0x2a8` | catégorie |
| `def + 0x2b4` | `value` (prix) — **identité du bypass** |
| `def + 0x2b8` | flags |
| `def + 0x40` | index de variante (0..7) |

### 2.2 Neutralisation du sur-régime (patch de code)

Le test natif qui explose la chambre est, dans la branche
`jet_engine_component_type == 2` :

```c
if (... && DAT_140bb07d8 < *(float*)(this + 0x1bc) &&  // RPS > 200.0f
    *(char*)(this + 0x4d9) == 0) {
    *(char*)(this + 0x4d9) = 1;
    /* spawn de l'explosion */
}
```

La constante `200.0f` vit dans `.rdata` et est partagée par de nombreuses
fonctions : elle n'est **jamais** modifiée. Le loader repère le site par une
signature d'opcodes de 32 octets, propre à chaque build :

```
F3 0F 10 87 BC 01 00 00   movss xmm0,[rdi+0x1bc]      ; RPS interne
F3 0F 10 35 ?? ?? ?? ??   movss xmm6,[rip+rel32]      ; 200.0f (rel32 joker)
0F 2F C6                  comiss xmm0,xmm6
0F 86 ?? ?? ?? ??         jbe  skip                    ; rel32 joker
44 38 BF D9 04 00 00      cmp  byte [rdi+0x4d9],0      ; garde explosion
```

- **1.15.23** : site à la RVA **`0x35352a`**, une seule occurrence.
- **1.15.18** : site à la RVA **`0x35428a`**, une seule occurrence.

Le loader exige **exactement une occurrence** dans les sections exécutables non
discardables, puis résout le `rel32` du `movss` et vérifie que la constante
pointée vaut bien `200.0f` (`0x43480000`). Sinon : aucun patch (fail-closed).

La fenêtre patchée fait **19 octets** (`movss` + `movss` + `comiss`) :

```
E8 <rel32>                 CALL stub        ; 5 octets
90 90 90 90 90 90 90 90    NOP              ; 14 octets
90 90 90 90 90 90
```

Le `jbe` original (6 octets) **reste en place** et consomme les flags produits
par le stub. Le stub est alloué à ±2 Go du site (portée d'un `CALL rel32`), dans
une page `PAGE_EXECUTE_READWRITE` de `0x68` octets.

Encodage du stub (`BuildJetStub`, 0x3A octets utiles, constantes à `0x3c`/`0x40`) :

```asm
push rax
push r10
mov  rax,[rdi+0x58]          ; définition
test rax,rax
jz   .vanilla                ; définition pas encore affectée -> vanilla
mov  r10d, 500123            ; identité du composant
cmp  dword [rax+0x2b4], r10d
movss xmm6,[rip+d1]          ; +inf
je   +8
movss xmm6,[rip+d2]          ; 200.0f (copie de la constante vanilla)
.vanilla:
movss xmm0,[rdi+0x1bc]       ; RPS (XMM0, pas XMM1)
comiss xmm0,xmm6
pop  r10
pop  rax
ret                          ; ne modifie pas les flags
```

Décisions :

- `value == 500123` → limite `+inf` ;
- toute autre valeur, ou définition nulle → `200.0f` (comportement vanilla) ;
- le `comiss` final produit les flags pour le `jbe` original ;
- aucun accès à une chaîne de caractères par tick : une seule comparaison
  d'entier.

### 2.3 Pourquoi `+inf` couvre aussi `+inf` et les NaN

Le `jbe` original saute le bloc d'explosion si `RPS <= limite` **ou** si la
comparaison est non ordonnée. Avec `limite = +inf` :

- `RPS = +inf` : `comiss` égal → `ZF=1` → `jbe` pris ;
- `RPS = NaN` : `comiss` non ordonné → `CF=1`, `ZF=1` → `jbe` pris ;
- `RPS` fini : `RPS <= +inf` → `jbe` pris.

Une valeur finie comme `1e30f` couvrait les NaN (`comiss` non ordonné → `CF=1`)
mais **pas** `+inf` (`+inf > 1e30f` → `CF=0`, `ZF=0`, donc `jbe` non pris →
explosion). C'est pourquoi le code utilise `INFINITY` et non `1e30f`. Le `+inf`
ne modifie pas la constante globale : il n'existe que dans le stub.

### 2.4 Activation

Les deux mécanismes sont armés par le loader lorsqu'un mod **actif** déclare un
composant compilé (`inject_components`) et que le XML de descripteur correspondant
existe :

- `InitJetExhaustUnlimited()` cherche et installe le patch de sur-régime ;
- `InitJetCatalogueInjection()` installe les hooks de catalogue et injecte le
  clone.

Si le patch échoue (signature absente/ambiguë, constante inattendue, allocation
impossible), le composant peut tout de même être injecté au catalogue : il se
comportera alors comme une chambre vanilla et **explosera**. C'est le choix
fail-closed : jamais de patch approximatif, jamais de crash.

---

## 3. Compatibilité

Le loader n'installe un hook que sur les exécutables **exactement** reconnus par
leurs signatures et leurs SHA-256.

| Build | SHA-256 de `stormworks64.exe` |
|---|---|
| **1.15.23** | `f9206d85c82f4d02fd0ac391781d19c5c68394a9ffc48accca0e1f5966db8699` |
| **1.15.18** | `55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3` |

Sur tout autre binaire : validation refusée, aucun hook, aucun patch, aucun
risque de saut dans le vide.

### 3.1 Table des RVAs

| Élément | 1.15.23 | 1.15.18 | Utilisé par le code actuel |
|---|---:|---:|---|
| Site de sur-régime (`movss/comiss/jbe`) | `0x35352a` | `0x35428a` | oui (`jet_patch`) |
| `FindComponentDefinitionByName` | `0x46e900` | `0x46f380` | oui (`jet_catalogue`) |
| `ComponentRegistry::Clear` | `0x5f1ac0` | `0x5f1fd0` | oui (`jet_catalogue`) |
| Table d'enregistrement de localisation | `0x1f3b30` | `0x1f5610` | oui (`jet_catalogue`) |
| `RingBuffer::Grow` | `0x8de0b0` | `0x8dd150` | oui (`jet_catalogue`) |
| Chargeur de définitions/composants | `0x46c180` | `0x46cf70` | historique, plus utilisé |
| Fabrique de composant depuis un `.bin` | `0x314ce0` | `0x315a90` | historique, plus utilisé |
| Anti-doublon | `0x46e870` | `0x46f2f0` | historique, plus utilisé |
| Enregistrement au catalogue | `0x46ec30` | `0x46f6b0` | historique, plus utilisé |

Les quatre dernières lignes documentent l'ancien chemin d'injection par fichier
`.bin` ; le code actuel clone la définition vanilla en mémoire et n'appelle plus
ces fonctions. Elles restent dans la documentation de rétro-ingénierie comme
référence.

Constantes de disposition :

| Élément | 1.15.23 | 1.15.18 |
|---|---:|---:|
| Taille d'une `ComponentDefinition` | `0x2d08` | `0x2848` |
| Dernier champ détruit | `0x2d00` | `0x2840` |
| Champ `value` | `+0x2b4` | `+0x2b4` |
| Champ RPS | `+0x1bc` | `+0x1bc` |
| Garde d'explosion | `+0x4d9` | `+0x4d9` |
| Définition depuis l'instance | `this + 0x58` | `this + 0x58` |

La correspondance build → RVA est faite par `JetCatalogueBuild()` (signature de
64 octets à l'adresse de `FindComponentDefinitionByName`) et par
`EngineRva()` pour les autres fonctions. Aucune RVA du site de sur-régime n'est
codée en dur : il est trouvé par `FindSignature()` dans les sections exécutables.

---

## 4. Installation

### 4.1 Pré-requis

- Stormworks x64, build **1.15.23** ou **1.15.18** (SHA-256 ci-dessus).
- Le loader StormFabric compilé : `StormFabric\build\iphlpapi.dll`.
- La DLL du mod compilée : `jet_unlimited.dll` (voir ci-dessous).

La DLL du mod est une fine couche SDK : elle journalise et expose `SwModInit` /
`SwModUnload`. Elle ne contient ni adresse moteur ni patch. Le loader la charge
comme DLL principale du mod (sinon le mod est signalé `MISSING DLL`) ;
l'injection du composant est faite par le loader lui-même, pas par cette DLL.

Compilation depuis StormFabric :

```bat
cd /d "H:\Bordel 4TO NVME\StormFabric"
cmd /c ".\build.bat"
cmd /c "examples\build_examples.bat"
```

Le second script produit `build\examples\jet_unlimited\jet_unlimited.dll`.

### 4.2 Arborescence à installer

Jeu fermé, copier :

```text
<Stormworks>\
├── stormworks64.exe
├── iphlpapi.dll                         ← loader StormFabric
└── mods\
    ├── _modloader.ini                   ← état d'activation
    └── jet_unlimited\
        ├── mod.json
        ├── jet_unlimited.dll
        ├── jet_engine_combustion_chamber_unlimited.xml
        ├── jet_engine_combustion_chamber_unlimited.bin
        ├── jet_engine_exhaust_unlimited.xml   (variante cosmétique, optionnelle)
        └── README.md
```

Le dossier peut être créé à la main ou par le menu **MODS** du loader. Le nom du
dossier doit rester `jet_unlimited` : c'est la clé lue dans `_modloader.ini`.

### 4.3 Étapes

1. **Fermer Stormworks.**
2. Copier `StormFabric\build\iphlpapi.dll` à la racine du jeu, à côté de
   `stormworks64.exe`. C'est un proxy : il forwarde les exports vers la vraie
   `iphlpapi.dll` de `System32`. Si un `iphlpapi.dll` existait déjà, sauvegardez-le
   avant de le remplacer.
3. Créer `<Stormworks>\mods\jet_unlimited\`.
4. Y copier `mod.json`, `jet_unlimited.dll`, les deux XML et le `.bin` (voir
   l'arborescence ci-dessus).
5. Vérifier `mods\_modloader.ini`. La ligne doit être :

   ```ini
   jet_unlimited=1
   ```

   Si le fichier n'existe pas, le loader le crée ; la clé est le nom du dossier.
   Une valeur `0` désactive le mod.
6. Lancer le jeu par Steam. Le loader s'installe au chargement du processus.
7. Ouvrir le menu **MODS** : `jet_unlimited` doit être listé et activé.
8. Dans l'atelier, catégorie **JET ENGINES**, le composant
   **Jet Combustion Chamber Unlimited** doit apparaître.

### 4.4 Vérification dans le log

Le loader écrit `StormworksModLoader.log` à côté de l'exécutable. Les lignes
attendues (ordre indicatif) :

```text
Jet: disposition definition build 2 : 11528 o par variante (dernier champ detruit +0x2D00)
Jet: hooks catalogue installes (build 2) : lookup + ComponentRegistry::Clear
Jet: source='jet_engine_combustion_chamber' ptr=... cat=11 value=... flags=... -> clone ...
Jet: composant 'jet_engine_combustion_chamber_unlimited' injecte au catalogue (memoire, value=500123, registre=...)
Jet: patch sur-regime installe en mode BYPASS (site ..., stub ...)
```

Selon le moment du chargement, l'injection peut être annoncée
« a la demande » (`Jet: lookup ... -> injecte a la demande`) plutôt que par la
pulsation UI. Les deux chemins sont normaux.

### 4.5 Désactivation et désinstallation

- **Désactiver l'injection catalogue uniquement** (le composant disparaît, les
  hooks de diagnostic restent) :

  ```bat
  set SW_JET_CATALOGUE=0
  ```

  La variable est lue au démarrage du processus. Absente ou valeur non reconnue
  (`1`, `y`, `oui`, …) : injection active. Valeur commençant par `0`, `n`, `N`,
  `f`, `F` : injection désactivée. Le patch de sur-régime, lui, reste armé, mais
  sans composant `value=500123` il n'a aucun effet visible.

- **Désactiver tout le mod** : mettre `jet_unlimited=0` dans
  `mods\_modloader.ini`, ou décocher le mod dans le menu **MODS**, puis
  redémarrer. Le loader ne pose alors ni hook catalogue ni patch.

- **Bissection** : créer un fichier vide
  `mods\jet_unlimited\.no_catalogue_injection`. L'insertion au catalogue est
  désactivée, les hooks de diagnostic restent posés. Supprimer le fichier pour
  réactiver.

- **Désinstaller le loader** : fermer le jeu, supprimer `iphlpapi.dll` (ou
  restaurer votre proxy précédent). Aucun fichier du jeu n'a été modifié.

- **Désinstaller le mod** : fermer le jeu, supprimer
  `mods\jet_unlimited\` et mettre `jet_unlimited=0` dans `_modloader.ini`. Aucun
  autre dossier n'est touché.

---

## 5. Sécurité et réversibilité

- **Aucune écriture dans `rom/` ni dans `%APPDATA%`.** Le chemin actif lit
  `mods/jet_unlimited/mod.json`, vérifie la présence du `.bin` et lit le `.xml`
  de descripteur ; il injecte ensuite le composant dans les rings du registre en
  mémoire. Il ne matérialise aucun fichier de définition, ne crée aucun
  `mod.xml` local et ne copie rien dans `%APPDATA%\Stormworks`.
  Le code de réconciliation de fichiers historiques (`ReconcileDefinitions`,
  `ReconcileComponents`, `ReconcileModAssets`) reste compilé dans
  `src/jet_patch.cpp` et couvert par les tests, mais **n'est plus appelé** par le
  chemin d'installation actuel.
- **Aucun `.exe` modifié.** Le loader est une DLL proxy ; le binaire du jeu n'est
  jamais écrit sur disque.
- **Patch réversible.** Les 19 octets d'origine du site sont sauvegardés avant
  écriture. À la fermeture (`DllMain`, `DLL_PROCESS_DETACH`),
  `ShutdownJetExhaustUnlimited()` les restaure puis libère le stub. Si la
  restauration échoue, le stub est conservé et une erreur est journalisée : jamais
  de saut vers un bloc libéré.
- **Catalogue rendu au moteur.** `ShutdownJetCatalogueInjection()` retire nos
  entrées des rings **avant** de restaurer les hooks, pour qu'un
  `ComponentRegistry::Clear` ultérieur ne détruise pas nos définitions avec les
  allocateurs du jeu.
- **Fail-closed.** Signature absente, ambiguë, constante différente de `200.0f`,
  registre invalide, ring plein, allocation impossible : le loader refuse et
  journalise. Il ne tente pas de « réparer » à l'aveugle.
- **Le composant vanilla n'est pas modifié.** Le stub compare `value` et charge
  `200.0f` pour tout le reste. La garde `+0x4d9` reste en place, donc l'explosion
  par dégâts fonctionne toujours.
- **Portée locale au processus.** Le patch et les hooks disparaissent à la
  fermeture du jeu. Aucun service, pilote ou tâche planifiée n'est installé.

---

## 6. Relation avec StormFabric

Ce dépôt est le **projet du mod** : documentation, assets, copies de référence du
code de hook et des tests. Il n'est **pas** un fork complet du loader.

La **source de vérité compilée** reste le dépôt StormFabric :

```text
StormFabric/src/jet_patch.cpp
StormFabric/src/jet_patch.h
StormFabric/src/jet_catalogue.cpp
StormFabric/src/jet_catalogue.h
StormFabric/tests/jet_patch_tests.cpp
StormFabric/tests/jet_catalogue_tests.cpp
StormFabric/tests/jet_test_sigs.h
StormFabric/examples/jet_unlimited/
StormFabric/docs/jet_exhaust_unlimited.md
StormFabric/docs/ENGINE_FUNCTION_MAPPING.md
```

Le `build.bat` de StormFabric compile `src\jet_patch.cpp` et
`src\jet_catalogue.cpp` **dans** `iphlpapi.dll`. Les hooks ne sont pas une DLL
séparée : ils font partie du loader.

### 6.1 Resynchroniser ce dépôt depuis StormFabric

Depuis la racine de ce dépôt :

```bat
copy /Y "H:\Bordel 4TO NVME\StormFabric\src\jet_patch.cpp"      src\
copy /Y "H:\Bordel 4TO NVME\StormFabric\src\jet_patch.h"        src\
copy /Y "H:\Bordel 4TO NVME\StormFabric\src\jet_catalogue.cpp"  src\
copy /Y "H:\Bordel 4TO NVME\StormFabric\src\jet_catalogue.h"    src\
copy /Y "H:\Bordel 4TO NVME\StormFabric\tests\jet_patch_tests.cpp"     tests\
copy /Y "H:\Bordel 4TO NVME\StormFabric\tests\jet_catalogue_tests.cpp" tests\
copy /Y "H:\Bordel 4TO NVME\StormFabric\tests\jet_test_sigs.h"         tests\
copy /Y "H:\Bordel 4TO NVME\StormFabric\examples\jet_unlimited\*"      mod\jet_unlimited\
```

Puis vérifier que le build et les tests StormFabric passent toujours (section 7).
Les docs `docs\jet_exhaust_unlimited.md` et `docs\ENGINE_FUNCTION_MAPPING.md`
sont des copies de référence ; les mettre à jour dans StormFabric d'abord, puis
les recopier ici.

### 6.2 Renvoyer les modifications vers StormFabric

Le script `tools\sync_to_stormfabric.bat` copie les **4 fichiers source** et les
**3 fichiers de test** de ce dépôt vers un checkout StormFabric, sans toucher aux
autres modules :

```bat
tools\sync_to_stormfabric.bat "H:\Bordel 4TO NVME\StormFabric"
```

Ensuite, dans StormFabric :

```bat
cmd /c ".\build.bat"
cmd /c ".\tests\run_tests.bat"
```

> **Règle de travail.** Modifier d'abord StormFabric (source de vérité), compiler
> et tester, puis recopier ici. L'inverse est possible pour une correction
> urgente, mais doit être immédiatement validé par le build et les tests du
> loader.

---

## 7. Tests

Les tests du jet sont des tests **hors jeu** : ils n'exécutent jamais le moteur
Stormworks. Ils utilisent des buffers locaux, une page RWX pour le stub et,
optionnellement, une image PE mappée en lecture seule pour vérifier les
signatures sur les vrais exécutables.

### 7.1 Lancer la suite complète (recommandé)

```bat
cd /d "H:\Bordel 4TO NVME\StormFabric"
cmd /c ".\build.bat"
cmd /c ".\tests\run_tests.bat"
```

Attendu :

```text
Build successful: ...\StormFabric\build\iphlpapi.dll
...
All available test suites passed successfully!
```

`run_tests.bat` compile et exécute `jet_patch_tests.exe` et
`jet_catalogue_tests.exe`, puis les autres suites du loader. Si les exécutables
du jeu ne sont pas trouvés, les contrôles « binaire réel » sont ignorés (SKIP) ;
les tests purs restent exécutés.

### 7.2 Lancer uniquement les tests du jet

```bat
cd /d "H:\Bordel 4TO NVME\StormFabric"

build\tests\jet_patch_tests.exe ^
  "H:\Jeux\StormworksV1.15.23\Stormworks\stormworks64.exe"

build\tests\jet_catalogue_tests.exe ^
  "H:\Jeux\StormworksV1.15.23\Stormworks\stormworks64.exe" ^
  "H:\Jeux\Stormworks\stormworks64.exe"
```

Ordre des arguments de `jet_catalogue_tests.exe` : **1.15.23 d'abord**, **1.15.18
ensuite** (le test attend `build == 2` puis `build == 1`). Sans argument, les
vérifications binaires sont SKIP.

### 7.3 Ce que couvrent les tests

`tests\jet_patch_tests.cpp` :

- signature du site : occurrence **unique**, **absente**, **ambiguë** (deux
  occurrences → refus), zone trop courte, tolérance des `rel32` jokers ;
- résolution de la constante via le `rel32` du `movss` et exigence
  `0x43480000` (`200.0f`) ;
- encodage du stub : registres `rax`/`r10`, garde de définition nulle,
  `value=500123`, `rip`-relatifs internes au bloc, `comiss` final ;
- **exécution réelle** du stub et du `jbe` original dans une page RWX :
  vanilla 250/100 RPS exécutés et journalisés, `unlimited` à 10 000 / `1e30` /
  `+inf` RPS exécuté, définition nulle → comportement vanilla ; les assertions
  fortes portent sur `1e30`, `+inf` et la définition nulle (les cas vanilla sont
  la preuve d'exécution affichée en `[dbg]`, pas une assertion stricte) ;
- patch/rollback : `CALL rel32` + 14 NOP, sauvegarde des 19 octets, restauration,
  refus si le site a déjà été modifié ;
- noms de fichiers assainis (traversée `..`, séparateurs, extensions) ;
- parsing de `mod.json` (`inject_definitions` et `inject_components`) ;
- matérialisation/rafraîchissement/retrait des définitions et composants
  (mécanique historique, encore testée) ;
- disposition d'un mod local (`mod.xml` + `data/definitions` +
  `data/components`) et non-suppression des dossiers tiers ;
- validation optionnelle sur un vrai binaire : une occurrence du site et
  constante `0x43480000`.

`tests\jet_catalogue_tests.cpp` :

- encodage du saut absolu `FF 25 00000000 <imm64>` + NOP, `disp32 = 0`, refus
  des tailles hors bornes ;
- **croissance de ring** : `cap == 0` (ring jamais alloué) → allocation
  autorisée même dans une frame du jeu ; `cap > 0` plein → `realloc` toujours
  refusé ;
- **géométrie du clone** : `0x2d08` par variante en 1.15.23, détection du
  chevauchement si on applique `0x2848`, cohérence en 1.15.18 ;
- **cohérence des chaînes moteur** `{ptr,len,cap}` : `cap >= 32`, `len <= cap`,
  pointeur non nul si `len > 0`, et la valeur fautive `0x0000002000000000` du
  crash dump ;
- **injection à la demande** : plan (`kNotOurs`, `kPresent`, `kInject`), issue
  (`kSourceMissing`, `kRefused`, `kInjected`), textes journalisés, réinjection
  après `Clear` ;
- décision `JetCatalogueNeedsInjection` (registre déjà servi, second registre,
  après `Clear`) ;
- validation des signatures de `FindComponentDefinitionByName` et
  `ComponentRegistry::Clear` sur les deux vrais binaires, et refus si un octet
  est altéré.

### 7.4 Preuve d'exécution après copie

La copie vers ce dépôt n'écrit **aucun** fichier dans StormFabric. La
vérification finale a été faite après la copie :

```text
cmd /c ".\build.bat"            -> Build successful
cmd /c ".\tests\run_tests.bat"  -> All available test suites passed successfully!
```

Les sorties détaillées sont dans l'historique de la session de création du
dépôt ; les tests du jet affichent notamment :

```text
PASS: signature unique/absente/ambigue, jokers rel32, zone courte
[dbg] vanilla 250 RPS -> 1 (attendu 1)
[dbg] vanilla 100 RPS -> 0 (attendu 0)
[dbg] unlimited 10000 RPS -> 0 (attendu 0)
PASS: execution reelle du stub (vanilla 200 RPS, unlimited jamais, def nulle)
PASS: patch installé (CALL rel32 + NOPs), rollback, refus si site modifié
PASS: geometrie du clone (0x2d08 par variante en 1.15.23, chevauchement 0x2848 detecte)
PASS: croissance de ring (allocation d'un ring vide seule autorisee, realloc toujours interdite)
PASS: injection a la demande (plan) - idempotente, anti-doublon, reinjection apres Clear
[Jet] site 0x35352A : 1 occurrence(s)
[Jet] constante du site : 0x43480000 (200.0f attendu)
PASS: jet_patch site réel (signature unique, constante 200.0f)
[Jet] H:/Jeux/StormworksV1.15.23/Stormworks/stormworks64.exe -> build 2
[Jet] H:/Jeux/Stormworks/stormworks64.exe -> build 1
PASS: signature catalogue validee sur binaire reel + refus si octet altere
```

---

## 8. Limites connues

1. **Deux builds seulement.** 1.15.23 et 1.15.18. Sur une autre version, les
   signatures échouent et le loader refuse tout hook. Ce n'est pas un support
   automatique de toutes les versions.
2. **Multijoueur non certifié.** Une définition additionnelle peut désynchroniser
   des clients vanilla (checksums de composants, packets de spawn). À tester au
   cas par cas ; a priori solo/hôte uniquement.
3. **`value = 500123` est aussi le prix.** C'est le champ `value`/prix de la
   définition, utilisé comme identité de signature. En mode économie, le
   composant coûte donc 500123 ; ce n'est pas un attribut purement invisible.
4. **Fenêtre de réinjection.** Le catalogue est vidé par
   `ComponentRegistry::Clear` lors des transitions. Le loader réinjecte au premier
   lookup, mais si un véhicule est désérialisé avant toute réinjection, le
   composant peut manquer et être retiré du build. C'est le symptôme historique
   « le composant disparaît de mon véhicule » ; le correctif d'injection à la
   demande réduit fortement la fenêtre, il ne peut pas la supprimer totalement
   sans hook de désérialisation dédié.
5. **Collision d'identité.** Tout autre composant portant `value = 500123`
   bénéficierait du même bypass. La valeur est réservée par convention au mod.
6. **Clone, pas nouvelle classe.** La chambre est un clone de la chambre vanilla
   (mêmes meshes, mêmes surfaces, mêmes nœuds logiques) avec une nouvelle
   identité. Aucun nouveau modèle 3D n'est ajouté.
7. **Le fichier `jet_engine_exhaust_unlimited.xml` n'est pas le composant
   fonctionnel.** C'est une variante cosmétique d'échappement. Le déverrouillage
   est porté par `jet_engine_combustion_chamber_unlimited`.
8. **Pas de `mod.sig`.** Le dépôt ne contient pas de fichier `mod.sig` : la
   signature est la constante `500123` du champ `value`, codée dans le loader
   (`kSignatureValue`) et recopiée dans le XML. Aucun fichier de signature séparé
   n'est lu.
9. **Glyphes accentués.** La police du jeu ne rend pas correctement certains
   caractères comme `É`. Les noms affichés du composant et les clés de
   localisation restent en ASCII ; les textes français accentués du dépôt ne sont
   pas destinés à l'UI du jeu.
10. **Commentaires historiques.** Dans la copie source, le commentaire au-dessus
    de `kOverrevPatchEnabled` (et un commentaire de `mods_ui.cpp`) parle d'un
    bypass « désactivé ». La constante vaut `true` dans cette version : le bypass
    est actif. Ces commentaires datent d'une phase de diagnostic et n'ont pas été
    réécrits pour ne pas diverger de StormFabric.
11. **Code de réconciliation historique.** `ReconcileDefinitions`,
    `ReconcileComponents` et `ReconcileModAssets` peuvent écrire des fichiers
    s'ils sont appelés, mais le chemin actuel ne les appelle pas. Ils restent
    compilés et testés pour compatibilité.
12. **Constante de test.** `tests/jet_test_sigs.h` déclare
    `kStubBlockBytes = 0x40` alors que le bloc de production fait `0x68` ; le
    test du stub passe sur MSVC mais cette constante mérite d'être alignée lors
    d'une prochaine resynchronisation. Le code de production n'est pas affecté.
    De plus, les cas vanilla 250/100 RPS de `TestStubExecution` sont exécutés et
    journalisés, mais les assertions strictes ne portent que sur `1e30`, `+inf`
    et la définition nulle.
13. **Pas de télémétrie.** Le loader journalise dans
    `StormworksModLoader.log` ; aucun envoi réseau n'est effectué par ce mod.

---

## 9. Contenu du dépôt

```text
Jet Exhaust Unlimited Mod StormFabric/
├── README.md                  ← ce document
├── CHANGELOG.md
├── LICENSE                    (MIT, repris de StormFabric)
├── .gitignore
├── .gitattributes
├── docs/
│   ├── FONCTIONNEMENT.md      architecture détaillée (patch + catalogue)
│   ├── REVERSE_ENGINEERING.md RVA, signatures, offsets, méthode
│   ├── INSTALLATION.md        guide pas à pas
│   ├── jet_exhaust_unlimited.md   note de travail RE d'origine (copie)
│   └── ENGINE_FUNCTION_MAPPING.md référentiel moteur (copie)
├── src/
│   ├── jet_patch.cpp/.h       patch de sur-régime + stub machine
│   └── jet_catalogue.cpp/.h   injection du composant au catalogue
├── mod/
│   └── jet_unlimited/
│       ├── mod.json
│       ├── jet_unlimited.cpp
│       ├── jet_engine_combustion_chamber_unlimited.xml
│       ├── jet_engine_combustion_chamber_unlimited.bin
│       ├── jet_engine_exhaust_unlimited.xml
│       └── README.md
├── tests/
│   ├── jet_patch_tests.cpp
│   ├── jet_catalogue_tests.cpp
│   ├── jet_test_sigs.h
│   └── README.md
└── tools/
    ├── sync_to_stormfabric.bat
    ├── crash_dump.py          parseur de minidump
    ├── match_clear.py         extraction de la signature Clear
    ├── gen_grow.py            extraction de la signature Grow
    ├── README.md
    └── ghidra/
        └── Dump.java
```

Le dossier `mod/jet_unlimited/` est le contenu à copier dans
`<Stormworks>\mods\jet_unlimited\` (avec la DLL compilée). Les fichiers `.bin`
et `.xml` sont volontairement versionnés : ils font partie des assets du mod.

---

## 10. Dépannage

| Symptôme | Cause probable | Action |
|---|---|---|
| Le composant n'apparaît pas | `SW_JET_CATALOGUE=0`, ou marqueur `.no_catalogue_injection` | Vérifier la variable et le marqueur ; consulter le log |
| Le composant n'apparaît pas | mod désactivé / DLL manquante / assets manquants | Vérifier `mods\_modloader.ini`, `jet_unlimited.dll`, le `.bin` et le `.xml` |
| Le composant apparaît mais explose | patch de sur-régime non installé (signature/constante) | Chercher `Jet: signature ... absente/ambigue` ou `constante ... inattendue` dans le log ; vérifier le SHA-256 du binaire |
| Le composant disparaît d'un véhicule | `Clear` + désérialisation avant réinjection | Relire le log `Jet: lookup ... -> injecte a la demande` ; redémarrer et recharger le véhicule |
| Le jeu refuse de démarrer | mauvaise version / DLL 32 bits / proxy en conflit | Vérifier le SHA-256, la compilation x64, et la présence de la vraie `iphlpapi.dll` dans `System32` |
| Crash au shutdown | entrée de catalogue laissée dans les rings | Vérifier la version du loader (correctif `ShutdownJetCatalogueInjection`) ; ne pas mélanger les copies |
| Tests réels ignorés | chemins d'exécutables absents | Passer les chemins en argument (section 7.2) |

Logs utiles :

```text
StormworksModLoader.log
```

Rechercher `Jet:` et `Jet[diag]`. Le module journalise les refus, les géométries
de rings, les sources trouvées et les décisions d'injection.

---

## Licence et avertissement

Le code repris de StormFabric est sous licence **MIT** (voir `LICENSE`).
Stormworks: Build and Rescue est une marque de Geometa ; ce projet n'est ni
affilié ni approuvé par Geometa. Le mod est expérimental : testez sur une
sauvegarde de monde non critique et en solo d'abord.
