#include <vector>

#include "OffsetFinder/OffsetFinder.h"
#include "Unreal/ObjectArray.h"

#include "Platform.h"
#include "RemoteMemory.h"
#include "RemoteContainers.h"

/* UObject */
int32_t OffsetFinder::FindUObjectFlagsOffset()
{
	constexpr auto EnumFlagValueToSearch = 0x43;

	/* We're looking for a commonly occuring flag and this number basically defines the minimum number that counts ad "commonly occuring". */
	constexpr auto MinNumFlagValuesRequiredAtOffset = 0xA0;

	for (int i = 0; i < 0x20; i++)
	{
		int Offset = 0x0;
		while (Offset != OffsetNotFound)
		{
			// Look for 0x43 in this object, as it is a really common value for UObject::Flags
			Offset = FindOffset(std::vector{ std::pair{ ObjectArray::GetByIndex(i).GetAddress(), EnumFlagValueToSearch } }, Offset, 0x40);

			if (Offset == OffsetNotFound)
				break; // Early exit

			/* We're looking for a common flag. To check if the flag  is common we're checking the first 0x100 objects to see how often the flag occures at this offset. */
			int32 NumObjectsWithFlagAtOffset = 0x0;

			int Counter = 0;
			for (UEObject Obj : ObjectArray())
			{
				// Only check the (possible) flags of the first 0x100 objects
				if (Counter++ == 0x100)
					break;

				const int32 TypedValueAtOffset = RDeref<int32>(reinterpret_cast<uintptr_t>(Obj.GetAddress()) + Offset);

				if (TypedValueAtOffset == EnumFlagValueToSearch)
					NumObjectsWithFlagAtOffset++;
			}

			if (NumObjectsWithFlagAtOffset > MinNumFlagValuesRequiredAtOffset)
				return Offset;

			// FindOffset returns HighestFoundOffset == MinOffset when a match is at the scan's
			// starting position, so without advancing past it here the while-loop spins forever
			// whenever the count-check rejects the first candidate (common when 0x43 isn't the
			// dominant flag value in the first 256 objects for this particular game).
			Offset += sizeof(int32);
		}
	}

	return OffsetNotFound;
}

int32_t OffsetFinder::FindUObjectIndexOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	Infos.emplace_back(ObjectArray::GetByIndex(0x055).GetAddress(), 0x055);
	Infos.emplace_back(ObjectArray::GetByIndex(0x123).GetAddress(), 0x123);

	return FindOffset<4>(Infos, sizeof(void*)); // Skip VTable
}

int32_t OffsetFinder::FindUObjectClassOffset()
{
	/* Checks for a pointer that points to itself in the end. The UObject::Class pointer of "Class CoreUObject.Class" will point to "Class CoreUObject.Class". */
	auto IsValidCyclicUClassPtrOffset = [](const uint8_t* ObjA, const uint8_t* ObjB, int32_t ClassPtrOffset)
	{
		/* Will be advanced before they are used. */
		const uint8_t* NextClassA = ObjA;
		const uint8_t* NextClassB = ObjB;

		for (int MaxLoopCount = 0; MaxLoopCount < 0x10; MaxLoopCount++)
		{
			const uint8_t* CurrentClassA = NextClassA;
			const uint8_t* CurrentClassB = NextClassB;

			NextClassA = reinterpret_cast<const uint8_t*>(RDeref<uintptr_t>(NextClassA + ClassPtrOffset));
			NextClassB = reinterpret_cast<const uint8_t*>(RDeref<uintptr_t>(NextClassB + ClassPtrOffset));

			/* If this was UObject::Class it would never be invalid. The pointer would simply point to itself.*/
			if (!NextClassA || !NextClassB || Platform::IsBadReadPtr(NextClassA) || Platform::IsBadReadPtr(NextClassB))
				return false;

			if (CurrentClassA == NextClassA && CurrentClassB == NextClassB)
				return true;
		}

		return false;
	};

	const uint8_t* const ObjA = static_cast<const uint8_t*>(ObjectArray::GetByIndex(0x055).GetAddress());
	const uint8_t* const ObjB = static_cast<const uint8_t*>(ObjectArray::GetByIndex(0x123).GetAddress());

	int32_t Offset = 0;
	while (Offset != OffsetNotFound)
	{
		Offset = GetValidPointerOffset<true>(ObjA, ObjB, Offset + sizeof(void*), 0x50);

		if (IsValidCyclicUClassPtrOffset(ObjA, ObjB, Offset))
			return Offset;
	}

	return OffsetNotFound;
}

/*
* IsPotentialValidOffset: A function to filter offsets that can not possibly be valid for UObject::Name or FField::Name.
*						  Example for UObject::Name: it can 100% not be at the same offset as UObject::Class
* 
* DataGatherer: A function to gather values at the offsets not filterd by 'IsPotentialValidOffset'. Data is later used to filter more offsets, until hopefully only one is left.
*/
template<typename IteratorType>
int32_t FindNameOffsetForSomeClass(std::function<bool(int32_t Value)> IsPotentialValidOffset, IteratorType DataSetStartIterator, IteratorType DataSetEndIterator)
{
	/*
	* Requirements:
	*	- CmpIdx > 0x10 && CmpIdx < 0xF0000000
	*	- AverageValue >= 0x100 && AverageValue <= 0xFF00000;
	*	- Offset != { OtherOffsets }
	*/

	/* A struct describing the value */
	struct ValueInfo
	{
		int32 Offset;					   // Offset from the UObject start to this value
		int32 NumNamesWithLowCmpIdx = 0x0; // The number of names where the comparison index is in the range [0, 16]. Usually this should be far less than 0x20 names.
		uint64 TotalValue = 0x0;		   // The total value of the int32 data at this offset over all objects in GObjects
		bool bIsValidCmpIdxRange = true;   // Whether this value could be a valid FName::ComparisonIndex
	};


	std::vector<ValueInfo> PossibleOffsets;

	constexpr auto MaxAllowedComparisonIndexValue = 0x4000000; // Somewhat arbitrary limit. Make sure this isn't too low for games on FNamePool with lots of names and 0x14 block-size bits

	constexpr auto MaxAllowedAverageComparisonIndexValue = MaxAllowedComparisonIndexValue / 2; // Also somewhat arbitrary limit, but the average value shouldn't be as high as the max allowed one
	constexpr auto MinAllowedAverageComparisonIndexValue = 0x280; // If the average name is below 0x100 it is either the smallest UE application ever, or not the right offset

	constexpr auto LowComparisonIndexUpperCap = 0x10; // The upper limit of what is considered a "low" comparison index
	constexpr auto MaxAllowedNamesWithLowCmpIdx = 0x40;


	for (int i = sizeof(void*); i <= 0x40; i += 0x4)
	{
		if (!IsPotentialValidOffset(i))
			continue;

		PossibleOffsets.push_back(ValueInfo{ i });
	}

	auto GetDataAtOffsetAsInt = [](const void* Ptr, int32 Offset) -> uint32 { return RDeref<uint32>(reinterpret_cast<const uintptr_t>(Ptr) + Offset); };

	int NumObjectsConsidered = 0;

	for (; DataSetStartIterator != DataSetEndIterator; ++DataSetStartIterator)
	{
		constexpr auto X86SmallPageSize = 0x1000;
		constexpr auto MaxAccessedSizeInUObject = 0x44;

		const void* CurrentObjectOrField = (*DataSetStartIterator).GetAddress();

		/*
		* Purpose: Make sure all offsets in the UObject::Name finder can be accessed
		* Reasoning: Objects are allocated in Blocks, these allocations are page-aligned in both size and base. If an object + MaxAccessedSizeInUObject goes past the page-bounds
		*            it might also go past the extends of an allocation. There's no reliable way of getting the size of UObject without knowing it's offsets first.
		*/
		const bool bIsGoingPastPageBounds = (reinterpret_cast<const uintptr_t>(CurrentObjectOrField) & (X86SmallPageSize - 1)) > (X86SmallPageSize - MaxAccessedSizeInUObject);
		if (bIsGoingPastPageBounds)
			continue;

		NumObjectsConsidered++;

		for (ValueInfo& Info : PossibleOffsets)
		{
			const uint32 ValueAtOffset = GetDataAtOffsetAsInt(CurrentObjectOrField, Info.Offset);

			Info.TotalValue += ValueAtOffset;
			Info.bIsValidCmpIdxRange = Info.bIsValidCmpIdxRange && ValueAtOffset < MaxAllowedComparisonIndexValue;
			Info.NumNamesWithLowCmpIdx += (ValueAtOffset <= LowComparisonIndexUpperCap);
		}
	}

	if (NumObjectsConsidered == 0)
	{
		std::cerr << "[FindNameOffsetForSomeClass] No objects were considered (probe ran over an empty iterator). "
		             "Usually this means the Field walk is broken by paged-out target memory; returning OffsetNotFound.\n";
		return -1;
	}

	int32 FirstValidOffset = -1;
	for (const ValueInfo& Info : PossibleOffsets)
	{
		const auto AverageValue = (Info.TotalValue / NumObjectsConsidered);

		if (Info.bIsValidCmpIdxRange && Info.NumNamesWithLowCmpIdx <= MaxAllowedNamesWithLowCmpIdx
			&& AverageValue >= MinAllowedAverageComparisonIndexValue && AverageValue <= MaxAllowedAverageComparisonIndexValue)
		{
			if (FirstValidOffset == -1)
			{
				FirstValidOffset = Info.Offset;
				continue;
			}

			/* This shouldn't be the case, so log it as an info but continue, as the first offset is still likely the right one. */
			std::cerr << std::format("Dumper-7: Another [UObject/FField]::Name offset (0x{:04X}) is also considered valid.\n", Info.Offset);
		}
	}

	return FirstValidOffset;
}

