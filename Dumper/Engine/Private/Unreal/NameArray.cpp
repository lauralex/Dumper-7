
#include <format>

#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"

#include "Platform.h"
#include "Architecture.h"

#include "RemoteMemory.h"

uint8* NameArray::GNames = nullptr;

FNameEntry::FNameEntry(void* Ptr)
	: Address((uint8*)Ptr)
{
}

std::wstring FNameEntry::GetWString()
{
	if (!Address)
		return L"";

	return GetStr(Address);
}

std::string FNameEntry::GetString()
{
	if (!Address)
		return "";

	return UtfN::WStringToString(GetWString());
}

void* FNameEntry::GetAddress()
{
	return Address;
}

void FNameEntry::Init(const uint8_t* FirstChunkPtr, int64 NameEntryStringOffset)
{
	if (Settings::Internal::bUseNamePool)
	{
		constexpr int64 NoneStrLen = 0x4;
		constexpr uint16 BytePropertyStrLen = 0xC;

		constexpr uint32 BytePropertyStartAsUint32 = 'etyB'; // "Byte" part of "ByteProperty"

		Off::FNameEntry::NamePool::StringOffset = NameEntryStringOffset;
		Off::FNameEntry::NamePool::HeaderOffset = NameEntryStringOffset == 6 ? 4 : 0;

		// FirstChunkPtr points at the chunks array header; first entry [0] is the pointer to
		// the first chunk (the "None" entry block). Read that pointer remotely, then scan
		// forward for the "ByteProperty" header to determine the shift count used to encode
		// the length field in the FNameEntry header.
		const uintptr_t firstChunkAddr = RDeref<uintptr_t>(FirstChunkPtr);
		uintptr_t candidate = firstChunkAddr + NameEntryStringOffset + NoneStrLen;

		for (int i = 0; i < 0x4; ++i)
		{
			const uint32 FirstPartOfByteProperty = RDeref<uint32>(candidate + NameEntryStringOffset);
			if (FirstPartOfByteProperty == BytePropertyStartAsUint32)
				break;
			candidate += 1;
		}

		uint16 BytePropertyHeader = RDeref<uint16>(candidate + Off::FNameEntry::NamePool::HeaderOffset);
		constexpr int32 MaxAllowedShiftCount = sizeof(BytePropertyHeader) * 0x8;

		while (BytePropertyHeader != BytePropertyStrLen && FNameEntryLengthShiftCount < MaxAllowedShiftCount)
		{
			FNameEntryLengthShiftCount++;
			BytePropertyHeader >>= 1;
		}

		if (FNameEntryLengthShiftCount == MaxAllowedShiftCount)
		{
			std::cerr << "\nDumper-7: Error, couldn't get FNameEntryLengthShiftCount!\n" << std::endl;
			GetStr = [](uint8* NameEntry) -> std::wstring { (void)NameEntry; return L"Invalid FNameEntryLengthShiftCount!"; };
			return;
		}

		GetStr = [](uint8* NameEntry) -> std::wstring
		{
			// NameEntry is a remote VA. Read the header word first, compute the string length,
			// then bulk-read the string payload into a local buffer.
			const uint16 HeaderWithoutNumber = RDeref<uint16>(NameEntry + Off::FNameEntry::NamePool::HeaderOffset);
			const int32 NameLen = HeaderWithoutNumber >> FNameEntry::FNameEntryLengthShiftCount;

			if (NameLen == 0)
			{
				const int32 EntryIdOffset = Off::FNameEntry::NamePool::StringOffset + ((Off::FNameEntry::NamePool::StringOffset == 6) * 2);

				const int32 NextEntryIndex = RDeref<int32>(NameEntry + EntryIdOffset);
				const int32 Number = RDeref<int32>(NameEntry + EntryIdOffset + sizeof(int32));

				if (Number > 0)
					return NameArray::GetNameEntry(NextEntryIndex).GetWString() + L'_' + std::to_wstring(Number - 1);

				return NameArray::GetNameEntry(NextEntryIndex).GetWString();
			}

			// Sanity clamp to avoid huge reads on corrupt pointers.
			const int32 clampedLen = (NameLen > 0x400) ? 0x400 : NameLen;
			const uintptr_t stringAddr = reinterpret_cast<uintptr_t>(NameEntry) + Off::FNameEntry::NamePool::StringOffset;

			if (HeaderWithoutNumber & NameWideMask)
			{
				std::wstring wide(clampedLen, L'\0');
				RemoteMemory::ReadBuffer(stringAddr, wide.data(), clampedLen * sizeof(wchar_t), RemoteMemory::PartialReadPolicy::ZeroFillOnGap);
				return wide;
			}

			std::string narrow(clampedLen, '\0');
			RemoteMemory::ReadBuffer(stringAddr, narrow.data(), clampedLen, RemoteMemory::PartialReadPolicy::ZeroFillOnGap);
			return UtfN::StringToWString(narrow);
		};
	}
	else
	{
		// Legacy TNameEntryArray path. External mode does not support this — but we can still
		// compute the offsets if somehow this path is reached via a manual override. The bytes
		// at FNameEntry[0] / [3] / [8] are read from the target via RemoteMemory.
		const uintptr_t FNameEntryNone     = reinterpret_cast<uintptr_t>(NameArray::GetNameEntry(0x0).GetAddress());
		const uintptr_t FNameEntryIdxThree = reinterpret_cast<uintptr_t>(NameArray::GetNameEntry(0x3).GetAddress());
		const uintptr_t FNameEntryIdxEight = reinterpret_cast<uintptr_t>(NameArray::GetNameEntry(0x8).GetAddress());

		for (int i = 0; i < 0x20; i++)
		{
			if (RDeref<uint32>(FNameEntryNone + i) == 'enoN') // None
			{
				Off::FNameEntry::NameArray::StringOffset = i;
				break;
			}
		}

		for (int i = 0; i < 0x20; i++)
		{
			if ((RDeref<uint32>(FNameEntryIdxThree + i) >> 1) == 0x3 &&
				(RDeref<uint32>(FNameEntryIdxEight + i) >> 1) == 0x8)
			{
				Off::FNameEntry::NameArray::IndexOffset = i;
				break;
			}
		}

		GetStr = [](uint8* NameEntry) -> std::wstring
		{
			const int32 NameIdx = RDeref<int32>(NameEntry + Off::FNameEntry::NameArray::IndexOffset);
			const uintptr_t stringAddr = reinterpret_cast<uintptr_t>(NameEntry) + Off::FNameEntry::NameArray::StringOffset;

			if (NameIdx & NameWideMask)
			{
				wchar_t buf[0x200]{};
				RemoteMemory::ReadBuffer(stringAddr, buf, sizeof(buf), RemoteMemory::PartialReadPolicy::ZeroFillOnGap);
				return std::wstring(buf);
			}

			char buf[0x200]{};
			RemoteMemory::ReadBuffer(stringAddr, buf, sizeof(buf), RemoteMemory::PartialReadPolicy::ZeroFillOnGap);
			return UtfN::StringToWString<std::string>(buf);
		};
	}
}

