# External Mode (hv-nebula) — Findings and Plan

Branch: `external-mode-hv-nebula`
HEAD: uncommitted WIP (2026-04-17)
Test target: `DoubleClutch-Win64-Shipping.exe` (UE5.1.1.0, reflection-stripped)

## 2026-04-17 diagnosis: target is multiply-hardened

Three separate problems compound on DoubleClutch:

**1. Target FField heap pages are not physically resident.** `Actor+0x50` reads a
non-zero pointer (e.g. `0x2072BCF0900`), proving Actor's own page is resident AND our
`ChildProperties` offset is correct. But reading the FField AT that pointer returns 48
bytes of zeros via the hypercall, and `hv::get_physical_address(cr3, 0x2072BCF0900)`
returns **0** — no physical mapping for that VA. `VirtualQueryEx` via an external handle
confirms the VA is `MEM_COMMIT`/`PAGE_READWRITE` with a valid `AllocationBase`, so the
target has the page committed in its VAD tree but the PTE is marked "not present". The
hypervisor's physical-memory read can't fault in paged-out pages, so every
ChildProperties → FField walk dead-ends immediately and every property-offset finder
that relies on that walk returns -1. Same pattern for Color, Vector, Guid, and every
other struct we probe.

**2. `ReadProcessMemory` is blocked at the kernel level.** Opening the process with
`PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ` succeeds (handle 0xb4 returned),
and `VirtualQueryEx` on that handle works. But `ReadProcessMemory` at any address
returns 0 bytes with `GetLastError == ERROR_ACCESS_DENIED (5)` — including on pages the
hypercall DOES read (e.g. Actor's own UStruct page at allocBase `0x20708EA00000`, where
`phys=0x14B7521B0` and the hypercall returns real layout bytes). This is the
anti-cheat (EAC presumably) hooking `NtReadVirtualMemory` in the kernel. So the "fall
back to RPM when hypercall returns zero" plan is DOA — RPM isn't available.

**3. `hv::query_process_cr3` is non-deterministic.** Same hypercall, same PID,
different invocations return different CR3 values: the dumper got `0x3519A5000`,
hv-mcp got `0x342CA5000`, on the same PID 18240 at nearly the same time. Only one of
them is "good enough" to read Actor's UStruct. `scan_process_dtb` returns 0 for this
target. The dumper now validates candidate CR3s by reading the main module's 'MZ'
signature; it lands on the right one, but the underlying hypervisor behavior is still
wrong and we don't control it from here.

### What this means for "everything should be working"

Pure-hypervisor dumping cannot read this target's FField heap because the pages aren't
resident and the anti-cheat blocks the RPM fallback. No amount of client-side
hardening will read data that doesn't have a physical backing.

Options, in order of invasiveness:

1. **Accept the limitation.** Document that stripped-reflection targets with
   EAC-class anti-cheat aren't usable in pure-hypervisor mode. Users dump warmer
   targets (other UE5 games where reflection pages are resident).

2. **Add a hypercall to hv-nebula** (separate codebase) that forces pages resident by
   issuing a kernel-side touch in the target's context — basically doing
   `MmProbeAndLockPages` or `MmMapLockedPagesSpecifyCache` via the hypervisor's access
   to the target's PEB/VAD. Invisible to anti-cheat since no user-mode syscall is
   involved. Requires a separate patch.

3. **Signature-based offset discovery.** Task #10 in the plan: pattern-scan the main
   module's disk-backed `.text` for known UE engine function prologues (`UStruct::Link`,
   `FProperty::InitializeValue`, etc.) and extract offsets from their `mov [rcx+OFF]`
   displacements. This side-steps the heap entirely — all data comes from the .text
   cache, which is disk-backed and always readable. Doesn't require any changes to
   hv-nebula. Scope is large (~15-20 offsets), but it's the only fully-external
   approach that works on hardened targets.

### Fixes landed in this WIP session (uncommitted)

