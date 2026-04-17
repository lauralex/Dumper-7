#include <format>

#include "Unreal/UnrealObjects.h"
#include "Unreal/ObjectArray.h"
#include "OffsetFinder/Offsets.h"

#include "RemoteMemory.h"
#include "RemoteContainers.h"


void* UEFFieldClass::GetAddress()
{
	return Class;
}

UEFFieldClass::operator bool() const
{
	return Class != nullptr;
}

EFieldClassID UEFFieldClass::GetId() const
{
	return RDeref<EFieldClassID>(Class + Off::FFieldClass::Id);
}

EClassCastFlags UEFFieldClass::GetCastFlags() const
{
	return RDeref<EClassCastFlags>(Class + Off::FFieldClass::CastFlags);
}

EClassFlags UEFFieldClass::GetClassFlags() const
{
	return RDeref<EClassFlags>(Class + Off::FFieldClass::ClassFlags);
}

UEFFieldClass UEFFieldClass::GetSuper() const
{
	return UEFFieldClass(RDeref<void*>(Class + Off::FFieldClass::SuperClass));
}

FName UEFFieldClass::GetFName() const
{
	return FName(Class + Off::FFieldClass::Name); //Not the real FName, but a wrapper which holds the address of a FName
}

bool UEFFieldClass::IsType(EClassCastFlags Flags) const
{
	return (Flags != EClassCastFlags::None ? (GetCastFlags() & Flags) : true);
}

std::string UEFFieldClass::GetName() const
{
	return Class ? GetFName().ToString() : "None";
}

std::string UEFFieldClass::GetValidName() const
{
	return Class ? GetFName().ToValidString() : "None";
}

std::string UEFFieldClass::GetCppName() const
{
	// This is evile dark magic code which shouldn't exist
	return "F" + GetValidName();
}

void* UEFField::GetAddress()
{
	return Field;
}

const void* UEFField::GetAddress() const
{
	return Field;
}

EObjectFlags UEFField::GetFlags() const
{
	return RDeref<EObjectFlags>(Field + Off::FField::Flags);
}

class UEObject UEFField::GetOwnerAsUObject() const
{
	if (IsOwnerUObject())
	{
		if (Settings::Internal::bUseMaskForFieldOwner)
			return (void*)(RDeref<uintptr_t>(Field + Off::FField::Owner) & ~0x1ull);

		return RDeref<void*>(Field + Off::FField::Owner);
	}

	return nullptr;
}

class UEFField UEFField::GetOwnerAsFField() const
{
	if (!IsOwnerUObject())
		return RDeref<void*>(Field + Off::FField::Owner);

	return nullptr;
}

class UEObject UEFField::GetOwnerUObject() const
{
	UEFField Field = *this;

	while (!Field.IsOwnerUObject() && Field.GetOwnerAsFField())
	{
		Field = Field.GetOwnerAsFField();
	}

	return Field.GetOwnerAsUObject();
}

UEFFieldClass UEFField::GetClass() const
{
	return UEFFieldClass(RDeref<void*>(Field + Off::FField::Class));
}

FName UEFField::GetFName() const
{
	return FName(Field + Off::FField::Name); //Not the real FName, but a wrapper which holds the address of a FName
}

UEFField UEFField::GetNext() const
{
	return UEFField(RDeref<void*>(Field + Off::FField::Next));
}

std::vector<std::pair<std::string, std::string>> UEFField::GetMetaData() const
{
	// EditorOnlyMetadata is a TMap<FName, FString>* in the target. External-mode walks the
	// map structure via RemoteContainers::ReadNameFStringMap (which internally walks
	// TSparseArray + FBitArray to find active elements and bulk-reads each FString's
	// wchar buffer). The keys are still remote addresses — we wrap them in FName so
	// subsequent FName::ToString() goes through NameArray-backed remote reads.
	const size_t keySize = (Off::InSDK::Name::FNameSize > 0x8) ? 0x10 : 0x8;

	const uintptr_t mapAddr = RDeref<uintptr_t>(Field + Off::FField::EditorOnlyMetadata);
	if (mapAddr == 0)
		return {};

	std::vector<std::pair<std::string, std::string>> Result;
	for (const auto& entry : RemoteContainers::ReadNameFStringMap(mapAddr, keySize))
	{
		Result.emplace_back(FName(reinterpret_cast<const uint8*>(entry.KeyRemoteAddr)).ToString(),
		                    entry.Value.ToString());
	}
	return Result;
}

template<typename UEType>
UEType UEFField::Cast() const
{
	return UEType(Field);
}

bool UEFField::IsOwnerUObject() const
{
	if (Settings::Internal::bUseMaskForFieldOwner)
	{
		return RDeref<uintptr_t>(Field + Off::FField::Owner) & 0x1;
	}

	return RDeref<bool>(Field + Off::FField::Owner + 0x8);
}

bool UEFField::IsA(EClassCastFlags Flags) const
{
	return (Flags != EClassCastFlags::None ? GetClass().IsType(Flags) : true);
}

std::string UEFField::GetName() const
{
	return Field ? GetFName().ToString() : "None";
}

std::string UEFField::GetValidName() const
{
	return Field ? GetFName().ToValidString() : "None";
}

