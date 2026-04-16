
#include <iostream>
#include <fstream>
#include <format>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>

#include "Unreal/ObjectArray.h"
#include "OffsetFinder/Offsets.h"
#include "Utils.h"

#include "Platform.h"
#include "RemoteMemory.h"


namespace fs = std::filesystem;

// External-mode chunk cache: each GetByIndex used to issue 3 hypercalls (chunks-table ptr,
// chunk base, item pointer). For games with ~100k+ UObjects and multiple full iteration
// passes this dominated wall-clock time. We now bulk-read each chunk (~1.5 MB) on first
// touch and serve subsequent lookups from the local buffer. Num() is also cached so the
// iterator's null-skip loop doesn't issue a hypercall per step.
namespace
{
	struct ChunkCacheEntry
	{
		uintptr_t RemoteBase = 0;
		std::vector<uint8_t> Data;
	};

	std::vector<ChunkCacheEntry> g_ChunkCache;
	int32 g_CachedNum = -1;
	int32 g_CachedMax = -1;
	int32 g_CachedNumChunks = -1;
	int32 g_CachedMaxChunks = -1;

	void PopulateChunkTable(uintptr_t ChunksTableVA, int32 NumChunks)
	{
		g_ChunkCache.clear();
		g_ChunkCache.resize(NumChunks);

		std::vector<uintptr_t> chunkBases(NumChunks);
		RemoteMemory::ReadBuffer(ChunksTableVA, chunkBases.data(), NumChunks * sizeof(uintptr_t),
			RemoteMemory::PartialReadPolicy::ErrorOnGap);
		for (int32 i = 0; i < NumChunks; ++i)
			g_ChunkCache[i].RemoteBase = chunkBases[i];
	}

	const uint8_t* EnsureChunkCached(int32 ChunkIndex, size_t ChunkSizeBytes)
	{
		if (ChunkIndex < 0 || ChunkIndex >= static_cast<int32>(g_ChunkCache.size()))
			return nullptr;
		auto& entry = g_ChunkCache[ChunkIndex];
		if (entry.RemoteBase == 0)
			return nullptr;
		if (entry.Data.empty())
		{
			entry.Data.resize(ChunkSizeBytes);
			RemoteMemory::ReadBuffer(entry.RemoteBase, entry.Data.data(), ChunkSizeBytes,
				RemoteMemory::PartialReadPolicy::ZeroFillOnGap);
		}
		return entry.Data.data();
	}
}

constexpr inline std::array FFixedUObjectArrayLayouts =
{
	FFixedUObjectArrayLayout // Default UE4.11 - UE4.20
	{
		.ObjectsOffset = 0x0,								// 0x00
		.MaxObjectsOffset = sizeof(void*),					// 0x08 (64bit) OR 0x04 (32bit)
		.NumObjectsOffset = sizeof(void*) + sizeof(int)		// 0x0C (64bit) OR 0x08 (32bit)
	}
};

constexpr inline std::array FChunkedFixedUObjectArrayLayouts =
{
	FChunkedFixedUObjectArrayLayout // Default UE4.21 and above
	{
		.ObjectsOffset = 0x00,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x14,
		.MaxChunksOffset = 0x18,
		.NumChunksOffset = 0x1C,
	},
	FChunkedFixedUObjectArrayLayout // Back4Blood
	{
		.ObjectsOffset = 0x10, // last
		.MaxElementsOffset = 0x00,
		.NumElementsOffset = 0x04,
		.MaxChunksOffset = 0x08,
		.NumChunksOffset = 0x0C,
	},
	FChunkedFixedUObjectArrayLayout // Mutliversus
	{
		.ObjectsOffset = 0x18,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x00, // first
		.MaxChunksOffset = 0x14,
		.NumChunksOffset = 0x20,
	},
	FChunkedFixedUObjectArrayLayout // MindsEye
	{
		.ObjectsOffset = 0x18,
		.MaxElementsOffset = 0x00, // first
		.NumElementsOffset = 0x14,
		.MaxChunksOffset = 0x10,
		.NumChunksOffset = 0x04,
	}
};

