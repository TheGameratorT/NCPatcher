#include "patchmaker.hpp"

#include <fstream>
#include <functional>
#include <iomanip>

#include "../elf.hpp"

#include "../main.hpp"
#include "../log.hpp"
#include "../except.hpp"
#include "../config/buildconfig.hpp"
#include "../util.hpp"

/*
 * TODO: keep track of overlays that used to be modified and aren't anymore
 * */

namespace fs = std::filesystem;

struct PatchType {
	enum { Jump, Call, Hook, Over };
};

struct PatchSymbolInfo
{
	u32 srcAddress; // the address of the symbol (only fetched after linkage)
	int srcAddressOv; // the overlay the address of the symbol (-1 arm, >= 0 overlay)
	u32 destAddress; // the address to be patched
	int destAddressOv; // the overlay of the address to be patched
	std::size_t patchType; // the patch type
	int sectionIdx; // the index of the section (-1 label, >= 0 section index)
	std::string symbol; // the symbol of the patch (used to generate linker script)
};

static const char* s_patchTypeNames[] = { "jump", "call", "hook", "over" };

PatchMaker::PatchMaker() = default;
PatchMaker::~PatchMaker() = default;

void PatchMaker::makeTarget(
	const BuildTarget& target,
	const std::filesystem::path& targetWorkDir,
	const std::filesystem::path& buildDir,
	const HeaderBin& header,
	std::vector<std::unique_ptr<SourceFileJob>>& srcFileJobs
	)
{
	m_target = &target;
	m_targetWorkDir = &targetWorkDir;
	m_buildDir = &buildDir;
	m_header = &header;
	m_srcFileJobs = &srcFileJobs;

	createBackupDirectory();

	loadArmBin();
	loadOverlayTableBin();
	// TODO: Here check if the overlay was used in the previous build, and if so, load it

	m_newcodeAddr = getArm()->read<u32>(target.arenaLo);

	gatherInfoFromObjects();
	// TODO: Check which functions have the same address as hooks before generating the linker script

	// TODO: Gather ncp_setxxxx definitions after linkage

	saveOverlayBins();
	saveOverlayTableBin();
	saveArmBin();
}

