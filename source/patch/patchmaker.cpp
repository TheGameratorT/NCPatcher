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
 * */

namespace fs = std::filesystem;

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

static const char* s_patchTypeNames[] = {
	"jump", "call", "hook", "over",
	"setjump", "setcall", "sethook",
	"rtrepl"
};

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

	m_newcodeAddr = getArm()->read<u32>(target.arenaLo);

	gatherInfoFromObjects();
	createLinkerScript();
	linkElfFile();
	gatherInfoFromElf();

	// TODO: Gather ncp_setxxxx definitions after linkage

	saveOverlayBins();
	saveOverlayTableBin();
	saveArmBin();
}

void PatchMaker::gatherInfoFromObjects()
{
	constexpr bool DEBUG_PRINT_HOOK_TABLE = false;

	fs::current_path(*m_targetWorkDir);

	for (auto& srcFileJob : *m_srcFileJobs)
	{
		const fs::path& objPath = srcFileJob->objFilePath;
		//Log::out << ANSI_bYELLOW << objPath.string() << ANSI_RESET << std::endl;

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

		auto parseSymbol = [&](std::string_view symbolName, int sectionIdx, int sectionSize){
			std::string_view labelName = symbolName.substr(sectionIdx != -1 ? 5 : 4);

			std::size_t patchTypeNameEnd = labelName.find('_');
			if (patchTypeNameEnd == std::string::npos)
				return;

			std::string_view patchTypeName = labelName.substr(0, patchTypeNameEnd);
			std::size_t patchType = Util::indexOf(patchTypeName, s_patchTypeNames, 8);
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
		forEachSection([&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
			if (sectionName.starts_with(".ncp_") && sectionName.substr(5) != "set")
				parseSymbol(sectionName, int(sectionIdx), int(section.sh_size));
		});

		// Find patches in symbols
		forEachSymbol([&](const Elf32_Sym& symbol, std::string_view symbolName){
			if (symbolName.starts_with("ncp_") && symbolName.substr(4) != "dest")
				parseSymbol(symbolName, -1, 0);
		});

		// Find functions that should be external
		forEachSymbol([&](const Elf32_Sym& symbol, std::string_view symbolName){
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
		});

		for (GenericPatchInfo* p : patchInfoForThisObj)
		{
			if (p->sectionIdx == -1) // is patch instructed by label
				m_externSymbols.emplace_back(p->symbol);
		}
	}

	if (DEBUG_PRINT_HOOK_TABLE)
	{
		Log::out << "Hooks:\nSRC_ADDR, SRC_ADDR_OV, DST_ADDR, DST_ADDR_OV, PATCH_TYPE, SECTION_IDX, SECTION_SIZE, SYMBOL" << std::endl;
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

static void LDSAddSectionInclude(std::string& o, std::string& objPath, const char* secInc)
{
	o += "\t\t\"";
	o += objPath;
	o += "\" (.";
	o += secInc;
	o += ")\n";
}

void PatchMaker::createLinkerScript()
{
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
		if (dest == -1)
		{
			memEntry = new LDSMemoryEntry{ "arm", m_newcodeAddr, region->length };
		}
		else
		{
			u32 addr;
			switch (region->mode)
			{
			case BuildTarget::Mode::append:
				addr = m_ovtEntries[dest]->ramSize;
				break;
			case BuildTarget::Mode::replace:
				addr = m_ovtEntries[dest]->ramAddress;
				break;
			case BuildTarget::Mode::create:
				addr = region->address;
				break;
			}

			std::string memName; memName.reserve(8);
			memName += "ov";
			memName += std::to_string(dest);
			memEntry = new LDSMemoryEntry{ std::move(memName), addr, region->length };
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
						LDSAddSectionInclude(o, objPath, secInc);
				}
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
					LDSAddSectionInclude(o, objPath, "(.bss)");
					LDSAddSectionInclude(o, objPath, "(.bss.*)");
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
		o += " : { * (";
		o += p->info->symbol;
		o += ") } > ";
		o += p->memory->name;
		o += " AT > bin\n";
	}
	if (!overPatches.empty())
		o += '\n';

	o += "\t.ncp_set : { * (.ncp_set) } > ncp_set AT > bin\n\n"
		 "\t/DISCARD/ : {*(.*)}\n"
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

}