bool IsAddressValidGObjects(const uintptr_t Address, const FFixedUObjectArrayLayout& Layout)
{
	// External-mode rewrite: all header reads go through RemoteMemory::TryRead so a hypercall
	// fault on an unmapped candidate address surfaces as failure rather than producing a
	// zero-filled struct that might pass the loose validation below.
	struct FUObjectItem
	{
		void* Object;
		uint8_t Pad[sizeof(void*) * 2];
	};

	auto objectsOpt = RemoteMemory::TryRead<uintptr_t>(Address + Layout.ObjectsOffset);
	auto maxOpt = RemoteMemory::TryRead<int32>(Address + Layout.MaxObjectsOffset);
	auto numOpt = RemoteMemory::TryRead<int32>(Address + Layout.NumObjectsOffset);
	if (!objectsOpt || !maxOpt || !numOpt)
		return false;

	const int32 MaxElements = *maxOpt;
	const int32 NumElements = *numOpt;
	const uintptr_t ObjectsButDecrypted = reinterpret_cast<uintptr_t>(
		ObjectArray::DecryptPtr(reinterpret_cast<void*>(*objectsOpt)));

	if (NumElements > MaxElements)
		return false;
	if (MaxElements > 0x400000)
		return false;
	if (NumElements < 0x1000)
		return false;
	if (Platform::IsBadReadPtr(ObjectsButDecrypted))
		return false;

	// FifthObject->Object field, then read through it to get InternalIndex.
	auto fifthObjOpt = RemoteMemory::TryRead<uintptr_t>(ObjectsButDecrypted + 0x5 * sizeof(FUObjectItem));
	if (!fifthObjOpt || *fifthObjOpt == 0)
		return false;

	// FifthObject -> InternalIndex at offset sizeof(void*) + sizeof(int32)
	auto indexOpt = RemoteMemory::TryRead<int32>(*fifthObjOpt + sizeof(void*) + sizeof(int32));
	if (!indexOpt || *indexOpt != 0x5)
		return false;

	return true;
}

bool IsAddressValidGObjects(const uintptr_t Address, const FChunkedFixedUObjectArrayLayout& Layout)
{
	auto objectsOpt = RemoteMemory::TryRead<uintptr_t>(Address + Layout.ObjectsOffset);
	auto maxElemOpt = RemoteMemory::TryRead<int32>(Address + Layout.MaxElementsOffset);
	auto numElemOpt = RemoteMemory::TryRead<int32>(Address + Layout.NumElementsOffset);
	auto maxChunksOpt = RemoteMemory::TryRead<int32>(Address + Layout.MaxChunksOffset);
	auto numChunksOpt = RemoteMemory::TryRead<int32>(Address + Layout.NumChunksOffset);
	if (!objectsOpt || !maxElemOpt || !numElemOpt || !maxChunksOpt || !numChunksOpt)
		return false;

	const int32 MaxElements = *maxElemOpt;
	const int32 NumElements = *numElemOpt;
	const int32 MaxChunks = *maxChunksOpt;
	const int32 NumChunks = *numChunksOpt;

	const uintptr_t ObjectsPtrButDecrypted = reinterpret_cast<uintptr_t>(
		ObjectArray::DecryptPtr(reinterpret_cast<void*>(*objectsOpt)));

	if (NumChunks > 0x14 || NumChunks < 0x1)
		return false;
	if (MaxChunks > 0x5FF || MaxChunks < 0x6)
		return false;
	if (NumElements <= 0x800 || MaxElements <= 0x10000)
		return false;
	if (NumElements > MaxElements || NumChunks > MaxChunks)
		return false;
	if ((MaxElements % 0x10) != 0)
		return false;

	const int32_t ElementsPerChunk = MaxElements / MaxChunks;
	if ((ElementsPerChunk % 0x10) != 0)
		return false;
	if (ElementsPerChunk < 0x8000 || ElementsPerChunk > 0x80000)
		return false;

	const bool bNumChunksFitsNumElements = ((NumElements / ElementsPerChunk) + 1) == NumChunks;
	if (!bNumChunksFitsNumElements)
		return false;

	const bool bMaxChunksFitsMaxElements = (MaxElements / ElementsPerChunk) == MaxChunks;
	if (!bMaxChunksFitsMaxElements)
		return false;

	if (ObjectsPtrButDecrypted == 0 || Platform::IsBadReadPtr(ObjectsPtrButDecrypted))
		return false;

	for (int i = 0; i < NumChunks; i++)
	{
		auto chunkOpt = RemoteMemory::TryRead<uintptr_t>(ObjectsPtrButDecrypted + i * sizeof(void*));
		if (!chunkOpt || *chunkOpt == 0 || Platform::IsBadReadPtr(*chunkOpt))
			return false;
	}

	return true;
}


