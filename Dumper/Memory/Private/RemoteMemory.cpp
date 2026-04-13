#include "RemoteMemory.h"

#include <cstring>
#include <iostream>
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

	bool Init(DWORD pid)
	{
		g_pid = pid;

		hv::g_cr3 = hv::query_process_cr3(pid);
		if (hv::g_cr3 == 0)
		{
			std::cerr << "[RemoteMemory] query_process_cr3 returned 0, falling back to scan_process_dtb...\n";
			hv::g_cr3 = hv::scan_process_dtb(pid);
		}
		if (hv::g_cr3 == 0)
		{
			std::cerr << "[RemoteMemory] Failed to resolve CR3 for PID " << pid << "\n";
			return false;
		}

		g_mainModuleBase = hv::get_section_base_process(pid);
		if (g_mainModuleBase == 0)
		{
			std::cerr << "[RemoteMemory] get_section_base_process returned 0 for PID " << pid << "\n";
			return false;
		}

		uint16_t probe = 0;
		if (hv::read_virt_mem(&probe, reinterpret_cast<void*>(g_mainModuleBase), sizeof(probe)) != sizeof(probe) || probe != 0x5A4D)
		{
			std::cerr << "[RemoteMemory] Main module base 0x" << std::hex << g_mainModuleBase
			          << " does not start with 'MZ' (got 0x" << probe << "). CR3 may be wrong.\n" << std::dec;
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

		uint8_t* out = static_cast<uint8_t*>(dst);
		size_t remaining = size;
		uintptr_t cursor = addr;

		while (remaining > 0)
		{
			const uintptr_t pageEnd = PageDown(cursor) + PageSize;
			const size_t chunk = (pageEnd - cursor) < remaining ? (pageEnd - cursor) : remaining;

			const size_t got = hv::read_virt_mem(out, reinterpret_cast<void*>(cursor), chunk);
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
