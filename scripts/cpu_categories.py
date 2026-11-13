"""Categories for scripts/cpu_report.py: where each sampled frame's CPU time belongs.

The rules are keyed on things that survive a rebuild and most upstream changes: the module, the object file (the source file
the linker map names) and the symbol. They never use addresses of our own binaries. All lifted game code is one
category, game/lifted; a finer split by game subsystem needs a map of the game binary, which stays on the developer's PC
(cpu_report.py --guest-map).

A frame no rule matches is "unclassified": cpu_report.py lists those, so a new upstream file shows up as a to-do in the
report instead of being counted somewhere wrong. Add a rule here when that happens.

Each rule is (module, object, symbol, category): regular expressions matched with re.search, None for "any". The first match
wins. A category is "top/sub" (one level of subcategory; with a local --guest-map the report adds the game's subsystem under game/).
"""

# (the system's own modules: on-CPU work there is charged, in the "owner" view, to the first frame of ours above it)
SYSTEM_MODULES = r"(?i)^(ntdll|kernelbase|kernel32|win32u|user32|gdi32|ucrtbase|msvcrt|vcruntime\d*|ws2_32|combase|rpcrt4|advapi32|sechost|bcrypt|libc\.so.*|libpthread.*|ld-linux.*|libm\.so.*)(\.dll)?$"
# (GPU drivers' user-mode modules)
DRIVER_MODULES = r"(?i)^(amdvlk64|amdxc64|amdxx64|amdvlk32|nvoglv64|nvwgf2umx|nvgpucomp64|nvlddmkm|igvk64|igxelpicd64|ig\w*icd64|vulkan-1|libvulkan.*|libvulkan_radeon.*|libnvidia.*|radv.*|libdrm.*)(\.dll|\.so.*)?$"
GAME_MODULES = r"^(hookshot\.exe|hookshot)$"
VK_MODULES = r"^(hookshot_vk\.dll|hookshot_vk\.so)$"

# (system calls that wait: a sample whose leaf is one of these is blocked, not on the CPU)
BLOCKING_SYSCALLS = r"^(Nt|Zw)(WaitFor|DelayExecution|RemoveIoCompletion|SignalAndWait|UserMsgWaitForMultipleObjects|UserGetMessage|ReadFile$|DeviceIoControlFile$|FsControlFile$|AlpcSendWaitReceivePort)|^(futex|syscall|__futex|poll|epoll_wait|nanosleep|clock_nanosleep|pthread_cond_wait|pthread_cond_timedwait|sem_wait|read|recv)$"

