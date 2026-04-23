# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Dumper-7 is an SDK generator for any Unreal Engine game (UE4 + UE5). It builds as a Windows DLL that the user injects into a running game process; once loaded, `MainThread` (`Dumper/main.cpp`) walks the game's `GObjects` / `GNames`, finds all the relevant offsets at runtime, and writes a generated SDK to disk. By default the output goes to `C:/Dumper-7/{GameVersion}-{GameName}/`.

The dumper is invasive — it reads live process memory, scans modules with byte/string patterns, parses x86 instructions, and depends on engine-version-specific assumptions. Most "bug fixes" here are in fact game-specific edge cases.

## Build

The project is x64-only and Windows-only. C++20 is required and `/arch:AVX2` is enabled (xorstr intrinsics). Three build systems are kept in sync — pick whichever the user is using:

- **MSBuild / Visual Studio (canonical)** — `Dumper-7.sln`. The CI uses this:
  ```
  msbuild .\Dumper\Dumper.vcxproj /p:Configuration=Release /p:Platform=x64 /p:WarningLevel=0
  ```
  Configurations: `Debug`, `Release`, `Debug-Clang`, `Release-Clang` × `x64` (Win32 also exists in the project files but is not a supported target). Ship builds use `Release|x64`. The DLL lands in `Dumper/x64/Release/Dumper-7.dll`.

- **CMake** — `CMakeLists.txt` + `CMakePresets.json` (presets: `vs2022`, `vs2019`, `vs2017`, `clang`). See `UsingCMake.md`.

- **xmake** — `xmake.lua`. See `Xmake.md`.

There is **no test runner**. The `SDKTest` project referenced by `Dumper-7.sln` is not present in this checkout, and `friend class GeneratorTest` / `friend class CppGeneratorTest` declarations exist for a test harness that lives outside the repo. Don't try to run tests; verification happens by injecting into a real game.

## Run

The DLL has no CLI — it runs from `DllMain` → `MainThread`. To exercise it: build, inject into a UE game, watch the console (output goes to **`stderr`**, not stdout), and check `C:/Dumper-7/`. Press **F6** in the game to unload the DLL cleanly.

Per-run config can be put in `Dumper-7.ini` (next to the game exe for per-game, or `C:/Dumper-7/Dumper-7.ini` global). Only `SleepTimeout` and `SDKNamespaceName` are runtime-configurable; everything else is in `Dumper/Settings.h` and requires a rebuild.

### Two operating modes

- **`main` branch (DLL injection)** — described above. `Dumper-7.dll` is injected into the target; runs from `DllMain` → `MainThread`.
- **`external-mode-hv-nebula` branch (external binary + hypervisor)** — `Dumper-7.exe` is a standalone CLI that reads the target's memory via `hv-nebula` hypervisor hypercalls instead of injecting. Useful for anti-cheat-protected targets where DLL injection or `ReadProcessMemory` are blocked at the kernel level. Run with `Dumper-7.exe --process <name.exe>` or `--pid <N>`; requires the `hv-nebula` driver loaded on the host (see the `reference_sibling_projects` memory for hv-nebula/hv-mcp locations and the edit → rebuild driver → reload HV → rebuild MCP → rebuild Dumper-7 workflow).

## Architecture

The dumper has a clean four-layer split. Headers live under `<Layer>/Public/...`, implementations under `<Layer>/Private/...`, and `Dumper/` is added as a root include directory so `#include "Unreal/ObjectArray.h"` works from anywhere.

