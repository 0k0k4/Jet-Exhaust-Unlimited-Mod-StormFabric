# Stormworks 64-bit Engine Function & Structure Mapping

> **Reference copy.** This reference comes from StormFabric. The values the mod
> actually uses (sizes, offsets) are the ones in `src/jet_catalogue.cpp` and
> `src/jet_patch.cpp` (engine addresses come from the loader's discovery
> profile); if they differ, the code and `docs/REVERSE_ENGINEERING.md` are
> authoritative.

This document lists the reverse-engineered functions, structures and offsets of
the `stormworks64.exe` binary (Steam x64, default base `0x140000000`).

---

## 1. General information & ABI

- **Architecture:** x86-64 (AMD64)
- **Calling convention:** Microsoft x64 Calling Convention
  - Integer/pointer parameters: `RCX`, `RDX`, `R8`, `R9`, then the stack
    (`[RSP + 0x20]`, `[RSP + 0x28]`, etc.). 32-byte (0x20) shadow space reserved
    by the caller.
  - Float parameters: `XMM0`, `XMM1`, `XMM2`, `XMM3`.
  - Non-volatile registers (preserved by the callee): `RBX`, `RBP`, `RSI`,
    `RDI`, `R12`, `R13`, `R14`, `R15`.
- **Graphics engine:** OpenGL 3.3 Core (shared context managed through
  `ResourceManager` / `graphicsContext`).

---

## 2. Key data structures

### `SwString` / `SwEngineString` (size: 16 bytes / 0x10)
Internal representation of a dynamic string in the engine.
| Offset | Type | Name | Description |
|---|---|---|---|
| `+0x00` | `const char*` | `data` | Pointer to the ASCIIZ / UTF-8 string |
| `+0x08` | `uint32_t` | `length` | Length in bytes (without the null terminator) |
| `+0x0c` | `uint32_t` | `capacity` | Allocated capacity of the buffer |

---

### `DefRingBuffer` (size: 24 bytes / 0x18)
Generic circular buffer used throughout the engine to store dynamic arrays
(definitions, voxels, surfaces).
| Offset | Type | Name | Description |
|---|---|---|---|
| `+0x00` | `void*` | `data` | Element buffer / pointer array |
| `+0x08` | `uint32_t` | `capacity` | Current maximum capacity of the buffer |
| `+0x0c` | `int32_t` | `head` | Circular head index |
| `+0x10` | `uint32_t` | `count` | Number of active elements |
| `+0x14` | `uint32_t` | `pad` | Alignment / padding |

---

### `ComponentRegistry` (`world + 0xbb670`)
Central registry of all the components in a world instance (editor or gameplay
simulation world). Holds 4 nested ring buffers:
| Offset | Type | Name | Description |
|---|---|---|---|
| `+0xc8` | `void**` | `variantDefsBuffer` | **Critical for spawning:** circular buffer of ALL the individual variants (8 per component). Used by the vehicle deserializer (`0x1404b8a50`) at spawn time to find the exact definition of the placed block (`idStr` + `variantIndex`). |
| `+0xd0` | `uint32_t` | `variantDefsCapacity` | Capacity of the variants ring buffer (e.g. 2048) |
| `+0xd4` | `int32_t` | `variantDefsHead` | Head index of the variants buffer |
| `+0xd8` | `uint32_t` | `variantDefsCount` | Total number of registered variants |
| `+0xe0` | `void**` | `xmlDescriptorsBuffer`| Circular buffer of the XML descriptors (32-byte structures `{ idStr, idLen, idCap, xmlData, xmlLen, pad }`). Queried by `FindDefinitionDescriptorByName` (`0x14046f2f0`) and `0x1404c6280` when the spawn package is built. |
| `+0xe8` | `uint32_t` | `xmlDescriptorsCapacity`| Capacity of the XML descriptors ring buffer |
| `+0xec` | `int32_t` | `xmlDescriptorsHead` | Head index of the XML descriptors |
| `+0xf0` | `uint32_t` | `xmlDescriptorsCount`| Number of registered XML descriptors |
| `+0xf8` | `void**` | `allDefsBuffer` | Circular buffer of all the components (`ComponentDefinition*` variant 0). Used by `FindComponentDefinitionByName` (`0x14046f380`). |
| `+0x100` | `uint32_t` | `allDefsCapacity` | Capacity of the global ring buffer (e.g. 1024) |
| `+0x104` | `int32_t` | `allDefsHead` | Head index |
| `+0x108` | `uint32_t` | `allDefsCount` | Total number of registered components |
| `+0x110 + cat * 24` | `DefRingBuffer` | `categoryBuckets[16]` | Circular buffers filtered by category (e.g. cat 6 = Displays, used by the catalogue UI) |

