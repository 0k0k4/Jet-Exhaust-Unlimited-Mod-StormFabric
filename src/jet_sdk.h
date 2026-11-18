#pragma once
// Thin, mod-side wrapper around the StormFabric native SDK.
//
// The mod owns its own logic; the loader only provides generic primitives
// (symbol resolution from its discovery profile, bounded memory access, inline
// hooks, engine allocator/strings, executable scratch memory).
#include "sw_mod_sdk.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace jet {

struct Engine {
    const SwModHost* host = nullptr;
    const SwNativeApi* native = nullptr;
    uint8_t* base = nullptr;    // game image (ASLR)
    uint32_t build = 0;         // 1/2 = certified builds, 0 = unknown
};
extern Engine g_engine;

// Validates the SDK contract and resolves the engine services. Fail-closed: a
// missing native API, unknown build or unresolved allocator leaves the mod inert.
bool InitEngine(const SwModHost* host);

// Logging routed through host->log (StormworksModLoader.log + loader UI).
void LogInfo(const char* fmt, ...);
void LogWarn(const char* fmt, ...);
void LogError(const char* fmt, ...);

// Engine strings {data,len,capacity}: allocated by the engine allocator, freed
// by the engine deallocator. Never mix with sw_game_alloc blocks.
void StrMake(SwEngineString& string, const char* utf8);
void StrRelease(SwEngineString& string);

std::wstring Utf8ToWide(const std::string& utf8);
}  // namespace jet