- `FixupHardcodedOffsets` now scores both FField layouts via FName resolution, with
  legacy `+0x18` pointer check as primary and scoring as fallback when the page IS
  resident. Documents that both scores == 0 means pages aren't faulted in.
- `FindOffset<T>` now rejects pointer-valued probes with nullptr `.second` (fixed a
  false-discovery where `Off::UStruct::Children=0x28` on UE5 targets with stripped
  `PlayerController` reflection).
- `InitializeFUObjectItem` uses score-based probing over 256 slots instead of
  sequential validation, robust against per-page paging of the chunk.
- `InitLargeWorldCoordinateSettings` falls back to Vector struct size when X property
  isn't findable (UE5 is ≥0x18, UE4 is 0x0C).
- `InitWeakObjectPtrSettings` scans any SoftObjectProperty as fallback if LoadAsset
  UFunction is missing, then falls back to Vector-size inference.
- `FindFFieldNameOffset` bails early if the GuidChild / VectorChild page is zero-filled
  (prevents downstream NameArray crash from out-of-range ComparisonIndex).
- `FindNameOffsetForSomeClass` guards against division-by-zero when the iterator yields
  no objects (also prevents crash on paged-out targets).
- `RemoteMemory::Init` validates candidate CR3s by reading `MZ` at main module base;
  falls back to `scan_process_dtb` CR3 if the primary fails validation.
- `Off::Property::{ArrayDim,ElementSize,PropertyFlags,Offset_Internal}` now get derived
  defaults from `FField::Flags + 4` instead of staying at -1 and cascading into
  StructProperty::Struct = -1 etc.

---

## Status snapshot

End-to-end pipeline runs to completion now. All four output subfolders get created under `C:/Dumper-7/<Version>-<Game>/`:

- `CppSDK/` — partial (throws during package iteration)
- `Dumpspace/` — partial (throws)
- `IDAMappings/` — **completes cleanly** — this is the only backend that finishes without an exception on the current test target
- `Mappings/` — partial (throws)

Plus `GObjects-Dump.txt` and `GObjects-Dump-WithProperties.txt` at the root of the output folder.

Against targets whose reflection is intact (full set of base UClasses registered in GObjects) the external dumper was previously observed to produce a byte-identical SDK to the injected version in ~69 s.

---

## Commits landed in this debugging session (newest first)

| SHA | Summary |
| --- | --- |
| `3fcbdf3` | Isolate generator backends — each `Generate<T>()` in its own try/catch so one failure doesn't block the rest. |
| `5cad7b9` | UE5.1+ offset fallbacks for every UStruct / UField / FField / FFieldClass / UClass offset; derive `Children` / `ChildProperties` defaults from discovered `Size`. |
| `dc150ee` | Reject `Address < 0x1000` in `IsBadReadPtr`. Some hv implementations treat `src=0` on `read_virt_mem` as a no-op success, making `IsValid(0)` return true. |
| `89715ab` | Five hardening fixes for protected / obfuscated UE5 targets (see breakdown below). |
| `a8adfea` | Initial port of the dumper to fully external mode via hv-nebula. |

### Breakdown of `89715ab`