The game keeps at least two live registries; clearing one must not touch the
other (see CHANGELOG 2.0.1).

---

### `MeshResource` (size: 160 bytes / 0xa0)
Structure holding the CPU data and the GPU VBO/VAO ids of a 3D mesh.
| Offset | Type | Name | Description |
|---|---|---|---|
| `+0x00` | `uint8_t` | `isLoaded` | 1 if the mesh is initialised and ready to render |
| `+0x08` | `uint16_t*` | `indicesBuffer` | CPU buffer of the face indices (triangles) |
| `+0x10` | `uint32_t` | `indicesCount` | Number of indices (e.g. 150 for monitor_1 = 50 triangles) |
| `+0x18` | `uint8_t*` | `vertexBuffer` | CPU buffer of the vertices (28 bytes per vertex) |
| `+0x20` | `uint32_t` | `vertexCount` | Number of vertices (e.g. 84 vertices for a screen) |
| `+0x28` | `void*` | `graphicsContext` | Pointer to the engine's OpenGL graphics context |
| `+0x30` | `void*` | `gpuBuffers` | Pointer to the OpenGL VAO / VBO structure allocated on the GPU |
| `+0x38` | `void*` | `graphicsContext2` | Secondary context |
| `+0x40` | `void*` | `submeshes` | Sub-mesh buffer |
| `+0x48` | `uint32_t` | `submeshCapacity`| Capacity of the sub-mesh array |

---

### `ComponentDefinition` (size of a variant: **0x2848 on 1.15.18, 0x2d08 on 1.15.23**)
> [!WARNING]
> The size CHANGED between the two reviewed builds. 1.15.23 is certified at
> **0x2d08** by three sites in the binary: `mov ecx,0x2d08 ; call operator new`
> (`0x46EEC0`), `operator delete(ptr,0x2d08)` in the variants ring destructor
> (`0x8B9FEE`) and the fields destroyed by the definition destructor
> (`0x87FDB0`, up to `+0x2D00`). Applying 0x2848 to 1.15.23 makes each variant
> overlap the next by 0x4C0 bytes: destroying our variants then frees fields of
> the neighbour (double free + `str_free` of a `(len,cap)` pair read back as a
> pointer) → AV on shutdown. The size is now picked per build
> (`JetCatalogueDefinitionBytes`).

