
#include <algorithm>
#include <cstring>
#include <format>
#include <vector>

#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"

#include "Platform.h"
#include "Architecture.h"

#include "RemoteMemory.h"

uint8* NameArray::GNames = nullptr;

namespace
{
	// Bulk-cached FNamePool chunks. Each chunk is reallocated into a local buffer on Init so that
	// FName::ToString resolves locally instead of issuing 3-4 hypercalls per lookup (chunks-array
	// pointer + chunk base + entry header + string bytes). On a target where FindObjectFast must
	// miss (stripped class), the cache turns a ~1M hypercall iteration into ~117k (just one
	// Object-bytes read per name) + local memcpy.
	//
	// The cache is populated once at NameArray::Init and never invalidated. That's safe because
	// the FNamePool only grows (never moves existing chunks) during the game's lifetime, and we
	// run against an idle/paused target. If future runs start mid-gameplay with live name
	// additions, add a periodic re-read of {NumChunks, ByteCursor} and grow the cache as needed.
	struct FNamePoolChunkCache
	{
		std::vector<uintptr_t> ChunkRemoteBases;        // [i] → remote VA of chunk i
		std::vector<std::vector<uint8_t>> ChunkBuffers; // [i] → local bytes of chunk i
		size_t ChunkSizeBytes = 0;
		int32 CachedNumChunks = -1;
		int32 CachedByteCursor = -1;
		bool Populated = false;
	};

	FNamePoolChunkCache g_NamePoolCache;
}

// Translate a remote FNameEntry VA into a local pointer inside the cached chunk buffer.
// Returns nullptr if the address isn't inside any cached chunk — callers fall back to
// hypercalls in that case.
const uint8_t* NameArray::TryResolveLocalNameEntry(uintptr_t RemoteVA)
{
	if (!g_NamePoolCache.Populated)
		return nullptr;
	for (size_t i = 0; i < g_NamePoolCache.ChunkRemoteBases.size(); ++i)
	{
		const uintptr_t base = g_NamePoolCache.ChunkRemoteBases[i];
		if (base == 0) continue;
		const size_t size = g_NamePoolCache.ChunkBuffers[i].size();
		if (RemoteVA >= base && RemoteVA < base + size)
			return g_NamePoolCache.ChunkBuffers[i].data() + (RemoteVA - base);
	}
	return nullptr;
}

int32 NameArray::GetCachedNumChunks()
{
	return g_NamePoolCache.CachedNumChunks;
}

int32 NameArray::GetCachedByteCursor()
{
	return g_NamePoolCache.CachedByteCursor;
}

uintptr_t NameArray::GetCachedChunkBase(int32 ChunkIndex)
{
	if (!g_NamePoolCache.Populated) return 0;
	if (ChunkIndex < 0 || ChunkIndex >= static_cast<int32>(g_NamePoolCache.ChunkRemoteBases.size()))
		return 0;
	return g_NamePoolCache.ChunkRemoteBases[ChunkIndex];
}

