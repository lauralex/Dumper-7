#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <Windows.h>

#include "hv.h"

/*
 * Process-wide remote-memory primitives for reading the target game process via hv-nebula
 * hypercalls. Every pointer the Engine layer stores as `uint8*` is a remote VA in the target
 * process; reads/writes go through this namespace rather than through direct dereference.
 *
 * Initialization:
 *   RemoteMemory::Init(pid) resolves the target's CR3 and main module base.
 *   Must be called once before any Read/Write/ReadBuffer.
 *
 * Read<T>:           returns T{} on failure. Default for the surgical-replacement sweep.
 * TryRead<T>:        returns std::optional<T>. For callers that must distinguish
 *                    "zero bytes read" from "value is genuinely zero."
 * ReadBuffer:        multi-page-aware bulk read with explicit partial-read policy.
 */
namespace RemoteMemory
{
	enum class PartialReadPolicy
	{
		ErrorOnGap,     // Abort and return false if any page inside the range fails to read.
		ZeroFillOnGap,  // Zero-fill missing ranges and continue. Used for code-section bulk reads.
	};

	bool Init(DWORD pid);

	DWORD GetTargetPid();
	uint64_t GetCr3();
	uintptr_t GetMainModuleBase();
	size_t GetMainModuleSize();

	bool ReadBuffer(uintptr_t addr, void* dst, size_t size, PartialReadPolicy policy = PartialReadPolicy::ErrorOnGap);
	bool WriteBuffer(uintptr_t addr, const void* src, size_t size);

	bool IsValid(uintptr_t addr);

	template <typename T>
	inline T Read(uintptr_t addr)
	{
		T value = {};
		ReadBuffer(addr, &value, sizeof(T), PartialReadPolicy::ZeroFillOnGap);
		return value;
	}

	template <typename T>
	inline T Read(const void* addr)
	{
		return Read<T>(reinterpret_cast<uintptr_t>(addr));
	}

	template <typename T>
	inline std::optional<T> TryRead(uintptr_t addr)
	{
		T value = {};
		if (!ReadBuffer(addr, &value, sizeof(T), PartialReadPolicy::ErrorOnGap))
			return std::nullopt;
		return value;
	}

	template <typename T>
	inline bool Write(uintptr_t addr, const T& value)
	{
		return WriteBuffer(addr, &value, sizeof(T));
	}
}

/*
 * Convenience wrappers for the dense `*reinterpret_cast<T*>(BaseUint8Ptr + Offset)` pattern the
 * Engine layer uses hundreds of times. In injected mode those were plain pointer dereferences;
 * externally every one of them has to go through a hypercall. The helpers below let the sweep
 * be mechanical: `*reinterpret_cast<T*>(Base + Off)` becomes `RDeref<T>(Base + Off)`.
 */
template <typename T>
inline T RDeref(const uint8_t* addr)
{
	return RemoteMemory::Read<T>(reinterpret_cast<uintptr_t>(addr));
}

template <typename T>
inline T RDeref(const void* addr)
{
	return RemoteMemory::Read<T>(reinterpret_cast<uintptr_t>(addr));
}

template <typename T>
inline T RDeref(uintptr_t addr)
{
	return RemoteMemory::Read<T>(addr);
}
