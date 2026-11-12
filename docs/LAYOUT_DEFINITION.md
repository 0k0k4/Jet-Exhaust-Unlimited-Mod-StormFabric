# Disposition d'une `ComponentDefinition` (Stormworks 1.15.23) — preuves

Ce document fige les **offsets et largeurs** des champs que le clone du mod
écrit. Il répond à une question précise : `flags` (`+0x2b8`) fait-il 4 ou
8 octets ?

**Conclusion : 8 octets.** Le mod écrit déjà `uint64_t` ; ce document et les
`static_assert` de `src/jet_catalogue.h` empêchent une régression vers un store
32 bits.

## Méthode

Analyse statique de `stormworks64.exe` (1.15.23, base d'image `0x140000000`)
avec Ghidra 12.1.4 headless, projet `C:\Users\cedri\ghidra_stormwork_new`,
script `StormFabric/tools/ghidra/Dump.java` (commandes `A`/`I`/`D`).
Les adresses ci-dessous sont des **adresses absolues** (RVA + `0x140000000`).

## Tableau certifié

| Champ | Offset | Largeur | Preuve (instruction moteur) |
|---|---:|---:|---|
| index de variante | `+0x40` | 1 | `uint8_t` écrit par le clone ; non relu par le moteur dans les sites vérifiés |
| `id` pointeur | `+0x288` | 8 | `0x14046ed20` : `FUN_14017cbb0(pcVar5+0x288, len)` écrit le pointeur ; `0x14046e550` : `MOV RDX,[RBX+0x288]` |
| `id` longueur | `+0x290` | 4 | `0x14046ed20` : `MOV dword ptr [pcVar5+0x290], len` ; `0x14046e550` : `MOV [..+0x290]` (uint32) |
| `id` capacité | `+0x294` | 4 | `FUN_14017cbb0` : `MOV dword ptr [RDI+0xc], EBX` (string_t = `{ptr,len,cap}`) |
| `name` pointeur | `+0x298` | 8 | `0x140190c40` : `MOV RCX,R14` puis `FUN_14017cbb0(R14, len)` ; `R14 = &def->name` |
| `name` longueur | `+0x2a0` | 4 | `0x140190c40` : `MOV dword ptr [R14+0x8], EAX` |
| `name` capacité | `+0x2a4` | 4 | `FUN_14017cbb0` : `MOV dword ptr [RDI+0xc], EBX` |
| `category` | `+0x2a8` | 4 | `0x14093d1d0` : `MOV dword ptr [R15], EAX` ; lecture `0x14046e7e4` : `MOVSXD RAX,dword ptr [RSI+0x2a8]` |
| champ voisin (non utilisé) | `+0x2ac` | 4 | `0x14093d410` : `MOV dword ptr [R15], EAX` |
| `mass` (non utilisé) | `+0x2b0` | 4 | `0x1408efd50` : `MOVSS dword ptr [R15], XMM1` |
| `value` | `+0x2b4` | 4 | `0x1408ef630` : `MOV dword ptr [R15], EAX` (après `CALL 0x1409b3b94`) |
| **`flags`** | **`+0x2b8`** | **8** | **écriture `0x1408efaf0` : `MOV qword ptr [R15], RAX` à `0x1408efb48`** |
| fin du champ `flags` | `+0x2c0` | — | le champ suivant nommé (`reward_tier`) est à `+0x2e0` |

## Preuves indépendantes pour `flags`

### 1. Écriture — helper de parsing `0x1408efaf0`

```
1408efb43  CALL 0x1409b3b94          ; chaîne XML -> entier 64 bits (RAX)
1408efb48  MOV qword ptr [R15],RAX   ; store 8 octets dans def->flags
```

`R15` reçoit `R9`, c'est-à-dire le pointeur `&this->flags` passé par
`0x1403168d0` (`LEA R9,[RSI+0x2b8]` à `0x140316b26`). Le store est **64 bits**.

### 2. Lecture 64 bits — enregistrement dans le bucket `0x14046ec30`

```
14046ec9c  MOV RAX,qword ptr [RBX+0x2b8]   ; lecture 8 octets
14046eca3  TEST RAX,0x80
14046ecab  TEST RAX,0x800
```

C'est le site qui décide si la définition entre dans le bucket de catégorie
(les bits `0x80`/`0x800` la font sauter). Il lit **8 octets**.

### 3. Lecture 32 bits du dword bas — `ComponentRegistry::Clear` `0x14046e750`

```
14046e7d6  MOV EAX,dword ptr [RSI+0x2b8]   ; dword bas seulement
14046e7dc  TEST RAX,0x20000000
```

Le moteur teste un bit du **dword bas** avec une lecture 32 bits. C'est
cohérent avec un champ 64 bits dont la valeur utile tient dans les 32 bits bas :
les deux sites lisent la même valeur, à des largeurs différentes.

### 4. `value` juste avant est bien 32 bits

```
1408ef683  CALL 0x1409b3b94
1408ef688  MOV dword ptr [R15],EAX   ; value = 4 octets
```

`value` (`+0x2b4`) et `flags` (`+0x2b8`) sont donc deux champs distincts et
contigus : `value` sur 4 octets, `flags` sur 8.

## Valeur attendue

La définition vanilla `rom/data/definitions/jet_engine_combustion_chamber.xml`
porte `flags="16384"` = `0x4000`. Les octets hauts du champ 64 bits sont donc
nuls ; le clone recopie néanmoins la valeur **64 bits** complète
(`info.flags & ~0x880`), pas seulement son dword bas.

## Conséquence pour le clone

- `flags` doit être écrit avec un store 64 bits. Un store 32 bits n'écraserait
  pas le voisin (`+0x2bc` reste dans le champ), mais laisserait les 4 octets
  hauts dans leur état précédent — une valeur 64 bits non nulle serait tronquée.
- `value` et `category` restent des stores 32 bits.
- `id`/`name` sont des `string_t` `{ptr 8, len 4, cap 4}`.

Le test `TestCloneFieldWidths` (`tests/mod_logic_tests.cpp`) remplit un buffer
de `0x2d08` octets avec une sentinelle, écrit les champs via
`JetCatalogueWriteCloneFields`, puis vérifie que les 8 octets de `flags` sont
écrits, que `+0x2c0` n'est pas touché, et qu'un store 32 bits laisserait bien
`+0x2bc..+0x2bf` à la sentinelle.
