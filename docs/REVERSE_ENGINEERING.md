# Reverse engineering — Jet Exhaust Unlimited

Ce document rassemble les adresses, signatures et offsets nécessaires pour
comprendre et maintenir le mod. La référence absolue reste le code de
`src/jet_patch.cpp` et `src/jet_catalogue.cpp` : toute valeur ci-dessous doit
pouvoir y être retrouvée par recherche directe.

---

## 1. Binaires cibles

| Build | Fichier | SHA-256 |
|---|---|---|
| 1.15.23 | `stormworks64.exe` | `f9206d85c82f4d02fd0ac391781d19c5c68394a9ffc48accca0e1f5966db8699` |
| 1.15.18 | `stormworks64.exe` | `55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3` |

Base d'image par défaut : `0x140000000` (RVA = VA − base). Les deux binaires
sont des exécutables x64 ; les RVA ci-dessous sont indépendantes de l'ASLR.

---

## 2. Méthode

1. **Ghidra** sur `stormworks64.exe` 1.15.23, projet de référence
   `C:\Users\cedri\ghidra_stormwork_new`.
2. Localisation de la mise à jour du réacteur (`FUN_140350f40`, slot 107 de la
   vtable `0x140b887c0`), puis de la branche
   `jet_engine_component_type == 2` (chambre de combustion).
3. Identification du test `200.0f < *(float*)(this + 0x1bc)` et de la garde
   `*(char*)(this + 0x4d9) == 0`.
4. Extraction d'une signature d'opcodes de 32 octets autour du site, avec les
   `rel32` en jokers (ils changent d'un build à l'autre).
5. Report de la signature sur 1.15.18 par comparaison d'octets (masque commun),
   puis vérification qu'elle n'apparaît qu'une fois dans les sections
   exécutables.
6. Pour le catalogue : identification de `FindComponentDefinitionByName`,
   `ComponentRegistry::Clear`, de la table de localisation et de
   `RingBuffer::Grow`, puis extraction de signatures de 64/72 octets avec
   masque (les `call rel32` et `lea rip-relatif` sont masqués).
7. Validation hors jeu : `tests/jet_patch_tests.cpp` et
   `tests/jet_catalogue_tests.cpp` mappent l'image PE en lecture seule et
   vérifient les signatures, les constantes et les décisions.

Outils :

- `tools/ghidra/Dump.java` : dump de fonctions / décompilation Ghidra ;
- `tools/match_clear.py` : report de la signature de `Clear` de 1.15.18 vers
  1.15.23 (et extraction du masque) ;
- `tools/gen_grow.py` : même méthode pour `RingBuffer::Grow` ;
- `tools/crash_dump.py` : parseur de minidump (modules, piles, exception,
  symbolisation par FunctionTable), utilisé pour le crash au shutdown.

> Les scripts `find_catalogue.py`, `gen_sigs*.py`, `compare_builds.py`,
> `read_const.py`, `match_funcs.py`, `match_helpers.py` et `binparse*.py`
> mentionnés dans certains plans de travail **n'existent pas** dans le dépôt
> StormFabric au moment de la copie. Les scripts réellement présents et liés au
> jet sont ceux listés ci-dessus.

---

## 3. Site de sur-régime

### 3.1 RVA

| Build | RVA du site | Occurrences |
|---|---:|---:|
| 1.15.23 | `0x35352a` | 1 |
| 1.15.18 | `0x35428a` | 1 |

Le site est dans une section exécutable non discardable. `FindSignature()` le
retrouve sans RVA codée en dur ; les RVA ci-dessus servent de témoin de
validation.

### 3.2 Signature (32 octets)

```text
F3 0F 10 87 BC 01 00 00   movss xmm0,[rdi+0x1bc]      ; RPS interne
F3 0F 10 35 ?? ?? ?? ??   movss xmm6,[rip+rel32]      ; 200.0f (rel32 joker)
0F 2F C6                  comiss xmm0,xmm6
0F 86 ?? ?? ?? ??         jbe  rel32                   ; rel32 joker
44 38 BF D9 04 00 00      cmp  byte [rdi+0x4d9],0     ; garde explosion
```

Offsets des jokers dans la signature :

| Offset | Taille | Champ |
|---:|---:|---|
| `12` | 4 | `rel32` du `movss` vers la constante |
| `20` | 4 | `rel32` du `jbe` |

