# Outils

Outils d'analyse et de synchronisation réellement présents dans le dépôt
StormFabric au moment de la copie. Ils sont fournis pour rejouer la
rétro-ingénierie et le diagnostic, pas pour être exécutés à chaque build.

| Fichier | Rôle |
|---|---|
| `ghidra/Dump.java` | script Ghidra : dump de fonctions / décompilation à partir d'un fichier de commandes |
| `crash_dump.py` | parseur de minidump Windows (MDMP) : modules, threads, piles, exception, mémoire, FunctionTable |
| `match_clear.py` | report de la signature de `ComponentRegistry::Clear` de 1.15.18 vers 1.15.23 et extraction du masque |
| `gen_grow.py` | même méthode pour `RingBuffer::Grow` |
| `sync_to_stormfabric.bat` | copie les 4 fichiers source et les 3 fichiers de test de ce dépôt vers un checkout StormFabric |

## `sync_to_stormfabric.bat`

```bat
tools\sync_to_stormfabric.bat "H:\Bordel 4TO NVME\StormFabric"
```

Le script :

- vérifie que la destination contient bien `src\jet_patch.cpp` ;
- copie `src\jet_patch.cpp`, `src\jet_patch.h`, `src\jet_catalogue.cpp`,
  `src\jet_catalogue.h` ;
- copie `tests\jet_patch_tests.cpp`, `tests\jet_catalogue_tests.cpp`,
  `tests\jet_test_sigs.h` ;
- ne touche ni aux autres modules, ni aux docs, ni aux assets.

Après exécution, compiler et tester dans StormFabric :

```bat
cmd /c ".\build.bat"
cmd /c ".\tests\run_tests.bat"
```

## Scripts absents

Les noms `find_catalogue.py`, `gen_sigs*.py`, `compare_builds.py`,
`read_const.py`, `match_funcs.py`, `match_helpers.py` et `binparse*.py`
apparaissent dans certains plans de travail, mais ces fichiers **n'existent pas**
dans `StormFabric/build/tests/` au moment de la copie. Les scripts réellement
présents et liés au jet sont ceux listés ci-dessus.

## Dépendances

Les scripts Python utilisent la bibliothèque standard (`struct`, `os`, `sys`) et
supposent les chemins des exécutables de référence :

```text
H:/Jeux/StormworksV1.15.23/Stormworks/stormworks64.exe
H:/Jeux/Stormworks/stormworks64.exe
```

Adapter ces chemins dans les scripts si votre installation diffère.
