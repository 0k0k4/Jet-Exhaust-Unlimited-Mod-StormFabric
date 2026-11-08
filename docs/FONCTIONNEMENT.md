# Fonctionnement — Jet Exhaust Unlimited

Ce document décrit l'implémentation réelle du mod, telle qu'elle existe dans
`src/jet_patch.cpp` et `src/jet_catalogue.cpp`. Les valeurs entre parenthèses
renvoient aux constantes du code ; elles sont vérifiables par recherche directe.

---

## 1. Vue d'ensemble

Le mod ajoute un composant de chambre de combustion sans la destruction
automatique au sur-régime. Il ne remplace aucun fichier du jeu et ne modifie pas
la définition vanilla.

Deux modules distincts, compilés dans **la DLL du mod** (le loader n'en connaît
rien) :

| Module | Rôle | Fonction d'entrée |
|---|---|---|
| `jet_catalogue` | clone une définition vanilla et l'insère dans le registre du jeu, en mémoire | `InitJetCatalogueInjection()` |
| `jet_patch` | détourne le test « RPS > 200.0f » pour le seul composant `value=500123` | `InitOverrevPatch()` |

Toutes les adresses proviennent du profil de découverte du loader (rôles moteur)
ou d'une signature propre au mod résolue par `sw_engine_signature_find`. Les
écritures de code passent par `sw_hook_inline`, qui valide le prologue, fournit
le trampoline et restaure le prologue à l'identique.

Le point de jonction est le champ `value` de la définition :

```
XML du mod : value="500123"
        │
        ├──► catalogue : le clone reçoit value = 500123
        │
        └──► patch : le stub teste [def+0x2b4] == 500123
```

Sans catalogue, le patch ne voit jamais de composant `value=500123` : il est
armé mais sans effet. Sans patch, le composant existe mais explose comme une
chambre vanilla. Les deux sont donc nécessaires au comportement complet.

---

## 2. Injection au catalogue

### 2.1 Pourquoi un clone en mémoire

Le catalogue d'inventaire du jeu est construit à partir de données internes et
de composants compilés, pas simplement par énumération de
`rom/data/definitions`. Ajouter un XML dans `rom/` ne suffit pas : le nom peut
apparaître dans une infobulle, mais l'entrée de catalogue reste un placeholder.

La solution retenue est de **cloner une définition déjà chargée** (la chambre de
combustion vanilla) et de l'insérer dans les structures que le jeu parcourt. Le
jeu ne fait alors aucune différence entre ce clone et une définition officielle :
même classe native, mêmes meshes, mêmes surfaces, mêmes nœuds logiques.

### 2.2 Structures du registre

Le registre de composants est un objet du monde (`world + 0xbb670`). Les rings
sont des `{ void** buf; uint32_t cap; int32_t head; uint32_t count; }` de
24 octets (`stride 0x18`) :

| Offset | Ring | Contenu |
|---:|---|---|
| `+0xc8` | variantes | les 8 variantes de chaque définition |
| `+0xe0` | descripteurs XML | structures de 32 octets `{id, len, cap, xml, xmlLen, xmlCap}` |
| `+0xf8` | global | variante 0 de chaque composant |
| `+0x110 + cat*0x18` | buckets de catégorie | 16 buckets, lus par le catalogue de l'atelier |

Une `ComponentDefinition` est allouée en 8 variantes (indices 0..7, combinaisons
de symétrie). Les champs utilisés :

| Offset | Type | Rôle |
|---:|---|---|
| `+0x40` | `uint8_t` | index de variante |
| `+0x288` | `char*` | id technique |
| `+0x290` / `+0x294` | `uint32_t` | longueur / capacité de l'id |
| `+0x298` | `char*` | nom affiché |
| `+0x2a0` / `+0x2a4` | `uint32_t` | longueur / capacité du nom |
| `+0x2a8` | `int32_t` | catégorie (11 = JET ENGINES) |
| `+0x2b4` | `uint32_t` | `value` (prix) — identité du bypass |
| `+0x2b8` | `uint64_t` | flags |

### 2.3 Hooks

`InitJetCatalogueInjection()` valide d'abord le build :

- signature de 64 octets de `FindComponentDefinitionByName` à
  `0x46e900` (1.15.23) ou `0x46f380` (1.15.18) ;
- signature de 64 octets de `ComponentRegistry::Clear` à `0x5f1ac0` ou
  `0x5f1fd0` ;
