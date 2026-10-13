#!/usr/bin/env bash
# Outbreak VR: Monado, the open source OpenXR runtime, for the headless OpenXR test (jobs monado and
# frame-monado in outbreak-vr.yml). Built from a pinned release, in-process (no monado-service: the runtime
# library runs inside the app), with the simulated HMD driver and the null compositor, which needs no display
# and takes the frames without showing them. frame-monado.sh runs frame-xrprobe and ARMSX2 against it, on
# Mesa's lavapipe (Vulkan on the CPU).
#
# Usage: build-monado.sh deps             install what Monado builds and runs with (run as root)
#        build-monado.sh build <prefix>   clone the pinned release, build it and install it into <prefix>
#
# The workflow caches <prefix> under a key made from this file's hash, so a change here builds Monado again.

set -euo pipefail

MONADO_TAG=v25.1.0
MONADO_COMMIT=3b450e37b624f5edea366492ee2d267d673a35aa
MONADO_URL=https://gitlab.freedesktop.org/monado/monado.git

# Monado's own Ubuntu 24.04 CI list, less the optional packages for drivers and tools this test does not use
# (OpenCV, SDL2, GStreamer, Bluetooth, libuvc, systemd). The frame-monado job installs the same list, so the
# libraries the cached build links to are there when it runs.
DEPS=(build-essential cmake ninja-build pkg-config git ca-certificates python3 glslang-tools libeigen3-dev
	libvulkan-dev libvulkan1 mesa-vulkan-drivers vulkan-tools libudev-dev libusb-1.0-0-dev libhidapi-dev libbsd-dev
	libcjson-dev libx11-dev libx11-xcb-dev libxcb-randr0-dev libxrandr-dev libxxf86vm-dev libgl1-mesa-dev
	libglvnd-dev libegl1-mesa-dev)

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
retry=$here/../../../../tools/retry.sh

case "${1:-}" in
	deps)
		"$retry" apt-get -o Acquire::ForceIPv4=true update
		"$retry" apt-get -y install "${DEPS[@]}"
		;;
	build)
		prefix=${2:?usage: build-monado.sh build <prefix>}
		mkdir -p "$prefix"
		prefix=$(cd "$prefix" && pwd)
		work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/monado.XXXXXX")
		"$retry" git clone --quiet --depth 1 --branch "$MONADO_TAG" "$MONADO_URL" "$work/src"
		got=$(git -C "$work/src" rev-parse HEAD)
		if [ "$got" != "$MONADO_COMMIT" ]; then
			echo "::error::Monado $MONADO_TAG is commit $got, expected $MONADO_COMMIT (the tag moved?)"
			exit 1
		fi
		echo "Monado $MONADO_TAG at $got"
		# XRT_FEATURE_SERVICE=OFF: the in-process runtime (Monado's CI builds it as "cmake-no-service").
		# XRT_MODULE_COMPOSITOR_NULL: the compositor frame-monado.sh selects with XRT_COMPOSITOR_NULL=1.
		cmake -S "$work/src" -B "$work/build" -G Ninja \
			-DCMAKE_BUILD_TYPE=RelWithDebInfo \
			-DCMAKE_INSTALL_PREFIX="$prefix" \
			-DGIT_DESC="$MONADO_TAG" \
			-DBUILD_TESTING=OFF \
			-DXRT_FEATURE_SERVICE=OFF \
			-DXRT_MODULE_COMPOSITOR_NULL=ON \
			-DXRT_BUILD_DRIVER_SIMULATED=ON \
			-DXRT_FEATURE_STEAMVR_PLUGIN=OFF \
			-DXRT_HAVE_SDL2=OFF \
			-DXRT_HAVE_OPENCV=OFF \
			-DXRT_HAVE_GST=OFF \
			-DXRT_HAVE_SYSTEMD=OFF
		cmake --build "$work/build"
		cmake --install "$work/build"
		manifest=$(find "$prefix" -name 'openxr_monado.json' | head -n 1)
		if [ -z "$manifest" ]; then
			echo "::error::the Monado install in $prefix has no openxr_monado.json"
			exit 1
		fi
		echo "$MONADO_TAG $MONADO_COMMIT" >"$prefix/monado-version.txt"
		echo "Runtime manifest: $manifest"
		cat "$manifest"
		rm -rf "$work"
		;;
	*)
		echo "usage: build-monado.sh deps | build <prefix>" >&2
		exit 2
		;;
esac
