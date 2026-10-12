# Where pcsx2/VR comes from

The OpenXR VR layer in this folder is copied from **PenguinScreen2** (https://github.com/beadbeed/PenguinScreen2),
branch `pc-firstperson`, commit **c2c5ee249e66f2552894b20c465f2fa7ef9f41ef** (2026-10-12, "STATUS.md: one page for
where Outbreak VR stands ..."). It is built only when `ENABLE_OPENXR` is on, which only
`.github/workflows/outbreak-vr.yml` does (the Steam Frame build). Every other ARMSX2 build compiles none of it and
is unchanged.

The files were copied whole, not merged: PenguinScreen2 commit 990b456 stripped comments from 938 files, so a git
merge between the two trees is noise. To update, copy the files again from a newer PenguinScreen2 commit, re-apply
the local edits below, and check the hook sites in the table at the end against PenguinScreen2's code.

## Copied files

Unchanged copies of PenguinScreen2 c2c5ee2 (39,362 lines of C++ in total):

- `pcsx2/VR/`: BeltPouches, CameraDriver, ControlQuads, DepthHistogram, GameFeedback, GrabBreakout, HandModel,
  HeadPose, HudCards, ItemData, MemWatch, MenuPointer, PadLook, SeatCast, SeatSession, SpatialControls,
  SplitState, StereoAuto, StereoDump, VRInput, VRManager, VRProfileDB, VRVulkanBridge, XRCompositor, XRSession
  (each `.cpp` and `.h`), plus `VRInputState.h`, `VRInternal.h` and `LICENSE.note`.
- `pcsx2/Input/VRInputSource.cpp` and `.h`.
- `bin/resources/vr-profiles/`: `SLPM-65428.yaml`, `SLPM-65692.yaml` (Outbreak File #1 and #2), `SLUS-20090.yaml`,
  `SLUS-20851.yaml`, `SLUS-20919.yaml`, `README.md`, `LICENSE.md`.

Not copied, on purpose:

- `pcsx2/VR/StereoState.h` and `.cpp`: ARMSX2 already had these (its stereo renderer uses them on every platform).
  PenguinScreen2's version is ARMSX2's plus a few fields and functions, which were added to ARMSX2's files under
  `#ifdef ENABLE_OPENXR` (see the local edits), so the other builds see exactly the old files.
- `pcsx2/VR/StereoSettings.h` and `.cpp` are ARMSX2's own (they do not exist in PenguinScreen2). In M0 they still
  publish the stereo state for ARMSX2's stereo renderer; `VR::UpdateSettings` publishes it too. Both say "off" with
  the Frame's first-run ini, so they agree. M1 has to pick one owner.
- `3rdparty/openxr` came in the first slice (Khronos SDK 1.1.61, unchanged).

## Local edits to the copied files

Every change made here to a file copied from PenguinScreen2, to be ported back:

| File | Line | What | Why |
|---|---|---|---|
| `pcsx2/VR/StereoState.h` | 60, 93 | PenguinScreen2's extra `Params` fields (`first_person`, `near_floor`, `fp_*`) and functions (`PublishHeadRoll`, `HeadRoll`, `SetRollParallaxEnabled`, `RollParallaxEnabled`, `SetDumpAllowed`, `DumpAllowed`), added to ARMSX2's file under `#ifdef ENABLE_OPENXR` | ARMSX2 builds StereoState everywhere (Android, iOS, macOS, Windows); the guard keeps those builds unchanged. Nothing to port back: PenguinScreen2 has them unguarded. |
| `pcsx2/VR/StereoState.cpp` | 8, 53 | the bodies of those functions and their `<atomic>`/`<chrono>` includes, same guard | same |

**clang 17 fixes: none were needed.** The first CI build with the whole VR layer (run 38212330071, clang 17,
libstdc++ 14, ubuntu-24.04-arm) compiled and linked every copied file as it is, so nothing has to be ported back
to PenguinScreen2 from this slice. (The VR layer was written Linux-first, and its printf formats, 64-bit literals
and includes were already portable.)

## Shims in shared ARMSX2 files (all under `#ifdef ENABLE_OPENXR`)

What the VR layer needs from the rest of the emulator that ARMSX2 did not have:

| File | What |
|---|---|
| `pcsx2/Config.h`, `pcsx2/Pcsx2Config.cpp` | `Pcsx2Config::VROptions` (copied unchanged from PenguinScreen2 `Config.h`), its `LoadSave` (ini section `[VR]`), `operator==`/`!=`, the `VR` member and its `LoadSave` call; `EmuFolders::VRProfiles` (`[Folders] VRProfiles`, default `vrprofiles` under the data folder) with its default, log line and folder creation. Its keys do not clash with StereoSettings' `[VR]` keys. |
| `pcsx2/VMManager.h/.cpp` | `VMManager::IsOnlineLocked()`: the network adapter is on, or `PCSX2_TEST_ONLINE_LOCKS=1`. PenguinScreen2 also enforces the online locks inside VMManager (no pause, speed change or state load/save online); that is not ported yet. |
| `pcsx2/GS/Renderers/Common/GSDevice.h/.cpp` | `GSDevice::ShadeBoost(const GSVector4& params)`: ShadeBoost with explicit parameters, per layer, for the headset's presentation lift. |
| `pcsx2/SIO/Pad/PadBase.h`, `PadDualshock2.h` | `PadBase::GetAxisScale()` (1.0 by default; the DualShock 2 returns its axis scale), read by VRInputSource. |
| `pcsx2/Input/InputManager.h/.cpp` | `InputSourceType::VR` and its name `"VR"` (on by default), the VRInputSource registration, the VR binding overlay (`SetVRBindingOverlay`, `GetVRBindingOverlay`, `HasVRBindingOverlay`) merged into the pad and USB bindings, and the rumble hooks (`GameFeedback::OnPadRumble`, `PauseRumble`). |
| `cmake/BuildParameters.cmake` | `ENABLE_OPENXR` joins `PCSX2_DEFS`. |
| `pcsx2/CMakeLists.txt` | `pcsx2VRSources`/`pcsx2VRHeaders` (PenguinScreen2's lists without StereoState), outside the LTO list; `openxr_loader` linked into `PCSX2_FLAGS`; `VR/XRSession.cpp` and `VR/SeatSession.cpp` with `-fexceptions` and no precompiled header. |

## Hook sites

PenguinScreen2 line numbers are at c2c5ee2; ARMSX2 line numbers are the `#ifdef ENABLE_OPENXR` lines on
`outbreak-vr-frame` after this slice.

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
| GSRenderer: `PCSX2_VR_SNAPSHOT_LAYER` | 1023 | none | M1 |
| VMManager: includes | VMManager.cpp 6 | VMManager.cpp 7 | M0, done (CameraDriver, GameFeedback, VRManager; HudCards and SplitState come with M1) |
| VMManager: `IsOnlineLocked` | 332 | 391 | M0 shim, done |
| VMManager: `VR::UpdateSettings` in CPUThreadInitialize | 437 | 543 | M0, done |
| VMManager: `VR::UpdateSettings` in UpdateDiscDetails | 1125 | 1476 | M0, done |
| VMManager: `VR::ClearSpatialControls` in ClearDiscDetails | 1155 | 1508 | M0, done (after releasing the info lock, as PenguinScreen2 does) |
| VMManager: `CameraDriver::OnStateLoaded`, `GameFeedback::OnStateLoaded` in DoLoadState | 1864 | 2598 | M0, done |
| VMManager: per-vsync block (`CameraDriver::Apply`, `GameFeedback::Poll`, `HudCards::Poll`, `ApplySceneStereo`, `SplitState::Apply`, GS dump trigger) | 2877 | none | **M1** |
| VMManager: `VR::UpdateSettings` when `[VR]` changes | 3113 | 3934 | M0, done |
| VMManager: online locks in SetPaused, DoLoadState, DoSaveState, SetLimiterMode, UpdateTargetSpeed | 1471-2747 | none | M2 or before any online play |
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
| SettingsWindow: the VR settings page (VRSettingsWidget) | SettingsWindow.cpp 25, 201 | none | M5 |
| PadDualshock2: PadLook probes, recenter chord, right-stick look | PadDualshock2.cpp 15-310 | none (only `GetAxisScale`) | M1/M2 |
| Hotkeys: VR recenter, headset brightness up/down | Hotkeys.cpp 12, 99, 198 | none | M2 |
| PINE: VR fake input, qhist flush (0xE0-0xE5) | PINE.cpp 11, 205, 552 | none | M2 (headset-free test kit) |
| GSRendererHW: first-person changes | GSRendererHW.cpp (9 sites) | none | M1 |
| GSTextureCache: display-chain stereo promotion | GSTextureCache.cpp 13, 81, 3505, 6368 | none (ARMSX2 has its own per-eye targets) | M1, compare with ARMSX2's |
| tfx.glsl, gsrunner | various | none | M1 |
| iR5900 (x86): MemWatch hit hook | iR5900.cpp 19, 1479 | none | needs an arm64 memcheck; MemWatch stays inactive |

`tools/outbreak-vr/hook-audit.sh` checks that every line added to a shared file since 3cf66e03aa that mentions the
VR layer or OpenXR is under `#ifdef ENABLE_OPENXR` (ARMSX2's own stereo code under `ENABLE_VR` is exempt), and that
`ENABLE_OPENXR` still defaults to OFF. The frame-smoke CI job runs it.

## Status of this slice (2026-10-12)

Done and green in CI (see the run ids in the commit messages and `frame/README-FRAME.md`):

1. The VR layer, the profiles and the shims, built with clang 17 on ubuntu-24.04-arm.
2. The M0 hooks above: the queue lock, LSFG off in VR, the GSRenderer frame hooks, the VMManager settings
   hooks, the input source and binding overlay, `--vr`, `-vr-info`, the profile check at launch and the
   no-pause rule.
3. `frame/launch-outbreak-vr.sh` with the first-run ini, the frame-smoke additions and the hook audit.

Not done, next:

- Nothing has run in a headset. The next step is Alex's M0 session on the Frame (checklist in
  `frame/README-FRAME.md`): run `frame-xrprobe` first, then the game.
- The per-vsync VR block in VMManager and everything else marked M1 above (first-person camera, stereo,
  render-pose sync), then M2 (controls, online locks, hotkeys).
- Needs investigation once the probe log is back: which Frame controller profile path SteamVR accepts.
  VRInput.cpp suggests `/interaction_profiles/valve/frame_controller`; Valve documents
  `/interaction_profiles/valve/frame_controller_valve`. VRInput falls back to the Touch and simple profiles,
  so M0 should work either way, but the button names may differ.
- The Monado-in-CI stretch goal (Step 7.6 of the M0 brief) was not attempted in this slice.
