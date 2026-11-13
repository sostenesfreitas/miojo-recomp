# The control bridge

A local socket inside the game process that tools, tests and coding agents use to drive a running game and read its
state. Today they write presses into a file the game polls (`HOS_INPUT_LIVE`), take screenshots with `RX_VK_SHOTS` and
read the game's state from the log (the `vk: N presents, F fps; a frame: D draws` line). The bridge does the same things
through one connection, with replies. Presses are timed in frames, and every reply carries the frame it was made at.

- The game side is `host/control.c`. Hooks: `host/nn_hid.c` (the controller's read), `gpu/vk/vk_replay.cpp` (shots at a
  present), `gpu/vk/vk_menu.cpp` (the Insert menu's settings, through its own setter), `host/hos_thread.c` (log lines),
  `host/hos_main.c` (the start, and for OoT the current map and the debug teleport), `host/hos_mem.c` (guest memory's
  bounds), `host/nn_fs.c` (the save folder), `host/rx_backend.c` (the renderer's side, `MkwVkRenderer`).
- The client is `scripts/hookshot_ctl.py` (standard library only).
- Tests: `tests/test_control.py` (the bridge against a fake game) and `tests/test_hookshot_ctl.py` (the client against a
  fake bridge).

## Turning it on

```
HOS_CONTROL=/run/user/1000/hookshot/control.sock ./hookshot
```

It is off unless `HOS_CONTROL` is set. When it is on, the log says so once:
`control: listening on <path> (HOS_CONTROL: ...)`. On Windows the value is a named pipe, `\\.\pipe\<name>`; a bare name
gets that prefix.

`hookshot_ctl.py launch <game folder>` starts a game folder this way. It makes a run folder of its own (mode 0700) and
puts the socket (`control.sock`), the log (`log.txt`) and the screenshots (`shots/`) there. It waits until the bridge
answers, then prints the socket's path.

## Security

- **Opt-in.** There is no socket unless `HOS_CONTROL` names one.
- **Local only.** There is a Unix domain socket on Linux and a named pipe on Windows, and never TCP. The pipe refuses
  remote clients.
- **This user only.** The socket is created with mode 0600 (the umask is set around `bind` too), and a client whose
  `SO_PEERCRED` user differs from the game's is turned away. The pipe's DACL grants the current user alone.
  `FILE_FLAG_FIRST_PIPE_INSTANCE` keeps another process from owning the name first.
- **A folder of this user's.** The socket's folder has to be owned by the game's user and writable by no one else
  (not a shared `/tmp`): otherwise the bridge stays off and the log says why. Another user could otherwise put a link
  or a socket of theirs where the bridge binds, removes a stale socket, or writes a shot by a relative path.
  `hookshot_ctl.py launch` makes such a folder (0700, in `$XDG_RUNTIME_DIR`). The path is always a file system path:
  there is no abstract socket name.
- **Nothing in the way is replaced.** A file at the path that is not a socket is left alone and the bridge stays off. A
  socket that a live game answers on is left alone too. A stale one, left by a game that ended, is removed.
- **Guest memory is a second opt-in.** It is off unless `HOS_CONTROL_MEM` is set, apart from `HOS_CONTROL`:
  `HOS_CONTROL_MEM=ro` allows reads, `rw` allows reads and writes (aliases: `r`/`read` for `ro`, `readwrite` for `rw`;
  case does not matter; any other value logs once and leaves it off). See [Guest memory](#guest-memory).
- **What a client can do.** Anything the player can do with a controller and the Insert menu's settings (not its debug
  tools). It can also write a PNG to any path the game's user can write, list the save folder, and end the game.
- **Bounded messages.** A request line is at most 64 KB (longer: `too_long`, and the rest of the line is skipped); an
  `id` at most 200 characters as JSON; at most 8 clients, 64 requests waiting, 256 inputs queued, 8 memory requests of
  1 MB each. Replies a client does not read are dropped with the client once 8 MB wait.
- **The game never waits for a client.** The bridge's own thread does all the socket I/O. The controller's read takes
  one short lock that the bridge never holds over I/O. A client that sends nonsense gets errors. A client that stops
  reading is dropped once 8 MB waits for it, and one that goes away mid-command has its presses let go.

**The client trusts the game no further than it must.** `hookshot_ctl.py` may run outside a sandbox the game is in, so
what it does with the game's replies is checked: `slots --thumbs` takes only digit slot names and a bounded slice of
that slot's `caption.sav`, and plain output shows control characters from the game (a log line's escape sequences) as
`?`. `launch --save` refuses a save folder that holds links or anything but plain files and folders: a save from
someone else could hold a link to one of this user's files, which the copy would read and the game then write through.

**With a Landlock sandbox of the game's process** (an open proposal, #882): the bridge's socket is made before the
restriction (it starts with the game), so the sandbox need not allow making sockets. The bridge's thread is started
before the restriction too, so it is covered only where the restriction covers every thread at once (Landlock ABI 8,
`LANDLOCK_RESTRICT_SELF_TSYNC`); on an older kernel it would stay outside the sandbox, like the window's main thread
there, and a game taken over could act through it. If both land, start the bridge after the restriction (with its
folder allowed) or keep it off under the sandbox without ABI 8. The other way round there is nothing to gain: a client
already runs as the game's user outside the sandbox, so reaching the game through the bridge gives it nothing it did not
have. The socket's folder must stay writable for shots taken there.

## Protocol

The protocol is newline-delimited JSON. Each request is one JSON object on one line, at most 64 KB. Each reply is one
line, and it gives back the request's `id` (any JSON value; `null` when there is none) and the current `frame` (the
game's presents so far):

```
> {"id": 1, "cmd": "status"}
< {"id":1,"ok":true,"frame":5321,"protocol":1,"fps":59.8,"state":"play","stalled":false,"draws":2410.3,...}
```

Requests on one connection may be answered out of order: a `status` sent while a `wait` is pending is answered first.
Match replies by `id`. Lines with an `"event"` key and no `"ok"` key are subscription events (see `subscribe`).

A client may shut its sending side once its requests are sent (`printf '...\n' | socat - UNIX-CONNECT:<path>`): the
bridge reads no more from it, ends its subscription, answers what it waits for, and closes the connection once those
replies are out. (Linux; a Windows pipe has no half-close.)

### Errors

```
< {"id":7,"ok":false,"frame":5400,"error":"bad_request","message":"frames: 1 to 216000"}
```

| error | when |
|---|---|
| `bad_json` | the line is not JSON (`id` is `null`) |
| `too_long` | a line over 64 KB; the rest of it is skipped and the connection goes on |
| `bad_request` | not an object, no `cmd`, or a bad argument |
| `unknown_cmd` | no such command |
| `timeout` | a `wait`, `shot` or input did not finish before its `timeout` (seconds) |
| `busy` | too many requests waiting (64), inputs queued (256) or clients (8) |
| `unavailable` | the renderer is not up yet, or is not the Vulkan one (shots, settings); or a memory command that is not served |
| `forbidden` | a memory command while `HOS_CONTROL_MEM` does not allow it |
| `out_of_range` | a memory address or range outside mapped guest memory |
| `not_found` | no such setting |
| `failed` | a shot could not be written |
| `refused` | the game turned a teleport down (an event playing, a map loading, ...); the message says why |
| `cancelled` | a press that was waited on was let go by `release`, or a teleport was replaced by a later one |

### Frames and inputs

A frame is one present of the game's own; frames made by frame generation do not count. An input starts at the first
controller read after its request arrives, plus `at` frames, and is held for `frames` frames. The game reads the
controller once a frame, so `"frames": 8` is 8 game frames. The bridge's buttons are added to the player's. On the
axes it sets, its sticks replace a script's (`HOS_INPUT_SCRIPT`); the player's own pad, keys and mouse still count after
it, the larger deflection winning, unless the input is exclusive. `"exclusive": true` keeps the player's own pad, keyboard and mouse out from
the request until the input ends (compare `HOS_INPUT_EXCLUSIVE`).

Buttons use the names `HOS_INPUT_SCRIPT` uses: `A B X Y L R ZL ZR PLUS MINUS LS RS LEFT UP RIGHT DOWN`. The stick
directions are `LUP LDOWN LLEFT LRIGHT RUP RDOWN RLEFT RRIGHT`, and `LX= LY= RX= RY=` set an axis from -32767 to 32767
(positive y is up). Names are given as `"A+B"` or `["A", "B"]`.

The reply comes once the input is over, with the frames it covered: `{"start": 5400, "end": 5408}` (`end` is the first
frame without it). With `"wait": false` the reply is immediate: `{"queued": 1, "frames": 8}`.

### Commands

**status**: the frame, the frame rate over the last second, the draws a frame over the last ~300 ms, the state, the
renderer's presents (`replayed`: what a shot's `present` counts in), the picture's size, the map the game is in, the
version.