1. **`FindUObjectFlagsOffset` infinite loop** — the `while`-loop passed the same `Offset` back to `FindOffset`, which returns the same match when `MinOffset` already equals a matching position. Fix: advance `Offset += sizeof(int32)` between iterations.
2. **Toolhelp32Snapshot denied (ERROR_ACCESS_DENIED) on protected targets.** Switched to a PEB walk via `NtQueryInformationProcess(ProcessBasicInformation)` + hypercall reads of `Ldr->InMemoryOrderModuleList`. Toolhelp is now the fallback.
3. **Main module's `IMAGE_IMPORT_DESCRIPTOR` table zeroed at runtime.** Some UE5 shipping builds wipe the descriptor array after loader fixup; the IAT still holds resolved addresses. Added an export-directory fallback that resolves via the target DLLs' own export tables with forwarder-chain handling (kernel32!InitializeSRWLock → ntdll!RtlInitializeSRWLock).
4. **`.rdata` / `.text` pages demand-paged out of live memory.** Hypercall reads return zeros for non-resident pages, which made string-ref scans for L"ByteProperty" etc. miss and broke FNamePool discovery. Now memory-map the target exe from disk; read-only sections come from disk (byte-identical for non-packed modules); writable sections still come from memory.
5. **ObjectArray iteration perf.** 4 hypercalls/step × 113k objects was dominating wall time. Added a chunk cache (bulk-read each 1.5 MB chunk on first touch), cached `Num()`/`Max()`, and memoized `FindObjectFast` results.

---

## Test target characteristics — DoubleClutch (Rematch) UE5.1.1.0

Worth capturing because these are the non-obvious properties that broke the dumper and will likely break similarly-built UE5 shipping games:

- **Imports obfuscated.** `IMAGE_IMPORT_DESCRIPTOR` table at `module_base + RVA 0x77EE254` is zero-filled. `IMAGE_DIRECTORY_ENTRY_IAT` (directory index 12) is at RVA `0x5BD4000` and is intact with resolved function pointers.
- **`Toolhelp32Snapshot(TH32CS_SNAPMODULE, pid)` returns `ERROR_ACCESS_DENIED`.** `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` works, so PEB walking via `NtQueryInformationProcess` is reliable.
- **`hv-mcp` also can't parse the target's PE imports** (`get_pe_imports` returns 0), which confirms this isn't dumper-specific.
- **Demand-paged `.rdata`.** Many string constants (e.g. `L"ByteProperty"` at `0x7FF7EF8C7498`) are not resident at dump time — live memory reads return zeros. Disk-backed bytes match what would be there if the page were faulted in.
- **`FStructBaseChain` is inherited by UStruct.** Adds 16 bytes between UField and UStruct's own members, so layout shifts by `0x10`:
  ```
  0x28  UField::Next
  0x30  FStructBaseChain::Array       (UE5.1+ only; not in UE4)
  0x38  FStructBaseChain::NumMinusOne (+ padding)
  0x40  UStruct::SuperStruct          (0x30 in pre-5.1 layouts)
  0x48  UStruct::Children             (0x38 in pre-5.1 layouts)
  0x50  UStruct::ChildProperties      (0x40 in pre-5.1 layouts)
  0x58  UStruct::PropertiesSize       (discoverable via Color/Guid size match)
  0x5C  UStruct::MinAlignment
  ```
- **GObjects is populated**, 113391 total slots, ~106444 non-null, 2 chunks of 65536 items each at 24 bytes per `FUObjectItem`. Layout matches `FChunkedFixedUObjectArrayLayouts[0]`.
- **GNames (FNamePool) is populated** and discoverable via the existing LEA+call → ctor → InitSRWLock + "ByteProperty" pattern once disk-backed .rdata is in place.
- **Reflection is partially stripped.** Base UClasses that ARE registered in this game's GObjects:
  `Object`, `Actor`, `Pawn`, `Character`, `Class`, `Function`, `ScriptStruct`, `Color`, `Vector`, `Vector2D`, `Vector4`, `Guid`, `Transform`, `TwoVectors`, `World`, `Level`, `ActorComponent`, `Interface_AssetUserData`, `Engine`.
  Base UClasses that AREN'T registered:
  `Struct`, `Field`, `PlayerController`, `Controller`, `KismetSystemLibrary`, `KismetStringLibrary`, `GameMode`, `LocalPlayer`, `DefaultPawn`, most Default__* objects.
  UFunctions referencing the missing classes do exist (e.g. `GetController`, `GetLocalViewingPlayerController`), just not their owning classes.

---

## Remaining errors (blocking full SDK generation)

