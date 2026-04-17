#include "RemoteContainers.h"
#include "RemoteMemory.h"

#include <Windows.h>
#include <cstring>
#include <iostream>

namespace RemoteContainers
{
	// =============================================================================================
	// TArray header
	// =============================================================================================

	bool TArrayHeader::IsValid() const
	{
		// Sanity cap: Unreal's TArrays in the metadata paths we walk (enum name/value pairs,
		// TMap<FName, FString> editor metadata, TArray<FImplementedInterface>, etc.) are small.
		// A Num in the hundreds of thousands is already suspicious; millions means we read
		// garbage (uninitialised FField memory on stripped targets). Treating such headers
		// as invalid prevents us from reserve()-ing gigabytes and either OOM-ing or spending
		// minutes on hypercalls to validate a ghost buffer.
		constexpr int32_t kMaxSaneNum = 1 << 20; // 1 million
		if (Data == 0 || Num <= 0 || Max < Num)
			return false;
		if (Num > kMaxSaneNum)
			return false;
		// Reject obvious non-userspace pointers (below 0x10000 is never heap, above canonical
		// userspace upper bound is kernel memory we can't read anyway).
		if (Data < 0x10000 || Data >= 0x0000800000000000ULL)
			return false;
		return true;
	}

	TArrayHeader ReadTArrayHeader(uintptr_t remoteHeaderAddr)
	{
		TArrayHeader h{};
		if (remoteHeaderAddr == 0)
			return h;

		struct RawHeader
		{
			uint64_t Data;
			int32_t Num;
			int32_t Max;
		};

		auto raw = RemoteMemory::TryRead<RawHeader>(remoteHeaderAddr);
		if (!raw)
			return h;

		h.Data = static_cast<uintptr_t>(raw->Data);
		h.Num = raw->Num;
		h.Max = raw->Max;
		return h;
	}

	// =============================================================================================
	// FBitArray
	// =============================================================================================

	// Layout (on x64):
	//   0x00: int32 InlineData[4]          (16 bytes)
	//   0x10: int32* SecondaryData          (8 bytes)
	//   0x18: int32 NumBits                 (4)
	//   0x1C: int32 MaxBits                 (4)
	// Total 32 bytes.
	constexpr size_t kFBitArrayInlineBytes = 16;
	constexpr size_t kFBitArrayInlineWords = 4;
	constexpr size_t kFBitArraySecondaryOffset = 16;
	constexpr size_t kFBitArrayNumBitsOffset = 24;
	constexpr size_t kFBitArrayMaxBitsOffset = 28;

	std::vector<uint32_t> ReadFBitArray(uintptr_t remoteBitArrayAddr, int32_t* outNumBits)
	{
		std::vector<uint32_t> bits;
		if (remoteBitArrayAddr == 0)
			return bits;

		struct RawBitArray
		{
			uint32_t Inline[4];
			uint64_t Secondary;
			int32_t NumBits;
			int32_t MaxBits;
		};

		auto raw = RemoteMemory::TryRead<RawBitArray>(remoteBitArrayAddr);
		if (!raw)
			return bits;

		if (outNumBits)
			*outNumBits = raw->NumBits;

		const int32_t numBits = raw->NumBits;
		if (numBits <= 0)
			return bits;

		// Same sanity cap as TArrayHeader: FBitArray on stripped-reflection targets can read
		// back with gigantic NumBits and we'd allocate GB of zeros for no reason.
		constexpr int32_t kMaxSaneBits = 1 << 22; // 4 million bits
		if (numBits > kMaxSaneBits)
			return bits;

		const int32_t numWords = (numBits + 31) >> 5;
		bits.resize(numWords, 0);

		const bool usingSecondary = (raw->Secondary != 0) && (static_cast<size_t>(numWords) > kFBitArrayInlineWords);

		if (usingSecondary)
		{
			if (!RemoteMemory::ReadBuffer(static_cast<uintptr_t>(raw->Secondary),
			                              bits.data(),
			                              numWords * sizeof(uint32_t),
			                              RemoteMemory::PartialReadPolicy::ErrorOnGap))
			{
				bits.clear();
				return bits;
			}
		}
		else
		{
			const size_t inlineWordsToCopy = (static_cast<size_t>(numWords) < kFBitArrayInlineWords)
				? static_cast<size_t>(numWords)
				: kFBitArrayInlineWords;
			std::memcpy(bits.data(), raw->Inline, inlineWordsToCopy * sizeof(uint32_t));

			if (static_cast<size_t>(numWords) > kFBitArrayInlineWords && raw->Secondary != 0)
			{
				const size_t remaining = numWords - kFBitArrayInlineWords;
				if (!RemoteMemory::ReadBuffer(static_cast<uintptr_t>(raw->Secondary),
				                              bits.data() + kFBitArrayInlineWords,
				                              remaining * sizeof(uint32_t),
				                              RemoteMemory::PartialReadPolicy::ErrorOnGap))
				{
					bits.clear();
					return bits;
				}
			}
		}

		return bits;
	}

