// SPDX-FileCopyrightText: 2026 Outbreak VR contributors
// SPDX-License-Identifier: GPL-3.0+

// frame-xrprobe: one run on the Steam Frame answers the OpenXR and Vulkan questions the Outbreak VR
// port depends on (milestone M0 in PenguinScreen2's tools/outbreak-vr/STEAM-FRAME.md), before the
// port itself exists. It needs no game files.
//
// Usage: frame-xrprobe [--no-session] [--seconds-per-phase N] [--log-dir DIR]
//
// Sections, in order:
//   Host     kernel, page size, glibc, CPUs, container, environment
//   OpenXR   runtime manifests, API layers, extensions, instance, system, views
//   Vulkan   through XR_KHR_vulkan_enable2, the same path PenguinScreen2's XRSession.cpp takes, so the
//            answers hold for the real VR layer: driver, queue families, multiview, a second queue
//   Session  reference spaces, swapchain formats, refresh rates, controller profiles
//   Phases   solid colours the wearer reports back, each shown for N seconds:
//              A  one projection layer, one swapchain with arraySize 2: left RED, right BLUE
//              B  one projection layer, two swapchains: left GREEN, right MAGENTA
//              C  black projection plus a 1.6 x 0.9 m quad 2 m ahead: YELLOW
//              D  (cylinder extension only) the same as a cylinder: CYAN
//
// The log goes to DIR/probe-YYYYmmdd-HHMMSS.log (default DIR: $HOME/frame-probe) and to stderr: the
// Frame keeps no stdout, and stderr is still useful over SSH.
//
// Exit codes: 0 when the probe ran to its end, including stopping early because something is not there
// (no runtime, no headset system, no Vulkan binding); 1 when a call that should have worked failed or the
// session never ran its phases; 2 for bad arguments.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_reflection.h>

#include <dirent.h>
#include <dlfcn.h>
#include <gnu/libc-version.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef FRAME_XRPROBE_VERSION
#define FRAME_XRPROBE_VERSION "unknown"
#endif

extern char** environ;

namespace
{
	// ------------------------------------------------------------------------------------------------
	// Options and logging
	// ------------------------------------------------------------------------------------------------

	struct Options
	{
		bool no_session = false;
		int seconds_per_phase = 8;
		std::string log_dir;
	};
	Options s_opt;

	FILE* s_log = nullptr;
	int s_log_fd = -1;
	std::string s_log_path;
	std::mutex s_log_mutex;
	const std::chrono::steady_clock::time_point s_start = std::chrono::steady_clock::now();

	int64_t NowUs()
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - s_start).count();
	}

	int64_t NowMs()
	{
		return NowUs() / 1000;
	}

	void LogLine(const char* line)
	{
		const int64_t ms = NowMs();
		char stamp[32];
		std::snprintf(stamp, sizeof(stamp), "[%5lld.%03lld] ", static_cast<long long>(ms / 1000), static_cast<long long>(ms % 1000));

		std::lock_guard<std::mutex> lock(s_log_mutex);
		FILE* const outs[2] = {s_log, stderr};
		for (FILE* f : outs)
		{
			if (!f)
				continue;
			std::fputs(stamp, f);
			std::fputs(line, f);
			std::fputc('\n', f);
			// Flushed per line: when the runtime or the driver kills the process, the log still ends where it stopped.
			std::fflush(f);
		}
	}

	__attribute__((format(printf, 1, 2))) void Log(const char* fmt, ...)
	{
		char line[4096];
		va_list ap;
		va_start(ap, fmt);
		std::vsnprintf(line, sizeof(line), fmt, ap);
		va_end(ap);
		LogLine(line);
	}

	__attribute__((format(printf, 1, 2))) std::string Format(const char* fmt, ...)
	{
		char buf[1024];
		va_list ap;
		va_start(ap, fmt);
		std::vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		return buf;
	}

	void Section(const char* name)
	{
		Log("");
		Log("==== %s ====", name);
	}

	// ------------------------------------------------------------------------------------------------
	// Watchdog and signals
	// ------------------------------------------------------------------------------------------------

	// The blocking call in progress, for the watchdog and the crash line.
	std::atomic<const char*> s_step{nullptr};
	std::atomic<int64_t> s_step_since{0};
	std::atomic_bool s_watchdog_quit{false};
	volatile sig_atomic_t s_quit_signal = 0;

	class ScopedStep
	{
	public:
		explicit ScopedStep(const char* what)
		{
			s_step_since.store(NowMs());
			s_step.store(what);
		}
		~ScopedStep() { s_step.store(nullptr); }
		ScopedStep(const ScopedStep&) = delete;
		ScopedStep& operator=(const ScopedStep&) = delete;
	};

	// Says "still waiting" every 10 s while one call blocks, so a hang (for example the FDM injection layer
	// stalling vkCreateDevice, SteamVR-for-Linux #972) reads differently in the log from a crash.
	void WatchdogMain()
	{
		int64_t reported_since = -1;
		int64_t reported_bucket = 0;
		while (!s_watchdog_quit.load())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			const char* const step = s_step.load();
			if (!step)
				continue;
			const int64_t since = s_step_since.load();
			const int64_t bucket = (NowMs() - since) / 10000;
			if (bucket < 1 || (since == reported_since && bucket == reported_bucket))
				continue;
			reported_since = since;
			reported_bucket = bucket;
			Log("STILL WAITING: %s has not returned after %lld s", step, static_cast<long long>(bucket * 10));
		}
	}

	void AppendRaw(char* buf, size_t cap, size_t& len, const char* s)
	{
		while (*s && len + 1 < cap)
			buf[len++] = *s++;
		buf[len] = '\0';
	}

	// Async-signal-safe only (write, no stdio, no allocation): a crash inside the runtime or the driver still
	// leaves one line saying what the probe was doing. SIGABRT included, because SteamVR's asserts abort
	// (SteamVR-for-Linux #989).
	void OnFatalSignal(int sig)
	{
		char buf[320];
		size_t len = 0;
		buf[0] = '\0';
		char num[8] = {};
		int n = sig;
		int i = 6;
		do
		{
			num[i--] = static_cast<char>('0' + n % 10);
			n /= 10;
		} while (n > 0 && i >= 0);
		AppendRaw(buf, sizeof(buf), len, "CRASH: signal ");
		AppendRaw(buf, sizeof(buf), len, &num[i + 1]);
		const char* const step = s_step.load();
		AppendRaw(buf, sizeof(buf), len, step ? " during " : " (between the probe's own calls)");
		if (step)
			AppendRaw(buf, sizeof(buf), len, step);
		AppendRaw(buf, sizeof(buf), len, "\n");
		if (s_log_fd >= 0)
			(void)!write(s_log_fd, buf, len);
		(void)!write(STDERR_FILENO, buf, len);
		raise(sig); // SA_RESETHAND restored the default action, so this ends the process as the signal would have.
	}

	void OnQuitSignal(int)
	{
		s_quit_signal = 1;
	}

	void InstallSignalHandlers()
	{
		struct sigaction fatal = {};
		fatal.sa_handler = OnFatalSignal;
		fatal.sa_flags = SA_RESETHAND | SA_NODEFER;
		for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
			sigaction(sig, &fatal, nullptr);

		// Steam's "stop" and Ctrl+C over SSH end the session cleanly instead of killing it mid-frame.
		struct sigaction quit = {};
		quit.sa_handler = OnQuitSignal;
		quit.sa_flags = SA_RESTART;
		for (int sig : {SIGINT, SIGTERM, SIGHUP})
			sigaction(sig, &quit, nullptr);
	}

	// ------------------------------------------------------------------------------------------------
	// Small helpers
	// ------------------------------------------------------------------------------------------------

	bool PathExists(const std::string& path)
	{
		struct stat st;
		return stat(path.c_str(), &st) == 0;
	}

	std::string ReadFile(const std::string& path, size_t max_bytes, bool* truncated = nullptr)
	{
		std::string out;
		if (truncated)
			*truncated = false;
		FILE* f = std::fopen(path.c_str(), "rb");
		if (!f)
			return out;
		char buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		{
			if (out.size() + n > max_bytes)
			{
				out.append(buf, max_bytes - out.size());
				if (truncated)
					*truncated = true;
				break;
			}
			out.append(buf, n);
		}
		std::fclose(f);
		return out;
	}

	std::string Trim(const std::string& s)
	{
		size_t b = 0;
		size_t e = s.size();
		while (b < e && (std::isspace(static_cast<unsigned char>(s[b])) || s[b] == '\0'))
			b++;
		while (e > b && (std::isspace(static_cast<unsigned char>(s[e - 1])) || s[e - 1] == '\0'))
			e--;
		return s.substr(b, e - b);
	}

	std::vector<std::string> Split(const std::string& s, char sep)
	{
		std::vector<std::string> out;
		size_t start = 0;
		while (start <= s.size())
		{
			const size_t end = s.find(sep, start);
			const std::string part = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
			if (!part.empty())
				out.push_back(part);
			if (end == std::string::npos)
				break;
			start = end + 1;
		}
		return out;
	}

	std::string EnvOr(const char* name, const char* fallback)
	{
		const char* v = std::getenv(name);
		return (v && *v) ? std::string(v) : std::string(fallback);
	}

	std::string DirName(const std::string& path)
	{
		const size_t slash = path.rfind('/');
		return slash == std::string::npos ? std::string(".") : (slash == 0 ? std::string("/") : path.substr(0, slash));
	}

	bool MakeDirs(const std::string& path)
	{
		std::string partial;
		for (const std::string& part : Split(path, '/'))
		{
			partial += "/" + part;
			if (mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST)
				return false;
		}
		return PathExists(path);
	}

	void LogFileContents(const std::string& text, const char* indent)
	{
		for (const std::string& line : Split(text, '\n'))
			Log("%s%s", indent, line.c_str());
	}

	// Enough for runtime and layer manifests; not a JSON parser.
	std::string JsonStringField(const std::string& json, const char* key)
	{
		const std::string needle = std::string("\"") + key + "\"";
		size_t p = json.find(needle);
		if (p == std::string::npos)
			return {};
		p = json.find(':', p + needle.size());
		if (p == std::string::npos)
			return {};
		p = json.find('"', p + 1);
		if (p == std::string::npos)
			return {};
		std::string out;
		for (size_t i = p + 1; i < json.size() && json[i] != '"'; i++)
		{
			if (json[i] == '\\' && i + 1 < json.size())
				i++;
			out += json[i];
		}
		return out;
	}

	std::vector<std::string> ListJsonFiles(const std::string& dir)
	{
		std::vector<std::string> out;
		DIR* d = opendir(dir.c_str());
		if (!d)
			return out;
		while (const dirent* e = readdir(d))
		{
			const std::string name = e->d_name;
			if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0)
				out.push_back(dir + "/" + name);
		}
		closedir(d);
		std::sort(out.begin(), out.end());
		return out;
	}

	void AddUnique(std::vector<std::string>& list, const std::string& item)
	{
		if (!item.empty() && std::find(list.begin(), list.end(), item) == list.end())
			list.push_back(item);
	}

	struct RunningStat
	{
		uint64_t n = 0;
		double sum = 0.0;
		double min = 0.0;
		double max = 0.0;

		void Add(double v)
		{
			if (n == 0 || v < min)
				min = v;
			if (n == 0 || v > max)
				max = v;
			sum += v;
			n++;
		}
		double Avg() const { return n ? sum / static_cast<double>(n) : 0.0; }
	};

	// ------------------------------------------------------------------------------------------------
	// Enum names
	// ------------------------------------------------------------------------------------------------

#define PROBE_ENUM_CASE(name, value) \
	case name: \
		return #name;

	const char* XrResultName(XrResult r)
	{
		switch (r)
		{
			XR_LIST_ENUM_XrResult(PROBE_ENUM_CASE)
			default:
				return "XR_RESULT_UNKNOWN";
		}
	}

	const char* ReferenceSpaceName(XrReferenceSpaceType t)
	{
		switch (t)
		{
			XR_LIST_ENUM_XrReferenceSpaceType(PROBE_ENUM_CASE)
			default:
				return "XR_REFERENCE_SPACE_TYPE_UNKNOWN";
		}
	}

	const char* ViewConfigurationName(XrViewConfigurationType t)
	{
		switch (t)
		{
			XR_LIST_ENUM_XrViewConfigurationType(PROBE_ENUM_CASE)
			default:
				return "XR_VIEW_CONFIGURATION_TYPE_UNKNOWN";
		}
	}

	const char* BlendModeName(XrEnvironmentBlendMode m)
	{
		switch (m)
		{
			XR_LIST_ENUM_XrEnvironmentBlendMode(PROBE_ENUM_CASE)
			default:
				return "XR_ENVIRONMENT_BLEND_MODE_UNKNOWN";
		}
	}

