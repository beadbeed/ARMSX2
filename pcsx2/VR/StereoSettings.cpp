// SPDX-FileCopyrightText: 2026 Outbreak VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "VR/StereoSettings.h"
#include "VR/StereoState.h"

#include "common/Console.h"
#include "common/SettingsInterface.h"

#include <atomic>

namespace VR::StereoSettings
{
	namespace
	{
		std::atomic_bool s_interleave{false};
	}

	void Apply(const SettingsInterface& si)
	{
		StereoState::Params p;
		p.enabled = si.GetBoolValue("VR", "StereoEnable", false);
		p.separation = si.GetFloatValue("VR", "Separation", 0.01f);
		p.convergence = si.GetFloatValue("VR", "Convergence", 6.0f);
		p.z_driven_depth = si.GetBoolValue("VR", "ZDrivenDepth", true);
		p.pin_uniform_q = si.GetBoolValue("VR", "PinUniformQ", false);
		p.collimate_disparity = si.GetFloatValue("VR", "CollimateDisparity", 0.0f);
		StereoState::Publish(p);

		const bool interleave = p.enabled && si.GetBoolValue("VR", "InterleaveEyes", false);
		if (interleave != s_interleave.exchange(interleave) || p.enabled)
		{
			Console.WriteLn("(VR) Stereo %s: separation %.4f, convergence %.4g, z-driven %s%s", p.enabled ? "on" : "off",
				p.separation, p.convergence, p.z_driven_depth ? "yes" : "no", interleave ? ", interleaved eyes" : "");
		}
	}

	bool InterleaveEyes()
	{
		return s_interleave.load(std::memory_order_relaxed);
	}
}
