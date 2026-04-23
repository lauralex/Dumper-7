#include "RemoteMemory.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <iostream>
#include <mutex>
#include <Windows.h>

#include "hv.h"

namespace RemoteMemory
{
	namespace
	{
		constexpr size_t PageSize = 0x1000;

		DWORD g_pid = 0;
		uint64_t g_mainModuleBase = 0;
		size_t g_mainModuleSize = 0;

		// CR3 auto-refresh state. Anti-cheat systems like EAC rotate the target's CR3 every
		// few minutes; any cached CR3 goes stale and every subsequent hv::read_virt_mem returns
		// zero bytes. We guard against that by periodically (and on read-failure) re-querying
		// and re-validating the CR3 via 'MZ' at the main module base.
		std::mutex g_Cr3Mutex;
		std::chrono::steady_clock::time_point g_LastCr3Check{};
		constexpr std::chrono::seconds kCr3RevalidateCooldown{5};
		std::atomic<uint64_t> g_Cr3RefreshCount{0};

		// Periodic liveness probe — belt-and-suspenders for the byte-count-based CR3 staleness
		// detection in ReadBuffer. Every N ReadBuffer calls we read 'MZ' at the main module
		// base through the current CR3; if it fails, force a CR3 refresh. This catches the
		// (rare) case where reads "succeed" against a drifted CR3 that still happens to map
		// non-image pages. One hypercall every kMzProbeEvery reads is negligible cost.
		constexpr uint64_t kMzProbeEvery = 4096;
		std::atomic<uint64_t> g_ReadBufferCount{0};

		inline uintptr_t PageDown(uintptr_t addr)
		{
			return addr & ~static_cast<uintptr_t>(PageSize - 1);
		}

		// Read IMAGE_NT_HEADERS SizeOfImage directly from the target to get the main module size.
		size_t ReadMainModuleSize(uint64_t moduleBase)
		{
			uint16_t dosMagic = 0;
			if (hv::read_virt_mem(&dosMagic, reinterpret_cast<void*>(moduleBase), sizeof(dosMagic)) != sizeof(dosMagic))
				return 0;
			if (dosMagic != 0x5A4D) // 'MZ'
				return 0;

			int32_t e_lfanew = 0;
			if (hv::read_virt_mem(&e_lfanew, reinterpret_cast<void*>(moduleBase + 0x3C), sizeof(e_lfanew)) != sizeof(e_lfanew))
				return 0;

			const uint64_t ntHeaders = moduleBase + static_cast<uint32_t>(e_lfanew);
			uint32_t ntSignature = 0;
			if (hv::read_virt_mem(&ntSignature, reinterpret_cast<void*>(ntHeaders), sizeof(ntSignature)) != sizeof(ntSignature))
				return 0;
			if (ntSignature != 0x00004550) // 'PE\0\0'
				return 0;

			// SizeOfImage is at IMAGE_NT_HEADERS64.OptionalHeader.SizeOfImage.
			// NT sig (4) + IMAGE_FILE_HEADER (20) + offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfImage) = 4 + 20 + 56 = 80.
			uint32_t sizeOfImage = 0;
			if (hv::read_virt_mem(&sizeOfImage, reinterpret_cast<void*>(ntHeaders + 4 + 20 + 56), sizeof(sizeOfImage)) != sizeof(sizeOfImage))
				return 0;

			return sizeOfImage;
		}
	}

	// Validate a candidate CR3 by probing the main module base: needs to map to a physical
	// page AND the first two bytes must be 'MZ'. Anti-cheat / EPROCESS-spoofing games sometimes
	// return stale or decoy CR3s from query_process_cr3; scan_process_dtb's linear DTB scan is
	// a better fallback there even when the primary call returns non-zero.
	static bool ValidateCr3(uint64_t candidate, uintptr_t moduleBase)
	{
		if (candidate == 0 || moduleBase == 0)
			return false;
		if (hv::get_physical_address(candidate, reinterpret_cast<void*>(moduleBase)) == 0)
			return false;
		uint16_t probe = 0;
		if (hv::read_virt_mem(candidate, &probe, reinterpret_cast<void*>(moduleBase), sizeof(probe)) != sizeof(probe))
			return false;
		return probe == 0x5A4D;
	}