void PatchMaker::gatherInfoFromObjects()
{
	constexpr bool DEBUG_PRINT_HOOK_TABLE = true;

	fs::current_path(*m_targetWorkDir);

	for (auto& srcFileJob : *m_srcFileJobs)
	{
		const fs::path& objPath = srcFileJob->objFilePath;
		//Log::out << ANSI_bYELLOW << objPath.string() << ANSI_RESET << std::endl;

		const BuildTarget::Region* region = srcFileJob->region;

		std::vector<PatchSymbolInfo*> patchInfoForThisObj;

		if (!std::filesystem::exists(objPath))
			throw ncp::file_error(objPath, ncp::file_error::find);
		Elf32 elf;
		if (!elf.load(objPath))
			throw ncp::file_error(objPath, ncp::file_error::read);

		const Elf32_Ehdr& eh = elf.getHeader();
		auto sh_tbl = elf.getSectionHeaderTable();
		auto str_tbl = elf.getSection<char>(sh_tbl[eh.e_shstrndx]);

		auto forEachSection = [&](std::function<void(std::size_t, const Elf32_Shdr&, std::string_view)> cb){
			for (std::size_t i = 0; i < eh.e_shnum; i++)
			{
				const Elf32_Shdr& sh = sh_tbl[i];
				std::string_view sectionName(&str_tbl[sh.sh_name]);
				cb(i, sh, sectionName);
			}
		};

		auto forEachSymbol = [&](std::function<void(const Elf32_Sym&, std::string_view)> cb){
			for (std::size_t i = 0; i < eh.e_shnum; i++)
			{
				const Elf32_Shdr& sh = sh_tbl[i];
				if ((sh.sh_type == SHT_SYMTAB) || (sh.sh_type == SHT_DYNSYM))
				{
					auto sym_tbl = elf.getSection<Elf32_Sym>(sh);
					auto sym_str_tbl = elf.getSection<char>(sh_tbl[sh.sh_link]);
					for (std::size_t j = 0; j < sh.sh_size / sizeof(Elf32_Sym); j++)
					{
						const Elf32_Sym& sym = sym_tbl[j];
						std::string_view symbolName(&sym_str_tbl[sym.st_name]);
						cb(sym, symbolName);
					}
				}
			}
		};

		auto parseSymbol = [&](std::string_view symbolName, int sectionIdx){
			std::string_view labelName = symbolName.substr(sectionIdx != -1 ? 5 : 4);

			std::size_t patchTypeNameEnd = labelName.find('_');
			if (patchTypeNameEnd == std::string::npos)
				return;

			std::string_view patchTypeName = labelName.substr(0, patchTypeNameEnd);
			std::size_t patchType = Util::indexOf(patchTypeName, s_patchTypeNames, 4);
			if (patchType == -1)
			{
				Log::out << OWARN << "Found invalid patch type: " << patchTypeName << std::endl;
				return;
			}

			bool expectingOverlay = true;
			std::size_t addressNameStart = patchTypeNameEnd + 1;
			std::size_t addressNameEnd = labelName.find('_', addressNameStart);
			if (addressNameEnd == std::string::npos)
			{
				addressNameEnd = labelName.length();
				expectingOverlay = false;
			}
			std::string_view addressName = labelName.substr(addressNameStart, addressNameEnd - addressNameStart);
			u32 destAddress;
			try {
				destAddress = Util::addrToInt(std::string(addressName));
			} catch (std::exception& e) {
				Log::out << OWARN << "Found invalid address for patch: " << labelName << std::endl;
				return;
			}

			int destAddressOv = -1;
			if (expectingOverlay)
			{
				std::size_t overlayNameStart = addressNameEnd + 1;
				std::size_t overlayNameEnd = labelName.length();
				std::string_view overlayName = labelName.substr(overlayNameStart, overlayNameEnd - overlayNameStart);
				if (!overlayName.starts_with("ov"))
				{
					Log::out << OWARN << "Expected overlay definition in patch for: " << labelName << std::endl;
					return;
				}
				try {
					destAddressOv = Util::addrToInt(std::string(overlayName.substr(2)));
				} catch (std::exception& e) {
					Log::out << OWARN << "Found invalid overlay for patch: " << labelName << std::endl;
					return;
				}
			}

			auto* patchInfoEntry = new PatchSymbolInfo({
				.srcAddress = 0, // we do not yet know it, only after linkage
				.srcAddressOv = region->destination,
				.destAddress = destAddress,
				.destAddressOv = destAddressOv,
				.patchType = patchType,
				.sectionIdx = sectionIdx,
				.symbol = std::string(symbolName)
			});

			patchInfoForThisObj.emplace_back(patchInfoEntry);
			m_patchInfo.emplace_back(patchInfoEntry);
		};

		// Find patches in sections
		forEachSection([&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
			if (sectionName.starts_with(".ncp_"))
				parseSymbol(sectionName, int(sectionIdx));
		});

		// Find patches in symbols
		forEachSymbol([&](const Elf32_Sym& symbol, std::string_view symbolName){
			if (symbolName.starts_with("ncp_") && !symbolName.substr(4).starts_with("dest"))
				parseSymbol(symbolName, -1);
		});

		// Find functions that should be external
		forEachSymbol([&](const Elf32_Sym& symbol, std::string_view symbolName){
			if (ELF32_ST_TYPE(symbol.st_info) == STT_FUNC)
			{
				for (PatchSymbolInfo* p : patchInfoForThisObj)
				{
					if (p->sectionIdx != -1) // is patch instructed by section
					{
						// if function has the same section as the patch instruction section
						if (p->sectionIdx == symbol.st_shndx)
							m_externSymbols.emplace_back(symbolName);
					}
				}
			}
		});

		for (PatchSymbolInfo* p : patchInfoForThisObj)
		{
			if (p->sectionIdx == -1) // is patch instructed by label
				m_externSymbols.emplace_back(p->symbol);
		}
	}

	if (DEBUG_PRINT_HOOK_TABLE)
	{
		Log::out << "Hooks:\nSRC_ADDR, SRC_ADDR_OV, DST_ADDR, DST_ADDR_OV, PATCH_TYPE, SECTION_IDX, SYMBOL" << std::endl;
		for (auto& p : m_patchInfo)
		{
			Log::out <<
					 std::setw(8) << std::hex << p->srcAddress << "  " <<
					 std::setw(11) << std::dec << p->srcAddressOv << "  " <<
					 std::setw(8) << std::hex << p->destAddress << "  " <<
					 std::setw(11) << std::dec << p->destAddressOv << "  " <<
					 std::setw(10) << s_patchTypeNames[p->patchType] << "  " <<
					 std::setw(11) << std::dec << p->sectionIdx << "  " <<
					 std::setw(6) << p->symbol << std::endl;
		}
		Log::out << "\nExternal symbols:\n";
		for (const std::string& sym : m_externSymbols)
			Log::out << sym << '\n';
		Log::out << std::flush;
	}
}