int32_t OffsetFinder::FindUObjectNameOffset()
{
	auto IsPotentiallyValidOffset = [](int32 Offset) -> bool
	{
		// Make sure 0x4 aligned Offsets are neither the start, nor the middle of a pointer-member. Irrelevant for 32-bit, because the 2nd check will be 0x2 aligned then.
		return Offset != Off::UObject::Class && Offset != (Off::UObject::Class + (sizeof(void*) / 2))
			&& Offset != Off::UObject::Outer && Offset != (Off::UObject::Outer + (sizeof(void*) / 2))
			&& Offset != Off::UObject::Flags
			&& Offset != Off::UObject::Index
			&& Offset != Off::UObject::Vft && Offset != (Off::UObject::Vft + (sizeof(void*) / 2));
	};

	return FindNameOffsetForSomeClass(IsPotentiallyValidOffset, ObjectArray().begin(), ObjectArray().end());
}

int32_t OffsetFinder::FindUObjectOuterOffset()
{
	int32_t LowestFoundOffset = 0xFFFF;

	// Outer always lives after Name (FName, 8B) in the UObject layout. Starting the scan at
	// Name + 8 avoids false positives where the 8 bytes at Name (ComparisonIndex + Number)
	// happen to pass the pointer-validity probe.
	const int32_t MinOuter = Off::UObject::Name > 0 ? (Off::UObject::Name + sizeof(int32) * 2) : (Off::UObject::Class > 0 ? Off::UObject::Class : 0);
	const int32_t InitialOffset = MinOuter - static_cast<int32_t>(sizeof(void*)); // the inner loop adds sizeof(void*) on entry

	// loop a few times in case we accidentally choose a UPackage (which doesn't have an Outer) to find Outer
	for (int i = 0; i < 0x10; i++)
	{
		int32_t Offset = InitialOffset;

		const void* ObjA = ObjectArray::GetByIndex(rand() % 0x400).GetAddress();
		const void* ObjB = ObjectArray::GetByIndex(rand() % 0x400).GetAddress();

		while (Offset != OffsetNotFound)
		{
			Offset = GetValidPointerOffset(ObjA, ObjB, Offset + sizeof(void*), 0x50);

			// Make sure we didn't re-find the Class offset or Index (if the Index filed is a valid pionter for some ungodly reason).
			if (Offset != Off::UObject::Class && Offset != Off::UObject::Index)
				break;
		}

		if (Offset != OffsetNotFound && Offset < LowestFoundOffset)
			LowestFoundOffset = Offset;
	}

	return LowestFoundOffset == 0xFFFF ? OffsetNotFound : LowestFoundOffset;
}

void OffsetFinder::FixupHardcodedOffsets()
{
	if (Settings::Internal::bUseCasePreservingName)
	{
		Off::FField::Flags += 0x8;

		Off::FFieldClass::Id += 0x08;
		Off::FFieldClass::CastFlags += 0x08;
		Off::FFieldClass::ClassFlags += 0x08;
		Off::FFieldClass::SuperClass += 0x08;
	}

	if (!Settings::Internal::bUseFProperty)
		return;

	/*
	 * Detect UE5.1.1+ FFieldVariant shrinkage (removal of bOwnerIsField bool + 7B padding).
	 *
	 * Primary: the legacy +0x18 pointer-validity check across Actor/ActorComponent/Pawn.
	 * Backup:  score both candidate layouts by walking ChildProperties and counting how many
	 *          FField nodes expose a real FName. This only works if the heap pages are
	 *          resident; on hypercall-only external mode with paged-out target heaps both
	 *          scores come back 0.
	 *
	 * When BOTH are ambiguous (legacy check all-zero AND both scores zero) we fall back to
	 * keeping the pre-5.1.1 layout. Per-game overrides via Settings are still the escape hatch.
	 */
	std::vector<UEStruct> testStructs;
	for (const char* n : { "Actor", "ActorComponent", "Pawn", "Object",
	                       "Color", "Vector", "Vector2D", "Vector4", "Guid", "TwoVectors", "Transform" })
	{
		const UEStruct s = ObjectArray::FindStructFast(n);
		if (s)
			testStructs.push_back(s);
	}

	auto isLikelyRealName = [](const std::string& s) -> bool
	{
		if (s.empty() || s == "None")
			return false;
		const unsigned char c0 = static_cast<unsigned char>(s[0]);
		if (c0 < 0x20 || c0 > 0x7E)
			return false;
		if (!(std::isalpha(c0) || c0 == '_'))
			return false;
		for (unsigned char c : s)
		{
			if (c < 0x20 || c > 0x7E)
				return false;
		}
		return true;
	};

	auto scoreLayout = [&](int nextOff, int nameOff) -> int
	{
		int score = 0;
		for (UEStruct s : testStructs)
		{
			uintptr_t head = RDeref<uintptr_t>(reinterpret_cast<const uint8*>(s.GetAddress()) + Off::UStruct::ChildProperties);
			if (head == 0 || Platform::IsBadReadPtr(head))
				continue;

			for (int depth = 0; depth < 16; ++depth)
			{
				const FName asFName(reinterpret_cast<const uint8*>(head) + nameOff);
				const std::string resolved = asFName.ToString();
				if (isLikelyRealName(resolved))
					++score;

				const uintptr_t next = RDeref<uintptr_t>(reinterpret_cast<uint8*>(head) + nextOff);
				if (next == 0 || next == head || Platform::IsBadReadPtr(next))
					break;
				head = next;
			}
		}
		return score;
	};

	// Legacy pointer-validity probe (picks up 5.1.1+ when at least one of the three structs
	// has a Next pointer at the bool's old offset).
	const int32 OffsetToCheck = Off::FField::Owner + 0x8;
	const void* ActorChildPropsField     = ObjectArray::FindClassFast("Actor").GetChildProperties().GetAddress();
	const void* ActorCompChildPropsField = ObjectArray::FindClassFast("ActorComponent").GetChildProperties().GetAddress();
	const void* PawnChildPropsField      = ObjectArray::FindClassFast("Pawn").GetChildProperties().GetAddress();

	auto LegacyProbe = [&](const void* Head) -> bool
	{
		if (!Head || Platform::IsBadReadPtr(Head))
			return false;
		const void* val = RDeref<void*>(reinterpret_cast<const uint8*>(Head) + OffsetToCheck);
		return !Platform::IsBadReadPtr(val) && (reinterpret_cast<uintptr_t>(val) & 0x1) == 0;
	};
	const bool legacyHit = LegacyProbe(ActorChildPropsField) && LegacyProbe(ActorCompChildPropsField) && LegacyProbe(PawnChildPropsField);

	// Score-based probe (picks up 5.1.1+ when heap pages are resident enough to walk).
	const int scoreOld = scoreLayout(0x20, 0x28);
	const int scoreNew = scoreLayout(0x18, 0x20);

	std::cerr << std::format(
		"[FixupHardcodedOffsets] legacy-probe-hit={}, scores: old(Next=0x20,Name=0x28)={}, new(Next=0x18,Name=0x20)={}\n",
		legacyHit, scoreOld, scoreNew);

	// Diagnostic: dump UStruct bytes AND the first-FField bytes so a future debugging run can
	// tell whether the ChildProperties offset points into readable memory and what the first
	// FField slot actually holds. On stripped/protected targets these often come back as
	// zeros because the target's FField heap pages aren't physically resident — hv-nebula
	// reads physical memory via page tables and returns 0 bytes for a "not present" PTE.
	for (const char* n : { "Actor", "Color", "TwoVectors" })
	{
		UEStruct s = ObjectArray::FindStructFast(n);
		if (!s)
			continue;

		const uintptr_t cp = RDeref<uintptr_t>(reinterpret_cast<const uint8*>(s.GetAddress()) + Off::UStruct::ChildProperties);
		if (cp == 0)
			continue;

		uint8 buf[48]{};
		RemoteMemory::ReadBuffer(cp, buf, sizeof(buf), RemoteMemory::PartialReadPolicy::ZeroFillOnGap);
		const uint64_t phys = hv::get_physical_address(hv::g_cr3, reinterpret_cast<void*>(cp));
		std::cerr << std::format("[FixupHardcodedOffsets] {}.first-FField @ 0x{:X} phys=0x{:X} bytes:", n, cp, phys);
		for (int i = 0; i < 48; ++i)
			std::cerr << std::format(" {:02X}", buf[i]);
		std::cerr << "\n";
	}

	const bool applyNewLayout = legacyHit || (scoreNew > scoreOld && scoreNew > 0);

	if (applyNewLayout)
	{
		std::cerr << "[FixupHardcodedOffsets] Applying UE5.1.1+ FFieldVariant shrinkage fix\n";
		Settings::Internal::bUseMaskForFieldOwner = true;
		Off::FField::Next -= 0x08;
		Off::FField::Name -= 0x08;
		Off::FField::Flags -= 0x08;
	}
	else
	{
		std::cerr << "[FixupHardcodedOffsets] Keeping pre-5.1.1 FField layout\n";
	}
}

