/* (linux) The part of the Win32 API Hookshot's runtime and Vulkan back end use, on POSIX: host/posix is on the include
   path of Linux builds only (nx2/build.py), so `#include <windows.h>` finds this file and the Windows code paths stay
   as they are. Implemented in win32_posix.c (threads, events, fibers, files, memory, modules, exceptions, waveOut) and
   display_sdl.c (the window, keyboard, mouse).

   Not a general emulation: what is here is what the code calls, with the behaviour it relies on. Handles are
   pointers to objects of this file's own (threads, events, timers, files, find searches); the guest window is one
   mapping (hos_main.c), so committing memory is a no-op and decommitting zeroes it. */
#ifndef HOOKSHOT_POSIX_WINDOWS_H
#define HOOKSHOT_POSIX_WINDOWS_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- types ---- */
typedef int BOOL;
typedef unsigned char BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int64_t LONG64, LONGLONG;
typedef uint64_t ULONG64, ULONGLONG, DWORD64, DWORDLONG;
typedef uintptr_t DWORD_PTR, ULONG_PTR, UINT_PTR, WPARAM;
typedef intptr_t LONG_PTR, INT_PTR, LPARAM, LRESULT;
typedef size_t SIZE_T;
typedef unsigned int UINT;
typedef int INT;
typedef short SHORT;
typedef unsigned short USHORT;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef void *HANDLE, *HMODULE, *HINSTANCE, *HWND, *HDC, *HGDIOBJ, *HBITMAP, *HFONT, *HBRUSH, *HICON, *HCURSOR, *HKEY;
typedef void *PVOID, *LPVOID;
typedef const void *LPCVOID;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef wchar_t *PWSTR, *LPWSTR;
typedef const wchar_t *PCWSTR, *LPCWSTR;
typedef DWORD *LPDWORD;
typedef LONG HRESULT;
typedef DWORD COLORREF;
typedef void (*FARPROC)(void);
typedef union { struct { DWORD LowPart; LONG HighPart; } u; int64_t QuadPart; } LARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct { LONG cx, cy; } SIZE;

#define WINAPI
#define CALLBACK
#define APIENTRY
#define WINAPIV
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
#define INFINITE 0xFFFFFFFFu
#define S_OK 0
#define S_FALSE 1
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr) ((HRESULT)(hr) < 0)
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define LOWORD(l) ((WORD)((uintptr_t)(l) & 0xffff))
#define HIWORD(l) ((WORD)(((uintptr_t)(l) >> 16) & 0xffff))
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define __declspec(x) HOOKSHOT_DECLSPEC_##x
#define HOOKSHOT_DECLSPEC_dllexport __attribute__((visibility("default")))
#define HOOKSHOT_DECLSPEC_dllimport
#define HOOKSHOT_DECLSPEC_noinline __attribute__((noinline))
#define HOOKSHOT_DECLSPEC_thread __thread
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strdup strdup
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _mkdir(p) mkdir((p), 0755)
#define _getpid getpid
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#define _environ environ
#define _fileno fileno
#define _chsize_s(fd, size) ftruncate((fd), (off_t)(size))
#define _atoi64 atoll
#define _mkgmtime timegm
/* (MSVC's argument order: the result first; 0 on success) */
static inline int gmtime_s(struct tm *out, const time_t *t) { return gmtime_r(t, out) ? 0 : EINVAL; }
static inline int localtime_s(struct tm *out, const time_t *t) { return localtime_r(t, out) ? 0 : EINVAL; }

/* ---- errors (GetLastError: errno mapped to the codes the callers test) ---- */
#define ERROR_SUCCESS 0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_NO_MORE_FILES 18L
#define ERROR_HANDLE_EOF 38L
#define ERROR_NOT_SUPPORTED 50L
#define ERROR_FILE_EXISTS 80L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_COMMITMENT_LIMIT 1455L
#define ERROR_NOT_ALL_ASSIGNED 1300L
#define ERROR_TIMEOUT 1460L
#define ERROR_DEVICE_NOT_CONNECTED 1167L
DWORD GetLastError(void);
void SetLastError(DWORD e);

