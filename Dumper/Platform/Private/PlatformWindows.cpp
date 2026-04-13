// External-mode reimplementation.
//
// In injected mode, every Platform::* helper here dereferenced target pointers directly
// (same address space). For the external dumper, every pointer into the target has to be
// read via hypercalls (RemoteMemory::Read / ReadBuffer). Pattern scanners work against a
// bulk-read snapshot of the target's main module (.text / .rdata / .data / ...) so the
// inner loops stay pure CPU-local byte matching, and only the final match address has to
// be translated back to a remote VA.
//
// See C:\Users\Authority\.claude\plans\agile-cuddling-stroustrup.md for the full plan.

#include "TmpUtils.h"
#include "PlatformWindows.h"
#include "Arch_x86.h"

#include "RemoteMemory.h"

#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
	using namespace PlatformWindows;

	// ============================================================================================
	// Remote PE helpers — read the target's IMAGE_* headers via hypercalls.
	// ============================================================================================

	constexpr uint16_t kImageDosSignature = 0x5A4D; // 'MZ'
	constexpr uint32_t kImageNtSignature  = 0x00004550; // 'PE\0\0'

	struct RemoteSectionHeader
	{
		char     Name[8];
		uint32_t VirtualSize;
		uint32_t VirtualAddress;
		uint32_t SizeOfRawData;
		uint32_t PointerToRawData;
		uint32_t PointerToRelocations;
		uint32_t PointerToLinenumbers;
		uint16_t NumberOfRelocations;
		uint16_t NumberOfLinenumbers;
		uint32_t Characteristics;
	};
	static_assert(sizeof(RemoteSectionHeader) == sizeof(IMAGE_SECTION_HEADER), "RemoteSectionHeader size mismatch");

	// Locate the IMAGE_NT_HEADERS of a module by reading e_lfanew from the target.
	// Returns 0 on failure.
	uintptr_t ReadNtHeadersAddress(uintptr_t moduleBase)
	{
		if (moduleBase == 0)
			return 0;

		auto dosMagic = RemoteMemory::TryRead<uint16_t>(moduleBase);
		if (!dosMagic || *dosMagic != kImageDosSignature)
			return 0;

		auto e_lfanew = RemoteMemory::TryRead<int32_t>(moduleBase + 0x3C);
		if (!e_lfanew)
			return 0;

		const uintptr_t ntHeaders = moduleBase + static_cast<uint32_t>(*e_lfanew);
		auto ntSig = RemoteMemory::TryRead<uint32_t>(ntHeaders);
		if (!ntSig || *ntSig != kImageNtSignature)
			return 0;

		return ntHeaders;
	}

	// Offsets within IMAGE_NT_HEADERS64 after the 4-byte signature and 20-byte IMAGE_FILE_HEADER.
	constexpr uint32_t kOptionalHeaderOffset = 4 + 20;
	constexpr uint32_t kSizeOfImageOffset = kOptionalHeaderOffset + 56;
	constexpr uint32_t kDataDirectoriesOffset = kOptionalHeaderOffset + 112;
	constexpr uint32_t kImportTableDirectoryOffset = kDataDirectoriesOffset + (IMAGE_DIRECTORY_ENTRY_IMPORT * 8);
	constexpr uint32_t kExportTableDirectoryOffset = kDataDirectoriesOffset + (IMAGE_DIRECTORY_ENTRY_EXPORT * 8);

	uint32_t ReadImageSize(uintptr_t moduleBase, uintptr_t ntHeaders)
	{
		(void)moduleBase;
		return RemoteMemory::Read<uint32_t>(ntHeaders + kSizeOfImageOffset);
	}

	// ============================================================================================
	// Module snapshot — bulk-read the target's main module sections into local buffers once.
	// Pattern scans run over the local buffers; matches are translated back to remote VAs.
	// ============================================================================================

	struct SectionCache
	{
		std::string Name;
		uintptr_t RemoteBase = 0;     // Address of the section in the target.
		uint32_t Size = 0;            // Misc.VirtualSize
		uint32_t Characteristics = 0;
		std::unique_ptr<uint8_t[]> LocalBytes;    // Size bytes bulk-read from the target; null if unreadable.
	};

	struct ModuleCache
	{
		uintptr_t RemoteBase = 0;
		uint32_t ImageSize = 0;
		std::vector<SectionCache> Sections;

		bool Initialized = false;

		const SectionCache* FindSectionContaining(uintptr_t remoteAddr) const
		{
			for (const SectionCache& s : Sections)
			{
				if (remoteAddr >= s.RemoteBase && remoteAddr < (s.RemoteBase + s.Size))
					return &s;
			}
			return nullptr;
		}

		const SectionCache* FindSectionByName(const std::string& name) const
		{
			for (const SectionCache& s : Sections)
			{
				if (s.Name == name)
					return &s;
			}
			return nullptr;
		}
	};

	ModuleCache& GetMainModuleCache()
	{
		static ModuleCache cache;
		if (cache.Initialized)
			return cache;

		cache.RemoteBase = RemoteMemory::GetMainModuleBase();
		cache.ImageSize = static_cast<uint32_t>(RemoteMemory::GetMainModuleSize());

		const uintptr_t ntHeaders = ReadNtHeadersAddress(cache.RemoteBase);
		if (ntHeaders == 0)
		{
			std::cerr << "[Platform] Failed to read NT headers of main module at 0x"
			          << std::hex << cache.RemoteBase << std::dec << "\n";
			cache.Initialized = true;
			return cache;
		}

		// NumberOfSections is at IMAGE_FILE_HEADER + 2 (Machine is at +0).
		const uint16_t numberOfSections = RemoteMemory::Read<uint16_t>(ntHeaders + 4 + 2);
		const uint16_t sizeOfOptionalHeader = RemoteMemory::Read<uint16_t>(ntHeaders + 4 + 16);
		const uintptr_t firstSectionAddr = ntHeaders + 4 + 20 + sizeOfOptionalHeader;

		cache.Sections.reserve(numberOfSections);
		for (uint16_t i = 0; i < numberOfSections; ++i)
		{
			const uintptr_t shAddr = firstSectionAddr + (i * sizeof(RemoteSectionHeader));
			RemoteSectionHeader sh{};
			if (!RemoteMemory::ReadBuffer(shAddr, &sh, sizeof(sh), RemoteMemory::PartialReadPolicy::ErrorOnGap))
			{
				std::cerr << "[Platform] Failed to read section header #" << i << "\n";
				continue;
			}

			SectionCache entry;
			entry.Name = std::string(sh.Name, strnlen(sh.Name, 8));
			entry.RemoteBase = cache.RemoteBase + sh.VirtualAddress;
			entry.Size = sh.VirtualSize;
			entry.Characteristics = sh.Characteristics;

			if ((sh.Characteristics & IMAGE_SCN_MEM_READ) && entry.Size > 0)
			{
				entry.LocalBytes = std::make_unique<uint8_t[]>(entry.Size);
				if (!RemoteMemory::ReadBuffer(entry.RemoteBase, entry.LocalBytes.get(), entry.Size,
				                              RemoteMemory::PartialReadPolicy::ZeroFillOnGap))
				{
					std::cerr << "[Platform] Partial read on section '" << entry.Name
					          << "' (size 0x" << std::hex << entry.Size << std::dec << ")\n";
				}
			}

			cache.Sections.push_back(std::move(entry));
		}

		cache.Initialized = true;
		return cache;
	}

	// Translate a remote VA that lies inside a cached section to a pointer into the local buffer.
	// Returns nullptr if the address isn't in any cached section with a local buffer.
	const uint8_t* RemoteToLocal(uintptr_t remoteAddr, size_t* outRemainingBytes = nullptr)
	{
		const ModuleCache& cache = GetMainModuleCache();
		const SectionCache* sec = cache.FindSectionContaining(remoteAddr);
		if (!sec || !sec->LocalBytes)
			return nullptr;

		const uintptr_t offset = remoteAddr - sec->RemoteBase;
		if (outRemainingBytes)
			*outRemainingBytes = sec->Size - offset;
		return sec->LocalBytes.get() + offset;
	}

	int64_t GetAlignedSizeWithOffsetFromEnd(const uint32_t SizeToAlign, const uint32_t Alignment, const uint32_t OffsetFromEnd)
	{
		const uint32_t ValueToAlign = (SizeToAlign - (Alignment - 1) - OffsetFromEnd);
		if (ValueToAlign > SizeToAlign) // underflow
			return -1;
		return Align(ValueToAlign, Alignment);
	}

	// ============================================================================================
	// Opaque SectionInfo payload — maps to a pair (remoteBase, size) we can look up quickly.
	// Keeping the payload at 0x10 bytes to match the public struct.
	// ============================================================================================

	struct WindowsSectionInfo
	{
		uintptr_t RemoteBase = 0;
		uint32_t Size = 0;
		uint32_t Characteristics = 0;
	};
	static_assert(sizeof(WindowsSectionInfo) == sizeof(SectionInfo),
		"SectionInfo opaque payload size must match WindowsSectionInfo");

	inline WindowsSectionInfo SectionInfoToWinSectionInfo(const SectionInfo& Info)
	{
		return std::bit_cast<WindowsSectionInfo>(Info);
	}
	inline SectionInfo WinSectionInfoToSectionInfo(const WindowsSectionInfo& Info)
	{
		return std::bit_cast<SectionInfo>(Info);
	}

	// ============================================================================================
	// Import table walking on the main module — used by GetAddressOfImportedFunction* helpers.
	// Returns the resolved function address (loader-fixed-up IAT slot value), not the slot itself.
	// ============================================================================================

	uintptr_t WalkMainModuleImportForFunction(const char* moduleToImportFrom, const char* functionName)
	{
		const ModuleCache& cache = GetMainModuleCache();
		if (!cache.RemoteBase)
			return 0;

		const uintptr_t ntHeaders = ReadNtHeadersAddress(cache.RemoteBase);
		if (ntHeaders == 0)
			return 0;

		const uint32_t importTableRva = RemoteMemory::Read<uint32_t>(ntHeaders + kImportTableDirectoryOffset);
		const uint32_t importTableSize = RemoteMemory::Read<uint32_t>(ntHeaders + kImportTableDirectoryOffset + 4);
		if (importTableRva == 0 || importTableSize == 0)
			return 0;

		const std::string lowerWanted = Utils::StrToLower(moduleToImportFrom);

		// Walk IMAGE_IMPORT_DESCRIPTOR entries until the null terminator.
		for (uint32_t descOffset = 0; ; descOffset += sizeof(IMAGE_IMPORT_DESCRIPTOR))
		{
			IMAGE_IMPORT_DESCRIPTOR desc{};
			if (!RemoteMemory::ReadBuffer(cache.RemoteBase + importTableRva + descOffset,
			                              &desc, sizeof(desc),
			                              RemoteMemory::PartialReadPolicy::ErrorOnGap))
				return 0;
			if (desc.Characteristics == 0 && desc.FirstThunk == 0)
				return 0; // terminator

			// Read the module name for this descriptor.
			char nameBuf[256]{};
			if (desc.Name == 0 || desc.Name == 0xFFFF)
				continue;
			RemoteMemory::ReadBuffer(cache.RemoteBase + desc.Name, nameBuf, sizeof(nameBuf) - 1,
			                         RemoteMemory::PartialReadPolicy::ZeroFillOnGap);

			if (Utils::StrToLower(nameBuf) != lowerWanted)
				continue;

			// Walk the import name table (OriginalFirstThunk) in lockstep with the IAT (FirstThunk).
			// The loader fixes up FirstThunk entries to point to the real functions in kernel32/ntdll.
			for (uint32_t thunkIndex = 0; ; ++thunkIndex)
			{
				const uintptr_t nameThunkAddr = cache.RemoteBase + desc.OriginalFirstThunk + (thunkIndex * 8);
				const uintptr_t funcThunkAddr = cache.RemoteBase + desc.FirstThunk + (thunkIndex * 8);

				const uint64_t nameThunkValue = RemoteMemory::Read<uint64_t>(nameThunkAddr);
				if (nameThunkValue == 0)
					break;

				// Skip ordinals — we only match by name.
				if (nameThunkValue & IMAGE_ORDINAL_FLAG64)
					continue;

				// nameThunkValue is an RVA pointing at an IMAGE_IMPORT_BY_NAME: uint16 Hint + char Name[].
				char importedFuncName[256]{};
				RemoteMemory::ReadBuffer(cache.RemoteBase + (nameThunkValue & 0x7FFFFFFF) + 2,
				                         importedFuncName, sizeof(importedFuncName) - 1,
				                         RemoteMemory::PartialReadPolicy::ZeroFillOnGap);

				if (strcmp(importedFuncName, functionName) == 0)
				{
					// Return the *resolved* function address (the current IAT slot value) so that
					// later pattern scans looking for "call qword [RIP+disp]" targeting kernel32
					// can compare against it. The loader has already filled this in.
					return RemoteMemory::Read<uintptr_t>(funcThunkAddr);
				}
			}

			return 0; // found the module but not the function
		}
	}

	// ============================================================================================
	// Pattern scan primitive — runs on a local buffer, returns local offset of match or SIZE_MAX.
	// ============================================================================================

	size_t FindPatternInLocalBuffer(const std::vector<int>& signature, const uint8_t* buffer, size_t bufferSize, size_t skipCount)
	{
		const size_t patternLength = signature.size();
		if (bufferSize < patternLength)
			return SIZE_MAX;

		size_t currentSkips = 0;
		for (size_t i = 0; i <= bufferSize - patternLength; ++i)
		{
			bool matched = true;
			for (size_t j = 0; j < patternLength; ++j)
			{
				if (buffer[i + j] != signature[j] && signature[j] != -1)
				{
					matched = false;
					break;
				}
			}
			if (!matched)
				continue;

			if (currentSkips < skipCount)
			{
				++currentSkips;
				continue;
			}
			return i;
		}
		return SIZE_MAX;
	}

	std::vector<int> ParsePatternString(const char* pattern)
	{
		std::vector<int> bytes;
		char* cursor = const_cast<char*>(pattern);
		char* end = const_cast<char*>(pattern) + strlen(pattern);
		while (cursor < end)
		{
			if (*cursor == '?')
			{
				++cursor;
				if (cursor < end && *cursor == '?')
					++cursor;
				bytes.push_back(-1);
			}
			else if (*cursor == ' ')
			{
				++cursor;
			}
			else
			{
				bytes.push_back(strtoul(cursor, &cursor, 16));
			}
		}
		return bytes;
	}
}