void ObjectArray::InitializeFUObjectItem(uint8_t* FirstItemPtr)
{
	for (int i = 0x0; i < 0x20; i += 4)
	{
		const uintptr_t slotAddr = reinterpret_cast<uintptr_t>(FirstItemPtr) + i;
		const uintptr_t slotValue = RDeref<uintptr_t>(slotAddr);
		if (slotValue != 0 && !Platform::IsBadReadPtr(slotValue))
		{
			FUObjectItemInitialOffset = i;
			break;
		}
	}

	for (int i = FUObjectItemInitialOffset + sizeof(void*); i <= 0x38; i += 4)
	{
		const uintptr_t firstBase = reinterpret_cast<uintptr_t>(FirstItemPtr);
		const uintptr_t SecondObject = RDeref<uintptr_t>(firstBase + i);
		const uintptr_t ThirdObject  = RDeref<uintptr_t>(firstBase + (i * 2) - FUObjectItemInitialOffset);

		if (SecondObject && !Platform::IsBadReadPtr(SecondObject)
			&& RDeref<uintptr_t>(SecondObject) && !Platform::IsBadReadPtr(RDeref<uintptr_t>(SecondObject))
			&& ThirdObject && !Platform::IsBadReadPtr(ThirdObject)
			&& RDeref<uintptr_t>(ThirdObject) && !Platform::IsBadReadPtr(RDeref<uintptr_t>(ThirdObject)))
		{
			SizeOfFUObjectItem = i - FUObjectItemInitialOffset;
			break;
		}
	}

	Off::InSDK::ObjArray::FUObjectItemInitialOffset = FUObjectItemInitialOffset;
	Off::InSDK::ObjArray::FUObjectItemSize = SizeOfFUObjectItem;

	std::cerr << "Off::InSDK::ObjArray::FUObjectItemSize: " << Off::InSDK::ObjArray::FUObjectItemSize << "\n" << std::endl;
}

void ObjectArray::InitDecryption(uint8_t* (*DecryptionFunction)(void* ObjPtr), const char* DecryptionLambdaAsStr)
{
	DecryptPtr = DecryptionFunction;
	DecryptionLambdaStr = DecryptionLambdaAsStr;
}