Full definition of a game block / component. Allocated as 8 variants (indices 0
to 7 for the 8 XYZ symmetry/mirror combinations).
| Offset | Type | Name | Description |
|---|---|---|---|
| `+0x00..+0x38` | `void*[8]` | `variants` | Array of 8 pointers to each variant (0..7) |
| `+0x40` | `uint8_t` | `variantIndex` | Index of the current variant (0 to 7) |
| `+0x48` | `DefRingBuffer` | `surfaces` | 6-face contact surfaces (oriented: Back, Front, Left, Right, Top, Bottom) |
| `+0x60` | `DefRingBuffer` | `buoyancySurfaces` | Watertight surfaces for buoyancy and fluid physics |
| `+0x78` | `DefRingBuffer` | `logicNodes` | Logic nodes (Composite, Video, Number, Boolean, Electric) |
| `+0xa8` | `DefRingBuffer` | `voxels` | Circular buffer of the physical voxels (`VoxelEntry`, 60 bytes per voxel) |
| `+0xc0` | `int32_t[3]` | `voxelMin` | Minimum voxel coordinates `[minX, minY, minZ]` |
| `+0xcc` | `int32_t[3]` | `voxelMax` | Maximum voxel coordinates `[maxX, maxY, maxZ]` |
| `+0xd8` | `int32_t[3]` | `physicsMin` | Minimum physical bounding box |
| `+0xe4` | `int32_t[3]` | `physicsMax` | Maximum physical bounding box |
| `+0x110` | `float[3]` | `centerOfMass` | Centre of mass `[cx, cy, cz]` in metres |
| `+0x288` | `const char*` | `idStr` | Unique technical id (e.g. `"swml_cd_16x2"`, `"monitor_1"`) |
| `+0x290` | `uint32_t` | `idLen` | Length of the id |
| `+0x298` | `const char*` | `nameStr` | UI display name (e.g. `"Custom Display 16x2"`) |
| `+0x2a0` | `uint32_t` | `nameLen` | Length of the display name |
| `+0x2a8` | `int32_t` | `category` | Component category (6 = Displays) |
| `+0x2ac` | `int32_t` | `type` | Internal factory type (43 = `ComponentMonitor`) |
| `+0x2b0` | `float` | `mass` | Physical mass of the block (e.g. 4.0f * W * H) |
| `+0x2b4` | `int32_t` | `price` | Price in game money (e.g. 500 * W * H) |
| `+0x2b8` | `uint64_t` | `flags` | **Critical:** for a display clone it must be `0`. If bit 7 (0x80) is set, the virtual XML destructor runs on delete and crashes! (The jet clone keeps the source flags minus `0x80`/`0x800`.) |
| `+0x340` | `float[3]` | `constraintOffset`| Physical constraint point |
| `+0x3a0` | `MeshResource` | `mainMesh` | Main 3D mesh shown in game (OpenGL VBO/VAO) |
| `+0x440` | `MeshResource` | `meshSlot1` | Second mesh slot |
| `+0x4e0` | `MeshResource` | `meshSlot2` | Third mesh slot |
| `+0x580` | `MeshResource` | `meshSlot3` | Fourth mesh slot |
| `+0x620` | `MeshResource` | `editorArrowMesh` | Orientation arrow mesh in the editor |
| `+0x6c0` | `MeshResource` | `meshSlot5` | Sixth mesh slot |
| `+0x760` | `const char*` | `meshDataName` | Path to the mesh file (e.g. `"rom/meshes/component_screen_1.mesh"`) |
| `+0x790` | `const char*` | `meshEditorName` | Path to the editor mesh (e.g. `"rom/meshes/arrow_screen_1.mesh"`) |

---

### `VoxelEntry` (size: 60 bytes / 0x3c)
A cubic voxel of a component (one voxel = 0.25m x 0.25m x 0.25m).
| Offset | Type | Name | Description |
|---|---|---|---|
| `+0x00` | `int32_t` | `x` | Relative X coordinate in blocks |
| `+0x04` | `int32_t` | `y` | Relative Y coordinate in blocks |
| `+0x08` | `int32_t` | `z` | Relative Z coordinate in blocks |
| `+0x0c` | `uint32_t` | `flags` | 1 = active physical voxel |
| `+0x10` | `uint32_t` | `physicsShape` | Physical shape (0 = full cube) |
| `+0x14` | `int32_t[9]` | `rot` | 3x3 integer rotation matrix (identity `{1,0,0, 0,1,0, 0,0,1}`) |
| `+0x38` | `uint32_t` | `buoyPipes` | Pipes and internal watertightness |

---

## 3. Engine function directory (symbol mapping)

### Definition system & registry

#### `FindComponentDefinitionByName`
- **RVA:** `0x46f380` | **VA:** `0x14046f380`
- **Signature:**
  ```cpp
  ComponentDefinition* FindComponentDefinitionByName(void* registry, const SwEngineString* name);
  ```
