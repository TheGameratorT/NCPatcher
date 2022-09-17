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
#include "../process.hpp"

/*
 * TODO: keep track of overlays that used to be modified and aren't anymore
 * TODO: Endianness checks
 * */

namespace fs = std::filesystem;

constexpr bool PRINT_PATCH_TABLE = true;
constexpr bool PRINT_PATCH_TABLE_OBJ = true;

constexpr std::size_t SizeOfHookBridge = 20;

struct PatchType {
	enum {
		Jump, Call, Hook, Over,
		SetJump, SetCall, SetHook,
		RtRepl
	};
};

struct GenericPatchInfo
{
	u32 srcAddress; // the address of the symbol (only fetched after linkage)
	int srcAddressOv; // the overlay the address of the symbol (-1 arm, >= 0 overlay)
	u32 destAddress; // the address to be patched
	int destAddressOv; // the overlay of the address to be patched
	std::size_t patchType; // the patch type
	int sectionIdx; // the index of the section (-1 label, >= 0 section index)
	int sectionSize; // the size of the section (used for over patches)
	std::string symbol; // the symbol of the patch (used to generate linker script)
	SourceFileJob* job;
};

struct RtReplPatchInfo
{
	std::string symbol;
	SourceFileJob* job;
};

struct NewcodePatch
{
	std::size_t binSize;
	std::size_t bssSize;
};

struct HookMakerInfo
{
	u32 address;
	std::vector<u8> data;
};

struct LDSMemoryEntry
{
	std::string name;
	u32 origin;
	int length;
};

struct LDSRegionEntry
{
	int dest;
	LDSMemoryEntry* memory;
	const BuildTarget::Region* region;
	std::vector<GenericPatchInfo*> sectionPatches;
};

struct LDSOverPatch
{
	GenericPatchInfo* info;
	LDSMemoryEntry* memory;
};

static const char* s_patchTypeNames[] = {
	"jump", "call", "hook", "over",
	"setjump", "setcall", "sethook",
	"rtrepl"
};

static void print_data_as_hex(const void* data, size_t size, size_t rowlen)
{
	const auto* cdata = static_cast<const unsigned char*>(data);
	for (size_t i = 0, j = 0; i < size; i++)
	{
		printf("%02X ", cdata[i]);
		if (j++ > rowlen || i == (size - 1))
		{
			j = 0;
			printf("\n");
		}
	}
}

static void forEachElfSection(
	const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl, const char* str_tbl,
	const std::function<bool(std::size_t, const Elf32_Shdr&, std::string_view)>& cb
)
{
	for (std::size_t i = 0; i < eh.e_shnum; i++)
	{
		const Elf32_Shdr& sh = sh_tbl[i];
		std::string_view sectionName(&str_tbl[sh.sh_name]);
		if (cb(i, sh, sectionName))
			break;
	}
}

static void forEachElfSymbol(
	const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
	const std::function<bool(const Elf32_Sym&, std::string_view)>& cb
)
{
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
				if (cb(sym, symbolName))
					break;
			}
		}
	}
}

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


	m_ldscriptPath = *m_buildDir / (m_target->getArm9() ? "ldscript9.x" : "ldscript7.x");
	m_elfPath = *m_buildDir / (m_target->getArm9() ? "arm9.elf" : "arm7.elf");

	createBackupDirectory();

	loadArmBin();
	loadOverlayTableBin();
	// TODO: Here check if the overlay was used in the previous build, and if so, load it

	fetchNewcodeAddr();
	gatherInfoFromObjects();
	createLinkerScript();
	linkElfFile();
	loadElfFile();
	gatherInfoFromElf();
	applyPatchesToRom();
	unloadElfFile();

	saveOverlayBins();
	saveOverlayTableBin();
	saveArmBin();
}

void PatchMaker::fetchNewcodeAddr()
{
	m_newcodeAddrForDest[-1] = getArm()->read<u32>(m_target->arenaLo);
	for (auto& region : m_target->regions)
	{
		int dest = region.destination;
		if (dest != -1)
		{
			u32 addr;
			switch (region.mode)
			{
			case BuildTarget::Mode::append:
				addr = m_ovtEntries[dest]->ramSize;
				break;
			case BuildTarget::Mode::replace:
				addr = m_ovtEntries[dest]->ramAddress;
				break;
			case BuildTarget::Mode::create:
				addr = region.address;
				break;
			}
			m_newcodeAddrForDest[dest] = addr;
		}
	}
}

