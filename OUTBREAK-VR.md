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
the SteamVR questions the rest of M0 depends on. The VR layer itself is not in this branch yet.

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