#undef PROBE_ENUM_CASE

	const char* SessionStateName(XrSessionState state)
	{
		switch (state)
		{
			case XR_SESSION_STATE_IDLE:         return "IDLE";
			case XR_SESSION_STATE_READY:        return "READY";
			case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
			case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
			case XR_SESSION_STATE_FOCUSED:      return "FOCUSED";
			case XR_SESSION_STATE_STOPPING:     return "STOPPING";
			case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
			case XR_SESSION_STATE_EXITING:      return "EXITING";
			default:                            return "UNKNOWN";
		}
	}

	const char* VkResultName(VkResult r)
	{
		switch (r)
		{
			case VK_SUCCESS:                        return "VK_SUCCESS";
			case VK_NOT_READY:                      return "VK_NOT_READY";
			case VK_TIMEOUT:                        return "VK_TIMEOUT";
			case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
			case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
			case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
			case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
			case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
			case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
			case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
			case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
			case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
			case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
			case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
			default:                                return "VK_RESULT_OTHER";
		}
	}

	const char* VkFormatName(int64_t f)
	{
		switch (f)
		{
			case VK_FORMAT_R5G6B5_UNORM_PACK16:      return "R5G6B5_UNORM_PACK16";
			case VK_FORMAT_R8G8B8_UNORM:             return "R8G8B8_UNORM";
			case VK_FORMAT_R8G8B8_SRGB:              return "R8G8B8_SRGB";
			case VK_FORMAT_R8G8B8A8_UNORM:           return "R8G8B8A8_UNORM";
			case VK_FORMAT_R8G8B8A8_SRGB:            return "R8G8B8A8_SRGB";
			case VK_FORMAT_B8G8R8A8_UNORM:           return "B8G8R8A8_UNORM";
			case VK_FORMAT_B8G8R8A8_SRGB:            return "B8G8R8A8_SRGB";
			case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10_UNORM_PACK32";
			case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM_PACK32";
			case VK_FORMAT_R16G16B16A16_UNORM:       return "R16G16B16A16_UNORM";
			case VK_FORMAT_R16G16B16A16_SFLOAT:      return "R16G16B16A16_SFLOAT";
			case VK_FORMAT_R32G32B32A32_SFLOAT:      return "R32G32B32A32_SFLOAT";
			case VK_FORMAT_B10G11R11_UFLOAT_PACK32:  return "B10G11R11_UFLOAT_PACK32";
			case VK_FORMAT_D16_UNORM:                return "D16_UNORM";
			case VK_FORMAT_X8_D24_UNORM_PACK32:      return "X8_D24_UNORM_PACK32";
			case VK_FORMAT_D32_SFLOAT:               return "D32_SFLOAT";
			case VK_FORMAT_D24_UNORM_S8_UINT:        return "D24_UNORM_S8_UINT";
			case VK_FORMAT_D32_SFLOAT_S8_UINT:       return "D32_SFLOAT_S8_UINT";
			default:                                 return "other";
		}
	}

	const char* DriverIdName(VkDriverId id)
	{
		switch (id)
		{
			case VK_DRIVER_ID_MESA_TURNIP:            return "MESA_TURNIP";
			case VK_DRIVER_ID_QUALCOMM_PROPRIETARY:   return "QUALCOMM_PROPRIETARY";
			case VK_DRIVER_ID_MESA_LLVMPIPE:          return "MESA_LLVMPIPE";
			case VK_DRIVER_ID_MESA_RADV:              return "MESA_RADV";
			case VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA: return "INTEL_OPEN_SOURCE_MESA";
			case VK_DRIVER_ID_NVIDIA_PROPRIETARY:     return "NVIDIA_PROPRIETARY";
			case VK_DRIVER_ID_ARM_PROPRIETARY:        return "ARM_PROPRIETARY";
			case VK_DRIVER_ID_MESA_PANVK:             return "MESA_PANVK";
			case VK_DRIVER_ID_MESA_VENUS:             return "MESA_VENUS";
			default:                                  return "other";
		}
	}

	const char* DeviceTypeName(VkPhysicalDeviceType t)
	{
		switch (t)
		{
			case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
			case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete GPU";
			case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual GPU";
			case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU (software)";
			default:                                     return "other";
		}
	}

	std::string QueueFlagsString(VkQueueFlags f)
	{
		std::string s;
		const struct
		{
			VkQueueFlags bit;
			const char* name;
		} names[] = {
			{VK_QUEUE_GRAPHICS_BIT, "GRAPHICS"},
			{VK_QUEUE_COMPUTE_BIT, "COMPUTE"},
			{VK_QUEUE_TRANSFER_BIT, "TRANSFER"},
			{VK_QUEUE_SPARSE_BINDING_BIT, "SPARSE_BINDING"},
			{VK_QUEUE_PROTECTED_BIT, "PROTECTED"},
			{VK_QUEUE_VIDEO_DECODE_BIT_KHR, "VIDEO_DECODE"},
			{VK_QUEUE_VIDEO_ENCODE_BIT_KHR, "VIDEO_ENCODE"},
		};
		for (const auto& n : names)
		{
			if (f & n.bit)
				s += s.empty() ? n.name : std::string("|") + n.name;
		}
		return s.empty() ? std::string("none") : s;
	}

	std::string VersionString(uint32_t v)
	{
		return Format("%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
	}

	// ------------------------------------------------------------------------------------------------
	// Host
	// ------------------------------------------------------------------------------------------------

	const char* ArmPartName(unsigned implementer, unsigned part)
	{
		if (implementer == 0x51 && part == 0x001)
			return "Oryon";
		if (implementer != 0x41)
			return nullptr;
		switch (part)
		{
			case 0xd03: return "Cortex-A53";
			case 0xd05: return "Cortex-A55";
			case 0xd0b: return "Cortex-A76";
			case 0xd0c: return "Neoverse-N1";
			case 0xd0d: return "Cortex-A77";
			case 0xd40: return "Neoverse-V1";
			case 0xd41: return "Cortex-A78";
			case 0xd44: return "Cortex-X1";
			case 0xd46: return "Cortex-A510";
			case 0xd47: return "Cortex-A710";
			case 0xd48: return "Cortex-X2";
			case 0xd49: return "Neoverse-N2";
			case 0xd4d: return "Cortex-A715";
			case 0xd4e: return "Cortex-X3";
			case 0xd4f: return "Neoverse-V2";
			case 0xd80: return "Cortex-A520";
			case 0xd81: return "Cortex-A720";
			case 0xd82: return "Cortex-X4";
			case 0xd85: return "Cortex-X925";
			case 0xd87: return "Cortex-A725";
			default:    return nullptr;
		}
	}

	void ReportCpus()
	{
		// arm64 kernels list "CPU implementer" / "CPU part" per processor and no model name; x86 lists "model name".
		const std::string info = ReadFile("/proc/cpuinfo", 1 << 20);
		std::vector<std::pair<std::string, int>> models;
		std::string hardware;
		std::string model_name;
		unsigned implementer = 0;
		unsigned part = 0;
		bool in_cpu = false;
		auto flush = [&]() {
			if (!in_cpu)
				return;
			std::string desc;
			if (!model_name.empty())
				desc = model_name;
			else
			{
				const char* known = ArmPartName(implementer, part);
				desc = Format("%s (implementer 0x%02x, part 0x%03x)", known ? known : "unknown core", implementer, part);
			}
			if (!models.empty() && models.back().first == desc)
				models.back().second++;
			else
				models.emplace_back(desc, 1);
			model_name.clear();
			implementer = part = 0;
			in_cpu = false;
		};
		for (const std::string& raw : Split(info, '\n'))
		{
			const size_t colon = raw.find(':');
			if (colon == std::string::npos)
				continue;
			const std::string key = Trim(raw.substr(0, colon));
			const std::string value = Trim(raw.substr(colon + 1));
			if (key == "processor")
			{
				flush();
				in_cpu = true;
			}
			else if (key == "model name")
				model_name = value;
			else if (key == "CPU implementer")
				implementer = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 0));
			else if (key == "CPU part")
				part = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 0));
			else if (key == "Hardware")
				hardware = value;
		}
		flush();

		for (const auto& m : models)
			Log("CPU: %dx %s", m.second, m.first.c_str());
		if (!hardware.empty())
			Log("CPU hardware: %s", hardware.c_str());

		std::string freqs;
		const long cpus = sysconf(_SC_NPROCESSORS_CONF);
		for (long i = 0; i < cpus && i < 64; i++)
		{
			const std::string khz = Trim(ReadFile(Format("/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", i), 64));
			freqs += khz.empty() ? std::string(" ?") : Format(" %ld", std::strtol(khz.c_str(), nullptr, 10) / 1000);
		}
		Log("CPU max MHz per core:%s", freqs.c_str());
	}

	bool InterestingEnv(const char* kv)
	{
		static const char* const prefixes[] = {
			"XDG_", "XR_", "STEAM", "DISABLE_", "ENABLE_", "VK_", "MESA_", "TU_", "PRESSURE_VESSEL",
			"WAYLAND_DISPLAY=", "DISPLAY=", "LD_LIBRARY_PATH=", "LD_PRELOAD=", "SDL_VIDEODRIVER=", "FRAME_",
		};
		for (const char* p : prefixes)
		{
			if (std::strncmp(kv, p, std::strlen(p)) == 0)
				return true;
		}
		return false;
	}

	void ReportHost()
	{
		Section("Host");

		struct utsname u;
		if (uname(&u) == 0)
			Log("uname: %s %s %s %s %s", u.sysname, u.nodename, u.release, u.version, u.machine);
		Log("Page size: %ld bytes", sysconf(_SC_PAGESIZE));
		Log("glibc: %s (%s)", gnu_get_libc_version(), gnu_get_libc_release());
		Log("CPUs: %ld online, %ld configured", sysconf(_SC_NPROCESSORS_ONLN), sysconf(_SC_NPROCESSORS_CONF));
		ReportCpus();

		const std::string mem = ReadFile("/proc/meminfo", 256);
		if (!mem.empty())
			Log("Memory: %s", Trim(mem.substr(0, mem.find('\n'))).c_str());
		const std::string model = Trim(ReadFile("/sys/firmware/devicetree/base/model", 256));
		if (!model.empty())
			Log("Device model: %s", model.c_str());

		for (const std::string& line : Split(ReadFile("/etc/os-release", 8192), '\n'))
		{
			for (const char* key : {"PRETTY_NAME=", "ID=", "VARIANT_ID=", "VERSION_ID=", "BUILD_ID="})
			{
				if (line.compare(0, std::strlen(key), key) == 0)
					Log("os-release: %s", line.c_str());
			}
		}

		// Valve documents the Sniper runtime for devkit titles; the community saw arm64 titles run on the host.
		// Which one happened decides the glibc the AppImage meets and whether /opt/steamvr is visible.
		if (PathExists("/run/host/container-manager"))
			Log("Container: %s (/run/host/container-manager)", Trim(ReadFile("/run/host/container-manager", 256)).c_str());
		else if (PathExists("/.flatpak-info"))
			Log("Container: flatpak (/.flatpak-info)");
		else if (PathExists("/run/host"))
			Log("Container: /run/host exists, so probably some container");
		else
			Log("Container: none found (no /run/host), so this runs on the host system");
		Log("/opt/steamvr: %s", PathExists("/opt/steamvr") ? "present" : "not visible from here");

		char exe[4096] = {};
		if (readlink("/proc/self/exe", exe, sizeof(exe) - 1) > 0)
			Log("Executable: %s", exe);
		Log("uid %u, gid %u", static_cast<unsigned>(getuid()), static_cast<unsigned>(getgid()));

		std::vector<std::string> vars;
		for (char** e = environ; e && *e; ++e)
		{
			if (InterestingEnv(*e))
				vars.emplace_back(*e);
		}
		std::sort(vars.begin(), vars.end());
		Log("Environment (%zu variables of interest):", vars.size());
		for (const std::string& v : vars)
			Log("  %s", v.c_str());
	}

	// ------------------------------------------------------------------------------------------------
	// OpenXR: manifests, layers, extensions, instance, system
	// ------------------------------------------------------------------------------------------------

#if defined(__aarch64__)
	constexpr const char* kArchAbi = "aarch64";
#elif defined(__x86_64__)
	constexpr const char* kArchAbi = "x86_64";
#else
	constexpr const char* kArchAbi = "unknown";
