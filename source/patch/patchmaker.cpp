#include "patchmaker.hpp"

#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>

#include "arenalofinder.hpp"

#include "../elf.hpp"

#include "../main.hpp"
#include "../log.hpp"
#include "../except.hpp"
#include "../config/buildconfig.hpp"
#include "../config/rebuildconfig.hpp"
#include "../util.hpp"
#include "../process.hpp"

/*
 * TODO: Endianness checks
 * */

#define OSTRa(x) ANSI_bWHITE "\"" << (x) << "\"" ANSI_RESET

namespace fs = std::filesystem;

constexpr std::size_t SizeOfHookBridge = 20;
constexpr std::size_t SizeOfArm2ThumbJumpBridge = 8;

constexpr u32 armOpcodeB = 0xEA000000; // B
constexpr u32 armOpcodeBL = 0xEB000000; // BL
constexpr u32 armOpCodeBLX = 0xFA000000; // BLX
constexpr u32 armHookPush = 0xE92D500F; // PUSH {R0-R3,R12,LR}
constexpr u32 armHookPop = 0xE8BD500F; // POP {R0-R3,R12,LR}
constexpr u16 thumbOpCodeBL0 = 0xF000; // BL
constexpr u16 thumbOpCodeBL1 = 0xF800; // <BL>
constexpr u16 thumbOpCodeBLX1 = 0xE800; // <BL>X
constexpr u16 thumbOpCodePushLR = 0xB500; // PUSH {LR}
constexpr u16 thumbOpCodePopPC = 0xBD00; // POP {PC}