- signature de 72 octets de la table de localisation à `0x1f3b30` ou `0x1f5610` ;
- signature de 64 octets de `RingBuffer::Grow` à `0x8de0b0` ou `0x8dd150`.

Puis il pose deux hooks :

1. **`FindComponentDefinitionByName`** : détour de 16 octets
   `FF 25 00 00 00 00 <imm64>` + 2 NOP. Le registre arrive en `RCX`, le nom en
   `RDX`. Le hook lit le nom, capture le registre et tente une réinjection.
2. **`ComponentRegistry::Clear`** : détour de 18 octets. Avant le nettoyage
   moteur, le hook retire toutes nos entrées des rings (par identifiant et par
   identité de bloc publié). C'est indispensable : le moteur détruirait sinon nos
   définitions avec ses propres allocateurs et la taille `0x2d08`/`0x2848`.

L'écriture des sauts est ordonnée : la cible `imm64` et le `disp32` sont écrits
d'abord, les deux octets d'opcode `FF 25` en dernier. Un thread concurrent ne
peut donc jamais décoder un saut dont l'imm64 serait encore vide.

### 2.4 Construction du clone

`PrepareDefinition()` :

1. cherche la source `jet_engine_combustion_chamber` dans le registre
   (recherche exacte, puis tolérante) ;
2. lit sa catégorie, sa `value` et ses `flags` ;
3. lit et valide le XML du mod (max 256 Ko, pas de NUL interne) ;
4. alloue **8 variantes séparées** avec `api.op_new(taille)` où `taille` vaut
   `0x2d08` (1.15.23) ou `0x2848` (1.15.18) ;
5. copie la variante source correspondante dans chaque variante ;
6. réécrit les 8 pointeurs de variantes pour qu'ils pointent tous vers le jeu de
   clones ;
7. remplace id, nom, index de variante, catégorie, `value`, `flags` ;
8. construit le descripteur XML de 32 octets (id + XML lus dans le fichier du
   mod) ;
9. publie le clone dans les rings.

Les chaînes id/nom sont allouées par `sw::StrMake`, donc par l'allocateur du jeu
(`_aligned_free` compatible). Le descripteur est alloué par `operator new(32)` du
jeu et libérable par `operator delete(ptr, 32)`.

La catégorie et les flags sont **lus sur la source** :

```cpp
category = info.category;                       // 11 pour la chambre
flags    = info.flags & ~0x880ull;              // 0x80/0x800 empêchent le bucket
```

Les bits `0x80` et `0x800` sont retirés parce que, dans la fonction
d'enregistrement, ils font sauter l'insertion dans le bucket de catégorie — donc
la présence au catalogue de l'atelier. Les autres flags sont conservés.

### 2.5 Insertion et anti-doublon

`InsertDefinition()` publie le clone dans :

1. le ring des variantes (`+0xc8`) : les 8 variantes ;
2. le ring des descripteurs (`+0xe0`) : le descripteur XML ;
3. le ring global (`+0xf8`) : la variante 0 ;
4. le bucket de catégorie (`+0x110 + cat*0x18`) : la variante 0.

Chaque insertion est vérifiée. Au premier refus, **toutes** les entrées déjà
publiées sont retirées : jamais d'état partiel dans les rings vanilla. Le clone
et le descripteur restent construits pour une tentative ultérieure (le ring peut
être alloué plus tard par le jeu).

L'anti-doublon est double :

- `JetCatalogueLazyPlan(true, published)` renvoie `kPresent` si nos entrées sont
  déjà dans **ce** registre ;