// ================================================================================================
// PlatformWindows:: public API
// ================================================================================================

uintptr_t PlatformWindows::GetModuleBase(const char* const ModuleName)
{
	if (ModuleName == nullptr)
		return RemoteMemory::GetMainModuleBase();

	// v1 deliberately does not walk the target's PEB for named modules — Settings::General::
	// DefaultModuleName is typically nullptr. If a game actually relies on this path we'll
	// add target-PEB walking via NtQueryInformationThread-bootstrapped hypercalls.
	std::cerr << "[Platform] GetModuleBase(\"" << ModuleName
	          << "\") requested but named-module lookup is not implemented in external mode. "
	             "Falling back to main module.\n";
	return RemoteMemory::GetMainModuleBase();
}

uintptr_t PlatformWindows::GetOffset(const uintptr_t Address, const char* const ModuleName)
{
	const uintptr_t moduleBase = GetModuleBase(ModuleName);
	const size_t moduleSize = RemoteMemory::GetMainModuleSize();

	// Defensive assertion per the plan: any Platform caller passing a local-buffer pointer
	// here (instead of a properly-translated remote VA) would silently bake nonsense into
	// the generated SDK's offset constants. Catch it here, once.
	assert(Address == 0 || (Address >= moduleBase && Address < (moduleBase + moduleSize)));

	return Address - moduleBase;
}