void OffsetFinder::InitFNameSettings()
{
	UEObject FirstObject = ObjectArray::GetByIndex(0);

	const uint8* NameAddress = static_cast<const uint8*>(FirstObject.GetFName().GetAddress());

	const int32 FNameFirstInt /* ComparisonIndex */ = RDeref<int32>(NameAddress);
	const int32 FNameSecondInt /* [Number/DisplayIndex] */ = RDeref<int32>(NameAddress + 0x4);

	/* Some games move 'Name' before 'Class'. Just substract the offset of 'Name' with the offset of the member that follows right after it, to get an estimate of sizeof(FName). */
	const int32 FNameSize = !Settings::Internal::bIsObjectNameBeforeClass ? (Off::UObject::Outer - Off::UObject::Name) : (Off::UObject::Class - Off::UObject::Name);

	Off::FName::CompIdx = 0x0;
	Off::FName::Number = 0x4; // defaults for check

	 // FNames for which FName::Number == [1...4]
	auto GetNumNamesWithNumberOneToFour = []() -> int32
	{
		int32 NamesWithNumberOneToFour = 0x0;

		for (UEObject Obj : ObjectArray())
		{
			const uint32 Number = Obj.GetFName().GetNumber();

			if (Number > 0x0 && Number < 0x5)
				NamesWithNumberOneToFour++;
		}

		return NamesWithNumberOneToFour;
	};

	/*
	* Games without FNAME_OUTLINE_NUMBER have a min. percentage of 6% of all object-names for which FName::Number is in a [1...4] range
	* On games with FNAME_OUTLINE_NUMBER the (random) integer after FName::ComparisonIndex is in the range from [1...4] about 2% (or less) of times.
	*
	* The minimum percentage of names is set to 3% to give both normal names, as well as outline-numer names a buffer-zone.
	*
	* This doesn't work on some very small UE template games, which is why PostInitFNameSettings() was added to fix the incorrect behavior of this function
	*/
	constexpr float MinPercentage = 0.03f;

	/* Minimum required ammount of names for which FName::Number is in a [1...4] range */
	const int32 FNameNumberThreashold = (ObjectArray::Num() * MinPercentage);

	Off::FName::CompIdx = 0x0;

	if (FNameSize == 0x8 && FNameFirstInt == FNameSecondInt) /* WITH_CASE_PRESERVING_NAME + FNAME_OUTLINE_NUMBER */
	{
		Settings::Internal::bUseCasePreservingName = true;
		Settings::Internal::bUseOutlineNumberName = true;

		Off::FName::Number = -0x1;
		Off::InSDK::Name::FNameSize = 0x8;
	}
	else if (FNameSize == 0x10) /* WITH_CASE_PRESERVING_NAME */
	{
		Settings::Internal::bUseCasePreservingName = true;

		Off::FName::Number = FNameFirstInt == FNameSecondInt ? 0x8 : 0x4;

		Off::InSDK::Name::FNameSize = 0xC;
	}
	else if (GetNumNamesWithNumberOneToFour() < FNameNumberThreashold) /* FNAME_OUTLINE_NUMBER */
	{
		Settings::Internal::bUseOutlineNumberName = true;

		Off::FName::Number = -0x1;

		Off::InSDK::Name::FNameSize = 0x4;
	}
	else /* Default */
	{
		Off::FName::Number = 0x4;

		Off::InSDK::Name::FNameSize = 0x8;
	}
}

void OffsetFinder::PostInitFNameSettings()
{
	const UEClass PlayerStart = ObjectArray::FindClassFast("PlayerStart");

	const int32 FNameSize = PlayerStart.FindMember("PlayerStartTag").GetSize();

	/* Nothing to do for us, everything is fine! */
	if (Off::InSDK::Name::FNameSize == FNameSize)
		return;

	/* We've used the wrong FNameSize to determine the offset of FField::Flags. Substract the old, wrong, size and add the new one.*/
	Off::FField::Flags = (Off::FField::Flags - Off::InSDK::Name::FNameSize) + FNameSize;

	const uint8* NameAddress = static_cast<const uint8*>(PlayerStart.GetFName().GetAddress());

	const int32 FNameFirstInt /* ComparisonIndex */ = RDeref<int32>(NameAddress);
	const int32 FNameSecondInt /* [Number/DisplayIndex] */ = RDeref<int32>(NameAddress + 0x4);

	if (FNameSize == 0x8 && FNameFirstInt == FNameSecondInt) /* WITH_CASE_PRESERVING_NAME + FNAME_OUTLINE_NUMBER */
	{
		Settings::Internal::bUseCasePreservingName = true;
		Settings::Internal::bUseOutlineNumberName = true;

		Off::FName::Number = -0x1;
		Off::InSDK::Name::FNameSize = 0x8;
	}
	else if (FNameSize > 0x8) /* WITH_CASE_PRESERVING_NAME */
	{
		Settings::Internal::bUseOutlineNumberName = false;
		Settings::Internal::bUseCasePreservingName = true;

		Off::FName::Number = FNameFirstInt == FNameSecondInt ? 0x8 : 0x4;

		Off::InSDK::Name::FNameSize = 0xC;
	}
	else if (FNameSize == 0x4) /* FNAME_OUTLINE_NUMBER */
	{
		Settings::Internal::bUseOutlineNumberName = true;
		Settings::Internal::bUseCasePreservingName = false;

		Off::FName::Number = -0x1;

		Off::InSDK::Name::FNameSize = 0x4;
	}
	else /* Default */
	{
		Settings::Internal::bUseOutlineNumberName = false;
		Settings::Internal::bUseCasePreservingName = false;

		Off::FName::Number = 0x4;
		Off::InSDK::Name::FNameSize = 0x8;
	}
}

