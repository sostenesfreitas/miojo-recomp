/* (hookshot) The command a restart starts again (host/hos_relaunch.c, the Insert menu's Restart now), as plain C:
   on Linux the arguments and the environment the process started with, read from /proc/self/cmdline and
   /proc/self/environ (strings ended by a NUL each). tests/test_menu_restart.py checks it. */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

typedef struct {
    char *args, *env;      /* (the files' bytes, each string ended by a NUL) */
    char **argv, **envp;   /* (into them, each ended by a NULL) */
    int argc, envc;
} HosRelaunch;

/* A file's whole contents and one NUL more (a /proc file says it is empty: read until its end). NULL if it cannot be read */
static inline char *hos_relaunch_slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t n = 0, cap = 4096;
    char *b = (char *)malloc(cap + 1);
    for (size_t got; b && (got = fread(b + n, 1, cap - n, f)) > 0;) {
        n += got;
        if (n == cap) {
            char *more = (char *)realloc(b, cap * 2 + 1);
            if (!more) free(b);
            b = more, cap *= 2;
        }
    }
    fclose(f);
    if (!b) return NULL;
    b[n] = 0;
    *len = n;
    return b;
}

/* The strings in blob (len bytes, each ended by a NUL; blob[len] is a NUL too, for a last one without its own) as a
   NULL-ended array: their count, -1 out of memory */
static inline int hos_relaunch_split(char *blob, size_t len, char ***out) {
    int n = 0;
    for (size_t i = 0; i < len; i++)
        if (!blob[i] || i + 1 == len) n++;
    char **v = (char **)malloc(sizeof *v * (size_t)(n + 1));
    if (!v) return -1;
    int k = 0;
    for (size_t i = 0; i < len && k < n;) {
        v[k++] = blob + i;
        while (i < len && blob[i]) i++;
        i++;
    }
    v[k] = NULL;
    *out = v;
    return k;
}

/* The command from those two files (/proc/self/cmdline, /proc/self/environ): 1, or 0 when either cannot be read or
   there is no argument (argv[0]) */
static inline int hos_relaunch_command(const char *cmdline, const char *environ_file, HosRelaunch *r) {
    size_t an = 0, en = 0;
    memset(r, 0, sizeof *r);
    r->args = hos_relaunch_slurp(cmdline, &an);
    r->env = hos_relaunch_slurp(environ_file, &en);
    if (!r->args || !r->env || (r->argc = hos_relaunch_split(r->args, an, &r->argv)) < 1 ||
        (r->envc = hos_relaunch_split(r->env, en, &r->envp)) < 0) {
        free(r->args), free(r->env), free(r->argv), free(r->envp);
        memset(r, 0, sizeof *r);
        return 0;
    }
    return 1;
}

/* Windows' restart: the environment block the game started with (GetEnvironmentStringsW before hookshot_env.txt was read,
   strings ended by a NUL each and the block by one more), with name=value put in, in place of the same name in any case,
   at its place in the block's order (by name, case left out, as Windows keeps it). A new block (malloc), or NULL */
static inline int hos_relaunch_env_cmp(const wchar_t *a, const wchar_t *b) {   /* (two names, each ended by '=' or a NUL) */
    for (;; a++, b++) {
        const wint_t x = *a == L'=' ? 0 : towupper((wint_t)*a), y = *b == L'=' ? 0 : towupper((wint_t)*b);
        if (x != y || !x) return x < y ? -1 : x > y;
    }
}
static inline wchar_t *hos_relaunch_env_put(wchar_t *o, const wchar_t *name, const wchar_t *value) {
    const size_t n = wcslen(name), v = wcslen(value);
    memcpy(o, name, n * sizeof *o), o[n] = L'=', memcpy(o + n + 1, value, (v + 1) * sizeof *o);
    return o + n + 1 + v + 1;
}
static inline wchar_t *hos_relaunch_env_block(const wchar_t *saved, const wchar_t *name, const wchar_t *value) {
    size_t total = 0;
    for (const wchar_t *e = saved; e && *e; e += wcslen(e) + 1) total += wcslen(e) + 1;
    const size_t add = wcslen(name) + 1 + wcslen(value) + 1;
    wchar_t *out = (wchar_t *)malloc((total + add + 1) * sizeof *out), *o = out;
    if (!out) return NULL;
    int put = 0;
    for (const wchar_t *e = saved; e && *e; e += wcslen(e) + 1) {
        const int c = *e == L'=' ? -1 : hos_relaunch_env_cmp(e, name);   /* (=C: and the like stay first) */
        if (!put && c >= 0) {
            o = hos_relaunch_env_put(o, name, value);
            put = 1;
        }
        if (c == 0) continue;   /* (the old value of the name) */
        const size_t n = wcslen(e) + 1;
        memcpy(o, e, n * sizeof *o);
        o += n;
    }
    if (!put) o = hos_relaunch_env_put(o, name, value);
    *o = 0;
    return out;
}