uintptr_t PlatformWindows::GetOffset(const void* Address, const char* const ModuleName)
{
	return GetOffset(reinterpret_cast<uintptr_t>(Address), ModuleName);
}

SectionInfo PlatformWindows::GetSectionInfo(const std::string& SectionName, const char* const ModuleName)
{
	(void)ModuleName; // Only main module supported in v1; see GetModuleBase.
	const ModuleCache& cache = GetMainModuleCache();
	const SectionCache* sec = cache.FindSectionByName(SectionName);
	if (!sec)
		return WinSectionInfoToSectionInfo({});

	return WinSectionInfoToSectionInfo({ sec->RemoteBase, sec->Size, sec->Characteristics });
}

void* PlatformWindows::IterateSectionWithCallback(const SectionInfo& Info, const std::function<bool(void* Address)>& Callback, uint32_t Granularity, uint32_t OffsetFromEnd)
{
	const WindowsSectionInfo winSection = SectionInfoToWinSectionInfo(Info);
	if (winSection.RemoteBase == 0 || winSection.Size == 0)
		return nullptr;

	const int64_t iterationSize = GetAlignedSizeWithOffsetFromEnd(winSection.Size, Granularity, OffsetFromEnd);
	if (iterationSize < 0)
		return nullptr;

	for (uintptr_t remoteAddr = winSection.RemoteBase;
	     remoteAddr < (winSection.RemoteBase + static_cast<uintptr_t>(iterationSize));
	     remoteAddr += Granularity)
	{
		if (Callback(reinterpret_cast<void*>(remoteAddr)))
			return reinterpret_cast<void*>(remoteAddr);
	}
	return nullptr;
}