/* ---- atomics (type-generic, as MSVC's are per width) ---- */
#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedIncrement64 InterlockedIncrement
#define InterlockedDecrement64 InterlockedDecrement
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchange64 InterlockedExchange
#define InterlockedExchangePointer InterlockedExchange
#define InterlockedExchangeAdd(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd64 InterlockedExchangeAdd
#define InterlockedAdd(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedAdd64 InterlockedAdd
#define InterlockedOr(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedAnd(p, v) __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedOr64 InterlockedOr
#define InterlockedAnd64 InterlockedAnd
#define InterlockedCompareExchange(p, x, c) __sync_val_compare_and_swap((p), (c), (x))
#define InterlockedCompareExchange64 InterlockedCompareExchange
#define InterlockedCompareExchangePointer InterlockedCompareExchange
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define _ReadWriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define YieldProcessor() __builtin_ia32_pause()

/* ---- locks: SRW locks are POSIX rwlocks (zero-initialised is valid, as on Windows); condition variables a futex
   word, so they wait with either an SRW lock or a critical section ---- */
typedef pthread_rwlock_t SRWLOCK, *PSRWLOCK;
#define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
typedef struct { volatile uint32_t seq; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT {0}
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1
typedef pthread_mutex_t CRITICAL_SECTION, *LPCRITICAL_SECTION;
static inline void InitializeSRWLock(SRWLOCK *l) { pthread_rwlock_init(l, NULL); }
static inline void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_wrlock(l); }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_unlock(l); }
static inline void AcquireSRWLockShared(SRWLOCK *l) { pthread_rwlock_rdlock(l); }
static inline void ReleaseSRWLockShared(SRWLOCK *l) { pthread_rwlock_unlock(l); }
static inline BOOL TryAcquireSRWLockExclusive(SRWLOCK *l) { return pthread_rwlock_trywrlock(l) == 0; }
static inline BOOL TryAcquireSRWLockShared(SRWLOCK *l) { return pthread_rwlock_tryrdlock(l) == 0; }
void InitializeCriticalSection(CRITICAL_SECTION *cs);
static inline BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION *cs, DWORD n) { (void)n; InitializeCriticalSection(cs); return TRUE; }
static inline void EnterCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_lock(cs); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_unlock(cs); }
static inline BOOL TryEnterCriticalSection(CRITICAL_SECTION *cs) { return pthread_mutex_trylock(cs) == 0; }
static inline void DeleteCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_destroy(cs); }
static inline void InitializeConditionVariable(CONDITION_VARIABLE *cv) { cv->seq = 0; }
void WakeConditionVariable(CONDITION_VARIABLE *cv);
void WakeAllConditionVariable(CONDITION_VARIABLE *cv);
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, ULONG flags);
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms);
BOOL WaitOnAddress(volatile void *addr, void *compare, SIZE_T size, DWORD ms);
void WakeByAddressSingle(void *addr);
void WakeByAddressAll(void *addr);

/* ---- time ---- */
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000LL; return TRUE; }
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *c) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    c->QuadPart = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}
static inline ULONGLONG GetTickCount64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000 + (ULONGLONG)ts.tv_nsec / 1000000;
}
static inline DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }
void Sleep(DWORD ms);
static inline BOOL SwitchToThread(void) { return sched_yield() == 0; }
static inline UINT timeBeginPeriod(UINT p) { (void)p; return 0; }
static inline UINT timeEndPeriod(UINT p) { (void)p; return 0; }
void GetSystemTimeAsFileTime(FILETIME *ft);
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#define TIMER_ALL_ACCESS 0x1F0003
HANDLE CreateWaitableTimerExW(void *attr, LPCWSTR name, DWORD flags, DWORD access);
BOOL SetWaitableTimerEx(HANDLE t, const LARGE_INTEGER *due, LONG period, void *fn, void *arg, void *ctx, ULONG delay);

