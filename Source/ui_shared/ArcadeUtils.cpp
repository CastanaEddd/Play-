#include "ArcadeUtils.h"
#include <nlohmann/json.hpp>
#include "string_format.h"
#include "PathUtils.h"
#include "StdStreamUtils.h"
#include "PS2VM.h"
#include "PS2VM_Preferences.h"
#include "DiskUtils.h"
#include "AppConfig.h"
#include "BootablesDbClient.h"
#include "BootablesProcesses.h"
#include "hdd/HddDefs.h"
#include "discimages/ChdImageStream.h"
#include "iop/ioman/McDumpDevice.h"
#include "iop/ioman/HardDiskDumpDevice.h"
#include "iop/Iop_NamcoArcade.h"
#include "iop/namco_arcade/Iop_NamcoAcCdvd.h"
#include "iop/namco_arcade/Iop_NamcoAcRam.h"

struct ARCADE_MACHINE_DEF
{
	struct PATCH
	{
		uint32 address = 0;
		uint32 value = 0;
	};

	std::string id;
	std::string parent;
	std::string name;
	std::string dongleFileName;
	std::string cdvdFileName;
	std::string hddFileName;
	std::map<unsigned int, PS2::CControllerInfo::BUTTON> buttons;
	bool hasLightGun = false;
	std::array<float, 4> lightGunXform = {65535, 0, 65535, 0};
	uint32 eeFreqScaleNumerator = 1;
	uint32 eeFreqScaleDenominator = 1;
	std::string boot;
	std::vector<PATCH> patches;
};

// clang-format off
static const std::pair<const char*, PS2::CControllerInfo::BUTTON> g_buttonValues[] =
{
	{ "dpad_up", PS2::CControllerInfo::DPAD_UP },
	{ "dpad_down", PS2::CControllerInfo::DPAD_DOWN },
	{ "dpad_left", PS2::CControllerInfo::DPAD_LEFT },
	{ "dpad_right", PS2::CControllerInfo::DPAD_RIGHT },
	{ "select", PS2::CControllerInfo::SELECT },
	{ "start", PS2::CControllerInfo::START },
	{ "square", PS2::CControllerInfo::SQUARE },
	{ "triangle", PS2::CControllerInfo::TRIANGLE },
	{ "cross", PS2::CControllerInfo::CROSS },
	{ "circle", PS2::CControllerInfo::CIRCLE },
	{ "l1", PS2::CControllerInfo::L1 },
	{ "l2", PS2::CControllerInfo::L2 },
	{ "l3", PS2::CControllerInfo::L3 },
	{ "r1", PS2::CControllerInfo::R1 },
	{ "r2", PS2::CControllerInfo::R2 },
	{ "r3", PS2::CControllerInfo::R3 }
};
// clang-format on

uint32 ParseHexStringValue(const std::string& value)
{
	uint32 result = 0;
	int scanCount = sscanf(value.c_str(), "0x%x", &result);
	assert(scanCount == 1);
	return result;
}

