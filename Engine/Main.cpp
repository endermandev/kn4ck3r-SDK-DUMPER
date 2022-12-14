// Unreal Engine SDK Generator
// by KN4CK3R
// https://www.oldschoolhack.me

#include <windows.h>

#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <chrono>
#include <filesystem>
#include <bitset>
namespace fs = std::filesystem;
#include "cpplinq.hpp"

#include "Logger.hpp"

#include "IGenerator.hpp"
#include "ObjectsStore.hpp"
#include "Package.hpp"
#include "NameValidator.hpp"
#include "PrintHelper.hpp"

extern IGenerator* generator;

/// <summary>
/// Dumps the objects and names to files.
/// </summary>
/// <param name="path">The path where to create the dumps.</param>
void Dump(const fs::path& path)
{
	{
		std::ofstream o(path / "ObjectsDump.txt");
		tfm::format(o, "Address: 0x%P\n\n", ObjectsStore::GetAddress());

		for (auto obj : ObjectsStore())
		{
			tfm::format(o, "[%06i] %-100s 0x%P\n", obj.GetIndex(), obj.GetFullName(), obj.GetAddress());
		}
	}
}

/// <summary>
/// Generates the sdk header.
/// </summary>
/// <param name="path">The path where to create the sdk header.</param>
/// <param name="processedObjects">The list of processed objects.</param>
/// <param name="packageOrder">The package order info.</param>
void SaveSDKHeader(const fs::path& path, const std::unordered_map<UEObject, bool>& processedObjects, const std::vector<std::unique_ptr<Package>>& packages)
{
	std::ofstream os(path / "SDK.hpp");

	os << "#pragma once\n\n"
		<< tfm::format("// %s (%s) SDK\n\n", generator->GetGameName(), generator->GetGameVersion());

	//Includes
	os << "#include <set>\n";
	os << "#include <string>\n";
	os << "#include <locale>\n";
	os << "#include <Windows.h>\n";
	for (auto&& i : generator->GetIncludes())
	{
		os << "#include " << i << "\n";
	}

	//include the basics
	{
		{
			std::ofstream os2(path / "SDK" / tfm::format("%s_Basic.hpp", generator->GetGameNameShort()));

			PrintFileHeader(os2, true);
			
			os2 << generator->GetBasicDeclarations() << "\n";

			PrintFileFooter(os2);

			os << "\n#include \"SDK/" << tfm::format("%s_Basic.hpp", generator->GetGameNameShort()) << "\"\n";
		}
		{
			std::ofstream os2(path / "SDK" / tfm::format("%s_Basic.cpp", generator->GetGameNameShort()));

			PrintFileHeader(os2, { "\"../SDK.hpp\"" }, false);

			os2 << generator->GetBasicDefinitions() << "\n";

			PrintFileFooter(os2);
		}
	}

	using namespace cpplinq;

	//check for missing structs
	const auto missing = from(processedObjects) >> where([](auto&& kv) { return kv.second == false; });
	if (missing >> any())
	{
		std::ofstream os2(path / "SDK" / tfm::format("%s_MISSING.hpp", generator->GetGameNameShort()));

		PrintFileHeader(os2, true);

		for (auto&& s : missing >> select([](auto&& kv) { return kv.first.Cast<UEStruct>(); }) >> experimental::container())
		{
			os2 << "// " << s.GetFullName() << "\n// ";
			os2 << tfm::format("0x%04X\n", s.GetPropertySize());

			os2 << "struct " << MakeValidName(s.GetNameCPP()) << "\n{\n";
			os2 << "\tunsigned char UnknownData[0x" << tfm::format("%X", s.GetPropertySize()) << "];\n};\n\n";
		}

		PrintFileFooter(os2);

		os << "\n#include \"SDK/" << tfm::format("%s_MISSING.hpp", generator->GetGameNameShort()) << "\"\n";
	}

	os << "\n";

	for (auto&& package : packages)
	{
		os << R"(#include "SDK/)" << GenerateFileName(FileContentType::Structs, *package) << "\"\n";
		os << R"(#include "SDK/)" << GenerateFileName(FileContentType::Classes, *package) << "\"\n";
		if (generator->ShouldGenerateFunctionParametersFile())
		{
			os << R"(#include "SDK/)" << GenerateFileName(FileContentType::FunctionParameters, *package) << "\"\n";
		}
	}
}

/// <summary>
/// Process the packages.
/// </summary>
/// <param name="path">The path where to create the package files.</param>
void ProcessPackages(const fs::path& path)
{
	using namespace cpplinq;

	const auto sdkPath = path / "SDK";
	fs::create_directories(sdkPath);
	
	std::vector<std::unique_ptr<Package>> packages;

	std::unordered_map<UEObject, bool> processedObjects;

	auto packageObjects = from(ObjectsStore())
		>> select([](auto&& o) { return o.GetPackageObject(); })
		>> where([](auto&& o) { return o.IsValid(); })
		>> distinct()
		>> to_vector();

	for (auto obj : packageObjects)
	{
		if (obj.IsBlueprint() && !generator->GenerateBlueprints())
			continue;

		auto package = std::make_unique<Package>(obj);

		package->Process(processedObjects);
		if (package->Save(sdkPath))
		{
			Package::PackageMap[obj] = package.get();

			packages.emplace_back(std::move(package));
		}
	}

	if (!packages.empty())
	{
		// std::sort doesn't work, so use a simple bubble sort
		//std::sort(std::begin(packages), std::end(packages), PackageDependencyComparer());
		const PackageDependencyComparer comparer;
		for (auto i = 0u; i < packages.size() - 1; ++i)
		{
			for (auto j = 0u; j < packages.size() - i - 1; ++j)
			{
				if (!comparer(packages[j], packages[j + 1]))
				{
					std::swap(packages[j], packages[j + 1]);
				}
			}
		}
	}

	SaveSDKHeader(path, processedObjects, packages);
}

DWORD WINAPI OnAttach(LPVOID lpParameter)
{
	if (!ObjectsStore::Initialize())
	{
		MessageBoxA(nullptr, "ObjectsStore::Initialize failed", "Error", 0);
		return -1;
	}

	if (!generator->Initialize(lpParameter))
	{
		MessageBoxA(nullptr, "Initialize failed", "Error", 0);
		return -1;
	}

	fs::path outputDirectory(generator->GetOutputDirectory());
	if (!outputDirectory.is_absolute())
	{
		char buffer[2048];
		if (GetModuleFileNameA(static_cast<HMODULE>(lpParameter), buffer, sizeof(buffer)) == 0)
		{
			MessageBoxA(nullptr, "GetModuleFileName failed", "Error", 0);
			return -1;
		}

		outputDirectory = fs::path(buffer).remove_filename() / outputDirectory;
	}

	outputDirectory /= generator->GetGameNameShort();
	fs::create_directories(outputDirectory);
	
	std::ofstream log(outputDirectory / "Generator.log");
	Logger::SetStream(&log);

	if (generator->ShouldDumpArrays())
	{
		Dump(outputDirectory);
	}

	fs::create_directories(outputDirectory);

	const auto begin = std::chrono::system_clock::now();

	ProcessPackages(outputDirectory);

	Logger::Log("Finished, took %d seconds.", std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - begin).count());

	Logger::SetStream(nullptr);

	MessageBoxA(nullptr, "Finished!", "Info", 0);

	return 0;
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hModule);

		CreateThread(nullptr, 0, OnAttach, hModule, 0, nullptr);

		return TRUE;
	}

	return FALSE;
}
