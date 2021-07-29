#include "codepatcher.hpp"

#include <fstream>

#include "except.hpp"
#include "global.hpp"
#include "common.hpp"
#include "process.hpp"
#include "oansistream.hpp"

namespace fs = std::filesystem;

static void RemoveLines(std::string& str, size_t count);

CodePatcher::CodePatcher(const BuildTarget& target, const std::vector<CodeMaker::BuiltRegion>& builtRegions, int proc)
	: target(target)
	, builtRegions(builtRegions)
	, proc(proc)
{
	loadArmBin();
	newcodeAddr = arm->read<u32>(target.arenaLo);

	ncp::setErrorMsg("Could not link the final binary.");

	fs::current_path(ncp::asm_path);
	switch (proc)
	{ case 7: fs::current_path(ncp::build_cfg.arm7); break;
	  case 9: fs::current_path(ncp::build_cfg.arm9); break; }

	std::string cmd;
	int retcode;

	for (const CodeMaker::BuiltRegion& builtRegion : builtRegions)
	{
		const BuildTarget::Region* region = builtRegion.region;

		ansi::cout << OLINK << "Working on: " ANSI_bCYAN << region->destination << ANSI_RESET << std::endl;

		fs::path lsPath = target.build / (region->destination + ".ld");
		fs::path elfPath = target.build / (region->destination + ".elf");
		fs::path binPath = target.build / (region->destination + ".bin");

		bool doBuildElf = builtRegion.rebuildElf || !fs::exists(elfPath);
		bool doBuildBin = doBuildElf || !fs::exists(binPath);

		if (doBuildElf)
			buildElf(builtRegion, lsPath, elfPath);

		if (doBuildBin)
		{
			ansi::cout << OLINK << "  Making binary..." << std::endl;

			cmd = ncp::build_cfg.prefix + "objcopy -O binary \"" + elfPath.string() + "\" \"" + binPath.string() + "\"";
			retcode = process::start(cmd.c_str(), &ansi::cout);
			ansi::cout << std::flush;
			if (retcode != 0)
				throw ncp::exception("Failed to convert elf to bin.");
		}
	}
}

CodePatcher::~CodePatcher()
{
	delete arm;
}

void CodePatcher::loadArmBin()
{
	BuildCfg& bcfg = ncp::build_cfg;
	NDSHeader& hbin = ncp::header_bin;

	const char* binname;
	u32 entryAddress, ramAddress, autoLoadListHookOffset;
	switch (proc)
	{
	case 7:
		binname = "arm7.bin";
		entryAddress = hbin.arm7.entryAddress;
		ramAddress = hbin.arm7.ramAddress;
		autoLoadListHookOffset = hbin.arm7AutoLoadListHookOffset;
		break;
	case 9:
		binname = "arm9.bin";
		entryAddress = hbin.arm9.entryAddress;
		ramAddress = hbin.arm9.ramAddress;
		autoLoadListHookOffset = hbin.arm9AutoLoadListHookOffset;
		break;
	default: {
		std::ostringstream oss;
		oss << "how the fuck did this even happen??? :intense_flushed: like what is an arm" << proc << "??? *blushes*";
		throw ncp::exception(oss.str()); }
	}

	fs::current_path(ncp::asm_path);

	const fs::path& bakp = bcfg.backup;
	fs::path bakbinname = bakp / binname;

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
		if (!fs::exists(bakp))
		{
			if (!fs::create_directories(bakp))
			{
				std::ostringstream oss;
				oss << "Could not create backup directory: " << OSTR(bakp);
				throw ncp::exception(oss.str());
			}
		}

		std::ofstream fileo(bakbinname, std::ios::binary);
		if (!fileo.is_open())
			throw ncp::file_error(bakbinname, ncp::file_error::write);
		fileo.write(reinterpret_cast<char*>(bytes.data()), bytes.size());
		fileo.close();
	}
}

void CodePatcher::buildElf(const CodeMaker::BuiltRegion& builtRegion, const fs::path& lsPath, const fs::path& elfPath)
{
	ansi::cout << OLINK << "  Parsing objects..." << std::endl;

	std::string cmd;
	int retcode;

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

			std::string sectionName = line.substr(namepos, nameendpos - namepos);
			if (sectionName.starts_with(".ncp_over"))
				overPatches.push_back(sectionName);
			else if (sectionName.starts_with(".ncp_rtrepl"))
				rtreplPatches.push_back(sectionName);
		}
	}

	ansi::cout << OLINK << "  Creating linker script..." << std::endl;

	std::string linkerScript = createLinkerScript(builtRegion, overPatches, rtreplPatches);

	std::ofstream lsFile;
	lsFile.open(lsPath);
	lsFile << linkerScript;
	lsFile.close();

	ansi::cout << OLINK << "  Linking..." << std::endl;

	cmd = ncp::build_cfg.prefix + "ld " + builtRegion.region->ldFlags + " -T\"" + lsPath.string() + "\" -o \"" + elfPath.string() + "\"";
	retcode = process::start(cmd.c_str(), &ansi::cout);
	ansi::cout << std::flush;
	if (retcode != 0)
		throw ncp::exception("Failed to link objects.");
}

std::string CodePatcher::createLinkerScript(const CodeMaker::BuiltRegion& builtRegion, const std::vector<std::string>& overPatches, const std::vector<std::string>& rtreplPatches)
{
	std::string script;
	
	script += "/* Auto-generated linker script */\n\nINCLUDE ";
	script += target.symbols.string();
	script += "\n\nINPUT (\n";
	for (const fs::path& object : builtRegion.objects)
		script += "\t" + object.string() + "\n";
	script +=
	")\n\nMEMORY {\n"
	"\tbase (rwx): ORIGIN = 0x00000000, LENGTH = 0x100000\n"
	"\tcode (rwx): ORIGIN = " + util::int_to_addr(newcodeAddr, 8) + ", LENGTH = 0x1EFE20\n"
	"}\n\nSECTIONS {\n"
	"\t.text : ALIGN(4) {\n"
	"\t\t. += 0;\n"
	"\t\t*(.text)\n"
	"\t\t*(.ncp_jump*)\n"
	"\t\t*(.ncp_call*)\n"
	"\t\t*(.ncp_hook*)\n";
	for (const std::string& rtreplPatch : rtreplPatches)
	{
		std::string dotlessName = rtreplPatch.substr(1);
		script += "\t\t" + dotlessName + "_start = .;\n";
		script += "\t\t*(" + rtreplPatch + ")\n";
		script += "\t\t" + dotlessName + "_end = .;\n";
	}
	script +=
	"\t\t*(.rodata)\n"
	"\t\t*(.init_array)\n"
	"\t\t*(.data)\n"
	"\t\t. = ALIGN(4);\n"
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