ARCADE_MACHINE_DEF ReadArcadeMachineDefinition(const fs::path& arcadeDefPath)
{
	auto parseButtons =
	    [](const nlohmann::json& buttonsObject) {
		    auto buttonMap = buttonsObject.get<std::map<std::string, std::string>>();
		    decltype(ARCADE_MACHINE_DEF::buttons) buttons;
		    for(const auto& buttonPair : buttonMap)
		    {
			    char* endPtr = nullptr;
			    const char* buttonNumber = buttonPair.first.c_str();
			    const char* buttonName = buttonPair.second.c_str();
			    int number = strtol(buttonPair.first.c_str(), &endPtr, 10);
			    if(endPtr == buttonPair.first.c_str())
			    {
				    throw std::runtime_error(string_format("Failed to parse button number '%s'.", buttonNumber));
			    }
			    auto buttonValueIterator = std::find_if(std::begin(g_buttonValues), std::end(g_buttonValues),
			                                            [&](const auto& buttonValuePair) { return strcmp(buttonValuePair.first, buttonName) == 0; });
			    if(buttonValueIterator == std::end(g_buttonValues))
			    {
				    throw std::runtime_error(string_format("Unknown button name '%s'.", buttonName));
			    }
			    buttons[number] = buttonValueIterator->second;
		    }
		    return buttons;
	    };

	auto parsePatches =
	    [](const nlohmann::json& patchesArray) {
		    std::vector<ARCADE_MACHINE_DEF::PATCH> patches;
		    for(const auto& jsonPatch : patchesArray)
		    {
			    ARCADE_MACHINE_DEF::PATCH patch;
			    if(!jsonPatch.contains("address") || !jsonPatch.contains("value")) continue;
			    patch.address = ParseHexStringValue(jsonPatch["address"]);
			    patch.value = ParseHexStringValue(jsonPatch["value"]);
			    patches.emplace_back(std::move(patch));
		    }
		    return patches;
	    };

	auto defString =
	    [&arcadeDefPath]() {
		    auto defStream = Framework::CreateInputStdStream(arcadeDefPath.native());
		    return defStream.ReadString();
	    }();

	auto defJson = nlohmann::json::parse(defString);
	ARCADE_MACHINE_DEF def;
	def.id = defJson["id"];
	if(defJson.contains("parent"))
	{
		def.parent = defJson["parent"];
	}
	def.name = defJson["name"];
	def.dongleFileName = defJson["dongle"]["name"];
	if(defJson.contains("cdvd"))
	{
		def.cdvdFileName = defJson["cdvd"]["name"];
	}
	if(defJson.contains("hdd"))
	{
		def.hddFileName = defJson["hdd"]["name"];
	}
	if(defJson.contains("buttons"))
	{
		def.buttons = parseButtons(defJson["buttons"]);
	}
	if(defJson.contains("hasLightGun"))
	{
		def.hasLightGun = defJson["hasLightGun"];
	}
	if(defJson.contains("lightGunXform"))
	{
		auto lightGunXformArray = defJson["lightGunXform"];
		if(lightGunXformArray.is_array() && (lightGunXformArray.size() >= 4))
		{
			for(int i = 0; i < 4; i++)
			{
				def.lightGunXform[i] = lightGunXformArray[i];
			}
		}
	}
	if(defJson.contains("eeFrequencyScale"))
	{
		auto eeFreqScaleArray = defJson["eeFrequencyScale"];
		if(eeFreqScaleArray.is_array() && (eeFreqScaleArray.size() >= 2))
		{
			def.eeFreqScaleNumerator = eeFreqScaleArray[0];
			def.eeFreqScaleDenominator = eeFreqScaleArray[1];
		}
	}
	def.boot = defJson["boot"];
	if(defJson.contains("patches"))
	{
		def.patches = parsePatches(defJson["patches"]);
	}
	return def;
}

