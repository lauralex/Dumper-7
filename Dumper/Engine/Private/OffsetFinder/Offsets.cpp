#include <format>

#include "Utils.h"

#include "OffsetFinder/Offsets.h"
#include "OffsetFinder/OffsetFinder.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"

#include "Platform.h"
#include "Architecture.h"
#include "RemoteMemory.h"


void Off::InSDK::ProcessEvent::InitPE_Windows()
{
#ifdef PLATFORM_WINDOWS

	// Read the vtable pointer from object 0 via hypercall; then cast to void** so
	// IterateVTableFunctions can treat it as a remote VA of the vtable table.
	const uintptr_t Obj0Addr = reinterpret_cast<uintptr_t>(ObjectArray::GetByIndex(0).GetAddress());
	void** Vft = reinterpret_cast<void**>(RDeref<uintptr_t>(Obj0Addr));

#if defined(_WIN64)
	/* Primary, and more reliable, check for ProcessEvent */
	auto IsProcessEvent = [](const uint8_t* FuncAddress, [[maybe_unused]] int32_t Index) -> bool
	{
		return Platform::FindPatternInRange({ 0xF7, -0x1, Off::UFunction::FunctionFlags, 0x0, 0x0, 0x0, 0x0, 0x04, 0x0, 0x0 }, FuncAddress, 0x400)
			&& Platform::FindPatternInRange({ 0xF7, -0x1, Off::UFunction::FunctionFlags, 0x0, 0x0, 0x0, 0x0, 0x0, 0x40, 0x0 }, FuncAddress, 0xF00);
	};
#elif defined(_WIN32)
	/* Primary, and more reliable, check for ProcessEvent */
	auto IsProcessEvent = [](const uint8_t* FuncAddress, [[maybe_unused]] int32_t Index) -> bool
	{
		return Platform::FindPatternInRange({ 0xF7, -0x1, Off::UFunction::FunctionFlags, 0x0, 0x4, 0x0, 0x0 }, FuncAddress, 0x400)
			&& Platform::FindPatternInRange({ 0xF7, -0x1, Off::UFunction::FunctionFlags, 0x0, 0x0, 0x40, 0x0 }, FuncAddress, 0xF00);
	};
#endif

	const void* ProcessEventAddr = nullptr;
	int32_t ProcessEventIdx = 0;

	const auto [FuncPtr, FuncIdx] = Platform::IterateVTableFunctions(Vft, IsProcessEvent);

	ProcessEventAddr = FuncPtr;
	ProcessEventIdx = FuncIdx;

	if (!FuncPtr)
	{
		const void* StringRefAddr = Platform::FindByStringInAllSections(L"Accessed None", 0x0, 0x0, Settings::General::bSearchOnlyExecutableSectionsForStrings);
		/* ProcessEvent is sometimes located right after a func with the string L"Accessed None. Might as well check for it, because else we're going to crash anyways. */
		const void* PossiblePEAddr = reinterpret_cast<void*>(Architecture_x86_64::FindNextFunctionStart(StringRefAddr));

		auto IsSameAddr = [PossiblePEAddr](const uint8_t* FuncAddress, [[maybe_unused]] int32_t Index) -> bool
		{
			return FuncAddress == PossiblePEAddr;
		};

		const auto [FuncPtr2, FuncIdx2] = Platform::IterateVTableFunctions(Vft, IsSameAddr);
		ProcessEventAddr = FuncPtr2;
		ProcessEventIdx = FuncIdx2;
	}

	if (ProcessEventAddr)
	{
		Off::InSDK::ProcessEvent::PEIndex = ProcessEventIdx;
		Off::InSDK::ProcessEvent::PEOffset = Platform::GetOffset(ProcessEventAddr);

		std::cerr << std::format("PE-Offset: 0x{:X}\n", Off::InSDK::ProcessEvent::PEOffset);
		std::cerr << std::format("PE-Index: 0x{:X}\n\n", ProcessEventIdx);
		return;
	}

	std::cerr << "\nCouldn't find ProcessEvent!\n\n" << std::endl;

#endif // PLATFORM_WINDOWS
}