std::string UEFField::GetCppName() const
{
	static UEClass ActorClass = ObjectArray::FindClassFast("Actor");
	static UEClass InterfaceClass = ObjectArray::FindClassFast("Interface");

	std::string Temp = GetValidName();

	if (IsA(EClassCastFlags::Class))
	{
		if (Cast<UEClass>().HasType(ActorClass))
		{
			return 'A' + Temp;
		}
		else if (Cast<UEClass>().HasType(InterfaceClass))
		{
			return 'I' + Temp;
		}

		return 'U' + Temp;
	}

	return 'F' + Temp;
}

UEFField::operator bool() const
{
	return Field != nullptr && reinterpret_cast<void*>(Field + Off::FField::Class) != nullptr;
}

bool UEFField::operator==(const UEFField& Other) const
{
	return Field == Other.Field;
}

bool UEFField::operator!=(const UEFField& Other) const
{
	return Field != Other.Field;
}

void(*UEObject::PE)(void*, void*, void*) = nullptr;

void* UEObject::GetAddress()
{
	return Object;
}

const void* UEObject::GetAddress() const
{
	return Object;
}

void* UEObject::GetVft() const
{
	return RDeref<void*>(Object);
}

EObjectFlags UEObject::GetFlags() const
{
	return RDeref<EObjectFlags>(Object + Off::UObject::Flags);
}

int32 UEObject::GetIndex() const
{
	return RDeref<int32>(Object + Off::UObject::Index);
}

UEClass UEObject::GetClass() const
{
	return UEClass(RDeref<void*>(Object + Off::UObject::Class));
}

FName UEObject::GetFName() const
{
	return FName(Object + Off::UObject::Name); //Not the real FName, but a wrapper which holds the address of a FName
}

UEObject UEObject::GetOuter() const
{
	return UEObject(RDeref<void*>(Object + Off::UObject::Outer));
}

int32 UEObject::GetPackageIndex() const
{
	return GetOutermost().GetIndex();
}

bool UEObject::HasAnyFlags(EObjectFlags Flags) const
{
	return GetFlags() & Flags;
}

bool UEObject::IsA(EClassCastFlags TypeFlags) const
{
	return (TypeFlags != EClassCastFlags::None ? GetClass().IsType(TypeFlags) : true);
}

bool UEObject::IsA(UEClass Class) const
{
	if (!Class)
		return false;

	// Cap the super-chain walk in case GetSuper returns a cyclic/garbage pointer on a stripped
	// or paged-out target. Deepest real UE super chain is <32; 256 is far beyond that.
	constexpr int32 kMaxSuperDepth = 256;
	int32 depth = 0;
	for (UEClass Clss = GetClass(); Clss && depth < kMaxSuperDepth; Clss = Clss.GetSuper().Cast<UEClass>(), ++depth)
	{
		if (Clss == Class)
			return true;
	}

	return false;
}

UEObject UEObject::GetOutermost() const
{
	UEObject Outermost = *this;

	// Cap the Outer chain walk. On stripped / paged-out UE5 targets the Outer pointer can be
	// garbage that forms a cycle; without the cap the whole Generator hangs in PackageManager::
	// Init walking one bad chain forever.
	constexpr int32 kMaxOuterDepth = 256;
	int32 depth = 0;
	for (UEObject Outer = *this; Outer && depth < kMaxOuterDepth; Outer = Outer.GetOuter(), ++depth)
	{
		Outermost = Outer;
	}

	return Outermost;
}

std::string UEObject::StringifyObjFlags() const
{
	return *this ? StringifyObjectFlags(GetFlags()) : "NoFlags";
}

std::string UEObject::GetName() const
{
	return Object ? GetFName().ToString() : "None";
}

std::string UEObject::GetNameWithPath() const
{
	return Object ? GetFName().ToRawString() : "None";
}

std::string UEObject::GetValidName() const
{
	return Object ? GetFName().ToValidString() : "None";
}

std::string UEObject::GetCppName() const
{
	static UEClass ActorClass = nullptr;
	static UEClass InterfaceClass = nullptr;

	if (ActorClass == nullptr)
		ActorClass = ObjectArray::FindClassFast("Actor");

	if (InterfaceClass == nullptr)
		InterfaceClass = ObjectArray::FindClassFast("Interface");

	std::string Temp = GetValidName();

	if (IsA(EClassCastFlags::Class))
	{
		if (Cast<UEClass>().HasType(ActorClass))
		{
			return 'A' + Temp;
		}
		else if (Cast<UEClass>().HasType(InterfaceClass))
		{
			return 'I' + Temp;
		}

		return 'U' + Temp;
	}

	return 'F' + Temp;
}

std::string UEObject::GetFullName(int32& OutNameLength) const
{
	if (*this)
	{
		std::string Temp;

		for (UEObject Outer = GetOuter(); Outer; Outer = Outer.GetOuter())
		{
			Temp = Outer.GetName() + '.' + Temp;
		}

		std::string Name = GetName();
		OutNameLength = Name.size() + 1;

		Name = GetClass().GetName() + ' ' + Temp + Name;

		return Name;
	}

	return "None";
}

std::string UEObject::GetFullName() const
{
	if (*this)
	{
		std::string Temp;

		for (UEObject Outer = GetOuter(); Outer; Outer = Outer.GetOuter())
		{
			Temp = Outer.GetName() + "." + Temp;
		}

		std::string Name = GetClass().GetName();
		Name += " ";
		Name += Temp;
		Name += GetName();

		return Name;
	}

	return "None";
}

