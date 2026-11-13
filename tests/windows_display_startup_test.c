/* Mocks only: no windows, threads, logs or processes are created. */
#define _CRT_SECURE_NO_WARNINGS
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long DWORD;
typedef void *HANDLE;
typedef DWORD (*ThreadProc)(void *);
#define WINAPI
#define TRUE 1
#define FALSE 0
#define CreateWindowA(...) (window_result)
#define CreateEventA mock_CreateEventA
#define CreateThread mock_CreateThread
#define CloseHandle mock_CloseHandle
#define GetLastError mock_GetLastError
#define ExitProcess mock_ExitProcess
#define getenv mock_getenv
#define env_on(name) (getenv(name) && !env_value_off(getenv(name)))   /* (the switches read through the stand-in above) */

static jmp_buf process_exit;
static int failures;
#define CHECK(what, condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", what, __LINE__); failures++; } } while (0)

static int headless, event_calls, thread_calls, close_calls, flush_calls, exit_calls, renderer_calls;
static HANDLE event_result, thread_result, closed_handle;
static DWORD error_result, exit_code;
static ThreadProc thread_proc;
static void *thread_arg;
static char log_line[256];
static HANDLE renderer_up, hwnd, window_result;
static DWORD display_main(void *arg) { (void)arg; return 0; }

char *getenv(const char *name) {
    return !strcmp(name, "HOS_HEADLESS") && headless ? "1" : NULL;
}
HANDLE CreateEventA(void *security, int manual, int initial, const char *name) {
    (void)security; (void)manual; (void)initial; (void)name;
    event_calls++;
    return event_result;
}
HANDLE CreateThread(void *security, size_t stack, ThreadProc proc, void *arg, DWORD flags, DWORD *id) {
    (void)security; (void)stack; (void)flags; (void)id;
    thread_calls++; thread_proc = proc; thread_arg = arg;
    return thread_result;
}
int CloseHandle(HANDLE handle) { close_calls++; closed_handle = handle; return 1; }
DWORD GetLastError(void) { return error_result; }
void ExitProcess(unsigned int code) { exit_calls++; exit_code = code; longjmp(process_exit, 1); }
int rx_start(void *window) { (void)window; renderer_calls++; return 1; }
void hos_log(const char *format, ...) {
    va_list args; va_start(args, format); vsnprintf(log_line, sizeof log_line, format, args); va_end(args);
}
void hos_log_flush(void) { flush_calls++; }

/* ACTUAL_DISPLAY_STARTUP */

static void run_startup(void (*start)(void)) {
    if (!setjmp(process_exit)) start();
}

static void reset(void) {
    headless = event_calls = thread_calls = close_calls = flush_calls = exit_calls = renderer_calls = 0;
    event_result = (HANDLE)1; thread_result = (HANDLE)2; window_result = (HANDLE)3;
    hwnd = NULL; error_result = 0; exit_code = 0;
    thread_proc = NULL; thread_arg = NULL; closed_handle = NULL; log_line[0] = 0;
}
static void failed(const char *operation, DWORD error) {
    CHECK("process exits nonzero", exit_calls == 1 && exit_code != 0);
    CHECK("failure flushes log", flush_calls == 1);
    CHECK("operation is logged", strstr(log_line, operation) != NULL);
    char expected[32]; snprintf(expected, sizeof expected, "%lu", error);
    CHECK("captured error is logged", strstr(log_line, expected) != NULL);
}
int main(void) {
    reset(); headless = 1; run_startup(hos_display_start);
    CHECK("headless starts only offscreen renderer", renderer_calls == 1 && event_calls == 0 && thread_calls == 0);

    reset(); event_result = NULL; error_result = 101; run_startup(hos_display_start);
    failed("CreateEventA", 101);
    CHECK("event failure stops before thread", thread_calls == 0);

    reset(); thread_result = NULL; error_result = 202; run_startup(hos_display_start);
    failed("CreateThread", 202);
    CHECK("thread failure follows event creation", event_calls == 1);

    reset(); run_startup(hos_display_start);
    CHECK("successful startup stays asynchronous", event_calls == 1 && thread_calls == 1 && thread_proc != NULL);
    CHECK("successful startup does not exit", exit_calls == 0);
    CHECK("owned thread handle is closed", close_calls == 1 && closed_handle == thread_result);
    reset(); window_result = NULL; error_result = 303; run_startup(create_window);
    failed("CreateWindowA", 303);

    reset(); run_startup(create_window);
    CHECK("successful window creation does not exit", exit_calls == 0 && hwnd == window_result);

    if (failures) return 1;
    puts("PASS: Windows display startup API failure and success controls");
    return 0;
}
