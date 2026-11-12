# Changelog — Jet Exhaust Unlimited

Format : [Keep a Changelog](https://keepachangelog.com/fr/1.1.0/).
Version du mod : `2.0.0` (celle de `mod/jet_unlimited/mod.json`).

Le mod est **autonome** : tout le code de hook est compilé dans
`jet_unlimited.dll` (sources `src/jet_*.cpp`). Le loader StormFabric ne contient
plus aucun code propre à ce mod ; il expose des primitives natives génériques.

---

## [Unreleased]

### Vérifié / durci

- **Largeur de `flags` (`+0x2b8`) certifiée à 8 octets** sur 1.15.23 :
  écriture `mov qword ptr [r15],rax` (`0x1408efb48`), lecture 64 bits dans
  l'enregistrement de bucket (`0x14046ec9c`), lecture 32 bits du dword bas dans
  `ComponentRegistry::Clear` (`0x14046e7d6`). Preuves et instructions dans
  `docs/LAYOUT_DEFINITION.md`.
- Disposition des champs du clone centralisée dans `kDefinitionLayout`
  (`src/jet_catalogue.h`) avec `static_assert` sur chaque offset/largeur ;
  l'écriture des champs passe par `JetCatalogueWriteCloneFields` (pure).
- Test `TestCloneFieldWidths` : sentinelle sur un buffer de `0x2d08` octets,
  vérifie que `flags` écrit bien 8 octets, que `+0x2c0` reste intact et qu'un
  store 32 bits laisserait `+0x2bc..+0x2bf` inchangés.

## [2.0.0] — 2026-11-09

### Architecture

- **Le mod est autonome.** `jet_unlimited.dll` porte l'injection catalogue et le
  patch de sur-régime. Le loader ne contient plus aucune référence au composant.
- Le mod n'utilise que l'API native générique du loader (extension `api->native`) :
  `findSymbol` (rôles moteur résolus par le profil de découverte),
  `findSignature` (signature propre au mod, occurrence unique),
  `readMemory`/`writeMemory` (bornés, protection restaurée),
  `hookInline`/`hookRestore` (contrat d'octets, trampoline loader, restauration
  idempotente), `gameAlloc`, `stringMake`/`stringFree`, `codeAllocNear`,
  `engineBuild`.
- Plus aucune RVA d'exécutable dans le mod : les services moteur viennent du
  profil du loader ; le site du sur-régime vient d'une signature validée.
- Les hooks sont restaurés avant le déchargement de la DLL (`SwModUnload` puis
  `DllMain(PROCESS_DETACH)`), dans l'ordre inverse de l'installation.

### Ajouté

- `build_mod.bat` : compilation du mod contre le SDK StormFabric, produit
  `mod/jet_unlimited/jet_unlimited.dll`.
- `tests/run_tests.bat` + `tests/mod_logic_tests.cpp` : tests hors jeu autonomes
  (masque de signature, stub machine, croissance de ring, injection à la demande,
  géométrie du clone, chaînes, saut absolu).

### Supprimé

- `tools/sync_to_stormfabric.bat` : plus nécessaire, le mod ne copie plus ses
  sources dans le loader.
- Les copies de tests dépendantes des internes du loader.

### Corrigé

- Le composant **reste sur le véhicule après un aller-retour à l'atelier** :
  l'injection est déclenchée à la demande dans le hook de lookup et ré-armée
  après `ComponentRegistry::Clear`, sans sondage temporisé.

---

## [1.2.0] — 2026-11-08

### Ajouté

- Dépôt autonome du mod : documentation, assets, copies de référence du code de
  hook et des tests.
- `docs/FONCTIONNEMENT.md` : architecture complète (catalogue + patch).
- `docs/REVERSE_ENGINEERING.md` : RVA des deux builds, signatures, offsets,
  méthode et analyse du crash au shutdown.
- `docs/INSTALLATION.md` : installation, vérification, désactivation,
  désinstallation.
- `tools/sync_to_stormfabric.bat` : resynchronisation des 4 fichiers source et
  des 3 fichiers de test vers un checkout StormFabric.
- `tools/README.md` : portée des scripts d'analyse réellement présents.

### Corrigé (côté loader, repris dans cette copie)

- **Injection à la demande** : le hook de `FindComponentDefinitionByName`
  réinjecte au premier lookup, après `ComponentRegistry::Clear` ou dans un second
  registre. Corrige la disparition du composant lors de la désérialisation d'un
  véhicule.
- **Croissance de ring** : un ring `cap=0,count=0` (jamais alloué) est désormais
  alloué au lieu d'être confondu avec un ring plein. Le `realloc` d'un ring
  réellement plein reste interdit dans une frame de jeu.
- **Géométrie du clone** : taille de variante choisie par build (`0x2d08` en
  1.15.23, `0x2848` en 1.15.18) et vérification que le dernier champ détruit
  (`+0x2d00` / `+0x2840`) tient dans la variante. Corrige le crash au shutdown
  par chevauchement/double free.
- **Démontage** : retrait de nos entrées des rings avant `Clear`, restauration
  des prologues, conservation du stub si la restauration échoue.
- **Stub de sur-régime** : limite `+inf` au lieu de `1e30f`, pour couvrir `+inf`
  et les NaN ; chargement du RPS dans `XMM0` (et non `XMM1`) pour que le jeu
  relise la bonne valeur ; `comiss` final pour le `jbe` original.
- **Patch transactionnel** : vérification des 19 octets d'origine, sauvegarde,
  `CALL rel32` + 14 NOP, restauration.

### Modifié

- Le composant fonctionnel est la **chambre de combustion**
  `jet_engine_combustion_chamber_unlimited` (`value=500123`) ; le fichier
  `jet_engine_exhaust_unlimited.xml` reste une variante cosmétique.
- Le catalogue est injecté **entièrement en mémoire** : aucun fichier écrit dans
  `rom/` ni `%APPDATA%` par le chemin actif. Le code de réconciliation de
  fichiers historique reste compilé mais n'est plus appelé.
- Description de `mod.json` mise à jour pour refléter l'injection en mémoire
  (elle mentionnait encore `rom/data/components`).

### Connu

- Le champ `value=500123` est aussi le prix du composant.
- Support limité aux builds 1.15.23 et 1.15.18.
- Multijoueur non certifié.
- Commentaires historiques « bypass désactivé » dans `jet_patch.cpp` /
  `mods_ui.cpp` : la constante `kOverrevPatchEnabled` vaut `true`.
- `tests/jet_test_sigs.h` déclare `kStubBlockBytes = 0x40` alors que le bloc de
  production fait `0x68` (le test passe, mais la constante mérite d'être alignée).

---

## [1.1.0] — 2026-11-07

### Ajouté

- Injection du composant au catalogue par hooks de
  `FindComponentDefinitionByName` et `ComponentRegistry::Clear`.
- Clone de la définition vanilla en 8 variantes, avec id/nom/catégorie/value et
  descripteur XML.
- Validation des signatures de catalogue sur les deux builds (64/72 octets).
- Tests hors jeu : encodage du saut, croissance de ring, géométrie du clone,
  cohérence des chaînes, décisions d'injection à la demande.

### Corrigé

- L'injection n'est plus pilotée uniquement par les pulsations UI du menu
  principal (qui n'existent pas pendant le chargement d'un monde).
- Le registre n'est plus rejeté en silence pour un `head` hors bornes : il est
  normalisé et les refus sont journalisés.

---

## [1.0.0] — 2026-11-06

### Ajouté

- Site de sur-régime certifié sur 1.15.23 (`0x35352a`) et 1.15.18 (`0x35428a`).
- Signature d'opcodes de 32 octets, constante `200.0f` (`0x43480000`) résolue par
  `rel32`.
- Patch de 19 octets (`CALL` + NOP), stub à ±2 Go, identité par `value=500123`.
- Injection catalogue initiale et assets du mod (XML de chambre, XML
  d'échappement cosmétique, `.bin` compilé).
- Tests de signature, de stub, de patch/rollback et de matérialisation.

---

## Notes de maintenance

- Modifier d'abord StormFabric (source de vérité), compiler et tester, puis
  recopier ici (section 6 du README).
- Toute nouvelle version du jeu doit être précédée d'une RE complète : les
  signatures ne sont pas des garanties de compatibilité.
- Ne jamais patcher la constante globale `200.0f` : elle est partagée par de
  nombreuses fonctions du moteur.
