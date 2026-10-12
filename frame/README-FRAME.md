# Steam Frame: frame-xrprobe and Outbreak VR (M0 and M1)

This folder holds two things for the Steam Frame:

- **`frame-xrprobe`**, a small test program. One run, about two minutes with the headset on, answers the
  questions the Outbreak VR port depends on: which OpenXR extensions SteamVR offers on the Frame, which GPU
  driver and queues it hands out, whether both eyes get their own picture, and whether a flat or curved screen
  layer shows up. It needs no BIOS, disc or save.
- **Outbreak VR**: ARMSX2 with the OpenXR VR layer, played with the Frame controllers as the pad.
  - **M1 (this build, the default):** first person. In a room, the game camera is the character's eyes and turns
    with your head, and both eyes get their own picture, so the room has depth. Menus, doors and cutscenes go
    back to a flat screen.
  - **M0** (`FRAME_VIEW=flat`): the game on a fixed flat screen about 2 m in front of you.

  Run the probe first: if the probe fails, the game will too, and the probe log says why.

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

## 2. The game

Put the disc image's full path on the first line of `~/armsx2-vr/game.txt` (once):

    mkdir -p ~/armsx2-vr && echo "$HOME/armsx2-vr/games/<your disc image>" > ~/armsx2-vr/game.txt

Then over SSH:

    cd ~/outbreak-vr/outbreak-vr-frame-<sha>
    sleep 30 && ./launch-outbreak-vr.sh

(or `./launch-outbreak-vr.sh /path/to/the/disc/image` to name the game directly). As a devkit title: Name
`outbreak_vr`, Local Folder = the extracted folder, Start Command `launch-outbreak-vr.sh`, then "Devkit Game:
outbreak_vr". The same launch options work (`FRAME_NO_FDM=1`, `FRAME_NO_GSWSI=1`).

The first run writes `~/.config/ARMSX2/inis/PCSX2.ini` for the headset: no setup wizard, Vulkan at 1x
internal resolution, MTVU on, `[VR] Enable = true` with `HeadCamera = true` and `StereoMode = true` (first person,
M1). Later runs leave it alone; delete it to get the first-run one back. The script stops with a message in the log
if the game or the BIOS is missing.

**If you ran M0 before:** its ini has `HeadCamera = false` and `StereoMode = false`, and the launch log says so.
Run once with `FRAME_VIEW=first-person` in front of the command (or as a launch option). The launcher sets those
two keys to `true` and keeps everything else. `FRAME_VIEW=flat` sets them back to M0's flat screen.

A quick check without the headset on (SteamVR running or not): `QT_QPA_PLATFORM=offscreen ./AppRun -vr-info`
prints the OpenXR runtime, the headset and the extensions, or "No OpenXR runtime is available".

What to check for M0, the flat screen (`FRAME_VIEW=flat`; headset on, charger connected):

1. "Devkit Game: outbreak_vr" (or the SSH command) starts.
2. After a short flash of SteamVR's flat Theater view, the game appears on a flat screen about 2 m ahead and
   stays put when you turn your head.
3. The title screen comes up. The Frame controllers work the menus and move the character.
4. The on-screen speed stays at 95% or more for 5 minutes in a room (1x resolution).
5. Quitting returns to SteamVR without a crash.

It passes if 1-5 hold and the launch log has no "(VR) ... abandon" line and no "queue lock held over 500 ms".

## 3. First person (M1)

Play offline: Single Play, network adapter off. Turn both Frame controllers on before launching. Over SSH:

    cd ~/outbreak-vr/outbreak-vr-frame-<sha>
    sleep 30 && ./launch-outbreak-vr.sh

(with `FRAME_VIEW=first-person` in front once, if the ini came from M0). Load a save in File #1 or File #2 and walk
into a room. What to check, and what to write down:

1. **First person arms.** In a room, the view switches from the game's fixed camera to the character's eyes, and the
   body is hidden. A door, a cutscene or the item screen goes back to a flat screen; first person comes back after it.
   The log has a `(VR) First-person stereo:` line when it arms.
2. **The world holds still when you turn your head.** Turn slowly, then fast, left and right, up and down. The room
   should stay put, like a real room. Note any swim (the room drifts and catches up), judder or a lag you can feel.
3. **Stereo depth looks right.** Walls look far away, the furniture next to you looks near, and nothing looks doubled.
   Tilt your head to one side: the depth should stay comfortable. Rate depth and comfort from 1 to 5. Is the HUD
   doubled?
4. **The game keeps full speed.** Stand in a room and walk around for 10 minutes. The on-screen speed should stay at
   95% or more.
5. **A savestate keeps the camera.** Save a state, load it: first person must come back.
6. Quit: back to SteamVR without a crash.

It passes if 1-6 hold and the launch log has no "(VR) ... abandon" line and no "queue lock held over 500 ms".

The log lines that tell us what happened (all in `~/armsx2-vr/logs/launch-*.log`):

- `(VR) First-person stereo:` and `(VR) First first-person draw:` the separation, convergence and near floor in use.
- `(VR) CameraDriver: first-person frames placed at their matched render pose`: the head pose and the game frame are
  in step.
- `(VR) Pose-to-photon:` every 5 s in first person: how far behind its head pose each first-person image reaches the
  display, how many display frames show each game frame, and how many frames were placed by their own render pose.
- `(VR) Frames:` every 5 s: frames submitted per second, new game frames per second, and the frame call times.
- `(VR) StereoSettings: the OpenXR VR layer owns the stereo state`: the stereo settings came from `[VR] StereoMode`
  and the game profile.

## Logs to send back

- Probe: `~/frame-probe/probe-*.log` (the answers) and `~/frame-probe/launch-*.log` (everything printed).
- Game: `~/armsx2-vr/logs/launch-*.log` (everything printed, including the "(VR)" lines) and
  `~/.config/ARMSX2/logs/emulog.txt`. For M1, also your notes on items 1-6 above.
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
