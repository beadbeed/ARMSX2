# Where pcsx2/VR comes from

The OpenXR VR layer in this folder is copied from **PenguinScreen2** (https://github.com/beadbeed/PenguinScreen2),
branch `pc-firstperson`, commit **c2c5ee249e66f2552894b20c465f2fa7ef9f41ef** (2026-10-12). It is built only when
`ENABLE_OPENXR` is on, which only `.github/workflows/outbreak-vr.yml` does (the Steam Frame build). Every other
ARMSX2 build compiles none of it and is unchanged.

The files were copied whole, not merged: PenguinScreen2 commit 990b456 stripped comments from 938 files, so a git
merge between the two trees is noise. Fixes made here for clang 17 / libstdc++ on Linux arm64 are listed below so
they can be ported back to PenguinScreen2.

Status: work in progress (second slice of M0). See the end of this file.