struct PatchType {
	enum {
		Jump, Call, Hook, Over,
		SetJump, SetCall, SetHook,
		RtRepl,
		TJump, TCall, THook,
		SetTJump, SetTCall, SetTHook,
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
	bool isNcpSet; // if the patch is an ncp_set type patch
	bool srcThumb; // if the function of the symbol is thumb
	bool destThumb; // if the function to be patched is thumb
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
	const u8* binData;
	const u8* bssData;
	std::size_t binSize;
	std::size_t binAlign;
	std::size_t bssSize;
	std::size_t bssAlign;
};

struct AutogenDataInfo
{
	u32 address;
	u32 curAddress;
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
	std::size_t autogenDataSize;
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
	"rtrepl",
	"tjump", "tcall", "thook",
	"settjump", "settcall", "setthook"
};

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

static u32 getPatchOverwriteAmount(const GenericPatchInfo* p)
{
	std::size_t pt = p->patchType;
	if (pt == PatchType::Over)
		return p->sectionSize;
	if (pt == PatchType::Jump && p->destThumb)
		return 8;
	return 4;
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

	if (m_srcFileJobs->empty())
		throw ncp::exception("There are no source files to link.");

	createBuildDirectory();
	createBackupDirectory();

	loadArmBin();
	loadOverlayTableBin();

	std::vector<u32>& patchedOverlays = m_target->getArm9() ?
		RebuildConfig::getArm7PatchedOvs() :
		RebuildConfig::getArm9PatchedOvs();

	for (u32 ovID : patchedOverlays)
		loadOverlayBin(ovID);

	fetchNewcodeAddr();
	gatherInfoFromObjects();
	createLinkerScript();
	linkElfFile();
	loadElfFile();
	gatherInfoFromElf();
	applyPatchesToRom();
	unloadElfFile();

	patchedOverlays.clear();
	for (const auto& [id, ov] : m_loadedOverlays)
	{
		if (ov->getDirty())
			patchedOverlays.push_back(id);
	}

	saveOverlayBins();
	saveOverlayTableBin();
	saveArmBin();
}

void PatchMaker::fetchNewcodeAddr()
{
	ArmBin* arm = getArm();

	auto newcodeAddrFromMissingArenaLo = [&](){
		int arenaLo;
		ArenaLoFinder::findArenaLo(arm, arenaLo, m_newcodeAddrForDest[-1]);
		Log::out << OINFO << "Found ArenaLow at: " << std::uppercase << std::hex << arenaLo << std::endl;
	};

	if (m_target->arenaLo == 0)
	{
		Log::out << OINFO << OSTR("arenaLo") << " not specified, searching..." << std::endl;
		newcodeAddrFromMissingArenaLo();
	}
	else
	{
		u32 addr = arm->read<u32>(m_target->arenaLo);
		if (arm->sanityCheckAddress(addr))
		{
			m_newcodeAddrForDest[-1] = addr;
		}
		else
		{
			Log::out << OWARN << "Invalid " << OSTR("arenaLo") << " provided, searching..." << std::endl;
			newcodeAddrFromMissingArenaLo();
		}
	}
	
	for (auto& region : m_target->regions)
	{
		int dest = region.destination;
		if (dest != -1)
		{
			u32 addr;
			switch (region.mode)
			{
			case BuildTarget::Mode::Append:
			{
				auto& ovtEntry = m_ovtEntries[dest];
				addr = ovtEntry.ramAddress + ovtEntry.ramSize + ovtEntry.bssSize;
				break;
			}
			case BuildTarget::Mode::Replace:
			{
				addr = (region.address == 0xFFFFFFFF) ? m_ovtEntries[dest].ramAddress : region.address;
				break;
			}
			case BuildTarget::Mode::Create:
			{
				addr = region.address;
				break;
			}
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

		if (Main::getVerbose())
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
		const Elf32_Shdr* ncpSetSection = nullptr;
		const Elf32_Rel* ncpSetRel = nullptr;
		const Elf32_Sym* ncpSetRelSymTbl = nullptr;
		std::size_t ncpSetRelCount = 0;
		std::size_t ncpSetRelSymTblSize = 0;

		auto parseSymbol = [&](std::string_view symbolName, u32 symbolAddr, int sectionIdx, int sectionSize){
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

			bool forceThumb = false;
			if (patchType >= PatchType::TJump && patchType <= PatchType::THook)
			{
				patchType -= PatchType::TJump - PatchType::Jump;
				forceThumb = true;
			}
			else if (patchType >= PatchType::SetTJump && patchType <= PatchType::SetTHook)
			{
				patchType -= PatchType::SetTJump - PatchType::SetJump;
				forceThumb = true;
			}

			bool isNcpSet = false;
			if (patchType >= PatchType::SetJump && patchType <= PatchType::SetHook)
			{
				patchType -= PatchType::SetJump - PatchType::Jump;
				isNcpSet = true;
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
			if (forceThumb)
				destAddress |= 1;

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

			for (auto& region : m_target->regions)
			{
				if (region.destination == destAddressOv && region.mode != BuildTarget::Mode::Append)
				{
					std::ostringstream oss;
					oss << OSTRa(symbolName) << " (" << OSTR(srcFileJob->srcFilePath.string())
						<< ") cannot be applied to an overlay that is not in " << OSTRa("append") << " mode.";
					throw ncp::exception(oss.str());
				}
			}

			int srcAddressOv = patchType == PatchType::Over ? destAddressOv : region->destination;

			auto* patchInfoEntry = new GenericPatchInfo({
				.srcAddress = 0, // we do not yet know it, only after linkage
				.srcAddressOv = srcAddressOv,
				.destAddress = (destAddress & ~1),
				.destAddressOv = destAddressOv,
				.patchType = patchType,
				.sectionIdx = sectionIdx,
				.sectionSize = sectionSize,
				.isNcpSet = isNcpSet,
				.srcThumb = bool(symbolAddr & 1),
				.destThumb = bool(destAddress & 1),
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
				if (ncpSetSection == nullptr && sectionName.substr(5).starts_with("set"))
				{
					ncpSetSection = &section;
					int dest = region->destination;
					if (std::find(m_destWithNcpSet.begin(), m_destWithNcpSet.end(), dest) == m_destWithNcpSet.end())
						m_destWithNcpSet.emplace_back(dest);
					m_jobsWithNcpSet.emplace_back(srcFileJob.get());
					return false;
				}
				parseSymbol(sectionName, 0, int(sectionIdx), int(section.sh_size));
			}
			else if (ncpSetRel == nullptr && sectionName == ".rel.ncp_set")
			{
				ncpSetRel = elf.getSection<Elf32_Rel>(section);
				ncpSetRelCount = section.sh_size / sizeof(Elf32_Rel);
				ncpSetRelSymTbl = elf.getSection<Elf32_Sym>(sh_tbl[section.sh_link]);
				ncpSetRelSymTblSize = sh_tbl[section.sh_link].sh_size / sizeof(Elf32_Sym);
			}
			return false;
		});

		// Find the functions corresponding to the patch to check if they are thumb
		forEachElfSymbol(elf, eh, sh_tbl,
		[&](const Elf32_Sym& symbol, std::string_view symbolName){
			if (ELF32_ST_TYPE(symbol.st_info) == STT_FUNC)
			{
				for (GenericPatchInfo* p : patchInfoForThisObj)
				{
					// no need to check this condition because at this point all fetched patches are only section marked ones
					/*if (p->sectionIdx != -1) // is patch instructed by section
					{*/
					
					// if function has the same section as the patch instruction section
					if (p->sectionIdx == symbol.st_shndx)
					{
						p->srcThumb = symbol.st_value & 1;
						break;
					}
					
					//}
				}
			}
			return false;
		});

		// Find patches in symbols
		forEachElfSymbol(elf, eh, sh_tbl,
		[&](const Elf32_Sym& symbol, std::string_view symbolName){
			if (symbolName.starts_with("ncp_"))
			{
				std::string_view stemless = symbolName.substr(4);
				if (stemless != "dest")
				{
					u32 addr = symbol.st_value;
					if (stemless.starts_with("set")) // requires special care because of thumb function detection
					{
						if (ncpSetSection == nullptr)
							throw ncp::exception("Found an ncp_set hook, but an \".ncp_set\" section does not exist!");

						// Here we are just getting the address, so that we can know beforehand
						// if the function is thumb or not. The final address is obtained after linkage.
						auto& section = sh_tbl[symbol.st_shndx];
						auto sectionData = elf.getSection<char>(section);
						if (ncpSetRel == nullptr)
						{
							addr = Util::read<u32>(&sectionData[addr - section.sh_addr]);
						}
						else
						{
							for (int relIdx = 0; relIdx < ncpSetRelCount; relIdx++)
							{
								const Elf32_Rel& rel = ncpSetRel[relIdx];
								if (rel.r_offset == addr) // found the corresponding relocation
								{
									// layer after layer, we finally reach the symbol :p
									std::size_t symIdx = ELF32_R_SYM(rel.r_info);
									if (symIdx >= ncpSetRelSymTblSize)
									{
										std::ostringstream oss;
										oss << "Relocation entry with index " << relIdx
											<< " in " << OSTR(".rel.ncp_set") << " section has an index of " << symIdx
											<< " as linked symbol table entry but the symbol table only contains "
											<< ncpSetRelSymTblSize << " entries.";
										throw ncp::exception(oss.str());
									}
									addr = ncpSetRelSymTbl[symIdx].st_value;
									break;
								}
							}
						}
					}
					parseSymbol(symbolName, addr, -1, 0);
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

		if (Main::getVerbose())
		{
			if (patchInfoForThisObj.empty())
			{
				Log::out << "NO PATCHES" << std::endl;
			}
			else
			{
				Log::out << "SRC_ADDR_OV, DST_ADDR, DST_ADDR_OV, PATCH_TYPE, SEC_IDX, SEC_SIZE, NCP_SET, SRC_THUMB, DST_THUMB, SYMBOL" << std::endl;
				for (auto& p : patchInfoForThisObj)
				{
					Log::out <<
						std::setw(11) << std::dec << p->srcAddressOv << "  " <<
						std::setw(8) << std::hex << p->destAddress << "  " <<
						std::setw(11) << std::dec << p->destAddressOv << "  " <<
						std::setw(10) << s_patchTypeNames[p->patchType] << "  " <<
						std::setw(7) << std::dec << p->sectionIdx << "  " <<
						std::setw(8) << std::dec << p->sectionSize << "  " <<
						std::setw(7) << std::boolalpha << p->isNcpSet << "  " <<
						std::setw(9) << std::boolalpha << p->srcThumb << "  " <<
						std::setw(9) << std::boolalpha << p->destThumb << "  " <<
						std::setw(6) << p->symbol << std::endl;
				}
			}
		}
	}
	if (Main::getVerbose())
	{
		if (m_externSymbols.empty())
		{
			Log::out << "\nExternal symbols: NONE" << std::endl;
		}
		else
		{
			Log::out << "\nExternal symbols:\n";
			for (const std::string& sym : m_externSymbols)
				Log::out << sym << '\n';
			Log::out << std::flush;
		}
	}
}

void PatchMaker::createBuildDirectory()
{
	fs::current_path(Main::getWorkPath());
	const fs::path& buildDir = *m_buildDir;
	if (!fs::exists(buildDir))
	{
		if (!fs::create_directories(buildDir))
		{
			std::ostringstream oss;
			oss << "Could not create build directory: " << OSTR(buildDir);
			throw ncp::exception(oss.str());
		}
	}
}

void PatchMaker::createBackupDirectory()
{
	fs::current_path(Main::getWorkPath());
	const fs::path& bakDir = BuildConfig::getBackupDir();
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
	Log::info("Loading overlay table...");

	const char* binName = m_target->getArm9() ? "arm9ovt.bin" : "arm7ovt.bin";

	fs::current_path(Main::getWorkPath());

	fs::path bakBinName = BuildConfig::getBackupDir() / binName;

	fs::path workBinName;
	if (fs::exists(bakBinName)) //has backup
	{
		workBinName = bakBinName;
	}
	else //has no backup
	{
		fs::current_path(Main::getRomPath());
		if (!fs::exists(binName))
			throw ncp::file_error(binName, ncp::file_error::find);
		workBinName = binName;
	}

	uintmax_t fileSize = fs::file_size(workBinName);
	u32 overlayCount = fileSize / sizeof(OvtEntry);

	m_ovtEntries.resize(overlayCount);

	std::ifstream inputFile(workBinName, std::ios::binary);
	if (!inputFile.is_open())
		throw ncp::file_error(workBinName, ncp::file_error::read);
	for (u32 i = 0; i < overlayCount; i++)
		inputFile.read(reinterpret_cast<char*>(&m_ovtEntries[i]), sizeof(OvtEntry));
	inputFile.close();

	m_bakOvtEntries.resize(m_ovtEntries.size());
	std::memcpy(m_bakOvtEntries.data(), m_ovtEntries.data(), m_ovtEntries.size() * sizeof(OvtEntry));
}

void PatchMaker::saveOverlayTableBin()
{
	auto saveOvtEntries = [](const std::vector<OvtEntry>& ovtEntries, const fs::path& filePath){
		std::ofstream outputFile(filePath, std::ios::binary);
		if (!outputFile.is_open())
			throw ncp::file_error(filePath, ncp::file_error::write);
		outputFile.write(reinterpret_cast<const char*>(ovtEntries.data()), ovtEntries.size() * sizeof(OvtEntry));
		outputFile.close();
	};

	const char* binName = m_target->getArm9() ? "arm9ovt.bin" : "arm7ovt.bin";

	if (m_bakOvtChanged)
	{
		fs::current_path(Main::getWorkPath());
		saveOvtEntries(m_bakOvtEntries, BuildConfig::getBackupDir() / binName);
	}

	fs::current_path(Main::getRomPath());
	saveOvtEntries(m_ovtEntries, binName);
}

OverlayBin* PatchMaker::loadOverlayBin(std::size_t ovID)
{
	std::string prefix = m_target->getArm9() ? "overlay9" : "overlay7";

	fs::current_path(Main::getWorkPath());

	fs::path binName = fs::path(prefix) / (prefix + "_" + std::to_string(ovID) + ".bin");
	fs::path bakBinName = BuildConfig::getBackupDir() / binName;

	OvtEntry& ovte = m_ovtEntries[ovID];

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

		std::vector<u8>& backupBytes = overlay->backupData();
		backupBytes.resize(bytes.size());
		std::memcpy(backupBytes.data(), bytes.data(), bytes.size());

		m_bakOvtEntries[ovID].flag = 0;
		m_bakOvtChanged = true;
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

	for (auto& [ovID, ov] : m_loadedOverlays)
	{
		fs::path binName = fs::path(prefix) / (prefix + "_" + std::to_string(ovID) + ".bin");

		auto saveOvData = [](const std::vector<u8>& ovData, const fs::path& ovFilePath){
			std::ofstream outputFile(ovFilePath, std::ios::binary);
			if (!outputFile.is_open())
				throw ncp::file_error(ovFilePath, ncp::file_error::write);
			outputFile.write(reinterpret_cast<const char*>(ovData.data()), std::streamsize(ovData.size()));
			outputFile.close();
		};

		fs::current_path(Main::getRomPath());
		saveOvData(ov->data(), binName);

		if (!ov->backupData().empty())
		{
			fs::current_path(Main::getWorkPath());
			saveOvData(ov->backupData(), BuildConfig::getBackupDir() / binName);
		}
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
	fs::path symbolsFile;
	if (!m_target->symbols.empty())
		symbolsFile = fs::absolute(m_target->symbols);

	fs::current_path(Main::getWorkPath());

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

	std::vector<int> orderedDestWithNcpSet = m_destWithNcpSet;
	std::sort(orderedDestWithNcpSet.begin(), orderedDestWithNcpSet.end(), [](int a, int b){
		return a > b;
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
		regionEntries.emplace_back(new LDSRegionEntry{ dest, memEntry, region, 0 });
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
				if (ldsRegion->dest == info->job->region->destination)
				{
					if (info->sectionIdx != -1)
						ldsRegion->sectionPatches.emplace_back(info.get());

					if (info->patchType == PatchType::Hook)
					{
						ldsRegion->autogenDataSize += SizeOfHookBridge;
					}
					else if (info->patchType == PatchType::Jump)
					{
						if (!info->destThumb && info->srcThumb) // ARM -> THUMB
							ldsRegion->autogenDataSize += SizeOfArm2ThumbJumpBridge;
					}
				}
			}
		}
	}

	if (!orderedDestWithNcpSet.empty())
		memoryEntries.emplace_back(new LDSMemoryEntry{ "ncp_set", 0, 0x100000 });

	std::string o;
	o.reserve(65536);

	o += "/* NCPatcher: Auto-generated linker script */\n\n";

	if (!symbolsFile.empty())
	{
		o += "INCLUDE \"";
		o += fs::relative(symbolsFile).string();
		o += "\"\n\n";
	}
	
	o += "INPUT (\n";
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
			o += " = .;\n\t\tKEEP(* (";
			o += p->symbol;
			o += "))\n";
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
			if (s->autogenDataSize != 0)
			{
				o += "\t\t. = ALIGN(4);\n"
					 "\t\tncp_autogendata = .;\n"
					 "\t\tFILL(0)\n"
					 "\t\t. = ncp_autogendata + ";
				o += std::to_string(s->autogenDataSize);
				o += ";\n";
			}
		}
		else
		{
			for (auto& f : *m_srcFileJobs)
			{
				if (f->region == s->region)
				{
					std::string objPath = fs::relative(f->objFilePath).string();
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
			if (s->autogenDataSize)
			{
				o += "\t\t. = ALIGN(4);\n\t\tncp_autogendata_";
				o += s->memory->name;
				o += " = .;\n\t\tFILL(0)\n\t\t. = ncp_autogendata_";
				o += s->memory->name;
				o += " + ";
				o += std::to_string(s->autogenDataSize);
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
					std::string objPath = fs::relative(f->objFilePath).string();
					addSectionInclude(o, objPath, "bss");
					addSectionInclude(o, objPath, "bss.*");
				}
			}
		}
		o += "\t\t. = ALIGN(4);\n"
			 "\t} > ";
		o += s->memory->name;
		o += " AT > bin\n\n";
	}

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

	for (auto& p : orderedDestWithNcpSet)
	{
		o += "\t.ncp_set";
		if (p == -1)
		{
			o += " : { KEEP(* (.ncp_set)) } > ncp_set AT > bin\n\n";
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
					o += fs::relative(j->objFilePath).string();
					o += "\" (.ncp_set))\n\t"
						 "} > ncp_set AT > bin\n\n";
				}
			}
		}
	}

	o += "\t/DISCARD/ : {*(.*)}\n"
		 "}\n";

	if (!m_externSymbols.empty())
	{
		o += "\nEXTERN (\n";
		for (auto& e : m_externSymbols)
		{
			o += '\t';
			o += e;
			o += '\n';
		}
		o += ")\n";
	}

	// Output the file
	std::ofstream outputFile(m_ldscriptPath);
	if (!outputFile.is_open())
		throw ncp::file_error(m_ldscriptPath, ncp::file_error::write);
	outputFile.write(o.data(), std::streamsize(o.length()));
	outputFile.close();
}

std::string PatchMaker::ldFlagsToGccFlags(std::string flags)
{
	std::size_t cpos = 0;
	while ((cpos = flags.find(' ', cpos)) != std::string::npos)
	{
		std::size_t dpos = flags.find('-', cpos);
		if (dpos == std::string::npos)
			break;
		flags.replace(cpos, dpos - cpos, ",");
		cpos += 2;
	}
	return flags;
}

void PatchMaker::linkElfFile()
{
	Log::out << OLINK << "Linking the ARM binary..." << std::endl;

	fs::current_path(Main::getWorkPath());

	std::string ccmd;
	ccmd.reserve(64);
	ccmd += BuildConfig::getToolchain();
	ccmd += "gcc -nostartfiles -Wl,--gc-sections,-T\"";
	ccmd += fs::relative(m_ldscriptPath).string();
	ccmd += '\"';
	std::string targetFlags = ldFlagsToGccFlags(m_target->ldFlags);
	if (!targetFlags.empty())
		ccmd += ',';
	ccmd += targetFlags;

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
	Log::info("Getting patches from elf...");

	const Elf32_Ehdr& eh = m_elf->getHeader();
	auto sh_tbl = m_elf->getSectionHeaderTable();
	auto str_tbl = m_elf->getSection<char>(sh_tbl[eh.e_shstrndx]);

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
		if (symbolName.starts_with("ncp_autogendata"))
		{
			int srcAddrOv = -1;
			if (symbolName.length() != 15 && symbolName.substr(15).starts_with("_ov"))
			{
				try {
					srcAddrOv = std::stoi(std::string(symbolName.substr(18)));
				} catch (std::exception& e) {
					Log::out << OWARN << "Found invalid overlay parsing ncp_autogendata symbol: " << symbolName << std::endl;
					return false;
				}
			}
			auto* info = new AutogenDataInfo();
			info->address = symbol.st_value;
			info->curAddress = symbol.st_value;
			m_autogenDataInfoForDest.emplace(srcAddrOv, info);
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

			for (auto& p : m_patchInfo)
			{
				if (p->isNcpSet && p->srcAddressOv == srcAddrOv)
				{
					u32 dataOffset = p->srcAddress - section.sh_addr;
					if (dataOffset + 4 > section.sh_size)
					{
						std::ostringstream oss;
						oss << "Tried to read " << OSTR(sectionName) << " data out of bounds.";
						throw ncp::exception(oss.str());
					}
					p->srcAddress = Util::read<u32>(&sectionData[dataOffset]);
				}
			}
		}
		return false;
	});

	// Check if any overlapping patches exist
	bool foundOverlapping = false;
	for (std::size_t i = 0; i < m_patchInfo.size(); i++)
	{
		auto& a = m_patchInfo[i];
        for (std::size_t j = i + 1; j < m_patchInfo.size(); j++)
		{
			auto& b = m_patchInfo[j];
			if (a->destAddressOv != b->destAddressOv)
				continue;
			u32 aSz = getPatchOverwriteAmount(a.get());
			u32 bSz = getPatchOverwriteAmount(b.get());
			if (Util::overlaps(a->destAddress, a->destAddress + aSz, b->destAddress, b->destAddress + bSz))
			{
				Log::out << OERROR
					<< OSTRa(a->symbol) << "[sz=" << aSz << "] (" << OSTR(a->job->srcFilePath.string()) << ") overlaps with "
					<< OSTRa(b->symbol) << "[sz=" << bSz << "] (" << OSTR(b->job->srcFilePath.string()) << ")\n";
				foundOverlapping = true;
			}
        }
    }
	if (foundOverlapping)
		throw ncp::exception("Overlapping patches were detected.");
	
	if (Main::getVerbose())
	{
		Log::out << "Patches:\nSRC_ADDR, SRC_ADDR_OV, DST_ADDR, DST_ADDR_OV, PATCH_TYPE, SEC_IDX, SEC_SIZE, NCP_SET, SRC_THUMB, DST_THUMB, SYMBOL" << std::endl;
		for (auto& p : m_patchInfo)
		{
			Log::out <<
				std::setw(8) << std::hex << p->srcAddress << "  " <<
				std::setw(11) << std::dec << p->srcAddressOv << "  " <<
				std::setw(8) << std::hex << p->destAddress << "  " <<
				std::setw(11) << std::dec << p->destAddressOv << "  " <<
				std::setw(10) << s_patchTypeNames[p->patchType] << "  " <<
				std::setw(7) << std::dec << p->sectionIdx << "  " <<
				std::setw(8) << std::dec << p->sectionSize << "  " <<
				std::setw(7) << std::boolalpha << p->isNcpSet << "  " <<
				std::setw(9) << std::boolalpha << p->srcThumb << "  " <<
				std::setw(9) << std::boolalpha << p->destThumb << "  " <<
				std::setw(6) << p->symbol << std::endl;
		}
	}

	forEachElfSection(eh, sh_tbl, str_tbl,
	[&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
		auto insertSection = [&](int dest, bool isBss){
			auto& newcodeInfo = m_newcodeDataForDest[dest];
			if (newcodeInfo == nullptr)
			    newcodeInfo = std::make_unique<NewcodePatch>();

			(isBss ? newcodeInfo->bssData : newcodeInfo->binData) = m_elf->getSection<u8>(section);
			(isBss ? newcodeInfo->bssSize : newcodeInfo->binSize) = section.sh_size;
			(isBss ? newcodeInfo->bssAlign : newcodeInfo->binAlign) = section.sh_addralign;
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
				insertSection(dest, sectionName.substr(pos + 1) == "bss");
			}
		}
		return false;
	});

	if (Main::getVerbose())
	{
		Log::out << "New Code Info:\nNAME    CODE_SIZE    BSS_SIZE" << std::endl;
		for (const auto& [dest, newcodeInfo] : m_newcodeDataForDest)
		{
			Log::out <<
				std::setw(8) << std::left << (dest == -1 ? "ARM" : ("OV" + std::to_string(dest))) << std::right <<
				std::setw(9) << std::dec << newcodeInfo->binSize << "    " <<
				std::setw(8) << std::dec << newcodeInfo->bssSize << std::endl;
		}
	}
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

u32 PatchMaker::makeJumpOpCode(u32 opCode, u32 fromAddr, u32 toAddr)
{
	return opCode | ((((toAddr - fromAddr) >> 2) - 2) & 0xFFFFFF);
}

u32 PatchMaker::makeThumbCallOpCode(bool exchange, u32 fromAddr, u32 toAddr)
{
	// toAddr in BLX is always ARM, so it is a multiple of 4
	// BLX executes in ARM mode, so the low/high offset of fromAddr must be discarded
	u32 offset = ((toAddr - (exchange ? (fromAddr & ~3) : fromAddr)) >> 1) - 2;
	u16 opcode0 = thumbOpCodeBL0 | (offset & 0x3FF800) >> 11;
	u16 opcode1 = (exchange ? thumbOpCodeBLX1 : thumbOpCodeBL1) | (offset & 0x7FF);
	return (u32(opcode1) << 16) | opcode0;
}

u32 PatchMaker::fixupOpCode(u32 opCode, u32 ogAddr, u32 newAddr)
{
	// TODO: check for other relative instructions other than B and BL, like LDR
	if (((opCode >> 25) & 0b111) == 0b101)
	{
		u32 opCodeBase = opCode & 0xFF000000;
		u32 toAddr = (((opCode & 0xFFFFFF) + 2) << 2) + ogAddr;
		return makeJumpOpCode(opCodeBase, newAddr, toAddr);
	}
	return opCode;
}

void PatchMaker::applyPatchesToRom()
{
	Main::setErrorContext(m_target->getArm9() ?
		"Failed to apply patches for ARM9 target." :
		"Failed to apply patches for ARM7 target.");

	Log::info("Patching the binaries...");

	auto failInject = [](std::unique_ptr<GenericPatchInfo>& p, bool srcThumb, bool destThumb, const char* injectType){
		std::ostringstream oss;
		oss << "Injecting " << injectType << " from " << (destThumb ? "THUMB" : "ARM") << " to "
			<< (srcThumb ? "THUMB" : "ARM") << " is not supported, at "
			<< OSTRa(p->symbol) << " (" << OSTR(p->job->srcFilePath.string()) << ")";
		throw ncp::exception(oss.str());
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
			if (!p->destThumb && !p->srcThumb) // ARM -> ARM
			{
				bin->write<u32>(p->destAddress, makeJumpOpCode(armOpcodeB, p->destAddress, p->srcAddress));
			}
			else if (!p->destThumb && p->srcThumb) // ARM -> THUMB
			{
				/*
				 * If the patch type is a ARM to THUMB jump, the instruction at
				 * destAddr must become a jump to a ARM to THUMB jump bridge generated
				 * by NCPatcher and it should look as such:
				 *
				 * arm2thumb_jump_bridge:
				 *     LDR   PC, [PC,#-4]
				 *     .int: srcAddr+1
				 * */
				
				auto& info = m_autogenDataInfoForDest[p->srcAddressOv];
				if (info == nullptr)
					throw ncp::exception("Unexpected p->srcAddressOv for m_autogenDataInfoForDest encountered.");

				std::vector<u8>& bridgeData = info->data;
				std::size_t offset = bridgeData.size();
				bridgeData.resize(offset + SizeOfArm2ThumbJumpBridge);

				u32 bridgeAddr = info->curAddress;

				if (Main::getVerbose())
					Log::out << "ARM->THUMB BRIDGE: " << Util::intToAddr(bridgeAddr, 8) << std::endl;

				bin->write<u32>(p->destAddress, makeJumpOpCode(armOpcodeB, p->destAddress, bridgeAddr));

				u8* bridgeDataPtr = bridgeData.data() + offset;

				Util::write<u32>(bridgeDataPtr, 0xE51FF004);            // LDR PC, [PC,#-4]
				Util::write<u32>(bridgeDataPtr + 4, p->srcAddress | 1); // int value to jump to

				if (Main::getVerbose())
					Util::printDataAsHex(bridgeData.data() + offset, SizeOfArm2ThumbJumpBridge, 32);

				info->curAddress += SizeOfArm2ThumbJumpBridge;
			}
			else if (p->destThumb && !p->srcThumb) // THUMB -> ARM
			{
				u16 patchData[4];
				patchData[0] = thumbOpCodePushLR;
				Util::write<u32>(&patchData[1], makeThumbCallOpCode(true, p->destAddress + 2, p->srcAddress));
				patchData[3] = thumbOpCodePopPC;
				bin->writeBytes(p->destAddress, patchData, 8);
			}
			else // THUMB -> THUMB
			{
				u16 patchData[4];
				patchData[0] = thumbOpCodePushLR;
				Util::write<u32>(&patchData[1], makeThumbCallOpCode(false, p->destAddress + 2, p->srcAddress));
				patchData[3] = thumbOpCodePopPC;
				bin->writeBytes(p->destAddress, patchData, 8);
			}
			break;
		}
		case PatchType::Call:
		{
			if (p->destThumb != p->srcThumb && !m_target->getArm9())
			{
				std::ostringstream oss;
				oss << "Cannot create thumb-interworking veneer: BLX not supported on armv4. At "
					<< OSTRa(p->symbol) << " (" << OSTR(p->job->srcFilePath.string()) << ")";
				throw ncp::exception(oss.str());
			}

			if (!p->destThumb && !p->srcThumb) // ARM -> ARM
			{
				bin->write<u32>(p->destAddress, makeJumpOpCode(armOpcodeBL, p->destAddress, p->srcAddress));
			}
			else if (!p->destThumb && p->srcThumb) // ARM -> THUMB
			{
				u32 opcode = armOpCodeBLX | (((p->srcAddress % 4) >> 1) << 23);
				bin->write<u32>(p->destAddress, makeJumpOpCode(opcode, p->destAddress, p->srcAddress));
			}
			else if (p->destThumb && !p->srcThumb) // THUMB -> ARM
			{
				bin->write<u32>(p->destAddress, makeThumbCallOpCode(true, p->destAddress, p->srcAddress));
			}
			else // THUMB -> THUMB
			{
				bin->write<u32>(p->destAddress, makeThumbCallOpCode(false, p->destAddress, p->srcAddress));
			}
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
			 *     BL   srcAddr        @ BLX if srcAddr is THUMB
			 *     POP  {R0-R3,R12}
			 *     <unpatched destAddr's instruction>
			 *     B    (destAddr + 4)
			 * */

			if (p->destThumb)
				failInject(p, p->srcThumb, p->destThumb, "hook");

			// ARM -> ARM && ARM -> THUMB

			u32 ogOpCode = bin->read<u32>(p->destAddress);

			auto& info = m_autogenDataInfoForDest[p->srcAddressOv];
			if (info == nullptr)
				throw ncp::exception("Unexpected p->srcAddressOv for m_autogenDataInfoForDest encountered.");

			std::vector<u8>& hookData = info->data;
			std::size_t offset = hookData.size();
			hookData.resize(offset + SizeOfHookBridge);

			u32 hookBridgeAddr = info->curAddress;

			if (Main::getVerbose())
				Log::out << "HOOK BRIDGE: " << Util::intToAddr(hookBridgeAddr, 8) << std::endl;

			bin->write<u32>(p->destAddress, makeJumpOpCode(armOpcodeB, p->destAddress, hookBridgeAddr));

			u8* hookDataPtr = hookData.data() + offset;

			u32 jmpOpCode = p->srcThumb ? (armOpCodeBLX | (((p->srcAddress % 4) >> 1) << 23)) : armOpcodeBL;

			Util::write<u32>(hookDataPtr, armHookPush);
			Util::write<u32>(hookDataPtr + 4, makeJumpOpCode(jmpOpCode, hookBridgeAddr + 4, p->srcAddress));
			Util::write<u32>(hookDataPtr + 8, armHookPop);
			Util::write<u32>(hookDataPtr + 12, fixupOpCode(ogOpCode, p->destAddress, hookBridgeAddr + 12));
			Util::write<u32>(hookDataPtr + 16, makeJumpOpCode(armOpcodeB, hookBridgeAddr + 16, p->destAddress + 4));

			if (Main::getVerbose())
				Util::printDataAsHex(hookData.data() + offset, SizeOfHookBridge, 32);

			info->curAddress += SizeOfHookBridge;
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
	
	for (const auto& [dest, newcodeInfo] : m_newcodeDataForDest)
	{
		u32 newcodeAddr = m_newcodeAddrForDest[dest];

		// Fixes clang bug: https://marc.info/?l=llvm-bugs&m=154455815918394
		const auto& clang_dest = dest;
		const auto& clang_newcodeInfo = newcodeInfo;

		auto writeNewcode = [&](u8* addr){
			std::size_t autogenDataSize = 0;
			auto& autogenDataInfo = m_autogenDataInfoForDest[clang_dest];
			if (autogenDataInfo != nullptr)
				autogenDataSize = autogenDataInfo->data.size();

			// Write the patch data
			std::memcpy(addr, clang_newcodeInfo->binData, clang_newcodeInfo->binSize - autogenDataSize);
			if (autogenDataSize != 0)
				std::memcpy(&addr[clang_newcodeInfo->binSize - autogenDataSize], autogenDataInfo->data.data(), autogenDataSize);
		};

		if (dest == -1)
		{
			// If more data needs to be added
			if ((newcodeInfo->binSize + newcodeInfo->bssSize) != 0)
			{
				ArmBin* bin = getArm();
				std::vector<u8>& data = bin->data();

				// Extend the ARM binary
				data.resize(data.size() + newcodeInfo->binSize + 12);

				// Write the new relocated code address
				u32 heapReloc = newcodeAddr + newcodeInfo->binSize + (newcodeInfo->bssAlign - newcodeInfo->binSize % newcodeInfo->bssAlign) + newcodeInfo->bssSize;
				bin->write<u32>(m_target->arenaLo, heapReloc);

				ArmBin::ModuleParams* moduleParams = bin->getModuleParams();
				u32 ramAddress = bin->getRamAddress();

				u32 autoloadListStart = moduleParams->autoloadListStart;
				u32 autoloadListEnd = moduleParams->autoloadListEnd;
				u32 binAutoloadListStart = moduleParams->autoloadListStart - ramAddress; // Where our new code will be placed
				u32 binAutoloadListEnd = moduleParams->autoloadListEnd - ramAddress;
				u32 binAutoloadStart = moduleParams->autoloadStart - ramAddress;

				std::vector<ArmBin::AutoLoadEntry>& autoloadList = bin->getAutoloadList();
				autoloadList.insert(autoloadList.begin(), ArmBin::AutoLoadEntry{
					.address = newcodeAddr,
					.size = u32(newcodeInfo->binSize),
					.bssSize = u32(newcodeInfo->bssSize),
					.dataOff = binAutoloadStart
				});

				// Write the new data
				if (newcodeInfo->binSize != 0)
				{
					// Move/offset the old code by the size of our patch
					std::memcpy(&data[binAutoloadStart + newcodeInfo->binSize], &data[binAutoloadStart], binAutoloadListStart - binAutoloadStart);

					writeNewcode(&data[binAutoloadStart]);
				}

				// Set the new autoload list location
				moduleParams->autoloadListStart = autoloadListStart + newcodeInfo->binSize;
				moduleParams->autoloadListEnd = autoloadListEnd + newcodeInfo->binSize + 12;

				// Write the new autoload list after the new code
				u8* writeAutoloadPtr = data.data() + binAutoloadListStart + newcodeInfo->binSize;
				for (ArmBin::AutoLoadEntry& entry : autoloadList)
				{
					u32 entryData[3];
					entryData[0] = entry.address;
					entryData[1] = entry.size;
					entryData[2] = entry.bssSize;
					std::memcpy(writeAutoloadPtr, entryData, 12);
					writeAutoloadPtr += 12;
				}
			}
		}
		else
		{
			const BuildTarget::Region* region = nullptr;
			for (const BuildTarget::Region& r : m_target->regions)
			{
				if (r.destination == dest)
				{
					region = &r;
					break;
				}
			}
			if (region == nullptr)
				throw ncp::exception("region of overlay " + std::to_string(dest) + " set to add code could not be found!");

			switch (region->mode)
			{
			case BuildTarget::Mode::Append:
			{
				OverlayBin* bin = getOverlay(dest);
				auto& ovtEntry = m_ovtEntries[dest];

				ovtEntry.compressed = 0; // size of compressed "ramSize"
				ovtEntry.flag = 0;

				std::vector<u8>& data = bin->data();
				std::size_t szData = data.size();

				std::size_t totalOvSize = szData + ovtEntry.bssSize + newcodeInfo->binSize + newcodeInfo->bssSize;
				if (totalOvSize > region->length)
				{
					throw ncp::exception("Overlay " + std::to_string(dest) + " exceeds max length of "
						+ std::to_string(region->length) + " bytes, got " + std::to_string(totalOvSize) + " bytes.");
				}

				if (newcodeInfo->binSize > 0)
				{
					std::size_t newSzData = szData + ovtEntry.bssSize + newcodeInfo->binSize;
					data.resize(newSzData);
					u8* pData = data.data();
					std::memset(&pData[szData], 0, ovtEntry.bssSize); // Keep original BSS as data
					writeNewcode(&pData[szData + ovtEntry.bssSize]); // Write new code after BSS
					ovtEntry.ramSize = newSzData;
					ovtEntry.bssSize = newcodeInfo->bssSize; // Set the BSS to our new code BSS
				}
				else
				{
					ovtEntry.bssSize += newcodeInfo->bssSize;
				}

				bin->setDirty(true);
				break;
			}
			case BuildTarget::Mode::Replace:
			{
				OverlayBin* bin = getOverlay(dest);
				auto& ovtEntry = m_ovtEntries[dest];

				ovtEntry.ramAddress = newcodeAddr;
				ovtEntry.ramSize = newcodeInfo->binSize;
				ovtEntry.bssSize = newcodeInfo->bssSize;
				ovtEntry.sinitStart = 0;
				ovtEntry.sinitEnd = 0;
				ovtEntry.compressed = 0; // size of compressed "ramSize"
				ovtEntry.flag = 0;

				std::size_t totalOvSize = newcodeInfo->binSize + newcodeInfo->bssSize;
				if (totalOvSize > region->length)
				{
					throw ncp::exception("Overlay " + std::to_string(dest) + " exceeds max length of "
						+ std::to_string(region->length) + " bytes, got " + std::to_string(totalOvSize) + " bytes.");
				}

				std::vector<u8>& data = bin->data();
				
				// Write the new data
				if (newcodeInfo->binSize == 0)
				{
					data.clear();
				}
				else
				{
					data.resize(newcodeInfo->binSize);
					writeNewcode(data.data());
				}

				bin->setDirty(true);
				break;
			}
			case BuildTarget::Mode::Create:
			{
				// TO BE DESIGNED.
				throw ncp::exception("Creating new overlays is not yet supported.");
				break;
			}
			}
		}
	}

	Main::setErrorContext(nullptr);
}
