#pragma once

#include <vector>

#include "Unreal/ObjectArray.h"

#include "Platform.h"
#include "RemoteMemory.h"

namespace OffsetFinder
{
	constexpr int32 OffsetNotFound = -1;
	constexpr int32 OffsetFinderMinValue = Platform::Is32Bit() ? 0x18 : 0x28;

	template<int Alignement = 4, typename T>
	inline int32_t FindOffset(const std::vector<std::pair<void*, T>>& ObjectValuePair, int MinOffset = OffsetFinderMinValue, int MaxOffset = 0x1A0)
	{
		int32_t HighestFoundOffset = MinOffset;
		bool bFoundOffset = false;

		for (int i = 0; i < ObjectValuePair.size(); i++)
		{
			if (ObjectValuePair[i].first == nullptr)
			{
				std::cerr << "Dumper-7 ERROR: FindOffset is skipping ObjectValuePair[" << i << "] because .first is nullptr." << std::endl;
				continue;
			}

			// Pointer-valued probes must not pass a null expected value: struct padding reads
			// as zero at many offsets so a null expected-value false-matches everywhere. This
			// is a stripped-reflection hazard (callers do `FindObjectFast("X").GetAddress()`
			// which returns nullptr when the class isn't registered) and caused false
			// discoveries like UStruct::Children=0x28 on UE5 targets. Non-pointer probes with
			// genuine-zero expected values (e.g. ArrayDim==1) are still fine because their
			// offset ranges are controlled by MinOffset.
			if constexpr (std::is_pointer_v<T>)
			{
				if (ObjectValuePair[i].second == nullptr)
				{
					std::cerr << "Dumper-7 ERROR: FindOffset is skipping ObjectValuePair[" << i << "] because .second is nullptr." << std::endl;
					continue;
				}
			}

			for (int j = HighestFoundOffset; j < MaxOffset; j += Alignement)
			{
				const T TypedValueAtOffset = RDeref<T>(static_cast<uint8_t*>(ObjectValuePair[i].first) + j);

				if (TypedValueAtOffset == ObjectValuePair[i].second && j >= HighestFoundOffset)
				{
					bFoundOffset = true;

					if (j > HighestFoundOffset)
					{
						HighestFoundOffset = j;
						i = 0;
					}
					j = MaxOffset;
				}
			}
		}

		return bFoundOffset ? HighestFoundOffset : OffsetNotFound;
	}

	template<bool bCheckForVft = true>
	inline int32_t GetValidPointerOffset(const void* PtrObjA, const void* PtrObjB, int32_t StartingOffset, int32_t MaxOffset, bool bNeedsToBeInProcessMemory = false)
	{
		const uint8_t* ObjA = static_cast<const uint8_t*>(PtrObjA);
		const uint8_t* ObjB = static_cast<const uint8_t*>(PtrObjB);

		if (Platform::IsBadReadPtr(ObjA) || Platform::IsBadReadPtr(ObjB))
			return OffsetNotFound;

		for (int j = StartingOffset; j <= MaxOffset; j += sizeof(void*))
		{
			const uintptr_t ptrAtA = RDeref<uintptr_t>(ObjA + j);
			const uintptr_t ptrAtB = RDeref<uintptr_t>(ObjB + j);

			const bool bIsAValid = !Platform::IsBadReadPtr(ptrAtA)
				&& (bCheckForVft ? !Platform::IsBadReadPtr(RDeref<uintptr_t>(ptrAtA)) : true);
			const bool bIsBValid = !Platform::IsBadReadPtr(ptrAtB)
				&& (bCheckForVft ? !Platform::IsBadReadPtr(RDeref<uintptr_t>(ptrAtB)) : true);

			if (bNeedsToBeInProcessMemory)
			{
				if (!Platform::IsAddressInProcessRange(ptrAtA) || !Platform::IsAddressInProcessRange(ptrAtB))
					continue;
			}

			if (bIsAValid && bIsBValid)
				return j;
		}

		return OffsetNotFound;
	};

	/* UObject */
	int32_t FindUObjectFlagsOffset();
	int32_t FindUObjectIndexOffset();
	int32_t FindUObjectClassOffset();
	int32_t FindUObjectNameOffset();
	int32_t FindUObjectOuterOffset();

	void FixupHardcodedOffsets();
	void InitFNameSettings();
	void PostInitFNameSettings();

	/* UField */
	int32_t FindUFieldNextOffset();

	/* FField */
	int32_t FindFFieldNextOffset();
	int32_t FindFFieldNameOffset();
	int32_t NewFindFFieldNameOffset();
	int32_t FindFFieldClassOffset();
	int32_t FindFFieldEditorOnlyMetaDataOffset();

	/* FFieldClass */
	int32_t FindFieldClassCastFlagsOffset();

	/* UEnum */
	int32_t FindEnumNamesOffset();

	/* UStruct */
	int32_t FindSuperOffset();
	int32_t FindChildOffset();
	int32_t FindChildPropertiesOffset();
	int32_t FindStructSizeOffset();
	int32_t FindMinAlignmentOffset();
	int32_t FindStructBaseChainOffset();

	/* UFunction */
	int32_t FindFunctionFlagsOffset();
	int32_t FindFunctionNativeFuncOffset();

	/* UClass */
	int32_t FindCastFlagsOffset();
	int32_t FindDefaultObjectOffset();
	int32_t FindImplementedInterfacesOffset();

	/* Property */
	int32_t FindElementSizeOffset();
	int32_t FindArrayDimOffset();
	int32_t FindPropertyFlagsOffset();
	int32_t FindOffsetInternalOffset();

	/* BoolProperty */
	int32_t FindBoolPropertyBaseOffset();

	/* ObjectProperty */
	int32_t FindObjectPropertyClassOffset();

	/* EnumProperty */
	int32_t FindEnumPropertyBaseOffset();
	
	/* ByteProperty */
	int32_t FindBytePropertyEnumOffset();

	/* StructProperty */
	int32_t FindStructPropertyStructOffset();

	/* DelegateProperty */
	int32_t FindDelegatePropertySignatureFunctionOffset();

	/* ArrayProperty */
	int32_t FindInnerTypeOffset(const int32 PropertySize);

	/* SetProperty */
	int32_t FindSetPropertyBaseOffset(const int32 PropertySize);

	/* MapProperty */
	int32_t FindMapPropertyBaseOffset(const int32 PropertySize);

	/* InSDK -> ULevel */
	int32_t FindLevelActorsOffset();

	/* InSDK -> UDataTable */
	int32_t FindDatatableRowMapOffset();
}