/* ---- handles, threads, events ---- */
typedef DWORD (*LPTHREAD_START_ROUTINE)(void *);
#define WAIT_OBJECT_0 0u
#define WAIT_ABANDONED 0x80u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define STILL_ACTIVE 259
#define CREATE_SUSPENDED 0x4
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000
#define THREAD_PRIORITY_IDLE -15
#define THREAD_PRIORITY_LOWEST -2
#define THREAD_PRIORITY_BELOW_NORMAL -1
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define DUPLICATE_SAME_ACCESS 0x2
#define THREAD_ALL_ACCESS 0x1FFFFF
#define THREAD_QUERY_INFORMATION 0x40
#define THREAD_QUERY_LIMITED_INFORMATION 0x800
#define THREAD_SUSPEND_RESUME 0x2
#define THREAD_GET_CONTEXT 0x8
#define THREAD_SET_CONTEXT 0x10
#define SYNCHRONIZE 0x100000
#define EVENT_ALL_ACCESS 0x1F0003
HANDLE CreateThread(void *attr, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *id);
HANDLE GetCurrentThread(void);
static inline HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
static inline DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
DWORD GetCurrentThreadId(void);
DWORD GetThreadId(HANDLE t);
DWORD GetProcessId(HANDLE p);
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD id);
BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *out, DWORD access, BOOL inherit, DWORD options);
BOOL CloseHandle(HANDLE h);
BOOL SetThreadPriority(HANDLE t, int p);
int GetThreadPriority(HANDLE t);
DWORD_PTR SetThreadAffinityMask(HANDLE t, DWORD_PTR mask);
BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *proc, DWORD_PTR *sys);
HRESULT SetThreadDescription(HANDLE t, PCWSTR name);
HRESULT GetThreadDescription(HANDLE t, PWSTR *name);
BOOL GetThreadTimes(HANDLE t, FILETIME *create, FILETIME *exit, FILETIME *kernel, FILETIME *user);
DWORD SuspendThread(HANDLE t);
DWORD ResumeThread(HANDLE t);
BOOL GetExitCodeThread(HANDLE t, DWORD *code);
__attribute__((noreturn)) void ExitThread(DWORD code);
__attribute__((noreturn)) void ExitProcess(UINT code);
BOOL TerminateProcess(HANDLE p, UINT code);
HANDLE CreateEventA(void *attr, BOOL manual, BOOL initial, LPCSTR name);
#define CreateEvent CreateEventA
BOOL SetEvent(HANDLE e);
BOOL ResetEvent(HANDLE e);
HANDLE CreateSemaphoreA(void *attr, LONG initial, LONG max, LPCSTR name);
BOOL ReleaseSemaphore(HANDLE s, LONG n, LONG *prev);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms);
void *LocalFree(void *p);

/* fibers: ucontext, each on its own stack */
#define FIBER_FLAG_FLOAT_SWITCH 0x1
typedef void (*LPFIBER_START_ROUTINE)(void *);
void *ConvertThreadToFiberEx(void *param, DWORD flags);
void *ConvertThreadToFiber(void *param);
BOOL ConvertFiberToThread(void);
void *CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, void *param);
void *CreateFiber(SIZE_T stack, LPFIBER_START_ROUTINE fn, void *param);
void SwitchToFiber(void *fiber);
void DeleteFiber(void *fiber);
void *GetCurrentFiber(void);
void *GetFiberData(void);

/* processors */
typedef enum { RelationProcessorCore = 0, RelationNumaNode = 1, RelationCache = 2, RelationProcessorPackage = 3 } LOGICAL_PROCESSOR_RELATIONSHIP;
typedef struct {
    ULONG_PTR ProcessorMask;
    LOGICAL_PROCESSOR_RELATIONSHIP Relationship;
    union { struct { BYTE Flags; } ProcessorCore; ULONGLONG Reserved[2]; };
} SYSTEM_LOGICAL_PROCESSOR_INFORMATION, *PSYSTEM_LOGICAL_PROCESSOR_INFORMATION;
BOOL GetLogicalProcessorInformation(SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info, DWORD *len);
typedef struct {
    WORD wProcessorArchitecture, wReserved;
    DWORD dwPageSize;
    void *lpMinimumApplicationAddress, *lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
    WORD wProcessorLevel, wProcessorRevision;
} SYSTEM_INFO;
void GetSystemInfo(SYSTEM_INFO *si);

