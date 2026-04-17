#include <Windows.h>
#include <TlHelp32.h>
#include <iostream>
#include <string>
#include <string_view>
#include <chrono>
#include <filesystem>
#include <vector>

#pragma comment(lib, "Version.lib")

#include "HvProbe.h"
#include "RemoteMemory.h"
#include "Settings.h"

#include "Generators/CppGenerator.h"
#include "Generators/MappingGenerator.h"
#include "Generators/IDAMappingGenerator.h"
#include "Generators/DumpspaceGenerator.h"
#include "Generators/Generator.h"

namespace
{
	DWORD FindProcessByName(const std::wstring& name)
	{
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE)
			return 0;

		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);

		DWORD result = 0;
		if (Process32FirstW(snap, &entry))
		{
			do
			{
				if (_wcsicmp(entry.szExeFile, name.c_str()) == 0)
				{
					result = entry.th32ProcessID;
					break;
				}
			} while (Process32NextW(snap, &entry));
		}
		CloseHandle(snap);
		return result;
	}

	std::wstring Utf8ToWide(std::string_view s)
	{
		if (s.empty())
			return {};
		const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
		std::wstring w(len, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
		return w;
	}

	DWORD ResolveTargetPid(int argc, char** argv)
	{
		for (int i = 1; i < argc; ++i)
		{
			std::string_view arg = argv[i];
			if (arg == "--pid" && i + 1 < argc)
			{
				try { return static_cast<DWORD>(std::stoul(argv[++i])); }
				catch (...) { std::cerr << "Invalid --pid value\n"; return 0; }
			}
			if (arg == "--process" && i + 1 < argc)
			{
				const DWORD pid = FindProcessByName(Utf8ToWide(argv[++i]));
				if (!pid)
					std::cerr << "Process '" << argv[i] << "' not found\n";
				return pid;
			}
		}

		// TODO: fall back to TargetProcessName/TargetPid from Dumper-7.ini once wired through Settings::Config.
		std::cerr << "Usage: Dumper-7.exe --pid <N> | --process <name.exe>\n";
		return 0;
	}
}

namespace
{
	// Resolve the target process's executable path via OpenProcess + QueryFullProcessImageNameW.
	// Uses PROCESS_QUERY_LIMITED_INFORMATION which does not require VM_READ and is normally
	// permitted against protected targets without raising anti-cheat flags.
	std::wstring GetTargetExePath(DWORD pid)
	{
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!h)
			return {};
		wchar_t buf[MAX_PATH]{};
		DWORD size = MAX_PATH;
		const BOOL ok = QueryFullProcessImageNameW(h, 0, buf, &size);
		CloseHandle(h);
		if (!ok)
			return {};
		return std::wstring(buf, size);
	}

	std::string WideToUtf8(const std::wstring& w)
	{
		if (w.empty())
			return {};
		const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
		std::string out(len, '\0');
		WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
		return out;
	}

	// Read VS_FIXEDFILEINFO from the target exe on disk and return "Major.Minor.Build.Revision".
	std::string GetExeVersionString(const std::wstring& exePath)
	{
		if (exePath.empty())
			return {};
		const DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, exePath.c_str(), nullptr);
		if (size == 0)
			return {};
		std::vector<uint8_t> buffer(size);
		if (!GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, exePath.c_str(), 0, size, buffer.data()))
			return {};
		VS_FIXEDFILEINFO* info = nullptr;
		UINT infoSize = 0;
		if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &infoSize) || !info)
			return {};
		return std::format("{}.{}.{}.{}",
			HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
			HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
	}

	void ResolveGameNameAndVersionExternally(DWORD pid)
	{
		const std::wstring exePathW = GetTargetExePath(pid);
		if (exePathW.empty())
		{
			Settings::Generator::GameName = Settings::Generator::GameName.empty() ? "UnknownGame" : Settings::Generator::GameName;
			Settings::Generator::GameVersion = Settings::Generator::GameVersion.empty() ? "UnknownVersion" : Settings::Generator::GameVersion;
			return;
		}

		const std::filesystem::path exePath(exePathW);
		if (Settings::Generator::GameName.empty())
		{
			Settings::Generator::GameName = WideToUtf8(exePath.stem().wstring());
		}
		if (Settings::Generator::GameVersion.empty())
		{
			const std::string ver = GetExeVersionString(exePathW);
			Settings::Generator::GameVersion = ver.empty() ? "UnknownVersion" : ver;
		}
	}
}

