/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hos_report_run.h"
#include <stddef.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ntsecapi.h>
#else
#include <errno.h>
#include <sched.h>
#include <sys/random.h>
#endif

const char *hos_report_run_id(void) {
    static char id[33] = "unknown";
    static int state; /* 0: new, 1: generating, 2: immutable */
    int expected = 0;
    if (__atomic_compare_exchange_n(&state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        unsigned char bytes[16];
        int ok;
#ifdef _WIN32
        ok = RtlGenRandom(bytes, sizeof bytes) != 0;
#else
        size_t used = 0;
        while (used < sizeof bytes) {
            const ssize_t n = getrandom(bytes + used, sizeof bytes - used, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            used += (size_t)n;
        }
        ok = used == sizeof bytes;
#endif
        if (ok) {
            for (size_t i = 0; i < sizeof bytes; ++i) {
                id[i * 2] = "0123456789abcdef"[bytes[i] >> 4];
                id[i * 2 + 1] = "0123456789abcdef"[bytes[i] & 15];
            }
            id[32] = 0;
        }
        __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2) {
#ifdef _WIN32
            Sleep(0);
#else
            sched_yield();
#endif
        }
    }
    return id;
}

static int dbghelp_busy;
int hos_report_dbghelp_try(void) {
    int expected = 0;
    return __atomic_compare_exchange_n(&dbghelp_busy, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
void hos_report_dbghelp_done(void) {
    __atomic_store_n(&dbghelp_busy, 0, __ATOMIC_RELEASE);
}