/* UField */
int32_t OffsetFinder::FindUFieldNextOffset()
{
	const void* KismetSystemLibraryChild = ObjectArray::FindObjectFast<UEStruct>("KismetSystemLibrary").GetChild().GetAddress();
	const void* KismetStringLibraryChild = ObjectArray::FindObjectFast<UEStruct>("KismetStringLibrary").GetChild().GetAddress();

#undef max
	const auto HighestUObjectOffset = std::max({ Off::UObject::Index, Off::UObject::Name, Off::UObject::Flags, Off::UObject::Outer, Off::UObject::Class });
#define max(a,b)            (((a) > (b)) ? (a) : (b))

	return GetValidPointerOffset(KismetSystemLibraryChild, KismetStringLibraryChild, Align(HighestUObjectOffset + 0x4, static_cast<int>(sizeof(void*))), 0x60);
}

/* FField */
int32_t OffsetFinder::FindFFieldNextOffset()
{
	const void* GuidChildren = ObjectArray::FindStructFast("Guid").GetChildProperties().GetAddress();
	const void* VectorChildren = ObjectArray::FindStructFast("Vector").GetChildProperties().GetAddress();

	return GetValidPointerOffset(GuidChildren, VectorChildren, Off::FField::Owner + 0x8, 0x48);
}

int32_t OffsetFinder::FindFFieldNameOffset()
{
	// Bail out if the probe structs or their ChildProperties links aren't resolvable in the
	// target. Without valid GuidChild / VectorChild handles the inner loop would sweep
	// FField::Name across all 4-byte positions and call GetName() -> ToString() at each one;
	// with garbage FName bytes that produces FNamePool lookups with out-of-range ComparisonIndex
	// values and frequently segfaults. Let the caller fall back to the hardcoded UE5.x default.
	const UEStruct GuidStruct = ObjectArray::FindStructFast("Guid");
	const UEStruct VectorStruct = ObjectArray::FindStructFast("Vector");
	if (!GuidStruct || !VectorStruct)
		return OffsetNotFound;

	UEFField GuidChild = GuidStruct.GetChildProperties();
	UEFField VectorChild = VectorStruct.GetChildProperties();
	if (!GuidChild || !VectorChild)
		return OffsetNotFound;

	// Early bail if the ChildProperties pages aren't physically resident. Walking garbage
	// FNames calls into NameArray with out-of-range ComparisonIndex values and crashes the
	// dumper; a zero-byte first 16B indicates the heap page isn't faulted in and no
	// discovery is possible.
	auto pageLooksZero = [](const void* addr) -> bool
	{
		uint8 b[16]{};
		RemoteMemory::ReadBuffer(reinterpret_cast<uintptr_t>(addr), b, sizeof(b), RemoteMemory::PartialReadPolicy::ZeroFillOnGap);
		for (uint8 v : b)
			if (v != 0) return false;
		return true;
	};
	if (pageLooksZero(GuidChild.GetAddress()) || pageLooksZero(VectorChild.GetAddress()))
	{
		std::cerr << "[FindFFieldNameOffset] ChildProperties pages are zero-filled (not resident); skipping discovery\n";
		return OffsetNotFound;
	}

	std::string GuidChildName = GuidChild.GetName();
	std::string VectorChildName = VectorChild.GetName();

	if ((GuidChildName == "A" || GuidChildName == "D") && (VectorChildName == "X" || VectorChildName == "Z"))
		return Off::FField::Name;

	for (Off::FField::Name = Off::FField::Owner; Off::FField::Name < 0x40; Off::FField::Name += 4)
	{
		GuidChildName = GuidChild.GetName();
		VectorChildName = VectorChild.GetName();

		if ((GuidChildName == "A" || GuidChildName == "D") && (VectorChildName == "X" || VectorChildName == "Z"))
			return Off::FField::Name;
	}

	return OffsetNotFound;
}

int32_t OffsetFinder::NewFindFFieldNameOffset()
{
	auto IsPotentiallyValidOffset = [](int32 Offset) -> bool
	{
		// Make sure 0x4 aligned Offsets are neither the start, nor the middle of a pointer-member. Irrelevant for 32-bit, because the 2nd check will be 0x2 aligned then.
		return Offset != Off::FField::Class && Offset != (Off::FField::Class + (sizeof(void*) / 2))
			&& Offset != Off::FField::Next && Offset != (Off::FField::Next + (sizeof(void*) / 2))
			&& Offset != Off::FField::Vft && Offset != (Off::FField::Vft + (sizeof(void*) / 2));
	};

	AllFieldIterator TmpIt;

	return FindNameOffsetForSomeClass(IsPotentiallyValidOffset, TmpIt.begin(), TmpIt.end());
}

int32_t OffsetFinder::FindFFieldEditorOnlyMetaDataOffset()
{
	const UEFField GuidChild1 = ObjectArray::FindStructFast("Guid").GetChildProperties();
	const UEFField GuidChild2 = GuidChild1.GetNext();

	auto IsPotentiallyValidOffset = [](int32 Offset) -> bool
		{
			// Make sure 0x4 aligned Offsets are neither the start, nor the middle of a pointer-member. Irrelevant for 32-bit, because the 2nd check will be 0x2 aligned then.
			return Offset != Off::FField::Class && Offset != (Off::FField::Class + (sizeof(void*) / 2))
				&& Offset != Off::FField::Next && Offset != (Off::FField::Next + (sizeof(void*) / 2))
				&& Offset != Off::FField::Vft && Offset != (Off::FField::Vft + (sizeof(void*) / 2))
				&& Offset != Off::FField::Name && Offset != (Off::FField::Name + Off::InSDK::Name::FNameSize);
		};

	int32 StartingOffset = 0x8;

	// Only pay attention to the 0x8 aligned size-options of FName, since the pair in the TMap is 0x8 aligned because of FString
	struct alignas(0x4) Name08Byte { uint8 Pad[0x08]; };
	struct alignas(0x4) Name16Byte { uint8 Pad[0x10]; };

	// External-mode probe: instead of dereferencing a UC::TMap*, we read the TMap's underlying
	// TSparseArray<SetElement<TPair<Name, FString>>> via RemoteContainers. If the resulting
	// pair list is plausible (non-empty, bounded, first FString payload is readable), the
	// candidate offset is accepted.
	auto IsPlausibleMetadataMap = [](uintptr_t mapRemoteAddr, size_t keySize) -> bool
	{
		if (mapRemoteAddr == 0)
			return false;

		const auto pairs = RemoteContainers::ReadNameFStringMap(mapRemoteAddr, keySize);
		if (pairs.empty() || pairs.size() >= 0x10)
			return false;

		return pairs.front().Value.IsValid();
	};

	while (true)
	{
		if (!IsPotentiallyValidOffset(StartingOffset))
		{
			StartingOffset += sizeof(void*);
			continue;
		}

		const int32 Offset = GetValidPointerOffset<false>(GuidChild1.GetAddress(), GuidChild2.GetAddress(), StartingOffset, 0x40);
		StartingOffset = Offset + sizeof(void*);

		if (Offset == OffsetNotFound)
			break;

		if (!IsPotentiallyValidOffset(Offset))
			continue;

		const uintptr_t map1 = RDeref<uintptr_t>(reinterpret_cast<uintptr_t>(GuidChild1.GetAddress()) + Offset);
		const uintptr_t map2 = RDeref<uintptr_t>(reinterpret_cast<uintptr_t>(GuidChild2.GetAddress()) + Offset);

		if (map1 == 0 || map2 == 0 || Platform::IsBadReadPtr(map1) || Platform::IsBadReadPtr(map2))
			continue;

		const size_t keySize = (Off::InSDK::Name::FNameSize <= 0x8) ? 0x8 : 0x10;
		if (IsPlausibleMetadataMap(map1, keySize) && IsPlausibleMetadataMap(map2, keySize))
			return Offset;
	}

	return OffsetNotFound;
}

