#include "Wrappers/StructWrapper.h"
#include "Managers/MemberManager.h"

StructWrapper::StructWrapper(const PredefinedStruct* const Predef)
    : PredefStruct(Predef), InfoHandle()
{
}

StructWrapper::StructWrapper(const UEStruct Str)
    : Struct(Str), InfoHandle(StructManager::GetInfo(Str)), bIsUnrealStruct(true)
{
}

UEStruct StructWrapper::GetUnrealStruct() const
{
    assert(bIsUnrealStruct && "StructWrapper doesn't contain UnrealStruct. Illegal call to 'GetUnrealStruct()'.");

    return bIsUnrealStruct ? Struct : nullptr;
}

std::string StructWrapper::GetName() const
{
    return bIsUnrealStruct ? Struct.GetValidName() : PredefStruct->UniqueName;
}

std::string StructWrapper::GetRawName() const
{
    return bIsUnrealStruct ? Struct.GetName() : PredefStruct->UniqueName;
}

std::string StructWrapper::GetFullName() const
{
    return bIsUnrealStruct ? Struct.GetFullName() : "Predefined struct " + PredefStruct->UniqueName;
}

StructWrapper StructWrapper::GetSuper() const
{
    return bIsUnrealStruct ? StructWrapper(Struct.GetSuper()) : PredefStruct->Super;
}

MemberManager StructWrapper::GetMembers() const
{
    return bIsUnrealStruct ? MemberManager(Struct) : MemberManager(PredefStruct);
}


/* Name, bIsUnique */
std::pair<std::string, bool> StructWrapper::GetUniqueName() const
{
    if (bIsUnrealStruct)
    {
        // If StructManager didn't track this struct (e.g. a garbage dep that survived the
        // dep-prune because it happened to reference a real UObject index that wasn't a
        // struct, or the struct was synthesised from corrupted FField data), InfoHandle is
        // a null wrapper and InfoHandle.GetName() would dereference a null StructInfo*.
        // Fall back to the UEStruct's own name; it's at least internally consistent even
        // if it doesn't match the managed-uniqueness invariant.
        if (!InfoHandle.IsValidHandle())
            return { Struct ? Struct.GetValidName() : std::string("UnknownStruct"), true };

        const auto& StringEntry = InfoHandle.GetName();

        return { StringEntry.GetName(), StringEntry.IsUnique() };
    }

    return { PredefStruct->UniqueName, true };
}

int32 StructWrapper::GetLastMemberEnd() const
{
    if (bIsUnrealStruct)
        return InfoHandle.IsValidHandle() ? InfoHandle.GetLastMemberEnd() : 0x0;
    return 0x0;
}

int32 StructWrapper::GetAlignment() const
{
    if (bIsUnrealStruct)
        return InfoHandle.IsValidHandle() ? InfoHandle.GetAlignment() : alignof(void*);
    return PredefStruct->Alignment;
}

int32 StructWrapper::GetSize() const
{
    if (bIsUnrealStruct)
        return InfoHandle.IsValidHandle() ? InfoHandle.GetSize() : 0x0;
    return Align(PredefStruct->Size, PredefStruct->Alignment);
}

int32 StructWrapper::GetUnalignedSize() const
{
    if (bIsUnrealStruct)
        return InfoHandle.IsValidHandle() ? InfoHandle.GetUnalignedSize() : 0x0;
    return PredefStruct->Size;
}

bool StructWrapper::ShouldUseExplicitAlignment() const
{
    if (bIsUnrealStruct)
        return InfoHandle.IsValidHandle() && InfoHandle.ShouldUseExplicitAlignment();
    return PredefStruct->bUseExplictAlignment;
}

bool StructWrapper::HasReusedTrailingPadding() const
{
    return bIsUnrealStruct && InfoHandle.IsValidHandle() && InfoHandle.HasReusedTrailingPadding();
}

bool StructWrapper::IsFinal() const
{
    if (bIsUnrealStruct)
        return InfoHandle.IsValidHandle() && InfoHandle.IsFinal();
    return PredefStruct->bIsFinal;
}

bool StructWrapper::IsClass() const
{
    return bIsUnrealStruct ? Struct.IsA(EClassCastFlags::Class) : PredefStruct->bIsClass;
}

bool StructWrapper::IsUnion() const
{
    return !bIsUnrealStruct && PredefStruct->bIsUnion;
}

bool StructWrapper::IsFunction() const
{
    return bIsUnrealStruct && Struct.IsA(EClassCastFlags::Function);
}

bool StructWrapper::IsInterface() const
{
    static UEClass InterfaceClass = ObjectArray::FindClassFast("Interface");

    return bIsUnrealStruct && Struct.IsA(EClassCastFlags::Class) && Struct.HasType(InterfaceClass);
}

bool StructWrapper::IsAClassWithType(UEClass TypeClass) const
{
    return IsUnrealStruct() && IsClass() && Struct.Cast<UEClass>().IsA(TypeClass);
}


bool StructWrapper::IsValid() const
{
    // Struct and PredefStruct share the same memory location, if Struct is nullptr so is PredefStruct
    return PredefStruct != nullptr;
}

bool StructWrapper::IsUnrealStruct() const
{
    return bIsUnrealStruct;
}

bool StructWrapper::IsCyclicWithPackage(int32 PackageIndex) const
{
    if (!bIsUnrealStruct || PackageIndex == -1)
        return false;

    if (!InfoHandle.IsValidHandle() || !InfoHandle.IsPartOfCyclicPackage())
        return false;

    return StructManager::IsStructCyclicWithPackage(Struct.GetIndex(), PackageIndex);
}

bool StructWrapper::HasCustomTemplateText() const
{
    return !IsUnrealStruct() && !PredefStruct->CustomTemplateText.empty();
}

std::string StructWrapper::GetCustomTemplateText() const
{
    assert(!IsUnrealStruct() && "StructWrapper doesn't contain PredefStruct. Illegal call to 'GetCustomTemplateText()'.");

    return PredefStruct->CustomTemplateText;
}