RULES = [
    # ---- runtime helpers compiled into the lifted code's own objects: by name first, so they are not counted as game ----
    (GAME_MODULES, None, r"^(a64_v?fpx|recomp_fpe|recomp_fp_estimate|recomp_fp)", "runtime/float"),
    (GAME_MODULES, None, r"^a64_.*(lookup|site_call|dispatch|resolve)", "runtime/indirect-calls"),
    (GAME_MODULES, None, r"^a64_.*(mark|excl|monitor|atomic)", "runtime/atomics-and-marks"),
    (GAME_MODULES, None, r"^(a64_|recomp_)", "runtime/other"),

    # ---- lifted game code (split further only by a local --guest-map) ----
    (GAME_MODULES, r"^code_", None, "game/lifted"),
    (GAME_MODULES, None, r"^f_[0-9a-fA-F]{8,16}$", "game/lifted"),

    # ---- the runtime that lifted code needs (third_party/a64-kit) ----
    (GAME_MODULES, r"^a64_hooks", None, "runtime/hooks"),
    (GAME_MODULES, r"^a64_", r"lookup|site_call|dispatch|resolve", "runtime/indirect-calls"),
    (GAME_MODULES, r"^a64_", r"fpx|fpe|fma|recp|rsqrt|fp_", "runtime/float"),
    (GAME_MODULES, r"^a64_", r"mark|excl|monitor|ldxr|stxr|cas|atomic", "runtime/atomics-and-marks"),
    (GAME_MODULES, r"^a64_", r"poll|yield|ckpt", "runtime/polls"),
    (GAME_MODULES, r"^a64_", None, "runtime/other"),

    # ---- the HLE OS layer (host/) ----
    (GAME_MODULES, r"^host_nn_os_sync", None, "os/sync"),
    (GAME_MODULES, r"^host_hos_thread", r"vc_|vcore|checkpoint", "os/virtual-cores"),
    (GAME_MODULES, r"^host_hos_thread", r"sleep|delay", "os/sleep-and-timers"),
    (GAME_MODULES, r"^host_hos_thread", None, "os/threads"),
    (GAME_MODULES, r"^host_hos_svc", None, "os/svc"),
    (GAME_MODULES, r"^host_hos_mem|^host_hos_main", r"mem|map|commit|protect|watch", "os/memory"),
    (GAME_MODULES, r"^host_(nvn_native|nvn_layout|nvdrv|nn_syncpt|rx_backend)", None, "os/nvn-bridge"),
    (GAME_MODULES, r"^host_nn_(audio|audio_in|codec)", None, "os/audio-services"),
    (GAME_MODULES, r"^host_(nn_video|mf_av1)", None, "os/video-decode"),
    (GAME_MODULES, r"^host_(nn_hid|pad_sdl|mouse_native|hos_sdl|display|prompts)", None, "os/input-and-window"),
    (GAME_MODULES, r"^host_nn_fs", None, "os/filesystem"),
    (GAME_MODULES, r"^host_nn_libc", None, "os/libc"),
    (GAME_MODULES, r"^host_(nn_vi|nn_gs)", None, "os/display-and-pacing"),
    (GAME_MODULES, r"^host_mkw_glue|^host_pgo_rt", r"sample|prof|pgo", "tooling/profilers"),
    (GAME_MODULES, r"^host_", None, "os/other"),

    # ---- other code linked into the exe ----
    (GAME_MODULES, r"^(avcodec|avutil|avformat|swscale|swresample):", None, "os/video-decode"),
    (GAME_MODULES, r"^(opus|libopus)|opus_", None, "os/audio-services"),
    (GAME_MODULES, r"^gpu_render_|^gpu_third_party_", None, "render/webgpu-fallback"),
    (GAME_MODULES, r"(?i)^(libcmt|libcpmt|libucrt|libvcruntime|msvcrt|msvcprt|libc\+\+|libunwind|libgcc|libstdc\+\+|libwinpthread)[:.]", None, "crt"),

    # ---- the Vulkan renderer (gpu/vk/) ----
    (VK_MODULES, r"(?i)^(libcmt|libcpmt|libucrt|libvcruntime|msvcrt|msvcprt|libc\+\+|libunwind|libgcc|libstdc\+\+|libwinpthread)[:.]", None, "crt"),
    (VK_MODULES, r"^vk_vk_(replay|jobs)", None, "render/replay-and-submit"),
    (VK_MODULES, r"^vk_vk_draw", r"descriptor|desc_|bind|layout", "render/descriptors"),
    (VK_MODULES, r"^vk_vk_draw", None, "render/draw-translation"),
    (VK_MODULES, r"^vk_vk_memory", r"capture|take_", "render/capture"),
    (VK_MODULES, r"^vk_vk_memory", r"wb_|write_?back|readback", "render/write-back"),
    (VK_MODULES, r"^vk_vk_memory|^vk_vk_vma|^vk_vk_xfer", None, "render/memory-and-mirrors"),
    (VK_MODULES, r"^vk_vk_texture|^vk_.*textures_|^vk_vk_formats", None, "render/textures"),
    (VK_MODULES, r"^vk_vk_shader|^vk_eden_|^vk_sirit|^vk_vk_bank|^vk_vk_coopmat", None, "render/shaders-and-pipelines"),
    (VK_MODULES, r"^vk_vk_(main|fps|predict|reflex|antilag)", None, "render/present-and-pacing"),
    (VK_MODULES, r"^vk_vk_(dlss|fsr3|fsr3_inputs|fsr3fi|fsr4|framegen|temporal|ngx)", None, "render/upscale-and-framegen"),
    (VK_MODULES, r"^vk_vk_(menu|imgui)|^vk_imgui_", None, "render/menu-and-overlays"),
    (VK_MODULES, r"^vk_vk_(gprof|dump|hangdump)", None, "tooling/renderer-diagnostics"),
    (VK_MODULES, None, None, "render/other"),

    # ---- outside our modules ----
    (DRIVER_MODULES, None, None, "driver"),
    (SYSTEM_MODULES, None, r"^(Nt|Zw)Alert|AlertThread|WakeByAddress|RtlWake|WakeConditionVariable|WakeAll", "system/wake"),
    (SYSTEM_MODULES, None, r"SleepConditionVariable|RtlpWait|WaitOnAddress|SRW|CriticalSection|Rtlp.*Lock", "system/sync"),
    (SYSTEM_MODULES, None, r"QueryPerformanceCounter|GetSystemTime|NtQuerySystemTime|clock_gettime", "system/clock"),
    (SYSTEM_MODULES, None, r"^(Nt|Zw)Yield|SwitchToThread|sched_yield", "system/yield"),
    (SYSTEM_MODULES, None, r"memcpy|memmove|memset|memcmp", "crt"),
    (SYSTEM_MODULES, None, r"Heap|malloc|free|operator new|operator delete", "system/heap"),
    (SYSTEM_MODULES, None, None, "system/other"),
]

# (no guest address ranges here: a map of the game's code is made from the game binary, so it stays local and is
# passed with cpu_report.py --guest-map)