void PrepareArcadeEnvironment(CPS2VM* virtualMachine, const ARCADE_MACHINE_DEF& def)
{
	auto baseId = def.parent.empty() ? def.id : def.parent;
	auto romArchiveFileName = string_format("%s.zip", baseId.c_str());

	fs::path arcadeRomPath = CAppConfig::GetInstance().GetPreferencePath(PREF_PS2_ARCADEROMS_DIRECTORY);
	fs::path arcadeRomArchivePath = arcadeRomPath / romArchiveFileName;

	if(!fs::exists(arcadeRomArchivePath))
	{
		throw std::runtime_error(string_format("Failed to find '%s' in arcade ROMs directory.", romArchiveFileName.c_str()));
	}

	//Mount CDVD
	if(!def.cdvdFileName.empty())
	{
		fs::path cdvdPath = arcadeRomPath / baseId / def.cdvdFileName;
		if(!fs::exists(cdvdPath))
		{
			throw std::runtime_error(string_format("Failed to find '%s' in game's directory.", def.cdvdFileName.c_str()));
		}

		//Try to create the optical media for sanity checks (will throw exceptions on errors).
		DiskUtils::CreateOpticalMediaFromPath(cdvdPath);

		CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_CDROM0_PATH, cdvdPath);

		virtualMachine->CDROM0_SyncPath();
	}

	//Mount HDD
	if(!def.hddFileName.empty())
	{
		fs::path hddPath = arcadeRomPath / baseId / def.hddFileName;
		if(!fs::exists(hddPath))
		{
			throw std::runtime_error(string_format("Failed to find '%s' in game's directory.", def.hddFileName.c_str()));
		}

		auto imageStream = std::make_unique<CChdImageStream>(std::make_unique<Framework::CStdStream>(hddPath.string().c_str(), "rb"));
		assert(imageStream->GetUnitSize() == Hdd::g_sectorSize);
		auto device = std::make_shared<Iop::Ioman::CHardDiskDumpDevice>(std::move(imageStream));

		auto iopBios = dynamic_cast<CIopBios*>(virtualMachine->m_iop->m_bios.get());
		iopBios->GetIoman()->RegisterDevice("hdd0", device);
	}

	std::vector<uint8> mcDumpContents;

	{
		auto inputStream = Framework::CreateInputStdStream(arcadeRomArchivePath.native());
		Framework::CZipArchiveReader archiveReader(inputStream);
		auto header = archiveReader.GetFileHeader(def.dongleFileName.c_str());
		if(!header)
		{
			throw std::runtime_error(string_format("Failed to find file '%s' in archive '%s'.", def.dongleFileName.c_str(), romArchiveFileName.c_str()));
		}
		auto fileStream = archiveReader.BeginReadFile(def.dongleFileName.c_str());
		mcDumpContents.resize(header->uncompressedSize);
		fileStream->Read(mcDumpContents.data(), header->uncompressedSize);
	}

	//Override mc0 device with special device reading directly from zip file
	{
		auto device = std::make_shared<Iop::Ioman::CMcDumpDevice>(std::move(mcDumpContents));
		auto iopBios = dynamic_cast<CIopBios*>(virtualMachine->m_iop->m_bios.get());
		iopBios->GetIoman()->RegisterDevice("mc0", device);
		iopBios->GetIoman()->RegisterDevice("ac0", device);

		//Ridge Racer 5: Arcade Battle doesn't have any FILEIO in its IOPRP
		//Assuming that the BIOS image for arcade boards is version 2.0.5
		iopBios->SetDefaultImageVersion(2050);

		auto acRam = std::make_shared<Iop::Namco::CAcRam>(virtualMachine->m_iop->m_ram);
		iopBios->RegisterModule(acRam);
		iopBios->RegisterHleModuleReplacement("Arcade_Ext._Memory", acRam);

		auto acCdvdModule = std::make_shared<Iop::Namco::CAcCdvd>(*iopBios->GetSifman(), *iopBios->GetCdvdman(), virtualMachine->m_iop->m_ram, *acRam.get());
		acCdvdModule->SetOpticalMedia(virtualMachine->m_cdrom0.get());
		iopBios->RegisterModule(acCdvdModule);
		iopBios->RegisterHleModuleReplacement("ATA/ATAPI_driver", acCdvdModule);
		iopBios->RegisterHleModuleReplacement("CD/DVD_Compatible", acCdvdModule);

		{
			auto namcoArcadeModule = std::make_shared<Iop::CNamcoArcade>(*iopBios->GetSifman(), *acRam, def.id);
			iopBios->RegisterModule(namcoArcadeModule);
			iopBios->RegisterHleModuleReplacement("rom0:DAEMON", namcoArcadeModule);
			virtualMachine->m_pad->InsertListener(namcoArcadeModule.get());
			for(const auto& buttonPair : def.buttons)
			{
				namcoArcadeModule->SetButton(buttonPair.first, buttonPair.second);
			}
			if(def.hasLightGun)
			{
				virtualMachine->SetGunListener(namcoArcadeModule.get());
				namcoArcadeModule->SetLightGunXform(def.lightGunXform);
			}
		}
	}
	
	virtualMachine->SetEeFrequencyScale(def.eeFreqScaleNumerator, def.eeFreqScaleDenominator);
	if((def.eeFreqScaleNumerator != 1) || (def.eeFreqScaleDenominator != 1))
	{
		//If not running at base EE frequency, assume that IOP is running at 1.33x speed.
		//TODO: Find a better way to specify that through some kind of base profile.
		static const uint32 baseSamplingRate = Iop::Spu2::CCore::DEFAULT_BASE_SAMPLING_RATE * 4 / 3;
		virtualMachine->m_iop->m_spu2.GetCore(0)->SetBaseSamplingRate(baseSamplingRate);
		virtualMachine->m_iop->m_spu2.GetCore(1)->SetBaseSamplingRate(baseSamplingRate);
	}
}

