#pragma once

namespace Platform {
#ifdef _WIN32
inline constexpr const char* ScriptModuleFilename = "GameScripts.dll";
inline constexpr const char* EngineLibraryFilename = "Engine.dll";
inline constexpr const char* EngineLinkLibraryFilename = "Engine.lib";
inline constexpr const char* PlayerFilename = "GamEnginePlayer.exe";
inline constexpr const char* ExecutableSuffix = ".exe";
inline constexpr const char* RuntimeDirectory = "Runtime/Win64/Release";
inline constexpr const char* BuildPlatform = "Windows";
#else
inline constexpr const char* ScriptModuleFilename = "libGameScripts.so";
inline constexpr const char* EngineLibraryFilename = "libEngine.so";
inline constexpr const char* EngineLinkLibraryFilename = "libEngine.so";
inline constexpr const char* PlayerFilename = "GamEnginePlayer";
inline constexpr const char* ExecutableSuffix = "";
inline constexpr const char* RuntimeDirectory = "Runtime/Linux64/Release";
inline constexpr const char* BuildPlatform = "Linux";
#endif
} // namespace Platform