std::string UEObject::GetPathName() const
{
	if (*this)
	{
		std::string Temp;

		for (UEObject Outer = GetOuter(); Outer; Outer = Outer.GetOuter())
		{
			Temp = Outer.GetNameWithPath() + "." + Temp;
		}

		std::string Name = GetClass().GetNameWithPath();
		Name += " ";
		Name += Temp;
		Name += GetNameWithPath();

		return Name;
	}

	return "None";
}


UEObject::operator bool() const
{
	// if an object is 0x10000F000 it passes the nullptr check
	return Object != nullptr && reinterpret_cast<void*>(Object + Off::UObject::Class) != nullptr;
}

UEObject::operator uint8* ()
{
	return Object;
}

bool UEObject::operator==(const UEObject& Other) const
{
	return Object == Other.Object;
}

bool UEObject::operator!=(const UEObject& Other) const
{
	return Object != Other.Object;
}

void UEObject::ProcessEvent(UEFunction Func, void* Params)
{
	// In external mode the dumper cannot call into the target. Any call site that reaches
	// this is a bug — the two places that historically used ProcessEvent (game name/version
	// discovery in main.cpp and Conv_StringToText in InitTextOffsets) are now replaced with
	// passive techniques in Phase 4. Leaving a loud diagnostic so any regression surfaces.
	(void)Func;
	(void)Params;
	std::cerr << "[Dumper-7] UEObject::ProcessEvent invoked in external mode — this is a no-op "
	             "and the caller should be reworked to avoid calling into the target.\n";
}

UEField UEField::GetNext() const
{
	return UEField(RDeref<void*>(Object + Off::UField::Next));
}

bool UEField::IsNextValid() const
{
	return (bool)GetNext();
}

std::vector<std::pair<FName, int64>> UEEnum::GetNameValuePairs() const
{
	// External-mode rewrite: instead of reinterpret_cast<TArray<...>*>(remoteAddr) + iteration,
	// we walk the remote TArray via RemoteContainers::ReadNameValueTArray which bulk-reads the
	// element storage and returns per-element {keyRemoteAddr, valueBytes} pairs. The FName
	// wrappers are constructed from remote addresses so FName::ToString() drives through
	// NameArray-backed hypercall reads.
	static constexpr uintptr_t PointerMaskNoTag = ~uintptr_t{0x1};

	const size_t keySize = Settings::Internal::bUseCasePreservingName ? 0x10 : 0x8;

	// UE5.6+: FNameData layout. UEnum::Names - 8 points at a tagged pointer to Names.
	if (Settings::Internal::bIsNewUE5EnumNamesContainer)
	{
		std::vector<std::pair<FName, int64>> Ret;
		const uintptr_t base = reinterpret_cast<uintptr_t>(Object) + Off::UEnum::Names - 0x8;

		const uintptr_t taggedNamesPtr = RDeref<uintptr_t>(base);
		const bool bIsNamesPtrTagged = (taggedNamesPtr & 0x1) != 0;
		const uintptr_t namesPtr = taggedNamesPtr & PointerMaskNoTag;
		if (!bIsNamesPtrTagged)
		{
			std::cerr << "Dumper-7 [UEEnum::GetNameValuePairs()]: UEnum::Names pointer is not tagged! StaticNamesUTF8 is not supported yet!" << std::endl;
			return Ret;
		}

		const uintptr_t valuesPtr = RDeref<uintptr_t>(base + 0x8) & PointerMaskNoTag;
		const int32 numValues = RDeref<int32>(base + 0x10);
		if (numValues <= 0 || numValues > 0x10000)
			return Ret;

		const uint32_t fnameSize = Off::InSDK::Name::FNameSize;

		// Bulk-read the int64 values buffer once.
		std::vector<int64> values(numValues);
		if (!RemoteMemory::ReadBuffer(valuesPtr, values.data(), numValues * sizeof(int64)))
			return Ret;

		Ret.reserve(numValues);
		for (int32 i = 0; i < numValues; ++i)
		{
			const uintptr_t keyRemote = namesPtr + (static_cast<uintptr_t>(i) * fnameSize);
			Ret.push_back({ FName(reinterpret_cast<const uint8*>(keyRemote)), values[i] });
		}
		return Ret;
	}

	const uintptr_t namesArrayAddr = reinterpret_cast<uintptr_t>(Object) + Off::UEnum::Names;

	// Fast path: plain TArray<NameNByte> (just names, indexed by position — older UE).
	if (Settings::Internal::bIsEnumNameOnly)
	{
		std::vector<std::pair<FName, int64>> Ret;
		const RemoteContainers::TArrayHeader header = RemoteContainers::ReadTArrayHeader(namesArrayAddr);
		if (!header.IsValid())
			return Ret;

		Ret.reserve(header.Num);
		for (int32 i = 0; i < header.Num; ++i)
		{
			const uintptr_t keyRemote = header.Data + (static_cast<uintptr_t>(i) * keySize);
			Ret.push_back({ FName(reinterpret_cast<const uint8*>(keyRemote)), static_cast<int64>(i) });
		}
		return Ret;
	}

	// TArray<TPair<NameNByte, int64>> or TArray<TPair<NameNByte, UInt8As64>> (small enum values).
	const size_t valueSize = Settings::Internal::bIsSmallEnumValue ? sizeof(uint8) : sizeof(int64);

	std::vector<std::pair<FName, int64>> Ret;
	for (const auto& entry : RemoteContainers::ReadNameValueTArray(namesArrayAddr, keySize, valueSize))
	{
		int64 value = 0;
		if (Settings::Internal::bIsSmallEnumValue)
			value = static_cast<int64>(entry.ValueBytes & 0xFF);
		else
			value = static_cast<int64>(entry.ValueBytes);
		Ret.push_back({ FName(reinterpret_cast<const uint8*>(entry.KeyRemoteAddr)), value });
	}
	return Ret;
}

