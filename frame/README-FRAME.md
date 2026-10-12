# Steam Frame: frame-xrprobe and Outbreak VR (M0 to M3)

This folder holds two things for the Steam Frame:

- **`frame-xrprobe`**, a small test program. One run, about two minutes with the headset on, answers the
  questions the Outbreak VR port depends on: which OpenXR extensions SteamVR offers on the Frame, which GPU
  driver and queues it hands out, whether both eyes get their own picture, and whether a flat or curved screen
  layer shows up. It needs no BIOS, disc or save.
- **Outbreak VR**: ARMSX2 with the OpenXR VR layer, played with the Frame controllers as the pad.
  - **M1 (the default):** first person. In a room, the game camera is the character's eyes and turns
    with your head, and both eyes get their own picture, so the room has depth. Menus, doors and cutscenes go
    back to a flat screen.
  - **M2:** the PC build's controls and safety rules on top of M1: smooth turn, recenter, the grab breakout, haptics,
    and the online locks (no pause, speed change or savestate while the network adapter is on).
  - **M3 (this build):** the PC build's first-person presentation on top of M2: hands that hold the equipped weapon,
    the laser sight, the wrist card, menus you point at, belt pouches, the comfort blink and vignette, and the headset
    brightness.
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
M1) and `SnapTurn = false` (smooth turn, M2). Later runs leave it alone; delete it to get the first-run one back. The
script stops with a message in the log if the game or the BIOS is missing.

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

## 4. Controls (M2)

Play offline first (Single Play, network adapter off), seated, with both Frame controllers on before launching.
The same launch as section 3. Load a save in File #1 or File #2, walk into a room in first person, and check:

1. **Walking.** The left stick walks where you look (stick up = forward). Click the left stick while walking to run.
2. **Aiming and shooting.** Squeeze the right grip at your right hip (the holster) and keep squeezing to raise the
   weapon. The character aims where the right controller points, up and down too, and the right trigger fires.
   A thin red laser shows the aim. The left stick still walks while you aim.
3. **Smooth turn.** The right stick turns the view smoothly (left and right only; a mostly vertical push does not
   turn). It should feel even, with no judder. (`[VR] SnapTurn = true` in the ini gives 30 degree steps instead.)
4. **Recenter.** Face to one side and hold both thumbsticks down for half a second: the view and the screens face
   you again. The Frame's own recenter (from the system menu) does the same. The log has
   `(VR) VRInputSource: reset chord` or `(VR) Reference space recentered by the runtime`.
5. **Grab breakout.** Let a zombie grab you and shake either controller hard: the character breaks free about as
   fast as mashing buttons would. "Shake to break free!" shows on the first grab.
6. **Haptics.** The controllers buzz when you shoot, when you are hit, and with the game's own rumble. Note anything
   too weak, too strong or missing.
7. **A controller sleeping and waking leaves nothing stuck.** Put one controller down until it sleeps (or turn it
   off), then wake it up. Nothing should keep walking, turning, aiming or firing by itself. If something is stuck, hold
   both thumbsticks for half a second (the reset chord sets every control back to idle), and write down what was stuck.
8. **Headset brightness.** If the picture is too dark, the VR brightness hotkeys (when bound) change it in steps of
   0.1, and the headset shows "Brightness 1.10" and so on. The value is saved.

It passes if 1-7 hold and the log has no "(VR) ... abandon" line.

**Online (only when you play online):** with the network adapter on, the game must never pause, change speed or load
or save a state. Pausing, turbo or a savestate is refused with a message, and the log has a
`(VMManager) ... refused: the network adapter is on` line. Before going online, you can try this offline: launch with
`PCSX2_TEST_ONLINE_LOCKS=1` in front of the command.

## 5. Hands, HUD, menus and pouches (M3)

Play offline (Single Play, network adapter off), seated, with both Frame controllers on before launching. The same launch
as section 3.