bool NameArray::InitializeNameArray(uint8_t* NameArrayPtr)
{
	// Legacy TNameEntryArray support. External mode does not support this shape (the original
	// discovery path called a function pointer in the target to verify the array). Leaving
	// the code structurally intact for completeness, but it is no longer exercised externally
	// because TryFindNameArray_Windows returns false below.
	int32 ValidPtrCount = 0x0;
	int32 ZeroQWordCount = 0x0;

	if (!NameArrayPtr || Platform::IsBadReadPtr(NameArrayPtr))
		return false;

	for (int i = 0; i < 0x800; i += sizeof(void*))
	{
		const uintptr_t SomePtrAddr = reinterpret_cast<uintptr_t>(NameArrayPtr) + i;
		const uintptr_t SomePtr = RDeref<uintptr_t>(SomePtrAddr);

		if (SomePtr == 0)
		{
			ZeroQWordCount++;
		}
		else if (ZeroQWordCount == 0x0)
		{
			ValidPtrCount++;
		}
		else if (ZeroQWordCount > 0)
		{
			const int32 NumElements = RDeref<int32>(SomePtrAddr);
			const int32 NumChunks = RDeref<int32>(SomePtrAddr + 4);
			(void)NumElements;

			if (NumChunks == ValidPtrCount)
			{
				Off::NameArray::NumElements = i;
				Off::NameArray::MaxChunkIndex = i + 4;

				ByIndex = [](void* NamesArray, int32 ComparisonIndex, int32 /*NamePoolBlockOffsetBits*/) -> void*
				{
					const int32 ChunkIdx = ComparisonIndex / 0x4000;
					const int32 InChunk = ComparisonIndex % 0x4000;

					if (ComparisonIndex > NameArray::GetNumElements())
						return nullptr;

					const uintptr_t arrayAddr = reinterpret_cast<uintptr_t>(NamesArray);
					const uintptr_t chunkPtr = RDeref<uintptr_t>(arrayAddr + ChunkIdx * sizeof(void*));
					return reinterpret_cast<void*>(RDeref<uintptr_t>(chunkPtr + InChunk * sizeof(void*)));
				};

				return true;
			}
		}
	}

	return false;
}