void ApplyPatchesFromArcadeDefinition(CPS2VM* virtualMachine, const ARCADE_MACHINE_DEF& def)
{
	for(const auto& patch : def.patches)
	{
		assert(patch.address < PS2::EE_RAM_SIZE);
		*reinterpret_cast<uint32*>(virtualMachine->m_ee->m_ram + patch.address) = patch.value;
	}
}

void ArcadeUtils::RegisterArcadeMachines()
{
	//Remove any arcade bootable registered the old way
	//TEMP: Remove this when merging back to main
	{
		auto arcadeBootables = BootablesDb::CClient::GetInstance().GetBootables(BootablesDb::CClient::SORT_METHOD_ARCADE);
		for(const auto& bootable : arcadeBootables)
		{
			if(bootable.path.has_parent_path())
			{
				BootablesDb::CClient::GetInstance().UnregisterBootable(bootable.path);
			}
		}
	}

	auto arcadeDefsPath = Framework::PathUtils::GetAppResourcesPath() / "arcadedefs";
	if(!fs::exists(arcadeDefsPath))
	{
		return;
	}

	for(const auto& entry : fs::directory_iterator(arcadeDefsPath))
	{
		auto arcadeDefPath = entry.path();
		auto arcadeDefFilename = arcadeDefPath.filename();
		try
		{
			auto def = ReadArcadeMachineDefinition(arcadeDefsPath / arcadeDefFilename);
			BootablesDb::CClient::GetInstance().RegisterBootable(arcadeDefFilename, "", "");
			BootablesDb::CClient::GetInstance().SetTitle(arcadeDefFilename, def.name.c_str());
		}
		catch(const std::exception& exception)
		{
			printf("Warning: Failed to register arcade machine '%s': %s\r\n",
			       arcadeDefFilename.string().c_str(), exception.what());
		}
	}
}

void ArcadeUtils::BootArcadeMachine(CPS2VM* virtualMachine, const fs::path& arcadeDefFilename)
{
	auto arcadeDefsPath = Framework::PathUtils::GetAppResourcesPath() / "arcadedefs";
	auto def = ReadArcadeMachineDefinition(arcadeDefsPath / arcadeDefFilename);

	//Reset PS2VM
	virtualMachine->Pause();
	virtualMachine->Reset(PS2::EE_EXT_RAM_SIZE, PS2::IOP_EXT_RAM_SIZE);

	PrepareArcadeEnvironment(virtualMachine, def);

	//Boot mc0:/BOOT (from def)
	virtualMachine->m_ee->m_os->BootFromVirtualPath(def.boot.c_str(), {"DANGLE"});

	ApplyPatchesFromArcadeDefinition(virtualMachine, def);

	virtualMachine->BeforeExecutableReloaded =
	    [def](CPS2VM* virtualMachine) {
		    PrepareArcadeEnvironment(virtualMachine, def);
	    };

	virtualMachine->AfterExecutableReloaded =
	    [def](CPS2VM* virtualMachine) {
		    ApplyPatchesFromArcadeDefinition(virtualMachine, def);
	    };

#ifndef DEBUGGER_INCLUDED
	virtualMachine->Resume();
#endif

	TryUpdateLastBootedTime(arcadeDefFilename);
}
