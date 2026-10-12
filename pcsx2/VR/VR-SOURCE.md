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
- `pcsx2/VR/StereoSettings.h` and `.cpp` are ARMSX2's own (they do not exist in PenguinScreen2). Since M1, with
  `ENABLE_OPENXR` they publish nothing: `VR::UpdateSettings` and `VR::ApplySceneStereo` are the only writers of
  `VR::StereoState` (from `[VR] Enable`, `StereoMode` and the game's profile). StereoSettings keeps only
  `[VR] DebugView` and `[VR] InterleaveEyes` as debug options, and logs once that the VR layer owns the stereo state
  (and that `[VR] StereoEnable` is ignored, if it is set). Without `ENABLE_OPENXR` it works as before.
- `3rdparty/openxr` came in the first slice (Khronos SDK 1.1.61, unchanged).

## Local edits to the copied files

Every change made here to a file copied from PenguinScreen2, to be ported back:

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
| GSRenderer: `PCSX2_VR_SNAPSHOT_LAYER` in SaveSnapshotToMemory | 1023 | 1709, 1722 | M1, done (`current` is non-const under `ENABLE_OPENXR` only) |
| VMManager: includes | VMManager.cpp 6 | VMManager.cpp 7 | M0, done; M1 added HudCards and SplitState |
| VMManager: `IsOnlineLocked` | 332 | 391 | M0 shim, done |
| VMManager: `VR::UpdateSettings` in CPUThreadInitialize | 437 | 543 | M0, done |
| VMManager: `VR::UpdateSettings` in UpdateDiscDetails | 1125 | 1476 | M0, done |
| VMManager: `VR::ClearSpatialControls` in ClearDiscDetails | 1155 | 1508 | M0, done (after releasing the info lock, as PenguinScreen2 does) |
| VMManager: `CameraDriver::OnStateLoaded`, `GameFeedback::OnStateLoaded` in DoLoadState | 1864 | 2598 | M0, done |
| VMManager: per-vsync block (`CameraDriver::Apply`, `GameFeedback::Poll`, `HudCards::Poll`, `ApplySceneStereo`, `SplitState::Apply`, GS dump trigger) | 2877 | 3687 | M1, done, same order as PenguinScreen2 |
| VMManager: `StereoSettings::Apply` in LoadSettings | none | 766 (ENABLE_VR) | M1: publishes nothing with `ENABLE_OPENXR` (see above) |
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
| GSRendererHW: includes (DepthHistogram) | 14 | 34 | M1, done |
| GSRendererHW: Z-to-1/w fit | 25 | 39 (ENABLE_VR) | ARMSX2's own port of it, on every stereo build |
| GSRendererHW: snapshot layer on the frame and render-target dumps | 383, 419, 4776, 4968 | 37 (`VRSnapshotLayer`), 443, 473, 5928, 6084 | M1, done |
| GSRendererHW: first-person stereo constants (near floor, roll parallax), the HUD collimation, ZFIT and NDC log lines, the DepthHistogram draw classes | 5877 | 7406, 7441, 7508, inside ARMSX2's `DetermineVRStereoConfig` (ENABLE_VR) | M1, done. Not ported: the `PCSX2_VR_PINQ1`, `PCSX2_VR_INTERLEAVE` and `PCSX2_VR_HUDCOLL` debug switches (ARMSX2 has `[VR] InterleaveEyes`) |
| GSRendererHW: display-chain promotion of the draw's target | 8911 | 11624 | M1: checks `StereoState::Get().enabled` (and `!InterleaveEyes()`) with `ENABLE_OPENXR`, `StereoSettings::PerEyeTargets()` without |
| GSTextureCache: display-chain stereo promotion, two-layer targets | GSTextureCache.cpp 13, 81, 3505, 6368 | 4265, 7817 (ARMSX2's own per-eye targets, ENABLE_VR) | M1: the same switch as above |
| tfx.glsl (Vulkan): the linear map's near floor (`vr_splits.x`), the roll terms (`vr_splits.y/.z`) | 63, 116-129, 483-496 | 75, 149, 579 | M1, done. Each new term applies only when its constant is non-zero, so every shader without first person computes exactly what it did. The OpenGL, D3D and Metal shaders are not changed (the Frame uses Vulkan). |
| gsrunner: stereo replay, qhist | Main.cpp (9 sites) | none | M2 (headset-free test kit; GS dumps never go in CI) |
| iR5900 (x86): MemWatch hit hook | iR5900.cpp 19, 1479 | none | needs an arm64 memcheck; MemWatch stays inactive |

`tools/outbreak-vr/hook-audit.sh` checks that every line added to a shared file since 3cf66e03aa that mentions the
VR layer or OpenXR is under `#ifdef ENABLE_OPENXR` (ARMSX2's own stereo code under `ENABLE_VR` is exempt), and that
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

When `pcsx2/VR` is copied again from a newer PenguinScreen2 commit, put that commit in `PENGUINSCREEN2_COMMIT`, run
the parity check, and port or allowlist whatever it reports.

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
  PINE and the gsrunner stereo replay (the headset-free test kit). The hook-parity allowlist lists each.
- Still open from M0: which Frame controller profile path SteamVR accepts (`/interaction_profiles/valve/frame_controller`
  in VRInput.cpp, `.../frame_controller_valve` in Valve's docs; VRInput falls back to the Touch and simple profiles).
- The Monado-in-CI stretch goal was not attempted.