/* ---- strings ---- */
#define CP_ACP 0
#define CP_UTF8 65001
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int n, LPWSTR w, int cap);
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR w, int n, LPSTR s, int cap, LPCSTR def, BOOL *used);
void OutputDebugStringA(LPCSTR s);
#define OutputDebugString OutputDebugStringA

/* ---- memory ---- */
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define MEM_FREE 0x10000
#define MEM_PRIVATE 0x20000
#define MEM_MAPPED 0x40000
#define MEM_IMAGE 0x1000000
#define MEM_RESET 0x80000
#define MEM_WRITE_WATCH 0x200000
#define MEM_LARGE_PAGES 0x20000000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_WRITECOPY 0x08
#define PAGE_GUARD 0x100
typedef struct {
    void *BaseAddress, *AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State, Protect, Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;
void *VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(void *addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(void *addr, SIZE_T size, DWORD protect, DWORD *old);
SIZE_T VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T size);
static inline SIZE_T GetLargePageMinimum(void) { return 0; }

/* ---- modules: dlopen; ".dll" names map to the shared libraries (win32_posix.c) ---- */
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x2
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x4
HMODULE LoadLibraryA(LPCSTR name);
#define LoadLibrary LoadLibraryA
BOOL FreeLibrary(HMODULE m);
FARPROC GetProcAddress(HMODULE m, LPCSTR name);
HMODULE GetModuleHandleA(LPCSTR name);
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out);
DWORD GetModuleFileNameA(HMODULE m, LPSTR out, DWORD cap);
BOOL SetCurrentDirectoryA(LPCSTR dir);
DWORD GetCurrentDirectoryA(DWORD cap, LPSTR out);

/* ---- files: paths may use '\' (it is made '/') ---- */
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x1
#define FILE_SHARE_WRITE 0x2
#define FILE_SHARE_DELETE 0x4
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_HIDDEN 0x2
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400   /* (a symbolic link: find_next sets it) */
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define MOVEFILE_REPLACE_EXISTING 0x1
#define MOVEFILE_COPY_ALLOWED 0x2
#define MOVEFILE_WRITE_THROUGH 0x8
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define FILE_TYPE_UNKNOWN 0
#define FILE_TYPE_DISK 1
#define FILE_TYPE_CHAR 2
#define FILE_TYPE_PIPE 3
typedef struct {
    ULONG_PTR Internal, InternalHigh;
    union { struct { DWORD Offset, OffsetHigh; }; PVOID Pointer; };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, void *attr, DWORD disposition, DWORD flags, HANDLE tmpl);
#define CreateFile CreateFileA
BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, OVERLAPPED *ov);
BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *put, OVERLAPPED *ov);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *pos, DWORD method);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size);
DWORD GetFileSize(HANDLE h, DWORD *high);
BOOL SetEndOfFile(HANDLE h);
BOOL FlushFileBuffers(HANDLE h);
DWORD GetFileType(HANDLE h);
HANDLE GetStdHandle(DWORD which);
BOOL GetConsoleMode(HANDLE h, DWORD *mode);
DWORD GetFileAttributesA(LPCSTR path);
#define GetFileAttributes GetFileAttributesA
BOOL CreateDirectoryA(LPCSTR path, void *attr);
BOOL RemoveDirectoryA(LPCSTR path);
BOOL DeleteFileA(LPCSTR path);
BOOL MoveFileA(LPCSTR from, LPCSTR to);
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd);
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd);
BOOL FindClose(HANDLE h);
static inline BOOL AllocConsole(void) { return FALSE; }
static inline BOOL FreeConsole(void) { return TRUE; }
static inline BOOL SetConsoleTitleA(LPCSTR t) { (void)t; return TRUE; }