	// Re-query the target's CR3 and install the first candidate that still validates against the
	// main module's 'MZ' signature. Returns true when a different CR3 was installed. The 'force'
	// flag bypasses the cooldown — used by the read-failure path that already knows the current
	// CR3 is stale; the periodic path respects the cooldown to avoid thrashing.
	bool TryRefreshCr3(bool force)
	{
		std::lock_guard<std::mutex> lock(g_Cr3Mutex);

		const auto now = std::chrono::steady_clock::now();
		if (!force && (now - g_LastCr3Check) < kCr3RevalidateCooldown)
			return false;
		g_LastCr3Check = now;

		const uint64_t current = hv::g_cr3;
		if (ValidateCr3(current, static_cast<uintptr_t>(g_mainModuleBase)))
			return false; // still good

		const uint64_t candidates[3] = {
			hv::query_process_user_cr3(g_pid),
			hv::query_process_cr3(g_pid),
			hv::scan_process_dtb(g_pid),
		};
		static const char* const names[3] = { "user", "kernel", "scan" };

		for (int i = 0; i < 3; ++i)
		{
			if (candidates[i] == current || candidates[i] == 0)
				continue;
			if (!ValidateCr3(candidates[i], static_cast<uintptr_t>(g_mainModuleBase)))
				continue;
			hv::g_cr3 = candidates[i];
			++g_Cr3RefreshCount;
			std::cerr << std::format(
				"[RemoteMemory] CR3 rotated by AC/kernel — switched to {} CR3 0x{:X} "
				"(was 0x{:X}, refresh #{})\n",
				names[i], candidates[i], current, g_Cr3RefreshCount.load());
			return true;
		}

		std::cerr << std::format(
			"[RemoteMemory] CR3 rotation detected but no candidate validates: "
			"user=0x{:X} kernel=0x{:X} scan=0x{:X} (current=0x{:X}). "
			"Reads will continue to fail until next probe.\n",
			candidates[0], candidates[1], candidates[2], current);
		return false;
	}

	uint64_t GetCr3RefreshCount()
	{
		return g_Cr3RefreshCount.load();
	}

	bool Init(DWORD pid)
	{
		g_pid = pid;

		g_mainModuleBase = hv::get_section_base_process(pid);
		if (g_mainModuleBase == 0)
		{
			std::cerr << "[RemoteMemory] get_section_base_process returned 0 for PID " << pid << "\n";
			return false;
		}

		// Prefer KPROCESS::UserDirectoryTableBase over DirectoryTableBase. On KVAShadow-
		// enabled Windows (10 1803+ / 11) the kernel CR3 at KPROCESS+0x28 only maps the
		// subset of user-mode pages needed for kernel↔user transitions — the target's
		// heap pages we walk to reach ChildProperties and FField chains aren't mapped in
		// that CR3, so reads return zeros and every offset finder dead-ends. The user
		// CR3 (UserDirectoryTableBase) has the full user-mode mapping.
		//
		// Fallback chain: user CR3 → kernel CR3 → scan_process_dtb. Every candidate has
		// to pass the 'MZ' validation at main module base; if none do, we bail.
		const uint64_t userCr3 = hv::query_process_user_cr3(pid);
		const uint64_t kernelCr3 = hv::query_process_cr3(pid);
		const uint64_t scanCr3 = hv::scan_process_dtb(pid);
		std::cerr << std::format(
			"[RemoteMemory] query_process_user_cr3=0x{:X}, query_process_cr3=0x{:X}, scan_process_dtb=0x{:X}\n",
			userCr3, kernelCr3, scanCr3);

		if (ValidateCr3(userCr3, g_mainModuleBase))
		{
			hv::g_cr3 = userCr3;
			std::cerr << "[RemoteMemory] Using USER CR3 (KPROCESS::UserDirectoryTableBase)\n";
		}
		else if (ValidateCr3(kernelCr3, g_mainModuleBase))
		{
			hv::g_cr3 = kernelCr3;
			std::cerr << "[RemoteMemory] Using KERNEL CR3 (KPROCESS::DirectoryTableBase). "
			             "User-mode heap reads may fail on KVAShadow-enabled systems.\n";
		}
		else if (ValidateCr3(scanCr3, g_mainModuleBase))
		{
			hv::g_cr3 = scanCr3;
			std::cerr << "[RemoteMemory] Using scan_process_dtb CR3 (primary queries didn't validate)\n";
		}
		else
		{
			std::cerr << "[RemoteMemory] No CR3 candidate passes the 'MZ' validation at 0x"
			          << std::hex << g_mainModuleBase << std::dec << "\n";
			return false;
		}

		g_mainModuleSize = ReadMainModuleSize(g_mainModuleBase);
		if (g_mainModuleSize == 0)
		{
			std::cerr << "[RemoteMemory] Failed to read SizeOfImage from main module at 0x"
			          << std::hex << g_mainModuleBase << std::dec << "\n";
			return false;
		}

		return true;
	}

