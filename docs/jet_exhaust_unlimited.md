# Mode « Jet Exhaust Unlimited » — plan et preuves RE

> **Note (2026-11-09) — architecture cible atteinte.** Le code de ce plan est
> désormais compilé dans la **DLL du mod** (`src/jet_*.cpp`, `build_mod.bat`), et
> le loader StormFabric ne contient plus aucune référence au composant : il
> expose seulement les primitives natives génériques (`sw_hook_inline`,
> `sw_engine_symbol_find`, `sw_memory_*`, `sw_game_alloc`,
> `sw_engine_string_make`). Les adresses des services moteur viennent du profil
> de découverte du loader ; le site du sur-régime est résolu par signature.
>
> **Note de la copie (2026-11-08).** Ce document est repris tel quel de
> StormFabric : c'est la note de travail historique, pas la référence de l'état
> courant. Deux points ont évolué depuis : le stub utilise `INFINITY` (`+inf`) et
> non `1e30f`, et il écrit le RPS dans `XMM0` (et non `XMM1`). Le chemin actif
> injecte le composant **en mémoire** (clone de la définition vanilla) ; les
> sections sur la matérialisation de fichiers dans `rom/` décrivent l'ancien
> chemin, conservé dans le code mais plus appelé. Voir `README.md`,
> `docs/FONCTIONNEMENT.md` et `docs/REVERSE_ENGINEERING.md` pour l'état courant.

Objectif : un **nouveau** composant jet exhaust sans la limite de sur-régime
(explosion ~200 RPS), **sans modifier** le comportement du composant vanilla.
Cible : StormFabric (loader) + un mod SDK, builds reviewés 1.15.18 et newer.

## 1. Pourquoi ça explose (trouvé, preuves Ghidra)

Projet : `C:\Users\cedri\ghidra_stormwork_new` (stormworks64.exe 1.15.23,
SHA-256 `f9206d85…`, base `0x140000000`).

- Tous les composants du réacteur modulaire partagent **une seule classe native** :
  `c_scene_vehicle_component_jet_engine` (vtable `0x140b887c0`), hiérarchie
  `c_scene_vehicle_component_extended_base -> dynamic_base -> base`.
- La mise à jour par tick est le **slot 107** de la vtable : `FUN_140350f40`
  (RVA `0x350f40`, ~42 Ko, branchée sur `jet_engine_component_type`).
- Types : 0=intake, 1=compressor, **2=combustion chamber**, 3=turbine,
  4=exhaust (basic/afterburner/rotating), 5=duct.
- Dans la branche `type == 2` (**chambre de combustion**) :

```c
state = FUN_140294520();                    // singleton état du jeu
if (state[0x35] != 0 &&                     // flag dégâts activés (à confirmer)
    state[0x4a] != 0 &&                     // flag explosions (à confirmer)
    DAT_140bb07d8 < *(float*)(this + 0x1bc) &&   // RPS interne > 200.0f CODÉ EN DUR
    *(char*)(this + 0x4d9) == 0) {          // garde « déjà explosé »
    *(char*)(this + 0x4d9) = 1;
    FUN_140288f70(..., DAT_140bb0740, 200.0f, ...);  // spawn de l'explosion
}
```

- La constante `200.0f` vit en `.rdata` (`0x140bb07d8`) et est référencée par
  **50+ fonctions** : ne **jamais** patcher la constante, seulement le site de branche.
- **Piège important** : la garde `+0x4d9` sert aussi à l'explosion « détruit par
  dégâts » (fin de `FUN_140350f40`, test santé puis même flag). Pré-positionner le
  flag supprimerait les deux explosions — rejeté. Il faut neutraliser uniquement
  la branche sur-régime.
- Les XML de `rom/data/definitions/` ne contiennent **aucune** limite RPS
  (`jet_engine_exhaust_basic.xml` vérifié) : la limite est 100 % native.
- Les définitions sont chargées par **énumération du dossier** (chaînes
  `"definitions/"`, `".xml"`, `"rom/data/..."` wide, xrefs `FUN_14046ed20`,
  `FUN_140206400`, `FUN_140871570`, `FUN_140872440`) : ajouter un nouveau fichier
  XML ajoute un nouveau composant sans toucher aux vanilla.

## 2. Résultats RE complémentaires (2026-11-06)