`kMovssConstOperand = 12`, `kJbeOperand = 20`, `kSiteSigSize = 32`,
`kPatchSize = 19`.

### 3.3 Résolution de la constante

```cpp
int32_t rel = 0;
memcpy(&rel, site + 12, 4);
memcpy(&constBits, site + 16 + rel, 4);   // rip après le movss (8 octets)
```

La validation sémantique est faite par l'appelant :
`constBits == 0x43480000` (`200.0f`). Une constante différente → aucun patch.

### 3.4 Champs

| Expression | Offset | Rôle |
|---|---:|---|
| `this + 0x58` | `0x58` | pointeur vers la définition |
| `this + 0x1bc` | `0x1bc` | RPS interne (float) |
| `this + 0x4d9` | `0x4d9` | garde « déjà explosé » (byte) |
| `def + 0x2b4` | `0x2b4` | `value` (uint32) — identité |
| `def + 0xed4` | `0xed4` | `jet_engine_component_type` (int) |

Les offsets de champs sont **identiques** sur 1.15.23 et 1.15.18.

### 3.5 Fenêtre et stub

- Fenêtre patchée : 19 octets (`movss` 8 + `movss` 8 + `comiss` 3).
- Patch : `E8 <rel32>` (5) + 14 × `90`.
- Le `jbe` original (6 octets, à `site+19` dans le flux) reste intact.
- Stub : `0x3A` octets utiles, bloc `0x68`, constantes à `0x3c` (`+inf`) et
  `0x40` (`200.0f`).
- Allocation : ±2 Go, pas de 4 Mo, `PAGE_EXECUTE_READWRITE`.

Détail complet du stub : `docs/FONCTIONNEMENT.md` §3.4.

---

## 4. Catalogue

### 4.1 RVA utilisées

| Rôle | 1.15.23 | 1.15.18 | Taille signature |
|---|---:|---:|---:|
| `FindComponentDefinitionByName` | `0x46e900` | `0x46f380` | 64 |
| `ComponentRegistry::Clear` | `0x5f1ac0` | `0x5f1fd0` | 64 |
| Enregistrement de localisation | `0x1f3b30` | `0x1f5610` | 72 |
| `RingBuffer::Grow` | `0x8de0b0` | `0x8dd150` | 64 |

La détection de build (`JetCatalogueBuild`) teste la signature de
`FindComponentDefinitionByName` aux deux RVA :

```cpp
bool okNew = SigMatches(base + 0x46e900, kFindDefBytes, kFindDefMask, 64);
bool okOld = SigMatches(base + 0x46f380, kFindDefBytes, kFindDefMask, 64);
if (okNew == okOld) return 0;          // ni l'un ni l'autre, ou les deux
return okNew ? 2 : 1;                  // 2 = 1.15.23, 1 = 1.15.18
```

Les autres signatures sont validées avec la même fonction `SigMatches` (masque
octet par octet, `__try/__except` pour ne jamais fauter sur une lecture
invalide).

### 4.2 Ancien chemin d'injection (historique)

Ces RVA sont documentées dans `docs/jet_exhaust_unlimited.md` et ont servi à
l'injection par fichier `.bin`. Le code actuel clone la définition vanilla en
mémoire et ne les appelle plus.

| Rôle | 1.15.23 | 1.15.18 |
|---|---:|---:|
| Chargeur de définitions/composants | `0x46c180` | `0x46cf70` |
| Fabrique depuis un `.bin` | `0x314ce0` | `0x315a90` |
| Anti-doublon | `0x46e870` | `0x46f2f0` |
| Enregistrement au catalogue | `0x46ec30` | `0x46f6b0` |

### 4.3 Disposition d'une définition

| Élément | 1.15.23 | 1.15.18 |
|---|---:|---:|
| Taille d'une variante | `0x2d08` | `0x2848` |
| Dernier champ détruit | `0x2d00` | `0x2840` |
| Nombre de variantes | 8 | 8 |

Preuves pour `0x2d08` en 1.15.23 :