void* PlatformWindows::IterateAllSectionsWithCallback(const std::function<bool(void* Address)>& Callback, uint32_t Granularity, uint32_t OffsetFromEnd, const char* const ModuleName)
{
	(void)ModuleName;
	const ModuleCache& cache = GetMainModuleCache();

	for (const SectionCache& sec : cache.Sections)
	{
		if (!(sec.Characteristics & IMAGE_SCN_MEM_READ) || sec.Size == 0)
			continue;

		WindowsSectionInfo info{ sec.RemoteBase, sec.Size, sec.Characteristics };
		if (void* result = IterateSectionWithCallback(WinSectionInfoToSectionInfo(info), Callback, Granularity, OffsetFromEnd))
			return result;
	}
	return nullptr;
}

namespace
{
	struct ModuleRange { uintptr_t Base; size_t Size; };

	const std::vector<ModuleRange>& GetTargetModuleRanges()
	{
		static std::vector<ModuleRange> cache = []
		{
			std::vector<ModuleRange> out;
			const DWORD pid = RemoteMemory::GetTargetPid();
			if (!pid)
				return out;

			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
			if (snap == INVALID_HANDLE_VALUE)
				return out;

			MODULEENTRY32W entry{};
			entry.dwSize = sizeof(entry);
			if (Module32FirstW(snap, &entry))
			{
				do
				{
					out.push_back({ reinterpret_cast<uintptr_t>(entry.modBaseAddr), entry.modBaseSize });
				} while (Module32NextW(snap, &entry));
			}
			CloseHandle(snap);
			return out;
		}();
		return cache;
	}
}