int32_t OffsetFinder::FindFFieldClassOffset()
{
	const UEFField GuidChild = ObjectArray::FindStructFast("Guid").GetChildProperties();
	const UEFField VectorChild = ObjectArray::FindStructFast("Vector").GetChildProperties();

	return GetValidPointerOffset<false>(GuidChild.GetAddress(), VectorChild.GetAddress(), 0x8, 0x30, true);
}

// This function assumes that the EnumObj passed in is valid and that the values of the enum are starting at 0.
// External-mode: all reads go through RemoteMemory, and the Name/Value array walk uses
// RemoteContainers::ReadNameValueTArray so the TPair element bytes come from the target via
// a single bulk hypercall rather than per-field reinterpret_cast dereferences.
void InializeUEnumSettings(const void* EnumObj, const uint32_t UEnumNumValuesOffset)
{
	constexpr uintptr_t UE5EnumDynamicAllocationTag = 0x1;

	{
		// UE5.6+: two parallel arrays — FName[] and int64[] — tagged pointer right before NumValues.
		const uintptr_t PossibleValueArrayTaggedPtr = RDeref<uintptr_t>(
			reinterpret_cast<uintptr_t>(EnumObj) + UEnumNumValuesOffset - sizeof(void*));
		const uintptr_t PossibleValueArrayPtr = PossibleValueArrayTaggedPtr & ~UE5EnumDynamicAllocationTag;

		if (PossibleValueArrayPtr != 0 && !Platform::IsBadReadPtr(PossibleValueArrayPtr))
		{
			const int64 v0 = RDeref<int64>(PossibleValueArrayPtr);
			const int64 v1 = RDeref<int64>(PossibleValueArrayPtr + sizeof(int64));
			const int64 v2 = RDeref<int64>(PossibleValueArrayPtr + 2 * sizeof(int64));

			if (v0 == 0 && v1 == 1 && v2 == 2)
			{
				Settings::Internal::bIsNewUE5EnumNamesContainer = true;
				return;
			}
		}
	}

	const uintptr_t ArrayHeaderRemoteAddr = reinterpret_cast<uintptr_t>(EnumObj) + UEnumNumValuesOffset - 0x8;
	const size_t keySize = Settings::Internal::bUseCasePreservingName ? 0x10 : 0x8;
	const size_t valueSize = 0x8; // int64 values first pass
	const auto pairs = RemoteContainers::ReadNameValueTArray(ArrayHeaderRemoteAddr, keySize, valueSize);

	if (pairs.size() < 2)
	{
		Settings::Internal::bIsEnumNameOnly = true;
		return;
	}

	const int64 second0 = static_cast<int64>(pairs[1].ValueBytes);
	if (second0 == 1)
		return;

	if constexpr (Settings::EngineCore::bCheckEnumNamesInUEnum)
	{
		if (pairs.size() >= 3
			&& static_cast<uint8_t>(pairs[1].ValueBytes) == 1
			&& static_cast<uint8_t>(pairs[2].ValueBytes) == 2)
		{
			Settings::Internal::bIsSmallEnumValue = true;
			return;
		}
	}

	Settings::Internal::bIsEnumNameOnly = true;
}

/* FFieldClass */
int32_t OffsetFinder::FindFieldClassCastFlagsOffset()
{
	std::vector<std::pair<void*, EClassCastFlags>> Infos;

	const UEFField GuidChild = ObjectArray::FindStructFast("Guid").GetChildProperties();
	const UEFField ColourChild = ObjectArray::FindStructFast("Color").GetChildProperties();

	Infos.push_back({ GuidChild.GetClass().GetAddress(),   EClassCastFlags::Field | EClassCastFlags::Property | EClassCastFlags::NumericProperty | EClassCastFlags::IntProperty  });
	Infos.push_back({ ColourChild.GetClass().GetAddress(), EClassCastFlags::Field | EClassCastFlags::Property | EClassCastFlags::NumericProperty | EClassCastFlags::ByteProperty });

	const int32_t Offset = FindOffset(Infos, sizeof(void*), 0x30);

	return Offset != OffsetNotFound ? Offset : 0x10;
}

/* UEnum */
int32_t OffsetFinder::FindEnumNamesOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("ENetRole", EClassCastFlags::Enum).GetAddress(), 0x5 });
	Infos.push_back({ ObjectArray::FindObjectFast("ETraceTypeQuery", EClassCastFlags::Enum).GetAddress(), 0x22 });

	int UEnumNumValuesOffset = FindOffset(Infos);

	if (UEnumNumValuesOffset == OffsetNotFound)
	{
		Infos[0] = { ObjectArray::FindObjectFast("EAlphaBlendOption", EClassCastFlags::Enum).GetAddress(), 0x10 };
		Infos[1] = { ObjectArray::FindObjectFast("EUpdateRateShiftBucket", EClassCastFlags::Enum).GetAddress(), 0x8 };

		UEnumNumValuesOffset = FindOffset(Infos);
	}

	InializeUEnumSettings(Infos[0].first, UEnumNumValuesOffset);

	return UEnumNumValuesOffset - sizeof(void*);
}

/* UStruct */
int32_t OffsetFinder::FindSuperOffset()
{
	std::vector<std::pair<void*, void*>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("Struct").GetAddress(), ObjectArray::FindObjectFast("Field").GetAddress() });
	Infos.push_back({ ObjectArray::FindObjectFast("Class").GetAddress(), ObjectArray::FindObjectFast("Struct").GetAddress() });

	// Thanks to the ue4 dev who decided UStruct should be spelled Ustruct
	if (Infos[0].first == nullptr)
		Infos[0].first = Infos[1].second = ObjectArray::FindObjectFast("struct").GetAddress();

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindChildOffset()
{
	std::vector<std::pair<void*, void*>> Infos;

	if (ObjectArray::FindObject("ObjectProperty Engine.Controller.TransformComponent", EClassCastFlags::ObjectProperty))
	{
		Infos.push_back({ ObjectArray::FindObjectFast("Vector").GetAddress(), ObjectArray::FindObjectFastInOuter("X", "Vector").GetAddress() });
		Infos.push_back({ ObjectArray::FindObjectFast("Vector4").GetAddress(), ObjectArray::FindObjectFastInOuter("X", "Vector4").GetAddress() });
		Infos.push_back({ ObjectArray::FindObjectFast("Vector2D").GetAddress(), ObjectArray::FindObjectFastInOuter("X", "Vector2D").GetAddress() });
		Infos.push_back({ ObjectArray::FindObjectFast("Guid").GetAddress(), ObjectArray::FindObjectFastInOuter("A","Guid").GetAddress() });

		return FindOffset(Infos, 0x14);
	}

	Infos.push_back({ ObjectArray::FindObjectFast("PlayerController").GetAddress(), ObjectArray::FindObjectFastInOuter("WasInputKeyJustReleased", "PlayerController").GetAddress() });
	Infos.push_back({ ObjectArray::FindObjectFast("Controller").GetAddress(), ObjectArray::FindObjectFastInOuter("UnPossess", "Controller").GetAddress() });

	Settings::Internal::bUseFProperty = true;

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindChildPropertiesOffset()
{
	const void* ObjA = ObjectArray::FindStructFast("Color").GetAddress();
	const void* ObjB = ObjectArray::FindStructFast("Guid").GetAddress();

	return GetValidPointerOffset(ObjA, ObjB, Off::UStruct::Children + 0x08, 0x80);
}

int32_t OffsetFinder::FindStructSizeOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("Color").GetAddress(), 0x04 });
	Infos.push_back({ ObjectArray::FindObjectFast("Guid").GetAddress(), 0x10 });

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindMinAlignmentOffset()
{
	std::vector<std::pair<void*, int16_t>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("Transform").GetAddress(), 0x10 });

	if constexpr (Platform::Is32Bit())
	{
		Infos.push_back({ ObjectArray::FindObjectFast("InterpCurveLinearColor").GetAddress(), 0x04 });
	}
	else
	{
		Infos.push_back({ ObjectArray::FindObjectFast("PlayerController").GetAddress(), 0x8 });
	}

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindStructBaseChainOffset()
{
	// UStruct inherits from FStructBaseChain, so the members of base chain should come right after UField

	UEStruct Struct = ObjectArray::FindStructFast("Struct");
	if (!Struct)
		Struct = ObjectArray::FindStructFast("struct");

	const int32 UStructStart = Struct.GetSuper().GetStructSize();
	const int32 UStructEnd = UStructStart + Struct.GetStructSize();

	// If the members of UStruct come right after UField, FStructBaseChain either doesn't exist or is empty
	if (UStructStart == Off::UStruct::ChildProperties || UStructStart == Off::UStruct::Children)
		return OffsetNotFound;

	auto CountSuperClasses = [](const UEStruct InStruct) -> int32
	{
		int32 Count = 0;

		UEStruct CurrentSuper = InStruct.GetSuper();
		while (CurrentSuper)
		{
			Count++;
			CurrentSuper = CurrentSuper.GetSuper();
		}

		return Count;
	};

	/* Pair<UStruct, NumSuperClasses> */
	std::vector<std::pair<void*, int32_t>> Infos;

	UEStruct APlayerController = ObjectArray::FindClassFast("PlayerController");
	UEStruct AActor = ObjectArray::FindClassFast("Actor");

	Infos.push_back({ Struct.GetAddress(),              CountSuperClasses(Struct)            });
	Infos.push_back({ APlayerController.GetAddress(),   CountSuperClasses(APlayerController) });
	Infos.push_back({ AActor.GetAddress(),              CountSuperClasses(AActor)            });

	// FStructBaseChain::NumStructBasesInChainMinusOne is at offset 0x8, after a pointer
	return FindOffset(Infos, UStructStart, UStructEnd) - sizeof(void*);
}

