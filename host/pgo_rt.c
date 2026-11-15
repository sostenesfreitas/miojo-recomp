/* (hookshot) Profile-guided builds (RX_PGO=gen, nx2/build.py): what clang's -fprofile-instr-generate needs, in place of compiler-rt's
   profile library, which this toolchain lacks. The counters sit in sections .lprfc/.lprfd/.lprfn; this writes them in LLVM's raw
   format for llvm-profdata.
   HOS_PGO=<path>: every 500 ms, if <path>.reset exists the counters are cleared, and if <path>.dump exists they are written to <path>;
   each file is deleted when done. So a run can leave out the loading.
   The renderer module has a copy of its own (RX_VK_PGO=gen: the counters are each module's own), which reads RX_VK_PGO_FILE
   instead (-DPGO_ENV, below). After ProjectReLink (https://github.com/pixl27/ProjectReLink).
   On Linux the counters are in the ELF sections __llvm_prf_cnts/__llvm_prf_data/__llvm_prf_names, whose ends the linker names
   (__start_/__stop_), and the Win32 calls below are host/posix's. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int __llvm_profile_runtime = 0;

#ifdef _WIN32
#pragma section(".lprfc$A", read, write)
#pragma section(".lprfc$Z", read, write)
#pragma section(".lprfd$A", read, write)
#pragma section(".lprfd$Z", read, write)
#pragma section(".lprfn$A", read)
#pragma section(".lprfn$Z", read)
__declspec(allocate(".lprfc$A")) __attribute__((used, aligned(8))) char pgo_cnt_a[8] = {0};
__declspec(allocate(".lprfc$Z")) __attribute__((used, aligned(8))) char pgo_cnt_z[8] = {0};
__declspec(allocate(".lprfd$A")) __attribute__((used, aligned(8))) char pgo_dat_a[72] = {0};
__declspec(allocate(".lprfd$Z")) __attribute__((used, aligned(8))) char pgo_dat_z[72] = {0};
__declspec(allocate(".lprfn$A")) __attribute__((used)) const char pgo_nam_a[1] = {0};
__declspec(allocate(".lprfn$Z")) __attribute__((used)) const char pgo_nam_z[1] = {0};
#define PGO_CNT_BEGIN (pgo_cnt_a + 8)
#define PGO_CNT_END pgo_cnt_z
#define PGO_DAT_BEGIN (pgo_dat_a + sizeof pgo_dat_a)
#define PGO_DAT_END pgo_dat_z
#define PGO_NAM_BEGIN (pgo_nam_a + 1)
#define PGO_NAM_END pgo_nam_z
#else
/* (weak and hidden, as compiler-rt declares them: this module's own sections, and a module with nothing counted still links) */
extern char __start___llvm_prf_cnts[] __attribute__((weak, visibility("hidden")));
extern char __stop___llvm_prf_cnts[] __attribute__((weak, visibility("hidden")));
extern char __start___llvm_prf_data[] __attribute__((weak, visibility("hidden")));
extern char __stop___llvm_prf_data[] __attribute__((weak, visibility("hidden")));
extern const char __start___llvm_prf_names[] __attribute__((weak, visibility("hidden")));
extern const char __stop___llvm_prf_names[] __attribute__((weak, visibility("hidden")));
#define PGO_CNT_BEGIN __start___llvm_prf_cnts
#define PGO_CNT_END __stop___llvm_prf_cnts
#define PGO_DAT_BEGIN __start___llvm_prf_data
#define PGO_DAT_END __stop___llvm_prf_data
#define PGO_NAM_BEGIN __start___llvm_prf_names
#define PGO_NAM_END __stop___llvm_prf_names
#endif

/* (raw format 10 for clang 22 and older, a Linux system's: 64-byte records, no uniform counter fields in the header; 11 after) */
#if defined(__clang_major__) && __clang_major__ <= 22
enum { DATA_SIZE = 64, RAW_VERSION = 10 };
#define RAW_VERSION_11_FIELDS 0
#else
enum { DATA_SIZE = 72, RAW_VERSION = 11 };
#define RAW_VERSION_11_FIELDS 1
#endif

static void pgo_write(const char *path) {
    const char *cb = PGO_CNT_BEGIN, *ce = PGO_CNT_END, *db = PGO_DAT_BEGIN, *de = PGO_DAT_END, *nb = PGO_NAM_BEGIN, *ne = PGO_NAM_END;
    const uint64_t ndata = (uint64_t)(de - db) / DATA_SIZE, ncnt = (uint64_t)(ce - cb) / 8, nnames = (uint64_t)(ne - nb);
    const uint64_t header[] = {
        0xff6c70726f667281ULL,        /* magic: \xfflprofr\x81 */
        RAW_VERSION,                  /* (11: the format llvm-profdata 24 reads; 10 for clang 22) */
        0,                            /* binary ids */
        ndata, 0, ncnt, 0,
        0, 0,                         /* bitmap */
#if RAW_VERSION_11_FIELDS
        0, 0, 0,                      /* uniform counters */
#endif
        nnames,
        (uint64_t)(uintptr_t)cb - (uint64_t)(uintptr_t)db,   /* CountersDelta */
        (uint64_t)0,                  /* BitmapDelta */
        (uint64_t)(uintptr_t)nb,      /* NamesDelta */
        0, 0,                         /* vtables */
        2,                            /* ValueKindLast (IPVK_Last) */
    };
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(header, sizeof header, 1, f);
    char *copy = malloc((size_t)(de - db));
    memcpy(copy, db, (size_t)(de - db));
#ifdef _WIN32
    /* (the records' pointers to their counters are 32 bits in a COFF image, the rest zero, and the counters come first: negative.
       In an ELF image they are 64 bits already) */
    for (uint64_t i = 0; i < ndata; i++) {
        int32_t rel;
        memcpy(&rel, copy + i * DATA_SIZE + 16, 4);
        const int64_t wide = rel;
        memcpy(copy + i * DATA_SIZE + 16, &wide, 8);
    }
#endif
    fwrite(copy, 1, (size_t)(de - db), f);
    free(copy);
    fwrite(cb, 1, (size_t)(ce - cb), f);
    fwrite(nb, 1, (size_t)nnames, f);
    static const char zero[8];
    fwrite(zero, 1, (size_t)(-(int64_t)nnames & 7), f);
    fclose(f);
}

static DWORD WINAPI pgo_thread(void *arg) {
    const char *path = arg;
    char dump[MAX_PATH], reset[MAX_PATH];
    snprintf(dump, sizeof dump, "%s.dump", path);
    snprintf(reset, sizeof reset, "%s.reset", path);
    for (;;) {
        Sleep(500);
        if (GetFileAttributesA(reset) != INVALID_FILE_ATTRIBUTES) {
            memset(PGO_CNT_BEGIN, 0, (size_t)(PGO_CNT_END - PGO_CNT_BEGIN));
            DeleteFileA(reset);
        }
        if (GetFileAttributesA(dump) != INVALID_FILE_ATTRIBUTES) {
            pgo_write(path);
            DeleteFileA(dump);
        }
    }
    return 0;
}

/* (the renderer's copy reads a variable of its own, nx2/build.py: -DPGO_ENV="RX_VK_PGO_FILE") */
#ifndef PGO_ENV
#define PGO_ENV "HOS_PGO"
#endif
__attribute__((constructor)) static void pgo_start(void) {
    const char *e = getenv(PGO_ENV);
    if (e) CreateThread(NULL, 0, pgo_thread, _strdup(e), 0, NULL);
}