**First, once: the self-tests** (a few minutes). Over SSH:

    cd ~/outbreak-vr/outbreak-vr-frame-<sha>
    PCSX2_VR_SELFTEST=1 PCSX2_VR_HUD_SELFTEST=1 ./launch-outbreak-vr.sh

They run once a game with its VR profile is loaded (load a save and walk into a room), then quit. The launch log must
have `(VR) MenuPointer self-test: passed`, `(VR) BeltPouches self-test: passed`, `(VR) Item names self-test: passed`
and `(VR) HUD self-test: 0 failure(s)`. Any `FAIL` line: send the log and stop here. (The haptics self-test,
`PCSX2_VR_FEEDBACK_SELFTEST`, runs only without a headset session, so it skips itself here.)

Then launch normally, load a save in File #1 or File #2, walk into a room in first person, and check:

1. **The hands show the equipped weapon.** Squeeze the right grip at your right hip to raise the weapon: the right hand
   holds what the character has equipped (a pistol, a long gun, a melee weapon, a thrown item or a spray). Equip
   something else on the item screen and raise it again: the hand changes with it. With the weapon down, the hands are
   empty. Does the weapon sit right in the hand? The log has `(VR) Hands: the right hand shows a ...`.
2. **The laser is for guns only.** With a gun raised, a thin red laser runs from the muzzle along the aim and fades out
   after a few metres. A melee weapon, a thrown item or a spray has none, and neither does a lowered weapon. Fire: does
   the shot land where the laser points? (`[VR] LaserSight = false` turns it off.)
3. **The wrist card names the weapon.** Turn the back of your left wrist toward your eyes, as if checking a watch: a
   small card fades in with the character's condition, an HP bar, the virus gauge, and the equipped weapon's name and
   rounds. It fades out when you turn the wrist away. Does the name match the item screen? Is the text sharp enough to
   read? The log has `(VR) HUD: wrist card shown` and `(VR) HUD: wrist card live: HP ...`.
4. **Menus: point at them, and only the trigger selects.** Open the item screen. A beam and a dot come from the right
   hand. Point at one of the four item cells or the equipped box: the game's cursor moves there by itself, with a light
   tick when it lands. Then check:
   - pointing alone never selects anything, however long you rest on a cell;
   - the trigger (or A) selects the cell the dot rests on, and a quick point-and-pull selects the cell you rested on,
     not one the dot only crossed;
   - push the D-pad yourself while pointing: the pointer does not undo your move;
   - open an item's submenu: the pointer stops, and the trigger and D-pad work as usual in it.
   The log has `(VR) MenuPointer: item screen open`, `(VR) MenuPointer: cell <a> -> <b> in <n> press(es)` and, only
   after your own pull, `(VR) MenuPointer: confirm passed on cell <n>`.
5. **The pouches name the item and open the item screen at that cell.** With no menu up, reach an open hand to your
   chest: four pouches there mirror the item grid, and the belt buckle is the equipped box. Reaching into a pouch ticks
   (twice, and firmer, when its cell holds an item), and a short message names the item. Squeeze the grip in that pouch:
   the item screen opens with the cursor on that cell, and nothing is selected until you pull the trigger. Does the name
   match what is in that cell? The log has `(VR) Pouches: pouch <n> (cell <n>, <item>) squeezed with the <hand> hand`.
6. **The comfort cards.** The short messages ("Recentered", "Brightness 1.10") show ahead of you and fade. A snap turn
   (`[VR] SnapTurn = true`) and each switch between first person and the flat screen (a door, the item screen) hide
   behind a short fade to black (`[VR] ComfortBlink`). With `[VR] ComfortVignette = true`, the edges of the view darken
   while you turn or walk with the stick, and clear when you stop. Is any of it distracting or too slow?
