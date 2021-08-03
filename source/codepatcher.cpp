#include "codepatcher.hpp"

#include <fstream>

#include "except.hpp"
#include "global.hpp"
#include "common.hpp"
#include "process.hpp"
#include "oansistream.hpp"

namespace fs = std::filesystem;

static const char* MainPrefix = "Main";
static const char* MainFilePrefix = "main";
static const char* OverlayPrefix = "Overlay ";
static const char* OverlayFilePrefix = "ov";

enum PatchType
{
	Jump = 0,
	Call,
	Hook,
	Over,
	Set,
	RtRepl
};

struct U32Patch
{
	int ov;
	u32 addr;
	u32 data;
};

struct DATAPatch
{
	int ov;
	u32 addr;
	std::vector<u8> data;
};

static void RemoveLines(std::string& str, size_t count);

CodePatcher::CodePatcher(const BuildTarget& target, const std::vector<CodeMaker::BuiltRegion>& builtRegions, int proc)
	: target(target)
	, builtRegions(builtRegions)
	, proc(proc)
{
	createBackupDirectory();

	loadArmBin();
	loadOverlayTableBin();

	newcodeAddr = arm->read<u32>(target.arenaLo);

	ncp::setErrorMsg("Could not link the final binary.");

	fs::current_path(ncp::asm_path);
	fs::current_path(proc == 7 ? ncp::build_cfg.arm7 : ncp::build_cfg.arm9);

	for (const CodeMaker::BuiltRegion& builtRegion : builtRegions)
	{
		const BuildTarget::Region* region = builtRegion.region;

		int dest = region->destination;
		std::string destName;
		std::string destFileName;
		if (dest == -1) {
			destName = MainPrefix;
			destFileName = MainFilePrefix;
		}
		else {
			std::string ovNumStr = std::to_string(dest);
			destName = OverlayPrefix + ovNumStr;
			destFileName = OverlayFilePrefix + ovNumStr;
		}

		ansi::cout << OLINK << "Working on: " ANSI_bCYAN << destName << ANSI_RESET << std::endl;

		fs::path lsPath = target.build / (destFileName + ".ld");
		fs::path elfPath = target.build / (destFileName + ".elf");
		fs::path binPath = target.build / (destFileName + ".bin");

		bool doBuildElf = builtRegion.rebuildElf || !fs::exists(elfPath);
		bool doBuildBin = doBuildElf || !fs::exists(binPath);

		if (doBuildElf)
			buildElf(builtRegion, lsPath, elfPath);

		if (doBuildBin)
		{
			ansi::cout << OLINK << "  Making binary..." << std::endl;

			std::string cmd = ncp::build_cfg.prefix + "objcopy -O binary \"" + elfPath.string() + "\" \"" + binPath.string() + "\"";
			int retcode = process::start(cmd.c_str(), &ansi::cout);
			ansi::cout << std::flush;
			if (retcode != 0)
				throw ncp::exception("Failed to convert elf to bin.");
		}

		if (dest == -1)
		{
			fs::path symPath = target.build / (destFileName + ".sym");
			bool doBuildSym = doBuildElf || !fs::exists(symPath);
			if (doBuildSym)
			{
				ansi::cout << OLINK << "  Dumping symbols..." << std::endl;

				std::string cmd = ncp::build_cfg.prefix + "nm \"" + elfPath.string() + "\"";
				std::ostringstream oss;
				int retcode = process::start(cmd.c_str(), &oss);
				if (retcode != 0)
					throw ncp::exception("Failed to dump elf symbols.");

				std::string out = oss.str();
				std::string symTable;

				std::istringstream iss(out);
				std::string line;
				while (std::getline(iss, line))
				{
					size_t addrEnd = line.find_first_of(' ');
					if (addrEnd == std::string::npos)
						continue;
					size_t symTypeStart = line.find_first_not_of(' ', addrEnd);
					if (symTypeStart == std::string::npos)
						continue;
					size_t symTypeEnd = line.find_first_of(' ', symTypeStart);
					if (symTypeEnd == std::string::npos)
						continue;
					size_t symNameStart = line.find_first_not_of(' ', symTypeEnd);
					if (symNameStart == std::string::npos)
						continue;

					std::string_view lineV = std::string_view(line);
					std::string_view symType = lineV.substr(symTypeStart, symTypeEnd - symTypeStart);
					if (symType == "T" || symType == "B")
						symTable += line.substr(symNameStart) + " = 0x" + line.substr(0, addrEnd) + ";\n";
				}

				std::ofstream symTableFile(symPath);
				if (!symTableFile.is_open())
					throw ncp::file_error(symPath, ncp::file_error::write);
				symTableFile << symTable;
				symTableFile.close();
			}
		}
	}

	// process patches

	// load overlays

	// apply patches

	saveOverlayTableBin();
}