void PatchMaker::createBackupDirectory()
{
	const fs::path& bakDir = BuildConfig::getBackupDir();
	fs::current_path(Main::getWorkPath());
	if (!fs::exists(bakDir))
	{
		if (!fs::create_directories(bakDir))
		{
			std::ostringstream oss;
			oss << "Could not create backup directory: " << OSTR(bakDir);
			throw ncp::exception(oss.str());
		}
	}

	const char* prefix = m_target->getArm9() ? "overlay9" : "overlay7";
	fs::path bakOvDir = bakDir / prefix;
	if (!fs::exists(bakOvDir))
	{
		if (!fs::create_directories(bakOvDir))
		{
			std::ostringstream oss;
			oss << "Could not create overlay backup directory: " << OSTR(bakOvDir);
			throw ncp::exception(oss.str());
		}
	}
}

void PatchMaker::loadArmBin()
{
	bool isArm9 = m_target->getArm9();

	const char* binName; u32 entryAddress, ramAddress, autoLoadListHookOff;
	if (isArm9)
	{
		binName = "arm9.bin";
		entryAddress = m_header->arm9.entryAddress;
		ramAddress = m_header->arm9.ramAddress;
		autoLoadListHookOff = m_header->arm9AutoLoadListHookOffset;
	}
	else
	{
		binName = "arm7.bin";
		entryAddress = m_header->arm7.entryAddress;
		ramAddress = m_header->arm7.ramAddress;
		autoLoadListHookOff = m_header->arm7AutoLoadListHookOffset;
	}

	fs::current_path(Main::getWorkPath());

	fs::path bakBinName = BuildConfig::getBackupDir() / binName;

	m_arm = std::make_unique<ArmBin>();
	if (fs::exists(bakBinName)) //has backup
	{
		m_arm->load(bakBinName, entryAddress, ramAddress, autoLoadListHookOff, isArm9);
	}
	else //has no backup
	{
		fs::current_path(Main::getRomPath());
		m_arm->load(binName, entryAddress, ramAddress, autoLoadListHookOff, isArm9);
		const std::vector<u8>& bytes = m_arm->data();

		fs::current_path(Main::getWorkPath());
		std::ofstream outputFile(bakBinName, std::ios::binary);
		if (!outputFile.is_open())
			throw ncp::file_error(bakBinName, ncp::file_error::write);
		outputFile.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
		outputFile.close();
	}
}

void PatchMaker::saveArmBin()
{
	const char* binName = m_target->getArm9() ? "arm9.bin" : "arm7.bin";

	const std::vector<u8>& bytes = m_arm->data();

	fs::current_path(Main::getRomPath());
	std::ofstream outputFile(binName, std::ios::binary);
	if (!outputFile.is_open())
		throw ncp::file_error(binName, ncp::file_error::write);
	outputFile.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
	outputFile.close();
}

