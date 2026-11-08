# Tests du mod

`mod_logic_tests.cpp` vérifie uniquement la **logique pure** du mod : aucune
exécution de code du jeu, aucun hook réel.

Couverture :

- masque de la signature du site de sur-régime (rel32 jokers, octets fixes) ;
- encodage machine du stub (`value` en imm32, `definition+0x2b4`, constantes
  `200.0f` / `+inf`, saut absolu vers `site+19`) ;
- croissance de ring (`kAllocate` seulement pour `cap == 0`, realloc refusé) ;
- injection à la demande (`LazyPlan` / `LazyOutcome` / ré-injection après Clear) ;
- géométrie du clone (chevauchement interdit, tailles par build) ;
- cohérence des chaînes moteur ;
- encodage du saut absolu.

## Lancer

```bat
cd /d "H:\Bordel 4TO NVME\Jet Exhaust Unlimited Mod StormFabric"
cmd /c ".\tests\run_tests.bat"
```

Attendu : `All mod logic tests passed.`

Le SDK StormFabric doit être présent dans un checkout voisin
(`..\StormFabric\include`) ou passé en argument :

```bat
cmd /c ".\tests\run_tests.bat" "H:\chemin\vers\StormFabric\include"
```

Les primitives du **loader** (contrat de signature trouvé/absent/ambigu,
hook/restore, refus d'écriture) sont testées dans StormFabric :
`tests\run_tests.bat` (suite `native_prims_tests`).