/* UFunction */
int32_t OffsetFinder::FindFunctionFlagsOffset()
{
	std::vector<std::pair<void*, EFunctionFlags>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("WasInputKeyJustPressed", EClassCastFlags::Function).GetAddress(), EFunctionFlags::Final | EFunctionFlags::Native | EFunctionFlags::Public | EFunctionFlags::BlueprintCallable | EFunctionFlags::BlueprintPure | EFunctionFlags::Const });
	Infos.push_back({ ObjectArray::FindObjectFast("ToggleSpeaking", EClassCastFlags::Function).GetAddress(), EFunctionFlags::Exec | EFunctionFlags::Native | EFunctionFlags::Public });
	Infos.push_back({ ObjectArray::FindObjectFast("SwitchLevel", EClassCastFlags::Function).GetAddress(), EFunctionFlags::Exec | EFunctionFlags::Native | EFunctionFlags::Public });

	// Some games don't have APlayerController::SwitchLevel(), so we replace it with APlayerController::FOV() which has the same FunctionFlags
	if (Infos[2].first == nullptr)
		Infos[2].first = ObjectArray::FindObjectFast("FOV", EClassCastFlags::Function).GetAddress();

	const int32 Ret = FindOffset(Infos);

	if (Ret != OffsetNotFound)
		return Ret;

	for (auto& [_, Flags] : Infos)
		Flags |= EFunctionFlags::RequiredAPI;

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindFunctionNativeFuncOffset()
{
	std::vector<std::pair<void*, EFunctionFlags>> Infos;

	uintptr_t WasInputKeyJustPressed = reinterpret_cast<uintptr_t>(ObjectArray::FindObjectFast("WasInputKeyJustPressed", EClassCastFlags::Function).GetAddress());
	uintptr_t ToggleSpeaking = reinterpret_cast<uintptr_t>(ObjectArray::FindObjectFast("ToggleSpeaking", EClassCastFlags::Function).GetAddress());
	uintptr_t SwitchLevel_Or_FOV = reinterpret_cast<uintptr_t>(ObjectArray::FindObjectFast("SwitchLevel", EClassCastFlags::Function).GetAddress());

	// Some games don't have APlayerController::SwitchLevel(), so we replace it with APlayerController::FOV() which has the same FunctionFlags
	if (SwitchLevel_Or_FOV == NULL)
		SwitchLevel_Or_FOV = reinterpret_cast<uintptr_t>(ObjectArray::FindObjectFast("FOV", EClassCastFlags::Function).GetAddress());

	for (int i = 0x30; i < 0x140; i += sizeof(void*))
	{
		if (Platform::IsAddressInProcessRange(RDeref<uintptr_t>(WasInputKeyJustPressed + i)) &&
			Platform::IsAddressInProcessRange(RDeref<uintptr_t>(ToggleSpeaking + i)) &&
			Platform::IsAddressInProcessRange(RDeref<uintptr_t>(SwitchLevel_Or_FOV + i)))
			return i;
	}

	return 0x0;
}

/* UClass */
int32_t OffsetFinder::FindCastFlagsOffset()
{
	std::vector<std::pair<void*, EClassCastFlags>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("Actor").GetAddress(), EClassCastFlags::Actor });
	Infos.push_back({ ObjectArray::FindObjectFast("Class").GetAddress(), EClassCastFlags::Field | EClassCastFlags::Struct | EClassCastFlags::Class });

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindDefaultObjectOffset()
{
	std::vector<std::pair<void*, void*>> Infos;

	Infos.push_back({ ObjectArray::FindClassFast("Object").GetAddress(), ObjectArray::FindObjectFast("Default__Object").GetAddress() });
	Infos.push_back({ ObjectArray::FindClassFast("Field").GetAddress(), ObjectArray::FindObjectFast("Default__Field").GetAddress() });

	return FindOffset(Infos, 0x28, 0x200);
}

int32_t OffsetFinder::FindImplementedInterfacesOffset()
{
	UEClass Interface_AssetUserDataClass = ObjectArray::FindClassFast("Interface_AssetUserData");

	const uintptr_t ActorComponentClassPtr = reinterpret_cast<uintptr_t>(ObjectArray::FindClassFast("ActorComponent").GetAddress());

	// FImplementedInterface = { UClass* InterfaceClass, int32 PointerOffset, bool bImplementedByK2 }
	// aligned to 8 on x64; sizeof = 16 (padding for alignment).
	constexpr size_t kImplementedInterfaceStride = 16;

	for (int i = Off::UClass::ClassDefaultObject; i <= (0x350 - 0x10); i += sizeof(void*))
	{
		// TArray header at ActorComponentClassPtr + i: { T* Data, int32 Num, int32 Max }
		const RemoteContainers::TArrayHeader header = RemoteContainers::ReadTArrayHeader(ActorComponentClassPtr + i);
		if (!header.IsValid() || header.Num > 0x40)
			continue;

		// Read the first element's InterfaceClass pointer (offset 0 within the struct).
		const uintptr_t firstInterfaceClass = RDeref<uintptr_t>(header.Data);
		if (firstInterfaceClass == 0)
			continue;

		if (firstInterfaceClass == reinterpret_cast<uintptr_t>(Interface_AssetUserDataClass.GetAddress()))
			return i;

		// Unused stride reference to keep the intent visible in the source.
		(void)kImplementedInterfaceStride;
	}

	return OffsetNotFound;
}

/* Property */
int32_t OffsetFinder::FindElementSizeOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	UEStruct Guid = ObjectArray::FindStructFast("Guid");

	Infos.push_back({ Guid.FindMember("A").GetAddress(), 0x04 });
	Infos.push_back({ Guid.FindMember("C").GetAddress(), 0x04 });
	Infos.push_back({ Guid.FindMember("D").GetAddress(), 0x04 });

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindArrayDimOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	UEStruct Guid = ObjectArray::FindStructFast("Guid");

	Infos.push_back({ Guid.FindMember("A").GetAddress(), 0x01 });
	Infos.push_back({ Guid.FindMember("C").GetAddress(), 0x01 });
	Infos.push_back({ Guid.FindMember("D").GetAddress(), 0x01 });

	const int32_t MinOffset = Off::Property::ElementSize - 0x10;
	const int32_t MaxOffset = Off::Property::ElementSize + 0x10;

	return FindOffset(Infos, MinOffset, MaxOffset);
}

