// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "BuildVersion.h"
#include "Common.h"
#include "Host.h"
#include "Elfheader.h"
#include "GS.h"
#include "GS/GSPerfMon.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "MTGS.h"
#include "PerformanceMetrics.h"
#include "SaveState.h"
#include "PINE.h"
#include "SIO/Pad/Pad.h"
#include "VMManager.h"
#ifdef ENABLE_OPENXR
#include "Counters.h"
#include "MTGS.h"
#include "VR/BeltPouches.h"
#include "VR/CameraDriver.h"
#include "VR/DepthHistogram.h"
#include "VR/MemWatch.h"
#include "VR/MenuPointer.h"
#include "VR/VRInput.h"
#include "fmt/format.h"
#include <bit>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>
#endif
#include "vtlb.h"
#include "common/Error.h"
#include "common/FPControl.h"
#include "common/MemorySettingsInterface.h"
#include "common/SettingsInterface.h"
#include "common/SettingsWrapper.h"
#include "common/Threading.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <sys/types.h>
#include <thread>

#include "fmt/format.h"

#if defined(_WIN32)
#define read_portable(a, b, c) (recv(a, (char*)b, c, 0))
#define write_portable(a, b, c) (send(a, (const char*)b, c, 0))
#define safe_close_portable(a) \
	do \
	{ \
		if ((a) >= 0) \
		{ \
			closesocket((a)); \
			(a) = INVALID_SOCKET; \
		} \
	} while (0)
#include "common/RedtapeWindows.h"
#include <WinSock2.h>
#elif defined(__linux__) || defined(__FreeBSD__)
#define read_portable(a, b, c) (read(a, b, c))
#define write_portable(a, b, c) (send(a, b, c, MSG_NOSIGNAL))
#define safe_close_portable(a) \
	do \
	{ \
		if ((a) >= 0) \
		{ \
			close((a)); \
			(a) = -1; \
		} \
	} while (0)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#else
#define read_portable(a, b, c) (read(a, b, c))
#define write_portable(a, b, c) (write(a, b, c))
#define safe_close_portable(a) \
	do \
	{ \
		if ((a) >= 0) \
		{ \
			close((a)); \
			(a) = -1; \
		} \
	} while (0)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define PINE_EMULATOR_NAME "pcsx2"

// Which socket family PINE listens on is a property of the host, not of the protocol: the wire
// format below is identical either way, and reference clients already speak both because Windows
// has always needed TCP.
//
// Android joins Windows on the TCP side for a different reason. A Unix socket needs a path both
// sides can name, and Android gives us nowhere to put one: there is no /tmp, XDG_RUNTIME_DIR is
// unset, so the existing code would fall back to binding "/tmp/pcsx2.sock" and simply fail. The
// writable directories that do exist are all app-private, and every plausible client -- adb, a
// shell, another app -- runs under a different uid, so a socket placed there binds successfully
// and then admits nobody. Loopback TCP is the transport Android actually supports reaching into:
// "adb forward" bridges a device port to the workstation, which is what makes a handheld
// debuggable from a desk at all.
#if defined(_WIN32) || defined(__ANDROID__)
#define PINE_TCP_TRANSPORT 1
#endif

// sockaddr_in and htons()/htonl(). Windows already has them via WinSock2.h above; the POSIX
// spelling lives in headers the Unix-socket path never needed, so it is pulled in only here.
#if defined(PINE_TCP_TRANSPORT) && !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

// The TCP setup below is now shared, so its failure comparisons have to be too. Winsock spells
// these as named constants and POSIX returns -1 from both socket() and bind(); neither name
// exists on the other platform.
#ifdef _WIN32
#define PINE_INVALID_SOCKET INVALID_SOCKET
#define PINE_SOCKET_ERROR SOCKET_ERROR
#else
#define PINE_INVALID_SOCKET (-1)
#define PINE_SOCKET_ERROR (-1)
#endif

#ifdef _WIN32

static bool InitializeWinsock()
{
	static bool initialized = false;
	if (initialized)
		return true;

	WSADATA wsa = {};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;

	initialized = true;
	std::atexit([]() { WSACleanup(); });
	return true;
}

#endif

namespace PINEServer
{
	static std::thread s_thread;
	static int s_slot;

	// Two independent splits meet here, which is why the guards differ. The socket HANDLE type is
	// a Windows API question -- SOCKET is an opaque handle there, a file descriptor everywhere
	// else -- while the socket NAME only exists when the transport is a Unix socket, and Android
	// is POSIX with no socket name at all.
#ifdef _WIN32
	// windows claim to have support for AF_UNIX sockets but that is a blatant lie,
	// their SDK won't even run their own examples, so we go on TCP sockets.
	static SOCKET s_sock = INVALID_SOCKET;
	// the message socket used in thread's accept().
	static SOCKET s_msgsock = INVALID_SOCKET;
#else
	static int s_sock = -1;
	// the message socket used in thread's accept().
	static int s_msgsock = -1;
#endif

#ifndef PINE_TCP_TRANSPORT
	// absolute path of the socket. Stored in XDG_RUNTIME_DIR, if unset /tmp
	static std::string s_socket_name;
#endif

	// Whether the socket processing thread should stop executing/is stopped.
	static std::atomic_bool s_end{true};

	/**
	 * Maximum memory used by an IPC message request.
	 * Equivalent to 50,000 Write64 requests.
	 */
#define MAX_IPC_SIZE 650000

	/**
	 * Maximum memory used by an IPC message reply.
	 * Equivalent to 50,000 Read64 replies.
	 */
#define MAX_IPC_RETURN_SIZE 450000

	/**
	 * IPC return buffer.
	 * A preallocated buffer used to store all IPC replies.
	 * to the size of 50.000 MsgWrite64 IPC calls.
	 */
	static std::vector<u8> s_ret_buffer;

	/**
	 * IPC messages buffer.
	 * A preallocated buffer used to store all IPC messages.
	 */
	static std::vector<u8> s_ipc_buffer;

	/**
	 * IPC Command messages opcodes.
	 * A list of possible operations possible by the IPC.
	 * Each one of them is what we call an "opcode" and is the first
	 * byte sent by the IPC to differentiate between commands.
	 */
	enum IPCCommand : unsigned char
	{
		MsgRead8 = 0, /**< Read 8 bit value to memory. */
		MsgRead16 = 1, /**< Read 16 bit value to memory. */
		MsgRead32 = 2, /**< Read 32 bit value to memory. */
		MsgRead64 = 3, /**< Read 64 bit value to memory. */
		MsgWrite8 = 4, /**< Write 8 bit value to memory. */
		MsgWrite16 = 5, /**< Write 16 bit value to memory. */
		MsgWrite32 = 6, /**< Write 32 bit value to memory. */
		MsgWrite64 = 7, /**< Write 64 bit value to memory. */
		MsgVersion = 8, /**< Returns ARMSX2 version. */
		MsgSaveState = 9, /**< Saves a savestate. */
		MsgLoadState = 0xA, /**< Loads a savestate. */
		MsgTitle = 0xB, /**< Returns the game title. */
		MsgID = 0xC, /**< Returns the game ID. */
		MsgUUID = 0xD, /**< Returns the game UUID. */
		MsgGameVersion = 0xE, /**< Returns the game verion. */
		MsgStatus = 0xF, /**< Returns the emulator status. */

		// ARMSX2-local extensions. Upstream PINE stops at 0xF; these are private to
		// this fork, so a generic PINE client will simply never send them.
		MsgGetStats = 0x10, /**< Returns host-side performance statistics as JSON. */
		MsgGetSetting = 0x11, /**< Reads a setting by section/key. */
		MsgSetSetting = 0x12, /**< Writes a setting by section/key and applies it. */
		MsgFrameAdvance = 0x13, /**< Advances a paused VM by one frame. */
		MsgGSDump = 0x14, /**< Records a GS dump of the next N frames. */
		MsgGetEffectiveSetting = 0x15, /**< Reads what a setting is actually running as. */
		MsgPadSet = 0x30, /**< Sets one controller input (button or stick direction) to a 0..1 value. */
#ifdef ENABLE_OPENXR
		// Outbreak VR (PenguinScreen2): the headset-free test kit's commands.
		MsgVRQhistFlush = 0xE0, /**< Writes the live depth histogram (--qhist-live, not ported: refused). */
		MsgVRMemWatch = 0xE1, /**< Arms or clears a memory watch (never fires on the arm64 recompiler). */
		MsgVRMemWatchPoll = 0xE2, /**< Drains the memory watch hits. */
		MsgVRFakeInput = 0xE3, /**< Scripted VR controllers and head (PCSX2_VR_TEST_INPUT, offline only). */
		MsgVRTelemetry = 0xE4, /**< What the first-person camera saw on its last vsync. */
		MsgVRMenuPointer = 0xE5, /**< The menu pointer's and belt pouches' state, or a test hit. */
#endif