/* We don't speak about this function... */
void ObjectArray::Init(bool bScanAllMemory, const char* const ModuleName)
{
	if (!bScanAllMemory)
	{
		std::cerr << "\nDumper-7 by me, you & him\n\n\n";
		std::cerr << "Searching for GObjects...\n\n";
	}

	auto MatchesAnyLayout = []<typename ArrayLayoutType, size_t Size>(const std::array<ArrayLayoutType, Size>& ObjectArrayLayouts, uintptr_t Address)
	{
		for (const ArrayLayoutType& Layout : ObjectArrayLayouts)
		{
			if (!IsAddressValidGObjects(Address, Layout))
				continue;

			if constexpr (std::is_same_v<ArrayLayoutType, FFixedUObjectArrayLayout>)
			{
				Off::FUObjectArray::bIsChunked = false;
				Off::FUObjectArray::FixedLayout = Layout;
			}
			else
			{
				Off::FUObjectArray::bIsChunked = true;
				Off::FUObjectArray::ChunkedFixedLayout = Layout;
			}

			return true;
		}
		
		return false;
	};

	bool bIsGObjectsChunked = false;
	auto IsAddressValidGObjects = [MatchesAnyLayout, &bIsGObjectsChunked](const void* CurrentAddress) -> bool
	{
		//std::cerr << "checking addr: " << CurrentAddress << "\n";
		if (MatchesAnyLayout(FFixedUObjectArrayLayouts, reinterpret_cast<uintptr_t>(CurrentAddress)))
		{
			bIsGObjectsChunked = false;
			return true;
		}
		else if (MatchesAnyLayout(FChunkedFixedUObjectArrayLayouts, reinterpret_cast<uintptr_t>(CurrentAddress)))
		{
			bIsGObjectsChunked = true;
			return true;
		}

		return false;
	};

	void* GObjectsAddress = nullptr;

	if (bScanAllMemory)
	{
		GObjectsAddress = Platform::IterateAllSectionsWithCallback(IsAddressValidGObjects, 0x4, 0x50, ModuleName);
	}
	else
	{
		GObjectsAddress = Platform::IterateSectionWithCallback(Platform::GetSectionInfo(".data"), IsAddressValidGObjects, 0x4, 0x50);
	}


	if (GObjectsAddress)
	{
		if (!bIsGObjectsChunked)
		{
			GObjects = static_cast<uint8*>(GObjectsAddress);
			NumElementsPerChunk = -1;

			Off::InSDK::ObjArray::GObjects = Platform::GetOffset(GObjectsAddress);

			std::cerr << "Found FFixedUObjectArray GObjects at offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n\n";

			ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 /*PerChunk*/) -> void*
			{
				if (Index < 0 || Index > Num())
					return nullptr;

				// ObjectsArray is a remote VA pointing at the first slot of the FUObjectArray;
				// *ObjectsArray is the remote pointer to the flat FUObjectItem buffer.
				uint8_t* ChunkPtr = DecryptPtr(reinterpret_cast<void*>(
					RDeref<uintptr_t>(reinterpret_cast<uintptr_t>(ObjectsArray))));

				return reinterpret_cast<void*>(
					RDeref<uintptr_t>(reinterpret_cast<uintptr_t>(ChunkPtr) + FUObjectItemOffset + (Index * FUObjectItemSize)));
			};

			uint8_t* FirstItem = DecryptPtr(reinterpret_cast<void*>(
				RDeref<uintptr_t>(GObjects + Off::FUObjectArray::GetObjectsOffset())));

			ObjectArray::InitializeFUObjectItem(FirstItem);
		}
		else
		{
			GObjects = static_cast<uint8*>(GObjectsAddress);

			NumElementsPerChunk = Max() / MaxChunks();
			Off::InSDK::ObjArray::ChunkSize = NumElementsPerChunk;

			SizeOfFUObjectItem = sizeof(void*) + sizeof(int32) + sizeof(int32);
			FUObjectItemInitialOffset = 0x0;

			Off::InSDK::ObjArray::GObjects = Platform::GetOffset(GObjectsAddress);

			std::cerr << "Found FChunkedFixedUObjectArray GObjects at offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n\n";

			uint8_t* ChunksPtr = DecryptPtr(reinterpret_cast<void*>(
				RDeref<uintptr_t>(GObjects + Off::FUObjectArray::GetObjectsOffset())));

			// Cache scalar header fields and the per-chunk base pointers up-front so subsequent
			// GetByIndex/Num calls don't issue redundant hypercalls for values that don't change
			// once the game's object array is settled (running against an idle/paused target).
			g_CachedNum = RDeref<int32>(GObjects + Off::FUObjectArray::GetNumElementsOffset());
			g_CachedMax = RDeref<int32>(GObjects + Off::FUObjectArray::GetMaxElementsOffset());
			g_CachedNumChunks = RDeref<int32>(GObjects + Off::FUObjectArray::GetNumChunksOffset());
			g_CachedMaxChunks = RDeref<int32>(GObjects + Off::FUObjectArray::GetMaxChunksOffset());
			PopulateChunkTable(reinterpret_cast<uintptr_t>(ChunksPtr), g_CachedNumChunks);

			ByIndex = [](void* /*ObjectsArray*/, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
			{
				if (Index < 0 || Index >= g_CachedNum)
					return nullptr;

				const int32 ChunkIndex = Index / PerChunk;
				const int32 InChunkIdx = Index % PerChunk;
				const size_t ChunkBytes = static_cast<size_t>(PerChunk) * FUObjectItemSize;

				const uint8_t* Chunk = EnsureChunkCached(ChunkIndex, ChunkBytes);
				if (!Chunk)
					return nullptr;

				const uint8_t* ItemPtr = Chunk + (InChunkIdx * FUObjectItemSize) + FUObjectItemOffset;
				return reinterpret_cast<void*>(*reinterpret_cast<const uintptr_t*>(ItemPtr));
			};

			// InitializeFUObjectItem probes the first chunk directly via hypercalls; the cache is
			// for subsequent lookups. Pass the first chunk's remote base to it.
			if (!g_ChunkCache.empty())
				ObjectArray::InitializeFUObjectItem(reinterpret_cast<uint8_t*>(g_ChunkCache[0].RemoteBase));
		}

		return;
	}

	if (!bScanAllMemory)
	{
		ObjectArray::Init(true);
		return;
	}

	if (GObjects == nullptr)
	{
		std::cerr << "\nGObjects couldn't be found, please overwrite the offset in Generator.cpp.\n\n\n";
		Sleep(10000);
		exit(1);
	}
}