### Error 1 — `invalid unordered_map<K, T> key`

Thrown during `Generate<CppGenerator>`, `Generate<MappingGenerator>`, and `Generate<DumpspaceGenerator>`. Caught by `3fcbdf3`'s per-generator try/catch, but kills each backend mid-work.

**Not yet diagnosed.** Needs to be traced next. Plausible sources:

- `StructManager` / `EnumManager` / `CollisionManager` / `MemberManager` build `std::unordered_map` keyed by object addresses, UStruct indices, or name-hashes. When some UStruct's offsets default to fallback values and `GetChild()` / `GetChildProperties()` returns a garbage-but-nonzero handle, it can feed the map a key that hashes inconsistently (e.g. two different accesses produce different hashes because backing memory gets re-read differently), or a key that's rejected by the map's debug validation.
- MSVC's `unordered_map` iterator debug check (`_ITERATOR_DEBUG_LEVEL`) raises "invalid unordered_map<K, T> key" when you try `operator[]` on a key that's inside an erase-marked bucket, or when a container got into an inconsistent state from a bad hash.
- Could also be a `const char*` / string_view keyed map where the backing string buffer went out of scope.

**First investigation step**: run under a debugger and break on the exception throw, capture the stack trace and the map's key/value types from the `xhash` frame. Alternatively, recompile with `_ITERATOR_DEBUG_LEVEL=0` and see if it still throws (if it does, it's a real logic error; if not, it's a debug-mode-only validation).

### Error 2 — `'LoadAsset' wasn't found`

Message: `"Dumper-7: 'LoadAsset' wasn't found, could not determine value for 'bIsWeakObjectPtrWithoutTag'!"`

`InitSettings` probes for `LoadAsset`, a UFunction on `UObject`. This game's `Object` UClass might not expose `LoadAsset` as a direct member — it might be on a derived class, or have been stripped from the shipping build's reflection. The flag defaults false when not found; consequences of the wrong default are a wrong `TWeakObjectPtr` size emitted in the generated SDK.

**Fix direction**: try alternative probes. `TWeakObjectPtr` size can be inferred from any object that has a `TWeakObjectPtr<UObject>` member — scan for such properties. Or fall back to a hardcoded UE version map (`bIsWeakObjectPtrWithoutTag` is usually true on UE5.1+).

### Error 3 — `'Something went horribly wrong, FVector::X wasn't even found!'`

This is shouted by `InitSettings` when it can't locate `FVector::X` as a property. Possible because the scan iterates UProperty chains which depend on `Off::UStruct::ChildProperties` and `Off::FField::Next` — if either is slightly off for this game's layout, the walk breaks early before reaching X.

**Fix direction**: verify ChildProperties and FField::Next against the actual `Vector` struct layout via disassembly / IDA. The defaults we're using (`0x50` / `0x20`) were derived from `PropertiesSize=0x58` but may not account for all UE5.1 layout variants.

### Error 4 — Per-run variability

Between runs against the same PID the game is sometimes restarted (different CR3) and the discovery output differs — pattern hit counts for FNamePool fluctuate (12365 vs 27773 vs 58908), some UClasses appear in one run and not another. Not a dumper bug per se but makes debugging noisy.

**Mitigation**: run against a paused / idle game (main menu), confirm CR3 stability via `hv-mcp` before each run.

---

## Fundamental limitation — stripped reflection

Most of the `OffsetFinder::Find*Offset` functions rely on a specific named UClass being present in GObjects (`Guid`, `PlayerController`, `KismetSystemLibrary`, `DataTable`, etc.). When those are stripped, the finders return `OffsetNotFound` and the fallback defaults take over. The fallbacks are correct for UE5.1+ standard layouts but drift for any game that changed property / field layout internally.