- **Parameters:**
  - `RCX`: `registry` — pointer to the component registry (`world + 0xbb670`).
  - `RDX`: `name` — pointer to the `SwEngineString` holding the id looked up.
- **Return:** `RAX` — pointer to variant 0 of the component, or `nullptr` if not
  found.
- **Description:**
  Walks the global circular buffer (`registry + 0xf8`). Compares the name with
  `def->idStr` (`+0x288`). Used by 35 engine systems: vehicle spawner, editor,
  save loading, network, Lua.

#### `FindDefinitionDescriptorByName`
- **RVA:** `0x46f2f0` | **VA:** `0x14046f2f0`
- **Signature:**
  ```cpp
  DefinitionFileDescriptor* FindDefinitionDescriptorByName(void* registry, const SwEngineString* name);
  ```
- **Parameters:**
  - `RCX`: `registry` — pointer to the component registry (`world + 0xbb670`).
  - `RDX`: `name` — pointer to an `SwEngineString` (`data` and `length`).
- **Return:** `RAX` — pointer to the 32-byte descriptor structure
  (`+0x00`: idStr, `+0x08`: idLen, `+0x10`: xmlData, `+0x18`: xmlLen), or
  `nullptr`.
- **Description:**
  Walks the XML descriptors circular buffer (`registry + 0xe0`). Used by the
  spawn packager (`0x1404c6280`) and the network subsystems to get the raw XML
  definitions. Hooked by SWML to fill in the descriptors of screens created in
  memory.

---

### Vehicle spawn & life cycle

#### `VehicleComponent::DeserializeBlocks`
- **RVA:** `0x4b8a50` | **VA:** `0x1404b8a50`
- **Signature:**
  ```cpp
  void DeserializeVehicleComponents(void* body, void** registryPtr, void* xmlReader, void* outComponents, int bodyIndex);
  ```
- **Parameters:**
  - `RCX`: `body` — vehicle body instance (`[rcx + 0x70]` = `world`).
  - `RDX`: `registryPtr` — pointer to `ComponentRegistry*` (`*registryPtr` =
    `ComponentRegistry`).
  - `R8`: `xmlReader` — XML reader of the vehicle file.
  - `R9`: `outComponents` — array receiving the instantiated components.
  - `[RSP + 0x28]`: `bodyIndex` — index of the physical sub-body.
- **Description & assembly analysis:**
  The heart of vehicle instantiation at spawn and load time. Walks the
  vehicle's `<c d="...">` nodes (components) and looks each variant up directly
  in the registry's variants ring buffer: `registry + 0xc8`.
  ```x86asm
  48 8B 4D 48          mov rcx, [rbp + 0x48]  ; rcx = registryPtr
  48 8B 01             mov rax, [rcx]         ; rax = *registryPtr (ComponentRegistry)
  8B 90 D8 00 00 00    mov edx, [rax + 0xd8]  ; variantDefsCount
  8B 88 D0 00 00 00    mov ecx, [rax + 0xd0]  ; variantDefsCapacity
  4C 8B 80 C8 00 00 00 mov r8, [rax + 0xc8]   ; variantDefsBuffer
  44 8B B8 D4 00 00 00 mov r15d, [rax + 0xd4] ; variantDefsHead
  ; Loop over every variant of the ring buffer:
  49 8B 3C D0          mov rdi, [r8 + rdx * 8]  ; rdi = candidate variant
  44 3B AF 90 02 00 00 cmp r13d, [rdi + 0x290]  ; compare idLen
  75 26                jne NEXT_VARIANT
  48 8B 97 88 02 00 00 mov rdx, [rdi + 0x288]  ; rdx = idStr
  E8 8A 36 4E 00       call memcmp              ; compare idStr
  85 C0                test eax, eax
  75 09                jne NEXT_VARIANT
  0F B6 47 40          movzx eax, byte ptr [rdi + 0x40] ; variantIndex
  39 45 D0             cmp [rbp - 0x30], eax   ; compare variantIndex (0..7)
  74 53                je VARIANT_FOUND
  ```
  If the variant is not found in `registry + 0xc8`, the component is dropped and
  does not appear in the world. This lookup does not go through
  `FindComponentDefinitionByName`: our entries must already be in the variants
  ring when a vehicle is deserialized.

