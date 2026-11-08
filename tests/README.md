# Tests du jet

Les trois fichiers de ce dossier sont des **copies de référence** des tests du
loader StormFabric :

- `jet_patch_tests.cpp` : site de sur-régime, constante `200.0f`, encodage et
  exécution du stub, patch/rollback, matérialisation historique ;
- `jet_catalogue_tests.cpp` : signatures de catalogue, saut absolu, croissance de
  ring, géométrie du clone, cohérence des chaînes, injection à la demande ;
- `jet_test_sigs.h` : déclarations des internes de `src/jet_patch.cpp` pour les
  tests.

Ils ne sont **pas compilables seuls** : ils incluent des en-têtes du loader
(`mods_scan.h`, `game.h`, `log.h`, `test_temp.h`). La compilation se fait dans le
checkout StormFabric, qui est la source de vérité.

---

## Lancer les tests

### Suite complète

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

### Tests ciblés

```bat
cd /d "H:\Bordel 4TO NVME\StormFabric"

build\tests\jet_patch_tests.exe ^
  "H:\Jeux\StormworksV1.15.23\Stormworks\stormworks64.exe"

build\tests\jet_catalogue_tests.exe ^
  "H:\Jeux\StormworksV1.15.23\Stormworks\stormworks64.exe" ^
  "H:\Jeux\Stormworks\stormworks64.exe"
```

Ordre des arguments de `jet_catalogue_tests.exe` : **1.15.23** (`build == 2`)
puis **1.15.18** (`build == 1`). Sans argument, les contrôles sur binaire réel
sont SKIP ; les tests purs restent exécutés.

### Recompiler uniquement les tests du jet

Depuis StormFabric (invite de commandes avec `vcvars64.bat` chargé) :

```bat
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\jet_patch_tests.cpp src\jet_patch.cpp src\mods_scan.cpp src\log.cpp ^
   /Fo:build\tests\ /Fe:build\tests\jet_patch_tests.exe /link shell32.lib

cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tests\jet_catalogue_tests.cpp src\jet_catalogue.cpp src\mods_scan.cpp ^
   /Fo:build\tests\ /Fe:build\tests\jet_catalogue_tests.exe
```

---

## Couverture

### `jet_patch_tests.cpp`

| Test | Ce qui est prouvé |
|---|---|
| `TestSignature` | occurrence unique, absente, ambiguë, zone courte, jokers `rel32` |
| `TestConstantResolution` | lecture de la constante via le `rel32` du `movss` |
| `TestStubEncoding` | registres, garde de définition nulle, `value=500123`, `rip`-relatifs, `comiss` final |
| `TestStubExecution` | exécution réelle en page RWX : vanilla 250/100 RPS et unlimited 10 000/`1e30`/`+inf` exécutés, définition nulle ; assertions fortes sur `1e30`, `+inf` et la définition nulle |
| `TestInstallRollback` | `CALL rel32` + 14 NOP, sauvegarde, restauration, refus si site modifié |
| `TestSafeNames` | traversée `..`, séparateurs, extensions |
| `TestManifestParsing` | `inject_definitions` et `inject_components` dans `mod.json` |
| `TestReconcile` | matérialisation, rafraîchissement, retrait (mécanique historique) |
| `TestModAssetsLayout` | `mod.xml` + `data/definitions` + `data/components`, dossiers tiers intacts |
| `CheckRealBinary` | une occurrence du site et constante `0x43480000` |

### `jet_catalogue_tests.cpp`

| Test | Ce qui est prouvé |
|---|---|
| `TestJumpEncoding` | `FF 25 00000000 <imm64>` + NOP, `disp32=0`, refus des tailles hors bornes |
| `TestRingGrowth` | `cap=0` allouable, `cap>0` plein jamais réalloué |
| `TestCloneGeometry` | `0x2d08` par variante en 1.15.23, chevauchement `0x2848` détecté, cohérence 1.15.18 |
| `TestEngineStringCoherence` | `{ptr,len,cap}` : `cap>=32`, `len<=cap`, pointeur, valeur fautive du dump |
| `TestLazyInjectionDecision` | `kNotOurs` / `kPresent` / `kInject`, réinjection après `Clear` |
| `TestNeedsInjectionDecision` | registre déjà servi, second registre, après `Clear` |
| `TestLazyInjectionOutcome` | `kSourceMissing` / `kRefused` / `kInjected`, textes journalisés |
| `TestSignaturesOnRealBinaries` | build 2 puis 1, refus si un octet est altéré |

---

## Note sur `kStubBlockBytes`

`jet_test_sigs.h` déclare `kStubBlockBytes = 0x40`. Le bloc de production fait
`0x68` et `BuildJetStub` écrit deux floats à `0x3c` et `0x40`. Le tableau du test
déborde donc de 4 octets sur la pile ; le test passe sur MSVC mais cette
constante devrait être alignée à `0x68` lors d'une prochaine resynchronisation.
La production n'est pas affectée.
