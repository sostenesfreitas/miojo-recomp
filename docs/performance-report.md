# Report slow performance

If the game runs slowly, stutters or hitches, a normal bug report often isn't enough for the team to find out why. This
page shows how to have the game measure itself while it is slow. You only add a few lines to a text file: you don't need
to build anything again or install any tools.

## 1. Turn the measurements on

In the game folder (the folder with `image.bin` in it: `Games\Hookshot` in your user folder on Windows, `~/Games/Hookshot`
on Linux, unless you chose another one), create a text file named **`hookshot_env.txt`** that contains these lines:

```
HOS_FPS_LOG=1
HOS_THREAD_TIMES=5
RX_VK_PASS_TIMING=1
RX_VK_SLOW_LOG=50
```

| Line | What it adds to the log |
|---|---|
| `HOS_FPS_LOG=1` | The frame rate cap, VRR and the game's speed |
| `HOS_THREAD_TIMES=5` | Every 5 seconds, which parts of the game keep the CPU busy |
| `RX_VK_PASS_TIMING=1` | How long the graphics card spends on each part of a frame |
| `RX_VK_SLOW_LOG=50` | Details for every frame that takes more than 50 ms: what each stutter was spent on |

Without any of these lines, the log already records the frame rate and the spread of frame times every 5 seconds.

On Windows, check that the file isn't really called `hookshot_env.txt.txt`: in File Explorer, turn on **View → Show →
File name extensions**.

## 2. Play where it's slow

1. Start the game. If it was already running, close it and start it again, because it only reads the file at startup.
2. Go to the place where it runs badly and stay there for **60 to 90 seconds**. Do the same thing the whole time, for
   example stand still, turn the camera or walk one route. Write down what you did.
3. While it's still slow, open the menu (<kbd>F1</kbd>), go to **Gameplay** and choose **Bug report**. This saves a
   folder in `bug-reports` in the game folder with the log, a screenshot and your PC's details, with your user name
   taken out.
4. Quit the game normally.

## 3. Send it

Send the team, on [Discord](https://discord.gg/rexed) or with a [report](https://git.notrexed.dev/Hookshot/Hookshot/issues/new):

- the **bug report folder** from step 3, and `hookshot_log.txt` from the game folder's `logs` folder;
- **where** you were in the game, and **what you did** there;
- your **settings**: the quality preset, render resolution, upscaler, frame generation and V-Sync (or a screenshot of
  the menu);
- your **graphics card and driver version**.

Send nothing else from the game folder: no game files, keys or saves.

## 4. Turn it off again

Delete `hookshot_env.txt`, or the lines you added to it. Measuring costs a little speed, so don't leave it on while you
play normally.

If you already had a `hookshot_env.txt` before step 1 (with a workaround someone gave you, for
example), add the lines to it rather than replacing it, and remove only those lines afterwards.