#### `Vehicle::PackageDefinitionsForSpawn`
- **RVA:** `0x4c6280` | **VA:** `0x1404c6280`
- **Signature:**
  ```cpp
  void PackageDefinitionsForSpawn(void* vehicleComponents, void* registry, void* outDefinitionBundle);
  ```
- **Description:**
  Builds the bundle of XML definitions a vehicle needs before it spawns, by
  calling `FindDefinitionDescriptorByName` (`0x14046f2f0`) on `registry + 0xe0`.
  Lets the server/client check and sync the definitions.

#### `SpawnVehicleComponent`
- **RVA:** `0x7f4cc0` | **VA:** `0x1407f4cc0`
- **Signature:**
  ```cpp
  void* SpawnVehicleComponent(void* spawnerContext, void* componentRef);
  ```
- **Parameters:**
  - `RSI`: the world spawner context (`[rsi + 0x70]` = server/gameplay world
    instance, `[rsi + 0x1580]` = variant reference).
  - `RDX`: reference of the component to instantiate (`[rdx + 0x368]` =
    `SwEngineString` with the id).
- **Description & assembly analysis:**
  Critical function that instantiates a component on "Spawn" or on a
  scripted/save spawn:
  1. Resolving the component definition:
     ```x86asm
     48 8B 4E 70          mov rcx, [rsi + 0x70]      ; rcx = world
     48 81 C1 70 B6 0B 00 add rcx, 0xbb670           ; rcx = world + 0xbb670 (ComponentRegistry)
     48 81 C2 68 03 00 00 add rdx, 0x368             ; rdx = component ID string
     E8 A2 A6 C7 FF       call FindComponentDefinitionByName ; 0x14046f380
     48 85 C0             test rax, rax
     0F 84 D1 01 00 00    jz SKIP_COMPONENT          ; skips and ignores it if nullptr
     ```
  2. Picking the variant (0..7):
     ```x86asm
     48 8B 8E 80 15 00 00 mov rcx, [rsi + 0x1580]
     0F B6 51 40          movzx edx, byte ptr [rcx + 0x40] ; variantIndex
     4C 8B 2C D0          mov r13, [rax + rdx * 8]   ; r13 = ComponentDefinition variant
     ```
  3. Checking the GPU VBO / IBO buffers:
     ```x86asm
     49 8B 85 A0 03 00 00 mov rax, [r13 + 0x3A0]    ; MeshResource of the variant
     48 85 C0             test rax, rax
     74 4B                jz SKIP_MESH
     48 8B 48 08          mov rcx, [rax + 8]         ; c_resource_object_geometry / geom
     48 85 C9             test rcx, rcx
     74 43                jz SKIP_MESH
     8B 41 60             mov eax, [rcx + 0x60]      ; geom->vbo
     85 C0                test eax, eax
     74 3B                jz SKIP_MESH               ; SKIPS DRAWING THE 3D MESH IF VBO == 0!
     8B 41 64             mov eax, [rcx + 0x64]      ; geom->ibo
     85 C0                test eax, eax
     74 33                jz SKIP_MESH               ; SKIPS DRAWING THE 3D MESH IF IBO == 0!
     ```
  If `vbo == 0` or `ibo == 0`, the engine draws no 3D mesh for the block
  instantiated in the physical world.

#### `DeleteVehicleComponentBlock`
- **RVA:** `0x7f4648` | **VA:** `0x1407f4648`
- **Signature:**
  ```cpp
  void DeleteVehicleComponentBlock(void* editorContext, void* block);
  ```
- **Description:**
  Handles deleting a block in the vehicle editor. Checks the bits at
  `definition + 0x2b8`. If bit `0x80` is set, the engine tries to call the
  virtual XML destructor, which causes a memory access crash. Must have
  `flags = 0` for a display clone.

---