int32_t OffsetFinder::FindPropertyFlagsOffset()
{
	std::vector<std::pair<void*, EPropertyFlags>> Infos;


	UEStruct Guid = ObjectArray::FindStructFast("Guid");
	UEStruct Color = ObjectArray::FindStructFast("Color");

	constexpr EPropertyFlags GuidMemberFlags = EPropertyFlags::Edit | EPropertyFlags::ZeroConstructor | EPropertyFlags::SaveGame | EPropertyFlags::IsPlainOldData | EPropertyFlags::NoDestructor | EPropertyFlags::HasGetValueTypeHash;
	constexpr EPropertyFlags ColorMemberFlags = EPropertyFlags::Edit | EPropertyFlags::BlueprintVisible | EPropertyFlags::ZeroConstructor | EPropertyFlags::SaveGame | EPropertyFlags::IsPlainOldData | EPropertyFlags::NoDestructor | EPropertyFlags::HasGetValueTypeHash;

	Infos.push_back({ Guid.FindMember("A").GetAddress(), GuidMemberFlags });
	Infos.push_back({ Color.FindMember("R").GetAddress(), ColorMemberFlags });

	if (Infos[1].first == nullptr) [[unlikely]]
		Infos[1].first = Color.FindMember("r").GetAddress();

	int FlagsOffset = FindOffset(Infos);

	// Same flags without AccessSpecifier
	if (FlagsOffset == OffsetNotFound)
	{
		Infos[0].second |= EPropertyFlags::NativeAccessSpecifierPublic;
		Infos[1].second |= EPropertyFlags::NativeAccessSpecifierPublic;

		FlagsOffset = FindOffset(Infos);
	}

	return FlagsOffset;
}

int32_t OffsetFinder::FindOffsetInternalOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	const UEStruct Color = ObjectArray::FindStructFast("Color");
	const UEStruct Guid = ObjectArray::FindStructFast("Guid");

	Infos.push_back({ Color.FindMember("B").GetAddress(), 0x00 });
	Infos.push_back({ Color.FindMember("G").GetAddress(), 0x01 });
	Infos.push_back({ Guid.FindMember("C").GetAddress(), 0x08 });

	// Thanks to the ue5 dev who decided FColor::R should be spelled FColor::r
	if (Infos[2].first == nullptr) [[unlikely]]
		Infos[2].first = Color.FindMember("r").GetAddress();

	return FindOffset(Infos);
}

/* BoolProperty */
int32_t OffsetFinder::FindBoolPropertyBaseOffset()
{
	std::vector<std::pair<void*, uint8_t>> Infos;

	UEClass Engine = ObjectArray::FindClassFast("Engine");
	UEClass PlayerController = ObjectArray::FindClassFast("PlayerController");

	// Dump the first few FField names from Engine so we can see whether the linked-list
	// walk is broken or whether the specific member names we're looking for have been
	// removed/renamed in this build.
	std::cerr << "[FindBoolPropertyBaseOffset] Engine properties (first 10):\n";
	int debugCount = 0;
	for (UEFField F = Engine.GetChildProperties(); F && debugCount < 10; F = F.GetNext(), ++debugCount)
	{
		std::cerr << std::format("   [{}] addr={} name='{}' class='{}'\n",
			debugCount, F.GetAddress(), F.GetName(), F.GetClass().GetCppName());
	}

	std::cerr << std::format("[FindBoolPropertyBaseOffset] Engine={} PlayerController={}\n",
		Engine.GetAddress(), PlayerController.GetAddress());

	const void* m0 = Engine.FindMember("bIsOverridingSelectedColor").GetAddress();
	const void* m1 = Engine.FindMember("bEnableOnScreenDebugMessagesDisplay").GetAddress();
	const void* m2 = PlayerController.FindMember("bAutoManageActiveCameraTarget").GetAddress();
	std::cerr << std::format("[FindBoolPropertyBaseOffset]   bIsOverridingSelectedColor={}\n   bEnableOnScreenDebugMessagesDisplay={}\n   bAutoManageActiveCameraTarget={}\n",
		m0, m1, m2);

	Infos.push_back({ const_cast<void*>(m0), 0xFF });
	Infos.push_back({ const_cast<void*>(m1), 0b00000010 });
	Infos.push_back({ const_cast<void*>(m2), 0xFF });

	const int32_t rawOffset = FindOffset<1>(Infos, Off::Property::Offset_Internal);
	std::cerr << std::format("[FindBoolPropertyBaseOffset] raw FindOffset result: 0x{:X}\n", rawOffset);
	return rawOffset == OffsetNotFound ? OffsetNotFound : (rawOffset - 0x3);
}

/* ObjectPrperty */
int32_t OffsetFinder::FindObjectPropertyClassOffset()
{
	std::vector<std::pair<void*, void*>> Infos;

	const UEClass Controller = ObjectArray::FindClassFast("Controller");
	Infos.push_back({ Controller.FindMember("PlayerState").GetAddress(), ObjectArray::FindClassFast("PlayerState").GetAddress() });
	Infos.push_back({ Controller.FindMember("Pawn").GetAddress(), ObjectArray::FindClassFast("Pawn").GetAddress() });
	Infos.push_back({ ObjectArray::FindClassFast("World").FindMember("PersistentLevel").GetAddress(), ObjectArray::FindClassFast("Level").GetAddress() });

	const int32_t raw = FindOffset(Infos, Off::Property::Offset_Internal);
	std::cerr << std::format("[FindObjectPropertyClassOffset] raw FindOffset result: 0x{:X}\n", raw);
	return raw;
}

/* EnumProperty */
int32_t OffsetFinder::FindEnumPropertyBaseOffset()
{
	std::vector<std::pair<void*, const void*>> Infos;

	const void* ComponentCreationMethod = ObjectArray::FindObjectFast("EComponentCreationMethod", EClassCastFlags::Enum).GetAddress();
	const void* AutoPossessAI = ObjectArray::FindObjectFast("EAutoPossessAI", EClassCastFlags::Enum).GetAddress();

	if (!ComponentCreationMethod || !AutoPossessAI)
		return OffsetNotFound;

	void* CreationMethodMember = ObjectArray::FindClassFast("ActorComponent").FindMember("CreationMethod", EClassCastFlags::EnumProperty).GetAddress();
	void* AutoPossessAIMember = ObjectArray::FindClassFast("Pawn").FindMember("AutoPossessAI", EClassCastFlags::EnumProperty).GetAddress();

	// UE4.15 and below don't have EnumProperty
	if (!CreationMethodMember || !AutoPossessAIMember)
		return OffsetNotFound;

	Infos.push_back({ CreationMethodMember, ComponentCreationMethod });
	Infos.push_back({ AutoPossessAIMember , AutoPossessAI });

	// EnumProperty::Enum is the 2nd member after 'NumericProperty UnderlayingType'
	return FindOffset(Infos, Off::Property::Offset_Internal) - sizeof(void*);
}

/* ByteProperty */
int32_t OffsetFinder::FindBytePropertyEnumOffset()
{
	std::vector<std::pair<void*, const void*>> Infos;

	const void* CollisionResponseEnum = ObjectArray::FindObjectFast("ECollisionResponse", EClassCastFlags::Enum).GetAddress();

	const UEStruct CollisionResponseContainer = ObjectArray::FindStructFast("CollisionResponseContainer");

	if (!CollisionResponseEnum || !CollisionResponseContainer)
		return OffsetNotFound;

	const void* GameTraceChannel1 = CollisionResponseContainer.FindMember("GameTraceChannel1", EClassCastFlags::ByteProperty).GetAddress();
	const void* GameTraceChannel2 = CollisionResponseContainer.FindMember("GameTraceChannel2", EClassCastFlags::ByteProperty).GetAddress();

	if (!GameTraceChannel1 || !GameTraceChannel2)
		return OffsetNotFound;

	Infos.push_back({ const_cast<void*>(GameTraceChannel1), CollisionResponseEnum });
	Infos.push_back({ const_cast<void*>(GameTraceChannel2), CollisionResponseEnum });

	return FindOffset(Infos, Off::Property::Offset_Internal);
}