void Off::InSDK::ProcessEvent::InitPE(const int32 Index, const char* const ModuleName)
{
	Off::InSDK::ProcessEvent::PEIndex = Index;

	// Read the vtable pointer of object 0, then read the Index-th function pointer from it.
	// Both reads go through RemoteMemory in external mode.
	const uintptr_t objectAddr = reinterpret_cast<uintptr_t>(ObjectArray::GetByIndex(0).GetAddress());
	const uintptr_t vftAddr = RDeref<uintptr_t>(objectAddr);
	const uintptr_t peFuncAddr = RDeref<uintptr_t>(vftAddr + Off::InSDK::ProcessEvent::PEIndex * sizeof(void*));

	Off::InSDK::ProcessEvent::PEOffset = Platform::GetOffset(peFuncAddr, ModuleName);

	std::cerr << std::format("PE-Offset: 0x{:X}\n", Off::InSDK::ProcessEvent::PEOffset);
}

/* UWorld */
void Off::InSDK::World::InitGWorld()
{
	UEClass UWorld = ObjectArray::FindClassFast("World");

	for (UEObject Obj : ObjectArray())
	{
		if (Obj.HasAnyFlags(EObjectFlags::ClassDefaultObject) || !Obj.IsA(UWorld))
			continue;

		/* Try to find a pointer to the word, aka UWorld** GWorld */
		auto Results = Platform::FindAllAlignedValuesInProcess(Obj.GetAddress());

		void* Result = nullptr;
		if (Results.size())
		{
			if (Results.size() == 1)
			{
				Result = Results[0];
			}
			else if (Results.size() == 2)
			{
				const uintptr_t ObjAddress = reinterpret_cast<uintptr_t>(Obj.GetAddress());
				const uintptr_t PossibleGWorld = reinterpret_cast<uintptr_t>(Results[0]);
				uintptr_t CurrentValue = RDeref<uintptr_t>(PossibleGWorld);

				for (int i = 0; CurrentValue == ObjAddress && i < 50; ++i)
				{
					::Sleep(1);
					CurrentValue = RDeref<uintptr_t>(PossibleGWorld);
				}
				if (CurrentValue == ObjAddress)
				{
					Result = Results[0];
				}
				else
				{
					Result = Results[1];
					std::cerr << std::format("Filter GActiveLogWorld at 0x{:X}\n\n", PossibleGWorld);
				}
			}
			else
			{
				std::cerr << std::format("Detected {} GWorld \n\n", Results.size());
			}
		}

		/* Pointer to UWorld* couldn't be found */
		if (Result)
		{
			Off::InSDK::World::GWorld = Platform::GetOffset(Result);
			std::cerr << std::format("GWorld-Offset: 0x{:X}\n\n", Off::InSDK::World::GWorld);
			break;
		}
	}

	if (Off::InSDK::World::GWorld == 0x0)
		std::cerr << std::format("\nGWorld WAS NOT FOUND!!!!!!!!!\n\n");
}