bool PlatformWindows::IsAddressInAnyModule(const uintptr_t Address)
{
	for (const ModuleRange& r : GetTargetModuleRanges())
	{
		if (Address >= r.Base && Address < (r.Base + r.Size))
			return true;
	}
	return false;
}

bool PlatformWindows::IsAddressInAnyModule(const void* Address)
{
	return IsAddressInAnyModule(reinterpret_cast<uintptr_t>(Address));
}

bool PlatformWindows::IsAddressInProcessRange(const uintptr_t Address)
{
	const uintptr_t moduleBase = RemoteMemory::GetMainModuleBase();
	const size_t moduleSize = RemoteMemory::GetMainModuleSize();
	if (Address >= moduleBase && Address < (moduleBase + moduleSize))
		return true;
	return IsAddressInAnyModule(Address);
}

bool PlatformWindows::IsAddressInProcessRange(const void* Address)
{
	return IsAddressInProcessRange(reinterpret_cast<uintptr_t>(Address));
}

bool PlatformWindows::IsBadReadPtr(const uintptr_t Address)
{
	if constexpr (!Is32Bit())
	{
		if (!Architecture_x86_64::IsValid64BitVirtualAddress(reinterpret_cast<const void*>(Address)))
			return true;
	}
	return !RemoteMemory::IsValid(Address);
}

bool PlatformWindows::IsBadReadPtr(const void* Address)
{
	return IsBadReadPtr(reinterpret_cast<uintptr_t>(Address));
}

const void* PlatformWindows::GetAddressOfImportedFunction(const char* /*SearchModuleName*/, const char* ModuleToImportFrom, const char* SearchFunctionName)
{
	// External v1 only walks the main module's imports; SearchModuleName is ignored.
	return reinterpret_cast<const void*>(WalkMainModuleImportForFunction(ModuleToImportFrom, SearchFunctionName));
}

const void* PlatformWindows::GetAddressOfImportedFunctionFromAnyModule(const char* ModuleToImportFrom, const char* SearchFunctionName)
{
	return reinterpret_cast<const void*>(WalkMainModuleImportForFunction(ModuleToImportFrom, SearchFunctionName));
}

const void* PlatformWindows::GetAddressOfExportedFunction(const char* /*SearchModuleName*/, const char* /*SearchFunctionName*/)
{
	// Not currently needed in external mode. The only Engine-layer users of "exported function"
	// lookup went through the IAT path instead. If this ends up being required, the implementation
	// is: read IMAGE_EXPORT_DIRECTORY at main-module NT headers directory entry 0, walk the
	// name table and ordinal table via hypercalls, return the resolved function address.
	std::cerr << "[Platform] GetAddressOfExportedFunction is not implemented in external mode v1\n";
	return nullptr;
}