/* StructProperty */
int32_t OffsetFinder::FindStructPropertyStructOffset()
{
	std::vector<std::pair<void*, const void*>> Infos;

	const void* VectorClass = ObjectArray::FindStructFast("Vector").GetAddress();

	if (VectorClass == nullptr)
		VectorClass = ObjectArray::FindClassFast("vector").GetAddress();

	const UEStruct TwoVectorsStruct = ObjectArray::FindStructFast("TwoVectors");

	std::cerr << std::format("[FindStructPropertyStructOffset] VectorClass={} TwoVectorsStruct={}\n",
		VectorClass, (const void*)TwoVectorsStruct.GetAddress());

	if (!VectorClass || !TwoVectorsStruct)
		return OffsetNotFound;

	const void* v1 = TwoVectorsStruct.FindMember("v1", EClassCastFlags::StructProperty).GetAddress();
	const void* v2 = TwoVectorsStruct.FindMember("v2", EClassCastFlags::StructProperty).GetAddress();

	std::cerr << std::format("[FindStructPropertyStructOffset] v1={} v2={}\n", v1, v2);

	if (!v1 || !v2)
		return OffsetNotFound;

	Infos.push_back({ const_cast<void*>(v1), VectorClass });
	Infos.push_back({ const_cast<void*>(v2), VectorClass });

	const int32_t raw = FindOffset(Infos, Off::Property::Offset_Internal);
	std::cerr << std::format("[FindStructPropertyStructOffset] raw FindOffset result: 0x{:X}\n", raw);
	return raw;
}

/* DelegateProperty */
int32_t OffsetFinder::FindDelegatePropertySignatureFunctionOffset()
{
	std::vector<std::pair<void*, const void*>> Infos;

	const void* DelegateSignature = ObjectArray::FindObjectFast("TimerDynamicDelegate__DelegateSignature", EClassCastFlags::Function).GetAddress();

	const UEStruct TwoVectorsStruct = ObjectArray::FindStructFast("TwoVectors");

	if (!DelegateSignature || !TwoVectorsStruct)
		return OffsetNotFound;

	const void* Delegate1 = ObjectArray::FindObjectFast<UEFunction>("K2_GetTimerElapsedTimeDelegate", EClassCastFlags::Function).FindMember("Delegate", EClassCastFlags::DelegateProperty).GetAddress();
	const void* Delegate2 = ObjectArray::FindObjectFast<UEFunction>("K2_GetTimerRemainingTimeDelegate", EClassCastFlags::Function).FindMember("Delegate", EClassCastFlags::DelegateProperty).GetAddress();

	if (!Delegate1 || !Delegate2)
		return OffsetNotFound;

	Infos.push_back({ const_cast<void*>(Delegate1), DelegateSignature });
	Infos.push_back({ const_cast<void*>(Delegate2), DelegateSignature });

	return FindOffset(Infos, Off::Property::Offset_Internal);
}

/* ArrayProperty */
int32_t OffsetFinder::FindInnerTypeOffset(const int32 PropertySize)
{
	if (!Settings::Internal::bUseFProperty)
		return PropertySize;

	if (const UEProperty Property = ObjectArray::FindClassFast("GameViewportClient").FindMember("DebugProperties", EClassCastFlags::ArrayProperty))
	{
		const uintptr_t AddressToCheck = RDeref<uintptr_t>(reinterpret_cast<const uint8*>(Property.GetAddress()) + PropertySize);

		if (Platform::IsBadReadPtr(AddressToCheck))
			return PropertySize + sizeof(void*);
	}

	return PropertySize;
}

/* SetProperty */
int32_t OffsetFinder::FindSetPropertyBaseOffset(const int32 PropertySize)
{
	if (!Settings::Internal::bUseFProperty)
		return PropertySize;

	if (const auto Object = ObjectArray::FindStructFast("LevelCollection").FindMember("Levels", EClassCastFlags::SetProperty))
	{
		const uintptr_t AddressToCheck = RDeref<uintptr_t>(reinterpret_cast<const uint8*>(Object.GetAddress()) + PropertySize);

		if (Platform::IsBadReadPtr(AddressToCheck))
			return PropertySize + sizeof(void*);
	}

	return PropertySize;
}


/* MapProperty */
int32_t OffsetFinder::FindMapPropertyBaseOffset(const int32 PropertySize)
{
	if (!Settings::Internal::bUseFProperty)
		return PropertySize;

	if (const auto Object = ObjectArray::FindClassFast("UserDefinedEnum").FindMember("DisplayNameMap", EClassCastFlags::MapProperty))
	{
		const uintptr_t AddressToCheck = RDeref<uintptr_t>(reinterpret_cast<const uint8*>(Object.GetAddress()) + PropertySize);

		if (Platform::IsBadReadPtr(AddressToCheck))
			return PropertySize + sizeof(void*);
	}

	return PropertySize;
}

/* InSDK -> ULevel */
int32_t OffsetFinder::FindLevelActorsOffset()
{
	UEObject Level = nullptr;
	uintptr_t Lvl = 0x0;

	for (auto Obj : ObjectArray())
	{
		if (Obj.HasAnyFlags(EObjectFlags::ClassDefaultObject) || !Obj.IsA(EClassCastFlags::Level))
			continue;

		Level = Obj;
		Lvl = reinterpret_cast<uintptr_t>(Obj.GetAddress());
		break;
	}

	if (Lvl == 0x0)
		return OffsetNotFound;

	/*
	class ULevel : public UObject
	{
		FURL URL;
		TArray<AActor*> Actors;
		TArray<AActor*> GCActors;
	};

	SearchStart = sizeof(UObject) + sizeof(FURL)
	SearchEnd = offsetof(ULevel, OwningWorld)
	*/
	UEClass UObjectClass = ObjectArray::FindClassFast("Object");
	if (!UObjectClass)
		UObjectClass = ObjectArray::FindClassFast("object");

	const UEStruct FURLStruct = ObjectArray::FindObjectFast<UEStruct>("URL", EClassCastFlags::Struct);

	const UEProperty Level_OwningWorldProperty = Level.GetClass().FindMember("OwningWorld");

	if (!UObjectClass || !FURLStruct || !Level_OwningWorldProperty)
		return OffsetNotFound;

	const int32 SearchStart = UObjectClass.GetStructSize() + FURLStruct.GetStructSize();
	const int32 SearchEnd = Level_OwningWorldProperty.GetOffset();

	for (int i = SearchStart; i <= (SearchEnd - 0x10); i += sizeof(void*))
	{
		// Read the TArray header { Data*, Num, Max } from the target and do a loose validation.
		const RemoteContainers::TArrayHeader header = RemoteContainers::ReadTArrayHeader(Lvl + i);
		if (header.IsValid() && !Platform::IsBadReadPtr(header.Data))
		{
			return i;
		}
	}

	return OffsetNotFound;
}


/* InSDK -> UDataTable */
int32_t OffsetFinder::FindDatatableRowMapOffset()
{
	const UEClass DataTable = ObjectArray::FindClassFast("DataTable");

	constexpr int32 UObjectOuterSize = sizeof(void*);
	constexpr int32 RowStructSize = sizeof(void*);

	if (!DataTable)
	{
		std::cerr << "\nDumper-7: [DataTable] Couldn't find \"DataTable\" class, assuming default layout.\n" << std::endl;
		return (Off::UObject::Outer + UObjectOuterSize + RowStructSize);
	}

	UEProperty RowStructProp = DataTable.FindMember("RowStruct", EClassCastFlags::ObjectProperty);

	if (!RowStructProp)
	{
		std::cerr << "\nDumper-7: [DataTable] Couldn't find \"RowStruct\" property, assuming default layout.\n" << std::endl;
		return (Off::UObject::Outer + UObjectOuterSize + RowStructSize);
	}

	return RowStructProp.GetOffset() + RowStructProp.GetSize();
}