		MsgUnimplemented = 0xFF /**< Unimplemented IPC message. */
	};

	/**
	 * Emulator status enum.
	 * A list of possible emulator statuses.
	 */
	enum EmuStatus : uint32_t
	{
		Running = 0, /**< Game is running */
		Paused = 1, /**< Game is paused */
		Shutdown = 2 /**< Game is shutdown */
	};

	/**
	 * IPC message buffer.
	 * A list of all needed fields to store an IPC message.
	 */
	struct IPCBuffer
	{
		int size; /**< Size of the buffer. */
		std::vector<u8> buffer; /**< Buffer. */
	};

	/**
	 * IPC result codes.
	 * A list of possible result codes the IPC can send back.
	 * Each one of them is what we call an "opcode" or "tag" and is the
	 * first byte sent by the IPC to differentiate between results.
	 */
	enum IPCResult : unsigned char
	{
		IPC_OK = 0, /**< IPC command successfully completed. */
		IPC_FAIL = 0xFF /**< IPC command failed to complete. */
	};

	// Thread used to relay IPC commands.
	void MainLoop();
	void ClientLoop();

	/**
	 * Internal function, Parses an IPC command.
	 * buf: buffer containing the IPC command.
	 * buf_size: size of the buffer announced.
	 * ret_buffer: buffer that will be used to send the reply.
	 * return value: IPCBuffer containing a buffer with the result
	 *               of the command and its size.
	 */
	static IPCBuffer ParseCommand(std::span<u8> buf, std::vector<u8>& ret_buffer, u32 buf_size);

	/**
	 * Formats an IPC buffer
	 * ret_buffer: return buffer to use.
	 * size: size of the IPC buffer.
	 * return value: buffer containing the status code allocated of size
	 */
	static std::vector<u8>& MakeOkIPC(std::vector<u8>& ret_buffer, uint32_t size);
	static std::vector<u8>& MakeFailIPC(std::vector<u8>& ret_buffer, uint32_t size);

	/**
	 * Initializes an open socket for IPC communication.
	 */
	bool AcceptClient();

	/**
	 * Converts a primitive value to bytes in little endian
	 * res_vector: the vector to modify
	 * res: the value to convert
	 * i: where to insert it into the vector
	 * NB: implicitely inlined
	 */
	template <typename T>
	static void ToResultVector(std::vector<u8>& res_vector, T res, int i)
	{
		memcpy(&res_vector[i], (char*)&res, sizeof(T));
	}

	/**
	 * Converts bytes in little endian to a primitive value
	 * span: the span to convert
	 * i: where to load it from the span
	 * return value: the converted value
	 * NB: implicitely inlined
	 */
	template <typename T>
	static T FromSpan(std::span<u8> span, int i)
	{
		return *(T*)(&span[i]);
	}

	/**
	 * Ensures an IPC message isn't too big.
	 * return value: false if checks failed, true otherwise.
	 */
	static inline bool SafetyChecks(u32 command_len, int command_size, u32 reply_len, int reply_size = 0, u32 buf_size = MAX_IPC_SIZE - 1)
	{
		return !((command_len + command_size) > buf_size ||
				 (reply_len + reply_size) >= MAX_IPC_RETURN_SIZE);
	}

#ifdef ENABLE_OPENXR
	// MsgVRFakeInput fields, read one at a time at fixed little-endian offsets (never a struct copy).
	static float ReadF32(std::span<u8> span, u32& at)
	{
		const float v = std::bit_cast<float>(FromSpan<u32>(span, at));
		at += 4;
		return v;
	}

	// Quaternion xyzw, then position xyz.
	static void ReadPose(std::span<u8> span, u32& at, VR::VRPose* out)
	{
		for (float& c : out->orientation_xyzw)
			c = ReadF32(span, at);
		for (float& c : out->position_xyz)
			c = ReadF32(span, at);
	}
#endif

	/**
	 * Reads a length-prefixed ([u32 len][len bytes], no NUL) string argument, advancing
	 * buf_cnt past it. Returns false if the declared length runs off the end of the
	 * request, in which case the caller must fail the command.
	 */
	static bool ReadLengthPrefixedString(std::span<u8> buf, u32& buf_cnt, u32 buf_size, std::string* out)
	{
		if ((buf_cnt + 4) > buf_size)
			return false;

		const u32 len = FromSpan<u32>(buf, buf_cnt);
		buf_cnt += 4;

		// Bound the allocation by what the request could actually contain.
		if (len > buf_size || (buf_cnt + len) > buf_size)
			return false;

		out->assign(reinterpret_cast<const char*>(&buf[buf_cnt]), len);
		buf_cnt += len;
		return true;
	}

	/**
	 * Builds the host-side statistics document. Called on the PINE thread.
	 *
	 * PerformanceMetrics and g_perfmon are plain scalar reads -- racy against the GS
	 * thread but benign, since every field is an independently-meaningful number and a
	 * torn sample only costs one stale stat. GSgetStats/GSgetMemoryStats are NOT safe
	 * here: they dereference g_texture_cache and g_gs_device, which are GS-thread
	 * owned, so the texture-cache memory figures are gathered via RunOnGSThread.
	 */
	static std::string BuildStatsJson()
	{
		SmallString gs_memory;
		// Device identity is the axis nearly every mobile GPU bug turns on, so a stats
		// blob without it is not attributable to a driver. Adreno tiers run drivers
		// (Mesa Turnip vs Qualcomm proprietary) that behave oppositely for fbfetch and
		// push descriptors, so the driver string matters more than the device name.
		std::string device_name, driver_info;
		if (MTGS::IsOpen())
		{
			// The MTGS ring is single-producer: m_WritePos is owned by the EE/CPU
			// thread, so only it may push packets. RunOnGSThread pushes a packet, and
			// WaitGS drains the ring -- calling them from the PINE thread makes a second
			// producer that races the EE thread's own ring writes, desyncing the
			// pending-packet count. That lost-wakeup-deadlocks the EE thread (parked in
			// WaitGS waiting for empty) against the GS thread (asleep in WaitForWork on
			// no work). Marshal onto the CPU thread first -- the legitimate producer --
			// and block until the GS-owned sample has been gathered.
			Host::RunOnCPUThread(
				[&gs_memory, &device_name, &driver_info]() {
					MTGS::RunOnGSThread([&gs_memory, &device_name, &driver_info]() {
						GSgetMemoryStats(gs_memory);
						if (g_gs_device)
						{
							device_name = g_gs_device->GetName();
							driver_info = g_gs_device->GetDriverInfo();
						}
					});
					MTGS::WaitGS(false);
				},
				true);
		}
		// Newlines and quotes would break the JSON; GetDriverInfo() is multi-line on Vulkan.
		const auto sanitize = [](std::string& s) {
			for (char& c : s)
			{
				if (c == '\n' || c == '\r' || c == '\t')
					c = ' ';
				else if (c == '"' || c == '\\')
					c = '\'';
			}
		};
		sanitize(device_name);
		sanitize(driver_info);

		const auto counter = [](GSPerfMon::counter_t c) { return g_perfmon.Get(c); };

		return fmt::format(
			"{{"
			"\"fps\":{:.3f},\"internal_fps\":{:.3f},\"speed\":{:.3f},"
			"\"frame_ms_avg\":{:.3f},\"frame_ms_min\":{:.3f},\"frame_ms_max\":{:.3f},"
			"\"cpu_thread_pct\":{:.3f},\"cpu_thread_ms\":{:.3f},"
			"\"gs_thread_pct\":{:.3f},\"gs_thread_ms\":{:.3f},"
			"\"gs_back_thread_pct\":{:.3f},\"gs_back_thread_ms\":{:.3f},"
			"\"vu_thread_pct\":{:.3f},\"vu_thread_ms\":{:.3f},"
			"\"gpu_pct\":{:.3f},\"gpu_ms_avg\":{:.3f},\"gpu_ms_last\":{:.3f},"
			"\"gpu_vs_invocations\":{:.0f},\"gpu_ps_invocations\":{:.0f},"
			"\"prims\":{:.1f},\"draws\":{:.1f},\"draw_calls\":{:.1f},"
			"\"render_passes\":{:.1f},\"barriers\":{:.1f},\"readbacks\":{:.1f},"
			"\"texture_copies\":{:.1f},\"texture_uploads\":{:.1f},"
			"\"draw_calls_rov\":{:.1f},\"barriers_rov\":{:.1f},\"texture_copies_rov\":{:.1f},"
			"\"tc_source_hit\":{:.1f},\"tc_source_miss\":{:.1f},"
			"\"tc_target_hit\":{:.1f},\"tc_target_miss\":{:.1f},"
			"\"hash_cache_hit\":{:.1f},\"hash_cache_miss\":{:.1f},"
			"\"gs_memory\":\"{}\",\"frame_number\":{},\"gs_front_parser\":{},"
			"\"renderer\":\"{}\",\"device_name\":\"{}\",\"driver_info\":\"{}\""
			"}}",
			PerformanceMetrics::GetFPS(), PerformanceMetrics::GetInternalFPS(), PerformanceMetrics::GetSpeed(),
			PerformanceMetrics::GetAverageFrameTime(), PerformanceMetrics::GetMinimumFrameTime(),
			PerformanceMetrics::GetMaximumFrameTime(),
			PerformanceMetrics::GetCPUThreadUsage(), PerformanceMetrics::GetCPUThreadAverageTime(),
			PerformanceMetrics::GetGSThreadUsage(), PerformanceMetrics::GetGSThreadAverageTime(),
			// Zero unless GS multi-threading is on. gs_thread_* is the MTGS thread only,
			// so under the split the two have to be read together to see the GS cost.
			PerformanceMetrics::GetGSBackThreadUsage(), PerformanceMetrics::GetGSBackThreadAverageTime(),
			PerformanceMetrics::GetVUThreadUsage(), PerformanceMetrics::GetVUThreadAverageTime(),
			PerformanceMetrics::GetGPUUsage(), PerformanceMetrics::GetGPUAverageTime(),
			PerformanceMetrics::GetLastGPUTime(),
			PerformanceMetrics::GetGPUAverageVSInvocations(), PerformanceMetrics::GetGPUAveragePSInvocations(),
			counter(GSPerfMon::Prim), counter(GSPerfMon::Draw), counter(GSPerfMon::DrawCalls),
			counter(GSPerfMon::RenderPasses), counter(GSPerfMon::Barriers), counter(GSPerfMon::Readbacks),
			counter(GSPerfMon::TextureCopies), counter(GSPerfMon::TextureUploads),
			counter(GSPerfMon::DrawCallsROV), counter(GSPerfMon::BarriersROV), counter(GSPerfMon::TextureCopiesROV),
			counter(GSPerfMon::TCSourceHit), counter(GSPerfMon::TCSourceMiss),
			counter(GSPerfMon::TCTargetHit), counter(GSPerfMon::TCTargetMiss),
			counter(GSPerfMon::HashCacheHit), counter(GSPerfMon::HashCacheMiss),
			// Whether the two-object split actually engaged, which the BackThreadMode setting
			// alone does not tell you -- it downgrades to lockstep on an unsupported config.
			// True is also what makes gs_back_thread_* worth reading next to gs_thread_*.
			gs_memory.view(), PerformanceMetrics::GetFrameNumber(), GSHasFrontParser() ? "true" : "false",
			Pcsx2Config::GSOptions::GetRendererName(EmuConfig.GS.Renderer), device_name, driver_info);
	}