### Rendering & 3D meshes (OpenGL)

#### `MeshResource::LoadFromMemory`
- **RVA:** `0x2cbfb0` | **VA:** `0x1402cbfb0`
- **Signature:**
  ```cpp
  void MeshResource::LoadFromMemory(MeshResource* dst, const MemBuffer* fileBuf, void* graphicsContext);
  ```
- **Parameters:**
  - `RCX`: `dst` — location of the `MeshResource` (`definition + 0x3a0` or
    `+0x620`).
  - `RDX`: `fileBuf` — memory buffer `{ const void* data, uint32_t size }` of the
    binary `.mesh` file.
  - `R8`: `graphicsContext` — graphics context pointer (`baseMon + 0x3c8`).
- **Description:**
  Clears the old mesh through `MeshResource::Clear`, allocates the vertices and
  indices in RAM, and calls `CreateOpenGLMeshBuffers` (`0x14020d9a0`) to create
  and upload the OpenGL VBO/VAO to the graphics card. Lets a mesh computed live
  in C++ be loaded without touching the disk.

#### `MeshResource::LoadFromFile`
- **RVA:** `0x2cc670` | **VA:** `0x1402cc670`
- **Signature:**
  ```cpp
  bool MeshResource::LoadFromFile(MeshResource* dst, void* fileSystem, void* graphicsContext, const SwString* filename);
  ```
- **Description:**
  Opens a mesh from the game's file system / archive through `0x1401836f0`, then
  passes the resulting memory buffer to `MeshResource::LoadFromMemory`
  (`0x1402cbfb0`).

#### `MeshResource::Clear`
- **RVA:** `0x2cbef0` | **VA:** `0x1402cbef0`
- **Signature:**
  ```cpp
  void MeshResource::Clear(MeshResource* dst);
  ```
- **Description:**
  Frees the CPU buffers of the indices (`+0x08`), vertices (`+0x18`) and
  sub-meshes (`+0x40`) when they are non-null. Before calling `LoadFromMemory` on
  a cloned definition, a `memset(dst, 0, 0xa0)` is required so that `Clear` does
  not free the buffers of the source definition (`baseMon`).

#### `CreateOpenGLMeshBuffers`
- **RVA:** `0x20d9a0` | **VA:** `0x14020d9a0`
- **Signature:**
  ```cpp
  void CreateOpenGLMeshBuffers(void* graphicsContext, void** outGpuBuffers);
  ```
- **Description:**
  Allocates a `c_resource_object_geometry` (16 bytes, vtable `0x140b6a9c0`) into
  `outGpuBuffers` (`meshRes + 0x30`), then creates a deferred event
  `c_resource_manager_deferred::event_create_object_geometry` (16 bytes, vtable
  `0x140b5af78`, target = `*outGpuBuffers`) and adds it to the event ring buffer
  of `graphicsContext` (`+0x00`: events, `+0x08`: cap, `+0x0c`: head,
  `+0x10`: count).

#### `DeferredEventCreateGeometry::Exec`
- **RVA:** `0x20cf40` | **VA:** `0x14020cf40`
- **Signature:**
  ```cpp
  void DeferredEventCreateGeometry::Exec(void* event, void* graphicsContext);
  ```
- **Description:**
  Virtual method `vtable[1]` of the geometry creation event. Calls
  `AllocateMmResourceGeometry` (`0x1401b4a70`) and stores the resulting
  `mm_resource_geometry*` pointer in `*(event->target + 8)` (that is
  `*(c_resource_object_geometry + 8)`).

#### `DeferredEventCreateGeometry::Dtor`
- **RVA:** `0x20e800` | **VA:** `0x14020e800`
- **Signature:**
  ```cpp
  void DeferredEventCreateGeometry::Dtor(void* event, uint32_t flags);
  ```
- **Description:**
  Virtual destructor `vtable[0]` of the deferred event. Frees the event's memory
  through `operator delete(event, 0x10)`.

