/* Actual native home protocol over authored directories only. No setup, UI, download or app launch. */
#include <stdio.h>
#include <stdlib.h>
#include "portable_home.h"

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
static void join(wchar_t *out, const wchar_t *a, const wchar_t *b) {
    if (!hb_home_join(out, MAX_PATH, a, b)) exit(2);
}
static int put(const wchar_t *path, const void *bytes, DWORD size) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD wrote = 0;
    int ok = WriteFile(f, bytes, size, &wrote, NULL) && wrote == size;
    CloseHandle(f);
    return ok;
}
int wmain(int argc, wchar_t **argv) {
    if (argc == 3 && !wcscmp(argv[1], L"--read")) {
        CHECK(hb_home_marked(argv[2]) == 1);
        puts("PASS: native reader accepts the C# marker");
        return 0;
    }
    if (argc != 2) return 2;
    wchar_t volume[MAX_PATH], root[MAX_PATH], game[MAX_PATH], exe[MAX_PATH], ordinary[MAX_PATH], cwd[MAX_PATH];
    wchar_t marker[MAX_PATH], pointer[MAX_PATH], moved[MAX_PATH], next[MAX_PATH], normalized[MAX_PATH];
    join(volume, argv[1], L"volume old");
    join(root, volume, L"HookshotBuilder");
    join(game, volume, L"Game");
    join(exe, root, L"versions\\v-authored\\HookshotBuilder\\HookshotBuilder.exe");
    join(ordinary, argv[1], L"standard");
    join(cwd, argv[1], L"unrelated cwd");
    CHECK(SetCurrentDirectoryW(cwd));
    CHECK(!hb_home_full(normalized, L"relative"));
    CHECK(!hb_home_full(normalized, L"C:relative"));
    CHECK(!hb_home_full(normalized, L"\\\\?\\C:\\authored"));
    CHECK(!hb_home_full(normalized, L"\\\\.\\authored"));
    CHECK(hb_home_volume(L"C:\\"));
    CHECK(hb_home_marked(root) == 0);
    CHECK(hb_home_mark(root));
    CHECK(hb_home_mark(root));
    CHECK(hb_home_marked(root) == 1);
    HbHome home;
    CHECK(hb_home_discover(&home, NULL, exe, ordinary, 0) && home.portable && !wcscmp(home.root, root));
    wchar_t original_setup[MAX_PATH]; join(original_setup, volume, L"HookshotBuilder.exe");
    CHECK(hb_home_discover(&home, NULL, original_setup, ordinary, 0) && home.portable && !wcscmp(home.root, root));
    CHECK(hb_home_discover(&home, root, exe, ordinary, 0) && home.portable && !wcscmp(home.root, root));
    CHECK(hb_home_discover(&home, ordinary, exe, ordinary, 0) && !home.portable && !wcscmp(home.root, ordinary));
    CHECK(!hb_home_discover(&home, L"relative", exe, ordinary, 0) && !home.root[0]);
    join(pointer, game, HB_GAME_HOME);
    const char relative[] = "../HookshotBuilder\n";
    CHECK(put(pointer, relative, sizeof relative - 1));
    CHECK(hb_home_game(&home, game) == 1 && home.portable && !wcscmp(home.root, root));
    wchar_t launcher[MAX_PATH]; join(launcher, game, L"hookshot.exe");
    CHECK(hb_home_discover(&home, NULL, launcher, ordinary, 1) && home.portable && !wcscmp(home.root, root));
    CHECK(put(pointer, "C:\\unowned", 10));
    CHECK(!hb_home_discover(&home, NULL, launcher, ordinary, 1) && !home.root[0]);
    CHECK(DeleteFileW(pointer));
    wchar_t metadata[MAX_PATH]; join(metadata, game, L"hookshot_version.txt");
    const char portable_metadata[] = "version=v-authored\nbuilder_local=1\n";
    CHECK(put(metadata, portable_metadata, sizeof portable_metadata - 1));
    CHECK(!hb_home_discover(&home, NULL, launcher, ordinary, 1) && !home.root[0]);
    CHECK(DeleteFileW(metadata));
    CHECK(hb_home_discover(&home, NULL, launcher, ordinary, 1) && !home.portable);
    CHECK(put(pointer, relative, sizeof relative - 1));
    join(moved, argv[1], L"volume moved");
    CHECK(MoveFileW(volume, moved));
    join(next, moved, L"HookshotBuilder");
    join(game, moved, L"Game");
    join(launcher, game, L"hookshot.exe");
    CHECK(hb_home_discover(&home, NULL, launcher, ordinary, 1) && home.portable && !wcscmp(home.root, next));

    /* Corrupt a separate owned root; never overwrite the valid native interoperability marker. */
    join(root, argv[1], L"broken");
    join(marker, root, HB_HOME_MARKER);
    const char invalid[] = HB_HOME_MAGIC "\0x";
    CHECK(put(marker, invalid, sizeof invalid - 1));
    CHECK(hb_home_marked(root) == -1 && !hb_home_mark(root));
    CHECK(!hb_home_discover(&home, root, exe, ordinary, 0) && !home.root[0]);
    const char bom[] = "\xef\xbb\xbf" HB_HOME_MAGIC;
    CHECK(put(marker, bom, sizeof bom - 1) && hb_home_marked(root) == -1);
    char large[4096]; memset(large, 'x', sizeof large);
    CHECK(put(marker, large, sizeof large) && hb_home_marked(root) == -1);
    const char valid[] = HB_HOME_MAGIC "\r\n";
    CHECK(put(marker, valid, sizeof valid - 1) && hb_home_marked(root) == 1);
    printf("PASS: %u native root/marker/association/relocation assertions\n", checks);
    return 0;
}
