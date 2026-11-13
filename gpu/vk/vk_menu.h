// The Insert menu (vk_menu.cpp): options that apply while the game runs, kept in hookshot_options.ini beside the executable.
#pragma once
#include "auto_settings.h"

#include "vk_internal.h"

namespace mkwvk {

// (out of the box every feature is off: what the game does on its own. The ini beside the executable keeps a player's choices)
struct Options {
    int framegen = 0;     // made frames: 0 off, 1 Auto (to the display's refresh), or 2 to 8 times the game's frames
    int framegen_by = 0;  // (hookshot) who makes them at 2x and up: 0 Auto (NVIDIA DLSS Frame Generation where it runs, else the
                          // renderer's own), 1 the renderer's own (vk_framegen.cpp; vk_dlss.cpp dlss_fg_wanted)
    int pace = 0;         // frames shown on an even line (vk_framegen.cpp)
    int vsync = 0;        // (hookshot) V-Sync, the swapchain's present mode (vk_main.cpp create_swapchain, made again at once): 0 Auto
                          // (mailbox where the driver has it), 1 On (FIFO), 2 Off (immediate). RX_VK_FIFO / RX_VK_IMMEDIATE over it
    int hdr = 0;          // (hookshot) HDR10 out (vk_framegen.cpp framegen_hdr): 0 off, 1 the SDR picture expanded, 2 the game's own (native) HDR; when the display has it (the swapchain made again at once)
    int hdr_white = 200;  // HDR paper white, nits (the picture's white)
    int hdr_peak = 1000;  // HDR peak brightness, nits (its brightest highlights)
    int hdr_contrast = 100;   // HDR contrast in percent around mid grey, as RenoDX grades it; 100 none (vk_framegen.cpp framegen_hdr)
    int hdr_tonemap = 0;  // (hookshot) the scene's tone mapper with Native HDR: 0 the game's (Vanilla), 1 PsychoV35, as RenoDX's zelda-oot-oz
                          // (ir_opt/scene_tonemap_pass.h; vk_shader.cpp scene_tonemap_for)
    int fps_cap = 0;      // (hookshot) Frame rate cap: 0 60 (the console's), 90, 120, 144, 165, 240, 6 Uncapped (host/nn_vi.c reads frame_rate_cap= at the start)
    int resolution = 0;   // the 3D's (Render resolution): 0 Auto (the game's dynamic resolution), else a share of the output,
                          // 1..6 = 100%, 75%, 67%, 50%, 58%, 33% (upscaler_rows.h; nx2/oot_patches.py)
    int output_res = 0;   // the game's whole frame, docked: 0 its 2048x1152, then the sizes the screen offers (vk_menu.cpp outputs,
                          // display_modes.h), then the player's own (custom_output); saved as the size (output_res=3840x2160) and read
                          // once, at the start (host/nn_applet.c)
    int upscale = 0;      // to the window (Window scaling filter, saved as upscaling): 0 off (the plain copy), 1 Catmull-Rom, 2 Lanczos
    int reflex = 0;       // NVIDIA Reflex: 0 off, 1 on, 2 on + boost (vk_reflex.cpp; NVIDIA GPUs with VK_NV_low_latency2)
    int antilag = 0;      // AMD Anti-Lag 2: 0 off, 1 on (vk_antilag.cpp; AMD GPUs with VK_AMD_anti_lag)
    int dlss = 0;         // the Upscaler (its name from before FSR: options files and the tests' stand-ins keep it): 0 the game's
                          // own network, 1 NVIDIA DLSS, 2 NVIDIA DLAA (a build with NVIDIA's SDK only), 3 AMD FSR 3.1 (vk_fsr3.cpp),
                          // 4 AMD FSR 4, 5 its native AA (a build with FSR 4 only). upscaler_rows.h; vk_temporal.cpp. Saved as dlss
    int dlss_model = 0;   // DLSS's network, by its preset: 0 K (transformer), 1 M, 2 L (second-generation transformer), 3 NVIDIA's
                          // default (upscaler_rows.h model_preset; vk_dlss.cpp). RX_VK_DLSS_PRESET over it
    int sharpness = 0;    // 0 to 100
    int rt = 0;           // (miojo) Ray tracing: the scene as acceleration structures (gpu/vk/vk_rt.cpp); read once, at the start
    int gi = 0;           // (miojo) Bounce light's strength in percent, 0 off: one traced bounce (vk_rt.cpp), with Ray tracing on
    int ambient = 0;      // (miojo) Traced ambient's strength in percent, 0 off: the game's ambient light shaped by how much sky a place sees (rt_ao.comp), with Ray tracing on
    int lights = 0;       // (miojo) Light shadows' strength in percent, 0 off: a point light's shadow traced (vk_rt.cpp), with Ray tracing on
    int sun = 0;          // (miojo) Sun shadows' strength in percent, 0 the game's own shadow maps: traced (vk_rt.cpp), with Ray tracing on
    int ao = 0;           // (miojo) ambient occlusion's strength in percent, 0 off (gpu/vk/shaders/dlss_prep.comp: with a temporal upscaler)
    int anisotropy = 4;   // (hookshot) Anisotropic filtering: 0 the game's own (1x or 2x), 1 2x, 2 4x, 3 8x, 4 16x (the
                          // device's limit at most; gpu/vk/vk_draw.cpp aniso_override, RX_VK_ANISO over it)
    int realtime = 1;     // game time follows the clock (host/nn_vi.c); 0: per frame (slower frames, slower game)
    int handheld = 0;     // the game's handheld profile: ~4 fps, softer; read once, at the start (host/nn_applet.c)
    int ultrawide = 1;    // the Aspect ratio row. Saved as ultrawide=, read once, at the start (host/hos_main.c): 1 the monitor's own
                          // shape (the default), 0 the game's 16:9 (an older file's 0 on a screen narrower than 16:9 moves to 1,
                          // options_version 6: host/hos_wide_size.h hos_aspect_on). In the menu, from menu_init on: a place in
                          // vk_menu.cpp's output_shapes, the shapes of the sizes offered, the monitor's own marked; another shape than
                          // 16:9 and the monitor's is output_res's own (outputs_sync, save)
    int wide_menus = 1;   // ultrawide: the pause, item and map menus 0 at full width (stretched), 1 in the middle 16:9 with the sides
                          // blurred, 2 with black bars (nx2/oot_wide.py, vk_framegen.cpp)
    int show_fps = 0;     // (hookshot) the FPS counter: 0 Off, 1 FPS, 2 Frame time, 3 Graph, 4 Breakdown (fps_stats.h; saved as fps_counter=, an old On is 1)
    int fps_corner = 0;   // (hookshot) its corner: 0 top right, 1 top left, 2 bottom right, 3 bottom left
    int menu_shortcut = 2;   // 0 Insert or F1, 1 F1 only, 2 Insert, F1 or Esc (F1 always opens the menu). 2 by default, as a PC
                             // game's pause menu; an older file's 0, the default it then was, moves to 2 (options_version 5)
    int compact_menu_key = 0; // 0 Right Shift, 1 Right Ctrl (key to switch to or open the compact menu)
    int menu_size = 2;   // (hookshot) the overlays' size: 1 Small, 2 Medium, 3 Large (gpu/vk/vk_ui.h unit; an older file's 0 Auto: Medium)
    int fullscreen = 0;   // borderless, at the monitor's size: F11 is the same setting
    int gpu_device = 0;   // graphics card index: 0 Auto, 1..N device 0..N-1 (gpu/vk/vk_main.cpp; RX_VK_DEVICE)
    int pad_background = 0;   // the controllers count while another window has the focus (host/nn_hid.c; HOS_INPUT_BACKGROUND)
    int mouse_native = 0; // the mouse turns the camera's own angles (host/mouse_native.c); 0: as the right stick
    int disable_camera_centering = 0; // 0: stock movement-follow turning; 1: keep the player's horizontal direction
    int mouse_sens = 100; // the mouse's sensitivity in percent: 0.1 degrees a count at 100 with native look (host/nn_hid.c)
    int mouse_invert = 0; // the mouse's up and down swapped
    int text_speed = 0;   // how fast dialogue prints: 0 the game's, 1 2x, 2 4x, 3 instant (host/nn_vi.c, nx2/oot_patches.py; HOS_TEXT_SPEED)
    int fov = 100;        // vertical gameplay field of view, 50..150% of the game's current angle (host/oot_fov.c)
    int text_size = 0;    // dialogue and signs' text size: 0 the game's, 1 110%, 2 125%, 3 150%, 4 175% (host/oot_text_size.c,
                          // nx2/oot_patches.py; HOS_TEXT_SIZE)
    int camera_speed = 100;   // (hookshot) the right stick's camera turn in percent of the game's, 50 to 400 (host/nn_vi.c, nx2/oot_patches.py: HOS_OPT_CAMERA_SPEED, 48)
    int invert_cam_x = 0; // right-stick camera horizontal inversion: 0 normal, 1 inverted
    int invert_cam_y = 0; // right-stick camera vertical inversion: 0 normal, 1 inverted
    int rumble = 100;     // the controllers' rumble in percent of the game's: 0 Off, 25 to 150 (host/pad_sdl.c)
    int stick_dead_zone = 0;  // circular stick cutoff: 0 XInput's 7849 (~24%), 1..5 5% to 30% (host/nn_hid.c)
    int pad_layout = 0;   // 0 Nintendo (default), 1 Xbox / PC, 2 Swap Face Buttons, 3 Custom
    int gyro = 1;         // motion controls from a pad's gyroscope (host/pad_sdl.c, host/hos_motion.h); on by default
    int gyro_sens_x = 100;   // the motion aim's horizontal sensitivity in percent
    int gyro_sens_y = 100;   // the motion aim's vertical sensitivity in percent
    int gyro_inv_x = 0;   // the motion aim's horizontal turned around
    int gyro_inv_y = 0;   // the motion aim's vertical turned around
    int gyro_deadzone = 1;   // the motion's deadzone: 0 tight, 1 standard (the console's default), 2 loose
    int prompts = 0;      // the button icons: 0 Auto (the controller), 1 Switch, 2 Xbox, 3 PS4, 4 PS5, 5 Steam Deck (host/prompts.c)
    int robust = 0;       // bounds-checked buffer access: 0 Auto (on), 1 Off, 2 On (gpu/vk/vk_main.cpp; RX_VK_ROBUST)
    int descriptor_reuse = 1;  // (on by default since options_version 4) a draw keeps its descriptors through other slots' registrations (gpu/vk/vk_draw.cpp; RX_VK_DESC_REUSE)
    int sampler_cache = 0;     // sampler ids answer their parameters without the registrations' lock (gpu/vk/vk_draw.cpp; RX_VK_SAMPLER_CACHE)
    int astc_rgba8 = 0;   // (hookshot) big ASTC textures kept as decoded RGBA8, not re-encoded to BC3 (gpu/vk/vk_texture.cpp bc_ready;
                          // RX_VK_ASTC_RGBA8 over it): the AMD texture workaround. Read once, at the start
    int preset = 0;       // quality preset: 0 Custom, 1 Minimum (4GB), 2 Low (6GB), 3 Balanced (8GB), 4 High (12GB), 5 Ultra (16GB+),
                          // 6 Auto (this PC), kAutoPreset (its tier's budgets: budget)
    int vram_mb = 0;      // dynamic texture cache budget limit in MB
    int mirror_mb = 0;    // staging mirror limit in MB
    int budget = 0;       // the preset whose memory budgets (vram_mb, mirror_mb) hold: kept when another row turns the preset
                          // to Custom, so tuning Resolution after Low (6GB) keeps its cap. Saved as memory_budget; 0: none
    // (hookshot) the VRAM budget row: 0 Auto (the renderer's own rule), 1..7 a texture budget of 2.2, 3.6, 4.8, 6, 7.5, 9 or 11 GB
    // and its mirrors (vram_mb, mirror_mb come from it). A preset sets it; set on the row it is the player's (budget 0)
    int vram_budget = 0;
    int audio_output = 0; // (hookshot) the speakers the game mixes for: 0 Auto (the default output's), 1 Stereo, 2 5.1; read once, at the start (host/nn_audio.c; HOS_AUDIO_CHANNELS)
    int discord = 1;   // (hookshot) the Discord status (host/discord_rpc.c): 1 on, shown while a Discord client runs on this PC
    int debug_tools = 0;   // (hookshot) the Debug tab's tools (#785: flight, free camera, teleport, inventory): 0 off; HOS_OOT_CHEATS=1 also turns them on
    int volume = 100;     // (hookshot) the game's sound in percent, 0 to 100 (host/nn_audio.c: HOS_OPT_VOLUME, 27)
    int mute_background = 0;   // (hookshot) no sound while another window has the focus (host/nn_audio.c: HOS_OPT_MUTE_BACKGROUND, 28)
    int skip_opening = 0;   // (hookshot) skip the new-game opening (host/hos_main.c hos_oot_opening: HOS_OPT_SKIP_OPENING, 52). Off
    int game_lang = 0;    // the game's language (vk_menu_i18n.h game_lang_code): language.txt's, not kept in the options file
    int menu_lang = 0;    // this menu's language: 0 Auto (the game's own, language.txt), else menu_i18n::kLangs[menu_lang - 1]
                          // (vk_menu_i18n.cpp). Saved as menu_language
    int mic = 1;          // (hookshot) the ocarina's microphone: 0 Off, 1 the system's default, 2.. a device as host->mic_name lists it (host/nn_audio_in.c)
};
extern Options opts;
constexpr int kAutoPreset = 6;   // (the Quality preset Auto (this PC): menu_auto_for_gpu, auto_settings.h)
/// (vk_memory.cpp) true when the quality preset's memory budgets (vram_mb, mirror_mb) are to be used: the card has no more
/// video memory than the preset is for. A 4GB preset on an 8 GB card kept 256 MB of mirrors and 2.2 GB of textures: the
/// game's memory was read over PCIe and the textures made again every frame, 5 fps with the CPU idle (Discord, 11-08)
bool preset_caps_memory();

void menu_init();             // (the options from the file, once)
void menu_auto_for_gpu(const VkPhysicalDeviceProperties& props, ::auto_settings::Hardware hw);   // (the card chosen: Auto's profile)
void menu_auto_tune(double gpu_ms, double draws, double fps);   // (the replay's 120-present windows: Auto (this PC) measured in play)
/// (hookshot) The V-Sync row as this run applies it: RX_VK_FIFO=1 is On and RX_VK_IMMEDIATE=1 Off, whatever the row says
/// (for the run only: the row keeps the player's choice). 0 Auto, 1 On, 2 Off
int menu_vsync();
void menu_set_gpu_count(int count);
std::string driver_text();   // (vk_main.cpp: the driver as it names itself, for the log and Save my pipeline list)
void menu_tick();             // (each frame shown: the keys, while the game's window has the focus)
bool menu_is_open();
/// (hookshot) The game's scene tone mapped by PsychoV35: HDR set to Native on an HDR display, and the Tone mapper row so
bool scene_tonemap_on();
bool resolution_auto();       // (the game's dynamic resolution: Resolution Auto)
int resolution_scale();       // (the fixed path's scale in 1/10000: the Resolution row's, as a twelfth of the output)
void menu_option_set(int id, int value);   // (from the game's side: 7 the mouse's sensitivity, the - and = keys)
int menu_setting(int index, MkwVkSetting* out);   // (the control bridge, host/control.c: the rows with a value)
int menu_setting_set(const char* key, int value, MkwVkSetting* out);
int menu_heart_protection();
int menu_player_flight();
int menu_infinite_ammo();
int menu_time_lock();
int menu_moon_jump();
int menu_free_camera();
int menu_link_speed();   // (Movement speed in percent, 100 while the tools are off)
int menu_take_cheat_action();
void menu_note_frame(bool game_frame, int shown = 1);   // (the frame rates the FPS counter shows)
/// The game's picture's size (the Output resolution, its crop) and the window's, at each present: the Window scaling filter
/// row says when it is not needed
void menu_note_present_sizes(uint32_t picture_w, uint32_t picture_h, uint32_t window_w, uint32_t window_h);
/// Over the window's image `index` (TRANSFER_DST_OPTIMAL before and after), w x h: the start-up screen, the menu and the FPS
/// counter, drawn with Dear ImGui (vk_imgui.h, vk_ui.h). Nothing recorded while none of them shows
void menu_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h);
/// (headless shots, RX_VK_SHOT_MENU) The menu and the counter over another image: TRANSFER_DST_OPTIMAL before and after,
/// R8G8B8A8_UNORM with colour-attachment use, view a view of it
void menu_record_image(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h);
/// (hookshot) The start-up screen while the shaders are prepared (vk_main.cpp, vk_progress.h), drawn by menu_record under the
/// counter and the menu. Null takes it away.
struct StartupView;
void startup_screen(const StartupView* view);
/// The start-up screen's fade to black as it ends: 0 none, 1 black (vk_main.cpp prepare_with_screen)
void startup_fade_to(float f);

} // namespace mkwvk