#### `AllocateMmResourceGeometry`
- **RVA:** `0x1b4a70` | **VA:** `0x1401b4a70`
- **Signature:**
  ```cpp
  mm_resource_geometry* AllocateMmResourceGeometry(void* graphicsContext);
  ```
- **Description:**
  Allocates a 0x68-byte `mm_resource_geometry` structure (vtable `0x140b6abf8`),
  increments its reference count and registers it in the graphics context's
  resource table at `graphicsContext + 0x180`. Holds the final OpenGL ids:
  `+0x5c`: index count, `+0x60`: VBO (`GL_ARRAY_BUFFER`), `+0x64`: IBO
  (`GL_ELEMENT_ARRAY_BUFFER`).

#### `DrawGeometryBuffers` (crash site 0x14068d71c / nvoglv64.dll)
- **RVA:** `0x68d5b0` | **VA:** `0x14068d5b0`
- **Signature:**
  ```cpp
  void DrawGeometryBuffers(void* shaderCtx, mm_resource_geometry* geom, uint64_t indexOffset, int indexCount, ...);
  ```
- **Symptom & minidump trace:**
  Crash at `0x00007FFE206FD14E` in `nvoglv64.dll` (`RVA 0xB8D14E`,
  `Code 0xC0000005`, read of `0x0000000000000000`).
- **Description & root cause analysis:**
  Low-level OpenGL draw function doing:
  ```x86asm
  mov edx, dword ptr [rsi + 0x60] ; geom->vbo
  mov ecx, 0x8892                 ; GL_ARRAY_BUFFER
  call glBindBuffer
  mov edx, dword ptr [rsi + 0x64] ; geom->ibo
  mov ecx, 0x8893                 ; GL_ELEMENT_ARRAY_BUFFER
  call glBindBuffer
  ; Sets up glVertexAttribPointer (stride 28, pos=0, color=12, normal=16)
  mov edx, dword ptr [rsi + 0x5c] ; geom->indexCount
  call glDrawElements             ; glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, (void*)0)
  call glBindBuffer(0x8892, 0)
  call glBindBuffer(0x8893, 0)
  ```
  The engine uses no VAO in this routine; it binds `geom->vbo` and `geom->ibo`
  directly. If the mesh has null VBO/IBO ids (`0`),
  `glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)` unbinds the IBO, and Nvidia tries to
  read `(void*)0` as a CPU client memory pointer, which crashes immediately.
  If the mesh points to the `geom` of a 2x2 monitor (`monitor_2`), OpenGL draws
  the 2x2 mesh whatever the declared physical size.
- **Final solution:**
  1. Direct computation of the coordinates of the 84 vertices and 150 indices for
     any `W x H` size through `BuildCustomDisplayMeshBuffers(w, h, frameBuf, arrowBuf)`.
  2. Direct GPU allocation through `glGenBuffers` and `glBufferData` of dedicated
     VBO/IBO for each `W x H` size.
  3. Clone of the `c_resource_object_geometry` (16 bytes) and
     `mm_resource_geometry` (0x68 bytes) structures with the custom GPU handles,
     cached in `s_gpuMeshCache`.
  4. Applied to every variant (0..7) of the component's definition in the
     registry: the screen is drawn at the exact size both in the editor and in
     the simulation world.

---

### Component factory & types

#### `ComponentFactory`
- **RVA:** `0x45eb50` | **VA:** `0x14045eb50`
- **Description:**
  Central switch on `definition + 0x2ac` (type). Type 43 instantiates a
  `ComponentMonitor` (0x610 bytes, constructor at `0x140365b80`, vtable
  `0x140b70ea0`).

---

### Vehicle editor & interface