CodePatcher::~CodePatcher()
{
	delete arm;
	for (auto[id, ov] : loadedOverlays)
		delete ov;
}

void CodePatcher::loadArmBin()
{
	BuildCfg& bcfg = ncp::build_cfg;
	NDSHeader& hbin = ncp::header_bin;

	const char* binname;
	u32 entryAddress, ramAddress, autoLoadListHookOffset;
	if (proc == 7)
	{
		binname = "arm7.bin";
		entryAddress = hbin.arm7.entryAddress;
		ramAddress = hbin.arm7.ramAddress;
		autoLoadListHookOffset = hbin.arm7AutoLoadListHookOffset;
	}
	else
	{
		binname = "arm9.bin";
		entryAddress = hbin.arm9.entryAddress;
		ramAddress = hbin.arm9.ramAddress;
		autoLoadListHookOffset = hbin.arm9AutoLoadListHookOffset;
	}

	fs::current_path(ncp::asm_path);

	fs::path bakbinname = bcfg.backup / binname;

	if (fs::exists(bakbinname)) //has backup
	{
		arm = new ARM(bakbinname, entryAddress, ramAddress, autoLoadListHookOffset, proc);
	}
	else //has no backup
	{
		fs::current_path(ncp::rom_path);
		arm = new ARM(binname, entryAddress, ramAddress, autoLoadListHookOffset, proc);
		std::vector<u8> bytes = arm->data();

		fs::current_path(ncp::asm_path);
		std::ofstream fileo(bakbinname, std::ios::binary);
		if (!fileo.is_open())
			throw ncp::file_error(bakbinname, ncp::file_error::write);
		fileo.write(reinterpret_cast<char*>(bytes.data()), bytes.size());
		fileo.close();
	}
}

void CodePatcher::loadOverlayTableBin()
{
	ansi::cout << OINFO << "Loading overlay table..." << std::endl;

	BuildCfg& bcfg = ncp::build_cfg;

	const char* binname = proc == 7 ? "arm7ovt.bin" : "arm9ovt.bin";

	fs::current_path(ncp::asm_path);

	fs::path bakbinname = bcfg.backup / binname;

	fs::path workbinname;
	if (fs::exists(bakbinname))
	{
		workbinname = bakbinname;
	}
	else
	{
		fs::current_path(ncp::rom_path);
		workbinname = binname;
		if (!fs::exists(binname))
			throw ncp::file_error(binname, ncp::file_error::find);
	}

	uintmax_t fileSize = fs::file_size(workbinname);
	u32 overlayCount = fileSize / sizeof(OvtEntry);

	ovt.resize(overlayCount);

	std::ifstream filei(workbinname, std::ios::binary);
	if (!filei.is_open())
		throw ncp::file_error(workbinname, ncp::file_error::read);
	for (u32 i = 0; i < overlayCount; i++)
		filei.read(reinterpret_cast<char*>(&ovt[i]), sizeof(OvtEntry));
	filei.close();
}

void CodePatcher::saveOverlayTableBin()
{
	
}