std::string UEEnum::GetSingleName(int32 Index) const
{
	return GetNameValuePairs()[Index].first.ToString();
}

std::string UEEnum::GetEnumPrefixedName() const
{
	std::string Temp = GetValidName();

	return Temp[0] == 'E' ? Temp : 'E' + Temp;
}

std::string UEEnum::GetEnumTypeAsStr() const
{
	return "enum class " + GetEnumPrefixedName();
}

UEStruct UEStruct::GetSuper() const
{
	return UEStruct(RDeref<void*>(Object + Off::UStruct::SuperStruct));
}

UEField UEStruct::GetChild() const
{
	return UEField(RDeref<void*>(Object + Off::UStruct::Children));
}

UEFField UEStruct::GetChildProperties() const
{
	return UEFField(RDeref<void*>(Object + Off::UStruct::ChildProperties));
}

int16 UEStruct::GetMinAlignment() const
{
	return RDeref<int16>(Object + Off::UStruct::MinAlignment);
}

int32 UEStruct::GetStructSize() const
{
	return RDeref<int32>(Object + Off::UStruct::Size);
}

bool UEStruct::HasType(UEStruct Type) const
{
	if (Type == nullptr)
		return false;

	// Cap the super chain — a cyclic GetSuper on a stripped/paged-out target would otherwise spin.
	constexpr int32 kMaxSuperDepth = 256;
	int32 depth = 0;
	for (UEStruct S = *this; S && depth < kMaxSuperDepth; S = S.GetSuper(), ++depth)
	{
		if (S == Type)
			return true;
	}

	return false;
}

// External-mode guard: the FField/UField linked lists are walked by dereferencing the Next
// pointer each iteration. On stripped or paged-out UE5 targets the Next pointer can be
// garbage that happens to form a cycle (A -> B -> A) — without a depth cap the walker spins
// forever and the whole generator stage hangs. Real UE structs have at most a few hundred
// fields; the cap is several orders of magnitude above that but low enough to fail fast.
static constexpr int32 kMaxFieldChainDepth = 4096;

std::vector<UEProperty> UEStruct::GetProperties() const
{
	std::vector<UEProperty> Properties;

	if (Settings::Internal::bUseFProperty)
	{
		int32 depth = 0;
		for (UEFField Field = GetChildProperties(); Field && depth < kMaxFieldChainDepth; Field = Field.GetNext(), ++depth)
		{
			if (Field.IsA(EClassCastFlags::Property))
				Properties.push_back(Field.Cast<UEProperty>());
		}

		return Properties;
	}
	int32 depth = 0;
	for (UEField Field = GetChild(); Field && depth < kMaxFieldChainDepth; Field = Field.GetNext(), ++depth)
	{
		if (Field.IsA(EClassCastFlags::Property))
			Properties.push_back(Field.Cast<UEProperty>());
	}

	return Properties;
}

std::vector<UEFunction> UEStruct::GetFunctions() const
{
	std::vector<UEFunction> Functions;

	int32 depth = 0;
	for (UEField Field = GetChild(); Field && depth < kMaxFieldChainDepth; Field = Field.GetNext(), ++depth)
	{
		if (Field.IsA(EClassCastFlags::Function))
			Functions.push_back(Field.Cast<UEFunction>());
	}

	return Functions;
}

UEProperty UEStruct::FindMember(const std::string& MemberName, EClassCastFlags TypeFlags) const
{
	if (!Object)
		return nullptr;

	if (Settings::Internal::bUseFProperty)
	{
		int32 depth = 0;
		for (UEFField Field = GetChildProperties(); Field && depth < kMaxFieldChainDepth; Field = Field.GetNext(), ++depth)
		{
			if (Field.IsA(TypeFlags) && Field.GetName() == MemberName)
			{
				return Field.Cast<UEProperty>();
			}
		}
	}

	int32 depth = 0;
	for (UEField Field = GetChild(); Field && depth < kMaxFieldChainDepth; Field = Field.GetNext(), ++depth)
	{
		if (Field.IsA(TypeFlags) && Field.GetName() == MemberName)
		{
			return Field.Cast<UEProperty>();
		}
	}

	return nullptr;
}

bool UEStruct::HasMembers() const
{
	if (!Object)
		return false;

	if (Settings::Internal::bUseFProperty)
	{
		int32 depth = 0;
		for (UEFField Field = GetChildProperties(); Field && depth < kMaxFieldChainDepth; Field = Field.GetNext(), ++depth)
		{
			if (Field.IsA(EClassCastFlags::Property))
				return true;
		}
	}
	else
	{
		int32 depth = 0;
		for (UEField F = GetChild(); F && depth < kMaxFieldChainDepth; F = F.GetNext(), ++depth)
		{
			if (F.IsA(EClassCastFlags::Property))
				return true;
		}
	}

	return false;
}

EClassCastFlags UEClass::GetCastFlags() const
{
	return RDeref<EClassCastFlags>(Object + Off::UClass::CastFlags);
}

std::string UEClass::StringifyCastFlags() const
{
	return StringifyClassCastFlags(GetCastFlags());
}

bool UEClass::IsType(EClassCastFlags TypeFlag) const
{
	return (TypeFlag != EClassCastFlags::None ? (GetCastFlags() & TypeFlag) : true);
}

