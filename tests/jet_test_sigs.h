#pragma once
// Déclarations des internes de src/jet_patch.cpp exposées aux tests uniquement.
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace swml {
struct ModEntry;
namespace jet_internal {
size_t FindSignature(const uint8_t* data, size_t size, size_t& hitOffset);
bool ResolveConstant(const uint8_t* site, uint32_t& constBits);
size_t BuildJetStub(uint8_t* out, uint32_t signature, float hugeLimit, float vanillaLimit);
bool InstallPatch(uint8_t* site, const uint8_t* stub, const uint8_t* expectedOriginal,
                  uint8_t* savedOriginal, size_t patchSize);
bool RevertPatch(uint8_t* site, const uint8_t* savedOriginal, size_t patchSize);
uint8_t* AllocateNear(uint8_t* site);
bool ReconcileDefinitions(const std::vector<ModEntry>& mods, const std::wstring& defsDir,
                          std::vector<std::string>& injected);
bool ReconcileComponents(const std::vector<ModEntry>& mods, const std::wstring& componentsDir,
                         std::vector<std::string>& injected);
std::wstring UserStormworksModsDir();
bool ReconcileModAssets(const std::vector<ModEntry>& mods, const std::wstring& modsRoot,
                        std::vector<std::string>& injected);
bool SafeDefinitionName(const std::string& name);
constexpr size_t kStubBlockBytes = 0x40;
}
}
