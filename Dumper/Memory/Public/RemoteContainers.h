#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

/*
 * Snapshot helpers for UC::TArray / UC::FString / UC::TMap containers that live in the target
 * process. The dumper dereferences these containers while walking game state (UEEnum value
 * tables, UEFField metadata maps, UEClass interfaces, and offset-finder probes like
 * FindFFieldEditorOnlyMetaDataOffset). Naive reinterpret_cast to UC::TMap* followed by
 * operator[] / .Num() would chase further pointers into target memory — which is exactly what
 * breaks when the dumper runs externally.
 *
 * These helpers read the container headers once, bulk-read the data arrays, and return local
 * copies plus the remote VAs of individual elements. Callers that need to wrap an element in a
 * UEObject / FName / UEProperty handle should use the *remote* address; callers that need the
 * element as a value (e.g. an FString payload) get the local copy.
 *
 * v1 intentionally only covers the container shapes the Engine layer actually walks:
 *   - TArray<T>: sized element + bulk read
 *   - FString: wide-char buffer bulk read
 *   - TMap<NameNByte, FString>: snapshot pair remote-addresses + FString payloads
 *   - TArray<TPair<NameNByte, ValueType>>: snapshot pair remote-addresses + raw value bytes
 *
 * More shapes can be added if later offset-finder probes need them.
 */
namespace RemoteContainers
{
	// =============================================================================================
	// Low-level primitives
	// =============================================================================================

	struct TArrayHeader
	{
		uintptr_t Data;  // remote VA of element storage
		int32_t Num;
		int32_t Max;

		bool IsValid() const;
	};

	// Reads a UC::TArray<T> header (16 bytes) from a remote address. Returns {0, 0, 0} on failure.
	TArrayHeader ReadTArrayHeader(uintptr_t remoteHeaderAddr);

	// Reads a UC::FBitArray from a remote address. Layout (aligned to 8 on x64):
	//   { int32 InlineData[4], int32* SecondaryData, int32 NumBits, int32 MaxBits }
	// Returns the actual bit words (4 inline OR N secondary, based on NumBits) in the output.
	// Returns empty vector on failure.
	std::vector<uint32_t> ReadFBitArray(uintptr_t remoteBitArrayAddr, int32_t* outNumBits = nullptr);

	// Test a single bit in the bit array vector.
	inline bool TestBit(const std::vector<uint32_t>& bits, int32_t index)
	{
		const int32_t word = index >> 5;
		if (word < 0 || static_cast<size_t>(word) >= bits.size())
			return false;
		return (bits[word] >> (index & 31)) & 1u;
	}

	// =============================================================================================
	// RemoteFString — bulk-read the wide-char buffer of a remote UC::FString.
	// =============================================================================================

	class RemoteFString
	{
	public:
		RemoteFString() = default;
		explicit RemoteFString(uintptr_t remoteFStringHeaderAddr);

		bool IsValid() const { return m_valid; }
		int32_t Num() const { return m_num; }

		std::string ToString() const;
		std::wstring ToWString() const;

	private:
		std::wstring m_local;
		int32_t m_num = 0;
		bool m_valid = false;
	};

	// =============================================================================================
	// TSparseArray-shaped walkers — TMap and TSet are built on TSparseArray<SetElement<T>>.
	// =============================================================================================

	// UC::TSparseArray layout (56 bytes total on x64):
	//   0x00: TArray<FElementOrFreeListLink> Data              (16 bytes)
	//   0x10: FBitArray AllocationFlags                         (32 bytes)
	//   0x30: int32 FirstFreeIndex                              (4)
	//   0x34: int32 NumFreeIndices                              (4)
	constexpr size_t kTSparseArraySize = 56;
	constexpr size_t kTSparseArrayDataArrayOffset = 0;
	constexpr size_t kTSparseArrayAllocationFlagsOffset = 16;
	constexpr size_t kTSparseArrayNumFreeIndicesOffset = 52;

	// Returns the remote addresses of all *active* elements in a TSparseArray.
	// The FElementOrFreeListLink size must be passed by the caller (it's max(sizeof(T), 8)
	// rounded up to the element alignment, i.e. normally just sizeof(SetElement<T>) for T's
	// used by the Engine layer).
	std::vector<uintptr_t> GetActiveSparseArrayElementAddresses(uintptr_t sparseArrayRemoteAddr, size_t elementSize);

	// UC::TSet layout:
	//   0x00: TSparseArray<SetElement<T>> Elements              (56 bytes)
	//   0x38: TInlineAllocator<1>::ForElementType<int32> Hash   (16 bytes, 4 inline + 8 ptr + pad)
	//   0x48: int32 HashSize                                    (4)
	//
	// Since the Engine only ever iterates an active-element set via Num() and operator[],
	// RemoteTMap can skip the hash completely.

	// UC::SetElement<TPair<K, V>> layout:
	//   0x00: TPair<K, V> Value    (sizeof(K) + sizeof(V), padding as usual)
	//   +end: int32 HashNextId
	//   +end: int32 HashIndex
	//
	// For TMap<K, V>::operator[](i), the returned value is Elements[i].Value, which is
	// SetElement::Value, which is the TPair. TPair is { K First, V Second } laid out in order.

	// =============================================================================================
	// Typed helpers for the two container shapes the Engine layer actually walks.
	// =============================================================================================

	// Walks a UC::TMap<KeyOfSize, FString>* for a NameNByte → FString mapping.
	// Returns one entry per active pair with:
	//   - KeyRemoteAddr: remote VA of the K payload (suitable for FName(KeyRemoteAddr))
	//   - Value:         locally snapshotted RemoteFString (ToString / ToWString ready)
	struct NameFStringPair
	{
		uintptr_t KeyRemoteAddr;
		RemoteFString Value;
	};

	// keySize is the size of the key POD (e.g. 8 for Name08Byte, 16 for Name16Byte).
	// FString sits in the TPair right after the key, aligned to 8, and is 16 bytes.
	std::vector<NameFStringPair> ReadNameFStringMap(uintptr_t mapRemoteAddr, size_t keySize);

	// Walks a UC::TArray<TPair<KeyOfSize, ValueOfSize>>.
	// Returns one entry per array index with:
	//   - KeyRemoteAddr:   remote VA of the K payload
	//   - ValueBytes:      raw bytes of the value field (max 8; callers cast as needed)
	//   - ValueRemoteAddr: remote VA of the V payload (for callers that want to read further)
	struct NameValuePairEntry
	{
		uintptr_t KeyRemoteAddr;
		uintptr_t ValueRemoteAddr;
		uint64_t ValueBytes;   // zero-extended copy of the value (int8..int64)
	};

	std::vector<NameValuePairEntry> ReadNameValueTArray(
		uintptr_t tarrayHeaderRemoteAddr,
		size_t keySize,
		size_t valueSize);

	// Walks a UC::TArray<T> where T is a trivially-copyable POD, returning raw bytes.
	// Useful for small arrays of primitive values (int64[], uintptr_t[], etc.).
	bool ReadPodTArray(uintptr_t tarrayHeaderRemoteAddr, size_t elementSize, std::vector<uint8_t>& outBytes, int32_t* outNum = nullptr);
}
