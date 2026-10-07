// SPDX-FileCopyrightText: 2026 Outbreak VR contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

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
namespace VR::StereoSettings
{
	void Apply(const SettingsInterface& si);

	// True when stereo renders into ordinary single-layer targets, one eye per
	// frame. This is the first-milestone testbed (and works on any Vulkan
	// device); per-eye layered targets replace it in the next milestone.
	bool InterleaveEyes();
}
