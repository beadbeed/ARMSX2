# Outbreak VR branch

This branch is part of **Outbreak VR**: Resident Evil Outbreak (PS2, Japanese File #1 / #2
with the English patch) as a true-3D VR game for the Valve Steam Frame.

It is one of two builds that share the same VR ideas:

- **ARMSX2, branch `outbreak-vr`** (this repo): Steam Frame standalone (Linux arm64 AppImage built by GitHub Actions); per-eye stereo is done, the headset session is not
- **PenguinScreen2, branch `windows-steamvr`** (https://github.com/beadbeed/PenguinScreen2/tree/windows-steamvr): Windows PC build streamed to the Frame through SteamVR; stereo, controllers and gesture zones

Outbreak-specific VR settings are in `the `[VR]` ini section (pcsx2/VR/StereoSettings.cpp)`.
This repo holds code only. No game files, BIOS or saves are ever committed here.
