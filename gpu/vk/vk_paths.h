// The game folder's path for the files the game keeps there (the shader list, settings): host/hos_paths.h's rule, for this
// DLL. The game is bin\hookshot.exe beside its launcher since the launcher came; its files stay in the folder above bin.
// game_module_path answers as GetModuleFileNameA(nullptr, ...) does, with the folder's path when the exe is in a "bin" folder
// beside image.bin. What the game makes as it runs goes in two folders of the game folder: cache/ (cache_path: the shader and
// pipeline caches, made for this build and this GPU, which can be deleted) and logs/ (log_path: the logs and traces). The host
// makes both at its start and moves a build's from before into them (host/game_layout.c).
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef min
#undef max
#include <cstring>
#include <string>

namespace mkwvk {
inline DWORD game_module_path(char* buf, DWORD size) {
    DWORD n = GetModuleFileNameA(nullptr, buf, size);
    if (!n || n >= size) return n;
    char* name = buf + n;
    while (name > buf && name[-1] != '\\' && name[-1] != '/') name--;
    if (name - buf < 5) return n;
    char* dir = name - 1;
    char* bin = dir;
    while (bin > buf && bin[-1] != '\\' && bin[-1] != '/') bin--;
    if (dir - bin != 3 || _strnicmp(bin, "bin", 3) != 0) return n;
    char probe[MAX_PATH + 16];
    const size_t root = (size_t)(bin - buf);
    if (root + 10 >= sizeof probe) return n;
    std::memcpy(probe, buf, root);
    std::strcpy(probe + root, "image.bin");
    if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) return n;
    std::memmove(buf + root, name, std::strlen(name) + 1);
    return (DWORD)std::strlen(buf);
}
/// The game folder, with its separator at the end
inline std::string game_folder_path() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    const std::string path(exe, n < MAX_PATH ? n : 0);
    return path.substr(0, path.find_last_of("\\/") + 1);
}
/// <game folder>/<sub>/<name>, the folder made when it is not there (the host makes it at its start; a tool may not have)
inline std::string game_sub_path(const char* sub, const char* name) {
    const std::string dir = game_folder_path() + sub;
    if (GetFileAttributesA(dir.c_str()) == INVALID_FILE_ATTRIBUTES) CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "/" + name;
}
/// <game folder>/cache/<name>: the shader and pipeline caches ("" for the folder itself, with its separator)
inline std::string cache_path(const char* name) { return game_sub_path("cache", name); }
/// <game folder>/logs/<name>: the logs and traces a run writes
inline std::string log_path(const char* name) { return game_sub_path("logs", name); }
}  // namespace mkwvk
