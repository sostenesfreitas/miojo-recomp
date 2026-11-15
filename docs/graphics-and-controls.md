# Settings

Everything you can change is in Hookshot's in-game menu: graphics, sound, gameplay and controls. Pick a starting point
below, then look up any single setting in the list at the bottom.

## Open the menu

| | Open or close the menu | Move around it |
|---|---|---|
| **Keyboard** | <kbd>F1</kbd>, <kbd>Insert</kbd> or <kbd>Esc</kbd> | <kbd>Q</kbd> and <kbd>E</kbd> (or <kbd>Tab</kbd> and <kbd>Shift</kbd>+<kbd>Tab</kbd>) switch tabs, the arrow keys choose and change, <kbd>Esc</kbd> goes back |
| **Controller** | **Home**, or **Select** and **Start** together (or click the touchpad on a DualShock 4 or DualSense) | LB and RB switch tabs, the D-pad chooses and changes, A opens a page, B goes back |
| **Mouse** | | Click a tab or a row, click the left or right half of a value to lower or raise it, right-click to go back |

<kbd>F1</kbd> always works. **Controls → Menu shortcut** can take <kbd>Esc</kbd> or <kbd>Insert</kbd> off the list, for
example to leave <kbd>Insert</kbd> to another overlay.

Your choices are saved as you go, in `hookshot_options.ini` in the game folder. A setting marked **After a restart**
applies the next time the game starts: when you close the menu it asks whether to restart. **Later** keeps the change
for next time; **Restart now** starts the game again, and you lose any progress since your last save.

## Good starting points

A first start uses the **Auto (this PC)** quality preset. It picks settings from your graphics card and its video memory,
then adjusts the render resolution during the first minute of play. If you change nothing, that is what you get. The
Output resolution starts at the game's own 2560x1440, or at your screen's size where that is smaller (a Steam Deck's
1280x800, a 1080p monitor), in your screen's shape: a 2560x1080 ultrawide starts at 2560x1080 and a 2256x1504 laptop at
2256x1504, pixel for pixel. 2048x1152, marked "(reduced)", is the game's own smaller picture, lighter on the GPU.

| Your PC | Quality preset | Upscaler and render resolution | Frame rate cap | Frame generation |
|---|---|---|---|---|
| **Steam Deck, handheld, integrated GPU or a 4 to 6 GB card** | Auto (this PC) picks Minimum (4GB); Low (6GB) on a 6 GB card | Game's own, as the preset sets it, or AMD FSR 3.1 at Performance | 60 | Off |
| **Most PCs (8 GB card)** | Auto (this PC) | What Auto chooses: NVIDIA DLSS on an RTX card, AMD FSR 4 on an RX 9000 where the build has it, otherwise the game's own | 60 | Off |
| **A fast PC with a high-refresh monitor** | High (12GB) or Ultra (16GB+) | DLSS or FSR 4 at Quality, or Native (DLAA or FSR 4 Native AA) for the sharpest picture | 60 | 2x or Auto, up to your monitor's refresh rate |

- **Keep the frame rate cap at 60.** Caps from 90 to 240 and Uncapped are experimental: some bosses and cutscenes
  still count frames and can misbehave. For smoother motion, use frame generation instead.
- **Frame generation** adds frames between the game's own. Motion is smoother, but the game itself doesn't run any
  faster and input delay goes up a little. **Auto** matches your display's refresh rate. On an NVIDIA RTX 40 card or
  newer, with DLSS, DLAA or FSR as the upscaler, NVIDIA DLSS Frame Generation makes the frames; otherwise Hookshot's
  built-in frame generation does. **NVIDIA Reflex** and **AMD Anti-Lag 2** cut the added delay; they only appear on
  cards that support them.
- **DLSS and DLAA look softer** than the game's own upscaler: set **Sharpness** to around 80% with them.
- **Ultrawide and 4:3:** leave **Aspect ratio** on your monitor's shape (the default). The 3D fills the screen while the
  HUD and movies stay in the middle.
- **HDR:** turn on **HDR** (Expanded or Native), then set **HDR peak brightness** to your monitor's.

## Every setting

The menu's own descriptions, with the choices and defaults from the [menu reference](../README.md#in-game-menu). Labels on
a setting say when it applies: **After a restart**, only on some GPUs or builds, only while HDR is on, or set for you by a
quality preset.

<!-- graphics-settings -->

## Change one thing at a time

When something looks or runs wrong, change a single setting and compare the same place in the game. To report a
performance problem, follow the [performance steps](../README.md#low-fps-or-stutter); for bindings and controller
layouts, see the [controls reference](../README.md#controls).