- `IsPublishedEntry()` compare le pointeur à nos blocs publiés, indépendamment
  des champs (donc même si l'id a été corrompu).

### 2.6 Croissance de ring

`RingPush` distingue trois cas (`JetCatalogueRingGrowth`) :

| `cap` | `count` | Décision |
|---:|---:|---|
| `0` | `0` | **allocation** autorisée, même dans une frame du jeu |
| `>0` | `< cap` | rien à faire |
| `>0` | `>= cap` | `realloc` **toujours refusé** |

Un ring à `cap=0` n'a jamais été alloué : il n'existe aucun buffer que le moteur
pourrait parcourir dans la même frame, donc l'allouer est sûr. Un ring réellement
plein (`cap>0`) n'est jamais réalloué depuis un hook de frame de jeu : on attend
une pulsation UI qui autorise la croissance.

Cette distinction corrige le blocage historique : le code confondait
`cap=0,count=0` (ring vide jamais alloué) avec « ring plein », et refusait
l'injection pendant tout le chargement.

### 2.7 Injection à la demande et réinjection

Le jeu résout les identifiants d'un véhicule **pendant la désérialisation**
(retour à l'atelier, chargement d'une sauvegarde), avant toute pulsation UI. Une
injection pilotée par sondage arrive trop tard : le lookup renvoie null et le jeu
retire le composant du build.

Le hook de lookup déclenche donc l'injection **au premier lookup venu** dès que
`JetCatalogueNeedsInjection(registered, builtForThisRegistry)` est vrai :

- `registered == false` (après `Clear`) → réinjecter ;
- `builtForThisRegistry == false` (second registre : monde vs atelier) →
  réinjecter ;
- déjà publié dans ce registre → ne rien refaire.

Le hook de lookup ne provoque **jamais** de `realloc` de ring (`allowGrow=false`).
La cadence défensive est de 10 ms (`kReinjectMinPeriodMs`) pour ne pas refaire
8 × la taille de définition (`0x2d08` en 1.15.23, `0x2848` en 1.15.18) à chaque
lookup, sans réintroduire la fenêtre de 250 ms.

### 2.8 Localisation

Après une insertion réussie, `RegisterAllLocalization()` enregistre les clés :

```text
DEF_JET_ENGINE_COMBUSTION_CHAMBER_UNLIMITED_NAME
def_jet_engine_combustion_chamber_unlimited_name
def_jet_engine_combustion_chamber_unlimited_desc
def_jet_engine_combustion_chamber_unlimited_s_desc
def_jet_engine_combustion_chamber_unlimited_node_<n>_label
def_jet_engine_combustion_chamber_unlimited_node_<n>_desc
```

Les textes sont en anglais ASCII. La police du jeu ne rend pas correctement
certains caractères accentués comme `É` ; c'est pourquoi les noms affichés
évitent les accents.

### 2.9 Démontage

`ShutdownJetCatalogueInjection()` :

1. désarme l'injection (`shuttingDown = true`) ;
2. retire nos entrées des rings avant le `Clear` moteur ;
3. restaure les prologues des deux hooks ;
4. oublie l'état.

Le mod ne libère **jamais** la mémoire du jeu (ni nos chaînes, ni nos clones).
Les fuites sont bornées et volontaires : un démontage qui ne peut pas fauter vaut
mieux qu'un `free` avec le mauvais allocateur. Les trampolines ne sont pas
libérés pour la même raison (un thread du jeu peut encore les exécuter au moment
du `PROCESS_DETACH`).

---

## 3. Patch de sur-régime

### 3.1 Sémantique du code original

Dans la branche `jet_engine_component_type == 2` de la mise à jour du réacteur :

```c
if (state[0x35] != 0 &&
    state[0x4a] != 0 &&
    DAT_140bb07d8 < *(float*)(this + 0x1bc) &&   // 200.0f < RPS
    *(char*)(this + 0x4d9) == 0) {
    *(char*)(this + 0x4d9) = 1;
    /* explosion */
}
```

La constante `DAT_140bb07d8` vaut `200.0f` (`0x43480000`) et est partagée par
plus de 50 fonctions. La garde `+0x4d9` sert aussi à l'explosion « destruction
par dégâts » : la pré-positionner supprimerait les deux explosions. Le mod ne
touche donc **pas** à la garde ; il neutralise uniquement le test de RPS.

### 3.2 Signature

32 octets, `rel32` en jokers :

```text
F3 0F 10 87 BC 01 00 00   movss xmm0,[rdi+0x1bc]
F3 0F 10 35 ?? ?? ?? ??   movss xmm6,[rip+rel32]
0F 2F C6                  comiss xmm0,xmm6
0F 86 ?? ?? ?? ??         jbe  rel32
44 38 BF D9 04 00 00      cmp  byte [rdi+0x4d9],0
```

`FindSignature()` parcourt les sections `IMAGE_SCN_MEM_EXECUTE` non
`IMAGE_SCN_MEM_DISCARDABLE` et retourne le nombre d'occurrences. L'appelant exige
**exactement 1**. Les offsets jokers sont :

- `12..15` : `rel32` du `movss` vers la constante ;
- `20..23` : `rel32` du `jbe`.

`ResolveConstant()` lit le `rel32` à l'offset 12 et la constante à
`site + 16 + rel` (le `rip` est après l'instruction `movss` de 8 octets). Le
loader exige `0x43480000` (`200.0f`). Une constante différente signifie que le
site a changé de sémantique : aucun patch.

### 3.3 Fenêtre de patch

La fenêtre fait 19 octets : `movss` (8) + `movss` (8) + `comiss` (3). Le `jbe`
original (6 octets) n'est pas inclus ; il reste en place.

```text
site + 0   : E8 <rel32>                 ; CALL stub
site + 5   : 90 x 14                    ; NOP
site + 19  : 0F 86 <rel32>              ; jbe original, intact
```

Le stub est alloué par `AllocateNear()` :

- granularité `0x10000` ;
- candidats par pas de `0x400000` (4 Mo), ±64 pas ;
- `MEM_RESERVE | MEM_COMMIT`, `PAGE_EXECUTE_READWRITE` ;
- bloc de `0x68` octets, dans la portée d'un `CALL rel32` (±2 Go).

`InstallPatch()` vérifie les 19 octets originaux contre la signature, sauvegarde
les 19 octets, écrit le `CALL` + les NOP. `WriteProtected()` utilise
`PAGE_EXECUTE_READWRITE`, `FlushInstructionCache`, puis restaure l'ancienne
protection. Un échec de restauration est journalisé et traité comme un échec.

### 3.4 Stub

Taille utile : `0x3A` octets. Les constantes sont dans le même bloc, à `0x3c`
(`+inf`) et `0x40` (`200.0f`), donc les `rip`-relatifs sont fixes et testables
hors jeu.

| Offset | Octets | Instruction |
|---:|---|---|
| `0x00` | `50` | `push rax` |
| `0x01` | `41 52` | `push r10` |
| `0x03` | `48 8B 47 58` | `mov rax,[rdi+0x58]` |
| `0x07` | `48 85 C0` | `test rax,rax` |
| `0x0a` | `74 17` | `jz .vanilla` (offset 35) |
| `0x0c` | `41 BA <imm32>` | `mov r10d,500123` |
| `0x12` | `44 39 90 B4 02 00 00` | `cmp dword [rax+0x2b4],r10d` |
| `0x19` | `F3 0F 10 35 <d1>` | `movss xmm6,[rip+d1]` → `+inf` |
| `0x21` | `74 08` | `je +8` |
| `0x23` | `F3 0F 10 35 <d2>` | `movss xmm6,[rip+d2]` → `200.0f` |
| `0x2b` | `F3 0F 10 87 BC 01 00 00` | `movss xmm0,[rdi+0x1bc]` |
| `0x33` | `0F 2F C6` | `comiss xmm0,xmm6` |
| `0x36` | `41 5A` | `pop r10` |
| `0x38` | `58` | `pop rax` |
| `0x39` | `C3` | `ret` |
| `0x3c` | float | `+inf` |
| `0x40` | float | `200.0f` |

Points importants :

- **XMM0**, pas XMM1 : le code original charge le RPS dans `XMM0` et le jeu le
  relit après le site. Écrire `XMM1` laisserait `XMM0` avec une valeur obsolète.
- `ret` ne modifie pas les flags : le `jbe` original voit le résultat du
  `comiss` du stub.
- `test rax,rax` / `jz` évite tout déréférencement nul si la définition n'est pas
  encore affectée. Le stub n'a pas de SEH.
- Le stub ne compare aucune chaîne par tick : une seule comparaison d'entier.

### 3.5 `+inf` et NaN

Le `jbe` original est pris si `CF=1` ou `ZF=1`. Avec `xmm6 = +inf` :

| `xmm0` (RPS) | `comiss xmm0, +inf` | `jbe` |
|---|---|---|
| fini | `CF=1`, `ZF=0` (RPS < +inf) | pris |
| `+inf` | `CF=0`, `ZF=1` (égalité) | pris |
| `NaN` | `CF=1`, `ZF=1`, `PF=1` (non ordonné) | pris |

Une constante finie `1e30f` couvrait les NaN (`CF=1`) mais **pas** `+inf`
(`+inf > 1e30f` → `CF=0`, `ZF=0`, donc `jbe` non pris → explosion). C'est
pourquoi `kHugeLimit = INFINITY` : `+inf` couvre à la fois `+inf` et les NaN.

### 3.6 Mode diagnostic

`kOverrevPatchEnabled` vaut `true` dans la version copiée : le stub de bypass est
utilisé. Si on le passait à `false`, `InitJetExhaustUnlimited()` installerait un
stub de **diagnostic passif** (`BuildJetDiagStub`) qui enregistre `this`,
définition, RPS et `value` dans le bloc, puis reproduit exactement l'original
(`200.0f`). `JetDiagTick()` lit ces valeurs une fois. Ce mode ne change pas le
comportement du jeu et sert à vérifier l'identité des champs.

> Le commentaire au-dessus de `kOverrevPatchEnabled` dit « DÉSACTIVÉ » : c'est un
> reste d'une phase de diagnostic. La valeur effective est `true`.

### 3.7 Démontage

`ShutdownJetExhaustUnlimited()` :

1. restaure les 19 octets d'origine avec `RevertPatch()` ;
2. si la restauration réussit, libère le stub (`VirtualFree`) et remet l'état à
   zéro ;
3. si elle échoue, journalise et **conserve** le stub : un saut vers un bloc
   libéré serait pire que la fuite.

---

## 4. Cycle de vie complet

```text
DllMain(PROCESS_ATTACH)
  └─ InstallModsUi()
       ├─ ScanMods() : mod actif + inject_components + XML présent
       ├─ InitJetExhaustUnlimited()
       │    ├─ FindSignature() -> 1 occurrence
       │    ├─ ResolveConstant() == 200.0f
       │    ├─ AllocateNear() -> stub
       │    └─ InstallPatch() : CALL + NOP, sauvegarde 19 octets
       └─ InitJetCatalogueInjection(api, xmlPath)
            ├─ JetCatalogueBuild() -> 2 ou 1
            ├─ hooks FindComponentDefinitionByName + ComponentRegistry::Clear
            └─ (au premier lookup) clone + insertion dans les rings

En jeu
  ├─ lookup -> injection à la demande / réinjection après Clear
  ├─ tick de la chambre -> site patché -> stub -> +inf ou 200.0f
  └─ JetCatalogueTick() (UI/HUD) -> tentative d'injection si nécessaire

DllMain(PROCESS_DETACH)
  ├─ ShutdownJetCatalogueInjection() : retrait des rings puis hooks
  └─ ShutdownJetExhaustUnlimited() : restauration des 19 octets puis stub
```

---

## 5. Concurrence et réentrance

- `g_lock` est une `CRITICAL_SECTION` **réentrante** : le hook peut être rappelé
  par le trampoline. Un `SRWLOCK` ne serait pas réentrant.
- `g_cat.inInject` empêche la réentrance pendant la construction/insertion.
- `g_cat.clearing` empêche d'injecter pendant `ComponentRegistry::Clear`.
- `g_cat.shuttingDown` empêche toute écriture après `PROCESS_DETACH`.
- Le hook de lookup ne réalloue jamais un ring (`allowGrow=false`) ; seule la
  pulsation UI autorise la croissance, et uniquement pour `cap=0`.
- `RemoveOursFromRing()` utilise un buffer temporaire (ou un échange avec la
  dernière entrée pour les rings > 65536) : la copie ne s'écrase jamais
  elle-même quand `head != 0`.

---

## 6. Écritures sur disque

Le chemin actif lit :

- `mods/jet_unlimited/mod.json` (par `ScanMods`) ;
- `mods/jet_unlimited/jet_engine_combustion_chamber_unlimited.bin` (existence
  requise par la porte d'activation) ;
- `mods/jet_unlimited/jet_engine_combustion_chamber_unlimited.xml` (descripteur) ;
- éventuellement la DLL du mod.

Il écrit :

- le journal `StormworksModLoader.log` (loader) ;
- rien d'autre.

Le code de réconciliation historique de `jet_patch.cpp` (`ReconcileDefinitions`,
`ReconcileComponents`, `ReconcileModAssets`) n'est pas appelé par le chemin
actuel. Il est conservé pour les tests et pour l'historique, mais ne doit pas
être utilisé pour installer ce mod.

---

## 7. Résumé des invariants

1. Le composant vanilla n'est jamais modifié.
2. La constante globale `200.0f` n'est jamais écrite.
3. Le site de patch est validé par signature **et** par la valeur de la
   constante avant toute écriture.
4. Le clone a 8 variantes de taille moteur exacte, sans chevauchement.
5. Nos entrées de catalogue sont retirées avant le `Clear` moteur.
6. Toute anomalie est fail-closed : pas de hook, pas d'écriture, log explicite.
7. Le patch et les hooks sont restaurés à la fermeture.
