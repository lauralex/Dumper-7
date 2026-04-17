#pragma once

#include <filesystem>
#include <iostream>

#include "Unreal/ObjectArray.h"
#include "Managers/DependencyManager.h"
#include "Managers/MemberManager.h"
#include "HashStringTable.h"


namespace fs = std::filesystem;

void DumpEditorOnlyMetadata(const fs::path& DumperFolder);

template<typename GeneratorType>
concept GeneratorImplementation = requires(GeneratorType t)
{
    /* Require static variables of type */
    GeneratorType::PredefinedMembers;
    requires(std::same_as<decltype(GeneratorType::PredefinedMembers), PredefinedMemberLookupMapType>);

    GeneratorType::MainFolderName;
    requires(std::same_as<decltype(GeneratorType::MainFolderName), std::string>);
    GeneratorType::SubfolderName;
    requires(std::same_as<decltype(GeneratorType::SubfolderName), std::string>);

    GeneratorType::MainFolder;
    requires(std::same_as<decltype(GeneratorType::MainFolder), fs::path>);
    GeneratorType::Subfolder;
    requires(std::same_as<decltype(GeneratorType::Subfolder), fs::path>);
    
    /* Require static functions */
    GeneratorType::Generate();

    GeneratorType::InitPredefinedMembers();
    GeneratorType::InitPredefinedFunctions();
};

class Generator
{
private:
    friend class GeneratorTest;

private:
    static inline fs::path DumperFolder;
    static inline bool bDumpedGObjects = false;
	static inline bool bDumepdEditorOnlyMetadata = false;

public:
    static void InitEngineCore();
    static void InitInternal();

private:
    static bool SetupDumperFolder();

    static bool SetupFolders(std::string& FolderName, fs::path& OutFolder);
    static bool SetupFolders(std::string& FolderName, fs::path& OutFolder, std::string& SubfolderName, fs::path& OutSubFolder);

public:
    template<GeneratorImplementation GeneratorType>
    static void Generate()
    {
        if (DumperFolder.empty())
        {
            if (!SetupDumperFolder())
                return;

            if (!bDumpedGObjects)
            {
                bDumpedGObjects = true;
                ObjectArray::DumpObjects(DumperFolder);

                // DumpObjectsWithProperties walks ChildProperties (FField) chains for every
                // struct and emits a formatted line per property. On stripped-reflection
                // targets accessed via hypercalls, each read is remote-dispatched, and
                // iterating the full chain across ~117 k objects turns into tens of millions
                // of hypercalls plus gigabytes of std::format temporaries. It's a diagnostic
                // aid, not a dump output — skip when Settings::Debug::bSkipPropertyDump.
                if (Settings::Internal::bUseFProperty && !Settings::Debug::bSkipPropertyDump)
                    ObjectArray::DumpObjectsWithProperties(DumperFolder);
            }

            if (!bDumepdEditorOnlyMetadata)
            {
                bDumepdEditorOnlyMetadata = true;
                DumpEditorOnlyMetadata(DumperFolder);
            }
        }

        if (!SetupFolders(GeneratorType::MainFolderName, GeneratorType::MainFolder, GeneratorType::SubfolderName, GeneratorType::Subfolder))
            return;

        GeneratorType::InitPredefinedMembers();
        GeneratorType::InitPredefinedFunctions();

        MemberManager::SetPredefinedMemberLookupPtr(&GeneratorType::PredefinedMembers);

        GeneratorType::Generate();
    };
};