**Long-term fix**: **signature-based offset discovery.** Pattern-scan the target's `.text` for known UE engine function prologues (e.g. `UStruct::Link`, `UObjectBase::CreateStatID`, `FProperty::InitializeValue`, `UClass::FindFunctionByName`) and extract the offsets directly from `mov reg, [rcx+OFFSET]` displacements in the disassembly. This doesn't depend on named UClasses being present in GObjects.

Scope: roughly 15-20 offsets on the hot path, each needs one or two pattern-scans and a small disassembly walk. The `Architecture_x86_64` disassembler is already in the codebase. Tracked as **task #10**.

---

## Plan for next session

Priority-ordered:

1. **Diagnose the `invalid unordered_map<K, T> key` throw.**
   - Rebuild `Debug|x64`, set `_ITERATOR_DEBUG_LEVEL=2`, run under Visual Studio, break on throw, capture stack.
   - Confirm whether it's a real bad key or an iterator-debug false positive.
   - If real: fix the caller; if false positive: set `_ITERATOR_DEBUG_LEVEL=0` for Release builds.

2. **Validate ChildProperties / FField::Next defaults against the target's actual Vector struct layout.**
   - Read Vector's raw bytes at the discovered UStruct address (via a one-off diagnostic or IDA) and locate the real ChildProperties pointer offset. Adjust the derived default if `Size - 8` doesn't match.

3. **Harden `InitSettings` probes** (LoadAsset, FVector::X, etc.) against stripped reflection. Swap name-probes for signature-scans.

4. **Begin sig-based discovery (task #10).** Start with `Off::UStruct::Children` and `Off::UStruct::ChildProperties` — pattern-scan `UStruct::Link` or `UStruct::SerializeProperties` and extract from the first `mov [rcx+OFFSET], rax` displacement. One offset at a time, each with before/after parity check.

5. **Clean up diagnostic prints.** Once the above stabilize, strip the `[InitEngineCore]` / `[FindObjectFast]` / `[Platform]` markers and the `FixupHardcodedOffsets` probe dumps so the output is readable again.

6. **Parity run against a non-stripped UE5 target** to confirm none of the hardening fixes regressed the happy path. The previous ~69 s Rematch run is gone (game state has shifted), but any public UE5 demo game should do.

---

## Files changed (from `main` to `3fcbdf3`)

```
.gitignore                                         |  +1
CLAUDE.md                                          |  new
CMakeLists.txt                                     |  modified
xmake.lua                                          |  modified
Dumper/Dumper.vcxproj                              |  DLL -> EXE, MASM, /EHa
Dumper/Dumper.sln                                  |  (possibly, depending on sln)
Dumper/main.cpp                                    |  rewritten as standalone exe
Dumper/Memory/                                     |  new subtree (hv.h/asm, HvProbe, RemoteMemory, RemoteContainers)
Dumper/Platform/Private/PlatformWindows.cpp        |  rewritten for remote reads, PEB walk, disk cache, export fallback
Dumper/Platform/Private/Arch_x86.cpp               |  RDeref replacements
Dumper/Generator/Private/Generators/Generator.cpp  |  stage markers, external-mode game name/version
Dumper/Engine/Private/Unreal/ObjectArray.cpp       |  chunk cache, cached Num()/Max(), FindObjectFast memo
Dumper/Engine/Private/Unreal/NameArray.cpp         |  skip TryFindNameArray_Windows, keep FNamePool only
Dumper/Engine/Private/Unreal/UnrealTypes.cpp       |  bForceGNames unconditional
Dumper/Engine/Private/Unreal/UnrealObjects.cpp     |  RDeref replacements; ProcessEvent neutered
Dumper/Engine/Private/OffsetFinder/OffsetFinder.cpp|  FindUObjectFlagsOffset advance; FindFFieldNameOffset bailout
Dumper/Engine/Private/OffsetFinder/Offsets.cpp     |  offset fallbacks (OverwriteIfInvalidOffset) on every finder
```

Full per-commit diff: `git log --stat main..HEAD` on the branch.