	/**
	 * Escapes a string for embedding in a JSON string literal. Only paths go through this
	 * -- a Windows path is full of backslashes, which would otherwise leave the reply
	 * unparseable. BuildStatsJson's sanitizer is a different job: it flattens driver blurb
	 * that is allowed to lose fidelity, whereas a path the client is about to open is not.
	 */
	static std::string JsonEscape(const std::string_view str)
	{
		std::string out;
		out.reserve(str.size());
		for (const char c : str)
		{
			if (c == '"' || c == '\\')
				out.push_back('\\');
			out.push_back(c);
		}
		return out;
	}

	/**
	 * Reports what a setting is actually running as, which on any game carrying GameDB fixes is
	 * NOT what the INI says: applyGSHardwareFixes runs after the settings load and rewrites
	 * EmuConfig in memory without ever touching the file. Reading the persisted value alone gets
	 * you told that autoflush is off while the renderer is running it at 2.
	 *
	 * The measurement hazard is worse than the confusion. A settings A/B that writes a key to
	 * "off" measures the GameDB value in BOTH arms -- because GameDB re-applies it after every
	 * settings load -- while the two arms report different settings. That is a wrong answer with
	 * no symptom, on exactly the titles worth investigating.
	 *
	 * Effective values come from serialising the live EmuConfig back out through the same wrapper
	 * that writes the INI, so they land under the identical section/key names the caller already
	 * uses, and every setting is covered for free. A hand-written key map would need extending by
	 * every future setting, and the one that got missed would be the one somebody trusted.
	 *
	 * `known` false means the key is not part of Pcsx2Config at all -- EnableFastBoot, the UI
	 * section, anything host-side. That is not an error: it says the persisted value is the whole
	 * truth for that key, which is worth telling apart from "both agree".
	 *
	 * `differs` is deliberately a claim about the two STRINGS and not about the cause. Something
	 * mutated the config after it was loaded -- a GameDB fix, safe-mode masking, or a settings
	 * layer this query does not read -- and which one it was is not knowable from here. Naming it
	 * "overridden" would be inventing the reason.
	 */
	static std::string BuildEffectiveSettingJson(const std::string& section, const std::string& key)
	{
		MemorySettingsInterface effective_si;
		{
			// Serialise a copy, so nothing the wrapper does can reach the live config. The FPCR
			// backup matches every other Pcsx2Config round-trip in the tree: the struct carries FP
			// control state, and this runs on the PINE thread, whose FPCR is its own to restore.
			FPControlRegisterBackup fpcr_backup(FPControlRegister::GetDefault());
			Pcsx2Config snapshot = EmuConfig;
			SettingsSaveWrapper wrapper(effective_si);
			snapshot.LoadSave(wrapper);
		}

		std::string effective;
		const bool known = effective_si.GetStringValue(section.c_str(), key.c_str(), &effective);

		std::string persisted;
		{
			auto lock = Host::GetSettingsLock();
			persisted = Host::GetSettingsInterface()->GetStringValue(section.c_str(), key.c_str(), "");
		}

		// An unpersisted key reads back empty, which means "never written", not "set to empty".
		// Counting that as a difference would flag most of the config the moment anyone asked,
		// since the INI only stores what has been changed away from its default.
		const bool differs = known && !persisted.empty() && persisted != effective;

		return fmt::format(
			"{{\"section\":\"{}\",\"key\":\"{}\",\"effective\":\"{}\",\"persisted\":\"{}\","
			"\"known\":{},\"differs\":{}}}",
			JsonEscape(section), JsonEscape(key), JsonEscape(effective), JsonEscape(persisted),
			known ? "true" : "false", differs ? "true" : "false");
	}

	/**
	 * Queues a GS dump of the next `frames` frames, or stops a dump already recording when
	 * `frames` is zero -- the same pair of actions the GSDumpMultiFrame hotkey binds to press
	 * and release. `path` may be empty for the usual auto-named file under the snapshots
	 * folder. Returns false only if there is no GS to dump, which fails the command.
	 *
	 * Marshalled the same way as BuildStatsJson and for the same reason: the MTGS ring is
	 * single-producer and the PINE thread is not that producer, so pushing a packet from
	 * here races the EE thread's own writes and can deadlock the two of them. Hop to the CPU
	 * thread, push from there, and block until the GS thread has taken the request.
	 *
	 * The reply describes the request, not a finished file. The dump is opened on the next
	 * VSync and a multi-frame dump is closed some frames after that, so a client that waits
	 * for the file has to keep the VM running or step it with MsgFrameAdvance -- a paused VM
	 * never reaches the VSync that writes anything. A screenshot lands next to the dump too;
	 * that is inherent to the snapshot path, not something this command adds.
	 *
	 * `queued` false carries a `reason`, because both ways a request can be turned away are
	 * served commands that quietly did nothing rather than socket-level failures. The reasons
	 * are not symmetric: "snapshot pending" is a request that arrived between another one and
	 * the VSync servicing it, and retrying a frame later works. "already recording" is a
	 * refusal -- the snapshot path creates a dump only when none exists, so a second request
	 * would take a screenshot and write no dump. Handing back a path for a file that will never
	 * appear is the worst of the available answers, so stop the running dump first if you mean
	 * to. (Refusing here also predates the fix for the truncation that used to follow: the
	 * request and the recording shared a frame counter, so a second one cut the first short.
	 * They are separate fields now -- see GSSnapshotPolicy.h -- and the refusal stands on the
	 * unanswerable-path argument alone.)
	 */
	static bool QueueGSDump(u32 frames, std::string path, std::string* reply)
	{
		if (!MTGS::IsOpen())
			return false;

		// QueueSnapshot honours a caller-supplied path only when it ends in .png, which it
		// strips to get the base name shared by the screenshot and the dump; anything else is
		// silently discarded in favour of an auto-named file in the snapshots folder. Silence
		// is the wrong failure for a scripted client -- it writes somewhere the script never
		// looks -- so meet that contract here instead. Drop a dump or image suffix if the
		// caller spelled one out, so naming the file you want does not earn you a doubled
		// extension, and let what is left be the base name.
		if (!path.empty())
		{
			for (const std::string_view suffix : {".gs.zst", ".gs.xz", ".gs", ".png"})
			{
				if (StringUtil::EndsWithNoCase(path, suffix))
				{
					path.erase(path.size() - suffix.size());
					break;
				}
			}
		}

		bool queued = false, stopped = false;
		std::string base;
		const char* dump_ext = "";
		const char* reason = "";

		Host::RunOnCPUThread(
			[&]() {
				MTGS::RunOnGSThread([&]() {
					if (frames == 0)
					{
						GSStopGSDump();
						stopped = true;
						return;
					}

					if (GSIsDumpRecording())
					{
						reason = "already recording";
						return;
					}

					// Resolve the auto-name here rather than leaving it to QueueSnapshot, so the
					// reply can name the exact file the client is about to wait for.
					base = path.empty() ? GSGetBaseSnapshotFilename() : path;
					queued = GSQueueSnapshot(base + ".png", frames);
					if (!queued)
					{
						reason = "snapshot pending";
						return;
					}

					// GSConfig is the GS thread's copy of the config, so read the compression
					// method here -- it decides the extension the dump writer will append.
					switch (GSConfig.GSDumpCompression)
					{
						case GSDumpCompressionMethod::Uncompressed:
							dump_ext = ".gs";
							break;
						case GSDumpCompressionMethod::LZMA:
							dump_ext = ".gs.xz";
							break;
						default:
							dump_ext = ".gs.zst";
							break;
					}
				});
				MTGS::WaitGS(false);
			},
			true);

		*reply = fmt::format(
			"{{\"queued\":{},\"stopped\":{},\"frames\":{},\"path\":\"{}\",\"reason\":\"{}\"}}",
			queued ? "true" : "false", stopped ? "true" : "false", frames,
			queued ? JsonEscape(base + dump_ext) : std::string(), reason);
		return true;
	}
} // namespace PINEServer