UEObject UEClass::GetDefaultObject() const
{
	return UEObject(RDeref<void*>(Object + Off::UClass::ClassDefaultObject));
}

TArray<FImplementedInterface> UEClass::GetImplementedInterfaces() const
{
	// This method isn't actually called by the dumper itself — only OffsetFinder probes the
	// layout via its own reinterpret_cast. If a future caller needs the array contents in
	// external mode, wrap this with RemoteContainers; today it just reads the TArray header
	// as raw bytes. Data* inside the returned TArray is a remote VA; do not dereference.
	return RDeref<TArray<FImplementedInterface>>(Object + Off::UClass::ImplementedInterfaces);
}

UEFunction UEClass::GetFunction(const std::string& ClassName, const std::string& FuncName) const
{
	for (UEStruct Struct = *this; Struct; Struct = Struct.GetSuper())
	{
		if (Struct.GetName() != ClassName)
			continue;

		for (UEField Field = Struct.GetChild(); Field; Field = Field.GetNext())
		{
			if (Field.IsA(EClassCastFlags::Function) && Field.GetName() == FuncName)
			{
				return Field.Cast<UEFunction>();
			}
		}

	}

	return nullptr;
}

EFunctionFlags UEFunction::GetFunctionFlags() const
{
	return RDeref<EFunctionFlags>(Object + Off::UFunction::FunctionFlags);
}

bool UEFunction::HasFlags(EFunctionFlags FuncFlags) const
{
	return GetFunctionFlags() & FuncFlags;
}

void* UEFunction::GetExecFunction() const
{
	return RDeref<void*>(Object + Off::UFunction::ExecFunction);
}

UEProperty UEFunction::GetReturnProperty() const
{
	for (auto Prop : GetProperties())
	{
		if (Prop.HasPropertyFlags(EPropertyFlags::ReturnParm))
			return Prop;
	}

	return nullptr;
}


std::string UEFunction::StringifyFlags(const char* Seperator)  const
{
	return StringifyFunctionFlags(GetFunctionFlags(), Seperator);
}

std::string UEFunction::GetParamStructName() const
{
	return GetOuter().GetCppName() + "_" + GetValidName() + "_Params";
}

void* UEProperty::GetAddress()
{
	return Base;
}

const void* UEProperty::GetAddress() const
{
	return Base;
}

std::pair<UEClass, UEFFieldClass> UEProperty::GetClass() const
{
	if (Settings::Internal::bUseFProperty)
		return { UEClass(0), UEFField(Base).GetClass() };

	return { UEObject(Base).GetClass(), UEFFieldClass(0) };
}

EClassCastFlags UEProperty::GetCastFlags() const
{
	auto [Class, FieldClass] = GetClass();

	return Class ? Class.GetCastFlags() : FieldClass.GetCastFlags();
}

UEProperty::operator bool() const
{
	return Base != nullptr && ((Base + Off::UObject::Class) != nullptr || (Base + Off::FField::Class) != nullptr);
}


bool UEProperty::IsA(EClassCastFlags TypeFlags) const
{
	if (GetClass().first)
		return GetClass().first.IsType(TypeFlags);

	return GetClass().second.IsType(TypeFlags);
}

FName UEProperty::GetFName() const
{
	if (Settings::Internal::bUseFProperty)
	{
		return FName(Base + Off::FField::Name); //Not the real FName, but a wrapper which holds the address of a FName
	}

	return FName(Base + Off::UObject::Name); //Not the real FName, but a wrapper which holds the address of a FName
}

int32 UEProperty::GetArrayDim() const
{
	if (Settings::Internal::bUseUint8ArrayDim)
		return RDeref<uint8>(Base + Off::Property::ArrayDim);

	return RDeref<int32>(Base + Off::Property::ArrayDim);
}

int32 UEProperty::GetSize() const
{
	return RDeref<int32>(Base + Off::Property::ElementSize);
}

int32 UEProperty::GetOffset() const
{
	return RDeref<int32>(Base + Off::Property::Offset_Internal);
}

EPropertyFlags UEProperty::GetPropertyFlags() const
{
	return RDeref<EPropertyFlags>(Base + Off::Property::PropertyFlags);
}

bool UEProperty::HasPropertyFlags(EPropertyFlags PropertyFlag) const
{
	return GetPropertyFlags() & PropertyFlag;
}

bool UEProperty::IsType(EClassCastFlags PossibleTypes) const
{
	return (static_cast<uint64>(GetCastFlags()) & static_cast<uint64>(PossibleTypes)) != 0;
}

std::string UEProperty::GetName() const
{
	return Base ? GetFName().ToString() : "None";
}

std::string UEProperty::GetValidName() const
{
	return Base ? GetFName().ToValidString() : "None";
}