### Site de patch certifié sur les deux builds

Signature opcode unique par binaire (32 octets, rel32 en jokers) :

```
F3 0F 10 87 BC 01 00 00   movss xmm0,[rdi+0x1bc]      ; RPS interne
F3 0F 10 35 ?? ?? ?? ??   movss xmm6,[rip+…]          ; 200.0f (rel32 spécifique au build)
0F 2F C6                  comiss xmm0,xmm6
0F 86 ?? ?? ?? ??         jbe skip
44 38 BF D9 04 00 00      cmp byte [rdi+0x4d9],0      ; garde explosion
```

- 1.15.23 (SHA `f9206d85…`) : **RVA 0x35352a** (une seule occurrence)
- 1.15.18 (SHA `55914301…`) : **RVA 0x35428a** (une seule occurrence)
- Offsets de champs **identiques** sur les deux builds (`+0x1bc` RPS, `+0x4d9` garde).

Plan de patch (fenêtre 19 octets = movss+movss+comiss, le `jbe` original de 6 octets
reste en place et consomme les flags du stub) :

```
CALL stub (E8 rel32, 5 octets) + 14 NOP
stub: push rax/r10 ; def=[rdi+0x58]
      cmp def, [rip+g_unlimitedDef]      ; pointeur capturé à l'enregistrement
      movss xmm6,[rip+g_200]             ; adresse de la constante originale (par build)
      jne .vanilla
      movss xmm6,[rip+g_huge]            ; ~+inf pour notre composant
.vanilla:
      movss xmm1,[rdi+0x1bc]
      comiss xmm1,xmm6                   ; flags finaux pour le jbe original
      pop r10/rax ; ret                  ; ret ne modifie pas les flags
```