bool PINEServer::Initialize(int slot)
{
	s_end.store(false, std::memory_order_release);
	s_slot = slot;

#ifdef _WIN32
	if (!InitializeWinsock())
	{
		Console.WriteLn(Color_Red, "PINE: Cannot initialize winsock! Shutting down...");
		Deinitialize();
		return false;
	}
#endif

#ifdef PINE_TCP_TRANSPORT
	s_sock = socket(AF_INET, SOCK_STREAM, 0);
	if ((s_sock == PINE_INVALID_SOCKET) || slot > 65536)
	{
		Console.WriteLn(Color_Red, "PINE: Cannot open socket! Shutting down...");
		Deinitialize();
		return false;
	}

	// A listener that survives its own process is worse than one that fails to start: the port
	// sits in TIME_WAIT after a crash or a fast restart, bind() refuses, and PINE looks broken
	// for a minute or so for reasons nothing reports. This matters far more on a handheld than
	// on a desktop, because killing and relaunching the app is the normal debugging loop there.
#ifndef _WIN32
	const int reuse = 1;
	setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

	sockaddr_in server = {};
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
	server.sin_port = htons(slot);

	if (bind(s_sock, (struct sockaddr*)&server, sizeof(server)) == PINE_SOCKET_ERROR)
	{
		Console.WriteLn(Color_Red, "PINE: Error while binding to socket! Shutting down...");
		Deinitialize();
		return false;
	}

	// Loopback is not reachable from another machine, so on a handheld the port is useless until
	// something bridges it. Name the bridge in the log rather than leaving the reader to work it
	// out: on Android this line is the entire setup instruction for driving the device from a
	// workstation. Desktop hosts get the address only -- there the client is already local, and
	// telling a Windows user to run adb would just be wrong.
#ifdef __ANDROID__
	Console.WriteLnFmt("PINE: Listening on 127.0.0.1:{} -- bridge it with: adb forward tcp:{} tcp:{}",
		slot, slot, slot);
#else
	Console.WriteLnFmt("PINE: Listening on 127.0.0.1:{}.", slot);
#endif

#else
	char* runtime_dir = nullptr;
#ifdef __APPLE__
	runtime_dir = std::getenv("TMPDIR");
#else
	runtime_dir = std::getenv("XDG_RUNTIME_DIR");
#endif
	// fallback in case macOS or other OSes don't implement the XDG base
	// spec
	if (runtime_dir == nullptr)
		s_socket_name = "/tmp/" PINE_EMULATOR_NAME ".sock";
	else
	{
		s_socket_name = runtime_dir;
		s_socket_name += "/" PINE_EMULATOR_NAME ".sock";
	}

	if (slot != PINE_DEFAULT_SLOT)
		s_socket_name += "." + std::to_string(slot);

	struct sockaddr_un server;

	s_sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s_sock < 0)
	{
		Console.WriteLn(Color_Red, "PINE: Cannot open socket! Shutting down...");
		Deinitialize();
		return false;
	}
	server.sun_family = AF_UNIX;
	StringUtil::Strlcpy(server.sun_path, s_socket_name, sizeof(server.sun_path));

	// we unlink the socket so that when releasing this thread the socket gets
	// freed even if we didn't close correctly the loop
	unlink(s_socket_name.c_str());
	if (bind(s_sock, (struct sockaddr*)&server, sizeof(struct sockaddr_un)))
	{
		Console.WriteLn(Color_Red, "PINE: Error while binding to socket! Shutting down...");
		Deinitialize();
		return false;
	}
#ifdef ENABLE_OPENXR
	// Outbreak VR (Steam Frame): where the research and test tools (tools/outbreak-vr/pine.py) find PINE.
	Console.WriteLnFmt("PINE: Listening on {}.", s_socket_name);
#endif
#endif

	// maximum queue of 4096 commands before refusing, approximated to the
	// nearest legal value. We do not use SOMAXCONN as windows have this idea
	// that a "reasonable" value is 5, which is not.
	if (listen(s_sock, 4096))
	{
		Console.WriteLn(Color_Red, "PINE: Cannot listen for connections! Shutting down...");
		Deinitialize();
		return false;
	}

	// we allocate once buffers to not have to do mallocs for each IPC
	// request, as malloc is expansive when we optimize for µs.
	s_ret_buffer.resize(MAX_IPC_RETURN_SIZE);
	s_ipc_buffer.resize(MAX_IPC_SIZE);

	// we start the thread
	s_thread = std::thread(&PINEServer::MainLoop);

	return true;
}

bool PINEServer::IsInitialized()
{
	return !s_end.load(std::memory_order_acquire);
}

int PINEServer::GetSlot()
{
	return s_slot;
}

std::vector<u8>& PINEServer::MakeOkIPC(std::vector<u8>& ret_buffer, uint32_t size = 5)
{
	ToResultVector<uint32_t>(ret_buffer, size, 0);
	ret_buffer[4] = IPC_OK;
	return ret_buffer;
}

std::vector<u8>& PINEServer::MakeFailIPC(std::vector<u8>& ret_buffer, uint32_t size = 5)
{
	ToResultVector<uint32_t>(ret_buffer, size, 0);
	ret_buffer[4] = IPC_FAIL;
	return ret_buffer;
}