	// =============================================================================================
	// RemoteFString
	// =============================================================================================

	RemoteFString::RemoteFString(uintptr_t remoteFStringHeaderAddr)
	{
		if (remoteFStringHeaderAddr == 0)
			return;

		const TArrayHeader header = ReadTArrayHeader(remoteFStringHeaderAddr);
		if (!header.IsValid())
			return;

		// FString's Num includes the null terminator. Sanity cap to avoid trying to allocate
		// many MB if we read a corrupt header on some unmapped candidate.
		constexpr int32_t kMaxSaneFStringChars = 0x2000;
		if (header.Num > kMaxSaneFStringChars)
			return;

		std::wstring buffer;
		buffer.resize(header.Num);
		if (!RemoteMemory::ReadBuffer(header.Data,
		                              buffer.data(),
		                              header.Num * sizeof(wchar_t),
		                              RemoteMemory::PartialReadPolicy::ErrorOnGap))
			return;

		// Trim trailing null if present.
		if (!buffer.empty() && buffer.back() == L'\0')
			buffer.pop_back();

		m_local = std::move(buffer);
		m_num = header.Num;
		m_valid = true;
	}

	std::string RemoteFString::ToString() const
	{
		if (!m_valid)
			return {};
		// UTF-16 → UTF-8 (narrow). WideCharToMultiByte is the cleanest Windows-only path here.
		if (m_local.empty())
			return {};
		const int neededBytes = WideCharToMultiByte(CP_UTF8, 0,
			m_local.data(), static_cast<int>(m_local.size()),
			nullptr, 0, nullptr, nullptr);
		if (neededBytes <= 0)
			return {};
		std::string out(neededBytes, '\0');
		WideCharToMultiByte(CP_UTF8, 0,
			m_local.data(), static_cast<int>(m_local.size()),
			out.data(), neededBytes, nullptr, nullptr);
		return out;
	}

	std::wstring RemoteFString::ToWString() const
	{
		return m_valid ? m_local : std::wstring{};
	}

	// =============================================================================================
	// TSparseArray active-element address walker
	// =============================================================================================

	std::vector<uintptr_t> GetActiveSparseArrayElementAddresses(uintptr_t sparseArrayRemoteAddr, size_t elementSize)
	{
		std::vector<uintptr_t> active;
		if (sparseArrayRemoteAddr == 0 || elementSize == 0)
			return active;

		// Read the TArray<FElementOrFreeListLink> header at offset 0.
		const TArrayHeader dataArrayHeader = ReadTArrayHeader(sparseArrayRemoteAddr + kTSparseArrayDataArrayOffset);
		if (!dataArrayHeader.IsValid())
			return active;

		// Read the AllocationFlags FBitArray at offset 16.
		int32_t numBits = 0;
		const std::vector<uint32_t> allocationBits = ReadFBitArray(
			sparseArrayRemoteAddr + kTSparseArrayAllocationFlagsOffset, &numBits);
		if (allocationBits.empty())
			return active;

		// NumAllocated == dataArrayHeader.Num in the injected implementation. Clamp to the
		// smaller of the two.
		const int32_t numAllocated = (dataArrayHeader.Num < numBits) ? dataArrayHeader.Num : numBits;

		active.reserve(numAllocated);
		for (int32_t i = 0; i < numAllocated; ++i)
		{
			if (!TestBit(allocationBits, i))
				continue;
			active.push_back(dataArrayHeader.Data + (static_cast<uintptr_t>(i) * elementSize));
		}
		return active;
	}

	// =============================================================================================
	// TMap<NameOfSize, FString> walker
	// =============================================================================================