bool NameArray::InitializeNamePool(uint8_t* NamePool)
{
	Off::NameArray::MaxChunkIndex = 0x0;
	Off::NameArray::ByteCursor = 0x4;
	Off::NameArray::ChunksStart = 0x10;

	bool bWasMaxChunkIndexFound = false;

	for (int i = 0x0; i < 0x20; i += 4)
	{
		const int32 PossibleMaxChunkIdx = RDeref<int32>(NamePool + i);

		if (PossibleMaxChunkIdx <= 0 || PossibleMaxChunkIdx > 0x10000)
			continue;

		int32 NotNullptrCount = 0x0;
		bool bFoundFirstPtr = false;

		constexpr int32 MaxAllowedNumInvalidPtrs = 0x500;
		int32 NumPtrsSinceLastValid = 0x0;

		for (int j = 0x0; j < 0x10000; j += 8)
		{
			const int32 ChunkOffset = i + 8 + j + (i % 8);

			if (RDeref<uintptr_t>(NamePool + ChunkOffset) != 0)
			{
				NotNullptrCount++;
				NumPtrsSinceLastValid = 0;

				if (!bFoundFirstPtr)
				{
					bFoundFirstPtr = true;
					Off::NameArray::ChunksStart = i + 8 + j + (i % 8);
				}
			}
			else
			{
				NumPtrsSinceLastValid++;
				if (NumPtrsSinceLastValid == MaxAllowedNumInvalidPtrs)
					break;
			}
		}

		if (PossibleMaxChunkIdx == (NotNullptrCount - 1))
		{
			Off::NameArray::MaxChunkIndex = i;
			Off::NameArray::ByteCursor = i + 4;
			bWasMaxChunkIndexFound = true;
			break;
		}
	}

	if (!bWasMaxChunkIndexFound)
		return false;

	constexpr uint64 CoreUObjAsUint64 = 0x6A624F5565726F43; // little endian "jbOUeroC" ["/Script/CoreUObject"]
	constexpr uint32 NoneAsUint32 = 0x656E6F4E; // little endian "None"

	const uintptr_t chunkPtrRemoteAddr = reinterpret_cast<uintptr_t>(NamePool) + Off::NameArray::ChunksStart;
	const uintptr_t firstChunkAddr = RDeref<uintptr_t>(chunkPtrRemoteAddr);
	if (firstChunkAddr == 0)
		return false;

	bool bFoundCoreUObjectString = false;
	int64 FNameEntryHeaderSize = 0x0;

	constexpr int32 LoopLimit = 0x1000;

	for (int i = 0; i < LoopLimit; i++)
	{
		if (RDeref<uint32>(firstChunkAddr + i) == NoneAsUint32 && FNameEntryHeaderSize == 0)
		{
			FNameEntryHeaderSize = i;
		}
		else if (RDeref<uint64>(firstChunkAddr + i) == CoreUObjAsUint64)
		{
			bFoundCoreUObjectString = true;
			break;
		}
	}

	if (!bFoundCoreUObjectString)
		return false;

	NameEntryStride = FNameEntryHeaderSize == 2 ? 2 : 4;
	Off::InSDK::NameArray::FNameEntryStride = NameEntryStride;

	ByIndex = [](void* NamesArray, int32 ComparisonIndex, int32 NamePoolBlockOffsetBits) -> void*
	{
		const int32 ChunkIdx = ComparisonIndex >> NamePoolBlockOffsetBits;
		const int32 InChunkOffset = (ComparisonIndex & ((1 << NamePoolBlockOffsetBits) - 1)) * NameEntryStride;

		const bool bIsBeyondLastChunk = ChunkIdx == NameArray::GetNumChunks() && InChunkOffset > NameArray::GetByteCursor();

		if (ChunkIdx < 0 || ChunkIdx > GetNumChunks() || bIsBeyondLastChunk)
			return nullptr;

		const uintptr_t chunkArrayAddr = reinterpret_cast<uintptr_t>(NamesArray) + 0x10;
		const uintptr_t chunkBase = RDeref<uintptr_t>(chunkArrayAddr + ChunkIdx * sizeof(void*));
		return reinterpret_cast<void*>(chunkBase + InChunkOffset);
	};

	Settings::Internal::bUseNamePool = true;
	FNameEntry::Init(reinterpret_cast<uint8*>(chunkPtrRemoteAddr), FNameEntryHeaderSize);

	return true;
}