template<bool bShouldResolve32BitJumps>
std::pair<const void*, int32_t> PlatformWindows::IterateVTableFunctions(void** VTable, const std::function<bool(const uint8_t* Address, int32_t Index)>& CallBackForEachFunc, int32_t NumFunctions, int32_t OffsetFromStart)
{
	if (!CallBackForEachFunc)
		return { nullptr, -1 };

	const uintptr_t vtableRemote = reinterpret_cast<uintptr_t>(VTable);

	for (int32_t i = 0; i < 0x150; ++i)
	{
		const uintptr_t funcAddress = RemoteMemory::Read<uintptr_t>(vtableRemote + (i * sizeof(void*)));
		if (funcAddress == 0 || !IsAddressInProcessRange(funcAddress))
			break;

		// Resolve an initial E9 relative-jump trampoline at the entry, if present and enabled.
		uintptr_t resolvedAddress = funcAddress;
		if constexpr (bShouldResolve32BitJumps)
		{
			const uint8_t firstByte = RemoteMemory::Read<uint8_t>(funcAddress);
			if (firstByte == 0xE9)
			{
				const int32_t disp = RemoteMemory::Read<int32_t>(funcAddress + 1);
				const uintptr_t target = funcAddress + 5 + disp;
				if (IsAddressInProcessRange(target))
					resolvedAddress = target;
			}
		}

		if (CallBackForEachFunc(reinterpret_cast<const uint8_t*>(resolvedAddress), i))
			return { reinterpret_cast<const void*>(resolvedAddress), i };
	}
	return { nullptr, -1 };
}


void* PlatformWindows::FindPattern(const char* Signature, const uint32_t Offset, const bool bSearchAllSections, const uintptr_t StartAddress, const char* const ModuleName)
{
	(void)ModuleName;
	const ModuleCache& cache = GetMainModuleCache();

	auto scanSection = [&](const SectionCache& sec) -> void* {
		if (!sec.LocalBytes)
			return nullptr;

		uintptr_t searchStart = sec.RemoteBase;
		uint32_t searchRange = sec.Size;
		if (StartAddress != 0)
		{
			if (StartAddress < sec.RemoteBase || StartAddress >= (sec.RemoteBase + sec.Size))
				return nullptr;
			searchStart = StartAddress;
			searchRange = static_cast<uint32_t>(sec.RemoteBase + sec.Size - StartAddress);
		}

		return FindPatternInRange(Signature,
			reinterpret_cast<const void*>(searchStart),
			searchRange,
			Offset != 0,
			Offset);
	};

	if (bSearchAllSections)
	{
		for (const SectionCache& sec : cache.Sections)
		{
			if (void* result = scanSection(sec))
				return result;
		}
		return nullptr;
	}

	const SectionCache* textSec = cache.FindSectionByName(".text");
	if (!textSec)
		return nullptr;
	return scanSection(*textSec);
}

void* PlatformWindows::FindPatternInRange(const char* Signature, const void* Start, const uintptr_t Range, const bool bRelative, const uint32_t Offset)
{
	return FindPatternInRange(ParsePatternString(Signature), Start, Range, bRelative, Offset);
}

void* PlatformWindows::FindPatternInRange(const char* Signature, const uintptr_t Start, const uintptr_t Range, const bool bRelative, const uint32_t Offset)
{
	return FindPatternInRange(Signature, reinterpret_cast<void*>(Start), Range, bRelative, Offset);
}

void* PlatformWindows::FindPatternInRange(std::vector<int>&& Signature, const void* Start, const uintptr_t Range, const bool bRelative, uint32_t Offset, const uint32_t SkipCount)
{
	const uintptr_t startRemoteVA = reinterpret_cast<uintptr_t>(Start);
	if (startRemoteVA == 0)
		return nullptr;

	size_t remainingInSection = 0;
	const uint8_t* localStart = RemoteToLocal(startRemoteVA, &remainingInSection);
	if (!localStart)
	{
		// The address isn't inside any cached section. This commonly happens when a previous
		// scan returned a remote VA that we just want to resume scanning from. Fall back to a
		// tight hypercall-backed read of a transient buffer covering the requested range.
		const size_t rangeSize = Range;
		if (rangeSize == 0)
			return nullptr;

		std::vector<uint8_t> transient(rangeSize);
		if (!RemoteMemory::ReadBuffer(startRemoteVA, transient.data(), rangeSize, RemoteMemory::PartialReadPolicy::ZeroFillOnGap))
			return nullptr;

		const size_t localMatch = FindPatternInLocalBuffer(Signature, transient.data(), rangeSize, SkipCount);
		if (localMatch == SIZE_MAX)
			return nullptr;

		uintptr_t matchRemote = startRemoteVA + localMatch;
		if (bRelative)
		{
			if (Offset == static_cast<uint32_t>(-1))
				Offset = static_cast<uint32_t>(Signature.size());
			const int32_t disp = *reinterpret_cast<const int32_t*>(transient.data() + localMatch + Offset);
			matchRemote = matchRemote + Offset + 4 + disp;
		}
		return reinterpret_cast<void*>(matchRemote);
	}

	// Scan in place inside the cached local buffer. Clamp the scan length to the remaining
	// section size so we don't walk off the end of the buffer.
	const size_t effectiveRange = (Range < remainingInSection) ? Range : remainingInSection;
	const size_t localMatch = FindPatternInLocalBuffer(Signature, localStart, effectiveRange, SkipCount);
	if (localMatch == SIZE_MAX)
		return nullptr;

	uintptr_t matchRemote = startRemoteVA + localMatch;
	if (bRelative)
	{
		if (Offset == static_cast<uint32_t>(-1))
			Offset = static_cast<uint32_t>(Signature.size());
		const int32_t disp = *reinterpret_cast<const int32_t*>(localStart + localMatch + Offset);
		matchRemote = matchRemote + Offset + 4 + disp;
	}
	return reinterpret_cast<void*>(matchRemote);
}