void ObjectArray::Init(int32 GObjectsOffset, const FFixedUObjectArrayLayout& ObjectArrayLayout, const char* const ModuleName)
{
	GObjects = reinterpret_cast<uint8_t*>(Platform::GetModuleBase(ModuleName) + GObjectsOffset);
	Off::InSDK::ObjArray::GObjects = GObjectsOffset;

	std::cerr << "GObjects: 0x" << (void*)GObjects << "\n" << std::endl;

	Off::FUObjectArray::bIsChunked = false;
	Off::FUObjectArray::FixedLayout = ObjectArrayLayout.IsValid() ? ObjectArrayLayout : FFixedUObjectArrayLayouts[0];

	ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 /*PerChunk*/) -> void*
	{
		if (Index < 0 || Index > Num())
			return nullptr;

		const uintptr_t ItemsBase = RDeref<uintptr_t>(reinterpret_cast<uintptr_t>(ObjectsArray));
		const uintptr_t ItemPtr = ItemsBase + (Index * FUObjectItemSize);
		return reinterpret_cast<void*>(RDeref<uintptr_t>(ItemPtr + FUObjectItemOffset));
	};

	uint8_t* ChunksPtr = DecryptPtr(reinterpret_cast<void*>(
		RDeref<uintptr_t>(GObjects + Off::FUObjectArray::GetObjectsOffset())));

	std::cerr << "Overwrote FFixedUObjectArray GObjects to offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n" << std::endl;

	ObjectArray::InitializeFUObjectItem(reinterpret_cast<uint8_t*>(RDeref<uintptr_t>(reinterpret_cast<uintptr_t>(ChunksPtr))));
}

void ObjectArray::Init(int32 GObjectsOffset, int32 ElementsPerChunk, const FChunkedFixedUObjectArrayLayout& ObjectArrayLayout, const char* const ModuleName)
{
	GObjects = reinterpret_cast<uint8_t*>(Platform::GetModuleBase(ModuleName) + GObjectsOffset);
	Off::InSDK::ObjArray::GObjects = GObjectsOffset;

	Off::FUObjectArray::bIsChunked = true;
	Off::FUObjectArray::ChunkedFixedLayout = ObjectArrayLayout.IsValid() ? ObjectArrayLayout : FChunkedFixedUObjectArrayLayouts[0];

	NumElementsPerChunk = ElementsPerChunk;
	Off::InSDK::ObjArray::ChunkSize = ElementsPerChunk;

	ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
	{
		if (Index < 0 || Index > Num())
			return nullptr;

		const int32 ChunkIndex = Index / PerChunk;
		const int32 InChunkIdx = Index % PerChunk;

		const uintptr_t chunkTable = RDeref<uintptr_t>(reinterpret_cast<uintptr_t>(ObjectsArray));
		const uintptr_t chunk = RDeref<uintptr_t>(chunkTable + ChunkIndex * sizeof(void*));
		const uintptr_t itemPtr = chunk + (InChunkIdx * FUObjectItemSize);
		return reinterpret_cast<void*>(RDeref<uintptr_t>(itemPtr + FUObjectItemOffset));
	};

	uint8_t* ChunksPtr = DecryptPtr(reinterpret_cast<void*>(
		RDeref<uintptr_t>(GObjects + Off::FUObjectArray::GetObjectsOffset())));

	std::cerr << "Overwrote FChunkedFixedUObjectArray GObjects to offset 0x" << std::hex << Off::InSDK::ObjArray::GObjects << "\n" << std::endl;

	ObjectArray::InitializeFUObjectItem(reinterpret_cast<uint8_t*>(RDeref<uintptr_t>(reinterpret_cast<uintptr_t>(ChunksPtr))));
}

void ObjectArray::DumpObjects(const fs::path& Path, bool bWithPathname)
{
	std::ofstream DumpStream(Path / "GObjects-Dump.txt");

	DumpStream << "Object dump by Dumper-7\n\n";
	DumpStream << (!Settings::Generator::GameVersion.empty() && !Settings::Generator::GameName.empty() ? (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) + "\n\n" : "");
	DumpStream << "Count: " << Num() << "\n\n\n";

	for (auto Object : ObjectArray())
	{
		if (!bWithPathname)
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetFullName());
		}
		else
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetPathName());
		}
	}

	DumpStream.close();
}

void ObjectArray::DumpObjectsWithProperties(const fs::path& Path, bool bWithPathname)
{
	std::ofstream DumpStream(Path / "GObjects-Dump-WithProperties.txt");

	DumpStream << "Object dump by Dumper-7\n\n";
	DumpStream << (!Settings::Generator::GameVersion.empty() && !Settings::Generator::GameName.empty() ? (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) + "\n\n" : "");
	DumpStream << "Count: " << Num() << "\n\n\n";

	for (auto Object : ObjectArray())
	{
		if (!bWithPathname)
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetFullName());
		}
		else
		{
			DumpStream << std::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetPathName());
		}

		if (Object.IsA(EClassCastFlags::Struct))
		{
			for (UEProperty Prop : Object.Cast<UEStruct>().GetProperties())
			{
				DumpStream << std::format("[{:08X}] {{{}}}     {} {}\n", Prop.GetOffset(), Prop.GetAddress(), Prop.GetPropClassName(), Prop.GetName());
			}
		}
	}

	DumpStream.close();
}


