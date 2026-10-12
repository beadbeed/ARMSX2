# Outbreak VR branch

This branch is part of **Outbreak VR**: Resident Evil Outbreak (PS2, Japanese File #1 / #2
with the English patch) as a true-3D VR game for the Valve Steam Frame.

It is one of two builds that share the same VR ideas:

- **ARMSX2, branch `outbreak-vr`** (this repo): Steam Frame standalone (Linux arm64 AppImage built by GitHub Actions); per-eye stereo is done, the headset session is not
- **PenguinScreen2, branch `windows-steamvr`** (https://github.com/beadbeed/PenguinScreen2/tree/windows-steamvr): Windows PC build streamed to the Frame through SteamVR; stereo, controllers and gesture zones

Outbreak-specific VR settings are in `the `[VR]` ini section (pcsx2/VR/StereoSettings.cpp)`.
This repo holds code only. No game files, BIOS or saves are ever committed here.

## Steam Frame: probe (M0, first slice)

Branch `outbreak-vr-frame` adds `frame-xrprobe` (`tools/frame-xrprobe`), a small test program that runs on
the Frame by itself. It needs no BIOS, disc or save. With the headset on for about two minutes, it answers
the SteamVR questions the rest of M0 depends on. The VR layer came in the second slice (below).

**Build.** The "Outbreak VR (Steam Frame build)" workflow builds with `-DENABLE_OPENXR=ON`; every other build
leaves it off and is unchanged. Besides the AppImage, it uploads the artifact **`outbreak-vr-frame-<sha>`**.
That artifact holds `outbreak-vr-frame-<sha>.tar.zst`: the extracted AppDir, with `usr/bin/frame-xrprobe`,
and with `frame-probe.sh` and `README-FRAME.md` at the top. The `frame-smoke` job runs the probe on every
build without a headset or runtime and checks that nothing needs a newer glibc than the Frame's 2.39.

**Deploy and run** (step by step in `frame/README-FRAME.md`):

- SSH (recommended): copy the `.tar.zst` to the Frame, extract it there with `tar --zstd -xf` into
  `~/outbreak-vr`, then run `sleep 30 && ./frame-probe.sh` in the new folder and put the headset on.
- Devkit title: in the SteamOS Devkit Client, set Name to `frame_xrprobe` (a title id may not contain `-`),
  Local Folder to the extracted folder and Start Command to `frame-probe.sh`, then launch
  "Devkit Game: frame_xrprobe" on the Frame. If it does not start, use SSH, because a folder copied from
  Windows can lose its permissions.
- Launch options: `FRAME_NO_FDM=1` if the log stops at xrCreateVulkanDeviceKHR, and `--seconds-per-phase N`
  to show each colour longer.

**Logs.** `~/frame-probe/probe-<time>.log` holds the answers. `~/frame-probe/launch-<time>.log` holds
everything printed, including the OpenXR loader's own messages. Send both back, plus `xrclient_*.txt`,
`vrserver.txt` and `vrcompositor.txt` from `~/.local/share/Steam/logs`.

**What it answers:**

- Host: page size (whether the 4K-page build is right), glibc, CPU cores, whether the title runs inside a
  container, and whether `/opt/steamvr` is visible.
- OpenXR: where the active runtime manifest is, which API layers load (Valve's FDM injection layer), and
  whether SteamVR offers `XR_KHR_vulkan_enable2`, the cylinder layer, the Frame controller profile, refresh
  rate control, the depth layer and eye gaze. Also the runtime version, maxLayerCount, the recommended size
  per eye, and the blend modes.
- Vulkan, through the runtime as the VR layer does it: the Turnip version, the queue families, multiview,
  and whether the graphics family has a second queue (shared-queue lock or a dedicated XR queue).
- Session: the reference spaces, the swapchain formats (sRGB or not), the refresh rates, and which controller
  profile SteamVR picks. Valve documents `/interaction_profiles/valve/frame_controller_valve`, while
  PenguinScreen2's VRInput.cpp suggests `.../valve/frame_controller`. The probe suggests both and logs which
  one is accepted and used.
- Four colour phases the wearer reports. A: one swapchain with two array layers (left red, right blue, or
  one colour in both eyes?). B: one swapchain per eye (green, magenta). C: a yellow quad 2 m ahead. D: a cyan
  cylinder, if SteamVR offers it. Each phase logs its frame timing (display period, xrWaitFrame,
  vkQueueSubmit, xrEndFrame).
- It exits cleanly: one session per instance, torn down in the same order as PenguinScreen2
  (SteamVR-for-Linux #989).

## Steam Frame: the VR layer (M0, second slice)

Branch `outbreak-vr-frame` now also builds PenguinScreen2's OpenXR VR layer into ARMSX2 (`pcsx2/VR`, copied from
PenguinScreen2 c2c5ee2; `pcsx2/VR/VR-SOURCE.md` lists every copied file, every local edit and every hook site).
Only the Steam Frame build turns it on (`ENABLE_OPENXR`); every other ARMSX2 build is unchanged, and
`tools/outbreak-vr/hook-audit.sh` checks that every hook in a shared file is under `#ifdef ENABLE_OPENXR`.

**What it adds.** Launched with `--vr`, ARMSX2 opens an OpenXR session on the Frame's SteamVR, shows the game on
a fixed flat screen about 2 m ahead, and reads the Frame controllers as the pad through the game's VR profile
(`bin/resources/vr-profiles`, Outbreak File #1 and #2). No first-person camera and no stereo yet: that is M1.
`-vr-info` prints the OpenXR runtime report and exits. The bundle (`outbreak-vr-frame-<sha>.tar.zst`) now also
holds `launch-outbreak-vr.sh`, the game's devkit Start Command.

**How Alex launches it** (step by step in `frame/README-FRAME.md`): copy the BIOS to `~/.config/ARMSX2/bios` and
the disc image under `~` yourself (never through GitHub or a cloud service), put the image's path in
`~/armsx2-vr/game.txt`, then run `./launch-outbreak-vr.sh` in the extracted folder (or make it the devkit
title's Start Command, title name `outbreak_vr`). The first run writes a headset ini: no setup wizard, Vulkan at
1x, MTVU, VR on, head camera and stereo off. Logs go to `~/armsx2-vr/logs`.

**What to check:** the game appears on a flat screen that stays put when you turn your head, the title screen
comes up, the Frame controllers work the menus and move the character, the speed stays at 95% or more for
5 minutes in a room, and quitting returns to SteamVR without a crash. Run `frame-probe.sh` first.

**CI.** Besides the probe checks, the `frame-smoke` job now runs `AppRun -vr-info` with no display and no runtime
(the version line and "No OpenXR runtime is available"), checks the VR profiles are in the AppDir, runs the
launcher with no game (first-run ini written, exit 2), runs `AppRun -testconfig` on that ini (loaded without a
reset, BIOS folder at `~/.config/ARMSX2/bios`), and runs the hook audit.

## Steam Frame: first person (M1)

Branch `outbreak-vr-frame` now plays Outbreak in first person on the Frame. What M1 adds (every change to a shared
ARMSX2 file is under `#ifdef ENABLE_OPENXR`; `pcsx2/VR/VR-SOURCE.md` has the details and line numbers):

- **The head drives the game camera.** VMManager runs PenguinScreen2's per-vsync VR block: `CameraDriver::Apply`
  writes the profile's `camera.lookAt` from the head pose into game memory, then `GameFeedback::Poll`,
  `HudCards::Poll`, `ApplySceneStereo` and `SplitState::Apply`. The writes go through ARMSX2's normal memory path,
  and its arm64 recompiler protects code pages the same way as the x86 one, so a code hook (Outbreak uses none)
  would also be picked up.
- **One owner for the stereo state.** The VR layer (`VR::UpdateSettings`, `VR::ApplySceneStereo`) decides when stereo
  is on and with which numbers; ARMSX2's own `[VR] StereoEnable` keys are ignored in this build, and `DebugView` and
  `InterleaveEyes` stay as debug options. The texture cache gives the display chain a layer per eye whenever the VR
  layer turns stereo on.
- **First-person stereo from PenguinScreen2:** true depth in front of the screen plane down to the profile's nearest
  distance (the near floor), parallax that follows head roll, the HUD collimation and depth histogram hooks, and
  `PCSX2_VR_SNAPSHOT_LAYER=1` for right-eye dumps. The Vulkan shader's new terms apply only when first person sets
  them, so every other shader computes what it did before.
- **Compositor:** the two-layer target goes to the headset as two swapchains of one layer each (one per eye), which
  avoids the SteamVR-for-Linux array-layer bugs (#975, openvr#1822). A new `(VR) Pose-to-photon:` line every 5 s says
  how far behind its head pose each first-person image reaches the display.
- **Launcher:** the first-run ini now has `HeadCamera = true` and `StereoMode = true`. `FRAME_VIEW=flat` gives M0's
  flat screen, `FRAME_VIEW=first-person` turns an M0 ini into an M1 one.
- **Hook parity:** `tools/outbreak-vr/hook-parity.py` compares every PenguinScreen2 `ENABLE_VR` block with ARMSX2
  and fails unless each one is ported or in `hook-parity-allowlist.txt` with a reason. The frame-smoke job runs it
  against the PenguinScreen2 commit `pcsx2/VR` came from.

**What to check in the headset** (step by step in `frame/README-FRAME.md`, section 3): first person arms in a room;
the world holds still when you turn your head (no swim); stereo depth looks right and the HUD is not doubled; the game
keeps at least 95% speed for 10 minutes; loading a savestate keeps the camera. Send back `~/armsx2-vr/logs/launch-*.log`
(with the `(VR) Pose-to-photon:`, `(VR) Frames:` and `(VR) First-person stereo:` lines), `emulog.txt` and SteamVR's
logs.

## Steam Frame: the PC build's controls and safety rules (M2)

Branch `outbreak-vr-frame` now has the PC build's control scheme and its online rules. As before, every change to a
shared ARMSX2 file is under `#ifdef ENABLE_OPENXR` (details and line numbers in `pcsx2/VR/VR-SOURCE.md`):

- **Online locks.** With the network adapter on, the game never pauses, changes speed, or loads or saves a state:
  the server drops a client that goes quiet and bans speed changes, and a state load desyncs the room. VMManager
  refuses each one (pause, frame advance, turbo, slow motion, unlimited, target speed, every state load and save),
  the Qt pause buttons and the game-list view are refused too, and the hotkeys and PINE say why.
  `PCSX2_TEST_ONLINE_LOCKS=1` turns the locks on with the adapter off, for testing.
- **Controls.** The pad goes through PenguinScreen2's PadLook: the right stick turns the first-person view, and
  L1+R1+L3+R3 recenters. With the Frame controllers: walking on the left stick, aiming and shooting with the
  controller, smooth turn on the right stick (`[VR] SnapTurn = true` for 30 degree steps), the gesture zones, the grab
  breakout (shake a controller when a zombie grabs you), haptics from the game's rumble and cues, and the reset chord
  (both thumbsticks held half a second: every control back to idle, and a recenter). The system recenter of the Frame
  works too.
- **Hotkeys:** VR recenter, and headset brightness up and down.
- **PINE:** PenguinScreen2's test-kit commands 0xE0-0xE5 (scripted VR input only with `PCSX2_VR_TEST_INPUT` and never
  online, telemetry, menu pointer and belt pouch state), next to ARMSX2's own PINE commands. Memory watches arm but never
  fire on the Frame (its recompiler has no memory checks).
- **Launcher:** the first-run ini says `SnapTurn = false` (smooth turn). Seated play is the default.

**What to check in the headset** (step by step in `frame/README-FRAME.md`, section 4): walking, aiming and shooting,
smooth turn, recenter (chord and the system recenter), the grab breakout, haptics, and that a controller going to sleep
and waking up leaves nothing stuck.