template<bool bCheckIfLeaIsStrPtr, typename CharType>
void* PlatformWindows::FindByStringInAllSections(const CharType* RefStr, const uintptr_t StartAddress, int32_t Range, const bool bSearchOnlyExecutableSections, const char* const ModuleName)
{
	static_assert(std::is_same_v<CharType, char> || std::is_same_v<CharType, wchar_t>,
		"FindByStringInAllSections only supports 'char' and 'wchar_t'.");

	(void)ModuleName;
	const ModuleCache& cache = GetMainModuleCache();

	for (const SectionCache& sec : cache.Sections)
	{
		if (bSearchOnlyExecutableSections && !(sec.Characteristics & IMAGE_SCN_MEM_EXECUTE))
			continue;
		if (!sec.LocalBytes)
			continue;

		uintptr_t searchStart = sec.RemoteBase;
		int32_t searchRange = static_cast<int32_t>(sec.Size);
		if (StartAddress != 0)
		{
			if (StartAddress < sec.RemoteBase || StartAddress >= (sec.RemoteBase + sec.Size))
				continue;
			searchStart = StartAddress;
			searchRange = static_cast<int32_t>(sec.RemoteBase + sec.Size - StartAddress);
		}
		if (Range > 0 && Range < searchRange)
			searchRange = Range;

		constexpr int32_t InstructionBytesLength = 0x7;
		searchRange -= InstructionBytesLength;
		if (searchRange <= 0)
			continue;

		if (void* result = FindStringInRange<bCheckIfLeaIsStrPtr, CharType>(RefStr, searchStart, searchRange))
			return result;
	}
	return nullptr;
}

template<bool bCheckIfLeaIsStrPtr, typename CharType>
void* PlatformWindows::FindStringInRange(const CharType* RefStr, const uintptr_t StartAddress, const int32_t Range)
{
	const uint8_t* localStart = RemoteToLocal(StartAddress);
	if (!localStart)
		return nullptr;

	const int32_t refStrLen = StrlenHelper(RefStr) + 1;

	for (int32_t i = 0; i < Range; ++i)
	{
#if defined(_WIN64)
		// LEA opcode
		if ((localStart[i] == 0x4C || localStart[i] == 0x48) && localStart[i + 1] == 0x8D)
		{
			const uintptr_t leaInstrRemoteVA = StartAddress + i;
			const int32_t disp = *reinterpret_cast<const int32_t*>(localStart + i + 3);
			const uintptr_t strPtr = leaInstrRemoteVA + 7 + disp;

			if (!IsAddressInProcessRange(strPtr))
				continue;

			// Translate the resolved string pointer into a local buffer of whatever section holds it.
			const uint8_t* localStrPtr = RemoteToLocal(strPtr);
			if (!localStrPtr)
				continue;

			if (StrnCmpHelper(RefStr, reinterpret_cast<const CharType*>(localStrPtr), refStrLen))
				return reinterpret_cast<void*>(leaInstrRemoteVA);

			if constexpr (bCheckIfLeaIsStrPtr)
			{
				// strPtr is itself a pointer-sized slot; dereference it to get a second-level pointer.
				const uintptr_t doubleDerefPtr = *reinterpret_cast<const uintptr_t*>(localStrPtr);
				if (!IsAddressInProcessRange(doubleDerefPtr))
					continue;
				const uint8_t* localDoubleDerefPtr = RemoteToLocal(doubleDerefPtr);
				if (!localDoubleDerefPtr)
					continue;
				if (StrnCmpHelper(RefStr, reinterpret_cast<const CharType*>(localDoubleDerefPtr), refStrLen))
					return reinterpret_cast<void*>(leaInstrRemoteVA);
			}
		}
#elif defined(_WIN32)
		if (localStart[i] == 0x68)
		{
			const uintptr_t pushInstrRemoteVA = StartAddress + i;
			const uintptr_t strPtr = *reinterpret_cast<const uintptr_t*>(localStart + i + 1);
			if (!IsAddressInProcessRange(strPtr))
				continue;
			const uint8_t* localStrPtr = RemoteToLocal(strPtr);
			if (!localStrPtr)
				continue;
			if (StrnCmpHelper(RefStr, reinterpret_cast<const CharType*>(localStrPtr), refStrLen))
				return reinterpret_cast<void*>(pushInstrRemoteVA);
		}
#endif
	}
	return nullptr;
}