#endif

	// SteamVR only accepts bindings for its Steam Frame controller profile with this enabled. The header of
	// OpenXR SDK 1.1.61 has no macro for it, so it is spelled out (as PenguinScreen2's XRSession does).
	constexpr const char* kFrameControllerExtension = "XR_VALVE_frame_controller_interaction";

	XrInstance s_instance = XR_NULL_HANDLE;
	XrSystemId s_system = XR_NULL_SYSTEM_ID;
	XrViewConfigurationView s_views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	XrEnvironmentBlendMode s_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

	struct Extensions
	{
		bool vulkan2 = false;
		bool vulkan1 = false;
		bool cylinder = false;
		bool frame_controller = false;
		bool refresh_rate = false;
		bool depth = false;
		bool eye_gaze = false;
	};
	Extensions s_offered;
	Extensions s_enabled;

	PFN_xrGetVulkanGraphicsRequirements2KHR pfn_xrGetVulkanGraphicsRequirements2KHR = nullptr;
	PFN_xrCreateVulkanInstanceKHR pfn_xrCreateVulkanInstanceKHR = nullptr;
	PFN_xrGetVulkanGraphicsDevice2KHR pfn_xrGetVulkanGraphicsDevice2KHR = nullptr;
	PFN_xrCreateVulkanDeviceKHR pfn_xrCreateVulkanDeviceKHR = nullptr;
	PFN_xrEnumerateDisplayRefreshRatesFB pfn_xrEnumerateDisplayRefreshRatesFB = nullptr;
	PFN_xrGetDisplayRefreshRateFB pfn_xrGetDisplayRefreshRateFB = nullptr;

	enum class Outcome
	{
		Ok,   // carry on
		Stop, // something is not there: stop here, exit 0
		Fail, // something that should work failed: stop here, exit 1
	};

	bool CheckXR(XrResult r, const char* what)
	{
		if (XR_SUCCEEDED(r))
			return true;
		Log("FAIL: %s: %s (%d)", what, XrResultName(r), static_cast<int>(r));
		return false;
	}

	std::string PathString(XrPath path)
	{
		char buf[XR_MAX_PATH_LENGTH] = {};
		uint32_t n = 0;
		if (path == XR_NULL_PATH || XR_FAILED(xrPathToString(s_instance, path, sizeof(buf), &n, buf)))
			return "(none)";
		return buf;
	}

	void DescribeManifest(const std::string& path)
	{
		char target[4096] = {};
		if (readlink(path.c_str(), target, sizeof(target) - 1) > 0)
			Log("    symlink -> %s", target);
		char real[4096] = {};
		if (realpath(path.c_str(), real))
			Log("    resolves to %s", real);
		bool truncated = false;
		const std::string text = ReadFile(path, 8192, &truncated);
		LogFileContents(text, "    | ");
		if (truncated)
			Log("    | (cut at 8 KiB)");
		const std::string lib = JsonStringField(text, "library_path");
		if (!lib.empty())
		{
			// A relative library_path is relative to the manifest's own directory.
			const std::string full = lib[0] == '/' ? lib : DirName(path) + "/" + lib;
			Log("    library_path %s: %s", full.c_str(), PathExists(full) ? "exists" : "MISSING");
		}
	}

	void ReportRuntimeManifests()
	{
		// The same order as the vendored loader (src/loader/manifest_file.cpp): XR_RUNTIME_JSON, then
		// XDG_CONFIG_HOME (default ~/.config), each XDG_CONFIG_DIRS entry (default /etc/xdg), then /etc;
		// in each, the arch-specific file before the plain one.
		std::vector<std::string> candidates;
		if (const char* env = std::getenv("XR_RUNTIME_JSON"); env && *env)
			AddUnique(candidates, env);
		std::vector<std::string> prefixes;
		const std::string home = EnvOr("HOME", "");
		const std::string config_home = EnvOr("XDG_CONFIG_HOME", home.empty() ? "" : (home + "/.config").c_str());
		AddUnique(prefixes, config_home);
		for (const std::string& d : Split(EnvOr("XDG_CONFIG_DIRS", "/etc/xdg"), ':'))
			AddUnique(prefixes, d);
		AddUnique(prefixes, "/etc");
		for (const std::string& p : prefixes)
		{
			AddUnique(candidates, p + "/openxr/1/active_runtime." + kArchAbi + ".json");
			AddUnique(candidates, p + "/openxr/1/active_runtime.json");
		}

		Log("Runtime manifests, in the loader's search order (the first that exists is used):");
		bool found = false;
		for (const std::string& c : candidates)
		{
			if (!PathExists(c))
			{
				Log("  (missing) %s", c.c_str());
				continue;
			}
			Log("  (exists)  %s%s", c.c_str(), found ? "" : "   <- the loader uses this one");
			DescribeManifest(c);
			found = true;
		}

		// The default place, in case XDG_CONFIG_HOME points elsewhere and hides it from the loader.
		if (!home.empty())
		{
			const std::string def = home + "/.config/openxr/1/active_runtime.json";
			if (std::find(candidates.begin(), candidates.end(), def) == candidates.end() && PathExists(def))
			{
				Log("  (exists, but the loader does not look here because XDG_CONFIG_HOME is set) %s", def.c_str());
				DescribeManifest(def);
			}
		}
		if (!found)
			Log("No runtime manifest exists in any of those places.");
	}

	void ReportApiLayers()
	{
		// Same search directories as the loader. The implicit ones load into every OpenXR app; on the Frame
		// that includes Valve's FDM injection layer (#972), and its manifest names the variable that disables it.
		std::vector<std::string> bases;
		for (const std::string& d : Split(EnvOr("XDG_CONFIG_DIRS", "/etc/xdg"), ':'))
			AddUnique(bases, d);
		AddUnique(bases, "/etc");
		for (const std::string& d : Split(EnvOr("XDG_DATA_DIRS", "/usr/local/share:/usr/share"), ':'))
			AddUnique(bases, d);
		const std::string home = EnvOr("HOME", "");
		AddUnique(bases, EnvOr("XDG_DATA_HOME", home.empty() ? "" : (home + "/.local/share").c_str()));

		Log("API layer manifests:");
		int count = 0;
		for (const char* kind : {"implicit.d", "explicit.d"})
		{
			for (const std::string& base : bases)
			{
				for (const std::string& file : ListJsonFiles(base + "/openxr/1/api_layers/" + kind))
				{
					const std::string text = ReadFile(file, 16384);
					Log("  %s %s: name %s, library_path %s, disable_environment %s", kind, file.c_str(),
						JsonStringField(text, "name").c_str(), JsonStringField(text, "library_path").c_str(),
						JsonStringField(text, "disable_environment").c_str());
					count++;
				}
			}
		}
		if (count == 0)
			Log("  none found");

		uint32_t n = 0;
		XrResult r = xrEnumerateApiLayerProperties(0, &n, nullptr);
		if (XR_FAILED(r))
		{
			Log("xrEnumerateApiLayerProperties: %s (%d)", XrResultName(r), static_cast<int>(r));
			return;
		}
		std::vector<XrApiLayerProperties> props(n, {XR_TYPE_API_LAYER_PROPERTIES});
		if (n > 0)
			r = xrEnumerateApiLayerProperties(n, &n, props.data());
		Log("API layers the loader reports (explicit, enabled on request): %u", n);
		for (uint32_t i = 0; i < n && XR_SUCCEEDED(r); i++)
		{
			Log("  %s (spec %u.%u.%u, layer version %u): %s", props[i].layerName, XR_VERSION_MAJOR(props[i].specVersion),
				XR_VERSION_MINOR(props[i].specVersion), XR_VERSION_PATCH(props[i].specVersion), props[i].layerVersion,
				props[i].description);
		}
	}

	// Returns false when no runtime answers: the CI smoke test and a title run outside SteamVR end here.
	bool EnumerateExtensions()
	{
		uint32_t count = 0;
		XrResult r;
		{
			ScopedStep step("xrEnumerateInstanceExtensionProperties");
			r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
		}
		if (XR_FAILED(r))
		{
			Log("OpenXR runtime unavailable: xrEnumerateInstanceExtensionProperties returned %s (%d).", XrResultName(r),
				static_cast<int>(r));
			Log("(Either no runtime manifest above exists, or the runtime library failed to load. The launch log "
				"has the loader's own messages.)");
			return false;
		}
		std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
		r = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data());
		if (XR_FAILED(r))
		{
			Log("OpenXR runtime unavailable: listing its extensions failed with %s (%d).", XrResultName(r), static_cast<int>(r));
			return false;
		}

		Log("Instance extensions offered by the runtime: %u", count);
		for (uint32_t i = 0; i < count; i++)
		{
			const char* name = props[i].extensionName;
			bool* flag = nullptr;
			if (std::strcmp(name, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0)
				flag = &s_offered.vulkan2;
			else if (std::strcmp(name, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0)
				flag = &s_offered.vulkan1;
			else if (std::strcmp(name, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME) == 0)
				flag = &s_offered.cylinder;
			else if (std::strcmp(name, kFrameControllerExtension) == 0)
				flag = &s_offered.frame_controller;
			else if (std::strcmp(name, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0)
				flag = &s_offered.refresh_rate;
			else if (std::strcmp(name, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME) == 0)
				flag = &s_offered.depth;
			else if (std::strcmp(name, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0)
				flag = &s_offered.eye_gaze;
			if (flag)
				*flag = true;
			Log("  %s v%u%s", name, props[i].extensionVersion, (flag && flag != &s_offered.vulkan1) ? "   (the probe enables it)" : "");
		}

		Log("Answers: XR_KHR_vulkan_enable2 %s; cylinder layer %s; Frame controller profile %s; refresh rate control %s; "
			"depth layer %s; eye gaze %s.",
			s_offered.vulkan2 ? "YES" : "NO", s_offered.cylinder ? "YES" : "NO (the screen falls back to a quad)",
			s_offered.frame_controller ? "YES" : "NO", s_offered.refresh_rate ? "YES" : "NO", s_offered.depth ? "YES" : "NO",
			s_offered.eye_gaze ? "YES" : "NO");
		if (!s_offered.vulkan2)
		{
			Log("XR_KHR_vulkan_enable2 is NOT offered (XR_KHR_vulkan_enable %s). The VR layer needs a vulkan_enable "
				"fallback in XRSession before M0.",
				s_offered.vulkan1 ? "is" : "is not either");
		}
		return true;
	}

	Outcome CreateInstance()
	{
		std::vector<const char*> enabled;
		auto enable = [&](bool offered, const char* name, bool& flag) {
			if (!offered)
				return;
			enabled.push_back(name);
			flag = true;
		};
		enable(s_offered.vulkan2, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, s_enabled.vulkan2);
		enable(s_offered.cylinder, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME, s_enabled.cylinder);
		enable(s_offered.frame_controller, kFrameControllerExtension, s_enabled.frame_controller);
		enable(s_offered.refresh_rate, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, s_enabled.refresh_rate);
		enable(s_offered.depth, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, s_enabled.depth);
		enable(s_offered.eye_gaze, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME, s_enabled.eye_gaze);

		XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
		std::strncpy(ici.applicationInfo.applicationName, "frame-xrprobe", XR_MAX_APPLICATION_NAME_SIZE - 1);
		std::strncpy(ici.applicationInfo.engineName, "ARMSX2 Outbreak VR", XR_MAX_ENGINE_NAME_SIZE - 1);
		ici.applicationInfo.applicationVersion = 1;
		ici.applicationInfo.engineVersion = 1;
		// 1.0, as PenguinScreen2 asks: SteamVR's Android runtime is 1.0 only, and 1.0 is all the VR layer uses.
		ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
		ici.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
		ici.enabledExtensionNames = enabled.data();

		const int64_t t0 = NowMs();
		XrResult r;
		{
			ScopedStep step("xrCreateInstance");
			r = xrCreateInstance(&ici, &s_instance);
		}
		Log("xrCreateInstance (API 1.0, %zu extensions): %s, %lld ms", enabled.size(), XrResultName(r),
			static_cast<long long>(NowMs() - t0));
		if (XR_FAILED(r))
		{
			s_instance = XR_NULL_HANDLE;
			return Outcome::Fail;
		}

		XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
		if (CheckXR(xrGetInstanceProperties(s_instance, &ip), "xrGetInstanceProperties"))
		{
			Log("Runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
				XR_VERSION_PATCH(ip.runtimeVersion));
		}

#define PROBE_LOAD_XR_FN(name, wanted) \
	if (wanted && XR_FAILED(xrGetInstanceProcAddr(s_instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&pfn_##name)))) \
	{ \
		Log("FAIL: the runtime offers no " #name "."); \
		pfn_##name = nullptr; \
	}

		PROBE_LOAD_XR_FN(xrGetVulkanGraphicsRequirements2KHR, s_enabled.vulkan2)
		PROBE_LOAD_XR_FN(xrCreateVulkanInstanceKHR, s_enabled.vulkan2)
		PROBE_LOAD_XR_FN(xrGetVulkanGraphicsDevice2KHR, s_enabled.vulkan2)
		PROBE_LOAD_XR_FN(xrCreateVulkanDeviceKHR, s_enabled.vulkan2)
		PROBE_LOAD_XR_FN(xrEnumerateDisplayRefreshRatesFB, s_enabled.refresh_rate)
		PROBE_LOAD_XR_FN(xrGetDisplayRefreshRateFB, s_enabled.refresh_rate)
#undef PROBE_LOAD_XR_FN

		return Outcome::Ok;
	}

	Outcome ReportSystemAndViews()
	{
		XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
		sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		XrResult r;
		{
			ScopedStep step("xrGetSystem");
			r = xrGetSystem(s_instance, &sgi, &s_system);
		}
		if (XR_FAILED(r))
		{
			Log("No head-mounted system: xrGetSystem returned %s (%d). XR_ERROR_FORM_FACTOR_UNAVAILABLE usually means "
				"the headset is asleep or not ready.",
				XrResultName(r), static_cast<int>(r));
			return Outcome::Stop;
		}

		XrSystemEyeGazeInteractionPropertiesEXT eye = {XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
		XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
		if (s_enabled.eye_gaze)
			sp.next = &eye;
		if (CheckXR(xrGetSystemProperties(s_instance, s_system, &sp), "xrGetSystemProperties"))
		{
			Log("System: \"%s\" (vendorId 0x%x)", sp.systemName, sp.vendorId);
			Log("maxLayerCount: %u%s", sp.graphicsProperties.maxLayerCount,
				sp.graphicsProperties.maxLayerCount < 16 ? "   (below 16: lower the compositor's kLayerCapacity before M3)" : "");
			Log("Max swapchain size: %u x %u", sp.graphicsProperties.maxSwapchainImageWidth,
				sp.graphicsProperties.maxSwapchainImageHeight);
			Log("Tracking: orientation %s, position %s", sp.trackingProperties.orientationTracking ? "yes" : "no",
				sp.trackingProperties.positionTracking ? "yes" : "no");
			if (s_enabled.eye_gaze)
				Log("Eye gaze interaction supported: %s", eye.supportsEyeGazeInteraction ? "yes" : "no");
		}

		uint32_t n = 0;
		std::vector<XrViewConfigurationType> configs;
		if (XR_SUCCEEDED(xrEnumerateViewConfigurations(s_instance, s_system, 0, &n, nullptr)) && n > 0)
		{
			configs.resize(n);
			xrEnumerateViewConfigurations(s_instance, s_system, n, &n, configs.data());
		}
		std::string names;
		for (XrViewConfigurationType c : configs)
			names += std::string(" ") + ViewConfigurationName(c);
		Log("View configurations:%s", names.empty() ? " none" : names.c_str());
		if (std::find(configs.begin(), configs.end(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) == configs.end())
		{
			Log("No PRIMARY_STEREO view configuration, so there is nothing to render to.");
			return Outcome::Stop;
		}

		XrViewConfigurationProperties vcp = {XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
		if (CheckXR(xrGetViewConfigurationProperties(s_instance, s_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, &vcp),
				"xrGetViewConfigurationProperties"))
		{
			Log("PRIMARY_STEREO: fovMutable %s", vcp.fovMutable ? "yes" : "no");
		}

		n = 0;
		if (!CheckXR(xrEnumerateViewConfigurationViews(s_instance, s_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr),
				"xrEnumerateViewConfigurationViews") ||
			n != 2)
		{
			Log("FAIL: PRIMARY_STEREO should have 2 views, the runtime reports %u.", n);
			return Outcome::Fail;
		}
		if (!CheckXR(xrEnumerateViewConfigurationViews(s_instance, s_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, s_views),
				"xrEnumerateViewConfigurationViews"))
		{
			return Outcome::Fail;
		}
		for (uint32_t i = 0; i < 2; i++)
		{
			Log("View %u (%s eye): recommended %u x %u (%u samples), max %u x %u (%u samples)", i, i == 0 ? "left" : "right",
				s_views[i].recommendedImageRectWidth, s_views[i].recommendedImageRectHeight, s_views[i].recommendedSwapchainSampleCount,
				s_views[i].maxImageRectWidth, s_views[i].maxImageRectHeight, s_views[i].maxSwapchainSampleCount);
		}

		n = 0;
		std::vector<XrEnvironmentBlendMode> modes;
		if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(s_instance, s_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr)) && n > 0)
		{
			modes.resize(n);
			xrEnumerateEnvironmentBlendModes(s_instance, s_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, modes.data());
		}
		names.clear();
		for (XrEnvironmentBlendMode m : modes)
			names += std::string(" ") + BlendModeName(m);
		Log("Blend modes:%s", names.empty() ? " none" : names.c_str());
		if (!modes.empty())
			s_blend_mode = modes[0]; // the runtime's preferred mode comes first
		return Outcome::Ok;
	}

	// ------------------------------------------------------------------------------------------------
	// Vulkan
	// ------------------------------------------------------------------------------------------------

	// The Vulkan loader is opened with dlopen, as ARMSX2's VKLoader does on Linux, so the probe links no
	// libvulkan and still starts (and logs) on a system that has none.
	void* s_vk_lib = nullptr;
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

#define PROBE_VK_GLOBAL_FUNCS(X) \
	X(vkEnumerateInstanceVersion) \
	X(vkEnumerateInstanceLayerProperties) \
	X(vkCreateInstance)

#define PROBE_VK_INSTANCE_FUNCS(X) \
	X(vkDestroyInstance) \
	X(vkEnumeratePhysicalDevices) \
	X(vkGetPhysicalDeviceProperties) \
	X(vkGetPhysicalDeviceProperties2) \
	X(vkGetPhysicalDeviceFeatures2) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkGetPhysicalDeviceMemoryProperties) \
	X(vkEnumerateDeviceExtensionProperties) \
	X(vkGetDeviceProcAddr)

#define PROBE_VK_DEVICE_FUNCS(X) \
	X(vkDestroyDevice) \
	X(vkGetDeviceQueue) \
	X(vkDeviceWaitIdle) \
	X(vkCreateCommandPool) \
	X(vkDestroyCommandPool) \
	X(vkAllocateCommandBuffers) \
	X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) \
	X(vkResetCommandBuffer) \
	X(vkCmdPipelineBarrier) \
	X(vkCmdClearColorImage) \
	X(vkCmdBeginRenderPass) \
	X(vkCmdEndRenderPass) \
	X(vkQueueSubmit) \
	X(vkCreateFence) \
	X(vkDestroyFence) \
	X(vkWaitForFences) \
	X(vkResetFences) \
	X(vkCreateImageView) \
	X(vkDestroyImageView) \
	X(vkCreateRenderPass) \
	X(vkDestroyRenderPass) \
	X(vkCreateFramebuffer) \
	X(vkDestroyFramebuffer)

#define PROBE_DECLARE_VK_FN(name) PFN_##name name = nullptr;
	PROBE_VK_GLOBAL_FUNCS(PROBE_DECLARE_VK_FN)
	PROBE_VK_INSTANCE_FUNCS(PROBE_DECLARE_VK_FN)
	PROBE_VK_DEVICE_FUNCS(PROBE_DECLARE_VK_FN)
#undef PROBE_DECLARE_VK_FN

	VkInstance s_vk_instance = VK_NULL_HANDLE;
	VkPhysicalDevice s_vk_physical = VK_NULL_HANDLE;
	VkDevice s_vk_device = VK_NULL_HANDLE;
	VkQueue s_vk_queue = VK_NULL_HANDLE;
	uint32_t s_vk_graphics_family = 0;
	VkCommandPool s_vk_pool = VK_NULL_HANDLE;
	VkCommandBuffer s_vk_cmd = VK_NULL_HANDLE;
	VkFence s_vk_fence = VK_NULL_HANDLE;
	VkRenderPass s_vk_clear_pass = VK_NULL_HANDLE;

	struct DeviceFacts
	{
		uint32_t graphics_family = UINT32_MAX;
		uint32_t graphics_queue_count = 0;
		bool multiview = false;
	};

	bool LoadVulkanLibrary()
	{
		if (s_vk_lib)
			return true;
		for (const char* name : {"libvulkan.so.1", "libvulkan.so"})
		{
			s_vk_lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
			if (s_vk_lib)
				break;
			Log("dlopen(%s) failed: %s", name, dlerror());
		}
		if (!s_vk_lib)
		{
			Log("No Vulkan loader on this system.");
			return false;
		}
		vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(s_vk_lib, "vkGetInstanceProcAddr"));
		if (!vkGetInstanceProcAddr)
		{
			Log("FAIL: the Vulkan loader exports no vkGetInstanceProcAddr.");
			return false;
		}
		Dl_info info = {};
		if (dladdr(reinterpret_cast<void*>(vkGetInstanceProcAddr), &info) && info.dli_fname)
			Log("Vulkan loader: %s", info.dli_fname);

#define PROBE_LOAD_VK_GLOBAL(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(VK_NULL_HANDLE, #name));
		PROBE_VK_GLOBAL_FUNCS(PROBE_LOAD_VK_GLOBAL)
#undef PROBE_LOAD_VK_GLOBAL

		uint32_t version = VK_API_VERSION_1_0;
		if (vkEnumerateInstanceVersion)
			vkEnumerateInstanceVersion(&version);
		Log("Vulkan loader instance version: %s", VersionString(version).c_str());

		// Instance layers show what SteamOS injects into every Vulkan app (gamescope WSI, FDM injection, ...).
		uint32_t n = 0;
		if (vkEnumerateInstanceLayerProperties && vkEnumerateInstanceLayerProperties(&n, nullptr) == VK_SUCCESS)
		{
			std::vector<VkLayerProperties> layers(n);
			if (n > 0)
				vkEnumerateInstanceLayerProperties(&n, layers.data());
			Log("Vulkan instance layers: %u", n);
			for (uint32_t i = 0; i < n; i++)
				Log("  %s (spec %s): %s", layers[i].layerName, VersionString(layers[i].specVersion).c_str(), layers[i].description);
		}
		return vkCreateInstance != nullptr;
	}

	bool LoadInstanceFunctions()
	{
		bool ok = true;
#define PROBE_LOAD_VK_INSTANCE(name) \
	name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(s_vk_instance, #name)); \
	if (!name && std::strcmp(#name, "vkGetPhysicalDeviceProperties2") != 0 && std::strcmp(#name, "vkGetPhysicalDeviceFeatures2") != 0) \
	{ \
		Log("FAIL: Vulkan instance function " #name " is missing."); \
		ok = false; \
	}
		PROBE_VK_INSTANCE_FUNCS(PROBE_LOAD_VK_INSTANCE)
#undef PROBE_LOAD_VK_INSTANCE
		return ok;
	}

	bool LoadDeviceFunctions()
	{
		bool ok = true;
#define PROBE_LOAD_VK_DEVICE(name) \
	name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(s_vk_device, #name)); \
	if (!name) \
	{ \
		Log("FAIL: Vulkan device function " #name " is missing."); \
		ok = false; \
	}
		PROBE_VK_DEVICE_FUNCS(PROBE_LOAD_VK_DEVICE)
#undef PROBE_LOAD_VK_DEVICE
		return ok;
	}

	DeviceFacts ReportPhysicalDevice(VkPhysicalDevice pd, const char* label)
	{
		DeviceFacts facts;

		VkPhysicalDeviceProperties props = {};
		vkGetPhysicalDeviceProperties(pd, &props);
		Log("%s: %s (%s, vendor 0x%04x, device 0x%04x)", label, props.deviceName, DeviceTypeName(props.deviceType), props.vendorID,
			props.deviceID);
		// Mesa packs its own version like VK_MAKE_VERSION (10.10.12 bits), which ARMSX2's driver rules compare against.
		Log("  apiVersion %s, driverVersion 0x%08x (as Mesa packs it: %u.%u.%u)", VersionString(props.apiVersion).c_str(),
			props.driverVersion, props.driverVersion >> 22, (props.driverVersion >> 12) & 0x3ff, props.driverVersion & 0xfff);

		uint32_t ext_count = 0;
		vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, nullptr);
		std::vector<VkExtensionProperties> exts(ext_count);
		if (ext_count > 0)
			vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, exts.data());
		exts.resize(ext_count);
		auto has_ext = [&](const char* name) {
			return std::any_of(exts.begin(), exts.end(), [name](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
		};

		if (props.apiVersion >= VK_API_VERSION_1_1 && vkGetPhysicalDeviceProperties2 && vkGetPhysicalDeviceFeatures2)
		{
			// The instance asks for Vulkan 1.1, so the 1.2 driver-properties struct needs its extension to be chained.
			const bool driver_props = has_ext(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
			VkPhysicalDeviceDriverProperties driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
			VkPhysicalDeviceMultiviewProperties mv_props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES};
			VkPhysicalDeviceProperties2 props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
			props2.pNext = &mv_props;
			if (driver_props)
				mv_props.pNext = &driver;
			vkGetPhysicalDeviceProperties2(pd, &props2);
			if (driver_props)
			{
				Log("  driverID %s (%d), driverName \"%s\", driverInfo \"%s\"", DriverIdName(driver.driverID),
					static_cast<int>(driver.driverID), driver.driverName, driver.driverInfo);
				Log("  conformance version %u.%u.%u.%u", driver.conformanceVersion.major, driver.conformanceVersion.minor,
					driver.conformanceVersion.subminor, driver.conformanceVersion.patch);
			}
			else
			{
				Log("  no VK_KHR_driver_properties, so no driverID");
			}

			VkPhysicalDeviceMultiviewFeatures mv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
			VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			features2.pNext = &mv;
			vkGetPhysicalDeviceFeatures2(pd, &features2);
			facts.multiview = mv.multiview == VK_TRUE;
			Log("  multiview %s (with geometry shaders %s, tessellation %s), maxMultiviewViewCount %u", facts.multiview ? "YES" : "NO",
				mv.multiviewGeometryShader ? "yes" : "no", mv.multiviewTessellationShader ? "yes" : "no", mv_props.maxMultiviewViewCount);
		}
		else
		{
			Log("  Vulkan 1.0 device: no driverID or multiview query");
		}

		uint32_t qf_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, nullptr);
		std::vector<VkQueueFamilyProperties> qfs(qf_count);
		if (qf_count > 0)
			vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs.data());
		for (uint32_t i = 0; i < qf_count; i++)
		{
			Log("  queue family %u: %u queue(s), %s, timestampValidBits %u", i, qfs[i].queueCount, QueueFlagsString(qfs[i].queueFlags).c_str(),
				qfs[i].timestampValidBits);
			if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && facts.graphics_family == UINT32_MAX)
			{
				facts.graphics_family = i;
				facts.graphics_queue_count = qfs[i].queueCount;
			}
		}

		VkPhysicalDeviceMemoryProperties mem = {};
		vkGetPhysicalDeviceMemoryProperties(pd, &mem);
		for (uint32_t i = 0; i < mem.memoryHeapCount; i++)
		{
			Log("  memory heap %u: %.1f GiB%s", i, static_cast<double>(mem.memoryHeaps[i].size) / (1024.0 * 1024.0 * 1024.0),
				(mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? ", device local" : "");
		}

		std::vector<std::string> names;
		for (const VkExtensionProperties& e : exts)
			names.emplace_back(e.extensionName);
		std::sort(names.begin(), names.end());
		Log("  device extensions: %u", ext_count);
		std::string line;
		for (size_t i = 0; i < names.size(); i++)
		{
			line += (line.empty() ? "" : " ") + names[i];
			if (line.size() > 150 || i + 1 == names.size())
			{
				Log("    %s", line.c_str());
				line.clear();
			}
		}
		return facts;
	}

	// When no runtime answers (or it cannot bind Vulkan), the log still says which GPU driver this process sees.
	void ReportVulkanWithoutXR(const char* why)
	{
		Section("Vulkan (direct, without OpenXR)");
		Log("%s Listing the Vulkan devices directly instead.", why);
		if (!LoadVulkanLibrary())
			return;

		VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.pApplicationName = "frame-xrprobe";
		app.pEngineName = "ARMSX2 Outbreak VR";
		app.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ci.pApplicationInfo = &app;
		VkResult vr;
		{
			ScopedStep step("vkCreateInstance");
			vr = vkCreateInstance(&ci, nullptr, &s_vk_instance);
		}
		if (vr != VK_SUCCESS)
		{
			Log("vkCreateInstance (Vulkan 1.1): %s (%d), so no Vulkan driver is usable here.", VkResultName(vr), static_cast<int>(vr));
			s_vk_instance = VK_NULL_HANDLE;
			return;
		}
		if (LoadInstanceFunctions())
		{
			uint32_t n = 0;
			vkEnumeratePhysicalDevices(s_vk_instance, &n, nullptr);
			std::vector<VkPhysicalDevice> devices(n);
			if (n > 0)
				vkEnumeratePhysicalDevices(s_vk_instance, &n, devices.data());
			Log("Vulkan devices: %u", n);
			for (uint32_t i = 0; i < n; i++)
				ReportPhysicalDevice(devices[i], Format("Vulkan device %u", i).c_str());
		}
		vkDestroyInstance(s_vk_instance, nullptr);
		s_vk_instance = VK_NULL_HANDLE;
	}

	Outcome CreateVulkanThroughXR()
	{
		Section("Vulkan (through XR_KHR_vulkan_enable2)");
		if (!pfn_xrGetVulkanGraphicsRequirements2KHR || !pfn_xrCreateVulkanInstanceKHR || !pfn_xrGetVulkanGraphicsDevice2KHR ||
			!pfn_xrCreateVulkanDeviceKHR)
		{
			return Outcome::Fail;
		}
		if (!LoadVulkanLibrary())
			return Outcome::Fail;

		XrGraphicsRequirementsVulkan2KHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
		if (!CheckXR(pfn_xrGetVulkanGraphicsRequirements2KHR(s_instance, s_system, &reqs), "xrGetVulkanGraphicsRequirements2KHR"))
			return Outcome::Fail;
		Log("Runtime supports Vulkan %u.%u - %u.%u", XR_VERSION_MAJOR(reqs.minApiVersionSupported), XR_VERSION_MINOR(reqs.minApiVersionSupported),
			XR_VERSION_MAJOR(reqs.maxApiVersionSupported), XR_VERSION_MINOR(reqs.maxApiVersionSupported));

		// ARMSX2's GS asks for Vulkan 1.1, so that is what the probe asks for, unless the runtime insists on more.
		uint32_t api = VK_API_VERSION_1_1;
		const uint32_t min_major = static_cast<uint32_t>(XR_VERSION_MAJOR(reqs.minApiVersionSupported));
		const uint32_t min_minor = static_cast<uint32_t>(XR_VERSION_MINOR(reqs.minApiVersionSupported));
		if (min_major > 1 || (min_major == 1 && min_minor > 1))
		{
			api = VK_MAKE_API_VERSION(0, min_major, min_minor, 0);
			Log("NOTE: the runtime needs at least Vulkan %u.%u, more than the 1.1 ARMSX2 asks for. Using %u.%u here.", min_major,
				min_minor, min_major, min_minor);
		}

		VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.pApplicationName = "frame-xrprobe";
		app.pEngineName = "ARMSX2 Outbreak VR";
		app.apiVersion = api;
		VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ci.pApplicationInfo = &app;

		XrVulkanInstanceCreateInfoKHR xci = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
		xci.systemId = s_system;
		xci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
		xci.vulkanCreateInfo = &ci;
		VkResult vr = VK_SUCCESS;
		int64_t t0 = NowMs();
		XrResult r;
		{
			ScopedStep step("xrCreateVulkanInstanceKHR");
			r = pfn_xrCreateVulkanInstanceKHR(s_instance, &xci, &s_vk_instance, &vr);
		}
		Log("xrCreateVulkanInstanceKHR (Vulkan %s): %s, vkCreateInstance %s, %lld ms", VersionString(api).c_str(), XrResultName(r),
			VkResultName(vr), static_cast<long long>(NowMs() - t0));
		if (XR_FAILED(r) || vr != VK_SUCCESS)
		{
			s_vk_instance = VK_NULL_HANDLE;
			return Outcome::Fail;
		}
		if (!LoadInstanceFunctions())
			return Outcome::Fail;

		XrVulkanGraphicsDeviceGetInfoKHR gi = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
		gi.systemId = s_system;
		gi.vulkanInstance = s_vk_instance;
		if (!CheckXR(pfn_xrGetVulkanGraphicsDevice2KHR(s_instance, &gi, &s_vk_physical), "xrGetVulkanGraphicsDevice2KHR"))
			return Outcome::Fail;

		const DeviceFacts facts = ReportPhysicalDevice(s_vk_physical, "Vulkan device chosen by the runtime");
		if (facts.graphics_family == UINT32_MAX)
		{
			Log("FAIL: the runtime's device has no graphics queue family.");
			return Outcome::Fail;
		}

		// Two queues on the graphics family would let a later milestone give the runtime its own queue instead
		// of sharing the GS queue under the lock.
		const uint32_t want = std::min<uint32_t>(2, facts.graphics_queue_count);
		const float priorities[2] = {1.0f, 1.0f};
		VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qci.queueFamilyIndex = facts.graphics_family;
		qci.queueCount = want;
		qci.pQueuePriorities = priorities;
		VkPhysicalDeviceMultiviewFeatures mv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
		mv.multiview = facts.multiview ? VK_TRUE : VK_FALSE;
		VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		dci.pNext = &mv;
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;

		XrVulkanDeviceCreateInfoKHR xdci = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
		xdci.systemId = s_system;
		xdci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
		xdci.vulkanPhysicalDevice = s_vk_physical;
		xdci.vulkanCreateInfo = &dci;
		Log("Calling xrCreateVulkanDeviceKHR (graphics family %u, %u queue(s), multiview %s). If the log stops here, "
			"launch with FRAME_NO_FDM=1 (SteamVR-for-Linux #972).",
			facts.graphics_family, want, facts.multiview ? "on" : "off");
		t0 = NowMs();
		{
			ScopedStep step("xrCreateVulkanDeviceKHR");
			r = pfn_xrCreateVulkanDeviceKHR(s_instance, &xdci, &s_vk_device, &vr);
		}
		Log("xrCreateVulkanDeviceKHR: %s, vkCreateDevice %s, %lld ms", XrResultName(r), VkResultName(vr), static_cast<long long>(NowMs() - t0));
		if (XR_FAILED(r) || vr != VK_SUCCESS)
		{
			s_vk_device = VK_NULL_HANDLE;
			return Outcome::Fail;
		}
		Log("Answer: the graphics family offers %u queue(s): %s", facts.graphics_queue_count,
			facts.graphics_queue_count >= 2 ? "two or more, so a dedicated XR queue is possible later (M4)"
											: "only one, so the GS and the runtime share it under the queue lock");
		s_vk_graphics_family = facts.graphics_family;

		if (!LoadDeviceFunctions())
			return Outcome::Fail;
		vkGetDeviceQueue(s_vk_device, s_vk_graphics_family, 0, &s_vk_queue);

		VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = s_vk_graphics_family;
		if (vkCreateCommandPool(s_vk_device, &pci, nullptr, &s_vk_pool) != VK_SUCCESS)
			return Outcome::Fail;
		VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = s_vk_pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(s_vk_device, &cai, &s_vk_cmd) != VK_SUCCESS)
			return Outcome::Fail;
		VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // the first frame waits on it before any submit
		if (vkCreateFence(s_vk_device, &fci, nullptr, &s_vk_fence) != VK_SUCCESS)
			return Outcome::Fail;
		return Outcome::Ok;
	}

	// ------------------------------------------------------------------------------------------------
	// Session: spaces, formats, refresh rates, actions, swapchains
	// ------------------------------------------------------------------------------------------------

	XrSession s_session = XR_NULL_HANDLE;
	XrSpace s_local_space = XR_NULL_HANDLE;
	XrSpace s_view_space = XR_NULL_HANDLE;
	XrSessionState s_state = XR_SESSION_STATE_UNKNOWN;
	bool s_running = false;    // between xrBeginSession and xrEndSession
	bool s_ever_began = false;
	bool s_exit_loop = false;  // EXITING, LOSS_PENDING or instance loss
	int64_t s_session_created_ms = 0;
	int64_t s_state_since_ms = 0;
	int64_t s_began_ms = 0;
	int64_t s_swapchain_format = 0;

	XrActionSet s_action_set = XR_NULL_HANDLE;
	XrAction s_trigger_action = XR_NULL_HANDLE;
	XrAction s_aim_action = XR_NULL_HANDLE;
	XrPath s_hand_paths[2] = {XR_NULL_PATH, XR_NULL_PATH};
	XrSpace s_aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
	bool s_actions_attached = false;
	bool s_focused_profiles_logged = false;
	bool s_trigger_down[2] = {false, false};

	struct ProbeSwapchain
	{
		const char* name = "";
		XrSwapchain handle = XR_NULL_HANDLE;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t layers = 1;
		std::vector<VkImage> images;
		// Created without TRANSFER_DST (the runtime refused it): cleared through a render pass instead.
		bool render_pass = false;
		std::vector<VkImageView> views;           // [image * layers + layer]
		std::vector<VkFramebuffer> framebuffers;  // same indexing
	};
	ProbeSwapchain s_sc_array;   // phase A: arraySize 2
	ProbeSwapchain s_sc_eye[2];  // phases B-D: one per eye
	ProbeSwapchain s_sc_quad;    // phases C-D: the quad / cylinder image

	void LogInteractionProfiles(const char* why)
	{
		if (!s_actions_attached)
			return;
		for (int h = 0; h < 2; h++)
		{
			XrInteractionProfileState st = {XR_TYPE_INTERACTION_PROFILE_STATE};
			const XrResult r = xrGetCurrentInteractionProfile(s_session, s_hand_paths[h], &st);
			if (XR_FAILED(r))
				Log("Interaction profile (%s), %s hand: xrGetCurrentInteractionProfile %s", why, h ? "right" : "left", XrResultName(r));
			else
				Log("Interaction profile (%s), %s hand: %s", why, h ? "right" : "left", PathString(st.interactionProfile).c_str());
		}
	}

	struct Binding
	{
		XrAction action;
		const char* path;
	};

	bool Suggest(const char* profile, std::initializer_list<Binding> list, const char* note)
	{
		XrPath profile_path = XR_NULL_PATH;
		XrResult r = xrStringToPath(s_instance, profile, &profile_path);
		if (XR_FAILED(r))
		{
			Log("Bindings for %s%s: xrStringToPath %s", profile, note, XrResultName(r));
			return false;
		}
		std::vector<XrActionSuggestedBinding> bindings;
		for (const Binding& b : list)
		{
			XrPath p = XR_NULL_PATH;
			r = xrStringToPath(s_instance, b.path, &p);
			if (XR_FAILED(r))
			{
				Log("Bindings for %s%s: xrStringToPath(%s) %s", profile, note, b.path, XrResultName(r));
				return false;
			}
			bindings.push_back({b.action, p});
		}
		XrInteractionProfileSuggestedBinding s = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		s.interactionProfile = profile_path;
		s.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
		s.suggestedBindings = bindings.data();
		r = xrSuggestInteractionProfileBindings(s_instance, &s);
		Log("Bindings for %s%s: %s", profile, note, XR_SUCCEEDED(r) ? "accepted" : Format("rejected (%s)", XrResultName(r)).c_str());
		return XR_SUCCEEDED(r);
	}

	bool SetUpActions()
	{
		if (!CheckXR(xrStringToPath(s_instance, "/user/hand/left", &s_hand_paths[0]), "xrStringToPath(left)") ||
			!CheckXR(xrStringToPath(s_instance, "/user/hand/right", &s_hand_paths[1]), "xrStringToPath(right)"))
		{
			return false;
		}

		XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
		std::strncpy(asci.actionSetName, "probe", XR_MAX_ACTION_SET_NAME_SIZE - 1);
		std::strncpy(asci.localizedActionSetName, "Probe", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
		if (!CheckXR(xrCreateActionSet(s_instance, &asci, &s_action_set), "xrCreateActionSet"))
			return false;

		XrActionCreateInfo aci = {XR_TYPE_ACTION_CREATE_INFO};
		aci.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
		std::strncpy(aci.actionName, "trigger", XR_MAX_ACTION_NAME_SIZE - 1);
		std::strncpy(aci.localizedActionName, "Trigger", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
		aci.countSubactionPaths = 2;
		aci.subactionPaths = s_hand_paths;
		if (!CheckXR(xrCreateAction(s_action_set, &aci, &s_trigger_action), "xrCreateAction(trigger)"))
			return false;
		aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
		std::memset(aci.actionName, 0, sizeof(aci.actionName));
		std::memset(aci.localizedActionName, 0, sizeof(aci.localizedActionName));
		std::strncpy(aci.actionName, "aim", XR_MAX_ACTION_NAME_SIZE - 1);
		std::strncpy(aci.localizedActionName, "Aim pose", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
		if (!CheckXR(xrCreateAction(s_action_set, &aci, &s_aim_action), "xrCreateAction(aim)"))
			return false;

		// Valve documents frame_controller_valve; PenguinScreen2's VRInput.cpp suggests .../frame_controller. Both are
		// tried, and xrGetCurrentInteractionProfile after FOCUSED says which one the runtime really uses.
		if (s_enabled.frame_controller)
		{
			Suggest("/interaction_profiles/valve/frame_controller_valve",
				{{s_trigger_action, "/user/hand/left/input/trigger/value"}, {s_trigger_action, "/user/hand/right/input/trigger/value"},
					{s_aim_action, "/user/hand/left/input/aim/pose"}, {s_aim_action, "/user/hand/right/input/aim/pose"}},
				" (Valve's documented path)");
			Suggest("/interaction_profiles/valve/frame_controller",
				{{s_trigger_action, "/user/hand/left/input/trigger/value"}, {s_trigger_action, "/user/hand/right/input/trigger/value"},
					{s_aim_action, "/user/hand/left/input/aim/pose"}, {s_aim_action, "/user/hand/right/input/aim/pose"}},
				" (the path PenguinScreen2's VRInput.cpp uses)");
		}
		else
		{
			Log("XR_VALVE_frame_controller_interaction is not offered, so no Frame controller bindings are suggested.");
		}
		Suggest("/interaction_profiles/oculus/touch_controller",
			{{s_trigger_action, "/user/hand/left/input/trigger/value"}, {s_trigger_action, "/user/hand/right/input/trigger/value"},
				{s_aim_action, "/user/hand/left/input/aim/pose"}, {s_aim_action, "/user/hand/right/input/aim/pose"}},
			"");
		Suggest("/interaction_profiles/khr/simple_controller",
			{{s_trigger_action, "/user/hand/left/input/select/click"}, {s_trigger_action, "/user/hand/right/input/select/click"},
				{s_aim_action, "/user/hand/left/input/aim/pose"}, {s_aim_action, "/user/hand/right/input/aim/pose"}},
			"");

		XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
		attach.countActionSets = 1;
		attach.actionSets = &s_action_set;
		if (!CheckXR(xrAttachSessionActionSets(s_session, &attach), "xrAttachSessionActionSets"))
			return false;
		s_actions_attached = true;

		for (int h = 0; h < 2; h++)
		{
			XrActionSpaceCreateInfo sci = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
			sci.action = s_aim_action;
			sci.subactionPath = s_hand_paths[h];
			sci.poseInActionSpace.orientation.w = 1.0f;
			CheckXR(xrCreateActionSpace(s_session, &sci, &s_aim_space[h]), "xrCreateActionSpace(aim)");
		}
		return true;
	}

	VkRenderPass GetClearRenderPass()
	{
		if (s_vk_clear_pass != VK_NULL_HANDLE)
			return s_vk_clear_pass;
		VkAttachmentDescription att = {};
		att.format = static_cast<VkFormat>(s_swapchain_format);
		att.samples = VK_SAMPLE_COUNT_1_BIT;
		att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		// OpenXR hands colour images back to the runtime in COLOR_ATTACHMENT_OPTIMAL.
		att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription sub = {};
		sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		sub.colorAttachmentCount = 1;
		sub.pColorAttachments = &ref;
		VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		rpci.attachmentCount = 1;
		rpci.pAttachments = &att;
		rpci.subpassCount = 1;
		rpci.pSubpasses = &sub;
		if (vkCreateRenderPass(s_vk_device, &rpci, nullptr, &s_vk_clear_pass) != VK_SUCCESS)
			s_vk_clear_pass = VK_NULL_HANDLE;
		return s_vk_clear_pass;
	}

	bool CreateSwapchain(ProbeSwapchain& sc, const char* name, uint32_t width, uint32_t height, uint32_t layers)
	{
		sc.name = name;
		XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
		ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		ci.format = s_swapchain_format;
		ci.sampleCount = 1;
		ci.width = width;
		ci.height = height;
		ci.faceCount = 1;
		ci.arraySize = layers;
		ci.mipCount = 1;
		XrResult r = xrCreateSwapchain(s_session, &ci, &sc.handle);
		if (XR_FAILED(r))
		{
			Log("xrCreateSwapchain(%s, colour attachment + transfer dst): %s. Retrying as a colour attachment only, "
				"cleared by a render pass.",
				name, XrResultName(r));
			ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
			r = xrCreateSwapchain(s_session, &ci, &sc.handle);
			if (XR_FAILED(r))
			{
				Log("FAIL: xrCreateSwapchain(%s, %u x %u, arraySize %u): %s", name, width, height, layers, XrResultName(r));
				sc.handle = XR_NULL_HANDLE;
				return false;
			}
			sc.render_pass = true;
		}
		sc.width = width;
		sc.height = height;
		sc.layers = layers;

		uint32_t n = 0;
		if (!CheckXR(xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr), "xrEnumerateSwapchainImages"))
			return false;
		std::vector<XrSwapchainImageVulkan2KHR> images(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
		if (!CheckXR(xrEnumerateSwapchainImages(sc.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
				"xrEnumerateSwapchainImages"))
		{
			return false;
		}
		for (const XrSwapchainImageVulkan2KHR& img : images)
			sc.images.push_back(img.image);
		Log("Swapchain %s: %u x %u, arraySize %u, %u images%s", name, width, height, layers, n,
			sc.render_pass ? ", render-pass clear" : "");

		if (sc.render_pass)
		{
			const VkRenderPass pass = GetClearRenderPass();
			if (pass == VK_NULL_HANDLE)
				return false;
			for (VkImage image : sc.images)
			{
				for (uint32_t layer = 0; layer < layers; layer++)
				{
					VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
					vci.image = image;
					vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
					vci.format = static_cast<VkFormat>(s_swapchain_format);
					vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
					VkImageView view = VK_NULL_HANDLE;
					if (vkCreateImageView(s_vk_device, &vci, nullptr, &view) != VK_SUCCESS)
						return false;
					sc.views.push_back(view);
					VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
					fci.renderPass = pass;
					fci.attachmentCount = 1;
					fci.pAttachments = &view;
					fci.width = width;
					fci.height = height;
					fci.layers = 1;
					VkFramebuffer fb = VK_NULL_HANDLE;
					if (vkCreateFramebuffer(s_vk_device, &fci, nullptr, &fb) != VK_SUCCESS)
						return false;
					sc.framebuffers.push_back(fb);
				}
			}
		}
		return true;
	}

	void DestroySwapchain(ProbeSwapchain& sc)
	{
		for (VkFramebuffer fb : sc.framebuffers)
			vkDestroyFramebuffer(s_vk_device, fb, nullptr);
		for (VkImageView v : sc.views)
			vkDestroyImageView(s_vk_device, v, nullptr);
		sc.framebuffers.clear();
		sc.views.clear();
		sc.images.clear();
		if (sc.handle != XR_NULL_HANDLE)
			xrDestroySwapchain(sc.handle);
		sc.handle = XR_NULL_HANDLE;
	}

	Outcome CreateSession()
	{
		Section("Session");
		XrGraphicsBindingVulkan2KHR binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
		binding.instance = s_vk_instance;
		binding.physicalDevice = s_vk_physical;
		binding.device = s_vk_device;
		binding.queueFamilyIndex = s_vk_graphics_family;
		binding.queueIndex = 0;
		XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
		sci.next = &binding;
		sci.systemId = s_system;

		const int64_t t0 = NowMs();
		XrResult r;
		{
			ScopedStep step("xrCreateSession");
			r = xrCreateSession(s_instance, &sci, &s_session);
		}
		Log("xrCreateSession (Vulkan, queue family %u, queue 0): %s, %lld ms", s_vk_graphics_family, XrResultName(r),
			static_cast<long long>(NowMs() - t0));
		if (XR_FAILED(r))
		{
			s_session = XR_NULL_HANDLE;
			return Outcome::Fail;
		}
		s_session_created_ms = NowMs();
		s_state_since_ms = s_session_created_ms;

		uint32_t n = 0;
		std::vector<XrReferenceSpaceType> spaces;
		if (XR_SUCCEEDED(xrEnumerateReferenceSpaces(s_session, 0, &n, nullptr)) && n > 0)
		{
			spaces.resize(n);
			xrEnumerateReferenceSpaces(s_session, n, &n, spaces.data());
		}
		std::string names;
		for (XrReferenceSpaceType t : spaces)
			names += std::string(" ") + ReferenceSpaceName(t);
		Log("Reference spaces:%s", names.empty() ? " none" : names.c_str());

		XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		rsci.poseInReferenceSpace.orientation.w = 1.0f;
		rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		if (!CheckXR(xrCreateReferenceSpace(s_session, &rsci, &s_local_space), "xrCreateReferenceSpace(LOCAL)"))
			return Outcome::Fail;
		rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		CheckXR(xrCreateReferenceSpace(s_session, &rsci, &s_view_space), "xrCreateReferenceSpace(VIEW)");

		// Format choice as XRCompositor makes it: sRGB first, then UNORM. Anything else only so the colour
		// phases can still run; the VR layer itself would refuse it.
		n = 0;
		std::vector<int64_t> formats;
		if (XR_SUCCEEDED(xrEnumerateSwapchainFormats(s_session, 0, &n, nullptr)) && n > 0)
		{
			formats.resize(n);
			xrEnumerateSwapchainFormats(s_session, n, &n, formats.data());
		}
		Log("Swapchain formats offered: %u", n);
		for (int64_t f : formats)
			Log("  %lld %s", static_cast<long long>(f), VkFormatName(f));
		const auto offered = [&](VkFormat f) { return std::find(formats.begin(), formats.end(), static_cast<int64_t>(f)) != formats.end(); };
		if (offered(VK_FORMAT_R8G8B8A8_SRGB))
			s_swapchain_format = VK_FORMAT_R8G8B8A8_SRGB;
		else if (offered(VK_FORMAT_R8G8B8A8_UNORM))
			s_swapchain_format = VK_FORMAT_R8G8B8A8_UNORM;
		else if (!formats.empty())
		{
			s_swapchain_format = formats[0];
			Log("NOTE: neither R8G8B8A8_SRGB nor R8G8B8A8_UNORM is offered; XRCompositor would refuse this runtime.");
		}
		else
		{
			Log("FAIL: the runtime offers no swapchain formats.");
			return Outcome::Fail;
		}
		Log("Chosen swapchain format: %s (%lld)", VkFormatName(s_swapchain_format), static_cast<long long>(s_swapchain_format));

		if (s_enabled.refresh_rate && pfn_xrEnumerateDisplayRefreshRatesFB && pfn_xrGetDisplayRefreshRateFB)
		{
			n = 0;
			std::vector<float> rates;
			if (XR_SUCCEEDED(pfn_xrEnumerateDisplayRefreshRatesFB(s_session, 0, &n, nullptr)) && n > 0)
			{
				rates.resize(n);
				pfn_xrEnumerateDisplayRefreshRatesFB(s_session, n, &n, rates.data());
			}
			std::string list;
			for (float hz : rates)
				list += Format(" %.2f", static_cast<double>(hz));
			float current = 0.0f;
			pfn_xrGetDisplayRefreshRateFB(s_session, &current);
			Log("Display refresh rates (Hz):%s; current %.2f Hz", list.empty() ? " none" : list.c_str(), static_cast<double>(current));
		}
		else
		{
			Log("XR_FB_display_refresh_rate not offered: the rate shows only as predictedDisplayPeriod in the phases below.");
		}

		if (!SetUpActions())
			Log("Controller actions are not set up; the colour phases still run.");

		// Swapchains for the phases. A failure only drops the phases that need that swapchain.
		const uint32_t array_w = std::max(s_views[0].recommendedImageRectWidth, s_views[1].recommendedImageRectWidth);
		const uint32_t array_h = std::max(s_views[0].recommendedImageRectHeight, s_views[1].recommendedImageRectHeight);
		CreateSwapchain(s_sc_array, "A (arraySize 2)", array_w, array_h, 2);
		CreateSwapchain(s_sc_eye[0], "left eye", s_views[0].recommendedImageRectWidth, s_views[0].recommendedImageRectHeight, 1);
		CreateSwapchain(s_sc_eye[1], "right eye", s_views[1].recommendedImageRectWidth, s_views[1].recommendedImageRectHeight, 1);
		CreateSwapchain(s_sc_quad, "quad/cylinder", 512, 288, 1);
		return Outcome::Ok;
	}

	// ------------------------------------------------------------------------------------------------
	// Frame loop
	// ------------------------------------------------------------------------------------------------

	struct PhaseInfo
	{
		char letter;
		const char* description;
		const char* question;
	};

	constexpr PhaseInfo kPhases[] = {
		{'A', "one projection layer, one swapchain with arraySize 2 (imageArrayIndex 0 and 1): LEFT eye RED, RIGHT eye BLUE",
			"Left eye red and right eye blue, or the same colour in both? (Same colour: M1 must use per-eye swapchains.)"},
		{'B', "one projection layer, two swapchains with arraySize 1: LEFT eye GREEN, RIGHT eye MAGENTA",
			"Left eye green and right eye magenta?"},
		{'C', "black projection layer plus a quad layer 2 m ahead, 1.6 x 0.9 m: YELLOW",
			"A yellow rectangle straight ahead that stays put when the head turns?"},
		{'D', "black projection layer plus a cylinder layer, radius 2 m, 1.6 x 0.9 m: CYAN",
			"A cyan curved screen ahead?"},
	};

	constexpr float kRed[4] = {1.0f, 0.0f, 0.0f, 1.0f};
	constexpr float kBlue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
	constexpr float kGreen[4] = {0.0f, 1.0f, 0.0f, 1.0f};
	constexpr float kMagenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
	constexpr float kYellow[4] = {1.0f, 1.0f, 0.0f, 1.0f};
	constexpr float kCyan[4] = {0.0f, 1.0f, 1.0f, 1.0f};
	constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};

	struct ClearJob
	{
		ProbeSwapchain* sc;
		const float* colour[2]; // per array layer
		uint32_t image_index;
	};

	struct PhaseStats
	{
		int64_t start_ms = 0;
		uint64_t frames = 0;
		uint64_t rendered = 0;
		uint64_t end_frame_errors = 0;
		XrResult first_end_frame_error = XR_SUCCESS;
		RunningStat period_ms;
		RunningStat wait_ms;
		RunningStat submit_us;
		RunningStat end_us;
	};

	void RecordClear(const ClearJob& job)
	{
		ProbeSwapchain& sc = *job.sc;
		const VkImage image = sc.images[job.image_index];
		if (sc.render_pass)
		{
			for (uint32_t layer = 0; layer < sc.layers; layer++)
			{
				VkClearValue clear = {};
				std::memcpy(clear.color.float32, job.colour[layer], sizeof(clear.color.float32));
				VkRenderPassBeginInfo rpbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
				rpbi.renderPass = s_vk_clear_pass;
				rpbi.framebuffer = sc.framebuffers[job.image_index * sc.layers + layer];
				rpbi.renderArea.extent = {sc.width, sc.height};
				rpbi.clearValueCount = 1;
				rpbi.pClearValues = &clear;
				vkCmdBeginRenderPass(s_vk_cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
				vkCmdEndRenderPass(s_vk_cmd);
			}
			return;
		}

		// The whole image is overwritten, so its old contents (and layout) can be discarded.
		VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, sc.layers};
		vkCmdPipelineBarrier(s_vk_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
		for (uint32_t layer = 0; layer < sc.layers; layer++)
		{
			VkClearColorValue c;
			std::memcpy(c.float32, job.colour[layer], sizeof(c.float32));
			const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
			vkCmdClearColorImage(s_vk_cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
		}
		// OpenXR wants colour images released in COLOR_ATTACHMENT_OPTIMAL.
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		vkCmdPipelineBarrier(s_vk_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
	}

	// Acquire, clear and release every swapchain the frame uses. One command buffer, so the previous
	// frame's submit is waited first; the clears are trivial, so that costs nothing worth measuring.
	bool RenderJobs(ClearJob* jobs, size_t count, PhaseStats& stats)
	{
		if (vkWaitForFences(s_vk_device, 1, &s_vk_fence, VK_TRUE, 2'000'000'000ull) != VK_SUCCESS)
		{
			Log("FAIL: the previous frame's commands did not finish within 2 s.");
			return false;
		}
		vkResetFences(s_vk_device, 1, &s_vk_fence);

		size_t acquired = 0;
		bool ok = true;
		for (; acquired < count && ok; acquired++)
		{
			XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			if (!CheckXR(xrAcquireSwapchainImage(jobs[acquired].sc->handle, &ai, &jobs[acquired].image_index), "xrAcquireSwapchainImage"))
				break;
			XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wi.timeout = 1'000'000'000; // 1 s; XR_TIMEOUT_EXPIRED is a success code, so it is checked by value
			XrResult r = XR_TIMEOUT_EXPIRED;
			for (int tries = 0; tries < 3 && r == XR_TIMEOUT_EXPIRED; tries++)
				r = xrWaitSwapchainImage(jobs[acquired].sc->handle, &wi);
			if (r != XR_SUCCESS)
			{
				Log("FAIL: xrWaitSwapchainImage(%s): %s", jobs[acquired].sc->name, XrResultName(r));
				ok = false;
			}
		}
		if (acquired < count)
			ok = false;

		if (ok)
		{
			VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			vkResetCommandBuffer(s_vk_cmd, 0);
			vkBeginCommandBuffer(s_vk_cmd, &cbi);
			for (size_t i = 0; i < count; i++)
				RecordClear(jobs[i]);
			vkEndCommandBuffer(s_vk_cmd);
		}

		VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = ok ? 1 : 0;
		si.pCommandBuffers = &s_vk_cmd;
		const int64_t t0 = NowUs();
		// An empty submit still signals the fence when the frame is abandoned, so the next wait does not hang.
		const VkResult vr = vkQueueSubmit(s_vk_queue, 1, &si, s_vk_fence);
		stats.submit_us.Add(static_cast<double>(NowUs() - t0));
		if (vr != VK_SUCCESS)
		{
			Log("FAIL: vkQueueSubmit: %s", VkResultName(vr));
			ok = false;
		}

		// Released images must have their commands submitted already: the runtime may read them right away.
		for (size_t i = 0; i < acquired; i++)
		{
			XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			xrReleaseSwapchainImage(jobs[i].sc->handle, &ri);
		}
		return ok;
	}

	void HandleSessionState(XrSessionState state)
	{
		const int64_t now = NowMs();
		Log("Session state: %s -> %s (%lld ms after xrCreateSession, %lld ms in %s)", SessionStateName(s_state), SessionStateName(state),
			static_cast<long long>(now - s_session_created_ms), static_cast<long long>(now - s_state_since_ms), SessionStateName(s_state));
		s_state = state;
		s_state_since_ms = now;

		switch (state)
		{
			case XR_SESSION_STATE_READY:
			{
				XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
				bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (CheckXR(xrBeginSession(s_session, &bi), "xrBeginSession"))
				{
					Log("Session began.");
					s_running = true;
					if (!s_ever_began)
						s_began_ms = now;
					s_ever_began = true;
				}
				else
				{
					s_exit_loop = true;
				}
				break;
			}
			case XR_SESSION_STATE_STOPPING:
			{
				s_running = false;
				ScopedStep step("xrEndSession");
				if (CheckXR(xrEndSession(s_session), "xrEndSession"))
					Log("Session ended.");
				break;
			}
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				s_running = false;
				s_exit_loop = true;
				break;
			default:
				break;
		}
	}

	void PollEvents()
	{
		XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
		while (xrPollEvent(s_instance, &ev) == XR_SUCCESS)
		{
			switch (ev.type)
			{
				case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
					HandleSessionState(reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev)->state);
					break;
				case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
					Log("Event: instance loss pending.");
					s_exit_loop = true;
					break;
				case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
					LogInteractionProfiles("profile-changed event");
					break;
				case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
					Log("Event: %s reference space change pending (recenter).",
						ReferenceSpaceName(reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&ev)->referenceSpaceType));
					break;
				case XR_TYPE_EVENT_DATA_EVENTS_LOST:
					Log("Event: %u events lost.", reinterpret_cast<const XrEventDataEventsLost*>(&ev)->lostEventCount);
					break;
				case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB:
				{
					const auto* e = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&ev);
					Log("Event: display refresh rate %.2f -> %.2f Hz.", static_cast<double>(e->fromDisplayRefreshRate),
						static_cast<double>(e->toDisplayRefreshRate));
					break;
				}
				default:
					Log("Event: type %d.", static_cast<int>(ev.type));
					break;
			}
			ev = {XR_TYPE_EVENT_DATA_BUFFER};
		}
	}

	void SyncInput()
	{
		if (!s_actions_attached)
			return;
		XrActiveActionSet active = {s_action_set, XR_NULL_PATH};
		XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
		si.countActiveActionSets = 1;
		si.activeActionSets = &active;
		// XR_SESSION_NOT_FOCUSED is a success code: there is no input while unfocused, so only XR_SUCCESS counts.
		if (xrSyncActions(s_session, &si) != XR_SUCCESS)
			return;
		if (!s_focused_profiles_logged)
		{
			s_focused_profiles_logged = true;
			LogInteractionProfiles("first focused frame");
		}
		for (int h = 0; h < 2; h++)
		{
			XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
			gi.action = s_trigger_action;
			gi.subactionPath = s_hand_paths[h];
			XrActionStateFloat st = {XR_TYPE_ACTION_STATE_FLOAT};
			if (XR_FAILED(xrGetActionStateFloat(s_session, &gi, &st)) || !st.isActive)
				continue;
			const bool down = st.currentState > 0.5f;
			if (down != s_trigger_down[h])
			{
				s_trigger_down[h] = down;
				Log("Input: %s trigger %s (%.2f)", h ? "right" : "left", down ? "pulled" : "released", static_cast<double>(st.currentState));
			}
		}
	}

	void LogViews(const XrView* views)
	{
		constexpr double kDeg = 180.0 / 3.14159265358979323846;
		for (int eye = 0; eye < 2; eye++)
		{
			const XrFovf& f = views[eye].fov;
			const XrVector3f& p = views[eye].pose.position;
			Log("View %s: fov left %.1f, right %.1f, up %.1f, down %.1f degrees; position (%.3f, %.3f, %.3f) m", eye ? "right" : "left",
				f.angleLeft * kDeg, f.angleRight * kDeg, f.angleUp * kDeg, f.angleDown * kDeg, static_cast<double>(p.x),
				static_cast<double>(p.y), static_cast<double>(p.z));
		}
		const float dx = views[1].pose.position.x - views[0].pose.position.x;
		const float dy = views[1].pose.position.y - views[0].pose.position.y;
		const float dz = views[1].pose.position.z - views[0].pose.position.z;
		Log("Eye separation as tracked: %.1f mm", static_cast<double>(std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f));
	}

	void StartPhase(const PhaseInfo& phase, PhaseStats& stats)
	{
		stats = PhaseStats();
		stats.start_ms = NowMs();
		Log("");
		Log("---- Phase %c start (%d s): %s", phase.letter, s_opt.seconds_per_phase, phase.description);
		Log("     Wearer: %s", phase.question);
	}

	void EndPhase(const PhaseInfo& phase, const PhaseStats& s, XrTime time)
	{
		const double period = s.period_ms.Avg();
		Log("---- Phase %c end: %.1f s, %llu frames (%llu rendered), session %s", phase.letter, (NowMs() - s.start_ms) / 1000.0,
			static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.rendered), SessionStateName(s_state));
		Log("     predictedDisplayPeriod min/avg/max %.3f / %.3f / %.3f ms (%.1f Hz)", s.period_ms.min, period, s.period_ms.max,
			period > 0.0 ? 1000.0 / period : 0.0);
		Log("     xrWaitFrame avg %.2f ms, max %.2f ms", s.wait_ms.Avg(), s.wait_ms.max);
		Log("     vkQueueSubmit avg %.0f us, max %.0f us", s.submit_us.Avg(), s.submit_us.max);
		Log("     xrEndFrame avg %.0f us, max %.0f us, errors %llu%s%s", s.end_us.Avg(), s.end_us.max,
			static_cast<unsigned long long>(s.end_frame_errors), s.end_frame_errors ? ", first: " : "",
			s.end_frame_errors ? XrResultName(s.first_end_frame_error) : "");
		for (int h = 0; h < 2; h++)
		{
			if (s_aim_space[h] == XR_NULL_HANDLE)
				continue;
			XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
			if (XR_FAILED(xrLocateSpace(s_aim_space[h], s_local_space, time, &loc)))
				continue;
			const bool valid = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
			Log("     %s aim pose: %s (%.2f, %.2f, %.2f) m", h ? "right" : "left", valid ? "tracked" : "not tracked",
				static_cast<double>(loc.pose.position.x), static_cast<double>(loc.pose.position.y), static_cast<double>(loc.pose.position.z));
		}
		LogInteractionProfiles(Format("end of phase %c", phase.letter).c_str());
	}

	XrPosef IdentityPose()
	{
		XrPosef p = {};
		p.orientation.w = 1.0f;
		return p;
	}

	int RunFrameLoop()
	{
		Section("Frame loop");
		std::vector<const PhaseInfo*> phases;
		const bool eyes_ok = s_sc_eye[0].handle != XR_NULL_HANDLE && s_sc_eye[1].handle != XR_NULL_HANDLE;
		if (s_sc_array.handle != XR_NULL_HANDLE)
			phases.push_back(&kPhases[0]);
		if (eyes_ok)
			phases.push_back(&kPhases[1]);
		if (eyes_ok && s_sc_quad.handle != XR_NULL_HANDLE)
		{
			phases.push_back(&kPhases[2]);
			if (s_enabled.cylinder)
				phases.push_back(&kPhases[3]);
			else
				Log("Phase D (cylinder) skipped: the runtime does not offer XR_KHR_composition_layer_cylinder.");
		}
		std::string list;
		for (const PhaseInfo* p : phases)
			list += Format(" %c", p->letter);
		Log("Phases to run:%s, %d s each. They start once the session is VISIBLE.", list.empty() ? " none" : list.c_str(),
			s_opt.seconds_per_phase);
		if (phases.empty())
			Log("FAIL: no swapchain could be created, so there is nothing to show.");

		const int64_t phase_ms = static_cast<int64_t>(s_opt.seconds_per_phase) * 1000;
		int phase = -1;
		PhaseStats stats;
		bool failed = phases.empty();
		bool exit_requested = false;
		bool exit_sent = false;
		int64_t exit_requested_ms = 0;
		bool views_logged = false;
		bool locate_warned = false;
		uint64_t end_errors_logged = 0;
		uint64_t frames = 0;

		// xrRequestExitSession only works on a running session, so the request is sent from the frame loop.
		auto request_exit = [&](const char* why) {
			if (exit_requested)
				return;
			exit_requested = true;
			exit_requested_ms = NowMs();
			Log("%s Ending the session.", why);
		};

		while (!s_exit_loop)
		{
			PollEvents();
			if (s_exit_loop)
				break;
			const int64_t now = NowMs();

			if (s_quit_signal && !exit_requested)
			{
				request_exit("Signal received.");
				if (!s_running)
					break;
			}
			if (failed && !exit_requested)
				request_exit("Stopping after a failure.");

			if (!s_running)
			{
				// Before READY, or between STOPPING and EXITING (or READY again).
				if (!s_ever_began && now - s_session_created_ms > 60000)
				{
					Log("FAIL: the session did not become READY within 60 s. Headset asleep or not worn? SteamVR hides "
						"sessions while the headset is in standby.");
					failed = true;
					break;
				}
				if (exit_requested && now - exit_requested_ms > 10000)
				{
					Log("The runtime did not finish the session within 10 s of the exit request; destroying it anyway.");
					break;
				}
				if (s_ever_began && !exit_requested && now - s_state_since_ms > 120000)
				{
					Log("FAIL: the runtime stopped the session and has not made it READY again for 120 s.");
					failed = true;
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}

			if (exit_requested && !exit_sent)
			{
				exit_sent = true;
				if (!CheckXR(xrRequestExitSession(s_session), "xrRequestExitSession"))
					break;
			}
			if (exit_sent && now - exit_requested_ms > 10000)
			{
				Log("The runtime did not stop the session within 10 s of xrRequestExitSession; destroying it anyway.");
				break;
			}

			XrFrameWaitInfo fwi = {XR_TYPE_FRAME_WAIT_INFO};
			XrFrameState fs = {XR_TYPE_FRAME_STATE};
			int64_t t0 = NowUs();
			XrResult r;
			{
				ScopedStep step("xrWaitFrame");
				r = xrWaitFrame(s_session, &fwi, &fs);
			}
			const double wait_ms = static_cast<double>(NowUs() - t0) / 1000.0;
			if (XR_FAILED(r))
			{
				Log("xrWaitFrame: %s", XrResultName(r));
				if (r == XR_ERROR_SESSION_LOST || r == XR_ERROR_INSTANCE_LOST)
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
			XrFrameBeginInfo fbi = {XR_TYPE_FRAME_BEGIN_INFO};
			r = xrBeginFrame(s_session, &fbi);
			if (XR_FAILED(r))
			{
				Log("xrBeginFrame: %s", XrResultName(r));
				continue;
			}
			frames++;

			// Phases are timed from the moment someone can see them, not from session start.
			const bool shown = s_state == XR_SESSION_STATE_VISIBLE || s_state == XR_SESSION_STATE_FOCUSED;
			const int phase_count = static_cast<int>(phases.size());
			if (phase < 0 && !exit_requested && phase_count > 0)
			{
				if (shown || now - s_began_ms > 120000)
				{
					if (!shown)
						Log("WARNING: the session is still not VISIBLE 120 s after it began; running the phases anyway.");
					phase = 0;
					StartPhase(*phases[0], stats);
				}
			}
			else if (phase >= 0 && phase < phase_count && now - stats.start_ms >= phase_ms)
			{
				EndPhase(*phases[phase], stats, fs.predictedDisplayTime);
				phase++;
				if (phase < phase_count)
					StartPhase(*phases[phase], stats);
				else
					request_exit("All phases done.");
			}

			SyncInput();

			const bool active = phase >= 0 && phase < phase_count;
			XrCompositionLayerProjectionView pviews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
			XrCompositionLayerProjection projection = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
			XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
			XrCompositionLayerCylinderKHR cylinder = {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
			const XrCompositionLayerBaseHeader* layers[2] = {};
			uint32_t layer_count = 0;

			if (active)
			{
				stats.frames++;
				stats.period_ms.Add(static_cast<double>(fs.predictedDisplayPeriod) / 1e6);
				stats.wait_ms.Add(wait_ms);
			}
			if (active && fs.shouldRender)
			{
				XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
				XrViewState vs = {XR_TYPE_VIEW_STATE};
				XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
				vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				vli.displayTime = fs.predictedDisplayTime;
				vli.space = s_local_space;
				uint32_t view_count = 0;
				r = xrLocateViews(s_session, &vli, &vs, 2, &view_count, views);
				const bool located = XR_SUCCEEDED(r) && view_count == 2;
				if (located && !views_logged && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
				{
					views_logged = true;
					LogViews(views);
				}
				if (!located && !locate_warned)
				{
					locate_warned = true;
					Log("xrLocateViews: %s, %u views; frames go out without layers until it works.", XrResultName(r), view_count);
				}

				const char letter = phases[phase]->letter;
				ClearJob jobs[3] = {};
				size_t job_count = 0;
				if (letter == 'A')
					jobs[job_count++] = {&s_sc_array, {kRed, kBlue}, 0};
				else if (letter == 'B')
				{
					jobs[job_count++] = {&s_sc_eye[0], {kGreen, kGreen}, 0};
					jobs[job_count++] = {&s_sc_eye[1], {kMagenta, kMagenta}, 0};
				}
				else
				{
					const float* screen = letter == 'C' ? kYellow : kCyan;
					jobs[job_count++] = {&s_sc_eye[0], {kBlack, kBlack}, 0};
					jobs[job_count++] = {&s_sc_eye[1], {kBlack, kBlack}, 0};
					jobs[job_count++] = {&s_sc_quad, {screen, screen}, 0};
				}

				if (located && RenderJobs(jobs, job_count, stats))
				{
					stats.rendered++;
					for (int eye = 0; eye < 2; eye++)
					{
						const ProbeSwapchain& sc = letter == 'A' ? s_sc_array : s_sc_eye[eye];
						pviews[eye].pose = views[eye].pose;
						pviews[eye].fov = views[eye].fov;
						pviews[eye].subImage.swapchain = sc.handle;
						pviews[eye].subImage.imageRect.offset = {0, 0};
						pviews[eye].subImage.imageRect.extent = {static_cast<int32_t>(sc.width), static_cast<int32_t>(sc.height)};
						pviews[eye].subImage.imageArrayIndex = letter == 'A' ? static_cast<uint32_t>(eye) : 0;
					}
					projection.space = s_local_space;
					projection.viewCount = 2;
					projection.views = pviews;
					layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);

					XrSwapchainSubImage screen_image = {};
					screen_image.swapchain = s_sc_quad.handle;
					screen_image.imageRect.extent = {static_cast<int32_t>(s_sc_quad.width), static_cast<int32_t>(s_sc_quad.height)};
					if (letter == 'C')
					{
						quad.space = s_local_space;
						quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
						quad.subImage = screen_image;
						quad.pose = IdentityPose();
						quad.pose.position.z = -2.0f; // 2 m ahead of where the head was when the session started
						quad.size = {1.6f, 0.9f};
						layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
					}
					else if (letter == 'D')
					{
						// Centred on the head like PenguinScreen2's curved screen: radius 2 m, 1.6 m of arc, 16:9.
						cylinder.space = s_local_space;
						cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
						cylinder.subImage = screen_image;
						cylinder.pose = IdentityPose();
						cylinder.radius = 2.0f;
						cylinder.centralAngle = 1.6f / 2.0f;
						cylinder.aspectRatio = 1.6f / 0.9f;
						layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder);
					}
				}
				else if (located)
				{
					failed = true;
				}
			}

			XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
			fei.displayTime = fs.predictedDisplayTime;
			fei.environmentBlendMode = s_blend_mode;
			fei.layerCount = layer_count;
			fei.layers = layer_count ? layers : nullptr;
			t0 = NowUs();
			r = xrEndFrame(s_session, &fei);
			if (active)
				stats.end_us.Add(static_cast<double>(NowUs() - t0));
			if (XR_FAILED(r))
			{
				if (active && stats.end_frame_errors++ == 0)
					stats.first_end_frame_error = r;
				if (end_errors_logged++ < 5)
					Log("xrEndFrame (%u layers): %s", layer_count, XrResultName(r));
			}
		}

		if (phase >= 0 && phase < static_cast<int>(phases.size()))
		{
			Log("Phase %c was cut short.", phases[phase]->letter);
			EndPhase(*phases[phase], stats, 0);
			failed = true;
		}
		Log("Frame loop over: %llu frames in total.", static_cast<unsigned long long>(frames));
		if (phase < 0 && !phases.empty())
			failed = true;
		return failed ? 1 : 0;
	}

	// ------------------------------------------------------------------------------------------------
	// Teardown and main flow
	// ------------------------------------------------------------------------------------------------

	// Session, then the XR instance, then the Vulkan device and instance: the order PenguinScreen2's bridge
	// uses (OnGSDeviceDestroyed tears VR down before the device goes). Never a second session on this
	// instance: SteamVR-for-Linux #989 aborts the process.
	void DestroyAll()
	{
		if (s_vk_device != VK_NULL_HANDLE && vkDeviceWaitIdle)
			vkDeviceWaitIdle(s_vk_device);
		if (s_vk_device != VK_NULL_HANDLE)
		{
			DestroySwapchain(s_sc_array);
			DestroySwapchain(s_sc_eye[0]);
			DestroySwapchain(s_sc_eye[1]);
			DestroySwapchain(s_sc_quad);
		}
		for (XrSpace& space : s_aim_space)
		{
			if (space != XR_NULL_HANDLE)
				xrDestroySpace(space);
			space = XR_NULL_HANDLE;
		}
		if (s_view_space != XR_NULL_HANDLE)
			xrDestroySpace(s_view_space);
		if (s_local_space != XR_NULL_HANDLE)
			xrDestroySpace(s_local_space);
		s_view_space = s_local_space = XR_NULL_HANDLE;
		if (s_session != XR_NULL_HANDLE)
		{
			ScopedStep step("xrDestroySession");
			const int64_t t0 = NowMs();
			const XrResult r = xrDestroySession(s_session);
			Log("xrDestroySession: %s, %lld ms", XrResultName(r), static_cast<long long>(NowMs() - t0));
			s_session = XR_NULL_HANDLE;
		}
		if (s_action_set != XR_NULL_HANDLE)
			xrDestroyActionSet(s_action_set);
		s_action_set = XR_NULL_HANDLE;
		if (s_instance != XR_NULL_HANDLE)
		{
			ScopedStep step("xrDestroyInstance");
			const int64_t t0 = NowMs();
			const XrResult r = xrDestroyInstance(s_instance);
			Log("xrDestroyInstance: %s, %lld ms", XrResultName(r), static_cast<long long>(NowMs() - t0));
			s_instance = XR_NULL_HANDLE;
		}
		if (s_vk_device != VK_NULL_HANDLE)
		{
			if (s_vk_clear_pass != VK_NULL_HANDLE)
				vkDestroyRenderPass(s_vk_device, s_vk_clear_pass, nullptr);
			if (s_vk_fence != VK_NULL_HANDLE)
				vkDestroyFence(s_vk_device, s_vk_fence, nullptr);
			if (s_vk_pool != VK_NULL_HANDLE)
				vkDestroyCommandPool(s_vk_device, s_vk_pool, nullptr);
			if (vkDestroyDevice)
				vkDestroyDevice(s_vk_device, nullptr);
			s_vk_device = VK_NULL_HANDLE;
		}
		if (s_vk_instance != VK_NULL_HANDLE && vkDestroyInstance)
			vkDestroyInstance(s_vk_instance, nullptr);
		s_vk_instance = VK_NULL_HANDLE;
	}

	int Finish(Outcome o)
	{
		DestroyAll();
		return o == Outcome::Fail ? 1 : 0;
	}

	int RunProbe()
	{
		ReportHost();

		Section("OpenXR");
		ReportRuntimeManifests();
		ReportApiLayers();
		if (!EnumerateExtensions())
		{
			ReportVulkanWithoutXR("No OpenXR runtime.");
			Log("");
			Log("Result: runtime unavailable, so there is nothing more to probe.");
			return 0;
		}

		Outcome o = CreateInstance();
		if (o != Outcome::Ok)
			return Finish(o);
		o = ReportSystemAndViews();
		if (o != Outcome::Ok)
			return Finish(o);
		if (!s_enabled.vulkan2)
		{
			DestroyAll();
			ReportVulkanWithoutXR("XR_KHR_vulkan_enable2 is not offered.");
			return 0;
		}
		if (s_quit_signal)
			return Finish(Outcome::Stop);

		o = CreateVulkanThroughXR();
		if (o != Outcome::Ok)
			return Finish(o);
		if (s_opt.no_session)
		{
			Log("");
			Log("--no-session: stopping before xrCreateSession.");
			return Finish(Outcome::Ok);
		}
		if (s_quit_signal)
			return Finish(Outcome::Stop);

		o = CreateSession();
		if (o != Outcome::Ok)
			return Finish(o);
		const int code = RunFrameLoop();
		DestroyAll();
		return code;
	}

	void PrintUsage()
	{
		std::fprintf(stderr,
			"Usage: frame-xrprobe [--no-session] [--seconds-per-phase N] [--log-dir DIR]\n"
			"  --no-session           stop after the Vulkan section (no session, nothing shown)\n"
			"  --seconds-per-phase N  how long each colour phase is shown (default 8)\n"
			"  --log-dir DIR          where probe-YYYYmmdd-HHMMSS.log goes (default $HOME/frame-probe)\n");
	}

	bool ParseArgs(int argc, char** argv)
	{
		for (int i = 1; i < argc; i++)
		{
			std::string arg = argv[i];
			std::string value;
			const size_t eq = arg.find('=');
			const bool inline_value = arg.compare(0, 2, "--") == 0 && eq != std::string::npos;
			if (inline_value)
			{
				value = arg.substr(eq + 1);
				arg = arg.substr(0, eq);
			}
			auto take_value = [&]() -> bool {
				if (inline_value)
					return true;
				if (i + 1 >= argc)
					return false;
				value = argv[++i];
				return true;
			};

			if (arg == "--no-session")
				s_opt.no_session = true;
			else if (arg == "--seconds-per-phase")
			{
				if (!take_value())
					return false;
				char* end = nullptr;
				const long n = std::strtol(value.c_str(), &end, 10);
				if (!end || *end != '\0' || n < 1 || n > 600)
				{
					std::fprintf(stderr, "frame-xrprobe: --seconds-per-phase wants 1-600, got '%s'\n", value.c_str());
					return false;
				}
				s_opt.seconds_per_phase = static_cast<int>(n);
			}
			else if (arg == "--log-dir")
			{
				if (!take_value() || value.empty())
					return false;
				s_opt.log_dir = value;
			}
			else if (arg == "--help" || arg == "-h")
			{
				PrintUsage();
				std::exit(0);
			}
			else
			{
				std::fprintf(stderr, "frame-xrprobe: unknown argument '%s'\n", argv[i]);
				return false;
			}
		}
		return true;
	}

	void OpenLog(int argc, char** argv)
	{
		if (s_opt.log_dir.empty())
		{
			const std::string home = EnvOr("HOME", "");
			s_opt.log_dir = home.empty() ? std::string("/tmp/frame-probe") : home + "/frame-probe";
		}
		if (s_opt.log_dir[0] != '/')
		{
			char cwd[4096] = {};
			if (getcwd(cwd, sizeof(cwd)))
				s_opt.log_dir = std::string(cwd) + "/" + s_opt.log_dir;
		}

		const std::time_t t = std::time(nullptr);
		std::tm local = {};
		localtime_r(&t, &local);
		char stamp[32];
		std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
		char when[64];
		std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S %z", &local);

		if (MakeDirs(s_opt.log_dir))
		{
			s_log_path = s_opt.log_dir + "/probe-" + stamp + ".log";
			s_log = std::fopen(s_log_path.c_str(), "a");
			if (s_log)
				s_log_fd = fileno(s_log);
		}

		Log("frame-xrprobe %s, started %s", FRAME_XRPROBE_VERSION, when);
		std::string args;
		for (int i = 1; i < argc; i++)
			args += std::string(" ") + argv[i];
		Log("Arguments:%s", args.empty() ? " (none)" : args.c_str());
		if (s_log)
			Log("Log file: %s", s_log_path.c_str());
		else
			Log("WARNING: could not open a log file in %s (%s); logging to stderr only.", s_opt.log_dir.c_str(), std::strerror(errno));
		Log("OpenXR SDK headers %u.%u.%u, Vulkan headers %u", XR_VERSION_MAJOR(XR_CURRENT_API_VERSION), XR_VERSION_MINOR(XR_CURRENT_API_VERSION),
			XR_VERSION_PATCH(XR_CURRENT_API_VERSION), VK_HEADER_VERSION);
	}
} // namespace

int main(int argc, char** argv)
{
	if (!ParseArgs(argc, argv))
	{
		PrintUsage();
		return 2;
	}
	OpenLog(argc, argv);
	InstallSignalHandlers();
	std::thread watchdog(WatchdogMain);

	const int code = RunProbe();

	Log("");
	Log("frame-xrprobe finished, exit code %d.", code);
	s_watchdog_quit.store(true);
	watchdog.join();
	if (s_log)
		std::fclose(s_log);
	return code;
}