/* ---- exceptions: vectored handlers and the unhandled filter on SIGSEGV / SIGBUS / SIGILL / SIGFPE ---- */
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001Du
#define EXCEPTION_INT_DIVIDE_BY_ZERO 0xC0000094u
#define EXCEPTION_STACK_OVERFLOW 0xC00000FDu
#define EXCEPTION_BREAKPOINT 0x80000003u
#define EXCEPTION_IN_PAGE_ERROR 0xC0000006u
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_MAXIMUM_PARAMETERS 15
typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode, ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    void *ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;
typedef struct {
    DWORD64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15, Rip;
    DWORD EFlags, ContextFlags;
    DWORD64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;   /* (no thread's context is read or set here: GetThreadContext fails) */
} CONTEXT, *PCONTEXT;
#define EXCEPTION_SINGLE_STEP 0x80000004u
#define CONTEXT_DEBUG_REGISTERS 0x100010
#define CONTEXT_FULL 0x10000B
#define CONTEXT_CONTROL 0x100001
#define CONTEXT_INTEGER 0x100002
typedef struct { EXCEPTION_RECORD *ExceptionRecord; CONTEXT *ContextRecord; } EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;
typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS *);
typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS *);
void *AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h);
ULONG RemoveVectoredExceptionHandler(void *h);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f);
BOOL GetThreadContext(HANDLE t, CONTEXT *c);
BOOL SetThreadContext(HANDLE t, const CONTEXT *c);
/* (no SEH unwind tables: the host backtraces of crash reports stop at the faulting frame) */
typedef struct { DWORD BeginAddress, EndAddress, UnwindData; } RUNTIME_FUNCTION, *PRUNTIME_FUNCTION;
#define UNW_FLAG_NHANDLER 0
static inline PRUNTIME_FUNCTION RtlLookupFunctionEntry(DWORD64 pc, DWORD64 *base, void *table) { (void)pc, (void)table; *base = 0; return NULL; }
static inline void *RtlVirtualUnwind(DWORD type, DWORD64 base, DWORD64 pc, PRUNTIME_FUNCTION f, CONTEXT *c, void **hd,
                                     DWORD64 *frame, void *ptrs) {
    (void)type, (void)base, (void)pc, (void)f, (void)c, (void)hd, (void)frame, (void)ptrs;
    return NULL;
}

/* ---- the window, keyboard and screen (display_sdl.c) ---- */
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_NUMPAD0 0x60
#define VK_NUMPAD2 0x62
#define VK_NUMPAD4 0x64
#define VK_NUMPAD6 0x66
#define VK_NUMPAD8 0x68
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
/* the window's key state: 0x8000 while down (display_sdl.c fills it; the Vulkan back end reads the executable's) */
SHORT GetAsyncKeyState(int vk);
SHORT GetKeyState(int vk);
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
int GetSystemMetrics(int index);
HWND GetForegroundWindow(void);
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define WS_EX_TOOLWINDOW 0x80
#define WS_EX_NOACTIVATE 0x08000000
static inline LONG_PTR GetWindowLongPtrA(HWND w, int i) { (void)w, (void)i; return 0; }
/* (the Insert menu's Fullscreen row, gpu/vk/vk_menu.cpp: WM_APP + 7 to the window sets it, GWL_STYLE without WS_CAPTION reads it) */
#define WM_CLOSE 0x0010
#define WM_APP 0x8000
#define WS_CAPTION 0x00C00000L
BOOL PostMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp);
LONG GetWindowLongA(HWND w, int index);
static inline int GetClassNameA(HWND w, LPSTR s, int n) { (void)w; if (n > 0) s[0] = 0; return 0; }
DWORD GetWindowThreadProcessId(HWND w, DWORD *pid);
int MessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type);
#define MB_OK 0
#define MB_ICONERROR 0x10
#define MessageBox MessageBoxA
#ifndef MB_TOPMOST
#define MB_TOPMOST 0x00040000L
#endif
#ifndef MB_ICONWARNING
#define MB_ICONWARNING 0x00000030L
#endif

#ifdef __cplusplus
}
#endif
#endif