int32 UEProperty::GetAlignment() const
{
	EClassCastFlags TypeFlags = (GetClass().first ? GetClass().first.GetCastFlags() : GetClass().second.GetCastFlags());

	if (TypeFlags & EClassCastFlags::ByteProperty)
	{
		return alignof(uint8); // 0x1
	}
	else if (TypeFlags & EClassCastFlags::UInt16Property)
	{
		return alignof(uint16); // 0x2
	}
	else if (TypeFlags & EClassCastFlags::UInt32Property)
	{
		return alignof(uint32); // 0x4
	}
	else if (TypeFlags & EClassCastFlags::UInt64Property)
	{
		return sizeof(void*); // 0x4 on 32bit or 0x8 on 64bit
	}
	else if (TypeFlags & EClassCastFlags::Int8Property)
	{
		return alignof(int8); // 0x1
	}
	else if (TypeFlags & EClassCastFlags::Int16Property)
	{
		return alignof(int16); // 0x2
	}
	else if (TypeFlags & EClassCastFlags::IntProperty)
	{
		return alignof(int32); // 0x4
	}
	else if (TypeFlags & EClassCastFlags::Int64Property)
	{
		return sizeof(void*); // 0x4 on 32bit or 0x8 on 64bit
	}
	else if (TypeFlags & EClassCastFlags::FloatProperty)
	{
		return alignof(float); // 0x4
	}
	else if (TypeFlags & EClassCastFlags::DoubleProperty)
	{
		return sizeof(void*); // 0x4 on 32bit or 0x8 on 64bit
	}
	else if (TypeFlags & EClassCastFlags::ClassProperty)
	{
		return alignof(void*); // 0x4 / 0x8
	}
	else if (TypeFlags & EClassCastFlags::NameProperty)
	{
		return alignof(int32); // FName is a bunch of int32s
	}
	else if (TypeFlags & EClassCastFlags::StrProperty)
	{
		return alignof(FString); // 0x8
	}
	else if (TypeFlags & EClassCastFlags::TextProperty)
	{
		return alignof(FString); // alignof member FString
	}
	else if (TypeFlags & EClassCastFlags::BoolProperty)
	{
		return alignof(bool); // 0x1
	}
	else if (TypeFlags & EClassCastFlags::StructProperty)
	{
		return Cast<UEStructProperty>().GetUnderlayingStruct().GetMinAlignment();
	}
	else if (TypeFlags & EClassCastFlags::ArrayProperty)
	{
		return alignof(TArray<int>); // 0x8
	}
	else if (TypeFlags & EClassCastFlags::DelegateProperty)
	{
		return alignof(int32); // 0x4
	}
	else if (TypeFlags & EClassCastFlags::WeakObjectProperty)
	{
		return alignof(int32); // TWeakObjectPtr is a bunch of int32s
	}
	else if (TypeFlags & EClassCastFlags::LazyObjectProperty)
	{
		return alignof(int32); // TLazyObjectPtr is a bunch of int32s
	}
	else if (TypeFlags & EClassCastFlags::SoftClassProperty)
	{
		return alignof(FString); // alignof member FString
	}
	else if (TypeFlags & EClassCastFlags::SoftObjectProperty)
	{
		return alignof(FString); // alignof member FString
	}
	else if (TypeFlags & EClassCastFlags::ObjectProperty)
	{
		return alignof(void*); // 0x4 / 0x8
	}
	else if (TypeFlags & EClassCastFlags::MapProperty)
	{
		return alignof(TArray<int>); // 0x8, TMap contains a TArray
	}
	else if (TypeFlags & EClassCastFlags::SetProperty)
	{
		return alignof(TArray<int>); // 0x8, TSet contains a TArray
	}
	else if (TypeFlags & EClassCastFlags::EnumProperty)
	{
		UEProperty P = Cast<UEEnumProperty>().GetUnderlayingProperty();

		return P ? P.GetAlignment() : 0x1;
	}
	else if (TypeFlags & EClassCastFlags::InterfaceProperty)
	{
		return alignof(void*); // 0x4 / 0x8
	}
	else if (TypeFlags & EClassCastFlags::FieldPathProperty)
	{
		return alignof(TArray<int>); // alignof member TArray<FName> and ptr;
	}
	else if (TypeFlags & EClassCastFlags::MulticastSparseDelegateProperty)
	{
		return 0x1; // size in PropertyFixup (alignment isn't greater than size)
	}
	else if (TypeFlags & EClassCastFlags::MulticastInlineDelegateProperty)
	{
		return alignof(TArray<int>);  // alignof member TArray<FName>
	}
	else if (TypeFlags & EClassCastFlags::OptionalProperty)
	{
		UEProperty ValueProperty = Cast<UEOptionalProperty>().GetValueProperty();

		/* If this check is true it means, that there is no bool in this TOptional to check if the value is set */
		if (ValueProperty.GetSize() == GetSize()) [[unlikely]]
			return ValueProperty.GetAlignment();

		return  GetSize() - ValueProperty.GetSize();
	}

	if (Settings::Internal::bUseFProperty)
	{
		static std::unordered_map<void*, int32> UnknownProperties;

		static auto TryFindPropertyRefInOptionalToGetAlignment = [](std::unordered_map<void*, int32>& OutProperties, void* PropertyClass) -> int32
		{
			/* Search for a TOptionalProperty that contains an instance of this property */
			for (UEObject Obj : ObjectArray())
			{
				if (!Obj.IsA(EClassCastFlags::Struct))
					continue;

				for (UEProperty Prop : Obj.Cast<UEStruct>().GetProperties())
				{
					if (!Prop.IsA(EClassCastFlags::OptionalProperty) || Prop.IsA(EClassCastFlags::ObjectPropertyBase))
						continue;

					UEOptionalProperty Optional = Prop.Cast<UEOptionalProperty>();

					/* Safe to use first member, as we're guaranteed to use FProperty */
					if (Optional.GetValueProperty().GetClass().second.GetAddress() == PropertyClass)
						return OutProperties.insert({ PropertyClass, Optional.GetAlignment() }).first->second;
				}
			}

			return OutProperties.insert({ PropertyClass, 0x1 }).first->second;
		};

		auto It = UnknownProperties.find(GetClass().second.GetAddress());

		/* Safe to use first member, as we're guaranteed to use FProperty */
		if (It == UnknownProperties.end())
			return TryFindPropertyRefInOptionalToGetAlignment(UnknownProperties, GetClass().second.GetAddress());

		return It->second;
	}

	return 0x1;
}