/*
 * Finds a call to FName::GetNames, OR a reference to GNames directly, if the call has been inlined.
 * returns { GetNames/GNames, bIsGNamesDirectly };
*/
inline std::pair<uintptr_t, bool> FindFNameGetNamesOrGNames_Windows(const uintptr_t EnterCriticalSectionAddress, const uintptr_t StartAddress)
{
#ifdef PLATFORM_WINDOWS

	// NOTE: this helper is only referenced by the legacy TNameEntryArray path which returns
	// early in external mode (TryFindNameArray_Windows returns false). Left intact so that
	// manual-override use cases can still reach it via TryInit(OffsetOverride, ...).

	constexpr int32 ASMRelativeCallSizeBytes = 0x6;
	constexpr int32 GetNamesCallSearchRange = 0x150;

	const uint8* BytePropertyStringAddress = static_cast<uint8*>(Platform::FindByStringInAllSections(L"ByteProperty", StartAddress, 0x0, Settings::General::bSearchOnlyExecutableSectionsForStrings));
	if (!BytePropertyStringAddress)
		return { 0x0, false };

	for (int i = 0; i < GetNamesCallSearchRange; i++)
	{
		const uint8 byteBefore = RDeref<uint8>(BytePropertyStringAddress - i);
		if (byteBefore != 0xFF)
			continue;

#if defined(_WIN64)
		const uintptr_t CallTarget = Architecture_x86_64::Resolve32BitSectionRelativeCall(reinterpret_cast<uintptr_t>(BytePropertyStringAddress - i));
#elif defined(_WIN32)
		uintptr_t CallTarget = Architecture_x86_64::Resolve32bitAbsoluteCall(reinterpret_cast<uintptr_t>(BytePropertyStringAddress - i));
#endif

		if (CallTarget != EnterCriticalSectionAddress)
			continue;

		const uintptr_t InstructionAfterCall = reinterpret_cast<uintptr_t>(BytePropertyStringAddress - (i - ASMRelativeCallSizeBytes));

		if (RDeref<uint8>(InstructionAfterCall) == 0xE8)
			return { Architecture_x86_64::Resolve32BitRelativeCall(InstructionAfterCall), false };

#if defined(_WIN64)
		return { Architecture_x86_64::Resolve32BitRelativeMove(InstructionAfterCall), true };
#elif defined(_WIN32)
		return { Architecture_x86_64::Resolve32bitAbsoluteMove(InstructionAfterCall), true };
#endif
	}

	return FindFNameGetNamesOrGNames_Windows(EnterCriticalSectionAddress, reinterpret_cast<uintptr_t>(BytePropertyStringAddress) + ASMRelativeCallSizeBytes);

#endif // PLATFORM_WINDOWS
};

