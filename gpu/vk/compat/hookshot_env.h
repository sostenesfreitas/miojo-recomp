/* (hookshot) One rule for the game's on/off switches, in C (host/) and C++ (gpu/vk, its translator):

     env_on(name)           unset, empty, 0, false, off or no (any case): off; anything else, 1 or a value: on
     env_int(name, def)     the number it is set to; def when it is unset, empty or not a number

   Before this a switch read with getenv(name) != NULL was on for any value, 0 included: RX_VK_NO_PROGRAM_CACHE=0 left the
   cache out and HOS_HEADLESS=0 opened no window. Read once where it matters (a static): getenv walks the environment. */
#pragma once

/* (Windows' CRT marks getenv deprecated (C4996, clang's -Wdeprecated-declarations) unless _CRT_SECURE_NO_WARNINGS is set
   when its headers first come in: the checks paste this file ahead of code that sets it itself, so it is set here for
   that first include and taken back after, leaving a file's own #define of it no redefinition) */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#define HOOKSHOT_ENV_CRT_SET
#endif
#include <stdlib.h>
#ifdef HOOKSHOT_ENV_CRT_SET
#undef _CRT_SECURE_NO_WARNINGS
#undef HOOKSHOT_ENV_CRT_SET
#endif

#ifndef HOOKSHOT_ENV_H   /* (and a guard: the checks paste this file before the code they cut out) */
#define HOOKSHOT_ENV_H

#ifdef __cplusplus
#define HOOKSHOT_ENV_INLINE inline
#define HOOKSHOT_ENV_BOOL bool   /* (C++: a bool, so bool x{env_on(...)} is no narrowing) */
#else
#define HOOKSHOT_ENV_INLINE static inline
#define HOOKSHOT_ENV_BOOL int
#endif

/* (whether a value says off: "", "0", "false", "off" or "no", any case, spaces around it not counted) */
HOOKSHOT_ENV_INLINE HOOKSHOT_ENV_BOOL env_value_off(const char *v) {
    static const char *const off[] = {"0", "false", "off", "no"};
    const char *a = v, *b;
    unsigned i, k;
    while (*a == ' ' || *a == '\t') a++;
    b = a;
    while (*b) b++;
    while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r' || b[-1] == '\n')) b--;
    if (a == b) return 1;
    for (i = 0; i < sizeof off / sizeof off[0]; i++) {
        const char *w = off[i];
        for (k = 0; a + k < b && w[k]; k++)
            if ((a[k] | 0x20) != w[k]) break;
        if (a + k == b && !w[k]) return 1;
    }
    return 0;
}
HOOKSHOT_ENV_INLINE HOOKSHOT_ENV_BOOL env_on(const char *name) {
    const char *v = getenv(name);
    return v && !env_value_off(v);
}
HOOKSHOT_ENV_INLINE int env_int(const char *name, int def) {
    const char *v = getenv(name);
    char *end;
    long n;
    if (!v || !*v) return def;
    n = strtol(v, &end, 0);
    return end == v ? def : (int)n;
}
#endif