std::string UEProperty::GetCppType() const
{
	EClassCastFlags TypeFlags = (GetClass().first ? GetClass().first.GetCastFlags() : GetClass().second.GetCastFlags());

	if (TypeFlags & EClassCastFlags::ByteProperty)
	{
		return Cast<UEByteProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::UInt16Property)
	{
		return "uint16";
	}
	else if (TypeFlags & EClassCastFlags::UInt32Property)
	{
		return "uint32";
	}
	else if (TypeFlags & EClassCastFlags::UInt64Property)
	{
		return "uint64";
	}
	else if (TypeFlags & EClassCastFlags::Int8Property)
	{
		return "int8";
	}
	else if (TypeFlags & EClassCastFlags::Int16Property)
	{
		return "int16";
	}
	else if (TypeFlags & EClassCastFlags::IntProperty)
	{
		return "int32";
	}
	else if (TypeFlags & EClassCastFlags::Int64Property)
	{
		return "int64";
	}
	else if (TypeFlags & EClassCastFlags::FloatProperty)
	{
		return "float";
	}
	else if (TypeFlags & EClassCastFlags::DoubleProperty)
	{
		return "double";
	}
	else if (TypeFlags & EClassCastFlags::ClassProperty)
	{
		return Cast<UEClassProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::NameProperty)
	{
		return "class FName";
	}
	else if (TypeFlags & EClassCastFlags::StrProperty)
	{
		return "class FString";
	}
	else if (TypeFlags & EClassCastFlags::TextProperty)
	{
		return "class FText";
	}
	else if (TypeFlags & EClassCastFlags::BoolProperty)
	{
		return Cast<UEBoolProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::StructProperty)
	{
		return Cast<UEStructProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::ArrayProperty)
	{
		return Cast<UEArrayProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::WeakObjectProperty)
	{
		return Cast<UEWeakObjectProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::LazyObjectProperty)
	{
		return Cast<UELazyObjectProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::SoftClassProperty)
	{
		return Cast<UESoftClassProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::SoftObjectProperty)
	{
		return Cast<UESoftObjectProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::ObjectProperty)
	{
		return Cast<UEObjectProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::MapProperty)
	{
		return Cast<UEMapProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::SetProperty)
	{
		return Cast<UESetProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::EnumProperty)
	{
		return Cast<UEEnumProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::InterfaceProperty)
	{
		return Cast<UEInterfaceProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::FieldPathProperty)
	{
		if (Settings::Internal::bIsObjPtrInsteadOfFieldPathProperty)
			return Cast<UEObjectProperty>().GetCppType();

		return Cast<UEFieldPathProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::DelegateProperty)
	{
		return Cast<UEDelegateProperty>().GetCppType();
	}
	else if (TypeFlags & EClassCastFlags::OptionalProperty)
	{
		return Cast<UEOptionalProperty>().GetCppType();
	}
	else
	{
		return (GetClass().first ? GetClass().first.GetCppName() : GetClass().second.GetCppName()) + "_";;
	}
}

std::string UEProperty::GetPropClassName() const
{
	return GetClass().first ? GetClass().first.GetName() : GetClass().second.GetName();
}

std::string UEProperty::StringifyFlags() const
{
	return StringifyPropertyFlags(GetPropertyFlags());
}

UEEnum UEByteProperty::GetEnum() const
{
	return UEEnum(RDeref<void*>(Base + Off::ByteProperty::Enum));
}

std::string UEByteProperty::GetCppType() const
{
	if (UEEnum Enum = GetEnum())
	{
		return Enum.GetEnumTypeAsStr();
	}

	return "uint8";
}

uint8 UEBoolProperty::GetFieldMask() const
{
	return RDeref<Off::BoolProperty::UBoolPropertyBase>(Base + Off::BoolProperty::Base).FieldMask;
}

uint8 UEBoolProperty::GetByteOffset() const
{
	return RDeref<Off::BoolProperty::UBoolPropertyBase>(Base + Off::BoolProperty::Base).ByteOffset;
}

uint8 UEBoolProperty::GetBitIndex() const
{
	const uint8 FieldMask = GetFieldMask();

	const uint8_t InitialBitOffset = GetByteOffset() * 0x8; // Example: Offset 3 ==> This bitfield is in the 4th bit ==> 3 lower bytes have 3 * 8 = 24 bits

	if (FieldMask != 0xFF)
	{
		if (FieldMask == 0x01) { return InitialBitOffset + 0; }
		if (FieldMask == 0x02) { return InitialBitOffset + 1; }
		if (FieldMask == 0x04) { return InitialBitOffset + 2; }
		if (FieldMask == 0x08) { return InitialBitOffset + 3; }
		if (FieldMask == 0x10) { return InitialBitOffset + 4; }
		if (FieldMask == 0x20) { return InitialBitOffset + 5; }
		if (FieldMask == 0x40) { return InitialBitOffset + 6; }
		if (FieldMask == 0x80) { return InitialBitOffset + 7; }
	}

	return 0xFF;
}