void NameArray::PopulateFNamePoolCache(uintptr_t FNamePoolRemote, int32 NumChunks, int32 BlockOffsetBits, int32 Stride, int32 ByteCursor)
{
	// Per-chunk size: (1 << BlockOffsetBits) * Stride is the maximum byte-offset addressable
	// by ByIndex. UE's FNameEntryAllocator rounds up its allocations to this size.
	const size_t chunkSize = static_cast<size_t>(1ULL << BlockOffsetBits) * static_cast<size_t>(Stride);

	g_NamePoolCache.ChunkRemoteBases.assign(NumChunks, 0);
	g_NamePoolCache.ChunkBuffers.assign(NumChunks, std::vector<uint8_t>{});
	g_NamePoolCache.ChunkSizeBytes = chunkSize;
	g_NamePoolCache.CachedNumChunks = NumChunks;
	g_NamePoolCache.CachedByteCursor = ByteCursor;
	g_NamePoolCache.Populated = false;

	const uintptr_t chunkArrayAddr = FNamePoolRemote + 0x10;
	std::vector<uintptr_t> chunkPtrs(NumChunks);
	if (!RemoteMemory::ReadBuffer(chunkArrayAddr, chunkPtrs.data(),
		NumChunks * sizeof(uintptr_t), RemoteMemory::PartialReadPolicy::ErrorOnGap))
	{
		std::cerr << "[NameArray] Failed to read chunk pointers; cache disabled\n";
		return;
	}

	size_t totalBytes = 0;
	for (int32 i = 0; i < NumChunks; ++i)
	{
		if (chunkPtrs[i] == 0)
			continue;

		g_NamePoolCache.ChunkRemoteBases[i] = chunkPtrs[i];

		// Only populate up to ByteCursor for the final chunk so we don't pull in unmapped pages.
		const size_t bytesToRead = (i == NumChunks - 1)
			? (ByteCursor > 0 && static_cast<size_t>(ByteCursor) < chunkSize ? static_cast<size_t>(ByteCursor) : chunkSize)
			: chunkSize;

		g_NamePoolCache.ChunkBuffers[i].resize(bytesToRead);
		RemoteMemory::ReadBuffer(chunkPtrs[i], g_NamePoolCache.ChunkBuffers[i].data(),
			bytesToRead, RemoteMemory::PartialReadPolicy::ZeroFillOnGap);
		totalBytes += bytesToRead;
	}

	g_NamePoolCache.Populated = true;
	std::cerr << std::format("[NameArray] FNamePool cache populated: {} chunks, {} KB total\n",
		NumChunks, totalBytes / 1024);
}


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

		// Fast path: InitializeNamePool's chunk[0]-paged-out fallback pre-sets
		// FNameEntryLengthShiftCount=6 (UE4.22+ standard when header size is 2). If it's
		// already set to a sensible value and chunk[0] is unreadable, skip the ByteProperty
		// hunt — it would scan from 0+offset and either fault or produce garbage shifts.
		uint8_t firstChunkProbe[16]{};
		if (firstChunkAddr != 0)
			RemoteMemory::ReadBuffer(firstChunkAddr, firstChunkProbe, sizeof(firstChunkProbe), RemoteMemory::PartialReadPolicy::ZeroFillOnGap);
		const bool bFirstChunkReadable = firstChunkAddr != 0
			&& !std::all_of(std::begin(firstChunkProbe), std::end(firstChunkProbe), [](uint8_t b) { return b == 0; });

		if (!bFirstChunkReadable && FNameEntryLengthShiftCount > 0 && FNameEntryLengthShiftCount < 16)
		{
			// Shift count was pre-set by the caller; trust it.
			std::cerr << std::format(
				"[NameArray] FNameEntry::Init: chunk[0] unreadable; using preset FNameEntryLengthShiftCount={}\n",
				static_cast<int32>(FNameEntryLengthShiftCount));
		}
		else
		{
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
		}

		GetStr = [](uint8* NameEntry) -> std::wstring
		{
			// Per-thread recursion guard. Outline-number FNameEntries reference a sibling
			// entry by index (the NameLen==0 branch below). When the target's FNamePool is
			// partially paged-out or we're using a stale CR3, `NextEntryIndex` is garbage and
			// can form a cycle — the recursive resolver then blows up the stack and allocates
			// std::wstring instances at every frame (26 GB observed). Capping depth breaks the
			// cycle with a sentinel string.
			thread_local int recursionDepth = 0;
			constexpr int kMaxRecursionDepth = 8;
			struct DepthGuard {
				int* d; DepthGuard(int* p) : d(p) { ++*p; }
				~DepthGuard() { --*d; }
			} guard(&recursionDepth);
			if (recursionDepth > kMaxRecursionDepth)
				return L"__fname_cycle__";

			// Fast path: if the entry lives inside a cached FNamePool chunk, do the whole
			// header+string parse from local memory. That turns ~3 hypercalls into 0 and is
			// the difference between seconds and minutes on any FindObjectFast miss.
			if (const uint8_t* local = NameArray::TryResolveLocalNameEntry(reinterpret_cast<uintptr_t>(NameEntry)))
			{
				auto LocalRead16 = [](const uint8_t* p) { uint16 v; std::memcpy(&v, p, 2); return v; };
				auto LocalRead32 = [](const uint8_t* p) { int32  v; std::memcpy(&v, p, 4); return v; };

				const uint16 HeaderWithoutNumber = LocalRead16(local + Off::FNameEntry::NamePool::HeaderOffset);
				const int32 NameLen = HeaderWithoutNumber >> FNameEntry::FNameEntryLengthShiftCount;

				if (NameLen == 0)
				{
					const int32 EntryIdOffset = Off::FNameEntry::NamePool::StringOffset + ((Off::FNameEntry::NamePool::StringOffset == 6) * 2);
					const int32 NextEntryIndex = LocalRead32(local + EntryIdOffset);
					const int32 Number = LocalRead32(local + EntryIdOffset + sizeof(int32));
					if (NextEntryIndex <= 0 || NextEntryIndex > 0x04000000)
						return L"";
					if (Number > 0 && Number < 0x100000)
						return NameArray::GetNameEntry(NextEntryIndex).GetWString() + L'_' + std::to_wstring(Number - 1);
					return NameArray::GetNameEntry(NextEntryIndex).GetWString();
				}

				const int32 clampedLen = (NameLen > 0x400) ? 0x400 : (NameLen < 0 ? 0 : NameLen);
				if (clampedLen == 0)
					return L"";
				const uint8_t* stringLocal = local + Off::FNameEntry::NamePool::StringOffset;

				if (HeaderWithoutNumber & NameWideMask)
				{
					return std::wstring(reinterpret_cast<const wchar_t*>(stringLocal), clampedLen);
				}
				thread_local std::string narrowScratch;
				narrowScratch.assign(reinterpret_cast<const char*>(stringLocal), clampedLen);
				return UtfN::StringToWString(narrowScratch);
			}

			// Slow path: entry falls outside the cache (shouldn't happen once NamePool cache
			// is populated, but we keep it for manual overrides / malformed CR3 states).
			const uint16 HeaderWithoutNumber = RDeref<uint16>(NameEntry + Off::FNameEntry::NamePool::HeaderOffset);
			const int32 NameLen = HeaderWithoutNumber >> FNameEntry::FNameEntryLengthShiftCount;

			if (NameLen == 0)
			{
				const int32 EntryIdOffset = Off::FNameEntry::NamePool::StringOffset + ((Off::FNameEntry::NamePool::StringOffset == 6) * 2);

				const int32 NextEntryIndex = RDeref<int32>(NameEntry + EntryIdOffset);
				const int32 Number = RDeref<int32>(NameEntry + EntryIdOffset + sizeof(int32));

				if (NextEntryIndex <= 0 || NextEntryIndex > 0x04000000)
					return L"";

				if (Number > 0 && Number < 0x100000)
					return NameArray::GetNameEntry(NextEntryIndex).GetWString() + L'_' + std::to_wstring(Number - 1);

				return NameArray::GetNameEntry(NextEntryIndex).GetWString();
			}

			const int32 clampedLen = (NameLen > 0x400) ? 0x400 : (NameLen < 0 ? 0 : NameLen);
			if (clampedLen == 0)
				return L"";
			const uintptr_t stringAddr = reinterpret_cast<uintptr_t>(NameEntry) + Off::FNameEntry::NamePool::StringOffset;

			thread_local std::vector<wchar_t> wideBuf;
			thread_local std::vector<char> narrowBuf;

			if (HeaderWithoutNumber & NameWideMask)
			{
				if (wideBuf.size() < static_cast<size_t>(clampedLen)) wideBuf.resize(clampedLen);
				RemoteMemory::ReadBuffer(stringAddr, wideBuf.data(), clampedLen * sizeof(wchar_t), RemoteMemory::PartialReadPolicy::ZeroFillOnGap);
				return std::wstring(wideBuf.data(), clampedLen);
			}

			if (narrowBuf.size() < static_cast<size_t>(clampedLen)) narrowBuf.resize(clampedLen);
			RemoteMemory::ReadBuffer(stringAddr, narrowBuf.data(), clampedLen, RemoteMemory::PartialReadPolicy::ZeroFillOnGap);
			return UtfN::StringToWString(std::string(narrowBuf.data(), clampedLen));
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

	const uintptr_t chunkArrayRemoteAddr = reinterpret_cast<uintptr_t>(NamePool) + Off::NameArray::ChunksStart;

	bool bFoundCoreUObjectString = false;
	int64 FNameEntryHeaderSize = 0x0;

	constexpr int32 LoopLimit = 0x1000;

	// Derive FNameEntryHeaderSize from any readable chunk — not only chunk[0].
	//
	// Failure case we saw on MHUR.exe (UE 4.27 from-source build, idle chunks can page out
	// because they contain cold registrations that the game never touches post-init): chunk[0]
	// pages out, chunk[1+] stay resident because they hold gameplay-active class names.
	// The classic algorithm scanned *only* chunk[0] for "None" (header-size anchor) and
	// "/Script/CoreUObject" (sanity), and bailed if either was unreachable.
	//
	// FNameEntryHeaderSize is a property of the pool, invariant across chunks. Every chunk's
	// first entry starts at offset 0 with the same header layout, so we can determine the
	// header size from any readable chunk. Chunk[0] is still preferred (it lets us also
	// sanity-check on the "/Script/CoreUObject" string); only fall back when it's unusable.
	const int32 maxChunkIdxValue = RDeref<int32>(NamePool + Off::NameArray::MaxChunkIndex);
	// Bulk-read every chunk pointer so we don't issue (probe-count) separate hypercalls in the
	// scan loop. On an idle MHUR.exe the first ~150 chunks can be paged out, so capping the probe
	// at 16 (our original timid limit) meant we never found a live chunk and bailed with the
	// "couldn't be used by the generator" misleading error even though the pool was valid.
	const int32 chunksToProbe = std::min<int32>(maxChunkIdxValue + 1, 256);
	std::vector<uintptr_t> cachedChunkPtrs(chunksToProbe, 0);
	RemoteMemory::ReadBuffer(chunkArrayRemoteAddr, cachedChunkPtrs.data(),
		chunksToProbe * sizeof(uintptr_t), RemoteMemory::PartialReadPolicy::ZeroFillOnGap);

	auto isProbeChunkReadable = [](uintptr_t chunkAddr) -> bool
	{
		if (chunkAddr == 0)
			return false;
		uint8_t sniff[32]{};
		RemoteMemory::ReadBuffer(chunkAddr, sniff, sizeof(sniff), RemoteMemory::PartialReadPolicy::ZeroFillOnGap);
		// If every byte is zero, the page is either not mapped (HV read returned zeros) or the
		// chunk genuinely has no content yet. Either way it's useless for header detection.
		return !std::all_of(std::begin(sniff), std::end(sniff), [](uint8_t b) { return b == 0; });
	};

	// Check whether a uint16 at offset 0 parses as a plausible UE4.22+ FNamePool header
	// (bIsWide:1, ProbeHash:5, Len:10). We require wide=0 and a printable-ASCII string of
	// declared length, which is nearly impossible to satisfy by accident on non-name data.
	auto validateHeader2 = [](uintptr_t chunkAddr) -> bool
	{
		const uint16 hdr = RDeref<uint16>(chunkAddr);
		const bool wide = (hdr & 1) != 0;
		const int32 len = hdr >> 6;
		if (wide || len < 2 || len > 256)
			return false;
		for (int k = 0; k < len; ++k)
		{
			const uint8 b = RDeref<uint8>(chunkAddr + 2 + k);
			if (b < 0x20 || b > 0x7E)
				return false;
		}
		return true;
	};

	uintptr_t firstChunkAddr = cachedChunkPtrs.empty() ? 0 : cachedChunkPtrs[0];
	for (int32 probeIdx = 0; probeIdx < chunksToProbe && !bFoundCoreUObjectString; ++probeIdx)
	{
		const uintptr_t probeChunkAddr = cachedChunkPtrs[probeIdx];
		if (!isProbeChunkReadable(probeChunkAddr))
			continue;

		if (probeIdx == 0)
		{
			// Chunk 0 is live: use the classic scan for "None" (header-size anchor) and
			// "/Script/CoreUObject" (pool identity confirmation).
			for (int i = 0; i < LoopLimit; i++)
			{
				if (RDeref<uint32>(probeChunkAddr + i) == NoneAsUint32 && FNameEntryHeaderSize == 0)
				{
					FNameEntryHeaderSize = i;
				}
				else if (RDeref<uint64>(probeChunkAddr + i) == CoreUObjAsUint64)
				{
					bFoundCoreUObjectString = true;
					break;
				}
			}
		}
		else
		{
			// Chunk 0 was paged out. Derive header size from this readable chunk's entry at
			// offset 0. The structural FNamePool validation (matching MaxChunkIndex against
			// the non-null pointer count above) already gives high confidence this is the
			// real pool; the printable-ASCII header-2 test is the final empirical check.
			if (validateHeader2(probeChunkAddr))
			{
				FNameEntryHeaderSize = 2;
				bFoundCoreUObjectString = true;
				std::cerr << std::format(
					"[NameArray] chunk[0] paged out; derived FNameEntryHeaderSize=2 from chunk[{}] at 0x{:X}\n",
					probeIdx, probeChunkAddr);
			}
		}
	}

	if (!bFoundCoreUObjectString || firstChunkAddr == 0)
		return false;

	NameEntryStride = FNameEntryHeaderSize == 2 ? 2 : 4;
	Off::InSDK::NameArray::FNameEntryStride = NameEntryStride;

	ByIndex = [](void* NamesArray, int32 ComparisonIndex, int32 NamePoolBlockOffsetBits) -> void*
	{
		const int32 ChunkIdx = ComparisonIndex >> NamePoolBlockOffsetBits;
		const int32 InChunkOffset = (ComparisonIndex & ((1 << NamePoolBlockOffsetBits) - 1)) * NameEntryStride;

		// Use cached {NumChunks, ByteCursor, ChunkBase} when available to avoid 3 hypercalls per
		// ByIndex. On a FindObjectFast miss against 117k UObjects that saves ~351k hypercalls.
		const int32 cachedNumChunks   = NameArray::GetCachedNumChunks();
		const int32 cachedByteCursor  = NameArray::GetCachedByteCursor();
		const int32 numChunksForCheck = cachedNumChunks >= 0 ? cachedNumChunks : NameArray::GetNumChunks();
		const int32 byteCursorForCheck = cachedByteCursor >= 0 ? cachedByteCursor : NameArray::GetByteCursor();

		if (ChunkIdx < 0 || ChunkIdx > numChunksForCheck)
			return nullptr;
		if (ChunkIdx == numChunksForCheck && InChunkOffset > byteCursorForCheck)
			return nullptr;

		const uintptr_t cachedBase = NameArray::GetCachedChunkBase(ChunkIdx);
		const uintptr_t chunkBase = cachedBase != 0
			? cachedBase
			: RDeref<uintptr_t>(reinterpret_cast<uintptr_t>(NamesArray) + 0x10 + ChunkIdx * sizeof(void*));
		return reinterpret_cast<void*>(chunkBase + InChunkOffset);
	};

	Settings::Internal::bUseNamePool = true;

	// Populate the FNamePool chunk cache BEFORE FNameEntry::Init so that GetStr's fast path
	// is ready for the first FName resolution (which happens during Init's shift-count probe).
	{
		const int32 NumChunks = RDeref<int32>(NamePool + Off::NameArray::MaxChunkIndex) + 1;
		const int32 ByteCursor = RDeref<int32>(NamePool + Off::NameArray::ByteCursor);
		// FNameBlockOffsetBits defaults to 0x10 (16) at this point; PostInit refines it later.
		// The cache is sized for the default bits; if PostInit bumps bits we extend below.
		NameArray::PopulateFNamePoolCache(
			reinterpret_cast<uintptr_t>(NamePool),
			NumChunks,
			static_cast<int32>(NameArray::FNameBlockOffsetBits),
			static_cast<int32>(NameEntryStride),
			ByteCursor);
	}

	// When we derived the header size via the fallback (chunk[0] paged out), we also already
	// know we're on the UE4.22+ standard 2-byte header layout which always uses shift=6. Pre-
	// set the shift count and pass the first readable chunk so FNameEntry::Init doesn't go hunting
	// for "ByteProperty" in a chunk[0] it can't read.
	const bool bChunkZeroWasUsable = (firstChunkAddr != 0)
		&& [&]() { uint8_t s[32]{}; RemoteMemory::ReadBuffer(firstChunkAddr, s, sizeof(s), RemoteMemory::PartialReadPolicy::ZeroFillOnGap);
		           return !std::all_of(std::begin(s), std::end(s), [](uint8_t b) { return b == 0; }); }();
	if (!bChunkZeroWasUsable)
	{
		FNameEntry::FNameEntryLengthShiftCount = 6;
	}
	FNameEntry::Init(reinterpret_cast<uint8*>(chunkArrayRemoteAddr), FNameEntryHeaderSize);

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
		// Primary: derive bits from the FNamePool structure itself. ByteCursor tells us how many
		// bytes have been allocated in the CURRENT (last) chunk — that's always <= ChunkSize, and
		// ChunkSize = (1 << bits) * NameEntryStride. So the smallest bits that makes
		// (1 << bits) * stride >= ByteCursor is the right one.
		//
		// The legacy algorithm (iterate UObjects, find one whose CompIdx/2^bits == NumChunks-1) is
		// brittle when no UObject actually references a name from the LAST chunk of the pool — in
		// that case it converges on an undershooting bits value that leaves ByteCursor pointing
		// past the end of a "cached" chunk and every FName lookup for a higher-CompIdx name
		// resolves to garbage bytes in the next chunk's allocation.
		const int32 ByteCursor = GetByteCursor();
		if (ByteCursor > 0 && NameEntryStride > 0)
		{
			int32 bits = 0x8;
			while (bits < 0x18)
			{
				const int64 chunkSize = static_cast<int64>(1ULL << bits) * static_cast<int64>(NameEntryStride);
				if (chunkSize >= ByteCursor)
					break;
				++bits;
			}
			NameArray::FNameBlockOffsetBits = bits;
			std::cerr << std::format(
				"[NameArray] PostInit: bits={} (ByteCursor-minimum from 0x{:X} stride={})\n",
				NameArray::FNameBlockOffsetBits, ByteCursor, NameEntryStride);

			// ByteCursor gives the MINIMUM chunk size that still fits the last chunk's current
			// write offset — but the actual chunk allocations can be any larger power of 2. On
			// MHUR (UE4.27 with 64KB FNamePool blocks) the last chunk only held 0x2D2E bytes so
			// the minimum came out as bits=13 (16KB chunks), but the actual chunks are 64KB
			// (bits=15). Using the undershooting value makes every FName.CompIdx >= 8192 resolve
			// to garbage bytes in a later chunk, which then get read as wildly-long entries and
			// the dumper ballooned to 9 GB RSS allocating junk wstrings before being killed.
			//
			// Bits correction: try candidate {15, 16, 17} and validate each against real UObject
			// FNames. For each candidate, resolve 10 well-distributed UObject CompIdx values and
			// count how many resolve to a printable-ASCII FName entry. The candidate with ≥80%
			// validation wins; that's the "real" bits.
			//
			// Why this beats the previous probe-the-chunk-boundary approach: Windows heap places
			// same-sized FName chunk allocations near each other (same LFH bucket), so chunk[N]'s
			// alleged "tail" at chunk[N]+0x40000 is frequently *another* chunk's head. Heuristic
			// scans for "non-zero" or "looks ASCII" at alleged chunk ends can't distinguish those
			// cases and consistently overshoot. Verification against actual CompIdx values is the
			// only signal that unambiguously maps bits → correct-or-not.
			//
			// Sampling strategy: pick 10 UObjects distributed across the whole ObjectArray range,
			// filtering for ones whose chunk resolves to a mapped page (paged-out chunks can't
			// be validated — they return zeros for all bits, which looks like failure everywhere).
			// Given this target's paging pattern (cold chunks get swapped to disk), we don't
			// require all samples to succeed — just need enough hits to tiebreak between bits
			// candidates.
			auto resolveFNameStr = [](uintptr_t fnamePoolAddr, int32 compIdx, int32 bits,
				int32 stride, int32 chunksStartOff, int32 numChunks) -> std::pair<bool, std::string>
			{
				const int32 chunkIdx = compIdx >> bits;
				const int32 inChunkOffset = (compIdx & ((1 << bits) - 1)) * stride;
				if (chunkIdx < 0 || chunkIdx >= numChunks)
					return { false, "" };
				const uintptr_t chunkBase = RDeref<uintptr_t>(fnamePoolAddr + chunksStartOff + chunkIdx * 8);
				if (chunkBase == 0)
					return { false, "" };
				const uintptr_t entryAddr = chunkBase + inChunkOffset;
				uint8_t buf[258]{};
				if (!RemoteMemory::ReadBuffer(entryAddr, buf, sizeof(buf), RemoteMemory::PartialReadPolicy::ZeroFillOnGap))
					return { false, "" };
				const uint16 hdr = static_cast<uint16>(buf[0]) | (static_cast<uint16>(buf[1]) << 8);
				const bool wide = (hdr & 1) != 0;
				const int32 len = hdr >> 6;
				if (wide || len < 2 || len > 256)
					return { false, "" };
				for (int k = 0; k < len; ++k)
				{
					const uint8 b = buf[2 + k];
					if (b < 0x20 || b > 0x7E)
						return { false, "" };
				}
				return { true, std::string(reinterpret_cast<char*>(buf + 2), len) };
			};

			const int32 numChunks = GetNumChunks() + 1;
			const int32 chunksStartOff = Off::NameArray::ChunksStart;
			const int32 totalObjects = ObjectArray::Num();

			std::vector<int32> sampleCompIdxs;
			if (totalObjects > 0)
			{
				const int32 kNumSamples = 48;
				for (int32 s = 0; s < kNumSamples; ++s)
				{
					const int32 objIdx = (totalObjects / kNumSamples) * s + (totalObjects / (kNumSamples * 2));
					UEObject obj = ObjectArray::GetByIndex(objIdx);
					if (!obj) continue;
					const int32 compIdx = obj.GetFName().GetCompIdx();
					if (compIdx > 0)
						sampleCompIdxs.push_back(compIdx);
				}
			}

			int32 bestBits = NameArray::FNameBlockOffsetBits;
			int32 bestHits = -1;
			std::string bestExample;
			for (int32 cand : { 15, 16, 17, 14, 13 })
			{
				if (cand < NameArray::FNameBlockOffsetBits)
					continue; // never downshift below ByteCursor minimum
				int32 hits = 0;
				std::string firstExample;
				for (int32 compIdx : sampleCompIdxs)
				{
					auto [ok, str] = resolveFNameStr(
						reinterpret_cast<uintptr_t>(GNames), compIdx, cand,
						NameEntryStride, chunksStartOff, numChunks);
					if (ok)
					{
						++hits;
						if (firstExample.empty()) firstExample = str;
					}
				}
				if (hits > bestHits)
				{
					bestHits = hits;
					bestBits = cand;
					bestExample = firstExample;
				}
				std::cerr << std::format(
					"[NameArray] PostInit: bits={} candidate -> {}/{} samples resolved (example: \"{}\")\n",
					cand, hits, static_cast<int32>(sampleCompIdxs.size()), firstExample);
			}

			if (bestHits > 0 && bestBits != NameArray::FNameBlockOffsetBits)
			{
				std::cerr << std::format(
					"[NameArray] PostInit: bits corrected from {} to {} (validated via {} UObject FName samples, "
					"best example resolved to \"{}\")\n",
					NameArray::FNameBlockOffsetBits, bestBits, bestHits, bestExample);
				NameArray::FNameBlockOffsetBits = bestBits;
			}
			else if (bestHits == 0)
			{
				std::cerr << "[NameArray] PostInit: UObject-sample validation found ZERO hits for any bits — "
				             "FNamePool chunks probably all paged out. Falling back to bits=16 "
				             "(UE4.22+ standard). If resolved names look garbled, rerun while the "
				             "target is actively in-game to keep pages resident.\n";
				NameArray::FNameBlockOffsetBits = std::max<int32>(NameArray::FNameBlockOffsetBits, 16);
			}
		}
		else
		{
			// Fallback to the legacy UObject-walking algorithm if ByteCursor wasn't discovered.
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
		}
		Off::InSDK::NameArray::FNamePoolBlockOffsetBits = NameArray::FNameBlockOffsetBits;

		std::cerr << "NameArray::FNameBlockOffsetBits: 0x" << std::hex << NameArray::FNameBlockOffsetBits << "\n" << std::endl;

		// Repopulate the FNamePool chunk cache now that the real block-offset bits are known.
		// The initial cache (built in InitializeNamePool) assumed the default bits; if PostInit
		// bumped them, the chunks are larger than we sized for and entries past the old size
		// would fall through to the slow path on every lookup.
		const int32 FinalNumChunks = GetNumChunks() + 1;
		const int32 FinalByteCursor = GetByteCursor();
		PopulateFNamePoolCache(
			reinterpret_cast<uintptr_t>(GNames),
			FinalNumChunks,
			static_cast<int32>(NameArray::FNameBlockOffsetBits),
			static_cast<int32>(NameEntryStride),
			FinalByteCursor);
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