	DWORD GetTargetPid()        { return g_pid; }
	uint64_t GetCr3()           { return hv::g_cr3; }
	uintptr_t GetMainModuleBase() { return static_cast<uintptr_t>(g_mainModuleBase); }
	size_t GetMainModuleSize()  { return g_mainModuleSize; }

	bool ReadBuffer(uintptr_t addr, void* dst, size_t size, PartialReadPolicy policy)
	{
		if (!dst || size == 0)
			return false;

		// Periodic MZ liveness probe. Cheap (one qword read) and independent of whether
		// any individual page in this ReadBuffer is resident — catches CR3 drift to a
		// CR3 that happens to alias to non-image memory which would otherwise look like
		// "successful reads of non-zero garbage."
		const uint64_t nCall = g_ReadBufferCount.fetch_add(1, std::memory_order_relaxed);
		if (g_mainModuleBase != 0 && (nCall % kMzProbeEvery) == kMzProbeEvery - 1)
		{
			uint16_t mz = 0;
			if (hv::read_virt_mem(&mz, reinterpret_cast<void*>(g_mainModuleBase), sizeof(mz)) != sizeof(mz)
				|| mz != 0x5A4D)
			{
				TryRefreshCr3(/*force=*/true);
			}
		}

		uint8_t* out = static_cast<uint8_t*>(dst);
		size_t remaining = size;
		uintptr_t cursor = addr;
		int zeroBytePages = 0;
		int totalPages = 0;
		bool triedCr3Refresh = false;

		while (remaining > 0)
		{
			const uintptr_t pageEnd = PageDown(cursor) + PageSize;
			const size_t chunk = (pageEnd - cursor) < remaining ? (pageEnd - cursor) : remaining;

			size_t got = hv::read_virt_mem(out, reinterpret_cast<void*>(cursor), chunk);
			++totalPages;
			if (got == 0)
			{
				// A total read failure can mean: (a) the page is genuinely not resident, or
				// (b) the anti-cheat rotated CR3 and every read is now failing. Try refreshing
				// CR3 once per ReadBuffer and retry; if the page still reads zero, fall through
				// to the policy handling below.
				if (!triedCr3Refresh)
				{
					triedCr3Refresh = true;
					if (TryRefreshCr3(/*force=*/true))
						got = hv::read_virt_mem(out, reinterpret_cast<void*>(cursor), chunk);
				}
				if (got == 0)
					++zeroBytePages;
			}

			if (got != chunk)
			{
				if (policy == PartialReadPolicy::ErrorOnGap)
					return false;

				// ZeroFillOnGap: zero the portion that wasn't read and continue to the next page.
				if (got < chunk)
					std::memset(out + got, 0, chunk - got);
			}

			out       += chunk;
			cursor    += chunk;
			remaining -= chunk;
		}

		// If every page failed with zero bytes we're almost certainly on a stale CR3; schedule a
		// periodic re-check for the next ReadBuffer call (the cooldown prevents thrashing).
		if (zeroBytePages == totalPages && totalPages > 0)
			TryRefreshCr3(/*force=*/false);

		return true;
	}

	bool WriteBuffer(uintptr_t addr, const void* src, size_t size)
	{
		if (!src || size == 0)
			return false;

		const uint8_t* in = static_cast<const uint8_t*>(src);
		size_t remaining = size;
		uintptr_t cursor = addr;

		while (remaining > 0)
		{
			const uintptr_t pageEnd = PageDown(cursor) + PageSize;
			const size_t chunk = (pageEnd - cursor) < remaining ? (pageEnd - cursor) : remaining;

			const size_t wrote = hv::write_virt_mem(
				hv::g_cr3,
				reinterpret_cast<void*>(cursor),
				in,
				chunk);
			if (wrote != chunk)
				return false;

			in        += chunk;
			cursor    += chunk;
			remaining -= chunk;
		}

		return true;
	}

	bool IsValid(uintptr_t addr)
	{
		uint8_t probe = 0;
		return hv::read_virt_mem(&probe, reinterpret_cast<void*>(addr), 1) == 1;
	}
}
