#include "Wrappers/EnumWrapper.h"
#include "Managers/EnumManager.h"

EnumWrapper::EnumWrapper(const UEEnum Enm)
    : Enum(Enm), InfoHandle(EnumManager::GetInfo(Enm))
{
}

UEEnum EnumWrapper::GetUnrealEnum() const
{
    return Enum;
}

std::string EnumWrapper::GetName() const
{
    return Enum.GetEnumPrefixedName();
}

std::string EnumWrapper::GetRawName() const
{
    return Enum.GetName();
}

std::string EnumWrapper::GetFullName() const
{
    return Enum.GetFullName();
}

std::pair<std::string, bool> EnumWrapper::GetUniqueName() const
{
    // Defensive: EnumManager::GetInfo can return a null handle for enums it didn't track,
    // which happens on stripped-reflection targets. Fall back to the UEEnum's own name so
    // callers don't crash inside format strings when emitting the generated SDK.
    if (!InfoHandle.IsValidHandle())
        return { Enum ? Enum.GetEnumPrefixedName() : std::string("UnknownEnum"), true };

    const StringEntry& Name = InfoHandle.GetName();

    return { Name.GetName(), Name.IsUnique() };
}

uint8 EnumWrapper::GetUnderlyingTypeSize() const
{
    return InfoHandle.IsValidHandle() ? InfoHandle.GetUnderlyingTypeSize() : uint8(sizeof(uint8));
}

int32 EnumWrapper::GetNumMembers() const
{
    return InfoHandle.IsValidHandle() ? InfoHandle.GetNumMembers() : 0;
}

CollisionInfoIterator EnumWrapper::GetMembers() const
{
    static const std::vector<EnumCollisionInfo> s_Empty;
    return InfoHandle.IsValidHandle() ? InfoHandle.GetMemberCollisionInfoIterator() : CollisionInfoIterator(s_Empty);
}

bool EnumWrapper::IsValid() const
{
    return Enum != nullptr;
}