bool NameArray::TryFindNameArray_Windows()
{
	// External mode disables the legacy TNameEntryArray path. It depends on reinterpret_cast
	// to a function pointer in the target and actually *calling* it (NameArray.cpp:393 in the
	// original code), which cannot work when the dumper runs outside the target address space.
	// UE4.11–UE4.22 games using TNameEntryArray are not supported in v1 external dumps.
	std::cerr << "[NameArray] Skipping TryFindNameArray_Windows in external mode; FNamePool (UE4.23+) only.\n";
	return false;
}

bool NameArray::TryFindNamePool_Windows()
{
#ifdef PLATFORM_WINDOWS

	constexpr int32 InitSRWLockSearchRange = 0x50;
	constexpr int32 BytePropertySearchRange = 0x2A0;

	const uintptr_t InitSRWLockAddress = reinterpret_cast<uintptr_t>(Platform::GetAddressOfImportedFunctionFromAnyModule("kernel32.dll", "InitializeSRWLock"));
	const uintptr_t RtlInitSRWLockAddress = reinterpret_cast<uintptr_t>(Platform::GetAddressOfImportedFunctionFromAnyModule("ntdll.dll", "RtlInitializeSRWLock"));

	std::cerr << std::format("[NameArray] InitSRWLock IAT target:     0x{:X}\n", InitSRWLockAddress);
	std::cerr << std::format("[NameArray] RtlInitSRWLock IAT target:  0x{:X}\n", RtlInitSRWLockAddress);

	void* NamePoolIntance = nullptr;
	uintptr_t SigOccurrence = 0x0;
	int patternMatchCount = 0;
	int ctorInRangeCount = 0;
	int srwCallMatchCount = 0;

	while (!NamePoolIntance)
	{
		if (SigOccurrence > 0x0)
			SigOccurrence += 0x1;

		SigOccurrence = reinterpret_cast<uintptr_t>(Platform::FindPattern("48 8D 0D ? ? ? ? E8", 0x0, true, SigOccurrence));
		if (SigOccurrence == 0x0)
			break;
		++patternMatchCount;

		constexpr int32 SizeOfMovInstructionBytes = 0x7;
		const uintptr_t PossibleConstructorAddress = Architecture_x86_64::Resolve32BitRelativeCall(SigOccurrence + SizeOfMovInstructionBytes);

		if (!Platform::IsAddressInProcessRange(PossibleConstructorAddress))
			continue;
		++ctorInRangeCount;

		for (int i = 0; i < InitSRWLockSearchRange; i++)
		{
			if (RDeref<uint16>(PossibleConstructorAddress + i) != 0x15FF)
				continue;

			const uintptr_t RelativeCallTarget = Architecture_x86_64::Resolve32BitSectionRelativeCall(PossibleConstructorAddress + i);
			if (!Platform::IsAddressInProcessRange(RelativeCallTarget))
				continue;

			const uintptr_t ValueOfCallTarget = RDeref<uintptr_t>(RelativeCallTarget);
			if (ValueOfCallTarget != InitSRWLockAddress && ValueOfCallTarget != RtlInitSRWLockAddress)
				continue;
			++srwCallMatchCount;

			const void* StringRef = Platform::FindByStringInAllSections(L"ByteProperty", PossibleConstructorAddress, BytePropertySearchRange, Settings::General::bSearchOnlyExecutableSectionsForStrings);
			if (StringRef == nullptr)
				StringRef = Platform::FindByStringInAllSections("ByteProperty", PossibleConstructorAddress, BytePropertySearchRange, Settings::General::bSearchOnlyExecutableSectionsForStrings);

			if (StringRef)
			{
				NamePoolIntance = reinterpret_cast<void*>(Architecture_x86_64::Resolve32BitRelativeMove(SigOccurrence));
				break;
			}
		}
	}

	std::cerr << std::format("[NameArray] LEA+call pattern hits: {}, ctor-in-range: {}, SRW-match: {}\n",
		patternMatchCount, ctorInRangeCount, srwCallMatchCount);

	if (NamePoolIntance)
	{
		Off::InSDK::NameArray::GNames = Platform::GetOffset(NamePoolIntance);
		return true;
	}

	return false;

#endif // PLATFORM_WINDOWS
}