int32 ObjectArray::Num()
{
	// Serve from cache once Init() populated it; before that we're in the layout-detection
	// phase where hypercalls are unavoidable.
	if (g_CachedNum >= 0)
		return g_CachedNum;
	return RDeref<int32>(GObjects + Off::FUObjectArray::GetNumElementsOffset());
}

int32 ObjectArray::Max()
{
	if (g_CachedMax >= 0)
		return g_CachedMax;
	return RDeref<int32>(GObjects + Off::FUObjectArray::GetMaxElementsOffset());
}

int32 ObjectArray::NumChunks()
{
	if (g_CachedNumChunks >= 0)
		return g_CachedNumChunks;
	return RDeref<int32>(GObjects + Off::FUObjectArray::GetNumChunksOffset());
}

int32 ObjectArray::MaxChunks()
{
	if (g_CachedMaxChunks >= 0)
		return g_CachedMaxChunks;
	return RDeref<int32>(GObjects + Off::FUObjectArray::GetMaxChunksOffset());
}

template<typename UEType>
static UEType ObjectArray::GetByIndex(int32 Index)
{
	return UEType(ByIndex(GObjects + Off::FUObjectArray::GetObjectsOffset(), Index, SizeOfFUObjectItem, FUObjectItemInitialOffset, NumElementsPerChunk));
}

template<typename UEType>
UEType ObjectArray::FindObject(const std::string& FullName, EClassCastFlags RequiredType)
{
	for (UEObject Object : ObjectArray())
	{
		if (Object.IsA(RequiredType) && Object.GetFullName() == FullName)
		{
			return Object.Cast<UEType>();
		}
	}

	return UEType();
}

template<typename UEType>
UEType ObjectArray::FindObjectFast(const std::string& Name, EClassCastFlags RequiredType)
{
	// Per-process cache: offset-finders call FindObjectFast for the same handful of engine
	// classes many times (Actor, Pawn, Color, Guid, Vector, Struct, ...), and each call
	// iterates all ~100k UObjects. Caching by (name, type) turns that quadratic work into
	// a single scan per unique lookup. The UEObject stored here is a thin handle around a
	// remote VA, so caching a handle across calls stays valid for the lifetime of the dump.
	using CacheKey = std::pair<std::string, EClassCastFlags>;
	struct PairHash
	{
		size_t operator()(const CacheKey& k) const noexcept
		{
			return std::hash<std::string>{}(k.first) ^ static_cast<size_t>(k.second);
		}
	};
	static std::unordered_map<CacheKey, UEObject, PairHash> cache;

	const CacheKey key{ Name, RequiredType };
	if (auto it = cache.find(key); it != cache.end())
		return it->second.Cast<UEType>();

	auto ObjArray = ObjectArray();
	for (UEObject Object : ObjArray)
	{
		if (Object.IsA(RequiredType) && Object.GetName() == Name)
		{
			cache.emplace(key, Object);
			return Object.Cast<UEType>();
		}
	}

	cache.emplace(key, UEObject{}); // cache the miss too so repeated lookups don't re-scan
	return UEType();
}

template<typename UEType>
static UEType ObjectArray::FindObjectFastInOuter(const std::string& Name, std::string Outer)
{
	auto ObjArray = ObjectArray();

	for (UEObject Object : ObjArray)
	{
		if (Object.GetName() == Name && Object.GetOuter().GetName() == Outer)
		{
			return Object.Cast<UEType>();
		}
	}

	return UEType();
}

UEStruct ObjectArray::FindStruct(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Struct);
}

UEStruct ObjectArray::FindStructFast(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Struct);
}

UEClass ObjectArray::FindClass(const std::string& FullName)
{
	return FindObject<UEClass>(FullName, EClassCastFlags::Class);
}

UEClass ObjectArray::FindClassFast(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Class);
}

ObjectArray::ObjectsIterator ObjectArray::begin()
{
	return ObjectsIterator();
}
ObjectArray::ObjectsIterator ObjectArray::end()
{
	return ObjectsIterator(Num());
}


ObjectArray::ObjectsIterator::ObjectsIterator(int32 StartIndex)
	: CurrentIndex(StartIndex), CurrentObject(ObjectArray::GetByIndex(StartIndex))
{
}

