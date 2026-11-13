/* (hookshot) The Insert menu's Restart now (gpu/vk/vk_menu.cpp): it saves its options and asks the window to close for a
   restart (WM_APP + 9: host/display.c, host/posix/display_sdl.c). The window closes as when the player closes it, and at
   the very end of that, instead of only exiting, the game is started again:
     Linux: the process becomes the game again (execve) with the arguments and the environment it started with
     (/proc/self/cmdline and /proc/self/environ: hookshot_env.txt's lines are read again, not kept), so whatever waits
     for it (the launcher, a stream, a test) still has it. The game is bin/hookshot on disk, never run from inside the
     AppImage (hookshot-launch starts it with the AppImage's variables taken away), so /proc/self/exe is the game;
     argv[0] on the PATH if that is gone (the folder built again meanwhile).
     Windows: a new process of this executable (GetModuleFileNameW) with this one's command line (GetCommandLineW), in
     the same folder, with the environment this one started with (kept by hos_relaunch_keep_env before hookshot_env.txt
     was read: the file's lines are read again, and its edits since are taken, not left out as the environment's own);
     then this one exits. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "hos_relaunch.h"

static volatile LONG relaunch_asked;

void hos_relaunch_ask(void) { InterlockedExchange(&relaunch_asked, 1); }

#ifdef _WIN32
static wchar_t *start_env;   /* (the environment block at the start: hos_relaunch_keep_env) */

void hos_relaunch_keep_env(void) {
    wchar_t *b = GetEnvironmentStringsW();
    if (!b) return;
    size_t n = 0;
    while (b[n]) n += wcslen(b + n) + 1;
    start_env = (wchar_t *)malloc((n + 1) * sizeof *start_env);
    if (start_env) memcpy(start_env, b, (n + 1) * sizeof *start_env);
    FreeEnvironmentStringsW(b);
}

static void relaunch(void) {
    wchar_t exe[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH * 4);
    if (!n || n >= MAX_PATH * 4) {
        hos_log("restart: this executable's path was not found: the game only exits");
        return;
    }
    const wchar_t *line = GetCommandLineW();
    const size_t len = wcslen(line);
    wchar_t *cmd = (wchar_t *)malloc((len + 1) * sizeof *cmd);   /* (CreateProcessW may write into it) */
    if (!cmd) return;
    memcpy(cmd, line, (len + 1) * sizeof *cmd);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    /* (the new process waits for this one to end before it moves hookshot_log.txt to .1 and opens its own: this one still
       has it open until it exits, and a rename of an open file fails on Windows, so the new run overwrote the old run's
       log. hos_main.c reads HOS_RESTART_OF) */
    wchar_t pid[16];
    swprintf(pid, 16, L"%lu", (unsigned long)GetCurrentProcessId());
    SetEnvironmentVariableW(L"HOS_RESTART_OF", pid);
    wchar_t *env = start_env ? hos_relaunch_env_block(start_env, L"HOS_RESTART_OF", pid) : NULL;   /* (NULL: this one's) */
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, env ? CREATE_UNICODE_ENVIRONMENT : 0, env, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        hos_log("restart: the game could not be started again (error %lu): it only exits", (unsigned long)GetLastError());
        hos_log_flush();
    }
    free(cmd);
    free(env);
}
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

extern char **environ;

static void relaunch(void) {
    HosRelaunch r;
    if (!hos_relaunch_command("/proc/self/cmdline", "/proc/self/environ", &r)) {
        hos_log("restart: this process's command was not found: the game only exits");
        hos_log_flush();
        return;
    }
    /* (every file this process opened closes with it, as at an exit: the new one opens its own) */
    DIR *d = opendir("/proc/self/fd");
    if (d) {
        const int skip = dirfd(d);
        for (struct dirent *e; (e = readdir(d));) {
            const int fd = atoi(e->d_name);
            if (fd > 2 && fd != skip) fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
        }
        closedir(d);
    }
    fflush(NULL);
    execve("/proc/self/exe", r.argv, r.envp);
    environ = r.envp;   /* (execvp searches the PATH of the environment it is given: the one the game started with) */
    execvp(r.argv[0], r.argv);
    hos_log("restart: the game could not be started again: it only exits");
    hos_log_flush();
}
#endif

/* The window was closed (host/display.c's WM_CLOSE, host/posix/display_sdl.c's quit): the game exits, and starts again
   first when the menu asked for it */
void hos_window_closed_exit(void) {
    const int again = relaunch_asked != 0;
    hos_log(again ? "display: window closed for a restart (the Insert menu's Restart now)" : "display: window closed, exiting");
    hos_log_flush();
    if (again) relaunch();
    ExitProcess(0);
}