7. **Brightness is comfortable in dark rooms.** Stand in a dark room. Can you see the walls and the floor without the
   blacks turning grey? The Outbreak profile already lifts dark tones in first person (gamma 1.2); `[VR] Brightness`
   (0.8 to 1.8, default 1.00) goes on top of it. To change it, quit, set it in the ini over SSH (section 6), and start
   again; or bind the VR brightness hotkeys, which step it by 0.1 and save it. Write down the value that is comfortable
   in a dark room and still fine in a bright one. (Shade Boost in the graphics settings must stay off: the lift is not
   applied with it on.)

It passes if 1-7 hold and the launch log has no "(VR) ... abandon" line and no `(VR) Layer budget:` warning. The
`(VR) Runtime limits:` line should say `maxLayerCount 16` or more; with fewer, the compositor drops the least important
layers first (the toast, the wrist card, the lever cards, the hands, the laser, the menu dot, the vignette, the blink,
then the background) and says so in a `(VR) Layer budget:` line.

The log lines that tell us what happened (all in `~/armsx2-vr/logs/launch-*.log`):

- `(VR) Runtime limits:`, `(VR) Layer budget:` and `(VR) Screens:` at the start of each session: how many layers SteamVR
  takes a frame, and whether it has curved (cylinder) layers. Without them every screen is a flat quad, which the
  Outbreak profiles use anyway.
- `(VR) Hands:` and `(VR) HUD:` (laser sight, wrist card, menu pointer, vignette shown), `(VR) HUD toast:` and
  `(VR) HUD blink`.
- `(VR) MenuPointer:` and `(VR) Pouches:`.
- The self-test lines from the first run.

## 6. Settings

The Frame build reads its VR settings from section `[VR]` of `~/.config/ARMSX2/inis/PCSX2.ini`. Edit it over SSH while
ARMSX2 is not running, so your edit and ARMSX2's own saves (the brightness hotkeys save `Brightness`, for example) do
not cross. The same settings are on the "VR" page of ARMSX2's
Settings window, but that window is not reachable from inside the headset. A key that is not in the ini has its
default. The first-run ini writes only `Enable`, `HeadCamera`, `StereoMode`, `SnapTurn` and `HeadsetRateSubmit`.

