// SPDX-FileCopyrightText: 2026 Outbreak VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "VR/StereoSettings.h"
#include "VR/StereoState.h"

#include "common/Console.h"
#include "common/SettingsInterface.h"

#include <atomic>
#include <string>

namespace VR::StereoSettings
{
	namespace
	{
		std::atomic_bool s_interleave{false};
		std::atomic_bool s_per_eye{false};
		std::atomic<DebugView> s_debug_view{DebugView::Off};

		DebugView ParseDebugView(const std::string& v)
		{
			if (v == "SBS")
				return DebugView::SBS;
			if (v == "CrossEye")
				return DebugView::CrossEye;
			if (v == "Left")
				return DebugView::Left;
			if (v == "Right")
				return DebugView::Right;
			return DebugView::Off;
		}
	}

	void Apply(const SettingsInterface& si)
	{
#ifdef ENABLE_OPENXR
		// Outbreak VR (Steam Frame): the OpenXR VR layer owns VR::StereoState (VR::UpdateSettings and
		// VR::ApplySceneStereo publish it from [VR] Enable/StereoMode and the game's profile), so this
		// file publishes nothing. DebugView and InterleaveEyes stay as debug options; the stereo keys
		// below (StereoEnable, Separation, Convergence, ...) are not read.
		s_debug_view.store(ParseDebugView(si.GetStringValue("VR", "DebugView", "Off")), std::memory_order_relaxed);
		const bool xr_interleave = si.GetBoolValue("VR", "InterleaveEyes", false);
		const bool xr_stereo_keys = si.GetBoolValue("VR", "StereoEnable", false);
		s_per_eye.store(false, std::memory_order_relaxed);
		static bool s_logged = false;
		if (xr_interleave != s_interleave.exchange(xr_interleave) || !s_logged)
		{
			s_logged = true;
			Console.WriteLn("(VR) StereoSettings: the OpenXR VR layer owns the stereo state; debug view %s, interleaved eyes %s%s.",
				si.GetStringValue("VR", "DebugView", "Off").c_str(), xr_interleave ? "on" : "off",
				xr_stereo_keys ? " ([VR] StereoEnable is ignored here: use [VR] StereoMode)" : "");
		}
		return;
#endif
		StereoState::Params p;
		p.enabled = si.GetBoolValue("VR", "StereoEnable", false);
		p.separation = si.GetFloatValue("VR", "Separation", 0.01f);
		p.convergence = si.GetFloatValue("VR", "Convergence", 6.0f);
		p.z_driven_depth = si.GetBoolValue("VR", "ZDrivenDepth", true);
		p.pin_uniform_q = si.GetBoolValue("VR", "PinUniformQ", false);
		p.collimate_disparity = si.GetFloatValue("VR", "CollimateDisparity", 0.0f);
		StereoState::Publish(p);

		s_debug_view.store(ParseDebugView(si.GetStringValue("VR", "DebugView", "Off")), std::memory_order_relaxed);

		const bool interleave = p.enabled && si.GetBoolValue("VR", "InterleaveEyes", false);
		s_per_eye.store(p.enabled && !interleave, std::memory_order_relaxed);
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

	bool PerEyeTargets()
	{
		return s_per_eye.load(std::memory_order_relaxed);
	}

	DebugView GetDebugView()
	{
		return s_debug_view.load(std::memory_order_relaxed);
	}
}