### Engine — reading the live process (`Dumper/Engine/`)
Everything that touches game memory.
- **`Unreal/ObjectArray`** — wraps `GObjects`. Supports both `FFixedUObjectArray` (UE4.11–4.20) and `FChunkedFixedUObjectArray` (UE4.21+). New game-specific GObjects layouts go in the `FChunkedFixedUObjectArrayLayouts` array near the top of `ObjectArray.cpp`. Decryption hooks (`InitObjectArrayDecryption`) are how encrypted-pointer games like Back4Blood are supported.
- **`Unreal/NameArray`** — wraps `GNames`. Two implementations: legacy `TNameEntryArray` and modern `FNamePool`.
- **`Unreal/UnrealObjects`, `UnrealTypes`, `Enums`** — `UEObject` / `UEStruct` / `UEClass` / `UEFunction` / `UEProperty` / `UEFField` / `FName` etc. These are *thin handles around raw memory addresses* — they store a `uint8*` and read fields via runtime-discovered offsets. Treat them as cheap value types.
- **`OffsetFinder/OffsetFinder` + `Offsets`** — pattern-matches the live process to discover the offsets every other layer reads from. The discovered offsets land in the `Off::` namespace (`Off::UObject::Name`, `Off::UStruct::Children`, etc.); a few `Off::InSDK::*` values are *also* re-emitted into the generated SDK so the user's project can use them at runtime.

### Generator — producing output (`Dumper/Generator/`)
There are four generator backends, all run in sequence from `main.cpp`:

| Generator | Output | Folder |
|---|---|---|
| `CppGenerator` | C++ headers (the actual SDK) | `CppSDK/SDK/` |
| `MappingGenerator` | `.usmap` mapping file | `Mappings/` |
| `IDAMappingGenerator` | IDA scripts for symbol/struct import | `IDAMappings/` |
| `DumpspaceGenerator` | dumpspace.dev–compatible JSON | `Dumpspace/` |

Each backend satisfies the `GeneratorImplementation` concept (`Generator.h`): static `PredefinedMembers`, `MainFolderName`, `SubfolderName`, `MainFolder`, `Subfolder`, plus `Generate()` / `InitPredefinedMembers()` / `InitPredefinedFunctions()`. `Generator::Generate<T>()` is the templated entry point that wires it all together — to add a new backend, create a class that satisfies the concept and call `Generator::Generate<NewBackend>()` from `main.cpp`.

The Generator layer runs on top of Engine via two indirections that exist for good reasons:
- **`Managers/`** — `PackageManager`, `StructManager`, `EnumManager`, `MemberManager`, `CollisionManager`, `DependencyManager`. These pre-process the entire ObjectArray once: building package graphs, detecting inheritance chains, finding member-name collisions across the inheritance hierarchy, and computing cyclic-dependency clusters. Generators read from the managers, never directly from `ObjectArray`. Order of init in `Generator::InitInternal()` matters: `PackageManager::Init` → `StructManager::Init` → `EnumManager::Init` → `MemberManager::Init` → `PackageManager::PostInit` (PostInit handles cycle detection and depends on StructManager).
- **`Wrappers/`** — `StructWrapper`, `EnumWrapper`, `PropertyWrapper`, `FunctionWrapper`. A wrapper holds *either* a `UEStruct` from the game **or** a `PredefinedStruct` from `PredefinedMembers.h`, exposing one interface to the generators. This is how the C++ generator inserts hand-written types (`FVector`, `TArray`, `FString`…) alongside reflected ones.

`PredefinedMembers.h` is where you add hand-written members, functions, or whole structs that should appear in the generated SDK. Each generator backend has its own `static PredefinedMemberLookupMapType PredefinedMembers` and its own `InitPredefinedMembers()` / `InitPredefinedFunctions()` — the C++ generator's is by far the largest.

### Platform — OS/architecture abstraction (`Dumper/Platform/`)
Currently Windows-only. The Linux/Android paths in `Architecture.h` and `Platform.h` `#error` out. Platform-specific entry points use the `_Windows` postfix and are dispatched through `CALL_PLATFORM_SPECIFIC_FUNCTION(...)`. `Arch_x86.cpp` does the actual disassembly used by `OffsetFinder` to walk call sites and find vtable indices.

