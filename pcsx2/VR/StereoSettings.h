// SPDX-FileCopyrightText: 2026 Outbreak VR contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

class SettingsInterface;

// Loads the [VR] stereo settings and publishes them to VR::StereoState, which the
// hardware renderer reads per draw. This is the slim stand-in for PenguinScreen2's
// VRManager + VRProfileDB until those are ported: global settings only, no
// per-game profiles or scene rules yet.
//
//   [VR]
//   StereoEnable = true         master switch (default off)
//   Separation = 0.01           per-eye shift at infinity, in clip-space x units
//   Convergence = 6.0           1/w at which disparity reaches zero (game units)
//   ZDrivenDepth = true         use the per-frame Z->1/w fit for draws without Q
//   PinUniformQ = false         keep draws with one Q for every vertex at screen depth
//   CollimateDisparity = 0.0    fixed offset for screen-space (FST) draws, e.g. HUD
//   InterleaveEyes = false      no multiview target: alternate eyes every frame
//   DebugView = Off             with per-eye targets on a flat screen: SBS, CrossEye, Left, Right
namespace VR::StereoSettings
{
	enum class DebugView : u8
	{
		Off,      // show the left eye (what a mono screen would show)
		SBS,      // left | right, for parallel viewing and VR video players
		CrossEye, // right | left
		Left,
		Right,
	};

	void Apply(const SettingsInterface& si);

	// True when stereo renders into ordinary single-layer targets, one eye per
	// frame. This is the first-milestone testbed (and works on any Vulkan
	// device); per-eye layered targets replace it in the next milestone.
	bool InterleaveEyes();

	// True when stereo renders both eyes every frame into per-eye (two-layer) targets: the
	// texture cache gives the display chain a layer per eye and draws into it use multiview.
	bool PerEyeTargets();

	// How a flat window (or a frame dump) shows a per-eye frame.
	DebugView GetDebugView();
}
