#include "Settings.h"

#include <Windows.h>
#include <filesystem>
#include <string>

#include "Unreal/UnrealObjects.h"
#include "Unreal/ObjectArray.h"

void Settings::InitWeakObjectPtrSettings()
{
	constexpr int32 SizeOfFFWeakObjectPtr = 0x08;
	constexpr int32 OldUnrealAssetPtrSize = 0x10;

	const UEStruct SoftObjectPath = ObjectArray::FindStructFast("SoftObjectPath");
	const int32 SizeOfSoftObjectPath = SoftObjectPath ? SoftObjectPath.GetStructSize() : OldUnrealAssetPtrSize;

	// Primary probe: LoadAsset.Asset (a SoftObjectProperty). Its size tells us whether the
	// TWeakObjectPtr tail-tag is present — if the property size fits inside
	// SoftObjectPath + FWeakObjectPtr alone, the tag is gone (UE5.0+).
	const UEStruct LoadAsset = ObjectArray::FindObjectFast<UEFunction>("LoadAsset", EClassCastFlags::Function);
	if (LoadAsset)
	{
		if (const UEProperty Asset = LoadAsset.FindMember("Asset", EClassCastFlags::SoftObjectProperty))
		{
			Settings::Internal::bIsWeakObjectPtrWithoutTag = Asset.GetSize() <= (SizeOfSoftObjectPath + SizeOfFFWeakObjectPtr);
			return;
		}
	}

	// Fallback: scan every UStruct for any SoftObjectProperty and use the first one's size.
	// LoadAsset is sometimes stripped from shipping-build reflection; any SoftObjectProperty
	// of the right type will do, because the tail-tag presence is a per-build compile-time
	// choice, not a per-property one.
	for (const UEObject Obj : ObjectArray())
	{
		if (!Obj.IsA(EClassCastFlags::Struct))
			continue;

		for (const UEProperty P : Obj.Cast<UEStruct>().GetProperties())
		{
			if (!P.IsA(EClassCastFlags::SoftObjectProperty))
				continue;
			Settings::Internal::bIsWeakObjectPtrWithoutTag = P.GetSize() <= (SizeOfSoftObjectPath + SizeOfFFWeakObjectPtr);
			std::cerr << std::format(
				"\nDumper-7: 'LoadAsset' not found; inferred bIsWeakObjectPtrWithoutTag = {} from {}::{} (size 0x{:X})\n\n",
				Settings::Internal::bIsWeakObjectPtrWithoutTag,
				Obj.GetName(), P.GetName(), P.GetSize());
			return;
		}
	}

	// Last-resort default: for any target where the size of FVector is 0x18 (three doubles,
	// UE5+ LWC) the WeakObjectPtr tag is nearly always absent.
	const UEStruct VectorStruct = ObjectArray::FindStructFast("Vector");
	if (VectorStruct && VectorStruct.GetStructSize() >= 0x18)
	{
		Settings::Internal::bIsWeakObjectPtrWithoutTag = true;
		std::cerr << "\nDumper-7: 'LoadAsset' not found; inferred bIsWeakObjectPtrWithoutTag = true from UE5-sized FVector\n\n" << std::endl;
		return;
	}

	std::cerr << "\nDumper-7: 'LoadAsset' wasn't found and no SoftObjectProperty fallback worked; bIsWeakObjectPtrWithoutTag stays at default\n" << std::endl;
}

