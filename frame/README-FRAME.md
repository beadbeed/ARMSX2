# Steam Frame: frame-xrprobe and Outbreak VR (M0)

This folder holds two things for the Steam Frame:

- **`frame-xrprobe`**, a small test program. One run, about two minutes with the headset on, answers the
  questions the Outbreak VR port depends on: which OpenXR extensions SteamVR offers on the Frame, which GPU
  driver and queues it hands out, whether both eyes get their own picture, and whether a flat or curved screen
  layer shows up. It needs no BIOS, disc or save.
- **Outbreak VR, milestone M0**: ARMSX2 with the OpenXR VR layer. The game shows on a fixed flat screen about
  2 m in front of you, and the Frame controllers work as the pad. There is no first-person camera and no stereo
  yet (that is M1). Run the probe first: if the probe fails, the game will too, and the probe log says why.

## What you need

- Developer Mode on the Frame (Settings > System), with your developer password set.
- The Frame on its 45 W charger.
- This folder, extracted **on the Frame** (extracting on Windows can drop the file permissions).
- For the game only: your PS2 BIOS in `~/.config/ARMSX2/bios`, and your Outbreak disc image somewhere under `~`
  (for example `~/armsx2-vr/games/`). Copy them over the LAN, USB-C or microSD yourself. Never put them in this
  folder (a devkit upload replaces it) and never send them through GitHub or a cloud service.

## Get it onto the Frame (SSH)

1. Download the artifact `outbreak-vr-frame-<sha>` from the GitHub Actions run. It holds
   `outbreak-vr-frame-<sha>.tar.zst`.
2. Copy it over: `scp outbreak-vr-frame-<sha>.tar.zst steamos@frame:~/`
3. On the Frame (`ssh steamos@frame`):

       mkdir -p ~/outbreak-vr && cd ~/outbreak-vr
       tar --zstd -xf ~/outbreak-vr-frame-<sha>.tar.zst

   That makes `~/outbreak-vr/outbreak-vr-frame-<sha>/`, with `frame-probe.sh` and `launch-outbreak-vr.sh` at the
   top.

## 1. The probe

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

The colours start once the session is visible. Note what each eye sees:

| Phase | Shown | Tell us |
|---|---|---|
| A | left eye RED, right eye BLUE | red/blue, or the same colour in both eyes? |
| B | left eye GREEN, right eye MAGENTA | green/magenta? |
| C | black, with a YELLOW rectangle about 2 m ahead | is it there, and does it stay put when you turn your head? |
| D | black, with a CYAN curved screen (only if SteamVR offers curved layers) | is it there? |

Pull each trigger once during the phases; the log records it. The probe ends by itself.

## 2. The game (M0)

Put the disc image's full path on the first line of `~/armsx2-vr/game.txt` (once):

    mkdir -p ~/armsx2-vr && echo "$HOME/armsx2-vr/games/<your disc image>" > ~/armsx2-vr/game.txt

Then over SSH:

    cd ~/outbreak-vr/outbreak-vr-frame-<sha>
    sleep 30 && ./launch-outbreak-vr.sh

(or `./launch-outbreak-vr.sh /path/to/the/disc/image` to name the game directly). As a devkit title: Name
`outbreak_vr`, Local Folder = the extracted folder, Start Command `launch-outbreak-vr.sh`, then "Devkit Game:
outbreak_vr". The same launch options work (`FRAME_NO_FDM=1`, `FRAME_NO_GSWSI=1`).

The first run writes `~/.config/ARMSX2/inis/PCSX2.ini` for the headset: no setup wizard, Vulkan at 1x
internal resolution, MTVU on, `[VR] Enable = true` with `HeadCamera = false` and `StereoMode = false`. Later runs
leave it alone; delete it to get the first-run one back. The script stops with a message in the log if the game
or the BIOS is missing.

A quick check without the headset on (SteamVR running or not): `QT_QPA_PLATFORM=offscreen ./AppRun -vr-info`
prints the OpenXR runtime, the headset and the extensions, or "No OpenXR runtime is available".

What to check (headset on, charger connected):

1. "Devkit Game: outbreak_vr" (or the SSH command) starts.
2. After a short flash of SteamVR's flat Theater view, the game appears on a flat screen about 2 m ahead and
   stays put when you turn your head.
3. The title screen comes up. The Frame controllers work the menus and move the character.
4. The on-screen speed stays at 95% or more for 5 minutes in a room (1x resolution).
5. Quitting returns to SteamVR without a crash.

It passes if 1-5 hold and the launch log has no "(VR) ... abandon" line and no "queue lock held over 500 ms".

## Logs to send back

- Probe: `~/frame-probe/probe-*.log` (the answers) and `~/frame-probe/launch-*.log` (everything printed).
- Game: `~/armsx2-vr/logs/launch-*.log` (everything printed, including the "(VR)" lines) and
  `~/.config/ARMSX2/logs/emulog.txt`.
- From `~/.local/share/Steam/logs`: `xrclient_*.txt`, `vrserver.txt`, `vrcompositor.txt`.

From the PC:

    scp -r steamos@frame:frame-probe ./frame-probe-logs
    scp -r steamos@frame:armsx2-vr/logs ./armsx2-vr-logs

## Careful

- If the probe hangs, the log says "STILL WAITING" every 10 s. Stop it once (Ctrl+C, or stop the title) and give
  it up to 20 s to close the session; a second Ctrl+C kills it at once. A call that never returns ends the
  probe by itself after 2 minutes ("GIVING UP" in the log, exit code 3).
- Do not restart SteamVR over and over: three failed SteamVR runs in one boot trigger SteamOS's health
  check, which wipes `~/.local/share/Steam`. Reboot the Frame instead.
- The headset must be on your head (not in standby) for anything to appear.
- In the game, do not switch the renderer or restart the GS while VR runs: SteamVR for Linux aborts on a second
  session in one OpenXR instance (#989), so the VR layer tears everything down and starts again, with a flash of
  the Theater view.