bool NameArray::TryInit(bool bIsTestOnly)
{
	const uintptr_t ImageBase = Platform::GetModuleBase();

	uint8* GNamesAddress = nullptr;
	bool bFoundNameArray = false;
	bool bFoundnamePool = false;

	if (CALL_PLATFORM_SPECIFIC_FUNCTION(NameArray::TryFindNameArray))
	{
		std::cerr << std::format("Found 'TNameEntryArray GNames' at offset 0x{:X}\n", Off::InSDK::NameArray::GNames) << std::endl;
		GNamesAddress = reinterpret_cast<uint8*>(RDeref<uintptr_t>(ImageBase + Off::InSDK::NameArray::GNames));
		Settings::Internal::bUseNamePool = false;
		bFoundNameArray = true;
	}
	else if (CALL_PLATFORM_SPECIFIC_FUNCTION(NameArray::TryFindNamePool))
	{
		std::cerr << std::format("Found 'FNamePool GNames' at offset 0x{:X}\n", Off::InSDK::NameArray::GNames) << std::endl;
		GNamesAddress = reinterpret_cast<uint8*>(ImageBase + Off::InSDK::NameArray::GNames);
		Settings::Internal::bUseNamePool = true;
		bFoundnamePool = true;
	}

	if (!bFoundNameArray && !bFoundnamePool)
	{
		std::cerr << "\n\nCould not find GNames!\n\n" << std::endl;
		return false;
	}

	if (bIsTestOnly)
		return false;

	if (bFoundNameArray && NameArray::InitializeNameArray(GNamesAddress))
	{
		GNames = GNamesAddress;
		Settings::Internal::bUseNamePool = false;
		FNameEntry::Init();
		return true;
	}
	else if (bFoundnamePool && NameArray::InitializeNamePool(reinterpret_cast<uint8_t*>(GNamesAddress)))
	{
		GNames = GNamesAddress;
		Settings::Internal::bUseNamePool = true;
		return true;
	}

	std::cerr << "The address that was found couldn't be used by the generator, this might be due to GNames-encryption.\n" << std::endl;
	return false;
}


bool NameArray::TryInit(int32 OffsetOverride, bool bIsNamePool, const char* const ModuleName)
{
	const uintptr_t ImageBase = Platform::GetModuleBase(ModuleName);

	uint8* GNamesAddress = nullptr;

	const bool bIsNameArrayOverride = !bIsNamePool;
	const bool bIsNamePoolOverride = bIsNamePool;

	bool bFoundNameArray = false;
	bool bFoundnamePool = false;

	Off::InSDK::NameArray::GNames = OffsetOverride;

	if (bIsNameArrayOverride)
	{
		std::cerr << std::format("Overwrote offset: 'TNameEntryArray GNames' set as offset 0x{:X}\n", Off::InSDK::NameArray::GNames) << std::endl;
		GNamesAddress = reinterpret_cast<uint8*>(RDeref<uintptr_t>(ImageBase + Off::InSDK::NameArray::GNames));
		Settings::Internal::bUseNamePool = false;
		bFoundNameArray = true;
	}
	else if (bIsNamePoolOverride)
	{
		std::cerr << std::format("Overwrote offset: 'FNamePool GNames' set as offset 0x{:X}\n", Off::InSDK::NameArray::GNames) << std::endl;
		GNamesAddress = reinterpret_cast<uint8*>(ImageBase + Off::InSDK::NameArray::GNames);
		Settings::Internal::bUseNamePool = true;
		bFoundnamePool = true;
	}

	if (!bFoundNameArray && !bFoundnamePool)
	{
		std::cerr << "\n\nCould not find GNames!\n\n" << std::endl;
		return false;
	}

	if (bFoundNameArray && NameArray::InitializeNameArray(GNamesAddress))
	{
		GNames = GNamesAddress;
		Settings::Internal::bUseNamePool = false;
		FNameEntry::Init();
		return true;
	}
	else if (bFoundnamePool && NameArray::InitializeNamePool(reinterpret_cast<uint8_t*>(GNamesAddress)))
	{
		GNames = GNamesAddress;
		Settings::Internal::bUseNamePool = true;
		return true;
	}

	std::cerr << "The address was overwritten, but couldn't be used. This might be due to GNames-encryption.\n" << std::endl;
	return false;
}