bool PINEServer::AcceptClient()
{
	s_msgsock = accept(s_sock, 0, 0);
	if (s_msgsock < 0)
	{
		// everything else is non recoverable in our scope
		// we also mark as recoverable socket errors where it would block a
		// non blocking socket, even though our socket is blocking, in case
		// we ever have to implement a non blocking socket.
#ifdef _WIN32
		const int errno_w = WSAGetLastError();
		if (!(errno_w == WSAECONNRESET || errno_w == WSAEINTR || errno_w == WSAEINPROGRESS || errno_w == WSAEMFILE || errno_w == WSAEWOULDBLOCK) && s_sock != INVALID_SOCKET)
			Console.Error("PINE: accept() returned error %d", errno_w);
#else
		if (!(errno == ECONNABORTED || errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) && s_sock >= 0)
			Console.Error("PINE: accept() returned error %d", errno);
#endif

		return false;
	}

#ifdef __APPLE__
	int nosigpipe = 1;
	setsockopt(s_msgsock, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

	// Gross C-style cast, but SOCKET is a handle on Windows.
	Console.WriteLn("PINE: New client with FD %d connected.", (int)s_msgsock);
	return true;
}

void PINEServer::MainLoop()
{
	Threading::SetNameOfCurrentThread("PINE Server");

	while (!s_end.load(std::memory_order_acquire))
	{
		if (!AcceptClient())
			continue;

		ClientLoop();

		Console.WriteLn("PINE: Client disconnected.");
		safe_close_portable(s_msgsock);
	}
}

void PINEServer::ClientLoop()
{
	while (!s_end.load(std::memory_order_acquire))
	{
		// either int or ssize_t depending on the platform, so we have to
		// use a bunch of auto
		auto receive_length = 0;
		auto end_length = 4;
		const std::span<u8> ipc_buffer_span(s_ipc_buffer);

		// while we haven't received the entire packet, maybe due to
		// socket datagram splittage, we continue to read
		while (receive_length < end_length)
		{
			const auto tmp_length = read_portable(s_msgsock, &ipc_buffer_span[receive_length], MAX_IPC_SIZE - receive_length);

			// we recreate the socket if an error happens
			if (tmp_length <= 0)
				return;

			receive_length += tmp_length;

			// if we got at least the final size then update
			if (end_length == 4 && receive_length >= 4)
			{
				end_length = FromSpan<u32>(ipc_buffer_span, 0);
				// we'd like to avoid a client trying to do OOB
				if (end_length > MAX_IPC_SIZE || end_length < 4)
				{
					receive_length = 0;
					break;
				}
			}
		}
		PINEServer::IPCBuffer res;

		// we remove 4 bytes to get the message size out of the IPC command
		// size in ParseCommand.
		// also, if we got a failed command, let's reset the state so we don't
		// end up deadlocking by getting out of sync, eg when a client
		// disconnects
		if (receive_length != 0)
		{
			res = ParseCommand(ipc_buffer_span.subspan(4), s_ret_buffer, (u32)end_length - 4);

			// if we cannot send back our answer restart the socket
			if (write_portable(s_msgsock, res.buffer.data(), res.size) < 0)
				return;
		}
	}
}

void PINEServer::Deinitialize()
{
	s_end.store(true, std::memory_order_release);

	// Removing the socket file is about the Unix-socket transport, not about being POSIX: a TCP
	// listener has no filesystem entry to clean up, and Android has no s_socket_name to read.
#ifndef PINE_TCP_TRANSPORT
	if (!s_socket_name.empty())
	{
		unlink(s_socket_name.c_str());
		s_socket_name = {};
	}
#endif

	// shutdown() is needed, otherwise accept() will still block.
#ifdef _WIN32
	if (s_sock != INVALID_SOCKET)
		shutdown(s_sock, SD_BOTH);
#else
	if (s_sock >= 0)
		shutdown(s_sock, SHUT_RDWR);
#endif

	safe_close_portable(s_sock);
	safe_close_portable(s_msgsock);

	if (s_thread.joinable())
		s_thread.join();
}

PINEServer::IPCBuffer PINEServer::ParseCommand(std::span<u8> buf, std::vector<u8>& ret_buffer, u32 buf_size)
{
	u32 ret_cnt = 5;
	u32 buf_cnt = 0;

	while (buf_cnt < buf_size)
	{
		if (!SafetyChecks(buf_cnt, 1, ret_cnt, 0, buf_size)) [[unlikely]]
			return IPCBuffer{5, MakeFailIPC(ret_buffer)};
		buf_cnt++;
		// example IPC messages: MsgRead/Write
		// refer to the client doc for more info on the format
		//         IPC Message event (1 byte)
		//         |  Memory address (4 byte)
		//         |  |           argument (VLE)
		//         |  |           |
		// format: XX YY YY YY YY ZZ ZZ ZZ ZZ
		//        reply code: 00 = OK, FF = NOT OK
		//        |  return value (VLE)
		//        |  |
		// reply: XX ZZ ZZ ZZ ZZ
		switch ((IPCCommand)buf[buf_cnt - 1])
		{
			case MsgRead8:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 1, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u8 res = vtlb_ramRead<mem8_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 1;
				buf_cnt += 4;
				break;
			}
			case MsgRead16:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 2, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u16 res = vtlb_ramRead<mem16_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 2;
				buf_cnt += 4;
				break;
			}
			case MsgRead32:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 4, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u32 res = vtlb_ramRead<mem32_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 4;
				buf_cnt += 4;
				break;
			}
			case MsgRead64:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 8, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				const u64 res = vtlb_ramRead<mem64_t>(a);
				ToResultVector(ret_buffer, res, ret_cnt);
				ret_cnt += 8;
				buf_cnt += 4;
				break;
			}
			case MsgWrite8:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 1 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem8_t>(a, FromSpan<u8>(buf, buf_cnt + 4));
				buf_cnt += 5;
				break;
			}
			case MsgWrite16:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 2 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem16_t>(a, FromSpan<u16>(buf, buf_cnt + 4));
				buf_cnt += 6;
				break;
			}
			case MsgWrite32:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem32_t>(a, FromSpan<u32>(buf, buf_cnt + 4));
				buf_cnt += 8;
				break;
			}
			case MsgWrite64:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 8 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 a = FromSpan<u32>(buf, buf_cnt);
				vtlb_ramWrite<mem64_t>(a, FromSpan<u64>(buf, buf_cnt + 4));
				buf_cnt += 12;
				break;
			}