bool UEBoolProperty::IsNativeBool() const
{
	return RDeref<Off::BoolProperty::UBoolPropertyBase>(Base + Off::BoolProperty::Base).FieldMask == 0xFF;
}

std::string UEBoolProperty::GetCppType() const
{
	return IsNativeBool() ? "bool" : "uint8";
}

UEClass UEObjectProperty::GetPropertyClass() const
{
	return UEClass(RDeref<void*>(Base + Off::ObjectProperty::PropertyClass));
}

std::string UEObjectProperty::GetCppType() const
{
	return std::format("class {}*", GetPropertyClass() ? GetPropertyClass().GetCppName() : "UObject");
}

UEClass UEClassProperty::GetMetaClass() const
{
	return UEClass(RDeref<void*>(Base + Off::ClassProperty::MetaClass));
}

std::string UEClassProperty::GetCppType() const
{
	return HasPropertyFlags(EPropertyFlags::UObjectWrapper) ? std::format("TSubclassOf<class {}>", GetMetaClass().GetCppName()) : "class UClass*";
}

std::string UEWeakObjectProperty::GetCppType() const
{
	return std::format("TWeakObjectPtr<class {}>", GetPropertyClass() ? GetPropertyClass().GetCppName() : "UObject");
}

std::string UELazyObjectProperty::GetCppType() const
{
	return std::format("TLazyObjectPtr<class {}>", GetPropertyClass() ? GetPropertyClass().GetCppName() : "UObject");
}

std::string UESoftObjectProperty::GetCppType() const
{
	return std::format("TSoftObjectPtr<class {}>", GetPropertyClass() ? GetPropertyClass().GetCppName() : "UObject");
}

std::string UESoftClassProperty::GetCppType() const
{
	return std::format("TSoftClassPtr<class {}>", GetMetaClass() ? GetMetaClass().GetCppName() : GetPropertyClass().GetCppName());
}

std::string UEInterfaceProperty::GetCppType() const
{
	return std::format("TScriptInterface<class {}>", GetPropertyClass().GetCppName());
}

UEStruct UEStructProperty::GetUnderlayingStruct() const
{
	return UEStruct(RDeref<void*>(Base + Off::StructProperty::Struct));
}

std::string UEStructProperty::GetCppType() const
{
	return std::format("struct {}", GetUnderlayingStruct().GetCppName());
}

UEProperty UEArrayProperty::GetInnerProperty() const
{
	return UEProperty(RDeref<void*>(Base + Off::ArrayProperty::Inner));
}

std::string UEArrayProperty::GetCppType() const
{
	return std::format("TArray<{}>", GetInnerProperty().GetCppType());
}

UEFunction UEDelegateProperty::GetSignatureFunction() const
{
	return UEFunction(RDeref<void*>(Base + Off::DelegateProperty::SignatureFunction));
}

std::string UEDelegateProperty::GetCppType() const
{
	return "TDeleage<GetCppTypeIsNotImplementedForDelegates>";
}

UEFunction UEMulticastInlineDelegateProperty::GetSignatureFunction() const
{
	// Uses "Off::DelegateProperty::SignatureFunction" on purpose
	return UEFunction(RDeref<void*>(Base + Off::DelegateProperty::SignatureFunction));
}

std::string UEMulticastInlineDelegateProperty::GetCppType() const
{
	return "TMulticastInlineDelegate<GetCppTypeIsNotImplementedForDelegates>";
}

UEProperty UEMapProperty::GetKeyProperty() const
{
	return UEProperty(RDeref<Off::MapProperty::UMapPropertyBase>(Base + Off::MapProperty::Base).KeyProperty);
}

UEProperty UEMapProperty::GetValueProperty() const
{
	return UEProperty(RDeref<Off::MapProperty::UMapPropertyBase>(Base + Off::MapProperty::Base).ValueProperty);
}

std::string UEMapProperty::GetCppType() const
{
	return std::format("TMap<{}, {}>", GetKeyProperty().GetCppType(), GetValueProperty().GetCppType());
}

UEProperty UESetProperty::GetElementProperty() const
{
	return UEProperty(RDeref<void*>(Base + Off::SetProperty::ElementProp));
}

std::string UESetProperty::GetCppType() const
{
	return std::format("TSet<{}>", GetElementProperty().GetCppType());
}

UEProperty UEEnumProperty::GetUnderlayingProperty() const
{
	return UEProperty(RDeref<Off::EnumProperty::UEnumPropertyBase>(Base + Off::EnumProperty::Base).UnderlayingProperty);
}

UEEnum UEEnumProperty::GetEnum() const
{
	return UEEnum(RDeref<Off::EnumProperty::UEnumPropertyBase>(Base + Off::EnumProperty::Base).Enum);
}

std::string UEEnumProperty::GetCppType() const
{
	if (GetEnum())
		return GetEnum().GetEnumTypeAsStr();

	return GetUnderlayingProperty().GetCppType();
}

UEFFieldClass UEFieldPathProperty::GetFieldClass() const
{
	return UEFFieldClass(RDeref<void*>(Base + Off::FieldPathProperty::FieldClass));
}

std::string UEFieldPathProperty::GetCppType() const
{
	return std::format("TFieldPath<struct {}>", GetFieldClass().GetCppName());
}

UEProperty UEOptionalProperty::GetValueProperty() const
{
	return UEProperty(RDeref<void*>(Base + Off::OptionalProperty::ValueProperty));
}

std::string UEOptionalProperty::GetCppType() const
{
	return std::format("TOptional<{}>", GetValueProperty().GetCppType());
}