/* FText */
void Off::InSDK::Text::InitTextOffsets()
{
	// The injected-mode path called Conv_StringToText via ProcessEvent and then looked at
	// where the resulting FText stored the string. External mode cannot call into the target,
	// so we use a hardcoded layout that matches UE4.27 / UE5.0+ FText: the FTextData* lives at
	// the top of the FText, and the wide-char buffer is a subobject of FTextData whose
	// FString::Data pointer sits at offset 0x30 from the start of FTextData.
	//
	// Games on exotic UE versions (Fortnite and a few UE5 forks have a different FText shape)
	// will need a manual override via Settings::EngineCore::FTextLayoutOverride. The plan's
	// v1 fallback is a disassembly-based auto-discovery of Conv_StringToText's body; that is
	// deferred until parity-diff runs show whether the hardcoded defaults are accurate enough.

	const UEFunction Conv_StringToText = ObjectArray::FindObjectFast<UEFunction>("Conv_StringToText", EClassCastFlags::Function);
	if (Conv_StringToText)
	{
		UEProperty ReturnProp = nullptr;
		for (UEProperty Prop : Conv_StringToText.GetProperties())
		{
			if (Prop.HasPropertyFlags(EPropertyFlags::ReturnParm))
			{
				ReturnProp = Prop;
				break;
			}
		}
		if (ReturnProp)
			Off::InSDK::Text::TextSize = ReturnProp.GetSize();
	}

	if (Off::InSDK::Text::TextSize == 0)
		Off::InSDK::Text::TextSize = 0x18; // Standard FText size on UE4.27 / UE5

	Off::InSDK::Text::TextDatOffset = 0x0;
	Off::InSDK::Text::InTextDataStringOffset = 0x30;

	std::cerr << std::format("Off::InSDK::Text::TextSize: 0x{:X}\n", Off::InSDK::Text::TextSize);
	std::cerr << std::format("Off::InSDK::Text::TextDatOffset: 0x{:X} (hardcoded UE4.27/UE5 default)\n", Off::InSDK::Text::TextDatOffset);
	std::cerr << std::format("Off::InSDK::Text::InTextDataStringOffset: 0x{:X} (hardcoded UE4.27/UE5 default)\n\n", Off::InSDK::Text::InTextDataStringOffset);
}