#ifdef ENABLE_OPENXR
			case MsgVRQhistFlush:
			{
				if (!VMManager::HasValidVM() || !VR::DepthHistogramArmed() || VR::QhistLiveDir().empty())
				{
					Console.Warning("(PINE) MsgVRQhistFlush refused: %s",
						!VMManager::HasValidVM() ? "no VM" :
							(!VR::DepthHistogramArmed() ? "histogram not armed (launch with --qhist-live <dir>)" :
															"no live dir"));
					goto error;
				}
				struct FlushState
				{
					std::mutex m;
					std::condition_variable cv;
					bool done = false;
					bool ok = false;
					std::string path;
					std::string err;
				};
				auto st = std::make_shared<FlushState>();
				Host::RunOnCPUThread([st]() {
					MTGS::RunOnGSThread([st]() {
					static std::atomic<u32> s_seq{0};
					VR::DepthHistogram& h = VR::GlobalDepthHistogram();
					h.key.serial = VMManager::GetDiscSerial();
					h.key.crc = fmt::format("0x{:08x}", VMManager::GetDiscCRC());
					h.key.dump = std::string();
					h.key.frame = g_FrameCount;
					h.key.widescreen_hack = EmuConfig.EnableWideScreenPatches;
					const u32 seq = s_seq.fetch_add(1) + 1;
					const std::string path = fmt::format("{}/qhist-live-{}-{:03d}-f{}.json",
						VR::QhistLiveDir(), h.key.serial.empty() ? "unknown" : h.key.serial, seq, h.key.frame);
					std::string err;
					const bool ok = h.WriteJson(path, &err);
					if (ok)
						h.Reset();
					std::lock_guard<std::mutex> lock(st->m);
					st->ok = ok;
					st->path = path;
					st->err = err;
					st->done = true;
					st->cv.notify_one();
					});
				});
				{
					std::unique_lock<std::mutex> lock(st->m);
					if (!st->cv.wait_for(lock, std::chrono::seconds(10), [&st] { return st->done; }))
					{
						Console.Warning("(PINE) MsgVRQhistFlush: GS thread did not service the flush in 10s (VM paused?)");
						goto error;
					}
					if (!st->ok)
					{
						Console.ErrorFmt("(PINE) MsgVRQhistFlush: {}", st->err);
						goto error;
					}
				}
				const u32 size = static_cast<u32>(st->path.size()) + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], st->path.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgVRMemWatch:
			{
				if (!VMManager::HasValidVM())
					goto error;
				// Online (network adapter on) a state save/load or a MemWatch pause would stall or desync the room.
				if (EmuConfig.DEV9.EthEnable)
				{
					Console.Warning("(PINE) %s refused: the network adapter is on.", "MsgVRMemWatch");
					goto error;
				}

				if (!SafetyChecks(buf_cnt, 4 + 3, ret_cnt, 1, buf_size)) [[unlikely]]
					goto error;
				const u32 watch_addr = FromSpan<u32>(buf, buf_cnt);
				const u8 watch_size = FromSpan<u8>(buf, buf_cnt + 4);
				const u8 watch_cond = FromSpan<u8>(buf, buf_cnt + 5);
				const u8 watch_stop = FromSpan<u8>(buf, buf_cnt + 6);
				buf_cnt += 7;
				// ARMSX2: no EE recompiler here has PenguinScreen2's MemWatch hit hook (it is in PenguinScreen2's x86 iR5900.cpp,
				// and the Frame runs the arm64 recompiler, which has no memcheck at all), so a watch armed here never reports a hit.
				static bool s_memwatch_arm64_noted = false;
				if (!std::exchange(s_memwatch_arm64_noted, true))
					Console.Warning("(PINE) MsgVRMemWatch: ARMSX2 has no MemWatch hit hook in its EE recompilers, so watches never report a hit.");

				struct ArmState
				{
					std::mutex m;
					std::condition_variable cv;
					bool done = false;
					int id = -1;
				};
				auto arm = std::make_shared<ArmState>();
				Host::RunOnCPUThread([arm, watch_addr, watch_size, watch_cond, watch_stop]() {
					int id;
					if (watch_size == 0)
					{
						VR::MemWatchClearAll();
						id = -2;
					}
					else
					{
						id = VR::MemWatchArm(watch_addr, watch_size, watch_cond, watch_stop != 0);
					}
					std::lock_guard<std::mutex> lock(arm->m);
					arm->id = id;
					arm->done = true;
					arm->cv.notify_one();
				});
				{
					std::unique_lock<std::mutex> lock(arm->m);
					if (!arm->cv.wait_for(lock, std::chrono::seconds(10), [&arm] { return arm->done; }))
					{
						Console.Warning("(PINE) MsgVRMemWatch: the CPU thread did not service the request in 10s (VM shut down?)");
						goto error;
					}
					if (arm->id == -1)
					{
						Console.Warning("(PINE) MsgVRMemWatch refused: bad size/cond, or no free watch slot");
						goto error;
					}
					const u8 watch_id = (arm->id < 0) ? 0xFF : static_cast<u8>(arm->id);
					ToResultVector(ret_buffer, watch_id, ret_cnt);
				}
				ret_cnt += 1;
				break;
			}
			case MsgVRMemWatchPoll:
			{
				u32 dropped = 0;
				const std::vector<VR::MemWatchHit> hits = VR::MemWatchDrain(&dropped);
				const std::vector<u8> payload = VR::MemWatchTable::Serialize(hits, dropped);
				const u32 size = static_cast<u32>(payload.size());
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], payload.data(), size);
				ret_cnt += size;
				break;
			}
			case MsgVRFakeInput:
			{
				// Scripted VR controllers for the headset-free test runner (tools/outbreak-vr/vrtest.py).
				// Version 1, little-endian:
				//   u8 ver (1), u8 flags (bit0 head valid, bit1 recenter after applying), u16 ttl_ms
				//   head: 7 x f32 (quaternion xyzw, position xyz)
				//   2 x hand (left, right): u8 flags (bit0 aim valid, bit1 grip valid, bit2 head-relative,
				//     bit3 supplied: replace this hand), aim pose 7 x f32, grip pose 7 x f32,
				//     trigger, grip, stick x, stick y (4 x f32), u32 buttons (VRHandState order from bit0)
				// Reply: u8 1. Only with PCSX2_VR_TEST_INPUT set, and never while online play is possible:
				// injected input must not reach other players.
				constexpr int kPoseSize = 7 * 4;
				constexpr int kHandSize = 1 + 2 * kPoseSize + 4 * 4 + 4;
				constexpr int kFakeInputSize = 4 + kPoseSize + 2 * kHandSize;
				if (!SafetyChecks(buf_cnt, kFakeInputSize, ret_cnt, 1, buf_size)) [[unlikely]]
					goto error;
				const char* refusal = nullptr;
				if (!std::getenv("PCSX2_VR_TEST_INPUT"))
					refusal = "PCSX2_VR_TEST_INPUT is not set";
				else if (EmuConfig.VR.OnlineSafe || EmuConfig.DEV9.EthEnable)
					refusal = "online-safe mode or the network adapter is on";
				else if (FromSpan<u8>(buf, buf_cnt) != 1)
					refusal = "unknown version";
				if (refusal)
				{
					// A runner re-sends at 20 Hz; one line per hundred refusals is loud enough.
					static u32 s_fake_input_refusals = 0;
					if ((s_fake_input_refusals++ % 100) == 0)
						Console.Warning("(PINE) MsgVRFakeInput refused: %s.", refusal);
					goto error;
				}

				VR::XRInput::TestInput input;
				u32 at = buf_cnt;
				const u8 flags = FromSpan<u8>(buf, at + 1);
				input.ttl_ms = FromSpan<u16>(buf, at + 2);
				at += 4;
				ReadPose(buf, at, &input.head_pose);
				input.head_pose.valid = (flags & 0x01) != 0;
				for (VR::XRInput::TestInput::Hand& hand : input.hands)
				{
					const u8 hand_flags = FromSpan<u8>(buf, at);
					at += 1;
					ReadPose(buf, at, &hand.aim_pose);
					ReadPose(buf, at, &hand.grip_pose);
					hand.aim_pose.valid = (hand_flags & 0x01) != 0;
					hand.grip_pose.valid = (hand_flags & 0x02) != 0;
					hand.head_relative = (hand_flags & 0x04) != 0;
					hand.supplied = (hand_flags & 0x08) != 0;
					hand.trigger = ReadF32(buf, at);
					hand.grip = ReadF32(buf, at);
					hand.thumbstick_x = ReadF32(buf, at);
					hand.thumbstick_y = ReadF32(buf, at);
					hand.buttons = FromSpan<u32>(buf, at);
					at += 4;
				}
				buf_cnt += kFakeInputSize;

				VR::XRInput::SetTestInput(input);
				// After the head is in place, so the recenter takes the scripted head as forward.
				if ((flags & 0x02) != 0)
					VR::CameraDriver::RequestRecenter();
				ToResultVector<u8>(ret_buffer, 1, ret_cnt);
				ret_cnt += 1;
				break;
			}
			case MsgVRTelemetry:
			{
				// What the first-person camera saw on its last vsync, for the test runner. Read-only.
				// Reply: u32 size, then the payload (little-endian), version 1:
				//   u8 ver (1), u8 flags (bit0 armed, bit1 lookAt active, bit2 pauseWhen hit, bit3 weapon
				//   raised, bit4 walk-and-shoot moving, bit5 yaw anchor valid, bit6 online-safe, bit7 test
				//   head), u8 disarm reason (CameraDriver::DisarmReason), u8 0, u32 guard-fail vsyncs,
				//   f32 yaw anchor (rad), f32 base yaw now (rad), u32 match age, u32 frames matched,
				//   u64 vsync, f32 prediction horizon (ms)
				// Later versions only append, so a reader takes the size and the fields it knows.
				constexpr u32 kTelemetrySize = 36;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, kTelemetrySize + 4, buf_size)) [[unlikely]]
					goto error;
				const VR::CameraDriver::Telemetry t = VR::CameraDriver::GetTelemetry();
				u32 flags = 0;
				flags |= t.armed ? 0x01u : 0u;
				flags |= t.look_at_active ? 0x02u : 0u;
				flags |= t.pause_when_hit ? 0x04u : 0u;
				flags |= t.weapon_raised ? 0x08u : 0u;
				flags |= t.aim_moving ? 0x10u : 0u;
				flags |= t.yaw_anchor_valid ? 0x20u : 0u;
				flags |= t.online_safe ? 0x40u : 0u;
				flags |= t.test_head ? 0x80u : 0u;
				ToResultVector(ret_buffer, kTelemetrySize, ret_cnt);
				ret_cnt += 4;
				ToResultVector<u8>(ret_buffer, 1, ret_cnt + 0);
				ToResultVector<u8>(ret_buffer, static_cast<u8>(flags), ret_cnt + 1);
				ToResultVector<u8>(ret_buffer, static_cast<u8>(t.disarm_reason), ret_cnt + 2);
				ToResultVector<u8>(ret_buffer, 0, ret_cnt + 3);
				ToResultVector<u32>(ret_buffer, t.guard_fail_vsyncs, ret_cnt + 4);
				ToResultVector<float>(ret_buffer, t.yaw_anchor, ret_cnt + 8);
				ToResultVector<float>(ret_buffer, t.base_yaw_now, ret_cnt + 12);
				ToResultVector<u32>(ret_buffer, t.match_age, ret_cnt + 16);
				ToResultVector<u32>(ret_buffer, t.frames_matched, ret_cnt + 20);
				ToResultVector<u64>(ret_buffer, t.vsync, ret_cnt + 24);
				ToResultVector<float>(ret_buffer, t.prediction_ms, ret_cnt + 32);
				ret_cnt += kTelemetrySize;
				break;
			}
			case MsgVRMenuPointer:
			{
				// The menu pointer (VR::MenuPointer) for the test runner (tools/outbreak-vr/menutest.py). The first byte is
				// the op.
				// op 0, read-only, any time. Reply: u32 size, then the payload (little-endian), version 1:
				//   u8 ver (1), u8 flags (bit0 screen transform valid, bit1 hit valid, bit2 hit by the right hand, bit3 menu
				//   mode, bit4 point-to-select running, bit5 steering, bit6 confirm held back, bit7 suspended or latched),
				//   u8 flags2 (bit0 menu tablet, bit1 PINE test hit, bit2 self-check failed, bit3 refused online by
				//   coop: false), s8 hovered, s8 current, s8 target, u8 presses toward target, u8 D-pad (0xFF none),
				//   f32 u, v, hit distance, f32 screen centre xyz, quaternion xyzw, half width, half height,
				//   u32 cursor node, presses, arrivals, aborts, landing ticks, confirms passed, confirms swallowed
				// Later versions only append, so a reader takes the size and the fields it knows.
				// op 1: a test hit: u8 ver (1), u8 hand (0 left, 1 right), u16 ttl_ms, f32 u, f32 v (u below 0 clears it).
				// Reply: u8 1. Refused as MsgVRFakeInput is: only with PCSX2_VR_TEST_INPUT set, and never while online play
				// is possible.
				// op 2, read-only, any time: the belt pouches (VR::BeltPouches) and the pouch requests the menu pointer took.
				// Reply: u32 size, then the payload (little-endian), version 1:
				//   u8 ver (1), u8 flags (bit0 pouches running, bit1 item screen up, bit2 menu mode, bit3 head anchor, bit4
				//   open press on, bit5 a pouches.suspendWhen guard passes, bit6 left grip a pouch's, bit7 right grip a
				//   pouch's), u8 pouch count, s8 left hand's pouch, s8 right hand's pouch, s8 the menu pointer's waiting
				//   request (-1 none), u8 0, u8 0,
				//   u32 reaches, item double ticks, squeezes, open presses, requests sent, squeezes refused,
				//   u32 menu pointer requests received, started, refused, lapsed,
				//   8 x (f32 centre xyz in the XR base space, f32 radius, s8 cell, s8 item (1 holds one, 0 empty, -1 not
				//   read), u8 item byte, u8 0)
				// Later versions only append, so a reader takes the size and the fields it knows.
				if (!SafetyChecks(buf_cnt, 1, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u8 op = FromSpan<u8>(buf, buf_cnt);
				if (op == 2)
				{
					constexpr u32 kPouchesSize = 208;
					static_assert(VR::BeltPouches::kMaxPouches == 8, "the op 2 reply holds 8 pouches");
					if (!SafetyChecks(buf_cnt, 1, ret_cnt, kPouchesSize + 4, buf_size)) [[unlikely]]
						goto error;
					buf_cnt += 1;
					const VR::BeltPouches::Telemetry bt = VR::BeltPouches::GetTelemetry();
					const VR::MenuPointer::Telemetry mt = VR::MenuPointer::GetTelemetry();
					u32 pouch_flags = 0;
					pouch_flags |= bt.active ? 0x01u : 0u;
					pouch_flags |= bt.screen_up ? 0x02u : 0u;
					pouch_flags |= bt.menu_mode ? 0x04u : 0u;
					pouch_flags |= bt.head_anchor ? 0x08u : 0u;
					pouch_flags |= bt.open_pressing ? 0x10u : 0u;
					pouch_flags |= bt.suspended ? 0x20u : 0u;
					pouch_flags |= bt.owned[0] ? 0x40u : 0u;
					pouch_flags |= bt.owned[1] ? 0x80u : 0u;
					ToResultVector(ret_buffer, kPouchesSize, ret_cnt);
					ret_cnt += 4;
					ToResultVector<u8>(ret_buffer, 1, ret_cnt + 0);
					ToResultVector<u8>(ret_buffer, static_cast<u8>(pouch_flags), ret_cnt + 1);
					ToResultVector<u8>(ret_buffer, bt.count, ret_cnt + 2);
					ToResultVector<s8>(ret_buffer, bt.hover[0], ret_cnt + 3);
					ToResultVector<s8>(ret_buffer, bt.hover[1], ret_cnt + 4);
					ToResultVector<s8>(ret_buffer, mt.request, ret_cnt + 5);
					ToResultVector<u8>(ret_buffer, 0, ret_cnt + 6);
					ToResultVector<u8>(ret_buffer, 0, ret_cnt + 7);
					ToResultVector<u32>(ret_buffer, bt.enters, ret_cnt + 8);
					ToResultVector<u32>(ret_buffer, bt.item_ticks, ret_cnt + 12);
					ToResultVector<u32>(ret_buffer, bt.squeezes, ret_cnt + 16);
					ToResultVector<u32>(ret_buffer, bt.opens, ret_cnt + 20);
					ToResultVector<u32>(ret_buffer, bt.requests, ret_cnt + 24);
					ToResultVector<u32>(ret_buffer, bt.refused, ret_cnt + 28);
					ToResultVector<u32>(ret_buffer, mt.requests, ret_cnt + 32);
					ToResultVector<u32>(ret_buffer, mt.requests_started, ret_cnt + 36);
					ToResultVector<u32>(ret_buffer, mt.requests_refused, ret_cnt + 40);
					ToResultVector<u32>(ret_buffer, mt.requests_lapsed, ret_cnt + 44);
					for (u32 i = 0; i < 8; i++)
					{
						const VR::BeltPouches::Telemetry::PouchView& pv = bt.pouches[i];
						const u32 pouch_at = ret_cnt + 48 + i * 20;
						ToResultVector<float>(ret_buffer, pv.centre[0], pouch_at + 0);
						ToResultVector<float>(ret_buffer, pv.centre[1], pouch_at + 4);
						ToResultVector<float>(ret_buffer, pv.centre[2], pouch_at + 8);
						ToResultVector<float>(ret_buffer, pv.radius, pouch_at + 12);
						ToResultVector<s8>(ret_buffer, pv.cell, pouch_at + 16);
						ToResultVector<s8>(ret_buffer, pv.item, pouch_at + 17);
						ToResultVector<u8>(ret_buffer, pv.item_byte, pouch_at + 18);
						ToResultVector<u8>(ret_buffer, 0, pouch_at + 19);
					}
					ret_cnt += kPouchesSize;
					break;
				}
				if (op == 0)
				{
					constexpr u32 kMenuPointerSize = 84;
					if (!SafetyChecks(buf_cnt, 1, ret_cnt, kMenuPointerSize + 4, buf_size)) [[unlikely]]
						goto error;
					buf_cnt += 1;
					const VR::MenuPointer::Telemetry t = VR::MenuPointer::GetTelemetry();
					u32 flags = 0;
					flags |= t.transform_valid ? 0x01u : 0u;
					flags |= t.hit_valid ? 0x02u : 0u;
					flags |= t.hit_right ? 0x04u : 0u;
					flags |= t.menu_mode ? 0x08u : 0u;
					flags |= t.gate ? 0x10u : 0u;
					flags |= t.in_flight ? 0x20u : 0u;
					flags |= t.confirm_held ? 0x40u : 0u;
					flags |= t.suspended ? 0x80u : 0u;
					u32 flags2 = 0;
					flags2 |= t.tablet ? 0x01u : 0u;
					flags2 |= t.test_hit ? 0x02u : 0u;
					flags2 |= t.selfcheck_failed ? 0x04u : 0u;
					flags2 |= t.refused_online ? 0x08u : 0u;
					ToResultVector(ret_buffer, kMenuPointerSize, ret_cnt);
					ret_cnt += 4;
					ToResultVector<u8>(ret_buffer, 1, ret_cnt + 0);
					ToResultVector<u8>(ret_buffer, static_cast<u8>(flags), ret_cnt + 1);
					ToResultVector<u8>(ret_buffer, static_cast<u8>(flags2), ret_cnt + 2);
					ToResultVector<s8>(ret_buffer, t.hovered, ret_cnt + 3);
					ToResultVector<s8>(ret_buffer, t.current, ret_cnt + 4);
					ToResultVector<s8>(ret_buffer, t.target, ret_cnt + 5);
					ToResultVector<u8>(ret_buffer, t.presses_target, ret_cnt + 6);
					ToResultVector<u8>(ret_buffer, static_cast<u8>((t.dir < 0) ? 0xFF : t.dir), ret_cnt + 7);
					ToResultVector<float>(ret_buffer, t.u, ret_cnt + 8);
					ToResultVector<float>(ret_buffer, t.v, ret_cnt + 12);
					ToResultVector<float>(ret_buffer, t.hit_distance, ret_cnt + 16);
					for (u32 i = 0; i < 3; i++)
						ToResultVector<float>(ret_buffer, t.centre[i], ret_cnt + 20 + i * 4);
					for (u32 i = 0; i < 4; i++)
						ToResultVector<float>(ret_buffer, t.quat[i], ret_cnt + 32 + i * 4);
					ToResultVector<float>(ret_buffer, t.half_width, ret_cnt + 48);
					ToResultVector<float>(ret_buffer, t.half_height, ret_cnt + 52);
					ToResultVector<u32>(ret_buffer, t.node_raw, ret_cnt + 56);
					ToResultVector<u32>(ret_buffer, t.presses_total, ret_cnt + 60);
					ToResultVector<u32>(ret_buffer, t.arrivals, ret_cnt + 64);
					ToResultVector<u32>(ret_buffer, t.aborts, ret_cnt + 68);
					ToResultVector<u32>(ret_buffer, t.ticks, ret_cnt + 72);
					ToResultVector<u32>(ret_buffer, t.confirms_passed, ret_cnt + 76);
					ToResultVector<u32>(ret_buffer, t.confirms_swallowed, ret_cnt + 80);
					ret_cnt += kMenuPointerSize;
					break;
				}
				if (op != 1)
				{
					Console.Warning("(PINE) MsgVRMenuPointer: unknown op %u.", static_cast<unsigned>(op));
					goto error;
				}
				constexpr int kTestHitSize = 1 + 12;
				if (!SafetyChecks(buf_cnt, kTestHitSize, ret_cnt, 1, buf_size)) [[unlikely]]
					goto error;
				const char* refusal = nullptr;
				if (!std::getenv("PCSX2_VR_TEST_INPUT"))
					refusal = "PCSX2_VR_TEST_INPUT is not set";
				else if (EmuConfig.VR.OnlineSafe || EmuConfig.DEV9.EthEnable)
					refusal = "online-safe mode or the network adapter is on";
				else if (FromSpan<u8>(buf, buf_cnt + 1) != 1)
					refusal = "unknown version";
				if (refusal)
				{
					// A runner re-sends at 20 Hz; one line per hundred refusals is loud enough.
					static u32 s_test_hit_refusals = 0;
					if ((s_test_hit_refusals++ % 100) == 0)
						Console.Warning("(PINE) MsgVRMenuPointer test hit refused: %s.", refusal);
					goto error;
				}
				const u8 hand = FromSpan<u8>(buf, buf_cnt + 2);
				const u16 ttl_ms = FromSpan<u16>(buf, buf_cnt + 3);
				u32 at = buf_cnt + 5;
				const float hit_u = ReadF32(buf, at);
				const float hit_v = ReadF32(buf, at);
				buf_cnt += kTestHitSize;
				VR::XRInput::SetTestScreenHit(hand, hit_u, hit_v, ttl_ms);
				ToResultVector<u8>(ret_buffer, 1, ret_cnt);
				ret_cnt += 1;
				break;
			}
#endif
			case MsgPadSet:
			{
				// [u8 controller][u8 bind][f32 value]. Lets a script drive the game without the
				// host's keyboard, e.g. from a remote session. Bind indices follow the pad type's
				// Inputs enum (PadDualshock2: 0 = Up ... 5 = Circle, 6 = Cross ... 18-21 = left stick).
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 1 + 1 + 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				const u32 controller = FromSpan<u8>(buf, buf_cnt);
				const u32 bind = FromSpan<u8>(buf, buf_cnt + 1);
				const float value = std::clamp(std::bit_cast<float>(FromSpan<u32>(buf, buf_cnt + 2)), 0.0f, 1.0f);
				buf_cnt += 6;
				// Pad state belongs to the CPU thread, like every other input source's updates.
				Host::RunOnCPUThread([controller, bind, value]() { Pad::SetControllerState(controller, bind, value); });
				break;
			}
			case MsgVersion:
			{
				u32 size = strlen(BuildVersion::GitRev) + 8;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				snprintf(reinterpret_cast<char*>(&ret_buffer[ret_cnt]), size, "ARMSX2 %s", BuildVersion::GitRev);
				ret_cnt += size;
				break;
			}
			case MsgSaveState:
			{
				if (!VMManager::HasValidVM())
					goto error;
#ifdef ENABLE_OPENXR
				// Online (network adapter on) a state save/load would stall or desync the room (VMManager refuses it too;
				// this fails the command instead of answering OK).
				if (VMManager::IsOnlineLocked())
				{
					Console.Warning("(PINE) %s refused: the network adapter is on.", "MsgSaveState");
					goto error;
				}
#endif
				if (!SafetyChecks(buf_cnt, 1, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				Host::RunOnCPUThread([slot = FromSpan<u8>(buf, buf_cnt)] {
					VMManager::SaveStateToSlot(slot, true, [slot](const std::string& error) {
						SaveState_ReportSaveErrorOSD(error, slot);
					});
				});
				buf_cnt += 1;
				break;
			}
			case MsgLoadState:
			{
				if (!VMManager::HasValidVM())
					goto error;
#ifdef ENABLE_OPENXR
				// Online (network adapter on) a state save/load would stall or desync the room (VMManager refuses it too;
				// this fails the command instead of answering OK).
				if (VMManager::IsOnlineLocked())
				{
					Console.Warning("(PINE) %s refused: the network adapter is on.", "MsgLoadState");
					goto error;
				}
#endif
				if (!SafetyChecks(buf_cnt, 1, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				Host::RunOnCPUThread([slot = FromSpan<u8>(buf, buf_cnt)] {
					Error state_error;
					if (!VMManager::LoadStateFromSlot(slot, false, &state_error))
						SaveState_ReportLoadErrorOSD(state_error.GetDescription(), slot, false);
				});
				buf_cnt += 1;
				break;
			}
			case MsgTitle:
			{
				if (!VMManager::HasValidVM())
					goto error;
				const std::string gameName = VMManager::GetTitle(false);
				const u32 size = gameName.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], gameName.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgID:
			{
				if (!VMManager::HasValidVM())
					goto error;
				const std::string gameSerial = VMManager::GetDiscSerial();
				const u32 size = gameSerial.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], gameSerial.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgUUID:
			{
				if (!VMManager::HasValidVM())
					goto error;
				const std::string crc = fmt::format("{:08x}", VMManager::GetDiscCRC());
				const u32 size = crc.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], crc.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgGameVersion:
			{
				if (!VMManager::HasValidVM())
					goto error;

				const std::string ElfVersion = VMManager::GetDiscVersion();
				const u32 size = ElfVersion.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], ElfVersion.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgStatus:
			{
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, 4, buf_size)) [[unlikely]]
					goto error;
				EmuStatus status;

				switch (VMManager::GetState())
				{
					case VMState::Running:
						status = EmuStatus::Running;
						break;
					case VMState::Paused:
						status = EmuStatus::Paused;
						break;
					default:
						status = EmuStatus::Shutdown;
						break;
				}

				ToResultVector(ret_buffer, status, ret_cnt);
				ret_cnt += 4;
				break;
			}
			case MsgGetStats:
			{
				const std::string stats = BuildStatsJson();
				const u32 size = stats.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], stats.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgGetSetting:
			{
				std::string section, key;
				if (!ReadLengthPrefixedString(buf, buf_cnt, buf_size, &section) ||
					!ReadLengthPrefixedString(buf, buf_cnt, buf_size, &key)) [[unlikely]]
					goto error;

				std::string value;
				{
					auto lock = Host::GetSettingsLock();
					value = Host::GetSettingsInterface()->GetStringValue(section.c_str(), key.c_str(), "");
				}

				const u32 size = value.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], value.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgGetEffectiveSetting:
			{
				std::string section, key;
				if (!ReadLengthPrefixedString(buf, buf_cnt, buf_size, &section) ||
					!ReadLengthPrefixedString(buf, buf_cnt, buf_size, &key)) [[unlikely]]
					goto error;

				const std::string reply = BuildEffectiveSettingJson(section, key);
				const u32 size = reply.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], reply.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgSetSetting:
			{
				std::string section, key, value;
				if (!ReadLengthPrefixedString(buf, buf_cnt, buf_size, &section) ||
					!ReadLengthPrefixedString(buf, buf_cnt, buf_size, &key) ||
					!ReadLengthPrefixedString(buf, buf_cnt, buf_size, &value)) [[unlikely]]
					goto error;

				// Whether this change tears down the GS device, so the caller knows
				// whether it just paid for a reopen. Everything outside this set is
				// applied in place by GSUpdateConfig.
				const bool restart_required = Pcsx2Config::GSOptions::IsRestartOption(key.c_str());

				// Write the persisted key rather than poking EmuConfig directly: a direct
				// poke is silently reverted by the next ApplySettings, which re-derives
				// EmuConfig from the INI layer stack.
				Host::SetBaseStringSettingValue(section.c_str(), key.c_str(), value.c_str());
				Host::CommitBaseSettingChanges();
				Host::RunOnCPUThread([]() { VMManager::ApplySettings(); });

				const std::string reply = fmt::format("{{\"restart_required\":{}}}", restart_required ? "true" : "false");
				const u32 size = reply.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], reply.c_str(), size);
				ret_cnt += size;
				break;
			}
			case MsgFrameAdvance:
			{
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;
				Host::RunOnCPUThread([]() { VMManager::FrameAdvance(1); });
				break;
			}
			case MsgGSDump:
			{
				// [u32 frames][u32 path_len][path_len bytes]. frames == 0 stops a dump that is
				// already recording; UINT32_MAX records until something stops it.
				if (!VMManager::HasValidVM())
					goto error;
				if (!SafetyChecks(buf_cnt, 4, ret_cnt, 0, buf_size)) [[unlikely]]
					goto error;

				const u32 frames = FromSpan<u32>(buf, buf_cnt);
				buf_cnt += 4;

				std::string path;
				if (!ReadLengthPrefixedString(buf, buf_cnt, buf_size, &path)) [[unlikely]]
					goto error;

				std::string reply;
				if (!QueueGSDump(frames, std::move(path), &reply)) [[unlikely]]
					goto error;

				const u32 size = reply.size() + 1;
				if (!SafetyChecks(buf_cnt, 0, ret_cnt, size + 4, buf_size)) [[unlikely]]
					goto error;
				ToResultVector(ret_buffer, size, ret_cnt);
				ret_cnt += 4;
				memcpy(&ret_buffer[ret_cnt], reply.c_str(), size);
				ret_cnt += size;
				break;
			}
			default:
			{
			error:
				return IPCBuffer{5, MakeFailIPC(ret_buffer)};
			}
		}
	}
	return IPCBuffer{(int)ret_cnt, MakeOkIPC(ret_buffer, ret_cnt)};
}
