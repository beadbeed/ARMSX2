# Where pcsx2/VR comes from

The OpenXR VR layer in this folder is copied from **PenguinScreen2** (https://github.com/beadbeed/PenguinScreen2),
branch `pc-firstperson`, commit **c2c5ee249e66f2552894b20c465f2fa7ef9f41ef** (2026-10-12, "STATUS.md: one page for
where Outbreak VR stands ..."). It is built only when `ENABLE_OPENXR` is on, which only
`.github/workflows/outbreak-vr.yml` does (the Steam Frame build). Every other ARMSX2 build compiles none of it and
is unchanged.

The files were copied whole, not merged: PenguinScreen2 commit 990b456 stripped comments from 938 files, so a git
merge between the two trees is noise. To update, run `tools/outbreak-vr/sync-vr-layer.py` (see "Keeping the two copies
in step" below): it copies the files again from a newer PenguinScreen2 commit, puts the local edits below back on top
(they are kept as patch files in `tools/outbreak-vr/vr-local-edits/`), and runs the hook parity check on the hook sites
in the table at the end.

## Copied files

Unchanged copies of PenguinScreen2 c2c5ee2 (39,362 lines of C++ in total):

- `pcsx2/VR/`: BeltPouches, CameraDriver, ControlQuads, DepthHistogram, GameFeedback, GrabBreakout, HandModel,
  HeadPose, HudCards, ItemData, MemWatch, MenuPointer, PadLook, SeatCast, SeatSession, SpatialControls,
  SplitState, StereoAuto, StereoDump, VRInput, VRManager, VRProfileDB, VRVulkanBridge, XRCompositor, XRSession
  (each `.cpp` and `.h`), plus `VRInputState.h`, `VRInternal.h` and `LICENSE.note`.
- `pcsx2/Input/VRInputSource.cpp` and `.h`.
- `bin/resources/vr-profiles/`: `SLPM-65428.yaml`, `SLPM-65692.yaml` (Outbreak File #1 and #2), `SLUS-20090.yaml`,
  `SLUS-20851.yaml`, `SLUS-20919.yaml`, `README.md`, `LICENSE.md`.

Copied in M3, with the local edits listed below:

- `pcsx2-qt/Settings/VRSettingsWidget.cpp`, `.h` and `.ui` (c2c5ee2): the Qt VR settings page. Built only with
  `ENABLE_OPENXR` (`pcsx2-qt/CMakeLists.txt`), so hook-audit treats it as part of the VR layer.
- `tools/outbreak-vr/pine.py` (PenguinScreen2 2ecc16b, the same file as at c2c5ee2): the PINE client the research and
  test tools use. Only its transport is ARMSX2's.

Not copied, on purpose:

- `pcsx2/VR/StereoState.h` and `.cpp`: ARMSX2 already had these (its stereo renderer uses them on every platform).
  PenguinScreen2's version is ARMSX2's plus a few fields and functions, which were added to ARMSX2's files under
  `#ifdef ENABLE_OPENXR` (see the local edits), so the other builds see exactly the old files. Not copied by hand,
  but the sync tool does take PenguinScreen2's version of both and puts those guards back from a patch, because the
  guards are the only difference. Anything new PenguinScreen2 adds to them comes in unguarded: the tool says so, and
  it has to go under `#ifdef ENABLE_OPENXR` here.
- `pcsx2/VR/StereoSettings.h` and `.cpp` are ARMSX2's own (they do not exist in PenguinScreen2). Since M1, with
  `ENABLE_OPENXR` they publish nothing: `VR::UpdateSettings` and `VR::ApplySceneStereo` are the only writers of
  `VR::StereoState` (from `[VR] Enable`, `StereoMode` and the game's profile). StereoSettings keeps only
  `[VR] DebugView` and `[VR] InterleaveEyes` as debug options, and logs once that the VR layer owns the stereo state
  (and that `[VR] StereoEnable` is ignored, if it is set). Without `ENABLE_OPENXR` it works as before.
- `3rdparty/openxr` came in the first slice (Khronos SDK 1.1.61, unchanged).

## Local edits to the copied files

Every change made here to a file copied from PenguinScreen2, to be ported back. Each one is also a hunk in
`tools/outbreak-vr/vr-local-edits/<path>.patch` (one patch per file, written by `sync-vr-layer.py --record`), which is
what a sync re-applies. If you change a copied file on purpose, run `--record` so the patch has the change.

| File | Line | What | Why |
|---|---|---|---|
| `pcsx2/VR/StereoState.h` | 60, 93 | PenguinScreen2's extra `Params` fields (`first_person`, `near_floor`, `fp_*`) and functions (`PublishHeadRoll`, `HeadRoll`, `SetRollParallaxEnabled`, `RollParallaxEnabled`, `SetDumpAllowed`, `DumpAllowed`), added to ARMSX2's file under `#ifdef ENABLE_OPENXR` | ARMSX2 builds StereoState everywhere (Android, iOS, macOS, Windows); the guard keeps those builds unchanged. Nothing to port back: PenguinScreen2 has them unguarded. |
| `pcsx2/VR/StereoState.cpp` | 8, 53 | the bodies of those functions and their `<atomic>`/`<chrono>` includes, same guard | same |
| `pcsx2/VR/XRCompositor.cpp` | 302 | M1: `stat_fp_render_pose`, `stat_fp_lag_pose` counters | the Pose-to-photon line. **Port back.** |
| `pcsx2/VR/XRCompositor.cpp` | 4501 | M1: the `p2p_*` fields of `FrameStats` | same |
| `pcsx2/VR/XRCompositor.cpp` | 4528 | M1: `NotePoseToPhoton()` (new function) | same |
| `pcsx2/VR/XRCompositor.cpp` | 4612 | M1: the `(VR) Pose-to-photon:` line in `MaybeLogFrameStats`, every 5 s while the first-person screen is up | same |
| `pcsx2/VR/XRCompositor.cpp` | 5610, 6758 | M1: `NotePoseToPhoton` after each successful submit (pacer, and legacy) | same |
| `pcsx2/VR/XRCompositor.cpp` | 6354 | M1: `RenderPose` is called into `by_render_pose` first (it was the first operand of the `||` already, so the calls and their order are unchanged), then the matching counter goes up | same |

Every M1 edit in XRCompositor.cpp is marked with a comment that starts `ARMSX2 (Steam Frame M1)`.

M3's edits to the copied files, each marked with a comment that starts `ARMSX2 (Steam Frame M3)`:

| File | Line | What | Why |
|---|---|---|---|
| `pcsx2/VR/XRSession.cpp` | 40, 280, 621 | `s_max_layer_count` from `xrGetSystemProperties`, the `(VR) Runtime limits: maxLayerCount ...` line at every session start, and `XRSession::MaxLayerCount()` | the layer budget. **Port back.** |
| `pcsx2/VR/XRSession.h` | 47 | `MaxLayerCount()` | same |
| `pcsx2/VR/XRSession.cpp` | 237 | the cylinder line says a curved screen is shown as a flat quad when the runtime has no cylinder layers | the fallback was silent. **Port back.** |
| `pcsx2/VR/XRCompositor.cpp` | 163 | the drop order (`kRank*`, `HudCardRank`) and the budget's state (`s_layer_budget` and the drop counters) | the layer budget. **Port back.** |
| `pcsx2/VR/XRCompositor.cpp` | 247 | `PublishedSet::Entry::rank` | same |
| `pcsx2/VR/XRCompositor.cpp` | 4190, 4215, 4226, 4239 | `FillPublishedSet` gives each entry its rank | same |
| `pcsx2/VR/XRCompositor.cpp` | 4489, 4536, 4568 | `FitLayerBudget`, `NoteLayerBudgetDrop` (the `(VR) Layer budget:` warning) and `BuildSubmitArray` deciding what goes before it builds the array (both the pacer and the legacy path end their frames through it) | same |
| `pcsx2/VR/XRCompositor.cpp` | 6088 | `Initialize`: the budget is `min(maxLayerCount, kLayerCapacity + 1)`; the `(VR) Layer budget:` and `(VR) Screens:` lines | same |
| `pcsx2/VR/XRCompositor.cpp` | 6542 | `(VR) Screen: a N degree curve is asked for, but the runtime has no cylinder layers ...`, once per session | the fallback was silent. **Port back.** |
| `pcsx2-qt/Settings/VRSettingsWidget.cpp` | 143 | Edge Fill's recommended value says Unchecked | `[VR] EdgeFill` defaults to false (Config.h). **Port back** (PenguinScreen2's text says Checked). |
| `pcsx2-qt/Settings/VRSettingsWidget.cpp` | 237 | the status banner names `launch-outbreak-vr.sh` | the Frame's launcher; PenguinScreen2 names its own script, so nothing to port back. |
| `tools/outbreak-vr/pine.py` | 1-19, 62-95, 127, 439, 450-456, 491 | the transport: `unix_socket_path`, `endpoint` (TCP on Windows, `$XDG_RUNTIME_DIR/pcsx2.sock` on Linux, `PINE_SOCKET`, `PINE_TCP`, `PINE_HOST`), `Pine(slot, host, timeout, path)`, `wait_for_pine`, and the offline check's ini (ARMSX2's off Windows) | PINE on Linux is a Unix socket. **Port back**: PenguinScreen2's own tools then work against a Linux build, and nothing changes on Windows. |

With 16 or more layers (the OpenXR minimum, and what SteamVR offers) the layer budget drops nothing, so the compositor
submits exactly what it did.

**M2 made no edit to the copied files.** Its changes are all in shared ARMSX2 files (hook table below). A few of them
differ from PenguinScreen2 on purpose, and are worth porting back:

| ARMSX2 file | Line | What | Why |
|---|---|---|---|
| `pcsx2/VMManager.cpp` | 3137 | `FrameAdvance` refuses online | frame advance ends in a pause through `SetState`, not `SetPaused`, so the `SetPaused` lock misses it. PenguinScreen2 refuses it only in the FrameAdvance hotkey; ARMSX2's PINE `MsgFrameAdvance` comes here too. **Port back.** |
| `pcsx2/VMManager.cpp` | 395 | `OnlineLockRefusesStateLoad` and `OnlineLockRefusesStateSave` | PenguinScreen2's four refusals as two helpers, with the same log lines and messages. Optional. |
| `pcsx2/Hotkeys.cpp` | 51 | the target-speed lock is in `HotkeyAdjustTargetSpeed`, not in the IncreaseSpeed and DecreaseSpeed hotkeys | an `#ifdef` inside a `DEFINE_HOTKEY(...)` argument is a directive inside macro arguments, which is not portable, so the per-hotkey guards are not ported. VMManager refuses the limiter, frame advance and state hotkeys itself, with its own message. |
| `pcsx2/Hotkeys.cpp` | 173 | `CanPause` refuses online only while the VM is running | a VM paused before the adapter was switched on can still be resumed with the hotkey. **Port back.** |
| `pcsx2-qt/MainWindow.cpp` | 118 | `PauseLockedOnline` asks `VMManager::IsOnlineLocked()` | so `PCSX2_TEST_ONLINE_LOCKS=1` also covers the Qt pause buttons (PenguinScreen2 reads `DEV9.EthEnable`). **Port back.** |
| `pcsx2/PINE.cpp` | 1532, 1555 | `MsgSaveState` and `MsgLoadState` refuse through `VMManager::IsOnlineLocked()` | same reason |
| `pcsx2/PINE.cpp` | 1165 | `MsgVRMemWatch` logs once that watches never report a hit | no ARMSX2 EE recompiler has the MemWatch hit hook (see the iR5900 row) |

**clang 17 fixes: none were needed.** The first CI build with the whole VR layer (run 38212330071, clang 17,
libstdc++ 14, ubuntu-24.04-arm) compiled and linked every copied file as it is, so nothing has to be ported back
to PenguinScreen2 from this slice. (The VR layer was written Linux-first, and its printf formats, 64-bit literals
and includes were already portable.)

## Shims in shared ARMSX2 files (all under `#ifdef ENABLE_OPENXR`)

What the VR layer needs from the rest of the emulator that ARMSX2 did not have:

| File | What |
|---|---|
| `pcsx2/Config.h`, `pcsx2/Pcsx2Config.cpp` | `Pcsx2Config::VROptions` (copied unchanged from PenguinScreen2 `Config.h`), its `LoadSave` (ini section `[VR]`), `operator==`/`!=`, the `VR` member and its `LoadSave` call; `EmuFolders::VRProfiles` (`[Folders] VRProfiles`, default `vrprofiles` under the data folder) with its default, log line and folder creation. Its keys do not clash with StereoSettings' `[VR]` keys. |
| `pcsx2/VMManager.h/.cpp` | `VMManager::IsOnlineLocked()`: the network adapter is on, or `PCSX2_TEST_ONLINE_LOCKS=1`. Since M2, VMManager enforces the online locks as PenguinScreen2 does (hook table). |
| `pcsx2/GS/Renderers/Common/GSDevice.h/.cpp` | `GSDevice::ShadeBoost(const GSVector4& params)`: ShadeBoost with explicit parameters, per layer, for the headset's presentation lift. |
| `pcsx2/SIO/Pad/PadBase.h`, `PadDualshock2.h` | `PadBase::GetAxisScale()` (1.0 by default; the DualShock 2 returns its axis scale), read by VRInputSource. |
| `pcsx2/Input/InputManager.h/.cpp` | `InputSourceType::VR` and its name `"VR"` (on by default), the VRInputSource registration, the VR binding overlay (`SetVRBindingOverlay`, `GetVRBindingOverlay`, `HasVRBindingOverlay`) merged into the pad and USB bindings, and the rumble hooks (`GameFeedback::OnPadRumble`, `PauseRumble`). |
| `cmake/BuildParameters.cmake` | `ENABLE_OPENXR` joins `PCSX2_DEFS`. |
| `pcsx2/CMakeLists.txt` | `pcsx2VRSources`/`pcsx2VRHeaders` (PenguinScreen2's lists without StereoState), outside the LTO list; `openxr_loader` linked into `PCSX2_FLAGS`; `VR/XRSession.cpp` and `VR/SeatSession.cpp` with `-fexceptions` and no precompiled header. |
| `pcsx2-qt/CMakeLists.txt` | M3: `Settings/VRSettingsWidget.*` only `if(ENABLE_OPENXR)`, as PenguinScreen2 does with `ENABLE_VR`. |

## Hook sites

PenguinScreen2 line numbers are at c2c5ee2; ARMSX2 line numbers are the `#ifdef ENABLE_OPENXR` lines on
`outbreak-vr-frame` after M3.

| Where | PenguinScreen2 | ARMSX2 | Status |
|---|---|---|---|
| GSDeviceVK: include VRVulkanBridge.h (+ VRManager.h) | GSDeviceVK.cpp 14 | GSDeviceVK.cpp 43 | M0, done (first slice; VRManager.h added) |
| GSDeviceVK: instance through OpenXR | 136 | 240 | M0, done; also records the requested instance extensions for the driver report |
| GSDeviceVK: device through OpenXR | 666 | 1016 | M0, done (first slice), same as PenguinScreen2 |
| GSDeviceVK: queue lock in WaitForGPUIdle | 1138 | 1755 | M0, done |
| GSDeviceVK: queue lock from vkQueueSubmit through vkQueuePresentKHR | 1328, 1351 | 2127, 2174 | M0, done. ARMSX2's LSFG present sits between the two; it runs under the lock but LSFG is off in VR. |
| GSDeviceVK: queue lock on the spin submit (graphics queue) | 2024 | 2943 | M0, done |
| GSDeviceVK: OnGSDeviceDestroyed before the device goes | 2615 | 3580 | M0, done (first slice), same order as PenguinScreen2 |
| GSDeviceVK: frame generation (LSFG) off while VR is requested | none (PenguinScreen2 has no LSFG) | 3981 | M0, done. LSFG is stubbed out on the Linux build anyway (ARMSX2_HAS_LSFG is Android only). |
| GSDeviceVK: BeginVulkanBootstrap | 2975 | 4170 | M0, done (first slice) |
| GSDeviceVK: the runtime's physical device | 3036 | 4246 | M0, done (first slice) |
| GSDeviceVK: OnVulkanDeviceCreated | 3086 | 4303 | M0, done (first slice) |
| GSDeviceVK: DATE stencil warning on a stereo target | 6527 | none | not needed: ARMSX2's stereo renderer has its own |
| GSRenderer: includes | GSRenderer.cpp 15 | GSRenderer.cpp 28 | M0, done |
| GSRenderer: presentation lift (`VR::PresentationLift`, `ShadeBoost(lift)`) | 239 | 432 | M0, done |
| GSRenderer: `VR::EnsureFrameSubmitted` on a skipped non-blank present | 626 | 1177 | M0, done (ARMSX2 also skips blank frames there; those are blank, so they owe nothing) |
| GSRenderer: `VR::EndOfFrame`, `StereoDump::MaybeCapture` after the present | 696 | 1364 | M0, done |
| GSRenderer: `VR::EndOfFrame` in PresentCurrentFrame | 956 | 1623 | M0, done |
| GSRenderer: `PCSX2_VR_SNAPSHOT_LAYER` in SaveSnapshotToMemory | 1023 | 1709, 1722 | M1, done (`current` is non-const under `ENABLE_OPENXR` only) |
| VMManager: includes | VMManager.cpp 6 | VMManager.cpp 7 | M0, done; M1 added HudCards and SplitState |
| VMManager: `IsOnlineLocked` (and M2's state load and save refusal helpers) | 332 | 395 | M0 shim, done; M2 added the helpers |
| VMManager: `VR::UpdateSettings` in CPUThreadInitialize | 437 | 570 | M0, done |
| VMManager: `VR::UpdateSettings` in UpdateDiscDetails | 1125 | 1503 | M0, done |
| VMManager: `VR::ClearSpatialControls` in ClearDiscDetails | 1155 | 1535 | M0, done (after releasing the info lock, as PenguinScreen2 does) |
| VMManager: `CameraDriver::OnStateLoaded`, `GameFeedback::OnStateLoaded` in DoLoadState | 1864 | 2640 | M0, done |
| VMManager: per-vsync block (`CameraDriver::Apply`, `GameFeedback::Poll`, `HudCards::Poll`, `ApplySceneStereo`, `SplitState::Apply`, GS dump trigger) | 2877 | 3796 | M1, done, same order as PenguinScreen2 |
| VMManager: `StereoSettings::Apply` in LoadSettings | none | 788 (ENABLE_VR) | M1: publishes nothing with `ENABLE_OPENXR` (see above) |
| VMManager: `VR::UpdateSettings` when `[VR]` changes | 3113 | 4089 | M0, done |
| VMManager online lock: Initialize (the boot turbo/unlimited options are ignored, the limiter is Nominal) | 1471 | 2205 | M2, done |
| VMManager online lock: DoLoadState, DoSaveState | 1845, 1880 | 2623, 2656 | M2, done |
| VMManager online lock: LoadState (before DoLoadState, so a refusal does not reset the VM), LoadStateFromSlot | 2016, 2048 | 2803, 2836 | M2, done |
| VMManager online lock: SetLimiterMode (Nominal only), UpdateTargetSpeed (held at 100%, no host-refresh retiming) | 2144, 2201 | 2931, 2987 | M2, done |
| VMManager online lock: FrameAdvance | none (the hotkey only) | 3137 | M2, ARMSX2's own (see the local edits) |
| VMManager online lock: SetPaused | 2747 | 3646 | M2, done |
| VMManager online lock: CheckForDEV9ConfigChanges (adapter switched on: Nominal, speed re-clamped) | 3017 | 3962 | M2, done |
| MainWindow: `PauseLockedOnline` | MainWindow.cpp 1415 | MainWindow.cpp 118 | M2, done (asks `IsOnlineLocked`) |
| MainWindow: the pause buttons refused online (`onPauseActionToggled`) | 630, 633, 1425 | 667, 687 (a lambda in place of the slot) | M2, done |
| MainWindow: no game-list view online (it pauses) | 1447 | 1455 | M2, done |
| InputManager.h: `InputSourceType::VR`, binding overlay declarations | InputManager.h 32, 252 | InputManager.h 31, 322 | M0, done |
| InputManager.cpp: GameFeedback include | 12 | 23 | M0, done |
| InputManager.cpp: overlay storage | 121 | 145 | M0, done |
| InputManager.cpp: `"VR"` source name | 650 | 726 | M0, done |
| InputManager.cpp: VR source on by default | 682 | 758 | M0, done |
| InputManager.cpp: overlay in the pad bindings | 847 | 940 | M0, done |
| InputManager.cpp: overlay in the USB bindings | 971 | 1069 | M0, done |
| InputManager.cpp: `GameFeedback::OnPadRumble` | 1375 | 1546 | M0, done |
| InputManager.cpp: `GameFeedback::PauseRumble` | 1426 | 1625 | M0, done |
| InputManager.cpp: overlay functions | 1718 | 1988 | M0, done |
| InputManager.cpp: VRInputSource include and registration | 1794, 1805 | 2030, 2041 | M0, done |
| QtHost: includes | QtHost.cpp 37 | QtHost.cpp 39 | M0, done (VRManager, VRProfileDB; DepthHistogram comes with the qhist arguments) |
| QtHost: no pause on a controller disconnect online, or for a VR controller with OnlineSafe | 1763 | 1736 | M0, done |
| QtHost: help text for `--vr` and `-vr-info` | 2148 | 2139 | M0, done (`--vr-seat` and `--vr-cast` left out) |
| QtHost: `--vr` / `-vr`, `-vr-info` | 2196 | 2185 | M0, done. `--vr-seat`, `--vr-cast` and `--qhist-live` are not ported. |
| QtHost: `ProfileDB::ValidateAtLaunch` dialog | 2527 | 2501 | M0, done; each issue is also written to the log, because on the Frame the dialog may open where nobody sees it |
| SettingsWindow: the VR settings page (VRSettingsWidget) | SettingsWindow.cpp 25, 201 | SettingsWindow.cpp 25, 207 | M3, done (after Achievements, as in PenguinScreen2) |
| PadDualshock2: includes (PadLook, SplitState) | PadDualshock2.cpp 15 | PadDualshock2.cpp 15 | M2, done |
| PadDualshock2: pad 1's buttons through `PadLook::ProbeButtons`, the L1+R1+L3+R3 recenter chord | 205, 210 | 218 | M2, done |
| PadDualshock2: right stick X through `PadLook::ApplyRx` (not in split-screen), the stick and Cross/Square pressure probes | 262-310 | 282, 294, 300, 309, 327, 333 | M2, done |
| Hotkeys: includes (CameraDriver, HudCards, XRCompositor) | Hotkeys.cpp 12 | Hotkeys.cpp 12 | M2, done |
| Hotkeys: `HotkeyAdjustVRBrightness` | 99 | 122 | M2, done |
| Hotkeys: VRRecenterHead, VRBrightnessUp, VRBrightnessDown | 198 | 217 | M2, done |
| Hotkeys: online locks (`OnlineLocked`, slot load and save, `CanPause`, target speed) | 118-157, and in 12 hotkeys | 34, 51, 141, 162, 173 | M2, done in the functions; the per-hotkey guards are not ported (see the local edits) |
| PINE: includes, the 0xE0-0xE5 enum values, `ReadF32` and `ReadPose` | PINE.cpp 11, 149, 205 | PINE.cpp 17, 242, 352 | M2, done |
| PINE: MsgVRQhistFlush, MsgVRMemWatch, MsgVRMemWatchPoll, MsgVRFakeInput, MsgVRTelemetry, MsgVRMenuPointer | 552 | 1080 | M2, done, copied as it is (plus the MemWatch note). ARMSX2's own commands (0x10-0x15, MsgPadSet 0x30) are unchanged. MsgVRQhistFlush is refused until `--qhist-live` is ported. |
| PINE: MsgSaveState, MsgLoadState refused online | 983, 1004 | 1532, 1555 | M2, done |
| PINE: the Unix socket's path in the log (`PINE: Listening on ...`) | none | 781 | M3, ARMSX2's own: the Linux path logged nothing, and the tools need it |
| GSRendererHW: includes (DepthHistogram) | 14 | 34 | M1, done |
| GSRendererHW: Z-to-1/w fit | 25 | 39 (ENABLE_VR) | ARMSX2's own port of it, on every stereo build |
| GSRendererHW: snapshot layer on the frame and render-target dumps | 383, 419, 4776, 4968 | 37 (`VRSnapshotLayer`), 443, 473, 5928, 6084 | M1, done |
| GSRendererHW: first-person stereo constants (near floor, roll parallax), the HUD collimation, ZFIT and NDC log lines, the DepthHistogram draw classes | 5877 | 7406, 7441, 7508, inside ARMSX2's `DetermineVRStereoConfig` (ENABLE_VR) | M1, done. Not ported: the `PCSX2_VR_PINQ1`, `PCSX2_VR_INTERLEAVE` and `PCSX2_VR_HUDCOLL` debug switches (ARMSX2 has `[VR] InterleaveEyes`) |
| GSRendererHW: display-chain promotion of the draw's target | 8911 | 11624 | M1: checks `StereoState::Get().enabled` (and `!InterleaveEyes()`) with `ENABLE_OPENXR`, `StereoSettings::PerEyeTargets()` without |
| GSTextureCache: display-chain stereo promotion, two-layer targets | GSTextureCache.cpp 13, 81, 3505, 6368 | 4265, 7817 (ARMSX2's own per-eye targets, ENABLE_VR) | M1: the same switch as above |
| tfx.glsl (Vulkan): the linear map's near floor (`vr_splits.x`), the roll terms (`vr_splits.y/.z`) | 63, 116-129, 483-496 | 75, 149, 579 | M1, done. Each new term applies only when its constant is non-zero, so every shader without first person computes exactly what it did. The OpenGL, D3D and Metal shaders are not changed (the Frame uses Vulkan). |
| gsrunner: stereo replay, qhist | Main.cpp (9 sites) | none | M2 (headset-free test kit; GS dumps never go in CI) |
| iR5900 (x86): MemWatch hit hook | iR5900.cpp 19, 1479 | none | needs an arm64 memcheck; MemWatch stays inactive (PINE arms watches and logs once that they never report a hit) |

`tools/outbreak-vr/hook-audit.sh` checks that every line added to a shared file since 3cf66e03aa that mentions the
VR layer or OpenXR is under `#ifdef ENABLE_OPENXR` (the copied VR settings page counts as part of the VR layer) (ARMSX2's own stereo code under `ENABLE_VR` is exempt), and that
`ENABLE_OPENXR` still defaults to OFF. The frame-smoke CI job runs it.

`tools/outbreak-vr/hook-parity.py` (M1) checks the other direction: every `#ifdef ENABLE_VR` block in a PenguinScreen2
shared file is in ARMSX2. It reads PenguinScreen2 at the commit in `tools/outbreak-vr/PENGUINSCREEN2_COMMIT` (the one
`pcsx2/VR` was copied from), normalises both sides (no comments, whitespace folded, string literals emptied so log
wording may differ, code split into statements at `;`, `{` and `}`), and looks for each block's statements in the
code of the same ARMSX2 file that is under `ENABLE_OPENXR` or `ENABLE_VR`. The order is not checked, since ARMSX2's
copy is a port by reading. A block that is not fully there must be in `tools/outbreak-vr/hook-parity-allowlist.txt`
with a reason: `skip` (not ported, with the milestone), `differs` (ported in another form) or `omit:<regex>` (ported
less the statements the regexes name; the rest is still checked). It also checks the three first-person terms in
the Vulkan shader. The frame-smoke job checks out PenguinScreen2 read only and runs it. Locally:
`python3 tools/outbreak-vr/hook-parity.py --ps2 <PenguinScreen2 checkout> -v` lists every block.

## Keeping the two copies in step (M5)

`tools/outbreak-vr/sync-vr-layer.py` (Python 3.8 or newer, standard library and git only) brings a newer PenguinScreen2
commit into the copied files and puts ARMSX2's local edits back on top. It is a dry run unless given `--apply`:

    python3 tools/outbreak-vr/sync-vr-layer.py --ps2 <PenguinScreen2 checkout> --commit <commit or branch>
    python3 tools/outbreak-vr/sync-vr-layer.py --ps2 <PenguinScreen2 checkout> --commit <commit or branch> --apply

PenguinScreen2 is only read, from its git objects, so its checkout can be on any branch with any changes in it. A run
does five things:

1. **Lists what changed** in the copied files between `PENGUINSCREEN2_COMMIT` and the new commit (lines added and
   removed, and the commits that touched them). It also lists the files ARMSX2 does not copy but ports by hand
   (`pcsx2/Config.h` with `VROptions`, `VMManager.cpp` with the online locks, the GS, input and Qt hooks, the Vulkan
   shader, the CMake lists) that changed and mention the VR layer. A patch cannot carry those over: read each diff.
2. **Takes the new versions** of the copied files: everything in `pcsx2/VR` that PenguinScreen2 has (so also
   `StereoState.*`, never `StereoSettings.*` or this file), `pcsx2/Input/VRInputSource.*`,
   `bin/resources/vr-profiles`, the VR settings page, and `tools/outbreak-vr/pine.py`. A file PenguinScreen2 adds in
   one of those folders is copied (it is in no CMake list: add it by hand); a file it removes is left in place and
   named. PenguinScreen2's files have CRLF line endings and ARMSX2's mostly LF: the tool compares and patches with the
   line endings folded, and writes each file with the line endings the ARMSX2 file already has (LF for a new one).
3. **Re-applies the local edits.** They are `tools/outbreak-vr/vr-local-edits/<path>.patch`, one unified diff per file
   that differs from PenguinScreen2 at `PENGUINSCREEN2_COMMIT`. A hunk is looked for as it was written, at the nearest
   place to where the last hunk landed, so line numbers do not matter (`-v` says where each one went, for the tables
   above). A hunk that is already in the new copy (ported back) is skipped. A hunk that cannot be placed is reported
   with the closest place in the new file and the first line that differs; `--apply` writes it to `<file>.rej` and
   the rest of the file as far as it applied. `--fuzz N` lets a hunk lose N lines of context at each end.
4. **Updates `PENGUINSCREEN2_COMMIT` and the source line at the top of this file** (branch, commit, date, subject;
   the branch is the `--commit` argument when it names a branch, else `--branch`). The other places that still name
   the old commit (the copied-files list, the line numbers of the hook table) are listed for you to update.
5. **Runs `hook-parity.py`** against the new commit and prints its result (`-v` for every block). It needs no
   checkout of the new commit: the tool writes the files it reads to a temporary folder.

When `--apply` writes without a conflict it also writes the patch files again against the new commit and checks
itself with a dry run (it must show no changes). When it leaves `.rej` files, make those edits by hand in the file,
delete the `.rej`, and run `sync-vr-layer.py --ps2 <checkout> --record`, which writes the patches from the difference
between ARMSX2's copies and PenguinScreen2 at `PENGUINSCREEN2_COMMIT`. `--record` is also what to run after changing a
copied file on purpose.

`--apply` refuses to run when a copied file has an edit that is not in a patch (it would lose it), when a patch does
not apply to the recorded commit, or while a `.rej` file is there; it says which file. Exit codes: 0 clean; 1 a hunk or
a patch needs a decision, or hook parity fails; 2 bad arguments or an unusable checkout.

What the tool cannot judge:

- A patch can apply and still be wrong, when PenguinScreen2 changed what the edit depended on. The Edge Fill hunk in
  `VRSettingsWidget.cpp` says Unchecked because ARMSX2's `Config.h` defaults `[VR] EdgeFill` to false; when
  `Config.h` is ported to PenguinScreen2's true, that hunk has to go (`--record` drops it once the file is right).
- The hand-ported files (step 1) and the hook sites: the parity check covers only PenguinScreen2's `#ifdef ENABLE_VR`
  blocks, so a change in an unguarded part of `Config.h` is only in the list from step 1.
- `StereoState.*` is compiled by every ARMSX2 stereo build. New code PenguinScreen2 adds there arrives unguarded.

`tools/outbreak-vr/sync-vr-layer-test.py` checks the tool on small made-up repos (conflicts, ported-back edits, new and
removed files, unrecorded edits, `--apply` and `--record`).

First runs (2026-10-12, PenguinScreen2 checkout on `headset1`, which the tool does not care about): `--record` wrote
7 patches (`XRCompositor.cpp` 16 hunks, `pine.py` 5, `XRSession.cpp` 4, `StereoState.h` 4, `StereoState.cpp` 3,
`VRSettingsWidget.cpp` 2, `XRSession.h` 1). A dry run against the commit recorded then shows no change, 7 of 7 patches apply, and parity
passes. A dry run against `headset1` (29fec71) lists 5 copied files (`CameraDriver.cpp`, `HudCards.h`, `VRSettingsWidget.cpp`
and the two Outbreak profiles), applies all 7 patches, passes parity, and lists `pcsx2/Config.h` and
`pcsx2/VMManager.cpp` as hand-ported files that changed.

## Status of M0 (2026-10-12)

Done and green in CI ("Outbreak VR (Steam Frame build)" on `outbreak-vr-frame`: run 38212330071 on 64bccdc7a0 for
1 below, run 38214286969 on 6560910264 for 1-3; the build, the unit tests and frame-smoke all passed):

1. The VR layer, the profiles and the shims, built with clang 17 on ubuntu-24.04-arm.
2. The M0 hooks above: the queue lock, LSFG off in VR, the GSRenderer frame hooks, the VMManager settings
   hooks, the input source and binding overlay, `--vr`, `-vr-info`, the profile check at launch and the
   no-pause rule.
3. `frame/launch-outbreak-vr.sh` with the first-run ini, the frame-smoke additions and the hook audit.

The first-run ini, checked: it has `[UI] SettingsVersion = 1`, so ARMSX2 loads it as it is instead of writing
its defaults over it (frame-smoke runs `-testconfig` on it and checks nothing was reset). Keys it leaves out keep
ARMSX2's defaults: Pad1 is a DualShock 2, the `VR` input source is on, and the BIOS folder is
`~/.config/ARMSX2/bios`. The ini writes no pad bindings, so Pad1 gets only the VR profile's VR Gamepad bindings
(the binding overlay, section `Pad1`); a keyboard or a Steam Input pad does nothing until bound in the ini.

## Status of M1 (2026-10-12)

CI ("Outbreak VR (Steam Frame build)", dispatched on `outbreak-vr-frame`): run 38218593123 on 6b8e52b689 failed (a
clang error in StereoSettings.cpp, a redefined local; fixed in f591c6ffa5); run 38219015927 on da2c952527 (priorities
1-3 and the launcher) passed: the build, the unit tests and frame-smoke. Run 38220908329 on 205750a009 (all six
priorities) passed: the build, the unit tests, frame-smoke (with the FRAME_VIEW checks), the hook audit and the hook
parity check.

Done (the M1 brief's six priorities):

1. **The per-vsync VR block in VMManager** (VMManager.cpp 3687): `CameraDriver::Apply`, `GameFeedback::Poll`,
   `HudCards::Poll`, the `PCSX2_VR_GSDUMP_AT_VSYNC` local dump trigger, `ApplySceneStereo`, `SplitState::Apply`, in
   PenguinScreen2's order. The head pose now drives the game camera. The JIT question: CameraDriver writes game memory
   with `memWrite*` (vtlb). The look-at, heading and matrix writes are data, which recompiled loads read every time.
   The code hooks (`camera.codeHooks`, which the two Outbreak profiles do not use) rely on the page protection that
   ARMSX2's arm64 EE recompiler has like the x86 one (`mmap_MarkCountedRamPage` in iR5900-arm64.cpp, the page-fault
   handler in vtlb.cpp), so a write into compiled code clears the block. Nothing to change.
2. **One owner for StereoState.** With `ENABLE_OPENXR`, StereoSettings publishes nothing (see "Not copied"), and the
   per-eye target checks in GSTextureCache (4265, 7817) and GSRendererHW (11624) use `StereoState::Get().enabled`
   (and `!InterleaveEyes()`) instead of `StereoSettings::PerEyeTargets()`. Other builds are unchanged.
3. **PenguinScreen2's first-person GS changes**, ported by reading at matching anchors (rows "GSRendererHW: ..." and
   "tfx.glsl" in the hook table): the near floor, roll parallax, the vr_splits shader terms, the DepthHistogram draw
   classes, the snapshot-layer hook, and the HUD collimation, ZFIT and NDC log lines. With `ENABLE_OPENXR` the HUD
   collimation also follows PenguinScreen2's rule (only rule matches are collimated). Not ported: the
   `PCSX2_VR_PINQ1`, `PCSX2_VR_INTERLEAVE` and `PCSX2_VR_HUDCOLL` debug switches.
4. **Compositor.** Checked, nothing to change in the submission: XRCompositor already sends a two-layer target as two
   per-eye swapchains of `arraySize = 1` (`EnsureSwapchains(w, h, stereo)`, `CopyEyePair` copies layer 0 and 1 into
   `s.chains[0]` and `[1]`; every `XrSwapchainCreateInfo` in the file has `arraySize = 1`), so SteamVR-for-Linux #975
   and openvr#1822 do not apply. Added: the `(VR) Pose-to-photon:` line (local edits table above).
5. **`tools/outbreak-vr/hook-parity.py`** with `hook-parity-allowlist.txt` and `PENGUINSCREEN2_COMMIT`, run by the
   frame-smoke job. At c2c5ee2: 85 PenguinScreen2 `ENABLE_VR` blocks, 46 ported, 5 ported less allowlisted parts,
   37 allowlisted (M2/M5 work, ARMSX2's own forms), plus the 3 shader terms. Every first-person and stereo block is
   ported or allowlisted.
6. **Docs:** this file, `OUTBREAK-VR.md` (what M1 adds) and `frame/README-FRAME.md` (section 3, the M1 headset
   checklist and the log lines to send). The launcher's first-run ini now turns first person and stereo on;
   `FRAME_VIEW=flat|first-person` switches an existing ini.

Not done, next:

- Nothing has run in a headset. Next is Alex's session on the Frame: the probe and M0 (`FRAME_VIEW=flat`) if not
  done yet, then the M1 checklist in `frame/README-FRAME.md` section 3.
- From the M1 logs: read the `(VR) Pose-to-photon:` lines. If the head pose is more than about one display frame
  behind at the first showing, or "placed by the lag fallback" is high, retune `camera.lookAt.syncFramesBack` (2 in
  both Outbreak profiles) and `screen.firstPerson.poseLagMs` (40) for the Frame. The headset-rate pacer re-sends
  each game frame at the display rate, so 72 Hz against a 30 fps game shows each image about 2.4 times.
- The multiview self-test on lavapipe in CI is not ported (the M1 brief's optional item).
- M2: controls (PadLook in PadDualshock2, gesture zones, recenter, the hotkeys), the online locks in VMManager,
  PINE and the gsrunner stereo replay (the headset-free test kit). The hook-parity allowlist lists each. (Done
  except the gsrunner replay: see "Status of M2" below.)
- Still open from M0: which Frame controller profile path SteamVR accepts (`/interaction_profiles/valve/frame_controller`
  in VRInput.cpp, `.../frame_controller_valve` in Valve's docs; VRInput falls back to the Touch and simple profiles).
- The Monado-in-CI stretch goal was not attempted.

## Status of M2 (2026-10-12)

CI ("Outbreak VR (Steam Frame build)", dispatched on `outbreak-vr-frame`): run 38225845140 on 6e69b43be9 (priorities
1 and 2) passed: the build, the unit tests, frame-smoke, the hook audit and the hook parity check. Run 38227924321
on 2566847c95 (all five priorities) passed the same, with the first-run ini's `SnapTurn = false` check.

Done (the M2 brief's five priorities):

1. **The online locks.** Online (network adapter on, or `PCSX2_TEST_ONLINE_LOCKS=1`) the game never pauses, changes
   speed, or loads or saves a state. VMManager refuses at SetPaused, FrameAdvance (ARMSX2's own), DoLoadState,
   DoSaveState, LoadState (before DoLoadState, so a refusal never resets the VM), LoadStateFromSlot and SetLimiterMode
   (anything but Nominal); UpdateTargetSpeed holds the speed at 100% with no host-refresh retiming; boot ignores the
   turbo and unlimited options; switching the adapter on mid-game drops to Nominal at once. The Qt pause buttons and
   the game-list view (which pauses) are refused online, so the buttons never stay checked over a running game. The
   hotkeys say why (save and load slot, pause, target speed), and PINE's MsgSaveState and MsgLoadState fail online.
2. **The controls.** PadDualshock2 sends pad 1 through PenguinScreen2's PadLook: the button and stick probes, the
   L1+R1+L3+R3 recenter chord, and the right stick's X turning the first-person view (`PadLook::ApplyRx`, not in
   split-screen). The rest of the controls (gesture zones, smooth and snap turn, the reset chord on both thumbsticks,
   the grab breakout, haptics) were already in the copied VR layer and VRInputSource; they needed only this hook.
   Hotkeys: VR recenter (head camera and screen) and headset brightness up and down (saved in `[VR] Brightness`).
3. **PINE 0xE0-0xE5**, copied as they are, next to ARMSX2's own commands (0x10-0x15 and MsgPadSet 0x30, unchanged).
   MsgVRFakeInput and the MsgVRMenuPointer test hit need `PCSX2_VR_TEST_INPUT` and are refused online.
   MsgVRQhistFlush is refused until `--qhist-live` is ported. MemWatch arms watches, but no ARMSX2 EE recompiler has
   the hit hook (the arm64 recompiler has no memcheck at all), so a watch never reports a hit; PINE logs that once.
4. **Cheap extras.** The Frame's system recenter needed nothing: the copied XRSession already handles
   `XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING` for the LOCAL space (ignored in the first 2 s of a session,
   then a recenter of first person and the screens 300 ms later). Seated play is the VR layer's default (a LOCAL
   reference space; the Outbreak profiles hang the belt pouches from the seated player, `pouches.anchor: seat`), and
   so is smooth turning; the launcher's first-run ini now says `SnapTurn = false`, and frame-smoke checks it.
5. **Docs:** this file, the hook-parity allowlist (the PadDualshock2, Hotkeys and PINE entries are gone),
   `OUTBREAK-VR.md` and `frame/README-FRAME.md` (section 4, the M2 headset checklist).

hook-parity at c2c5ee2: 85 PenguinScreen2 `ENABLE_VR` blocks; 58 ported, 5 ported less allowlisted parts, 22
allowlisted (gsrunner, the VR settings page, the DATE warning, the x86 MemWatch hook, ARMSX2's own forms), plus the
3 shader terms.

Not done, next:

- Nothing has run in a headset. Alex's next session on the Frame: the M1 checklist (README-FRAME.md section 3), then
  the M2 one (section 4).
- Not ported: the per-hotkey online guards (see the local edits; VMManager covers them), the gsrunner stereo replay
  and `--qhist-live` (GS dumps never go in CI), the VR settings page (M5; ported in M3), and the MemWatch hit hook (needs a memcheck
  in ARMSX2's arm64 recompiler).
- Not tested in CI: the locks themselves (no game runs in CI). To check them by hand: start a game with
  `PCSX2_TEST_ONLINE_LOCKS=1` and try pause, turbo, a state save and a state load; each is refused with a
  `(VMManager) ... refused` line.
- M3 next: hands, the HUD, the wrist card, the menu pointer, belt pouches and brightness in the headset. Most of it is
  in the copied VR layer already; the work is checking it on the Frame.

## Status of M3 (2026-10-12)

CI ("Outbreak VR (Steam Frame build)", dispatched on `outbreak-vr-frame`): run 38232856645 on a49528f14e (priorities
2 and 3: the layer budget and the settings page) passed: the build, the unit tests, frame-smoke, the hook audit and the
hook parity check. Run 38235007771 on 60f5acd299 (all five priorities) passed the same, with the new frame-smoke checks:
`pine-transport-test.py` (the Unix socket at the default slot, at slot 28012 and through `PINE_SOCKET`, and TCP),
`FRAME_PINE=1` and `=0`, and `tools/pine.py` in the bundle.

Done (the M3 brief's five priorities):

1. **Hook sites for the hands, the HUD, the menu pointer and the pouches: none were missing.** Everything that drives them
   outside `pcsx2/VR` was ported in M0-M2, and the hook-parity check finds no PenguinScreen2 hook for them that ARMSX2
   lacks (no PenguinScreen2 shared file calls into them outside `#ifdef ENABLE_VR`, either):
   - `VRInputSource::PollEvents` (copied) runs `BeltPouches::Poll` and then `MenuPointer::Poll` every input poll, before
     the controls compose. VMManager's own `InputManager::PollSources` calls it; the source is registered since M0.
   - The per-vsync block in VMManager (M1) runs `HudCards::Poll` (the wrist card's data, the toast, the vignette and the
     blink), and `CameraDriver::Apply` tells `HandModel` what the hands hold (`SetWeaponHeld`, from the weapon raised and
     `GameFeedback`'s held-item class).
   - XRCompositor (copied) draws the hands, the laser or menu beam, the menu dot, the wrist card, the toast, the vignette
     and the blink as layers over the screen (`BuildHandLayers`, `BuildHudLayers`) at the GS present hooks of M0.
   - The brightness hotkeys and their toast (M2), the presentation lift in GSRenderer (M0), and PINE's
     `MsgVRMenuPointer` (0xE5: menu pointer and pouch state, the test hit; M2).
   - The rule that only the player's trigger confirms is in the copied MenuPointer and BeltPouches and is unchanged:
     the pointer and a pouch squeeze only steer the game's cursor with D-pad presses (`SpatialControls::SetPointerDpad`)
     and hold a confirm back (`SetPointerConfirmHold`, swallowed until the trigger is let go) while a steer is under way;
     the trigger or A, pulled by the player, is the only confirm.
2. **Layer budget.** XRSession logs `maxLayerCount` at every session start, and the compositor submits at most
   `min(maxLayerCount, 16)` layers a frame. A frame with more drops the least important first (the toast, the wrist
   card, the lever cards, the hands, the laser, the menu dot, the vignette, the blink, then the background; the game
   screen stays), keeps the order of the rest, and logs `(VR) Layer budget:` at the first drop and then at most every
   10 s. OpenXR requires at least 16, so with a conformant runtime nothing is dropped. The quad fallback for a missing
   `XR_KHR_composition_layer_cylinder` was already there (`curved = arc >= 5 && HasCylinderLayer()` at every placement);
   it is now logged at session start (`(VR) Screens:`), in the cylinder line, and once when a screen asks for a curve.
   The Outbreak profiles use flat screens (`arc: 0`) anyway.
3. **Settings: the Qt VR settings page is ported** (`pcsx2-qt/Settings/VRSettingsWidget.*`, the "VR" page in Settings,
   under `ENABLE_OPENXR`). On the Frame the Qt window is not usable from inside the headset, so `frame/README-FRAME.md`
   also lists every `[VR]` key the Frame build reads, with its default, for editing the ini over SSH. The launcher's
   first-run ini is unchanged.
4. **pine.py over SSH.** `tools/outbreak-vr/pine.py` (copied, transport adapted) connects to the Linux PINE socket,
   `$XDG_RUNTIME_DIR/pcsx2.sock` (`/tmp/pcsx2.sock` without `XDG_RUNTIME_DIR`, `.<slot>` added for a slot other than
   28011), as `pcsx2/PINE.cpp` names it, and to TCP 127.0.0.1:<slot> on Windows. `PINE_SOCKET`, `PINE_TCP=1` and
   `PINE_HOST` override it, so the tools run on the Frame over SSH, or on the PC through
   `ssh -N -L 28011:/run/user/1000/pcsx2.sock steamos@frame`. PINE now logs its socket path on Linux (ENABLE_OPENXR
   only). The launcher's `FRAME_PINE=1` (`=0`) sets `[EmuCore] EnablePINE` in an existing ini and keeps the rest, and the
   Frame bundle carries the client as `tools/pine.py` (`appimage-qt.sh`, in its `FRAME_BUNDLE` branch only).
   `tools/outbreak-vr/pine-transport-test.py` checks the transport against a fake PINE server; frame-smoke runs it, and
   checks `FRAME_PINE` and the bundled copy.
5. **Docs:** this file, the hook-parity allowlist (the SettingsWindow entry is gone), `OUTBREAK-VR.md` and
   `frame/README-FRAME.md` (section 5, the M3 headset checklist; section 6, the settings; section 7, the tools).

hook-parity at c2c5ee2: 85 PenguinScreen2 `ENABLE_VR` blocks; 60 ported, 5 ported less allowlisted parts, 20
allowlisted (gsrunner, the DATE warning, the x86 MemWatch hook, ARMSX2's own forms), plus the 3 shader terms.

Not done, next:

- Nothing has run in a headset. Alex's next Frame session: the M1, M2 and M3 checklists (README-FRAME.md sections 3-5).
- The self-tests (`PCSX2_VR_SELFTEST=1`, `PCSX2_VR_HUD_SELFTEST=1`) run only with a game and its profile loaded (the
  menu pointer, the pouches and the HUD poll only then), so CI cannot run them; README-FRAME section 5 has Alex run them
  once on the Frame. `PCSX2_VR_FEEDBACK_SELFTEST` skips itself while VR controller input runs, so it cannot run there.
- The other research and test tools (`menutest.py`, `vrtest.py`, `grabtest.py` and the rest) are not copied: copy them
  from PenguinScreen2 when needed; they import `pine.py` and need no change for the transport. Some assume the Windows
  test PC's paths and PowerShell launchers.
- M4: performance and heat (refresh rate, resolution sweep, thread pinning, Valve's perf CSV). M5: packaging, keeping
  the two copies of `pcsx2/VR` in step (port the local edits above back), and optional co-op.