### Memory — external-process access (`Dumper/Memory/`, external-mode branch only)
`RemoteMemory` is the abstraction every upper layer reads through: no-op identity when running as an injected DLL, and `hv-nebula` VMCALL reads when running as an external binary. `RemoteContainers` mirrors `TArray` / `FString` / `TMap` / `TSet` with remote-read semantics so Engine-layer code can dereference remote pointers without open-coding hypercalls. `HvProbe` detects whether the hypervisor is loaded before any VMCALL. `hv.h` / `hv.asm` are vendored copies of the hv-nebula user-mode binding — re-vendor them whenever the hypercall surface (hypercall numbers, argument layout) changes on the hv-nebula side.

### Utils — leaf-level helpers (`Dumper/Utils/`)
`Utils.h` (string/byte helpers), `Json/json.hpp` (nlohmann), `Compression/zstd.h` (header-only zstd, used by `MappingGenerator`), `Encoding/UnicodeNames.h`, `Dumpspace/DSGen.cpp` (vendored dumpspace serializer).

## Initialization order — don't reorder casually

The startup sequence in `Generator.cpp` has hard ordering constraints. From `main.cpp`:

1. `Settings::Config::Load()` — read `.ini` if present
2. `Generator::InitEngineCore()` — `ObjectArray::Init()` → `FName::Init` → `Off::Init()` → `PropertySizes::Init()` → `Off::InSDK::ProcessEvent::InitPE_Windows()` → `Off::InSDK::World::InitGWorld()` → `Off::InSDK::Text::InitTextOffsets()` → `InitSettings()`. The PE/GWorld/Text inits depend on offsets from `Off::Init()`; `InitSettings` walks `ObjectArray` to detect engine-version-dependent flags (`bUseLargeWorldCoordinates`, `bIsWeakObjectPtrWithoutTag`, `bIsObjPtrInsteadOfFieldPathProperty`, `bUseUint8ArrayDim`).
3. Game name/version are auto-discovered by calling `KismetSystemLibrary::GetGameName` / `GetEngineVersion` *via the dumper itself* (`UEClass::ProcessEvent`) — i.e. the dumper reflectively calls into the live game to name its own output folder.
4. `Generator::InitInternal()` — see Manager init order above.
5. `Generator::Generate<T>()` for each backend.

## Per-game overrides

When a game's `GObjects` / `GNames` / `AppendString` / `ProcessEvent` / `GObjects` decryption isn't auto-discovered, the user is expected to edit `Generator::InitEngineCore()` directly and call the explicit `::Init(...)` overloads (commented examples are at the top of the function). New `FChunkedFixedUObjectArrayLayout` shapes go in the `FChunkedFixedUObjectArrayLayouts` array in `ObjectArray.cpp`. This is the documented workflow — see the `Overriding Offsets` and `Overriding GObjects-Layout` sections in `README.md`. Don't refactor these into config: they're per-game and need to be in source.

**Target-specific findings (live addresses, chunk layouts, engine-version quirks, paging behaviour, anti-cheat fingerprint) live in per-target memory files** under `~/.claude/projects/.../memory/project_<target>_target.md`, NOT in CLAUDE.md and NOT in source comments. CLAUDE.md describes what is structurally true about the project across every target; memory files capture what was true for a specific game at a specific session. Chunk counts, VAs, RVAs, and discovered offsets rot across game patches — never inline them in CLAUDE.md or in comments in source files.

## Other notes

- Engine version flags live in `Settings::Internal::*` and are set by `InitSettings()` at runtime — *not* compile-time. Code that branches on engine version reads these flags.
- Output stream is `stderr`. `stdout` is unused.
- The build defines the project as `Dumper-7` (not `Dumper`); the produced DLL is `Dumper-7.dll`.
- `Settings.h` knobs like `bForceNoGWorldInSDK`, `bAddManualOverrideOptions`, `XORString` change the *generated SDK*, not the dumper itself — they only matter at SDK-emit time.
- See `UsingTheSDK.md` for what the *consumer* of the generated SDK does (it's a separate VS project; not built from this repo).
- `EXTERNAL_MODE_FINDINGS.md` (repo root) is a living session-diagnostic file for the external-mode branch. Append new diagnoses and dead-ends there rather than starting from scratch — it's the historical record of what has and hasn't worked across targets.