```
> {"id":1,"cmd":"status"}
< {"id":1,"ok":true,"frame":5321,"protocol":1,"fps":59.8,"state":"play","stalled":false,"draws":2410.3,"replayed":5320,
   "window":{"width":1280,"height":720},"uptime":95.2,"clients":1,"input":{"queued":0,"exclusive":false},
   "map":"KokiriForest_Child","game":{"event":false,"loading":false,"age":"child"},"version":"1.4.0-abc123",
   "renderer":"Vulkan: ..."}
```

`map` is the game's own name for the map (`Sequence_CurrentBanc`), read on the game's thread with the player's update
every 250 ms: the last one known, so it stays through loads and menus, and it is `null` until Link was first seen. `game`
(OoT, read with it) says whether an event is playing (a cutscene, a map change's), whether a map is loading
(`Sequence_IsLoadingBanc`) and Link's age: `{"event":false,"loading":false,"age":"child"}`, `null` until then. A script
that steps through places waits for `event` and `loading` to be false before its next step, instead of a log line.

`state` is a guess from the draws a frame, the way test scripts read the log:

| state | meaning |
|---|---|
| `boot` | nothing presented yet |
| `intro` | under 11 draws, before any menu was seen |
| `menu` | 11 to 149 draws |
| `loading` | under 11 draws, after a menu or play |
| `play` | 150 or more |
| `unknown` | no Vulkan renderer to count draws |

A new state has to hold for 300 ms before it is taken. `stalled` is true when no frame has come for a second.

**press**: `buttons`, `frames` (default 8), `at`, `exclusive`, `wait`, `timeout`.

```
> {"id":2,"cmd":"press","buttons":"A","frames":8}
< {"id":2,"ok":true,"frame":5410,"start":5401,"end":5409}
```

**stick**: `stick` (`L` default, or `R`), `x` and `y` from -1 to 1, then as press. Buttons can go with it.

```
> {"id":3,"cmd":"stick","stick":"L","x":0,"y":1,"frames":120,"exclusive":true}
```

**sequence**: `steps`, a list of press or stick objects each with its own `at`. Or `script`, `HOS_INPUT_SCRIPT`'s syntax
in frames, `<at>:<names>[/<frames held>],...` (8 frames held unless given). With `"units": "hos"` the script's times are
in seconds and its holds in milliseconds, as `HOS_INPUT_SCRIPT` and `HOS_INPUT_LIVE` take them (60 frames a second; 200
ms held unless given). The reply comes when the last step is over: `start` is the first step's start and `end` the last
step's end.

```
> {"id":4,"cmd":"sequence","script":"0:A/8,60:A/8"}
> {"id":5,"cmd":"sequence","script":"0:LUP/2000,2:A","units":"hos"}
> {"id":6,"cmd":"sequence","steps":[{"at":0,"buttons":"ZL","frames":30},{"at":10,"lx":-1,"frames":20}]}
```

**release**: the caller's queued inputs are let go, and those that clients which have gone left behind (sent with
`"wait": false`): `{"released": 3, "all": false}`. Another connected client's inputs stay. With `"all": true`, every
client's. Presses that were waited on reply `cancelled`.