- `0x46EEC0` : `mov ecx,0x2d08 ; call 0x958C28` (operator new d'une définition) ;
- `0x8B9FEE` : `mov edx,0x2d08 ; call 0x95881C` (operator delete(ptr,size) dans
  le destructeur du ring des variantes) ;
- `0x87FDB0` : destructeur de définition, libère des champs jusqu'à `+0x2D00`.

Appliquer `0x2848` à 1.15.23 fait empiéter chaque variante de `0x4C0` octets :
détruire la variante *k* libère des champs de la variante *k+1* (double free),
puis relit une paire `(len,cap)` comme un pointeur
(`0x0000002000000000` → `str_free` lit `[rcx-8]` = `0x0000001FFFFFFFF8`).
C'est le crash au shutdown analysé par `tools/crash_dump.py`.

### 4.4 Champs d'une définition

Largeurs vérifiées sur 1.15.23 (instructions à l'appui dans
`docs/LAYOUT_DEFINITION.md`) : `flags` fait **8 octets** (`mov [r15],rax` à
`0x1408efb48`), `value` et `category` 4 octets, `id`/`name` sont des
`string_t {ptr 8, len 4, cap 4}`. La table ci-dessous est la source de vérité
recopiée dans `kDefinitionLayout` (`src/jet_catalogue.h`), avec `static_assert`.

| Offset | Type | Rôle |
|---:|---|---|
| `+0x40` | `uint8_t` | index de variante |
| `+0x288` | `char*` | id |
| `+0x290` | `uint32_t` | longueur de l'id |
| `+0x294` | `uint32_t` | capacité de l'id |
| `+0x298` | `char*` | nom |
| `+0x2a0` | `uint32_t` | longueur du nom |
| `+0x2a4` | `uint32_t` | capacité du nom |
| `+0x2a8` | `int32_t` | catégorie |
| `+0x2ac` | `int32_t` | champ voisin, non utilisé par le clone |
| `+0x2b0` | `float` | `mass`, non utilisé par le clone |
| `+0x2b4` | `uint32_t` | `value` (prix) |
| `+0x2b8` | `uint64_t` | flags (8 octets, pas 4) |

### 4.5 Rings du registre

| Offset | Ring | Élément |
|---:|---|---|
| `+0xc8` | variantes | `ComponentDefinition*` |
| `+0xe0` | descripteurs XML | structure de 32 octets |
| `+0xf8` | global | `ComponentDefinition*` (variante 0) |
| `+0x110 + cat*0x18` | buckets | `ComponentDefinition*` |

Un ring est `{buf +0, cap +8, head +12, count +16}` (24 octets). `head` peut
être hors bornes ; il est normalisé modulo `cap`.

Le descripteur XML de 32 octets est :

| Offset | Type | Rôle |
|---:|---|---|
| `+0x00` | `char*` | id |
| `+0x08` | `uint32_t` | longueur |
| `+0x0c` | `uint32_t` | capacité |
| `+0x10` | `char*` | XML |
| `+0x18` | `uint32_t` | longueur |
| `+0x1c` | `uint32_t` | capacité |

---

## 5. Signatures du catalogue

Les tableaux complets `kFindDefBytes`, `kFindDefMask`, `kClearBytes`,
`kClearMask`, `kLocBytes`, `kLocMask`, `kGrowBytes` et `kGrowMask` sont dans
`src/jet_catalogue.cpp`. Ils sont reproduits en annexe A pour référence.

Règles de masque :

- `0xFF` : octet fixe, comparé ;
- `0x00` : octet joker (rel32, disp32, adresse absolue, décalage de saut).
- `SigMatches()` est protégé par `__try/__except` : une lecture invalide
  renvoie `false` au lieu de fauter.

---

## 6. Signature du composant

L'identité runtime n'est pas un fichier `mod.sig` : c'est la constante
`500123` (`kSignatureValue`), écrite dans le champ `value` de la définition.

- XML : `value="500123"` ;
- clone : `*(uint32_t*)(dst + 0x2b4) = 500123` ;
- stub : `cmp dword [rax+0x2b4], 500123`.

Le champ `value` est aussi le prix du composant. C'est assumé : il sert
d'identité stable, sans comparaison de chaîne par tick.

---

## 7. Crash au shutdown (2026-11-08)

Symptôme : `AV 0xC0000005` lecture de `0x1FFFFFFFF8` à
`stormworks64.exe+0x9B2D5D`, dans la libération de chaîne du moteur.

Chaîne causale :

1. taille de variante `0x2848` codée en dur sur un build 1.15.23 (`0x2d08`) ;
2. les 8 variantes du clone se chevauchent de `0x4C0` octets ;
3. le destructeur moteur détruit la variante *k*, ce qui libère des champs de la
   variante *k+1* ;
4. double free, puis relecture de `(len,cap)` comme pointeur
   (`0x0000002000000000`) ;
5. `str_free` lit `[rcx-8]` = `0x0000001FFFFFFFF8` → AV.

Correctifs :

- `JetCatalogueDefinitionBytes(build)` choisit `0x2d08` ou `0x2848` ;
- `JetCatalogueCloneGeometry` vérifie que le dernier champ détruit tient dans une
  variante ;
- `ShutdownJetCatalogueInjection` retire nos entrées des rings avant `Clear` ;
- `RemoveOursFromRing` compacte correctement (buffer temporaire ou échange avec
  la dernière entrée) pour ne pas dupliquer une entrée quand `head != 0`.

Le parseur `tools/crash_dump.py` permet de rejouer l'analyse : modules, piles,
exception, mémoire et symbolisation par FunctionTable.

---

## 8. Limites de la certification

- Seuls les deux SHA-256 ci-dessus sont certifiés. Un binaire différent peut
  contenir les mêmes motifs d'octets mais une sémantique différente : le loader
  exige aussi `200.0f` au site et les signatures de catalogue.
- La signification exacte des flags `state[0x35]` / `state[0x4a]` (probablement
  dégâts/explosions) n'est pas nécessaire au stub : il ne les modifie pas.
- Le multijoueur n'est pas certifié : une définition additionnelle peut
  désynchroniser des clients vanilla.
- Les adresses de fonctions historiques (section 4.2) ne sont plus utilisées par
  le code actuel ; ne pas les réintroduire sans RE complète.

---

## Annexe A — Tableaux de signatures

```cpp
constexpr uint8_t kFindDefBytes[64] = {0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4C,0x24,0x08,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x58,0x48,0x8B,0xF2,0x4C,0x8B,0xC1,0x33,0xED,0x8B,0xFD,0x89,0x6C,0x24,0x20,0x44,0x8B,0xFD,0x8B,0x81,0x08,0x01,0x00,0x00,0x89,0x84,0x24,0xB0,0x00,0x00,0x00,0x0F,0x1F,0x84,0x00,0x00,0x00,0x00,0x00};
constexpr uint8_t kFindDefMask[64] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF};
constexpr uint8_t kClearBytes[64] = {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x4C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x81,0xC1,0x10,0x01,0x00,0x00,0x4C,0x8D,0x0D,0x00,0x6D,0x2A,0x00,0xBA,0x18,0x00,0x00,0x00,0x44,0x8D,0x42,0xF8,0xE8,0xFA,0x6F,0x36,0x00,0x90,0x33,0xFF,0x39,0xBB,0x00,0x01,0x00,0x00,0x74,0x19,0x48,0x8B,0x8B,0xF8,0x00,0x00,0x00};
constexpr uint8_t kClearMask[64] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF};
constexpr uint8_t kLocBytes[72] = {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xDA,0x4C,0x8B,0xF1,0xE8,0x6D,0xFE,0xFF,0xFF,0x48,0x8D,0xB8,0x48,0xFF,0x00,0x00,0x8B,0x4F,0x10,0x44,0x8B,0x47,0x08,0x41,0x3B,0xC8,0x75,0x1D,0x43,0x8D,0x14,0x00,0xB8,0x20,0x00,0x00,0x00,0x3B,0xD0,0x0F,0x42,0xD0,0x48,0x8B,0xCF,0xE8};
constexpr uint8_t kLocMask[72] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
constexpr uint8_t kGrowBytes[64] = {0x40,0x56,0x57,0x48,0x83,0xEC,0x28,0x48,0x8B,0xF9,0x8B,0xF2,0x3B,0x51,0x08,0x0F,0x84,0xE1,0x00,0x00,0x00,0x48,0x89,0x6C,0x24,0x48,0x85,0xD2,0x0F,0x84,0xBF,0x00,0x00,0x00,0x8B,0xCE,0xBA,0x10,0x00,0x00,0x00,0x48,0xC1,0xE1,0x03,0xE8,0x8A,0x4C,0x0D,0x00,0x48,0x8B,0xE8,0x48,0x8B,0x07,0x48,0x85,0xC0,0x0F,0x84,0xAA,0x00,0x00};
constexpr uint8_t kGrowMask[64] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
```