void* WindowsPrivateImplHelper::FinAlignedValueInRangeImpl(const void* ValuePtr, ValueCompareFuncType ComparisonFunction, const int32_t ValueTypeSize, const int32_t Alignment, uintptr_t StartAddress, uint32_t Range)
{
	size_t remainingInSection = 0;
	const uint8_t* local = RemoteToLocal(StartAddress, &remainingInSection);
	if (!local)
		return nullptr;

	const uint32_t effectiveRange = (Range < remainingInSection) ? Range : static_cast<uint32_t>(remainingInSection);
	const int64_t sizeFromEnd = GetAlignedSizeWithOffsetFromEnd(effectiveRange, Alignment, ValueTypeSize);
	if (sizeFromEnd == -1)
		return nullptr;

	for (int64_t i = 0; i <= sizeFromEnd; i += Alignment)
	{
		if (ComparisonFunction(ValuePtr, local + i))
			return reinterpret_cast<void*>(StartAddress + i);
	}
	return nullptr;
}

void* WindowsPrivateImplHelper::FindAlignedValueInSectionImpl(const SectionInfo& Info, const void* ValuePtr, ValueCompareFuncType ComparisonFunction, const int32_t ValueTypeSize, const int32_t Alignment)
{
	const WindowsSectionInfo winSection = SectionInfoToWinSectionInfo(Info);
	if (winSection.RemoteBase == 0 || winSection.Size == 0)
		return nullptr;
	return FinAlignedValueInRangeImpl(ValuePtr, ComparisonFunction, ValueTypeSize, Alignment, winSection.RemoteBase, winSection.Size);
}

void* WindowsPrivateImplHelper::FindAlignedValueInAllSectionsImpl(const void* ValuePtr, ValueCompareFuncType ComparisonFunction, const int32_t ValueTypeSize, const int32_t Alignment, const uintptr_t StartAddress, int32_t Range, const char* const ModuleName)
{
	(void)ModuleName;
	const ModuleCache& cache = GetMainModuleCache();

	for (const SectionCache& sec : cache.Sections)
	{
		if (!(sec.Characteristics & IMAGE_SCN_MEM_READ) || !sec.LocalBytes)
			continue;

		uintptr_t sectionStart = sec.RemoteBase;
		uint32_t sectionRange = sec.Size;
		if (StartAddress != 0)
		{
			if (StartAddress < sec.RemoteBase || StartAddress >= (sec.RemoteBase + sec.Size))
				continue;
			sectionStart = StartAddress;
			sectionRange = static_cast<uint32_t>(sec.RemoteBase + sec.Size - StartAddress);
		}
		if (Range > 0 && static_cast<uint32_t>(Range) < sectionRange)
			sectionRange = static_cast<uint32_t>(Range);

		if (void* result = FinAlignedValueInRangeImpl(ValuePtr, ComparisonFunction, ValueTypeSize, Alignment, sectionStart, sectionRange))
			return result;
	}
	return nullptr;
}


// Explicit instantiations mirror the original file so the template definitions are linked in.
template void* PlatformWindows::FindByStringInAllSections<false, char>(const char*, const uintptr_t, int32_t, const bool, const char* const);
template void* PlatformWindows::FindByStringInAllSections<false, wchar_t>(const wchar_t*, const uintptr_t, int32_t, const bool, const char* const);
template void* PlatformWindows::FindByStringInAllSections<true, char>(const char*, const uintptr_t, int32_t, const bool, const char* const);
template void* PlatformWindows::FindByStringInAllSections<true, wchar_t>(const wchar_t*, const uintptr_t, int32_t, const bool, const char* const);

template std::pair<const void*, int32_t> PlatformWindows::IterateVTableFunctions<true>(void**, const std::function<bool(const uint8_t*, int32_t)>&, int32_t, int32_t);
template std::pair<const void*, int32_t> PlatformWindows::IterateVTableFunctions<false>(void**, const std::function<bool(const uint8_t*, int32_t)>&, int32_t, int32_t);