void PatchMaker::gatherInfoFromObjects()
{
	fs::current_path(*m_targetWorkDir);

	Log::info("Getting patches from objects...");

	for (auto& srcFileJob : *m_srcFileJobs)
	{
		const fs::path& objPath = srcFileJob->objFilePath;

		if (PRINT_PATCH_TABLE_OBJ)
			Log::out << ANSI_bYELLOW << objPath.string() << ANSI_RESET << std::endl;

		const BuildTarget::Region* region = srcFileJob->region;

		std::vector<GenericPatchInfo*> patchInfoForThisObj;

		if (!std::filesystem::exists(objPath))
			throw ncp::file_error(objPath, ncp::file_error::find);
		Elf32 elf;
		if (!elf.load(objPath))
			throw ncp::file_error(objPath, ncp::file_error::read);

		const Elf32_Ehdr& eh = elf.getHeader();
		auto sh_tbl = elf.getSectionHeaderTable();
		auto str_tbl = elf.getSection<char>(sh_tbl[eh.e_shstrndx]);

		auto parseSymbol = [&](std::string_view symbolName, int sectionIdx, int sectionSize){
			std::string_view labelName = symbolName.substr(sectionIdx != -1 ? 5 : 4);

			std::size_t patchTypeNameEnd = labelName.find('_');
			if (patchTypeNameEnd == std::string::npos)
				return;

			std::string_view patchTypeName = labelName.substr(0, patchTypeNameEnd);
			std::size_t patchType = Util::indexOf(patchTypeName, s_patchTypeNames, sizeof(s_patchTypeNames) / sizeof(char*));
			if (patchType == -1)
			{
				Log::out << OWARN << "Found invalid patch type: " << patchTypeName << std::endl;
				return;
			}

			if (patchType == PatchType::Over && sectionIdx == -1)
			{
				Log::out << OWARN << "\"over\" patch must be a section type patch: " << patchTypeName << std::endl;
				return;
			}

			if (patchType == PatchType::RtRepl)
			{
				if (sectionIdx != -1) // we do not want the labels, those are placeholders
				{
					m_rtreplPatches.emplace_back(new RtReplPatchInfo{
						/*.symbol = */std::string(symbolName),
						/*.job = */srcFileJob.get()
					});
				}
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

			int srcAddressOv = patchType == PatchType::Over ? destAddressOv : region->destination;

			auto* patchInfoEntry = new GenericPatchInfo({
				.srcAddress = 0, // we do not yet know it, only after linkage
				.srcAddressOv = srcAddressOv,
				.destAddress = destAddress,
				.destAddressOv = destAddressOv,
				.patchType = patchType,
				.sectionIdx = sectionIdx,
				.sectionSize = sectionSize,
				.symbol = std::string(symbolName),
				.job = srcFileJob.get()
			});

			patchInfoForThisObj.emplace_back(patchInfoEntry);
			m_patchInfo.emplace_back(patchInfoEntry);
		};

		// Find patches in sections
		forEachElfSection(eh, sh_tbl, str_tbl,
		[&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
			if (sectionName.starts_with(".ncp_"))
			{
				if (sectionName.substr(5).starts_with("set"))
				{
					int dest = region->destination;
					if (std::find(m_destWithNcpSet.begin(), m_destWithNcpSet.end(), dest) == m_destWithNcpSet.end())
						m_destWithNcpSet.emplace_back(dest);
					m_jobsWithNcpSet.emplace_back(srcFileJob.get());
					return false;
				}
				parseSymbol(sectionName, int(sectionIdx), int(section.sh_size));
			}
			return false;
		});

		// Find patches in symbols
		forEachElfSymbol(elf, eh, sh_tbl,
		[&](const Elf32_Sym& symbol, std::string_view symbolName){
			if (symbolName.starts_with("ncp_"))
			{
				std::string_view stemless = symbolName.substr(4);
				if (stemless != "dest" && !stemless.starts_with("set"))
					parseSymbol(symbolName, -1, 0);
			}
			return false;
		});

		// Find functions that should be external (section marked)
		forEachElfSymbol(elf, eh, sh_tbl,
		[&](const Elf32_Sym& symbol, std::string_view symbolName){
			if (ELF32_ST_TYPE(symbol.st_info) == STT_FUNC)
			{
				for (GenericPatchInfo* p : patchInfoForThisObj)
				{
					if (p->sectionIdx != -1) // is patch instructed by section
					{
						// if function has the same section as the patch instruction section
						if (p->sectionIdx == symbol.st_shndx)
							m_externSymbols.emplace_back(symbolName);
					}
				}
			}
			return false;
		});

		// Find functions that should be external (label marked)
		for (GenericPatchInfo* p : patchInfoForThisObj)
		{
			if (p->sectionIdx == -1) // is patch instructed by label
				m_externSymbols.emplace_back(p->symbol);
		}

		if (PRINT_PATCH_TABLE_OBJ)
		{
			if (patchInfoForThisObj.empty())
			{
				Log::out << "NO PATCHES" << std::endl;
			}
			else
			{
				Log::out << "SRC_ADDR_OV, DST_ADDR, DST_ADDR_OV, PATCH_TYPE, SECTION_IDX, SECTION_SIZE, SYMBOL" << std::endl;
				for (auto& p : patchInfoForThisObj)
				{
					Log::out <<
						std::setw(11) << std::dec << p->srcAddressOv << "  " <<
						std::setw(8) << std::hex << p->destAddress << "  " <<
						std::setw(11) << std::dec << p->destAddressOv << "  " <<
						std::setw(10) << s_patchTypeNames[p->patchType] << "  " <<
						std::setw(11) << std::dec << p->sectionIdx << "  " <<
						std::setw(12) << std::dec << p->sectionSize << "  " <<
						std::setw(6) << p->symbol << std::endl;
				}
			}
		}
	}
	Log::out << "\nExternal symbols:\n";
	for (const std::string& sym : m_externSymbols)
		Log::out << sym << '\n';
	Log::out << std::flush;
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
		overlay->load(bakBinName, ovte.ramAddress, ovte.flag & OVERLAY_FLAG_COMP, ovID);
		ovte.flag = 0;
	}
	else //has no backup
	{
		fs::current_path(Main::getRomPath());
		overlay->load(binName, ovte.ramAddress, ovte.flag & OVERLAY_FLAG_COMP, ovID);
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

void PatchMaker::createLinkerScript()
{
	auto addSectionInclude = [](std::string& o, std::string& objPath, const char* secInc){
		o += "\t\t\"";
		o += objPath;
		o += "\" (.";
		o += secInc;
		o += ")\n";
	};

	Log::out << OLINK << "Generating the linker script..." << std::endl;

	fs::current_path(*m_targetWorkDir);
	fs::path symbolsFile = fs::absolute(m_target->symbols);

	fs::current_path(*m_buildDir);

	std::vector<std::unique_ptr<LDSMemoryEntry>> memoryEntries;
	memoryEntries.emplace_back(new LDSMemoryEntry{ "bin", 0, 0x100000 });

	std::vector<std::unique_ptr<LDSRegionEntry>> regionEntries;

	// Overlays must come before arm section
	std::vector<const BuildTarget::Region*> orderedRegions(m_target->regions.size());
	for (std::size_t i = 0; i < m_target->regions.size(); i++)
		orderedRegions[i] = &m_target->regions[i];
	std::sort(orderedRegions.begin(), orderedRegions.end(), [](const BuildTarget::Region* a, const BuildTarget::Region* b){
		return a->destination > b->destination;
	});

	for (const BuildTarget::Region* region : orderedRegions)
	{
		LDSMemoryEntry* memEntry;

		int dest = region->destination;
		u32 newcodeAddr = m_newcodeAddrForDest[dest];
		if (dest == -1)
		{
			memEntry = new LDSMemoryEntry{ "arm", newcodeAddr, region->length };
		}
		else
		{
			std::string memName; memName.reserve(8);
			memName += "ov";
			memName += std::to_string(dest);
			memEntry = new LDSMemoryEntry{ std::move(memName), newcodeAddr, region->length };
		}

		memoryEntries.emplace_back(memEntry);
		regionEntries.emplace_back(new LDSRegionEntry{ dest, memEntry, region });
	}

	std::vector<std::unique_ptr<LDSOverPatch>> overPatches;

	// Iterate all patches to setup the linker script
	for (auto& info : m_patchInfo)
	{
		if (info->patchType == PatchType::Over)
		{
			std::string memName; memName.reserve(32);
			memName += "over_";
			memName += Util::intToAddr(int(info->destAddress), 8, false);
			if (info->destAddressOv != -1)
			{
				memName += '_';
				memName += std::to_string(info->destAddressOv);
			}
			auto* memEntry = new LDSMemoryEntry({ std::move(memName), info->destAddress, info->sectionSize });
			memoryEntries.emplace_back(memEntry);
			overPatches.emplace_back(new LDSOverPatch{ info.get(), memEntry });
		}
		else
		{
			for (auto& ldsRegion : regionEntries)
			{
				if (ldsRegion->dest == info->job->region->destination && info->sectionIdx != -1)
					ldsRegion->sectionPatches.emplace_back(info.get());
			}
		}
	}

	if (!m_destWithNcpSet.empty())
		memoryEntries.emplace_back(new LDSMemoryEntry{ "ncp_set", 0, 0x100000 });

	std::string o;
	o.reserve(65536);

	o += "/* NCPatcher: Auto-generated linker script */\n\nINCLUDE \"";

	o += fs::relative(symbolsFile).string();
	o += "\"\n\nINPUT (\n";

	for (auto& srcFileJob : *m_srcFileJobs)
	{
		o += "\t\"";
		o += fs::relative(srcFileJob->objFilePath).string();
		o += "\"\n";
	}

	o += ")\n\nOUTPUT (\"";
	o += fs::relative(m_elfPath).string();
	o += "\")\n\nMEMORY {\n";

	for (auto& memoryEntry : memoryEntries)
	{
		o += '\t';
		o += memoryEntry->name;
		o += " (rwx): ORIGIN = ";
		o += Util::intToAddr(int(memoryEntry->origin), 8);
		o += ", LENGTH = ";
		o += Util::intToAddr(int(memoryEntry->length), 8);
		o += '\n';
	}

	o += "}\n\nSECTIONS {\n";

	for (auto& s : regionEntries)
	{
		std::size_t hookCount = 0;
		// TEXT
		o += "\t.";
		o += s->memory->name;
		o += ".text : ALIGN(4) {\n";
		for (auto& p : s->sectionPatches)
		{
			// Convert the section patches into label patches,
			// except for over and set types
			o += "\t\t";
			o += std::string_view(p->symbol).substr(1);
			o += " = .;\n\t\t* (";
			o += p->symbol;
			o += ")\n";
			if (p->patchType == PatchType::Hook)
				hookCount++;
		}
		for (auto& p : m_rtreplPatches)
		{
			if (p->job->region == s->region)
			{
				std::string_view stem = std::string_view(p->symbol).substr(1);
				o += "\t\t";
				o += stem;
				o += "_start = .;\n\t\t* (";
				o += p->symbol;
				o += ")\n\t\t";
				o += stem;
				o += "_end = .;\n";
			}
		}
		if (s->dest == -1)
		{
			o += "\t\t* (.text)\n"
				 "\t\t* (.rodata)\n"
				 "\t\t* (.init_array)\n"
				 "\t\t* (.data)\n"
				 "\t\t* (.text.*)\n"
				 "\t\t* (.rodata.*)\n"
				 "\t\t* (.init_array.*)\n"
				 "\t\t* (.data.*)\n";
			if (hookCount != 0)
			{
				o += "\t\t. = ALIGN(4);\n"
					 "\t\tncp_hookdata = .;\n"
					 "\t\tFILL(0)\n"
					 "\t\t. = ncp_hookdata + ";
				o += std::to_string(hookCount * SizeOfHookBridge);
				o += ";\n";
			}
		}
		else
		{
			for (auto& f : *m_srcFileJobs)
			{
				if (f->region == s->region)
				{
					std::string objPath = f->objFilePath.string();
					static const char* secIncs[] = {
						"text",
						"rodata",
						"init_array",
						"data",
						"text.*",
						"rodata.*",
						"init_array.*",
						"data.*"
					};
					for (auto& secInc : secIncs)
						addSectionInclude(o, objPath, secInc);
				}
			}
			if (hookCount != 0)
			{
				o += "\t\t. = ALIGN(4);\n\t\tncp_hookdata_";
				o += s->memory->name;
				o += " = .;\n\t\tFILL(0)\n\t\t. = ncp_hookdata_";
				o += s->memory->name;
				o += " + ";
				o += std::to_string(hookCount * SizeOfHookBridge);
				o += ";\n";
			}
		}
		o += "\t\t. = ALIGN(4);\n"
			 "\t} > ";
		o += s->memory->name;
		o += " AT > bin\n"

		// BSS
		     "\n\t.";
		o += s->memory->name;
		o += ".bss : ALIGN(4) {\n";
		if (s->dest == -1)
		{
			o += "\t\t* (.bss)\n"
				 "\t\t* (.bss.*)\n";
		}
		else
		{
			for (auto& f : *m_srcFileJobs)
			{
				if (f->region == s->region)
				{
					std::string objPath = f->objFilePath.string();
					addSectionInclude(o, objPath, "(.bss)");
					addSectionInclude(o, objPath, "(.bss.*)");
				}
			}
		}
		o += "\t\t. = ALIGN(4);\n"
			 "\t} > ";
		o += s->memory->name;
		o += " AT > bin\n";
	}
	if (!regionEntries.empty())
		o += '\n';

	for (auto& p : overPatches)
	{
		o += '\t';
		o += p->info->symbol;
		o += " : { KEEP(* (";
		o += p->info->symbol;
		o += ")) } > ";
		o += p->memory->name;
		o += " AT > bin\n";
	}
	if (!overPatches.empty())
		o += '\n';

	for (auto& p : m_destWithNcpSet)
	{
		o += "\t.ncp_set";
		if (p == -1)
		{
			o += " : { KEEP(* (.ncp_set)) } > ncp_set AT > bin\n";
		}
		else
		{
			o += "_ov";
			o += std::to_string(p);
			o += " : {\n";
			for (auto& j : m_jobsWithNcpSet)
			{
				if (j->region->destination == p)
				{
					o += "\t\t KEEP(\"";
					o += j->objFilePath.string();
					o += "\" (.ncp_set))\n\t"
						 "} > ncp_set AT > bin\n";
				}
			}
		}
	}
	if (!m_destWithNcpSet.empty())
		o += '\n';

	o += "\t/DISCARD/ : {*(.*)}\n"
		 "}\n\nEXTERN (\n";

	for (auto& e : m_externSymbols)
	{
		o += '\t';
		o += e;
		o += '\n';
	}

	o += ")\n";

	// Output the file
	std::ofstream outputFile(m_ldscriptPath);
	if (!outputFile.is_open())
		throw ncp::file_error(m_ldscriptPath, ncp::file_error::write);
	outputFile.write(o.data(), std::streamsize(o.length()));
	outputFile.close();
}

void PatchMaker::linkElfFile()
{
	Log::out << OLINK << "Linking the ARM binary..." << std::endl;

	fs::current_path(*m_buildDir);

	std::string ccmd;
	ccmd.reserve(64);
	ccmd += BuildConfig::getToolchain();
	ccmd += "gcc -Wl,--gc-sections,-T\"";
	ccmd += fs::relative(m_ldscriptPath).string();
	ccmd += "\"";
	if (!m_target->ldFlags.empty())
		ccmd += ",";
	ccmd += m_target->ldFlags;

	std::ostringstream oss;
	int retcode = Process::start(ccmd.c_str(), &oss);
	if (retcode != 0)
	{
		Log::out << oss.str() << std::endl;
		throw ncp::exception("Could not link the ELF file.");
	}
}

void PatchMaker::gatherInfoFromElf()
{
	const Elf32_Ehdr& eh = m_elf->getHeader();
	auto sh_tbl = m_elf->getSectionHeaderTable();
	auto str_tbl = m_elf->getSection<char>(sh_tbl[eh.e_shstrndx]);

	auto parseNcpSetSymbol = [&](std::string_view symbolName, u32 srcAddress, int srcAddressOv){
		std::string_view labelName = symbolName.substr(7);

		std::size_t patchTypeNameEnd = labelName.find('_');
		if (patchTypeNameEnd == std::string::npos)
			return;

		std::string_view patchTypeName = labelName.substr(0, patchTypeNameEnd);
		std::size_t patchType = Util::indexOf(patchTypeName, s_patchTypeNames, sizeof(s_patchTypeNames) / sizeof(char*));
		if (patchType == -1)
		{
			Log::out << OWARN << "Found invalid patch type: " << patchTypeName << std::endl;
			return;
		}

		if (patchType != PatchType::Jump && patchType != PatchType::Call && patchType != PatchType::Hook)
		{
			Log::out << OWARN << "\"set\" patch type must be a jump, call or hook: " << patchTypeName << std::endl;
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

		auto* patchInfoEntry = new GenericPatchInfo({
			.srcAddress = srcAddress,
			.srcAddressOv = srcAddressOv,
			.destAddress = destAddress,
			.destAddressOv = destAddressOv,
			.patchType = patchType,
			.sectionIdx = -1,
			.sectionSize = 0,
			.symbol = std::string(symbolName),
			.job = nullptr
		});

		m_patchInfo.emplace_back(patchInfoEntry);
	};

	// Update the patch info with new values
	forEachElfSymbol(*m_elf, eh, sh_tbl,
	[&](const Elf32_Sym& symbol, std::string_view symbolName){
		for (auto& p : m_patchInfo)
		{
			if (p->sectionIdx != -1) // patch is section
			{
				std::string_view nameAsLabel = std::string_view(p->symbol).substr(1);
				if (nameAsLabel == symbolName)
				{
					p->srcAddress = symbol.st_value;
					p->sectionIdx = symbol.st_shndx;
					p->symbol = nameAsLabel;
				}
			}
			else
			{
				// This must run before fetching ncp_set section, otherwise ncp_set srcAddr will be overwritten
				if (p->symbol == symbolName)
				{
					p->srcAddress = symbol.st_value;
					p->sectionIdx = symbol.st_shndx;
				}
			}
		}
		if (symbolName.starts_with("ncp_hookdata"))
		{
			int srcAddrOv = -1;
			if (symbolName.length() != 12 && symbolName.substr(12).starts_with("_ov"))
			{
				try {
					srcAddrOv = std::stoi(std::string(symbolName.substr(15)));
				} catch (std::exception& e) {
					Log::out << OWARN << "Found invalid overlay parsing ncp_hookdata symbol: " << symbolName << std::endl;
					return false;
				}
			}
			auto* info = new HookMakerInfo();
			info->address = symbol.st_value;
			m_hookMakerInfoForDest.emplace(srcAddrOv, info);
		}
		return false;
	});

	forEachElfSection(eh, sh_tbl, str_tbl,
	[&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
		for (auto& p : m_patchInfo)
		{
			if (p->patchType == PatchType::Over)
			{
				if (p->symbol == sectionName)
				{
					p->srcAddress = section.sh_addr; // should be the same as the destination
					p->sectionIdx = int(sectionIdx);
				}
			}
		}
		if (sectionName.starts_with(".ncp_set"))
		{
			// found the ncp_set section, get all hook definitions stored there

			int srcAddrOv = -1;
			if (sectionName.length() != 8 && sectionName.substr(8).starts_with("_ov"))
			{
				try {
					srcAddrOv = std::stoi(std::string(sectionName.substr(11)));
				} catch (std::exception& e) {
					Log::out << OWARN << "Found invalid overlay reading ncp_set section: " << sectionName << std::endl;
					return false;
				}
			}

			const char* sectionData = m_elf->getSection<char>(section);

			forEachElfSymbol(*m_elf, eh, sh_tbl,
			[&](const Elf32_Sym& symbol, std::string_view symbolName){
				if (symbolName.starts_with("ncp_set") && symbol.st_shndx == sectionIdx)
				{
					u32 srcAddr = *reinterpret_cast<const u32*>(&sectionData[symbol.st_value - section.sh_addr]);
					parseNcpSetSymbol(symbolName, srcAddr, srcAddrOv);
				}
				return false;
			});
		}
		return false;
	});

	if (PRINT_PATCH_TABLE)
	{
		Log::out << "Patches:\nSRC_ADDR, SRC_ADDR_OV, DST_ADDR, DST_ADDR_OV, PATCH_TYPE, SECTION_IDX, SECTION_SIZE, SYMBOL" << std::endl;
		for (auto& p : m_patchInfo)
		{
			Log::out <<
				std::setw(8) << std::hex << p->srcAddress << "  " <<
				std::setw(11) << std::dec << p->srcAddressOv << "  " <<
				std::setw(8) << std::hex << p->destAddress << "  " <<
				std::setw(11) << std::dec << p->destAddressOv << "  " <<
				std::setw(10) << s_patchTypeNames[p->patchType] << "  " <<
				std::setw(11) << std::dec << p->sectionIdx << "  " <<
				std::setw(12) << std::dec << p->sectionSize << "  " <<
				std::setw(6) << p->symbol << std::endl;
		}
	}

	forEachElfSection(eh, sh_tbl, str_tbl,
	[](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
		auto insertSection = [&](int dest, bool isBss){

		};

		if (sectionName.starts_with(".arm"))
		{
			insertSection(-1, sectionName.substr(5) == "bss");
		}
		else if (sectionName.starts_with(".ov"))
		{
			std::size_t pos = sectionName.find('.', 3);
			if (pos != std::string::npos)
			{
				int dest = std::stoi(std::string(sectionName.substr(3, pos - 3)));
				insertSection(dest, sectionName.substr(pos) == "bss");
			}
		}
		return false;
	});
}

void PatchMaker::loadElfFile()
{
	if (!std::filesystem::exists(m_elfPath))
		throw ncp::file_error(m_elfPath, ncp::file_error::find);

	m_elf = std::make_unique<Elf32>();
	if (!m_elf->load(m_elfPath))
		throw ncp::file_error(m_elfPath, ncp::file_error::read);
}

void PatchMaker::unloadElfFile()
{
	m_elf = nullptr;
}

void PatchMaker::applyPatchesToRom()
{
	Main::setErrorContext(m_target->getArm9() ?
		"Failed to apply patches for ARM9 target." :
		"Failed to apply patches for ARM7 target.");

	const u32 armOpcodeB = 0xEA000000; // B
	const u32 armOpcodeBL = 0xEB000000; // BL
	const u32 armHookPush = 0xE92D100F; // PUSH {R0-R3,R12}
	const u32 armHookPop = 0xE8BD100F; // POP {R0-R3,R12}

	auto makeJump = [](u32 opCode, u32 srcAddr, u32 destAddr){
		return opCode | (((srcAddr >> 2) - (destAddr >> 2) - 2) & 0xFFFFFF);
	};

	auto sh_tbl = m_elf->getSectionHeaderTable();

	for (auto& p : m_patchInfo)
	{
		ICodeBin* bin = (p->destAddressOv == -1) ?
						static_cast<ICodeBin*>(getArm()) :
						static_cast<ICodeBin*>(getOverlay(p->destAddressOv));

		switch (p->patchType)
		{
		case PatchType::Jump:
		{
			bin->write<u32>(p->destAddress, makeJump(armOpcodeB, p->srcAddress, p->destAddress));
			break;
		}
		case PatchType::Call:
		{
			bin->write<u32>(p->destAddress, makeJump(armOpcodeBL, p->srcAddress, p->destAddress));
			break;
		}
		case PatchType::Hook:
		{
			/*
			 * If the patch type is a hook, the instruction at
			 * destAddr must become a jump to a hook bridge generated
			 * by NCPatcher and it should look as such:
			 *
			 * hook_bridge:
			 *     PUSH {R0-R3,R12}
			 *     BL   srcAddr
			 *     POP  {R0-R3,R12}
			 *     <unpatched destAddr's instruction>
			 *     B    (destAddr + 4)
			 * */

			u32 ogOpCode = bin->read<u32>(p->destAddress);

			auto& info = m_hookMakerInfoForDest[p->srcAddressOv];
			if (info == nullptr)
				throw ncp::exception("Unexpected p->srcAddressOv for m_hookMakerInfoForDest encountered.");

			std::vector<u8>& hookData = info->data;
			std::size_t offset = hookData.size();
			hookData.resize(offset + SizeOfHookBridge);

			u32 hookBridgeAddr = info->address;

			u8* hookDataPtr = hookData.data() + offset;

			auto writeHookData = [&hookDataPtr](u32 value){
				std::memcpy(hookDataPtr, &value, 4);
				hookDataPtr += 4;
			};

			writeHookData(armHookPush);
			writeHookData(makeJump(armOpcodeBL, hookBridgeAddr + 4, p->srcAddress));
			writeHookData(armHookPop);
			writeHookData(ogOpCode);
			writeHookData(makeJump(armOpcodeB, hookBridgeAddr + 16, p->destAddress + 4));

			info->address += SizeOfHookBridge;
			break;
		}
		case PatchType::Over:
		{
			const char* sectionData = m_elf->getSection<char>(sh_tbl[p->sectionIdx]);
			bin->writeBytes(p->destAddress, sectionData, p->sectionSize);
			break;
		}
		}
	}

	// TODO: Insert the binary data

	Main::setErrorContext(nullptr);
}