void Off::Init()
{
	auto OverwriteIfInvalidOffset = [](int32& Offset, int32 DefaultValue)
	{
		if (Offset == OffsetFinder::OffsetNotFound)
		{
			std::cerr << std::format("Defaulting to offset: 0x{:X}\n", DefaultValue);
			Offset = DefaultValue;
		}
	};

	Off::UObject::Flags = OffsetFinder::FindUObjectFlagsOffset();
	OverwriteIfInvalidOffset(Off::UObject::Flags, sizeof(void*)); // Default to right after VTable
	std::cerr << std::format("Off::UObject::Flags: 0x{:X}\n", Off::UObject::Flags);

	Off::UObject::Index = OffsetFinder::FindUObjectIndexOffset();
	OverwriteIfInvalidOffset(Off::UObject::Index, (Off::UObject::Flags + sizeof(int32))); // Default to right after Flags
	std::cerr << std::format("Off::UObject::Index: 0x{:X}\n", Off::UObject::Index);

	Off::UObject::Class = OffsetFinder::FindUObjectClassOffset();
	OverwriteIfInvalidOffset(Off::UObject::Class, (Off::UObject::Index + sizeof(int32))); // Default to right after Index
	std::cerr << std::format("Off::UObject::Class: 0x{:X}\n", Off::UObject::Class);

	// Find Name before Outer so the statistical FName scan isn't blocked by an
	// (not yet known) Outer exclusion, and so the Outer scan can start after Name.
	Off::UObject::Name = OffsetFinder::FindUObjectNameOffset();
	OverwriteIfInvalidOffset(Off::UObject::Name, (Off::UObject::Class + sizeof(void*))); // Default to right after Class
	std::cerr << std::format("Off::UObject::Name: 0x{:X}\n", Off::UObject::Name);

	Off::UObject::Outer = OffsetFinder::FindUObjectOuterOffset();
	OverwriteIfInvalidOffset(Off::UObject::Outer, (Off::UObject::Name + sizeof(int32) + sizeof(int32)));  // Default to right after Name
	std::cerr << std::format("Off::UObject::Outer: 0x{:X}\n\n", Off::UObject::Outer);

	OffsetFinder::InitFNameSettings();

	::NameArray::PostInit();

	// UE5.1+ default struct-layout offsets used as fallbacks when discovery fails because the
	// target has aggressively stripped reflection (e.g. some UE5 shipping builds lack
	// UStruct/UField/PlayerController/Controller as registered UClass objects). Without these
	// defaults the -1 OffsetNotFound values poison the rest of the pipeline and the dumper
	// crashes before the output folder is created.
	//
	// Layout notes:
	//   UE4 / pre-FStructBaseChain: UStruct members start right after UField::Next, so
	//     Children = 0x38, ChildProperties = 0x40, PropertiesSize = 0x48 on x64.
	//   UE5.1+ with FStructBaseChain inherited by UStruct: adds 16 bytes between UField and
	//     UStruct's own members, so everything shifts by 0x10:
	//     Children = 0x48, ChildProperties = 0x50, PropertiesSize = 0x58.
	//
	// We pick the fallback AFTER Size (PropertiesSize) has been discovered, since that
	// position is easy to find reliably (FindStructSizeOffset matches Color/Guid sizes) and
	// the rest of the UStruct members sit at fixed offsets relative to it.
	constexpr int32 DefaultUFieldNext = 0x28;

	// Castflags needs to stay here since the FindChildOffset() uses CastFlags
	Off::UClass::CastFlags = OffsetFinder::FindCastFlagsOffset();
	OverwriteIfInvalidOffset(Off::UClass::CastFlags, 0xD8); // UE5.1 default
	std::cerr << std::format("Off::UClass::CastFlags: 0x{:X}\n", Off::UClass::CastFlags);

	Off::UField::Next = OffsetFinder::FindUFieldNextOffset();
	OverwriteIfInvalidOffset(Off::UField::Next, DefaultUFieldNext);
	std::cerr << std::format("Off::UField::Next: 0x{:X}\n", Off::UField::Next);

	// Discover Size FIRST so SuperStruct / Children / ChildProperties defaults can be derived
	// from it consistently. On UE5.1+ targets with FStructBaseChain the layout is:
	//   Next (0x28) | StructBaseChain.Array (0x30) | NumMinusOne+pad (0x38) |
	//   SuperStruct (0x40) | Children (0x48) | ChildProperties (0x50) | Size (0x58).
	// On pre-FStructBaseChain (UE4) it's Next (0x28) | Super (0x30) | Children (0x38) |
	//   ChildProperties (0x40) | Size (0x48). Deriving from Size picks the right layout
	//   automatically because Size = Children + 16 in both, and Super = Children - 8.
	Off::UStruct::Size = OffsetFinder::FindStructSizeOffset();
	OverwriteIfInvalidOffset(Off::UStruct::Size, 0x58); // UE5.1+ default
	std::cerr << std::format("Off::UStruct::Size: 0x{:X}\n", Off::UStruct::Size);

	// Derive Children / ChildProperties / Super defaults from discovered Size. PropertiesSize
	// sits right after ChildProperties (8 bytes) which sits right after Children (8 bytes) which
	// sits right after SuperStruct (8 bytes).
	const int32 DefaultUStructChildProperties = Off::UStruct::Size - static_cast<int32>(sizeof(void*));
	const int32 DefaultUStructChildren = DefaultUStructChildProperties - static_cast<int32>(sizeof(void*));
	const int32 DefaultUStructSuper = DefaultUStructChildren - static_cast<int32>(sizeof(void*));

	Off::UStruct::SuperStruct = OffsetFinder::FindSuperOffset();
	OverwriteIfInvalidOffset(Off::UStruct::SuperStruct, DefaultUStructSuper);
	std::cerr << std::format("Off::UStruct::SuperStruct: 0x{:X}\n", Off::UStruct::SuperStruct);

	Off::UStruct::MinAlignment = OffsetFinder::FindMinAlignmentOffset();
	OverwriteIfInvalidOffset(Off::UStruct::MinAlignment, Off::UStruct::Size + sizeof(int32));
	std::cerr << std::format("Off::UStruct::MinAlignment: 0x{:X}\n", Off::UStruct::MinAlignment);

	Off::UStruct::Children = OffsetFinder::FindChildOffset();
	OverwriteIfInvalidOffset(Off::UStruct::Children, DefaultUStructChildren);
	std::cerr << std::format("Off::UStruct::Children: 0x{:X}\n", Off::UStruct::Children);

	// Re-run CastFlags now that Children/Super offsets exist so the Cast<UEClass> paths work.
	Off::UClass::CastFlags = OffsetFinder::FindCastFlagsOffset();
	OverwriteIfInvalidOffset(Off::UClass::CastFlags, 0xD8);
	std::cerr << std::format("Off::UClass::CastFlags: 0x{:X}\n", Off::UClass::CastFlags);

	// Castflags become available for use

	if (Settings::Internal::bUseFProperty)
	{
		std::cerr << std::format("\nGame uses FProperty system\n\n");

		Off::UStruct::ChildProperties = OffsetFinder::FindChildPropertiesOffset();
		OverwriteIfInvalidOffset(Off::UStruct::ChildProperties, DefaultUStructChildProperties);
		std::cerr << std::format("Off::UStruct::ChildProperties: 0x{:X}\n", Off::UStruct::ChildProperties);

		OffsetFinder::FixupHardcodedOffsets(); // must be called after FindChildPropertiesOffset

		// FixupHardcodedOffsets may have shifted the FField layout by -0x08 to account for UE5.1.1+
		// FFieldVariant shrinkage. Capture the post-fixup values BEFORE the finders run so we can
		// fall back to them if the finders return OffsetNotFound (e.g. on external-mode targets
		// where the FField heap is paged out and GetValidPointerOffset probes read zeros).
		// Without this, OverwriteIfInvalidOffset would stomp back to the hardcoded pre-shrinkage
		// default and produce the wrong layout on legitimately shrunk targets.
		const int32 FFieldNextFallback  = Off::FField::Next;
		const int32 FFieldClassFallback = Off::FField::Class;
		const int32 FFieldNameFallback  = Off::FField::Name;

		Off::FField::Next = OffsetFinder::FindFFieldNextOffset();
		OverwriteIfInvalidOffset(Off::FField::Next, FFieldNextFallback);
		std::cerr << std::format("Off::FField::Next: 0x{:X}\n", Off::FField::Next);

		Off::FField::Class = OffsetFinder::FindFFieldClassOffset();
		OverwriteIfInvalidOffset(Off::FField::Class, FFieldClassFallback);
		std::cerr << std::format("Off::FField::Class: 0x{:X}\n", Off::FField::Class);

		// Comment out this line if you're crashing here and see if the NewFindFFieldNameOffset might work!
		Off::FField::Name = OffsetFinder::FindFFieldNameOffset();
		//Off::FField::Name = OffsetFinder::NewFindFFieldNameOffset();

		if (Off::FField::Name == OffsetFinder::OffsetNotFound)
			Off::FField::Name = OffsetFinder::NewFindFFieldNameOffset();

		OverwriteIfInvalidOffset(Off::FField::Name, FFieldNameFallback);
		std::cerr << std::format("Off::FField::Name: 0x{:X}\n", Off::FField::Name);

		/*
		* FNameSize might be wrong at this point of execution.
		* FField::Flags is not critical so a fix is only applied later in OffsetFinder::PostInitFNameSettings().
		*/
		Off::FField::Flags = Off::FField::Name + Off::InSDK::Name::FNameSize;
		std::cerr << std::format("Off::FField::Flags: 0x{:X}\n", Off::FField::Flags);

		Off::FField::EditorOnlyMetadata = OffsetFinder::FindFFieldEditorOnlyMetaDataOffset();
		if (Off::FField::EditorOnlyMetadata != OffsetFinder::OffsetNotFound)
			std::cerr << std::format("Off::FField::EditorOnlyMetadata: 0x{:X}\n", Off::FField::EditorOnlyMetadata);

		Off::FFieldClass::CastFlags = OffsetFinder::FindFieldClassCastFlagsOffset();
		std::cerr << std::format("Off::FFieldClass::CastFlags: 0x{:X}\n\n", Off::FFieldClass::CastFlags);
	}

	Off::UStruct::StructBaseChain = OffsetFinder::FindStructBaseChainOffset();
	if (Off::UStruct::StructBaseChain != OffsetFinder::OffsetNotFound)
		std::cerr << std::format("Off::UStruct::StructBaseChain: 0x{:X}\n", Off::UStruct::StructBaseChain);

	Off::UClass::ClassDefaultObject = OffsetFinder::FindDefaultObjectOffset();
	std::cerr << std::format("Off::UClass::ClassDefaultObject: 0x{:X}\n", Off::UClass::ClassDefaultObject);

	Off::UClass::ImplementedInterfaces = OffsetFinder::FindImplementedInterfacesOffset();
	std::cerr << std::format("Off::UClass::ImplementedInterfaces: 0x{:X}\n", Off::UClass::ImplementedInterfaces);

	Off::UEnum::Names = OffsetFinder::FindEnumNamesOffset();
	std::cerr << std::format("Off::UEnum::Names: 0x{:X}\n", Off::UEnum::Names) << std::endl;

	Off::UFunction::FunctionFlags = OffsetFinder::FindFunctionFlagsOffset();
	OverwriteIfInvalidOffset(Off::UFunction::FunctionFlags, Off::UStruct::Size + 0x10); // UE5.1+ default (right after UStruct)
	std::cerr << std::format("Off::UFunction::FunctionFlags: 0x{:X}\n", Off::UFunction::FunctionFlags);

	Off::UFunction::ExecFunction = OffsetFinder::FindFunctionNativeFuncOffset();
	std::cerr << std::format("Off::UFunction::ExecFunction: 0x{:X}\n", Off::UFunction::ExecFunction) << std::endl;

	// Derive FProperty sub-offset defaults from the FField size. For UE5.1.1+ (FFieldVariant = void*)
	// FField ends at 0x30; for pre-5.1.1 (FFieldVariant = { void*, bool }) it ends at 0x38.
	// FProperty members start there: ArrayDim:int32, ElementSize:int32, PropertyFlags:uint64,
	// RepIndex:uint16, BlueprintReplicationCondition:uint8 (+pad), RepNotifyFunc:FName, Offset_Internal:int32.
	const int32 FFieldSize = Off::FField::Flags + static_cast<int32>(sizeof(int32));
	const int32 DefaultArrayDim = FFieldSize;                                        // +0
	const int32 DefaultElementSize = DefaultArrayDim + static_cast<int32>(sizeof(int32));       // +4
	const int32 DefaultPropertyFlags = DefaultElementSize + static_cast<int32>(sizeof(int32));  // +8
	// After PropertyFlags (uint64) comes RepIndex(uint16) + BlueprintReplicationCondition(uint8)
	// + padding (5B), then RepNotifyFunc (FName, FNameSize bytes), then Offset_Internal (int32).
	const int32 DefaultOffsetInternal = DefaultPropertyFlags + static_cast<int32>(sizeof(uint64))
		+ 0x8 /* RepIndex+Cond+pad */ + Off::InSDK::Name::FNameSize;

	Off::Property::ElementSize = OffsetFinder::FindElementSizeOffset();
	OverwriteIfInvalidOffset(Off::Property::ElementSize, DefaultElementSize);
	std::cerr << std::format("Off::Property::ElementSize: 0x{:X}\n", Off::Property::ElementSize);

	Off::Property::ArrayDim = OffsetFinder::FindArrayDimOffset();
	OverwriteIfInvalidOffset(Off::Property::ArrayDim, DefaultArrayDim);
	std::cerr << std::format("Off::Property::ArrayDim: 0x{:X}\n", Off::Property::ArrayDim);

	Off::Property::Offset_Internal = OffsetFinder::FindOffsetInternalOffset();
	OverwriteIfInvalidOffset(Off::Property::Offset_Internal, DefaultOffsetInternal);
	std::cerr << std::format("Off::Property::Offset_Internal: 0x{:X}\n", Off::Property::Offset_Internal);

	Off::Property::PropertyFlags = OffsetFinder::FindPropertyFlagsOffset();
	OverwriteIfInvalidOffset(Off::Property::PropertyFlags, DefaultPropertyFlags);
	std::cerr << std::format("Off::Property::PropertyFlags: 0x{:X}\n", Off::Property::PropertyFlags);

	Off::BoolProperty::Base = OffsetFinder::FindBoolPropertyBaseOffset();
	// BoolProperty::Base sits right after Offset_Internal + a PropertyLink/PropertyLinkNext chain
	// (6 pointers = 0x30 bytes) on most UE5.1 builds. Use that as a last-resort default.
	const int32 DefaultBoolPropertyBase = Off::Property::Offset_Internal + static_cast<int32>(sizeof(int32)) + 0x30;
	OverwriteIfInvalidOffset(Off::BoolProperty::Base, DefaultBoolPropertyBase);
	std::cerr << std::format("UBoolProperty::Base: 0x{:X}\n", Off::BoolProperty::Base) << std::endl;

	Off::EnumProperty::Base = OffsetFinder::FindEnumPropertyBaseOffset();
	std::cerr << std::format("Off::EnumProperty::Base: 0x{:X}\n", Off::EnumProperty::Base) << std::endl;


	if (Off::EnumProperty::Base == OffsetFinder::OffsetNotFound)
	{
		Off::InSDK::Properties::PropertySize = Off::BoolProperty::Base;
		Off::EnumProperty::Base = Off::BoolProperty::Base;
	}
	else
	{
		Off::InSDK::Properties::PropertySize = Off::EnumProperty::Base;
	}

	std::cerr << std::format("UPropertySize: 0x{:X}\n", Off::InSDK::Properties::PropertySize) << std::endl;

	Off::ObjectProperty::PropertyClass = OffsetFinder::FindObjectPropertyClassOffset();
	std::cerr << std::format("Off::ObjectProperty::PropertyClass: 0x{:X}", Off::ObjectProperty::PropertyClass) << std::endl;
	OverwriteIfInvalidOffset(Off::ObjectProperty::PropertyClass, Off::InSDK::Properties::PropertySize);

	Off::ByteProperty::Enum = OffsetFinder::FindBytePropertyEnumOffset();
	OverwriteIfInvalidOffset(Off::ByteProperty::Enum, Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::ByteProperty::Enum: 0x{:X}", Off::ByteProperty::Enum) << std::endl;

	Off::StructProperty::Struct = OffsetFinder::FindStructPropertyStructOffset();
	OverwriteIfInvalidOffset(Off::StructProperty::Struct, Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::StructProperty::Struct: 0x{:X}\n", Off::StructProperty::Struct) << std::endl;

	Off::DelegateProperty::SignatureFunction = OffsetFinder::FindDelegatePropertySignatureFunctionOffset();
	OverwriteIfInvalidOffset(Off::DelegateProperty::SignatureFunction, Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::DelegateProperty::SignatureFunction: 0x{:X}\n", Off::DelegateProperty::SignatureFunction) << std::endl;

	Off::ArrayProperty::Inner = OffsetFinder::FindInnerTypeOffset(Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::ArrayProperty::Inner: 0x{:X}\n", Off::ArrayProperty::Inner);

	Off::SetProperty::ElementProp = OffsetFinder::FindSetPropertyBaseOffset(Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::SetProperty::ElementProp: 0x{:X}\n", Off::SetProperty::ElementProp);

	Off::MapProperty::Base = OffsetFinder::FindMapPropertyBaseOffset(Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::MapProperty::Base: 0x{:X}\n", Off::MapProperty::Base) << std::endl;

	Off::InSDK::ULevel::Actors = OffsetFinder::FindLevelActorsOffset();
	std::cerr << std::format("Off::InSDK::ULevel::Actors: 0x{:X}\n", Off::InSDK::ULevel::Actors) << std::endl;

	Off::InSDK::UDataTable::RowMap = OffsetFinder::FindDatatableRowMapOffset();
	std::cerr << std::format("Off::InSDK::UDataTable::RowMap: 0x{:X}\n", Off::InSDK::UDataTable::RowMap) << std::endl;

	OffsetFinder::PostInitFNameSettings();

	std::cerr << std::endl;

	Off::FieldPathProperty::FieldClass = Off::InSDK::Properties::PropertySize;
	Off::OptionalProperty::ValueProperty = Off::InSDK::Properties::PropertySize;

	Off::ClassProperty::MetaClass = Off::ObjectProperty::PropertyClass + sizeof(void*); //0x8 inheritance from ObjectProperty
}

void PropertySizes::Init()
{
	InitTDelegateSize();
	InitFFieldPathSize();
	InitTMulticastInlineDelegateSize();
}

void PropertySizes::InitTDelegateSize()
{
	/* If the AudioComponent class or the OnQueueSubtitles member weren't found, fallback to looping GObjects and looking for a Delegate. */
	auto OnPropertyNotFound = [&]() -> void
	{
		for (UEObject Obj : ObjectArray())
		{
			if (!Obj.IsA(EClassCastFlags::Struct))
				continue;

			for (UEProperty Prop : Obj.Cast<UEClass>().GetProperties())
			{
				if (Prop.IsA(EClassCastFlags::DelegateProperty))
				{
					PropertySizes::DelegateProperty = Prop.GetSize();
					return;
				}
			}
		}
	};

	const UEClass AudioComponentClass = ObjectArray::FindClassFast("AudioComponent");

	if (!AudioComponentClass)
		return OnPropertyNotFound();

	const UEProperty OnQueueSubtitlesProp = AudioComponentClass.FindMember("OnQueueSubtitles", EClassCastFlags::DelegateProperty);

	if (!OnQueueSubtitlesProp)
		return OnPropertyNotFound();

	PropertySizes::DelegateProperty = OnQueueSubtitlesProp.GetSize();
}

void PropertySizes::InitFFieldPathSize()
{
	if (!Settings::Internal::bUseFProperty)
		return;

	/* If the SetFieldPathPropertyByName function or the Value parameter weren't found, fallback to looping GObjects and looking for a Delegate. */
	auto OnPropertyNotFound = [&]() -> void
	{
		for (UEObject Obj : ObjectArray())
		{
			if (!Obj.IsA(EClassCastFlags::Struct))
				continue;

			for (UEProperty Prop : Obj.Cast<UEClass>().GetProperties())
			{
				if (Prop.IsA(EClassCastFlags::FieldPathProperty))
				{
					PropertySizes::FieldPathProperty = Prop.GetSize();
					return;
				}
			}
		}
	};

	const UEFunction SetFieldPathPropertyByNameFunc = ObjectArray::FindObjectFast<UEFunction>("SetFieldPathPropertyByName", EClassCastFlags::Function);

	if (!SetFieldPathPropertyByNameFunc)
		return OnPropertyNotFound();

	const UEProperty ValueParamProp = SetFieldPathPropertyByNameFunc.FindMember("Value", EClassCastFlags::FieldPathProperty);

	if (!ValueParamProp)
		return OnPropertyNotFound();

	PropertySizes::FieldPathProperty = ValueParamProp.GetSize();
}

void PropertySizes::InitTMulticastInlineDelegateSize()
{
	/* If the AudioComponent class or the OnQueueSubtitles member weren't found, fallback to looping GObjects and looking for a Delegate. */
	auto OnPropertyNotFound = [&]() -> void
		{
			for (UEObject Obj : ObjectArray())
			{
				if (!Obj.IsA(EClassCastFlags::Struct))
					continue;

				for (UEProperty Prop : Obj.Cast<UEClass>().GetProperties())
				{
					if (Prop.IsA(EClassCastFlags::MulticastInlineDelegateProperty))
					{
						PropertySizes::DelegateProperty = Prop.GetSize();
						return;
					}
				}
			}
		};

	const UEClass EmitterClass = ObjectArray::FindClassFast("Emitter");

	if (!EmitterClass)
		return OnPropertyNotFound();

	const UEProperty OnParticleSpawn = EmitterClass.FindMember("OnParticleSpawn", EClassCastFlags::MulticastDelegateProperty);

	if (!OnParticleSpawn)
		return OnPropertyNotFound();

	PropertySizes::MulticastInlineDelegateProperty = OnParticleSpawn.GetSize();
}