An input sent with `"wait": false` stays queued after its client goes (so a one-shot `hookshot_ctl.py press --no-wait`
works) and is freed once it is over. An input that is waited on is let go when its client goes.

**shot**: a PNG of the first present after the request, or after frame `after`. It is the size `RX_VK_SHOTS` writes
(1280x720, or `RX_VK_SHOT_W` x `RX_VK_SHOT_H`); the reply's `width` and `height` are read from the file. `path` defaults to `shots/shot_<n>_f<frame>.png` in the socket's folder (made 0700). A relative path is
taken from the socket's folder, not the game's (on Windows, where a pipe has no folder, from `%TEMP%\hookshot-control-<pipe name>\`, which also holds `shots\`). A shot is the game's own picture: the Insert menu and the FPS counter, drawn over it on the way to the window, are not in it. The file is written whole under another name, then renamed, so the
path in the reply is complete. The `timeout` default is 30 s. Shots need the Vulkan renderer.

```
> {"id":7,"cmd":"shot","path":"/tmp/run/kokiri.png"}
< {"id":7,"ok":true,"frame":5440,"present":5432,"path":"/tmp/run/kokiri.png","width":1280,"height":720}
```

**wait**: `event`, plus a `timeout` in seconds (default 60). The reply has the state and the frames waited:
`{"state": "play", "waited": 1203}`. A timeout replies `timeout`, with the state at that time.

| event | until |
|---|---|
| `present` | `frames` more frames (default 1) |
| `frame` | frame number `frame` |
| `in_play` | the state is `play` (at once if it is) |
| `state` | the state is `state` |
| `loading_start` | the next change into `loading` |
| `loading_end` | the next change out of `loading` |
| `change` | the next change of state |
| `map` | the map is `map` (any case; at once if it is); without `map`, the next change of map. The reply has `map` |

**option_get**: the Insert menu's settings, or one of them by `name`. A name is the setting's key in
`hookshot_options.ini` (`text_speed`) or its row's name (`"Text speed"`), in any case, with `-` taken as `_`. The
settings are the rows with a value that the menu shows on this PC (a row for what the GPU cannot run is not listed),
not the section headings and not the Debug tab's tools. The bridge finds a row by its key each time: a key stays the
same when rows come and go or are renamed, a place in the list does not. `restart` marks a row that takes effect at the
next start; `fixed`, one with nothing to choose on this PC (the menu shows it greyed).

```
> {"id":8,"cmd":"option_get","name":"text_speed"}
< {"id":8,"ok":true,"frame":5500,"option":{"key":"text_speed","name":"Text speed","tab":"gameplay","value":0,
   "min":0,"max":3,"step":1,"restart":false,"fixed":false,"text":"Normal"}}
```

**option_set**: `name` and `value` (a number, or `true`/`false`). The value is set through the menu's own setter, as
Left and Right set it: cut to the row's range, the Quality preset turned to Custom unless the row is one it leaves
alone, a restart noted for the restart question, and kept in `hookshot_options.ini`. The rows the arrows step through
a list of their own (the Upscaler, Render resolution, the output resolution) are walked in the arrows' order until they
reach the value; one that none of their steps reach on this PC leaves them, the Quality preset and the rest as they
were, and replies `failed`. A `fixed` row replies `failed` too. The reply has the value it had before and the setting as it is now. The menu's lock keeps a change apart from the menu's own frame.

```
> {"id":9,"cmd":"option_set","name":"FPS counter","value":true}
< {"id":9,"ok":true,"frame":5510,"was":0,"option":{"key":"fps_counter",...,"value":1,"text":"On"}}
```

**subscribe**: a stream of events on this connection until `unsubscribe` or a disconnect; another `subscribe` replaces
it. Arguments:

- `events`: any of `present`, `state`, `log` and `map`. The default is `["present", "state"]`.
- `every`: frames between present events (default 60, 1 to 216000).
- `log`: only log lines that contain this text (it also turns `log` on).

The events carry `sub`, the subscribe request's `id`:

```
< {"event":"present","sub":10,"frame":5600,"fps":59.9,"state":"play","draws":2405.0}
< {"event":"state","sub":10,"frame":5720,"state":"loading","from":"play","draws":5.0}
< {"event":"log","sub":10,"frame":5721,"line":"[hos sead::MainThread] ..."}
< {"event":"map","sub":10,"frame":5900,"map":"HyruleField_Child","from":"KokiriForest_Child"}
< {"event":"log_dropped","sub":10,"lines":40}
```

**teleport** (OoT): the Debug tab's teleport, by destination: `to`, the destination's index or one of its maps' names
(any case). Without `to`, the destinations: `{"destinations":[{"index":0,"child":"KokiriForest_Child","adult":
"KokiriForest_Adult"},...]}` (a destination with no map for an age has `null` there). It is what the Cheats page's
Teleport list asks for: the game takes it at the player's next update, in play, and goes to the map of Link's age.

The reply comes once the game took it (within `timeout`, default 10 s):

- `"sent": true` and `map`, the map it goes to. `wait map` then tells when it arrived. `accepted` is the game's
  event manager's own answer to the request: `false` was seen when the map then did not change (an arrival event of
  the previous teleport still playing).
- `refused`, with the game's reason: an event playing (a cutscene, a map change's), a map loading, another teleport
  in the last 8 s, or no map for Link's age there.
- `timeout` when the game did not take it: Link is not being updated (the title, the file select, a pause, a load).
  The request is then withdrawn, so it does not happen later behind the client's back.
- `cancelled` when a later `teleport` replaced it before the game took it (one waits at a time).

With `"wait": false` the reply is at once, with `"sent": null`. The game logs each teleport (`cheats: teleport ...`).
It changes the game as the menu's teleport does, and a save made after it keeps the new place.

```
> {"id":12,"cmd":"teleport","to":"HyruleField_Child"}
< {"id":12,"ok":true,"frame":6003,"teleport":1,"child":"HyruleField_Child","adult":"HyruleField_Adult","sent":true,
   "map":"HyruleField_Child"}
> {"id":13,"cmd":"wait","event":"map","map":"HyruleField_Child","timeout":30}
< {"id":13,"ok":true,"frame":6190,"state":"loading","waited":187,"map":"HyruleField_Child"}
```

**age** (OoT): the Debug tab's Switch age, so a script can reach the adult's maps (or the child's) without a save of
that age: `to`, `"child"` or `"adult"`, none the other age. The game takes it as it takes a teleport, at the player's
next update in play, and does it as the menu does: Link's age flipped and the current map reloaded in the other age
(the Market's, when this map has no destination). It uses the teleport's tickets, so a later teleport or age switch
replaces one not taken yet (`cancelled`), and the replies are the teleport's:

- `"age"` (the age asked for), `"sent": true` and `map`, the map it reloads; `wait loading_end` then tells when it
  arrived. Not `wait map`: in a dungeon the map has the same name for both ages (`ForestTemple`), so a wait for it is met
  before the reload starts. `accepted` as for a teleport.
- `"sent": false` when Link is that age already: nothing is done. The game checks the age asked for when it takes it, so
  an old sample of Link's age in `status` (it is read every 250 ms, and not during a load) never flips him the wrong way.
  With no `to`, the other age is decided from that sample.
- `refused`, with the game's reason (as a teleport's, and Link's age not found or not written in the game's data);
  `timeout` when the game did not take it (not in play); `unavailable` before a save is loaded (the age is not known).

With `"wait": false` the reply is at once, with `"sent": null`. The game logs it (`cheats: age toggle: ...`).

```
> {"id":14,"cmd":"age","to":"adult"}
< {"id":14,"ok":true,"frame":7310,"age":"adult","sent":true,"map":"KokiriForest_Adult","accepted":true}
```

**quit**: the reply, then the game exits as when its window is closed (the log flushed, exit code 0). The socket is
removed.

## Saves

**saves** (no opt-in needed: it lists files, reads no game memory): the folder the game's saves are mounted from
(`<data>/hookshot/save/<user id>` on Linux), read on the bridge's thread. A slot the game is writing at that moment may
show a size or time between two writes. `unavailable` until the game has opened its saves.

```
> {"id":50,"cmd":"saves"}
< {"id":50,"ok":true,"frame":900,"path":"/tmp/run/data/hookshot/save/5a3c9e1f0b7d24680000000000000001","account":true,
   "option":true,"slots":[{"slot":"03","mtime":1762900000,"newest":true,"files":[{"name":"progress.sav","size":120724,
   "mtime":1762900000},{"name":"caption.sav","size":42239,"mtime":1762900000}],"caption":{"scene":"KokiriForest",
   "map":"KokiriForest/KokiriForest_Child","thumbnail":{"format":"jpeg","offset":1396,"size":40843}}}]}
```

Slots come newest first; `newest` marks the one the title's Continue loads. Times are Unix seconds. `caption` is there
only when the slot's `caption.sav` has the layout seen in this game's saves (the file select's preview): its scene and
map names, and where its picture (a 543x306 JPEG) is in the file. Those offsets were checked on Kokiri Forest saves
only.

The client:

- `slots` prints one line a slot (last written, sizes, scene; `*` the newest). `slots --thumbs DIR` writes each slot's
  picture as `DIR/slot_NN.jpg`, read from the save folder on this PC.
- `launch --save FOLDER [--slot NN]` copies a save folder (`account.sav`, `option.sav`, `slot_NN/`) into the run's
  data folder before the game starts: `--data`, or `data/` in the run folder. With `--slot` (`03` or `slot_03`) only
  that slot is copied, so Continue loads it. The source is only read. A destination that already holds saves is
  refused. Linux only (on Windows the game keeps its saves in its own folder).

## Guest memory

Off unless `HOS_CONTROL_MEM` is set (apart from `HOS_CONTROL`):

| `HOS_CONTROL_MEM` | commands | aliases |
|---|---|---|
| unset / empty | none (`mem_*` reply `forbidden` with how to turn it on) | |
| `ro` | `mem_read`, `mem_read_many` | `r`, `read` |
| `rw` | the reads, and `mem_write` | `readwrite` |

Case does not matter. Any other value logs a line once and leaves memory off (it never falls back to a more permissive
level).

**This is a debugging tool, not a safe sandbox.**

- `ro` exposes all of the game's memory to a client. That includes values the runtime keeps for itself, not only the
  game's own.
- `rw` lets a client change that memory. A write can corrupt or crash the game and the runtime, not only move the game
  along, and a corrupted state can be saved to disk. **Use a throwaway save.**
- The socket is already this user's only (0600, `SO_PEERCRED`), so a client of the bridge can reach no memory that any
  other process of the same user could not already reach. The opt-in is about what the bridge itself will do, not about
  who can connect.

Addresses are guest addresses (the image is at `0x80000000`, the heap at `0x1000000000`). An address or range outside
the memory the runtime has mapped — the image, the system region in use, the heap, or an address region — is an error
(`out_of_range`), never a fault. Each request reads or writes at most 1 MB.

Reads and writes are done on the game thread, at a controller read, so a read is one frame's memory and a write lands
between frames. `mem_read_many` reads all its ranges in that one pass, so they are consistent with each other. Every
write is logged in `hookshot_log.txt` (`control: wrote N bytes to guest 0x...`).

A write to memory the GPU reads goes through the same soft write-watch the game's own CPU writes use (the page is marked
and synced into the renderer's VRAM mirror on the next frame's memory sync), so it reaches the GPU like any other CPU
write; this has not been checked against the GPU's output in a scene.

**mem_read**: `addr` and `size`: whole numbers, or strings of decimal digits or `0x` and hex digits (a string for an
address past 2^53, which a JSON number cannot hold exactly; no sign, no octal). `format` is `hex` (default) or
`base64`.

```
> {"id":30,"cmd":"mem_read","addr":2147483648,"size":8}
< {"id":30,"ok":true,"frame":5000,"format":"hex","addr":2147483648,"size":8,"data":"030000141c300000"}
```

**mem_read_many**: `ranges`, a list of `{addr, size}`, read in one pass. The reply has a `ranges` array, each with its
`addr`, `size` and `data`.

**mem_write** (`rw` only): `addr`, and the bytes as `hex` or `base64`.

```
> {"id":31,"cmd":"mem_write","addr":4300000000,"hex":"2a000000"}
< {"id":31,"ok":true,"frame":5001,"wrote":4,"addr":4300000000}
```

TODO: named game values (scene and room id, Link's position, health, rupees, ...) through `mem get NAME` and a
`symbols` command: planned, once addresses are located and verified for this build. `symbols` replies `unavailable` for
now. Slot details from caption.sav or progress.sav beyond the scene and the picture (the player's name, hearts, play
time): planned.

## The client

```
export HOS_CONTROL=$(python -I scripts/hookshot_ctl.py launch ~/Games/Hookshot --data /tmp/oot-data --headless)
# (a first start prepares the shaders before the first frame: the game reads no controller until then)
python -I scripts/hookshot_ctl.py wait frame --frame 1 --timeout 900
# (the title and the file select need A: press it until the save is loaded)
until [ "$(python -I scripts/hookshot_ctl.py --json status | jq -r .state)" = play ]; do
    python -I scripts/hookshot_ctl.py press A --frames 8; python -I scripts/hookshot_ctl.py wait present --frames 60
done
python -I scripts/hookshot_ctl.py status
python -I scripts/hookshot_ctl.py stick L 0 1 --frames 90 --exclusive
python -I scripts/hookshot_ctl.py press A --frames 8
python -I scripts/hookshot_ctl.py sequence '0:A/8,60:A/8'
python -I scripts/hookshot_ctl.py sequence '0:LUP/2000,2:A' --hos       # HOS_INPUT_LIVE's line, as it is
python -I scripts/hookshot_ctl.py shot out.png
python -I scripts/hookshot_ctl.py option get
python -I scripts/hookshot_ctl.py option set text_speed 3
python -I scripts/hookshot_ctl.py teleport                              # the destinations
python -I scripts/hookshot_ctl.py teleport 1 --arrive 30                # sent (or refused, and why), then arrived
python -I scripts/hookshot_ctl.py age adult --arrive 30                 # the Debug tab's Switch age, then reloaded
python -I scripts/hookshot_ctl.py release                               # this client's inputs and abandoned ones (--all)
python -I scripts/hookshot_ctl.py watch --events present,state --log "hid:"
python -I scripts/hookshot_ctl.py raw '{"cmd": "wait", "event": "frame", "frame": 9000}'
python -I scripts/hookshot_ctl.py mem read 0x80000000 8               # needs HOS_CONTROL_MEM=ro or rw
python -I scripts/hookshot_ctl.py mem read 0x1000000000 16 --format u32
python -I scripts/hookshot_ctl.py mem write 0x1000000000 2a000000      # needs HOS_CONTROL_MEM=rw
python -I scripts/hookshot_ctl.py quit
```

Each command prints plain lines; `--json` prints the replies. `--sock` overrides `$HOS_CONTROL`. The exit status is 0
when done, 1 for an error reply, 2 when there is no game or the usage is bad, and 3 for a timeout. `launch --data`
sets `XDG_DATA_HOME` for the run, so it plays from saves of its own. `--env NAME=VALUE` adds switches.

### A benchmark route

`scripts/hookshot_bench.py` drives a repeatable route through the client: the same places, the same settling time and
the same measurement window every run, so two builds, two switches or two PCs compare by numbers.

```
python -I scripts/hookshot_bench.py ~/Games/Hookshot --name main --save ~/bench-save
python -I scripts/hookshot_bench.py ~/Games/Hookshot --name parallel3 --save ~/bench-save --env RX_VK_PARALLEL=3
python -I scripts/hookshot_bench.py ~/Games/Hookshot --name field --places HyruleField_Child --secs 120 --grep "VRAM:"
```

One run:
1. Starts the game folder (`launch`) and waits for its first frame. A first start's preparation can take hours on a slow
   driver, so it waits in 50-minute steps for up to 8 hours.
2. Presses A until the newest save is loaded.
3. For each place (default Kokiri Forest, then Hyrule Field, as a child), teleports there and waits until no event or
   load is playing. It then lets the scene settle (`--settle`, 15 s) and measures for `--secs` (60) without touching
   anything.
4. Takes a screenshot of each place and quits.

**Start every run from the same save (`--save`).** The game saves as it plays, a slot rewritten in each run, and Continue
loads the newest, so without it the second run starts from wherever the first left off. `--save` takes a save folder
(`<user id>` with `account.sav`, `option.sav` and `slot_NN/`; copy one aside once) and `--slot` one slot of it.
- **Windows:** the game folder's own `user/save` is set aside as `user/save.hookshot_bench` while the run lasts, and put
  back after the game has closed.
- **Linux:** the save is copied into the run folder's own data folder (`launch --save`).

A message that waits for a button (an event that lasts 8 s, such as the wake-up cucco a time of day hatches) is closed
with A, before the window and in it. `summary.json` counts those presses.

The numbers come from the game's own log lines over the window: the medians of `N presents, F fps; a frame: D draws`,
`GPU time`, every part of `a frame's replay time (ms)`, and the highest `VRAM: U MB used of A MB allowed`. The bridge's fps
and the process's memory at the end of the window are added, with the share of the window an event was playing in (the
game's clock goes on: a dialogue that a time of day brings can land in it). `summary.json` in the run folder
(`bench-runs/<time>-<name>`, which git ignores) holds them per place, with the build's version and the lines `--grep` asked for. The log,
the screenshots and the summary stay in that folder. Compare runs made with the same save, settings and cache state:
the first visit to a place after a cache change compiles pipelines, so run a route once first, or measure twice.

## Known limits

- **Windows**: the pipe is tested in CI (the windows-2022 runner). The bridge inside a Windows game build was driven by
  hand and by `hookshot_bench.py` (`launch`, `status`, `wait`, `press`, `teleport`, `shot`, `quit`; Windows 11, main
  `de866017`). Refusing remote pipe clients (`PIPE_REJECT_REMOTE_CLIENTS`) needs a second machine and is not tested.
- **The state is a guess** from the draws a frame (above). A scene with unusual counts can be misread; for OoT, `game`'s
  `event` and `loading` and `map` are the game's own.
- **A teleport the game sent but that did not move Link**: `accepted` (the event manager's answer) is reported, not turned into an error: `false` was seen once, when the map did not change, but that it always means no move is not established. Otherwise `wait map` times out.
- **The first `option_get` or `option_set`** after start can wait, on the bridge's thread, for the start-up check of
  the upscalers (NVIDIA's NGX start: about 3 s). The game does not wait for it; other replies to every client wait
  that long.
- **Named game values** (`mem get NAME`, `symbols`) are not served yet (see Guest memory).
- `HOS_CONTROL_MEM=rw` writes were checked against the game's memory (written, read back, put back), not against the
  picture the GPU draws from a written value.

## Tests

`tests/test_control.py` builds the bridge with a fake game and drives it over its real transport: the Unix socket on
Linux, the named pipe on Windows (the CI's windows-2022 runner). On Windows it also checks the pipe's access control:
another first instance of the name and a second bridge on it are refused, and the DACL, read back through a client's
handle, is protected with a single ACE for the current user. `tests/test_hookshot_ctl.py` drives the client against a
fake bridge on the same transports. `tests/test_hookshot_bench.py` checks the benchmark route's steps against a stand-in
bridge on a fake clock, and its sums against log lines in the game's own format.

What is not tested is under Known limits.