void PatchMaker::loadOverlayTableBin()
{
	// TODO: This probably needs to save a backup as well

	Log::info("Loading overlay table...");

	const char* binName = m_target->getArm9() ? "arm9ovt.bin" : "arm7ovt.bin";

	fs::current_path(Main::getWorkPath());

	fs::path bakBinName = BuildConfig::getBackupDir() / binName;

	fs::path workBinName;
	if (fs::exists(bakBinName))
	{
		workBinName = bakBinName;
	}
	else
	{
		fs::current_path(Main::getRomPath());
		workBinName = binName;
		if (!fs::exists(binName))
			throw ncp::file_error(binName, ncp::file_error::find);
	}

	uintmax_t fileSize = fs::file_size(workBinName);
	u32 overlayCount = fileSize / sizeof(OvtEntry);

	m_ovtEntries.resize(overlayCount);

	std::ifstream inputFile(workBinName, std::ios::binary);
	if (!inputFile.is_open())
		throw ncp::file_error(workBinName, ncp::file_error::read);
	for (u32 i = 0; i < overlayCount; i++)
	{
		auto* entry = new OvtEntry();
		inputFile.read(reinterpret_cast<char*>(entry), sizeof(OvtEntry));
		m_ovtEntries[i] = std::unique_ptr<OvtEntry>(entry);
	}
	inputFile.close();
}

void PatchMaker::saveOverlayTableBin()
{
	fs::current_path(Main::getRomPath());

	const char* binName = m_target->getArm9() ? "arm9ovt.bin" : "arm7ovt.bin";

	std::ofstream outputFile(binName, std::ios::binary);
	if (!outputFile.is_open())
		throw ncp::file_error(binName, ncp::file_error::read);
	for (auto& ovtEntry : m_ovtEntries)
		outputFile.write(reinterpret_cast<const char*>(ovtEntry.get()), sizeof(OvtEntry));
	outputFile.close();
}

OverlayBin* PatchMaker::loadOverlayBin(std::size_t ovID)
{
	std::string prefix = m_target->getArm9() ? "overlay9" : "overlay7";

	fs::current_path(Main::getWorkPath());

	fs::path binName = fs::path(prefix) / (prefix + "_" + std::to_string(ovID) + ".bin");
	fs::path bakBinName = BuildConfig::getBackupDir() / binName;

	OvtEntry& ovte = *m_ovtEntries[ovID];

	auto* overlay = new OverlayBin();
	if (fs::exists(bakBinName)) //has backup
	{
		overlay->load(bakBinName, ovte.ramAddress, ovte.flag & OVERLAY_FLAG_COMP);
		ovte.flag = 0;
		return overlay;
	}
	else //has no backup
	{
		fs::current_path(Main::getRomPath());
		overlay->load(binName, ovte.ramAddress, ovte.flag & OVERLAY_FLAG_COMP);
		ovte.flag = 0;
		const std::vector<u8>& bytes = overlay->data();

		fs::current_path(Main::getWorkPath());
		std::ofstream outputFile(bakBinName, std::ios::binary);
		if (!outputFile.is_open())
			throw ncp::file_error(bakBinName, ncp::file_error::write);
		outputFile.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
		outputFile.close();
	}

	m_loadedOverlays.emplace(ovID, std::unique_ptr<OverlayBin>(overlay));
	return overlay;
}

OverlayBin* PatchMaker::getOverlay(std::size_t ovID)
{
	for (auto& [id, ov] : m_loadedOverlays)
	{
		if (id == ovID)
			return ov.get();
	}
	return loadOverlayBin(ovID);
}

void PatchMaker::saveOverlayBins()
{
	std::string prefix = m_target->getArm9() ? "overlay9" : "overlay7";

	fs::current_path(Main::getRomPath());

	for (auto& [ovID, ov] : m_loadedOverlays)
	{
		fs::path binName = fs::path(prefix) / (prefix + "_" + std::to_string(ovID) + ".bin");

		const std::vector<u8>& bytes = ov->data();

		std::ofstream outputFile(binName, std::ios::binary);
		if (!outputFile.is_open())
			throw ncp::file_error(binName, ncp::file_error::write);
		outputFile.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
		outputFile.close();
	}
}

std::string PatchMaker::createLinkerScript()
{
	std::string o;
	o.reserve(65536);

	o += "/* Auto-generated linker script */\n\nINCLUDE ";

	return o;
}
