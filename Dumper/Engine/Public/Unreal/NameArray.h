#pragma once

#include "Unreal/UnrealTypes.h"

class FNameEntry
{
private:
	friend class NameArray;

private:
	static constexpr int32 NameWideMask = 0x1;

private:
	static inline int32 FNameEntryLengthShiftCount = 0x0;

	static inline std::wstring(*GetStr)(uint8* NameEntry) = nullptr;

private:
	uint8* Address;

public:
	FNameEntry() = default;

	FNameEntry(void* Ptr);

public:
	std::wstring GetWString();
	std::string GetString();
	void* GetAddress();

private:
	//Optional to avoid code duplication for FNamePool
	static void Init(const uint8_t* FirstChunkPtr = nullptr, int64 NameEntryStringOffset = 0x0);
};

class NameArray
{
private:
	static inline uint32 FNameBlockOffsetBits = 0x10;

private:
	static uint8* GNames;

	static inline int64 NameEntryStride = 0x0;

	static inline void* (*ByIndex)(void* NamesArray, int32 ComparisonIndex, int32 NamePoolBlockOffsetBits) = nullptr;

private:
	static bool InitializeNameArray(uint8_t* NameArray);
	static bool InitializeNamePool(uint8_t* NamePool);

public:
	/* Should be changed later and combined */
	static bool TryFindNameArray_Windows();
	static bool TryFindNamePool_Windows();

	static bool TryInit(bool bIsTestOnly = false);
	static bool TryInit(int32 OffsetOverride, bool bIsNamePool, const char* const ModuleName = Settings::General::DefaultModuleName);

	/* Initializes the GNames offset, but doesn't call NameArray::InitializeNameArray() or NameArray::InitializedNamePool() */
	static bool SetGNamesWithoutCommitting();

	static void PostInit();
	
public:
	static int32 GetNumChunks();

	static int32 GetNumElements();
	static int32 GetByteCursor();

	static FNameEntry GetNameEntry(const void* Name);
	static FNameEntry GetNameEntry(int32 Idx);

	// FNamePool bulk cache: populated once at InitializeNamePool and used by the GetStr
	// fast path to resolve FNameEntries without issuing hypercalls.
	static const uint8_t* TryResolveLocalNameEntry(uintptr_t RemoteVA);
	static int32 GetCachedNumChunks();
	static int32 GetCachedByteCursor();
	// Cached chunk base pointer for a given chunk index, or 0 if the cache isn't populated
	// (or the chunk index is out of range).
	static uintptr_t GetCachedChunkBase(int32 ChunkIndex);
	static void PopulateFNamePoolCache(uintptr_t FNamePoolRemote, int32 NumChunks, int32 BlockOffsetBits, int32 Stride, int32 ByteCursor);
};