Le stub doit résider à ±2 Go du site (allocation près de la base de l'image).
XMM6 original est mort après le comiss (rechargé fraîchement avant le spawn
d'explosion à 140352bda sur 1.15.23) — vérifié.

### Identité instance -> définition

- `this+0x58` -> définition parsée (struct > 0x2900 octets).
- `déf+0xed4` = `jet_engine_component_type` (int) — confirmé : le parseur de
  définitions `FUN_1403168d0` appelle le parseur d'attribut générique avec
  `(int*)(def+0xed4)` pour `"jet_engine_component_type"`.
- `déf+0x298` = `name` (chaîne).
- Stratégie retenue : **capture du pointeur de définition à l'enregistrement**
  (injection -> parse -> capture), comparaison pointeur exacte dans le stub :
  zéro comparaison de chaîne par tick.

### Injection de définition (option A)

- `FUN_14046c180` (1.15.23) scanne `L"rom/data/definitions/"` (itérateur de
  dossier `FUN_1401ec620`), construit la liste de noms, puis `FUN_140206400`
  charge chaque `<nom>.xml` (`FUN_140183d60` = lecture, `FUN_1402067e0` =
  parse + enregistrement) et les enregistre **par nom** dans une hash map.
- Point d'injection : le résultat de l'itération de dossier (le loader ajoute
  le nom de son XML virtuel au conteneur de noms après le scan), puis capture
  du pointeur de définition après `FUN_1402067e0`.

### Reste avant implémentation (mineur)

1. Confirmer l'écrivain de `this+0x58` (chaîne de ctors `FUN_14033b320` /
   site de création `FUN_14045dd20`) — quasi certain vu `déf[+0xed4]` = type.
2. Cerner le conteneur de noms dans `FUN_14046c180` (point d'ajout exact) et
   le site de capture post-enregistrement.
3. Sémantique exacte des flags `state[0x35]`/`state[0x4a]` (probablement
   réglages dégâts/explosions) — informatif, le stub les laisse intacts.

## 3. Plan de développement

### Phase RE restante (avant tout code)

Résolue — voir section 2 (site certifié 2 builds, identité, injection).

**Correction de cible (2026-11-06, après premier test en jeu)** : le test
`200.0f < RPS` se trouve dans la branche `jet_engine_component_type == 2`
(**chambre de combustion**) de `FUN_140350f40`. Le stub compare le `value` de la
définition du composant **updaté** — la signature `500123` doit donc porter sur
une nouvelle **chambre de combustion** (`jet_engine_combustion_chamber_unlimited.xml`,
copie vanilla avec `value="500123"`). Le « Jet Exhaust Unlimited » reste injecté
comme variante cosmétique, mais c'est la chambre qui porte le déverrouillage.

4. **Choix du hook** : patch inline au site de branche avec trampoline loader :
   - si le pointeur de définition du composant == pointeur capturé -> sauter le
     bloc explosion (comportement vanilla sinon) ;
   - installation transactionnelle, vérification des octets originaux, rollback,
     détection de conflit — même discipline que les hooks existants.
   Alternative rejetée : hook de slot 107 + ré-émulation de la logique (trop fragile).

### Phase dev loader

5. Nouveau binding reviewé (2 builds) + transaction de hook pour le site de branche.
6. Nouvelle surface SDK minimaire (append-only) : ex. `api->components` ou une
   fonction dédiée « register jet overrev bypass(signature) ». Les mods restent
   sans adresses moteur.
7. Logs et diagnostics : refus explicites si octets inattendus (lecture seule).

### Phase dev mod

8. Nouveau composant : `jet_engine_exhaust_unlimited.xml` — copie de
   `jet_engine_exhaust_basic.xml` avec nom « Jet Exhaust Unlimited », meshes
   vanilla réutilisés, et une **signature distinctive** dans un attribut sans
   effet gameplay (à choisir à l'étape RE-2) pour identification runtime.
9. Mod `jet_unlimited` (DLL SDK + manifeste) : enregistre la signature auprès du
   loader. L'installation copie le XML dans `rom/data/definitions/` (avec
   sauvegarde/désinstallation propre, jeu fermé).
10. Le composant vanilla reste intact et continue d'exploser > 200 RPS.

### Validation

11. Tests hors-ligne : logique de décision du bypass (fake backend), validation
    des bindings sur les deux binaires, régression suites existantes.
12. Validation en jeu : dépasser 200 RPS -> le nouveau composant ne explosse pas,
    un réacteur vanilla explose toujours ; l'explosion « destruction par dégâts »
    fonctionne sur les deux ; désinstallation propre.

## 4. Implémentation (2026-11-06) — FAIT

### Découverte majeure : le catalogue est baké, les composants de mods sont COMPILÉS

Deux faits établis par RE + observation en jeu :

1. **Le catalogue d'inventaire est baké dans l'exécutable** : les noms des
   définitions vanilla (`jet_engine_exhaust_basic`, `water_jet`, …) sont présents
   comme littéraux dans `stormworks64.exe`. Un XML ajouté à `rom/data/definitions`
   est bien *parsé* (le nom apparaît en infobulle 3D) mais n'entre pas dans le
   catalogue → entrée *placeholder* (`DEF_<FICHIER>_XML_NAME`, `$100`, `mass 1`,
   `tags: blocks`, mesh cube).
2. **Les composants de mods passent par un mod local officiel** :
   `%APPDATA%\Stormworks\data\mods\<id>\` avec `mod.xml` à la racine et les
   composants **compilés** dans `data/components/*.bin` (`FUN_14020f950` lit
   `<modroot>/mod.xml`). Référence observée : un mod Workshop installé
   (`..._more_monitors_mod_v2`) avec 39 XML + 49 `.bin`.

Format `.bin` : `[uint32 taille][contenu]` (compilé par
`<jeu>/sdk/component_mod_compiler.com`).

Notre pipeline : le `.bin` est compilé à la construction avec le compilateur **du
jeu cible** (format lié à la version), livré dans le dossier du mod, puis le loader
installe/rafraîchit un **mod local officiel** sous
`%APPDATA%\Stormworks\data\mods\<id>\` (`mod.xml` généré, `data/definitions` +
`data/components`, marqueur `.stormfabric`), avec nettoyage automatique quand le mod
est désactivé — sans jamais toucher aux mods tiers (vérifié par test).

| Composant | Rôle |
|---|---|
|---|---|
| `src/jet_patch.cpp` | Réconciliation des définitions (`mods/*/inject_definitions` -> `rom/data/definitions`, marqueur `.xml.swml`, retrait automatique), recherche de la signature dans les sections exécutables (exige exactement 1 occurrence), validation de la constante (200.0f exigée), allocation du stub à ±2 Go, patch transactionnel CALL+NOPs (19 octets), rollback propre |
| Stub (50 octets + 2 floats in-bloc) | `def=[rdi+0x58]` ; si `déf+0x2b4 == 500123` (champ value du XML du mod) alors limite `1e30f`, sinon la constante originale copiée ; `comiss` final pour le `jbe` original préservé. Aucune comparaison de chaîne par tick |
| `src/mods_scan.*` | Clés `inject_definitions` (XML -> rom/data/definitions) et `inject_components` (`.bin` -> rom/data/components) dans mod.json, exposées sur `ModEntry` |
| `examples/jet_unlimited/` | `mod.json` (inject_definitions), DLL SDK minimale, `jet_engine_combustion_chamber_unlimited.xml` (le composant fonctionnel : copie vanilla de la chambre, `value="500123"`) + `jet_engine_exhaust_unlimited.xml` (variante cosmétique) |
| `tests/jet_patch_tests.cpp` | Signature (unique/absente/ambiguë/jokers), résolution constante, encodage stub, patch/rollback fail-closed, noms assainis, matérialisation/rafraîchissement/retrait ; check binaire réel optionnel (image mappée) |

Résultats : 14 suites PASS ; check binaire réel sur les deux exécutables reviewés :
site `0x35352A` (1.15.23) et `0x35428A` (1.15.18), une occurrence chacun,
constante `0x43480000` (200.0f). Validation en jeu restante : composant présent
dans l'atelier, dépasser 200 RPS sans explosion, réacteur vanilla toujours
limité, désactivation du mod -> retrait du fichier au démarrage suivant.

## 4bis. Injection au catalogue en mémoire (2026-11-06) — FAIT

Objectif : **le loader C++ seul**, sans mod officiel et **sans aucun fichier écrit
hors de `mods/`**.

### Chemin d'enregistrement natif (RE)

| Rôle | Fonction | 1.15.23 | 1.15.18 |
|---|---|---|---|
| Chargeur de définitions/composants (appelé 1× à l'init) | `FUN_14046c180` | `0x46c180` | `0x46cf70` |
| Fabrique un composant depuis un `.bin` | `FUN_140314ce0` | `0x314ce0` | `0x315a90` |
| Anti-doublon | `FUN_14046e870` | `0x46e870` | `0x46f2f0` |
| **Enregistrement au catalogue** (ring `manager+0xF8`) | `FUN_14046ec30` | `0x46ec30` | `0x46f6b0` |

Le gestionnaire de composants est à `app + 0xbb670`. La fabrique ouvre le fichier
via `_wfopen_s` sur un chemin **relatif à la racine du jeu** → notre `.bin` reste
dans `mods/jet_unlimited/`. L'allocateur et le type de chaîne attendus sont
**déjà** ceux du loader (`op_new` 0x957478, `str_reserve` 0x17cb00, `str_free`
0x9b15a0).

### Mise en œuvre (`src/jet_catalogue.cpp`)

1. **Hook d'entrée** sur `FUN_14046c180` : détour de 50 octets qui sauvegarde les
   5 arguments (4 registres + `[rsp+0x28]`) dans un bloc alloué à ±2 Go, exécute
   l'instruction volée (`mov [rsp+0x20],r9`, la seule des 5 premiers octets) et
   reprend l'exécution. Aucun appel, donc pile intacte.
2. **Enregistrement différé** (`JetCatalogueTick`, appelé depuis la pulsation UI
   déjà hookée) : `StrMake("mods/jet_unlimited/<fichier>.bin")` → `FUN_140314ce0`
   → `FUN_14046e870` → `FUN_14046ec30`. Une seule fois, sur le thread du jeu.
3. **Fail-closed** : les 4 fonctions sont validées par signature d'octets de
   64 octets (masque commun aux 2 builds) ; toute divergence → aucun hook.

Le composant apparaît alors comme un vrai composant du catalogue (nom, prix,
mesh), **sans aucun mod** et sans fichier ajouté au jeu.

Suivi : ces 4 identités sont actuellement certifiées dans le module (le projet
Ghidra de la référence 1.15.18 n'existe plus) ; à terme, les intégrer aux bindings
reviewés centraux régénérés depuis les deux projets.

## 4bis. Injection au catalogue en mémoire (2026-11-06) — FAIT

Objectif : **le loader C++ seul**, sans mod officiel et **sans aucun fichier écrit
hors de `mods/`**.

### Solution retenue : hook de `FindComponentDefinitionByName`

Découverte décisive : l'ancien projet **CustomDisplay** (conservé dans
`tmp/src`, mod supprimé du dépôt) contenait déjà une injection catalogue
**fonctionnelle** (`custom_display_vehicle_observer.cpp`). Sa technique est reprise :

| Rôle | Détail |
|---|---|
| Hook | `FindComponentDefinitionByName(registry, name)` — le **registre arrive en RCX** | 1.15.23 `0x46e900` / 1.15.18 `0x46f380` |
| Hook | `FF 25 00 00 00 00 <imm64>` + 2 NOP (16 octets), trampoline = prologue copié + saut absolu vers `target+16` |
| Registre | ring global `+0xF8` (cap `+0x100`, head `+0x104`, count `+0x108`) ; buckets par catégorie `+0x110 + categorie*24` |
| Définition | objet de `0x2d08` octets en 1.15.23 (`0x2848` en 1.15.18) × 8 variantes, chacune dans SA PROPRE allocation ; id `+0x288`, nom `+0x298`, catégorie `+0x2a8`, **value `+0x2b4`**, flags `+0x2b8` |

Notre implémentation (`src/jet_catalogue.cpp`) :

1. Au **premier** lookup de définition, clone la chambre de combustion vanilla
   (`jet_engine_combustion_chamber`), pose l'identité
   `jet_engine_combustion_chamber_unlimited` / « Jet Combustion Chamber Unlimited »,
   catégorie 11, `value = 500123` (la signature du bypass 200 RPS) et flags 0.
2. Insère le clone dans le ring global **et** dans le bucket de catégorie — c'est ce
   que lit le catalogue de l'atelier.
3. Renvoie le clone quand le jeu cherche notre identifiant.

Aucune réallocation de structure interne : si un ring est plein, on refuse
(fail-closed). Signature d'octets 64 octets validée sur les deux builds ; toute
divergence → aucun hook.

## 5. Risques / ouvertes

- **Multijoueur** : une définition additionnelle peut désynchroniser des clients
  vanilla (checksums de composants) — à tester ; possiblement solo/hôte only.
- La signification exacte des flags `state[0x35]`/`state[0x4a]` (probablement
  réglages « dégâts » / « explosions ») à confirmer pour ne pas désactiver plus
  que prévu.
- Le hook est un patch inline (plus invasif qu'un slot de vtable) : fenêtre
  d'instructions certifiée obligatoire, rejet fail-closed sinon.

## 4ter. Correction du pilotage de l'injection (2026-11-07)

Le composant n'apparaissait jamais : `JetCatalogueTick()` n'était appelé que depuis
les hooks `default_view.init` / `set_alpha` / `interface.update`, qui **n'existent
que dans le menu principal**. Or le registre de composants n'est peuplé qu'au
chargement d'un monde / de l'atelier : le tick ne tournait jamais avec un registre
capturé (et `ValidateRegistry` pouvait en plus rejeter le registre en silence sur
un `head` hors bornes, sans le moindre log).

Corrections :

1. **Pilotage depuis `HookedFindDef`** : le registre arrive en RCX, il est capturé
   puis l'injection est tentée **immédiatement**, sans croissance de ring
   (`RingPush(..., allowGrow=false)`) pour ne jamais réallouer un ring que le
   moteur pourrait parcourir dans la même frame. Cadence bornée à 250 ms.
2. **Pilotage depuis la mise à jour HUD / éditeur** (`src/ingame_notifications.cpp`)
   via `JetCatalogueTick()` : ces mises à jour tournent en jeu, là où le registre
   existe (croissance autorisée à cet endroit).
3. **`head` normalisé** (`NormHead`) au lieu de rejeter le registre ; toute la
   géométrie des rings est journalisée une fois (`Jet[diag] ...`) et chaque refus
   de validation est loggé avec sa raison (`Jet: registre %p refuse par validation`).
4. **Catégorie/flags lus sur la source** : les buckets occupent `+0x110` à `+0x290`
   (16 × 24 octets, certifié par `ComponentRegistry::Clear` 0x5f1ac0) ; on réutilise
   donc la catégorie réelle de la chambre vanilla et l'on conserve ses flags sauf
   les bits `0x80`/`0x800` (qui, dans `FUN_14046ec30`, font sauter l'enregistrement
   dans le bucket de catégorie).
5. **Recherche tolérante de la source** : id exact, puis préfixe, puis tout id
   contenant `combustion_chamber` sans `unlimited` ; l'id réellement trouvé est
   journalisé.

## 4quater. « ring des descripteurs plein » = ring VIDE jamais alloué (2026-11-08)

Symptôme : `grep -c "descripteurs plein" StormworksModLoader.log` → 5, avec le
chargement de monde qui ne se termine pas (`c_game_state_spawning` jamais détruit,
compteur du jeu qui oscille 24↔25).

Preuve dans le log de la session :

```
[00:46:58.658] Jet[diag] registre=... | variantes cap=8192 head=0 count=6072 |
               xml cap=0 head=0 count=0 | global cap=1024 head=0 count=759
[00:46:58.660] Jet: ring des descripteurs plein ; injection annulee (croissance=0)
```

Le ring des descripteurs XML (`registry + 0xe0`) n'était **pas plein** : il était
`cap=0 head=0 count=0`, c'est-à-dire **jamais alloué**. Dans `RingPush`, le test
`count >= cap` est vrai par construction quand `cap == 0` : le code concluait
« plein » et, comme `allowGrow=false` sur le chemin du hook de lookup (interdiction
de réallouer un ring que le moteur peut parcourir dans la même frame), l'injection
échouait **à chaque tentative pendant tout le chargement**. `JetCatalogueTick()`
(seul chemin qui autorise la croissance) n'est pas appelé pendant l'écran de
chargement : l'injection n'a abouti qu'à `00:47:08.603`, 110 ms **après** la fin de
la phase de spawn — trop tard pour un prefab qui a besoin du composant.

Correction (`src/jet_catalogue.cpp`) :

1. `JetCatalogueRingGrowth()` (pure, testée) distingue **allocation** (`cap == 0`,
   aucun buffer à invalider ⇒ sûr même dans une frame du jeu, et
   `FUN_1408de0b0` gère explicitement l'ancien buffer nul) de **realloc**
   (`cap > 0`, toujours interdit dans une frame du jeu).
2. Le clone (8 variantes, `sizeof(ComponentDefinition)` par variante selon le build : `0x2d08` en 1.15.23, `0x2848` en 1.15.18 ; allocations séparées) et le descripteur 32 o sont construits **une seule fois**
   (`PrepareDefinition` / `EnsureDescriptorStruct`) : une insertion refusée ne
   reconstruit plus 82 Ko ni ne fuite ses chaînes ; seule l'insertion est rejouée
   (`InsertDefinition`, rollback intégral au premier refus).
3. Le log ne ment plus : `ring non alloue (cap=0)` / `ring plein` / `geometrie
   invalide`, une fois puis 1 refus sur 200.
4. **Diagnostic de chargement** : chaque identifiant que le jeu cherche sans le
   trouver pendant un chargement est journalisé une fois
   (`Jet: lookup manque pendant le chargement : '<id>'`) — c'est la mesure qui dit
   si un prefab est non instanciable (cause d'un spawn re-tenté en boucle).
5. **Bissection en un fichier** : créer `<mod>/.no_catalogue_injection` (soit
   `mods/jet_unlimited/.no_catalogue_injection`) désactive uniquement l'insertion
   au catalogue, en gardant les hooks de diagnostic. `mods/_modloader.ini` →
   `jet_unlimited=0` désactive tout le mod (aucun hook).

Procédure de bissection (jeu fermé, puis lancé par Steam) :

| Étape | Action | Verdict dans `StormworksModLoader.log` |
|---|---|---|
| A | lancer, charger le monde | `Jet: composant '...' injecte au catalogue` (ou `Jet: lookup '...' -> injecte a la demande`) pendant le chargement puis chargement termine sans blocage ⇒ injection hors de cause |
| B | créer `.no_catalogue_injection`, relancer | si le chargement aboutit **sans** injection ⇒ le blocage vient de l'injection ; s'il est toujours bloqué ⇒ le blocage est dans le monde (les lignes `Jet:` nomment le composant introuvable) |
