#include "jet_sdk.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace jet {

Engine g_engine;

namespace {
void Emit(const char* level, const char* fmt, va_list args) {
    char message[2048];
    vsnprintf(message, sizeof(message), fmt, args);
    message[sizeof(message) - 1] = '\0';
    if (!g_engine.host || !g_engine.host->log) return;
    char line[2200];
    snprintf(line, sizeof(line), "%s: %s", level, message);
    g_engine.host->log(g_engine.host->modId, line);
}
}  // namespace

bool InitEngine(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api) return false;
    // The native API is appended: a loader that predates it exposes a shorter
    // struct and no pointer, so the mod stays inert instead of guessing.
    if (host->api->structSize < offsetof(SwEngineApi, native) + sizeof(host->api->native))
        return false;
    const SwNativeApi* native = host->api->native;
    if (!native || native->structSize < sizeof(SwNativeApi) ||
        native->version != SW_NATIVE_API_VERSION || !native->findSymbol ||
        !native->hookInline || !native->hookRestore || !native->gameAlloc ||
        !native->stringMake || !native->codeAllocNear || !native->engineBuild)
        return false;
    g_engine.host = host;
    g_engine.native = native;
    g_engine.base = static_cast<uint8_t*>(host->gameBase);
    g_engine.build = native->engineBuild(host);
    if (!g_engine.base || !g_engine.build) {
        LogError("API native incomplete (base=%p build=%u) ; mod inactif",
                 (void*)g_engine.base, g_engine.build);
        return false;
    }
    return true;
}

void LogInfo(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Emit("INFO", fmt, args);
    va_end(args);
}
void LogWarn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Emit("WARN", fmt, args);
    va_end(args);
}
void LogError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Emit("ERREUR", fmt, args);
    va_end(args);
}

void StrMake(SwEngineString& string, const char* utf8) {
    string.data = nullptr;
    string.len = 0;
    string.cap = 0;
    if (!g_engine.native || !g_engine.host || !utf8) return;
    g_engine.native->stringMake(g_engine.host, &string, utf8);
}

void StrRelease(SwEngineString& string) {
    if (g_engine.native && g_engine.host)
        g_engine.native->stringFree(g_engine.host, &string);
    string.data = nullptr;
    string.len = 0;
    string.cap = 0;
}

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (length <= 0) return L"";
    std::wstring wide(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), length);
    return wide;
}
}  // namespace jet