Overlay* CodePatcher::loadOverlayBin(int ovID)
{
	std::string prefix = proc == 7 ? "overlay7" : "overlay9";

	fs::current_path(ncp::asm_path);

	fs::path binname = fs::path(prefix) / (prefix + "_" + std::to_string(ovID) + ".bin");
	fs::path bakbinname = ncp::build_cfg.backup / binname;

	OvtEntry& ovte = ovt[ovID];

	if (fs::exists(bakbinname))
	{
		Overlay* overlay = new Overlay(bakbinname, ovte.ramAddress, ovte.flag & OVERLAY_FLAG_COMP);
		ovte.flag = 0;
		return overlay;
	}
	else
	{
		fs::current_path(ncp::rom_path);
		Overlay* overlay = new Overlay(binname, ovte.ramAddress, ovte.flag & OVERLAY_FLAG_COMP);
		ovte.flag = 0;
		std::vector<u8> bytes = overlay->data();

		fs::current_path(ncp::asm_path);
		std::ofstream fileo(bakbinname, std::ios::binary);
		if (!fileo.is_open())
			throw ncp::file_error(bakbinname, ncp::file_error::write);
		fileo.write(reinterpret_cast<char*>(bytes.data()), bytes.size());
		fileo.close();

		return overlay;
	}
}

Overlay* CodePatcher::getOverlay(int ovID)
{
	for (auto[id, ov] : loadedOverlays)
	{
		if (id == ovID)
			return ov;
	}
	return loadOverlayBin(ovID);
}

void CodePatcher::createBackupDirectory()
{
	const fs::path& bakp = ncp::build_cfg.backup;
	fs::current_path(ncp::asm_path);
	if (!fs::exists(bakp))
	{
		if (!fs::create_directories(bakp))
		{
			std::ostringstream oss;
			oss << "Could not create backup directory: " << OSTR(bakp);
			throw ncp::exception(oss.str());
		}
	}

	std::string prefix = proc == 7 ? "overlay7" : "overlay9";
	fs::path bakpov = bakp / prefix;
	if (!fs::exists(bakpov))
	{
		if (!fs::create_directories(bakpov))
		{
			std::ostringstream oss;
			oss << "Could not create overlay backup directory: " << OSTR(bakpov);
			throw ncp::exception(oss.str());
		}
	}
}

void CodePatcher::buildElf(const CodeMaker::BuiltRegion& builtRegion, const fs::path& lsPath, const fs::path& elfPath)
{
	ansi::cout << OLINK << "  Parsing objects..." << std::endl;

	std::string cmd;
	int retcode;

	std::vector<std::string> hookPatches;
	std::vector<std::string> overPatches;
	std::vector<std::string> rtreplPatches;

	for (const fs::path& object : builtRegion.objects)
	{
		ansi::cout << OLINK << "    " << OSTR(object.string()) << std::endl;

		std::ostringstream oss;
		cmd = ncp::build_cfg.prefix + "objdump -h \"" + object.string() + "\"";
		retcode = process::start(cmd.c_str(), &oss);
		if (retcode != 0)
			throw ncp::exception("Failed to dump object.");
		std::string out = oss.str();

		RemoveLines(out, 5);

		bool isOddLine = true;
		std::istringstream iss(out);
		std::string line;
		while (std::getline(iss, line))
		{
			isOddLine = !isOddLine;
			if (isOddLine)
				continue;

			size_t idpos = line.find_first_not_of(' ', 0);
			if (idpos == std::string::npos)
				continue;
			size_t idendpos = line.find_first_of(' ', idpos);
			if (idendpos == std::string::npos)
				continue;
			size_t namepos = line.find_first_not_of(' ', idendpos);
			if (namepos == std::string::npos)
				continue;
			size_t nameendpos = line.find_first_of(' ', namepos);
			if (nameendpos == std::string::npos)
				continue;

			std::string_view sectionName = std::string_view(line).substr(namepos, nameendpos - namepos);
			if (sectionName.length() < 9)
				continue;
			
			std::string_view hookType = sectionName.substr(5);

			if (hookType.starts_with("jump") ||
			    hookType.starts_with("call") ||
				hookType.starts_with("hook"))
				hookPatches.emplace_back(sectionName);
			else if (hookType.starts_with("over"))
				overPatches.emplace_back(sectionName);
			else if (hookType.starts_with("rtrepl"))
				rtreplPatches.emplace_back(sectionName);
		}
	}

	ansi::cout << OLINK << "  Creating linker script..." << std::endl;

	std::string linkerScript = createLinkerScript(builtRegion, hookPatches, overPatches, rtreplPatches);

	std::ofstream lsFile(lsPath);
	if (!lsFile.is_open())
		throw ncp::file_error(lsPath, ncp::file_error::write);
	lsFile << linkerScript;
	lsFile.close();

	ansi::cout << OLINK << "  Linking..." << std::endl;

	cmd = ncp::build_cfg.prefix + "ld " + builtRegion.region->ldFlags + " -T\"" + lsPath.string() + "\" -o \"" + elfPath.string() + "\"";
	retcode = process::start(cmd.c_str(), &ansi::cout);
	ansi::cout << std::flush;
	if (retcode != 0)
		throw ncp::exception("Failed to link objects.");
}