	std::vector<NameFStringPair> ReadNameFStringMap(uintptr_t mapRemoteAddr, size_t keySize)
	{
		std::vector<NameFStringPair> result;
		if (mapRemoteAddr == 0 || keySize == 0)
			return result;

		// TPair<K, V> layout: { K First; V Second; } with padding for alignment.
		//   For Name08Byte + FString: alignof(TPair) = 8 (FString holds a wchar_t*)
		//   so FString lives right after the key at offset keySize (already 8-aligned).
		//   For Name16Byte + FString: same, at offset 16.
		// sizeof(TPair<K,FString>) = keySize + 16 (FString is { wchar_t*, int32 Num, int32 Max }).
		//
		// SetElement<TPair<K, V>> layout: { TPair value; int32 HashNextId; int32 HashIndex; }
		//   aligned to alignof(TPair) = 8. Total = sizeof(TPair) + 8 (HashNext + HashIndex,
		//   both int32), but HashIndex may need trailing alignment padding. In the injected
		//   build the struct is laid out by the compiler; we just need the stride.
		const size_t pairSize = keySize + 16;                          // sizeof(TPair<K, FString>)
		size_t setElementSize = pairSize + 8;                          // + HashNextId, HashIndex
		// Align setElementSize up to 8 (TPair's alignment — wchar_t* in FString pulls alignment to 8).
		setElementSize = (setElementSize + 7) & ~size_t{7};

		// Iterate the map's Elements (TSparseArray<SetElement<TPair<K, FString>>>).
		// TMap has exactly one member: TSet, which has TSparseArray at offset 0. So the
		// TSparseArray sits at offset 0 of the TMap.
		const std::vector<uintptr_t> elementAddrs = GetActiveSparseArrayElementAddresses(mapRemoteAddr, setElementSize);

		result.reserve(elementAddrs.size());
		for (uintptr_t elemAddr : elementAddrs)
		{
			// elemAddr points at the SetElement; SetElement::Value is at offset 0 = the TPair.
			const uintptr_t pairAddr = elemAddr;
			NameFStringPair entry;
			entry.KeyRemoteAddr = pairAddr;
			entry.Value = RemoteFString(pairAddr + keySize);
			result.push_back(std::move(entry));
		}
		return result;
	}

	// =============================================================================================
	// TArray<TPair<NameOfSize, ValueOfSize>> walker
	// =============================================================================================

	std::vector<NameValuePairEntry> ReadNameValueTArray(
		uintptr_t tarrayHeaderRemoteAddr,
		size_t keySize,
		size_t valueSize)
	{
		std::vector<NameValuePairEntry> result;
		if (tarrayHeaderRemoteAddr == 0 || keySize == 0 || valueSize == 0 || valueSize > 8)
			return result;

		const TArrayHeader header = ReadTArrayHeader(tarrayHeaderRemoteAddr);
		if (!header.IsValid())
			return result;

		// Sanity cap — enum value tables shouldn't have millions of entries.
		constexpr int32_t kMaxSaneEnumValues = 0x10000;
		if (header.Num > kMaxSaneEnumValues)
			return result;

		// TPair stride: K + V rounded up to max alignment. In practice the pair is either
		// 8 (Name08 + int8..int64 snug) or 16 (Name08 + int64 padded, or Name16 + whatever).
		// We'll trust the caller-supplied keySize + valueSize + 7-byte-up alignment.
		size_t pairStride = keySize + valueSize;
		// Align up to 8 (matches compiler behavior for pairs containing 8-byte values or
		// 16-byte keys).
		pairStride = (pairStride + 7) & ~size_t{7};

		std::vector<uint8_t> buffer(static_cast<size_t>(header.Num) * pairStride);
		if (!RemoteMemory::ReadBuffer(header.Data, buffer.data(), buffer.size(),
		                              RemoteMemory::PartialReadPolicy::ErrorOnGap))
			return result;

		result.reserve(header.Num);
		for (int32_t i = 0; i < header.Num; ++i)
		{
			NameValuePairEntry entry;
			entry.KeyRemoteAddr = header.Data + (i * pairStride);
			entry.ValueRemoteAddr = entry.KeyRemoteAddr + keySize;
			entry.ValueBytes = 0;
			std::memcpy(&entry.ValueBytes, buffer.data() + (i * pairStride) + keySize, valueSize);
			result.push_back(entry);
		}
		return result;
	}

	// =============================================================================================
	// POD TArray bulk read
	// =============================================================================================

	bool ReadPodTArray(uintptr_t tarrayHeaderRemoteAddr, size_t elementSize, std::vector<uint8_t>& outBytes, int32_t* outNum)
	{
		outBytes.clear();
		if (tarrayHeaderRemoteAddr == 0 || elementSize == 0)
			return false;

		const TArrayHeader header = ReadTArrayHeader(tarrayHeaderRemoteAddr);
		if (!header.IsValid())
		{
			if (outNum) *outNum = 0;
			return false;
		}

		if (outNum) *outNum = header.Num;
		outBytes.resize(static_cast<size_t>(header.Num) * elementSize);
		return RemoteMemory::ReadBuffer(header.Data, outBytes.data(), outBytes.size(),
		                                RemoteMemory::PartialReadPolicy::ErrorOnGap);
	}
}