| RVA | VA | Usual name | Role |
|---|---|---|---|
| `0x801340` | `0x140801340` | `VehicleEditor::SelectDefinitionInHand` | Puts a component in the player's hand in the editor |
| `0x611070` | `0x140611070` | `CatalogueWidget::AddItem` | Inserts a new item into the catalogue at runtime |
| `0x611050` | `0x140611050` | `CatalogueWidget::GetSelectedItem` | Gets the selected item of the catalogue |
| `0x60a470` | `0x14060a470` | `ToolbarWidget::GetActiveSlot` | Gets the active slot of the toolbar |
| `0x60bd10` | `0x14060bd10` | `LayoutGrid::Layout` | Recomputes the catalogue grid layout |
| `0x24b650` | `0x14024b650` | `ScrollPane::Refresh` | Updates the scrolling and the visible area of a scroll view |
| `0x25aa60` | `0x14025aa60` | `Widget::SetChildrenOpen` | Recursively enables/disables child widgets |
| `0x24f7d0` | `0x14024f7d0` | `Editbox::SetAlpha` | Sets the opacity and enabled state of an edit box |
| `0x8dd150` | `0x1408dd150` | `RingBuffer::Grow` | Reallocates and grows a full `DefRingBuffer` |

---

## 4. Vehicle editor vtables & life cycles

- Editor UI vtable: `stormworks64.exe + 0xb99940`
  - `uiInitSlot` (RVA `0x62ac80`): UI initialisation
  - `uiUpdateSlot` (RVA `0x62c530`): UI frame loop
  - `uiTeardownSlot` (RVA `0x241bc0`): UI close
  - `uiDestroySlot` (RVA `0x62ed20`): UI container destruction
- Vehicle state vtable: `stormworks64.exe + 0xb44838`
  - `stateEnterSlot` (RVA `0x7cfce0`): entering the vehicle editor
  - `stateLeaveSlot` (RVA `0x7d0770`): leaving the vehicle editor
  - `stateActionsSlot` (RVA `0x7fa7a0`): dispatch of user actions (catalogue
    selection, shortcuts, click)
  - `stateTeardownSlot` (RVA `0x7cf750`): state cleanup

---

## 5. Memory management & registry destructors

#### `ComponentRegistry::Clear`
- **RVA:** `0x5f1fd0` | **VA:** `0x1405f1fd0`
- **Signature:**
  ```cpp
  void ComponentRegistry::Clear(void* registry);
  ```
- **Description:**
  Clears and resets all the buffers of a component registry
  (`world + 0xbb670`) when a world changes or when entering the workbench.
  1. Empties the component categories (`registry + 0x110`).
  2. Frees the global buffer `registry + 0xf8` through `_aligned_free`
     (`0x1409B15A0`).
  3. Calls the XML descriptors destructor (`0x1408B8EE0`) on `registry + 0xe0`.
  4. Calls the variants destructor (`0x1408B8FB0`) on `registry + 0xc8`.

#### `DestructXmlDescriptors`
- **RVA:** `0x8b8ee0` | **VA:** `0x1408b8ee0`
- **Description:**
  Walks the XML descriptors ring buffer (`registry + 0xe0`), and frees, for each
  entry:
  - `desc->xmlData` (+0x10) through `_aligned_free` (`0x1409B15A0`)
  - `desc->idStr` (+0x00) through `_aligned_free` (`0x1409B15A0`)
  - `desc` through `operator delete(desc, 32)` (site `0x14095706C` on the
    reference build; what matters is the SIZE 32, the address changes per build)
  > [!CAUTION]
  > Any descriptor allocated by an external DLL's runtime and passed to this
  > destructor causes an instant `STATUS_HEAP_CORRUPTION` (`0xc0000374`) crash.
  > The mod intercepts `ComponentRegistry::Clear` to remove its own descriptors
  > before the engine call.

#### `DestructVariantDefs`
- **RVA:** `0x8b8fb0` | **VA:** `0x1408b8fb0`
- **Description:**
  Walks the variants ring buffer (`registry + 0xc8`), and calls for each
  variant:
  - the variant destructor: `0x14087EE00`
  - `operator delete(var, sizeof(ComponentDefinition))`: `0x2848` on 1.15.18,
    **`0x2d08` on 1.15.23** (1.15.23 site: `0x8B9FEE`)
  The mod's hook compacts the ring and removes all its own variants before the
  engine destructor runs, to avoid any heap conflict.