std::string CodePatcher::createLinkerScript(
	const CodeMaker::BuiltRegion& builtRegion,
	const std::vector<std::string>& hookPatches,
	const std::vector<std::string>& overPatches,
	const std::vector<std::string>& rtreplPatches
	)
{
	int dest = builtRegion.region->destination;
	u32 codeOrigin = dest == -1 ? newcodeAddr : ovt[dest].ramAddress;

	std::string script;
	
	script += "/* Auto-generated linker script */\n\nINCLUDE ";
	script += target.symbols.string();
	if (dest != -1)
	{
		fs::path symPath = target.build / (std::string(MainFilePrefix) + ".sym");
		script += "\nINCLUDE " + symPath.string();
	}
	script += "\n\nINPUT (\n";
	for (const fs::path& object : builtRegion.objects)
		script += "\t" + object.string() + "\n";
	script +=
	")\n\nMEMORY {\n"
	"\tbase (rwx): ORIGIN = 0x00000000, LENGTH = 0x100000\n"
	"\tcode (rwx): ORIGIN = " + util::int_to_addr(codeOrigin, 8) + ", LENGTH = 0x1EFE20\n"
	"}\n\nSECTIONS {\n"
	"\t.text : ALIGN(4) {\n"
	"\t\t*(.text)\n";
	for (const std::string& rtreplPatch : rtreplPatches)
	{
		std::string dotlessName = rtreplPatch.substr(1);
		script += "\t\t" + dotlessName + "_start = .;\n";
		script += "\t\t*(" + rtreplPatch + ")\n";
		script += "\t\t" + dotlessName + "_end = .;\n";
	}
	script +=
	"\t} > code AT > base\n\n";
	for (const std::string& hookPatch : hookPatches)
	{
		script += "\t" + hookPatch + " : ALIGN(4) {\n";
		script += "\t\t*(" + hookPatch + ")\n";
		script += "\t} > code AT > base\n\n";
	}
	script +=
	"\t.text : ALIGN(4) {\n"
	"\t\t*(.rodata)\n"
	"\t\t*(.init_array)\n"
	"\t\t*(.data)\n"
	"\t} > code AT > base\n\n"
	"\t.bss : ALIGN(4) {\n"
	"\t\t*(.bss)\n"
	"\t\t. = ALIGN(4);\n"
	"\t} > code AT > base\n\n"
	"\t.ncp_set : ALIGN(4) {\n"
	"\t\t*(.ncp_set)\n"
	"\t} AT > base\n\n";
	for (const std::string& overPatch : overPatches)
	{
		size_t pos1 = overPatch.find('_', 0);
		if (pos1 == std::string::npos)
			continue;
		size_t pos2 = overPatch.find('_', pos1 + 1);
		if (pos2 == std::string::npos)
			continue;
		size_t pos3 = overPatch.find('_', pos2 + 1);
		bool targetsOv = pos3 != std::string::npos;
		if (!targetsOv)
			pos3 = overPatch.length();
		
		std::string strTargetAddr = overPatch.substr(pos2 + 1, pos3);
		int targetAddr = util::addr_to_int(strTargetAddr);
		strTargetAddr = util::int_to_addr(targetAddr, 8);

		script += "\t" + overPatch + " " + strTargetAddr + " : SUBALIGN(1) {\n";
		script += "\t\t*(" + overPatch + ")\n";
		script += "\t} AT > base\n\n";
	}
	script += "\t/DISCARD/ : {*(.*)}\n}\n";

	return script;
}

static void RemoveLines(std::string& str, size_t count)
{
	size_t cpos = 0;
	for (size_t i = 0; i < count; i++) {
		size_t fpos = str.find('\n', cpos);
		if (fpos == std::string::npos)
			break;
		cpos = fpos + 1;
	}
	str.erase(0, cpos);
}
