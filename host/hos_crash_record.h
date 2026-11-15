/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* No renderer ABI: pending/crash-<UTC>.txt is the boundary. All strings copied by
   the crash writer are bounded; arbitrary fatal messages and memory are omitted. */
typedef struct HosCrashContext {
    const char *reason;                /* a fixed diagnostic label, never a formatted message */
    const char *thread;
    uint32_t code;
    int signal_number, signal_code;
    uintptr_t pc, sp, fp, fault_address;
    uintptr_t guest_base;
    uint64_t guest_size;
    const uint64_t *guest_registers;    /* x0..x31; read without dereferencing a bad pointer */
    void *native_context;             /* Windows CONTEXT when available */
    int exception_filter;             /* only this path actually requests the opt-in dump */
} HosCrashContext;

/* Normal startup, before installing handlers. dir is the game folder, not cwd.
   log_path is the selected stderr destination before rotation, or NULL when not
   redirecting. Failure (read-only folder etc.) disables recording without affecting the game. */
void hos_crash_record_start(const char *dir, const char *version, const char *log_path);
/* Explicit process-exit sites (ExitProcess/_exit do not run atexit). Never removes
   a report; only this process's running marker and its empty staging file. */
void hos_crash_clean_end(void);
/* Normal startup, after the version/run-id banner: at most one fixed pruning warning. */
void hos_crash_record_log_notes(void);
/* Failing-thread path: fixed buffers, OS reads/writes, no allocation or stdio.
   The first caller wins; reentry and competing faults never overwrite a record. */
int hos_crash_record_write(const HosCrashContext *context); /* 1 only for the publishing caller */
/* mkw_glue.c: adapt a normal fatal call, including hos_fatal and renderer FATAL. */
void hos_crash_record_fatal(const uint64_t *guest_registers, const char *reason);

/* Bounded equivalent of the report redactor's home/user handling, stricter for
   single-line diagnostic fields: known user/host identities also removed outside
   paths. Startup snapshots the private strings; never calls getenv in a handler. */
size_t hos_crash_redact(char *out, size_t cap, const char *text, size_t size);
#ifdef _WIN32
/* After the text is durable: request the startup-created, opt-in dump worker.
   exception is EXCEPTION_POINTERS; the worker uses a copied exception/context. */
void hos_crash_record_dump(void *exception);
#endif