int main(int argc, char** argv)
{
	// Disable stderr buffering so redirects to files flush immediately — otherwise when the
	// dumper gets killed for hitting a memory cap, all unflushed trace output is lost and
	// debugging is blind.
	std::setvbuf(stderr, nullptr, _IONBF, 0);

	// Enable low-fragmentation heap on the process heap. Without this, the many 1-2 KB
	// wstring allocations from FName resolution over ~100 k objects fragment the default
	// heap and working set inflates to multiple GB without actually being used — the
	// dumper would OOM-kill or take forever paging. LFH keeps free blocks in size-binned
	// lookaside lists and coalesces aggressively.
	{
		HANDLE heap = GetProcessHeap();
		ULONG info = 2;
		HeapSetInformation(heap, HeapCompatibilityInformation, &info, sizeof(info));
	}

	Settings::Config::Load();

	std::cerr << "Dumper-7 (external mode)\n";

	if (!HvProbe::IsRunning())
	{
		std::cerr << "hv-nebula hypervisor not detected. Load it first.\n";
		return 1;
	}

	const DWORD pid = ResolveTargetPid(argc, argv);
	if (!pid)
		return 1;

	std::cerr << "Target PID: " << pid << "\n";

	if (!RemoteMemory::Init(pid))
	{
		std::cerr << "Failed to acquire target process " << pid << "\n";
		return 1;
	}

	std::cerr << "CR3:              0x" << std::hex << RemoteMemory::GetCr3() << std::dec << "\n";
	std::cerr << "Main module:      0x" << std::hex << RemoteMemory::GetMainModuleBase() << std::dec << "\n";
	std::cerr << "Main module size: 0x" << std::hex << RemoteMemory::GetMainModuleSize() << std::dec << "\n\n";

	const auto dumpStart = std::chrono::high_resolution_clock::now();

	std::cerr << "[main] -> InitEngineCore\n";
	Generator::InitEngineCore();
	std::cerr << "[main] -> InitInternal\n";
	Generator::InitInternal();
	std::cerr << "[main] <- InitInternal\n";

	if (Settings::Generator::GameName.empty() || Settings::Generator::GameVersion.empty())
		ResolveGameNameAndVersionExternally(pid);

	std::cerr << "GameName:    " << Settings::Generator::GameName << "\n";
	std::cerr << "GameVersion: " << Settings::Generator::GameVersion << "\n\n";

	// Each backend runs independently; if one throws on a reflection-stripped target we still
	// want the others to produce their output (e.g. MappingGenerator may succeed even when
	// CppGenerator trips on a missing class's CppName).
	auto runGenerator = [](const char* name, auto generatorFn)
	{
		std::cerr << "[main] -> Generate<" << name << ">\n";
		try
		{
			generatorFn();
			std::cerr << "[main] <- Generate<" << name << "> ok\n";
		}
		catch (const std::exception& e)
		{
			std::cerr << "[main] <- Generate<" << name << "> threw: " << e.what() << "\n";
		}
		catch (...)
		{
			std::cerr << "[main] <- Generate<" << name << "> threw non-std exception\n";
		}
	};

	runGenerator("CppGenerator",        [] { Generator::Generate<CppGenerator>(); });
	runGenerator("MappingGenerator",    [] { Generator::Generate<MappingGenerator>(); });
	runGenerator("IDAMappingGenerator", [] { Generator::Generate<IDAMappingGenerator>(); });
	runGenerator("DumpspaceGenerator",  [] { Generator::Generate<DumpspaceGenerator>(); });
	std::cerr << "[main] <- all generators\n";

	const auto dumpEnd = std::chrono::high_resolution_clock::now();
	const std::chrono::duration<double, std::milli> elapsed = dumpEnd - dumpStart;
	std::cerr << "\nSDK generation took " << elapsed.count() << " ms\n";

	return 0;
}
