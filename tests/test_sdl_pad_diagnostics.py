"""SDL2/SDL3 connection diagnostics: the real loader and log call, without SDL or a controller.

Known mapping, no mapping, missing optional calls, zero GUID, allocation ownership,
existing gyro/Steam suffixes and bounded mapping text. The same fixture against
--source <base checkout> fails on the old connection line, not on compilation.
python -I -B tests/test_sdl_pad_diagnostics.py [--cc gcc] [--source repository]
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PRELUDE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
typedef void *HMODULE;
typedef void (*FARPROC)(void);
static FARPROC GetProcAddress(HMODULE module, const char *name);
static char logged[4096];
static void hos_log(const char *format, ...) {
    char message[1024];   /* host/hos_thread.c's real per-message limit */
    va_list ap;
    va_start(ap, format);
    vsnprintf(message, sizeof message, format, ap);
    va_end(ap);
    strncat(logged, message, sizeof logged - strlen(logged) - 1);
    strncat(logged, "\n", sizeof logged - strlen(logged) - 1);
}
static int version;
#define CALL(type, fn) ((type)f.fn)
'''

FIXTURE = r'''
enum { NO_GUID = 1, NO_MAPPING = 2, NO_FREE = 4, NO_JOYSTICK = 8 };
/* (takes, not "gets": MinGW's stdio.h still declares gets(), and a variable of that name does not compile there) */
static int missing, unknown, takes, frees, wrong_symbols, unexpected_calls, checks, failures;
static char *allocated;
static const char *mapping_text;
static void dummy(void) { unexpected_calls++; }
static const char *pad_name(void *pad) { (void)pad; return "Test controller"; }
static void *joystick(void *pad) { return pad; }
static HosSdlGuid guid_of(void *pad) {
    (void)pad;
    HosSdlGuid guid = {{0}};
    if (!unknown) {
        const unsigned char bytes[16] = {3,0,0,0,0x34,0x12,0,0,0x78,0x56,0,0,1,0,0,0};
        memcpy(guid.data, bytes, sizeof bytes);
    }
    return guid;
}
static char *mapping_of(void *pad) {
    (void)pad;
    takes++;
    if (!mapping_text) return NULL;
    allocated = malloc(strlen(mapping_text) + 1);
    if (allocated) strcpy(allocated, mapping_text);
    return allocated;
}
static void free_mapping(void *ptr) {
    if (ptr != allocated) unexpected_calls++;
    frees++;
    free(ptr);
    allocated = NULL;
}
static FARPROC GetProcAddress(HMODULE module, const char *name) {
    (void)module;
    if (strstr(name,"Serial") || strstr(name,"Path")) unexpected_calls++;
    if (!strcmp(name,"SDL_GetGamepadName") || !strcmp(name,"SDL_GameControllerName")) return (FARPROC)pad_name;
    if (!strcmp(name,"SDL_GetGamepadJoystick") || !strcmp(name,"SDL_GameControllerGetJoystick")) {
        if ((version == 3) != !strcmp(name,"SDL_GetGamepadJoystick")) wrong_symbols++;
        return missing & NO_JOYSTICK ? NULL : (FARPROC)joystick;
    }
    if (!strcmp(name,"SDL_GetJoystickGUID") || !strcmp(name,"SDL_JoystickGetGUID")) {
        if ((version == 3) != !strcmp(name,"SDL_GetJoystickGUID")) wrong_symbols++;
        return missing & NO_GUID ? NULL : (FARPROC)guid_of;
    }
    if (!strcmp(name,"SDL_GetGamepadMapping") || !strcmp(name,"SDL_GameControllerMapping")) {
        if ((version == 3) != !strcmp(name,"SDL_GetGamepadMapping")) wrong_symbols++;
        return missing & NO_MAPPING ? NULL : (FARPROC)mapping_of;
    }
    if (!strcmp(name,"SDL_free")) return missing & NO_FREE ? NULL : (FARPROC)free_mapping;
    return dummy;   /* required unrelated entry points are resolved, never called */
}
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"FAIL SDL%d line %d: %s\n  %s\n",version,__LINE__,#c,logged); failures++; } } while (0)
static void connect_pad(int motion, int steam) {
    struct { int motion, steam; } pads[1] = {{motion,steam}};
    const int slot = 0;
    void *gc = (void *)(uintptr_t)1;
    ACTUAL_CONNECTION
}
static void load(int absent) {
    memset(&f,0,sizeof f);
    missing = absent;
    takes = frees = 0;
    logged[0] = 0;
    CHECK(get_all((HMODULE)(uintptr_t)1, version == 3));
}
int main(void) {
    for (version = 2; version <= 3; version++) {
        unknown = 0;
        mapping_text = "03000000341200007856000001000000,Test mapping,a:b1,b:b2,x:b0,y:b3,platform:Windows,";
        load(0);
        connect_pad(1,1);
        CHECK(strstr(logged,"connected (SDL), with a gyroscope, Steam Input's") != NULL);
        CHECK(strstr(logged,"guid=03000000341200007856000001000000") != NULL);
        CHECK(strstr(logged,"mapping=available; mapping-name=\"Test mapping\"") != NULL);
        CHECK(strstr(logged,mapping_text) != NULL);
        CHECK(takes == 1 && frees == 1 && !allocated);
        printf("SDL%d known: %s\n",version,logged);

        unknown = 1;
        mapping_text = NULL;
        load(0);
        connect_pad(0,0);
        CHECK(strstr(logged,"guid=unknown; mapping=none; mapping-name=\"?\"") != NULL);
        CHECK(!strstr(logged,"with a gyroscope") && !strstr(logged,"Steam Input's"));
        CHECK(takes == 1 && frees == 0);
        printf("SDL%d unknown: %s\n",version,logged);

        load(NO_GUID | NO_MAPPING);
        connect_pad(0,0);
        CHECK(strstr(logged,"guid=unavailable; mapping=unavailable") != NULL);
        CHECK(takes == 0 && frees == 0);
        printf("SDL%d missing functions: %s\n",version,logged);

        unknown = 0;
        mapping_text = "03000000341200007856000001000000,Other name,a:b0,";
        load(NO_MAPPING);
        connect_pad(0,0);
        CHECK(strstr(logged,"guid=03000000341200007856000001000000; mapping=unavailable") != NULL);
        CHECK(!takes && !frees);
        load(NO_GUID);
        connect_pad(0,0);
        CHECK(strstr(logged,"guid=unavailable; mapping=available") != NULL);
        CHECK(takes == 1 && frees == 1);
        if (version == 2) {   /* SDL3 already requires SDL_free to enumerate controllers. */
            load(NO_FREE);
            connect_pad(0,0);
            CHECK(strstr(logged,"mapping=unavailable") != NULL);
            CHECK(!takes && !frees);   /* never allocate a string that cannot be freed */
        } else {
            load(NO_JOYSTICK);
            connect_pad(0,0);
            CHECK(strstr(logged,"guid=unavailable; mapping=available") != NULL);
            CHECK(takes == 1 && frees == 1);
        }
        char long_mapping[1400];
        memset(long_mapping,'z',sizeof long_mapping);
        memcpy(long_mapping,"03000000341200007856000001000000,Long mapping,",46);
        strcpy(long_mapping + 1300,"END_MUST_NOT_BE_LOGGED");
        mapping_text = long_mapping;
        load(0);
        connect_pad(0,0);
        CHECK(strstr(logged,"mapping=available") != NULL);
        CHECK(!strstr(logged,"END_MUST_NOT_BE_LOGGED"));
        CHECK(strstr(logged," (truncated)") != NULL);
        const char *first = strstr(logged,"mapping-string[1/2]=\""), *second = strstr(logged,"mapping-string[2/2]=\"");
        CHECK(first && second);
        if (first && second) {
            first += strlen("mapping-string[1/2]=\"");
            second += strlen("mapping-string[2/2]=\"");
            CHECK(!strncmp(first,long_mapping,512) && first[512] == '"');
            CHECK(!strncmp(second,long_mapping+512,512) && second[512] == '"');
        }
        CHECK(strlen(logged) < 1400);
        CHECK(takes == 1 && frees == 1 && !allocated);
        CHECK(!wrong_symbols && !unexpected_calls);
    }
    printf("%s: %d checks, %d failures; SDL2/SDL3 connection diagnostics\n",failures ? "FAIL" : "PASS",checks,failures);
    return failures != 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--source", type=Path, default=ROOT)
    args = parser.parse_args()
    sdl = (args.source / "host/hos_sdl.c").read_text(encoding="utf-8")
    pad = (args.source / "host/pad_sdl.c").read_text(encoding="utf-8")
    state = sdl[sdl.index("static struct {"):sdl.index("} f;") + len("} f;")]
    guid = re.search(r"typedef struct \{[^}]*\} HosSdlGuid;", sdl)
    # The baseline only needs this type for the fixture's fake SDL function ABI.
    declaration = guid.group(0) if guid else "typedef struct { uint8_t data[16]; } HosSdlGuid;"
    code = PRELUDE + state + "\n" + declaration + "\n"
    code += block(sdl, "static int get_all(") + "\n" + block(sdl, "const char *hos_sdl_pad_name(") + "\n"
    if "void hos_sdl_pad_log_connection(" in sdl:
        code += block(sdl, "void hos_sdl_pad_log_connection(") + "\n"
    at = pad.index("\n", pad.index("pads[slot].motion_only = 0;")) + 1
    connection = pad[at:pad.index(";", at) + 1].strip()
    code += FIXTURE.replace("ACTUAL_CONNECTION", connection)
    with tempfile.TemporaryDirectory(prefix="hookshot-sdl-diagnostics-") as temp:
        src, exe = Path(temp) / "test.c", Path(temp) / "test.exe"
        src.write_text(code, encoding="utf-8")
        subprocess.run([args.cc,"-std=c11","-Wall","-Wextra","-Werror",str(src),"-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)


if __name__ == "__main__":
    main()