void Settings::InitLargeWorldCoordinateSettings()
{
	const UEStruct FVectorStruct = ObjectArray::FindStructFast("Vector");

	if (!FVectorStruct) [[unlikely]]
	{
		std::cerr << "\nSomething went horribly wrong, FVector wasn't even found!\n\n" << std::endl;
		return;
	}

	// Try uppercase X first, then lowercase x (some UE5 builds use lowercase).
	UEProperty XProperty = FVectorStruct.FindMember("X");
	if (!XProperty)
		XProperty = FVectorStruct.FindMember("x");

	if (XProperty)
	{
		/* Check the underlaying type of FVector::X. If it's double we're on UE5.0, or higher, and using large world coordinates. */
		Settings::Internal::bUseLargeWorldCoordinates = XProperty.IsA(EClassCastFlags::DoubleProperty);
		return;
	}

	// Struct-size fallback: UE5 LWC-FVector is 0x18 (3 doubles), UE4 FVector is 0x0C (3 floats).
	// This works even when the FField walk is broken by stripped/paged-out ChildProperties.
	const int32 VectorSize = FVectorStruct.GetStructSize();
	Settings::Internal::bUseLargeWorldCoordinates = VectorSize >= 0x18;

	std::cerr << std::format(
		"\nDumper-7: 'FVector::X' not found; inferred bUseLargeWorldCoordinates = {} from FVector size 0x{:X}\n\n",
		Settings::Internal::bUseLargeWorldCoordinates, VectorSize);
}

void Settings::InitObjectPtrPropertySettings()
{
	const UEClass ObjectPtrPropertyClass = ObjectArray::FindClassFast("ObjectPtrProperty");

	if (!ObjectPtrPropertyClass)
	{
		// The class doesn't exist, this so FieldPathProperty couldn't have been replaced with ObjectPtrProperty
		std::cerr << std::format("\nDumper-7: bIsObjPtrInsteadOfFieldPathProperty = {}\n", Settings::Internal::bIsObjPtrInsteadOfFieldPathProperty) << std::endl;
		Settings::Internal::bIsObjPtrInsteadOfFieldPathProperty = false;
		return;
	}

	Settings::Internal::bIsObjPtrInsteadOfFieldPathProperty = ObjectPtrPropertyClass.GetDefaultObject().IsA(EClassCastFlags::FieldPathProperty);

	std::cerr << std::format("\nDumper-7: bIsObjPtrInsteadOfFieldPathProperty = {}\n", Settings::Internal::bIsObjPtrInsteadOfFieldPathProperty) << std::endl;
}

void Settings::InitArrayDimSizeSettings()
{
	/*
	 * UEProperty::GetArrayDim() is already fully functional at this point.
	 *
	 * This setting is just there to stop it from returning (int32)0xFFFFFF01 when it should be just (uint8)0x01.
	*/
	for (const UEObject Obj : ObjectArray())
	{
		if (!Obj.IsA(EClassCastFlags::Struct))
			continue;

		const UEStruct AsStruct = Obj.Cast<UEStruct>();

		for (const UEProperty Property : AsStruct.GetProperties())
		{
			// This number should just be 0x1 to indicate it's a single element, but the upper bytes aren't cleared to zero
			if (Property.GetArrayDim() >= 0x000F0001)
			{
				Settings::Internal::bUseUint8ArrayDim = true;
				std::cerr << std::format("\nDumper-7: bUseUint8ArrayDim = {}\n", Settings::Internal::bUseUint8ArrayDim) << std::endl;
				return;
			}
		}
	}

	Settings::Internal::bUseUint8ArrayDim = false;
	std::cerr << std::format("\nDumper-7: bUseUint8ArrayDim = {}\n", Settings::Internal::bUseUint8ArrayDim) << std::endl;
}

void Settings::Config::Load()
{
	namespace fs = std::filesystem;

	// Try local Dumper-7.ini
	const std::string LocalPath = (fs::current_path() / "Dumper-7.ini").string();
	const char* ConfigPath = nullptr;

	if (fs::exists(LocalPath)) 
	{
		ConfigPath = LocalPath.c_str();
	}
	else if (fs::exists(GlobalConfigPath)) // Try global path
	{
		ConfigPath = GlobalConfigPath;
	}

	// If no config found, use defaults
	if (!ConfigPath) 
		return;

	char SDKNamespace[256] = {};
	GetPrivateProfileStringA("Settings", "SDKNamespaceName", "SDK", SDKNamespace, sizeof(SDKNamespace), ConfigPath);

	SDKNamespaceName = SDKNamespace;
	SleepTimeout = max(GetPrivateProfileIntA("Settings", "SleepTimeout", 0, ConfigPath), 0);
}