| Key | Default | First-run ini | What it does |
|---|---|---|---|
| `Enable` | false | true | VR on (the launcher's `--vr` also turns it on for that run) |
| `HeadCamera` | true | true | the head turns the game camera (first person) |
| `StereoMode` | true | true | both eyes get their own picture |
| `StereoUseProfile` | true | | the game profile's stereo numbers instead of `StereoSeparation` and `StereoConvergence` |
| `StereoSeparation` | 0.02 | | eye separation without a profile (0 to 0.1) |
| `StereoConvergence` | 20.0 | | the depth that sits at the screen, without a profile (0 to 200) |
| `FirstPersonAutoStereo` | true | | first-person stereo from the headset's IPD; false: the profile's fixed numbers |
| `FirstPersonStereoStrength` | 1.0 | | how deep first person looks (0.5 to 1.5; 1.0 is life-size) |
| `NearDepthMetres` | 0 | | how near true depth reaches: 0 the profile's (1.0 m), below 0 off, 0.45 to 1.5 that distance |
| `RollParallax` | true | | parallax turns with head roll |
| `FirstPersonFov` | Profile | | the game camera's field of view: `Profile`, or `Auto` (fitted to the headset; softer centre) |
| `FirstPersonFovMarginDeg` | 6.0 | | what `Auto` adds to the headset's field of view (0 to 20) |
| `EdgeFill` | false | | a dim blur of the picture's edges around the first-person screen instead of black |
| `SnapTurn` | false | false | the right stick turns in 30 degree steps instead of smoothly |
| `ComfortBlink` | true | | a short fade to black on snap turns and view switches |
| `ComfortVignette` | false | | the view's edges darken while you turn or walk with the stick |
| `VignetteStrength` | 0.6 | | how dark the vignette gets (0.2 to 1.0) |
| `LaserSight` | true | | the red laser on a raised gun |
| `MenuPointer` | true | | the menu beam, the dot and point-to-select (never selects by itself) |
| `MenuTablet` | false | | menus on a small tilted screen in front of your chest instead of the big screen |
| `Brightness` | 1.00 | | headset brightness on top of the profile's lift (0.8 to 1.8), in the headset only |
| `GameRumble` | true | | the game's own rumble goes to the controllers |
| `HapticStrength` | 1.0 | | scales the game's haptics (0 to 1; 0 is off) |
| `OnlineSafe` | false | | turns off the first-person extras that change gameplay (walking while aiming, aim pitch); always on online except in private co-op |
| `PrivateCoopFirstPerson` | false | | first person online, in private co-op only |
| `ScreenDistance` | 2.0 | | the flat screen's distance in metres (0.5 to 10) |
| `ScreenHeight` | 1.4 | | the flat screen's height in metres (0.3 to 6) |
| `ScreenVerticalOffset` | 0.0 | | the flat screen's height offset in metres (-2 to 2) |
| `ScreenArcDeg` | 100 | | the flat screen's curve in degrees, where the runtime has cylinder layers and the profile does not say `arc: 0` (the Outbreak profiles do) |
| `HeadsetRateSubmit` | true | true | frames go to the headset at its own rate (the VR pacer); false: one per game frame |
| `OpaqueBackground` | true | | a black layer behind everything, so SteamVR's room never shows |
| `HandsDisplayRate` | true | | hands, laser and wrist card follow the controllers every display frame |
| `DebugView` | Off | | debug only: how the desktop window shows the two eyes (`SBS`, `CrossEye`, `Left` or `Right`) |
| `InterleaveEyes` | false | | debug only: both eyes in one target |
| `XrSeatRuntimeDirs`, `XrSeatRuntimeJsons` | empty | | co-op seats (not used: `--vr-seat` is not in this build) |

`[VR] StereoEnable`, `Separation`, `Convergence` and the other keys of ARMSX2's own stereo renderer are ignored in this
build (the log says so). Two more switches outside `[VR]`: `[EmuCore] EnablePINE` (section 7) and `[Folders] VRProfiles`
(default `vrprofiles`: a profile there replaces the bundled one for its game).

## 7. Research and test tools over SSH (PINE)

The research and test tools talk to ARMSX2 through PINE, which is off by default. Turn it on once (it stays on):

    FRAME_PINE=1 ./launch-outbreak-vr.sh

While a game runs, PINE listens on a Unix socket, `$XDG_RUNTIME_DIR/pcsx2.sock` (usually `/run/user/1000/pcsx2.sock`;
the emulog's `PINE: Listening on ...` line names it). `FRAME_PINE=0` turns PINE off again. Two ways to use it:

- On the Frame, over SSH: `python3 tools/pine.py` in the extracted folder prints the game's status and id and reads the
  EE RAM, as a check. Other tools from `tools/outbreak-vr` import `pine.py` and find the socket the same way.
- From the PC: forward a local port to the Frame's socket, then run the tools on the PC as with the PC build (TCP
  127.0.0.1:28011 is the default on Windows):

      ssh -N -L 28011:/run/user/1000/pcsx2.sock steamos@frame

  The tools' offline check reads the PC's ini then, not the Frame's: set `PCSX2_INI` to a copy of the Frame's ini, or
  only the game's own network flag is checked.

Never run the tools with the network adapter on: they pause, load states and send test input, and the online locks
refuse most of it anyway. `PINE_SOCKET=<path>`, `PINE_TCP=1` and `PINE_HOST=<host>` override where `pine.py` connects.

## Logs to send back

- Probe: `~/frame-probe/probe-*.log` (the answers) and `~/frame-probe/launch-*.log` (everything printed).
- Game: `~/armsx2-vr/logs/launch-*.log` (everything printed, including the "(VR)" lines) and
  `~/.config/ARMSX2/logs/emulog.txt`. For M1, M2 and M3, also your notes on the items in sections 3, 4 and 5 (for M3:
  the self-test run's launch log too, and the `[VR] Brightness` value you settled on).
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