bool NameArray::SetGNamesWithoutCommitting()
{
	if (Off::InSDK::NameArray::GNames != 0x0)
		return false;

	if (CALL_PLATFORM_SPECIFIC_FUNCTION(NameArray::TryFindNameArray))
	{
		std::cerr << std::format("Found 'TNameEntryArray GNames' at offset 0x{:X}\n", Off::InSDK::NameArray::GNames) << std::endl;
		Settings::Internal::bUseNamePool = false;
		return true;
	}
	else if (CALL_PLATFORM_SPECIFIC_FUNCTION(NameArray::TryFindNamePool))
	{
		std::cerr << std::format("Found 'FNamePool GNames' at offset 0x{:X}\n", Off::InSDK::NameArray::GNames) << std::endl;
		Settings::Internal::bUseNamePool = true;
		return true;
	}

	std::cerr << "\n\nCould not find GNames!\n\n" << std::endl;
	return false;
}

void NameArray::PostInit()
{
	if (GNames && Settings::Internal::bUseNamePool)
	{
		NameArray::FNameBlockOffsetBits = 0xE;

		int i = ObjectArray::Num();
		while (i >= 0)
		{
			const int32 CurrentBlock = NameArray::GetNumChunks();

			UEObject Obj = ObjectArray::GetByIndex(i);

			if (!Obj)
			{
				i--;
				continue;
			}

			const int32 ObjNameChunkIdx = Obj.GetFName().GetCompIdx() >> NameArray::FNameBlockOffsetBits;

			if (ObjNameChunkIdx == CurrentBlock)
				break;

			if (ObjNameChunkIdx > CurrentBlock)
			{
				NameArray::FNameBlockOffsetBits++;
				i = ObjectArray::Num();
			}

			i--;
		}
		Off::InSDK::NameArray::FNamePoolBlockOffsetBits = NameArray::FNameBlockOffsetBits;

		std::cerr << "NameArray::FNameBlockOffsetBits: 0x" << std::hex << NameArray::FNameBlockOffsetBits << "\n" << std::endl;
	}
}

int32 NameArray::GetNumChunks()
{
	return RDeref<int32>(GNames + Off::NameArray::MaxChunkIndex);
}

int32 NameArray::GetNumElements()
{
	return !Settings::Internal::bUseNamePool ? RDeref<int32>(GNames + Off::NameArray::NumElements) : 0;
}

int32 NameArray::GetByteCursor()
{
	return Settings::Internal::bUseNamePool ? RDeref<int32>(GNames + Off::NameArray::ByteCursor) : 0;
}

FNameEntry NameArray::GetNameEntry(const void* Name)
{
	return ByIndex(GNames, FName(Name).GetCompIdx(), FNameBlockOffsetBits);
}

FNameEntry NameArray::GetNameEntry(int32 Idx)
{
	return ByIndex(GNames, Idx, FNameBlockOffsetBits);
}