UEObject ObjectArray::ObjectsIterator::operator*() const
{
	return CurrentObject;
}

ObjectArray::ObjectsIterator& ObjectArray::ObjectsIterator::operator++()
{
	CurrentObject = ObjectArray::GetByIndex(++CurrentIndex);

	while (!CurrentObject && CurrentIndex < (ObjectArray::Num() - 1))
	{
		CurrentObject = ObjectArray::GetByIndex(++CurrentIndex);
	}

	if (!CurrentObject && CurrentIndex == (ObjectArray::Num() - 1)) [[unlikely]]
		CurrentIndex++;

	return *this;
}

bool ObjectArray::ObjectsIterator::operator==(const ObjectsIterator& Other) const
{
	return CurrentIndex == Other.CurrentIndex;
}

bool ObjectArray::ObjectsIterator::operator!=(const ObjectsIterator& Other) const
{
	return CurrentIndex != Other.CurrentIndex;
}

int32 ObjectArray::ObjectsIterator::GetIndex() const
{
	return CurrentIndex;
}

bool AllFieldIterator::operator!=(const AllFieldIterator& Other) const
{
	return CurrentObject != Other.CurrentObject || PropertyIndex != Other.PropertyIndex;
}

AllFieldIterator& AllFieldIterator::operator++()
{
	if (CurrenStructHasMoreMembers())
	{
		PropertyIndex++;

		return *this;
	}

	IterateToNextStructWithMembers();

	return *this;
}

UEProperty AllFieldIterator::operator*() const
{
	return Fields[PropertyIndex];
}


void AllFieldIterator::IterateToNextStruct()
{
	if (IsEndIterator())
		return;

	++CurrentObject;

	while (CurrentObject != ObjectEndIterator && !IsCurrentObjectStruct())
		++CurrentObject;
}
void AllFieldIterator::IterateToNextStructWithMembers()
{
	// Loop, in case we meet a struct wihtout any properties
	while (!CurrenStructHasMoreMembers())
	{
		IterateToNextStruct();
		PropertyIndex = 0;

		if (IsEndIterator())
			return;

		Fields = GetCurrentStruct().GetProperties();
	}
}


/*
* The compiler won't generate functions for a specific template type unless it's used in the .cpp file corresponding to the
* header it was declatred in.
*
* See https://stackoverflow.com/questions/456713/why-do-i-get-unresolved-external-symbol-errors-when-using-templates
*/
template UEObject ObjectArray::FindObject<UEObject>(const std::string& FullName, EClassCastFlags RequiredType);
template UEField ObjectArray::FindObject<UEField>(const std::string& FullName, EClassCastFlags RequiredType);
template UEEnum ObjectArray::FindObject<UEEnum>(const std::string& FullName, EClassCastFlags RequiredType);
template UEStruct ObjectArray::FindObject<UEStruct>(const std::string& FullName, EClassCastFlags RequiredType);
template UEClass ObjectArray::FindObject<UEClass>(const std::string& FullName, EClassCastFlags RequiredType);
template UEFunction ObjectArray::FindObject<UEFunction>(const std::string& FullName, EClassCastFlags RequiredType);
template UEProperty ObjectArray::FindObject<UEProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEByteProperty ObjectArray::FindObject<UEByteProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEBoolProperty ObjectArray::FindObject<UEBoolProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEObjectProperty ObjectArray::FindObject<UEObjectProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEClassProperty ObjectArray::FindObject<UEClassProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEStructProperty ObjectArray::FindObject<UEStructProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEArrayProperty ObjectArray::FindObject<UEArrayProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEMapProperty ObjectArray::FindObject<UEMapProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UESetProperty ObjectArray::FindObject<UESetProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEEnumProperty ObjectArray::FindObject<UEEnumProperty>(const std::string& FullName, EClassCastFlags RequiredType);

