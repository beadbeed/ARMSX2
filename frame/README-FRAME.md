# Steam Frame: frame-xrprobe

`frame-xrprobe` is a small test program for the Steam Frame. One run, about two minutes with the headset
on, answers the questions the Outbreak VR port depends on: which OpenXR extensions SteamVR offers on the
Frame, which GPU driver and queues it hands out, whether both eyes get their own picture, and whether a
flat or curved screen layer shows up. It needs no BIOS, disc or save. It is not the game.

## What you need

- Developer Mode on the Frame (Settings > System), with your developer password set.
- The Frame on its 45 W charger.
- This folder, extracted **on the Frame** (extracting on Windows can drop the file permissions).

## Get it onto the Frame (SSH)

1. Download the artifact `outbreak-vr-frame-<sha>` from the GitHub Actions run. It holds
   `outbreak-vr-frame-<sha>.tar.zst`.
2. Copy it over: `scp outbreak-vr-frame-<sha>.tar.zst steamos@frame:~/`
3. On the Frame (`ssh steamos@frame`):

       mkdir -p ~/outbreak-vr && cd ~/outbreak-vr
       tar --zstd -xf ~/outbreak-vr-frame-<sha>.tar.zst

   That makes `~/outbreak-vr/outbreak-vr-frame-<sha>/`, with `frame-probe.sh` at the top.

## Run it

Over SSH, give yourself time to put the headset on:

    cd ~/outbreak-vr/outbreak-vr-frame-<sha>
    sleep 30 && ./frame-probe.sh

Or as a devkit title (SteamOS Devkit Client on the PC): Name `frame_xrprobe` (letters, digits, `_` and `.`
only: a title id may not contain `-`), Local Folder = the extracted folder, Start Command `frame-probe.sh`.
Launch "Devkit Game: frame_xrprobe" from the Frame's library. If the title does not start, use SSH instead
(a folder copied from Windows can lose its permissions). The launch log says whether the title ran inside
the Steam Runtime container or on SteamOS itself.

Launch options, if needed (Steam > the title > Properties, or before the command over SSH):

- `FRAME_NO_FDM=1` if the log stops at "Calling xrCreateVulkanDeviceKHR" (SteamVR-for-Linux #972).
- `FRAME_NO_GSWSI=1` turns off the gamescope WSI layer.
- `--seconds-per-phase 12` shows each colour longer (default 8 s).

## What you will see

The colours start once the session is visible. Note what each eye sees:

| Phase | Shown | Tell us |
|---|---|---|
| A | left eye RED, right eye BLUE | red/blue, or the same colour in both eyes? |
| B | left eye GREEN, right eye MAGENTA | green/magenta? |
| C | black, with a YELLOW rectangle about 2 m ahead | is it there, and does it stay put when you turn your head? |
| D | black, with a CYAN curved screen (only if SteamVR offers curved layers) | is it there? |

Pull each trigger once during the phases; the log records it. The probe ends by itself.

## Logs to send back

- `~/frame-probe/probe-*.log` (the answers) and `~/frame-probe/launch-*.log` (everything printed).
- From `~/.local/share/Steam/logs`: `xrclient_*.txt`, `vrserver.txt`, `vrcompositor.txt`.

From the PC: `scp -r steamos@frame:frame-probe ./frame-probe-logs`

## Careful

- If it hangs, the log says "STILL WAITING" every 10 s. Stop it once (Ctrl+C, or stop the title) and give
  it up to 20 s to close the session; a second Ctrl+C kills it at once. A call that never returns ends the
  probe by itself after 2 minutes ("GIVING UP" in the log, exit code 3).
- Do not restart SteamVR over and over: three failed SteamVR runs in one boot trigger SteamOS's health
  check, which wipes `~/.local/share/Steam`. Reboot the Frame instead.
- The headset must be on your head (not in standby) for the colours to appear.