template UEObject ObjectArray::FindObjectFast<UEObject>(const std::string& FullName, EClassCastFlags RequiredType);
template UEField ObjectArray::FindObjectFast<UEField>(const std::string& FullName, EClassCastFlags RequiredType);
template UEEnum ObjectArray::FindObjectFast<UEEnum>(const std::string& FullName, EClassCastFlags RequiredType);
template UEStruct ObjectArray::FindObjectFast<UEStruct>(const std::string& FullName, EClassCastFlags RequiredType);
template UEClass ObjectArray::FindObjectFast<UEClass>(const std::string& FullName, EClassCastFlags RequiredType);
template UEFunction ObjectArray::FindObjectFast<UEFunction>(const std::string& FullName, EClassCastFlags RequiredType);
template UEProperty ObjectArray::FindObjectFast<UEProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEByteProperty ObjectArray::FindObjectFast<UEByteProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEBoolProperty ObjectArray::FindObjectFast<UEBoolProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEObjectProperty ObjectArray::FindObjectFast<UEObjectProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEClassProperty ObjectArray::FindObjectFast<UEClassProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEStructProperty ObjectArray::FindObjectFast<UEStructProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEArrayProperty ObjectArray::FindObjectFast<UEArrayProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEMapProperty ObjectArray::FindObjectFast<UEMapProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UESetProperty ObjectArray::FindObjectFast<UESetProperty>(const std::string& FullName, EClassCastFlags RequiredType);
template UEEnumProperty ObjectArray::FindObjectFast<UEEnumProperty>(const std::string& FullName, EClassCastFlags RequiredType);

template UEObject ObjectArray::FindObjectFastInOuter<UEObject>(const std::string& FullName, std::string Outer);
template UEField ObjectArray::FindObjectFastInOuter<UEField>(const std::string& FullName, std::string Outer);
template UEEnum ObjectArray::FindObjectFastInOuter<UEEnum>(const std::string& FullName, std::string Outer);
template UEStruct ObjectArray::FindObjectFastInOuter<UEStruct>(const std::string& FullName, std::string Outer);
template UEClass ObjectArray::FindObjectFastInOuter<UEClass>(const std::string& FullName, std::string Outer);
template UEFunction ObjectArray::FindObjectFastInOuter<UEFunction>(const std::string& FullName, std::string Outer);
template UEProperty ObjectArray::FindObjectFastInOuter<UEProperty>(const std::string& FullName, std::string Outer);
template UEByteProperty ObjectArray::FindObjectFastInOuter<UEByteProperty>(const std::string& FullName, std::string Outer);
template UEBoolProperty ObjectArray::FindObjectFastInOuter<UEBoolProperty>(const std::string& FullName, std::string Outer);
template UEObjectProperty ObjectArray::FindObjectFastInOuter<UEObjectProperty>(const std::string& FullName, std::string Outer);
template UEClassProperty ObjectArray::FindObjectFastInOuter<UEClassProperty>(const std::string& FullName, std::string Outer);
template UEStructProperty ObjectArray::FindObjectFastInOuter<UEStructProperty>(const std::string& FullName, std::string Outer);
template UEArrayProperty ObjectArray::FindObjectFastInOuter<UEArrayProperty>(const std::string& FullName, std::string Outer);
template UEMapProperty ObjectArray::FindObjectFastInOuter<UEMapProperty>(const std::string& FullName, std::string Outer);
template UESetProperty ObjectArray::FindObjectFastInOuter<UESetProperty>(const std::string& FullName, std::string Outer);
template UEEnumProperty ObjectArray::FindObjectFastInOuter<UEEnumProperty>(const std::string& FullName, std::string Outer);

template UEObject ObjectArray::GetByIndex<UEObject>(int32 Index);
template UEField ObjectArray::GetByIndex<UEField>(int32 Index);
template UEEnum ObjectArray::GetByIndex<UEEnum>(int32 Index);
template UEStruct ObjectArray::GetByIndex<UEStruct>(int32 Index);
template UEClass ObjectArray::GetByIndex<UEClass>(int32 Index);
template UEFunction ObjectArray::GetByIndex<UEFunction>(int32 Index);
template UEProperty ObjectArray::GetByIndex<UEProperty>(int32 Index);
template UEByteProperty ObjectArray::GetByIndex<UEByteProperty>(int32 Index);
template UEBoolProperty ObjectArray::GetByIndex<UEBoolProperty>(int32 Index);
template UEObjectProperty ObjectArray::GetByIndex<UEObjectProperty>(int32 Index);
template UEClassProperty ObjectArray::GetByIndex<UEClassProperty>(int32 Index);
template UEStructProperty ObjectArray::GetByIndex<UEStructProperty>(int32 Index);
template UEArrayProperty ObjectArray::GetByIndex<UEArrayProperty>(int32 Index);
template UEMapProperty ObjectArray::GetByIndex<UEMapProperty>(int32 Index);
template UESetProperty ObjectArray::GetByIndex<UESetProperty>(int32 Index);
template UEEnumProperty ObjectArray::GetByIndex<UEEnumProperty>(int32 Index);