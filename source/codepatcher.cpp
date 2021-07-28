#include "codepatcher.hpp"

#include <fstream>

#include "except.hpp"
#include "global.hpp"
#include "common.hpp"
#include "oansistream.hpp"

namespace fs = std::filesystem;

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

CodePatcher::CodePatcher(const BuildTarget& target, const std::vector<CodeMaker::BuiltRegion>& builtRegions, ARM& arm)
	: target(target)
	, builtRegions(builtRegions)
	, arm(arm)
{
	ncp::setErrorMsg("Could not generate the final binary.");
	
	newcodeAddr = arm.read<u32>(target.arenaLo);

	std::string cmd;
	int retcode;

	for (const CodeMaker::BuiltRegion& builtRegion : builtRegions)
	{
		std::string destination = builtRegion.region->destination;

		ansi::cout << OINFO << "Working on: " << destination << std::endl;

		ansi::cout << OINFO << "    Parsing objects..." << std::endl;

		std::vector<std::string> overPatches;
		std::vector<std::string> rtreplPatches;

		for (const fs::path& object : builtRegion.objects)
		{
			ansi::cout << OINFO << "    " << object.string() << std::endl;

			std::ostringstream oss;
			cmd = ncp::build_cfg.prefix + "objdump -h \"" + object.string() + "\"";
			retcode = exec(cmd.c_str(), oss);
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

		ansi::cout << OINFO << "    Creating linker script..." << std::endl;

		std::string linkerScript = createLinkerScript(builtRegion, overPatches, rtreplPatches);

		fs::path lsPath = target.build / (destination + ".ldscript");
		fs::path elfPath = target.build / (destination + ".elf");
		fs::path binPath = target.build / (destination + ".bin");

		std::ofstream lsFile;
		lsFile.open(lsPath);
		lsFile << linkerScript;
		lsFile.close();

		ansi::cout << OINFO << "    Linking..." << std::endl;

		cmd = ncp::build_cfg.prefix + "ld -T\"" + lsPath.string() + "\" -o \"" + elfPath.string() + "\"";
		exec(cmd.c_str(), ansi::cout);

		ansi::cout << OINFO << "    Making binary..." << std::endl;

		cmd = ncp::build_cfg.prefix + "objcopy -O binary \"" + elfPath.string() + "\" \"" + binPath.string() + "\"";
		exec(cmd.c_str(), ansi::cout);
	}
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
	"\tarm9 (rwx): ORIGIN = " + util::int_to_addr(newcodeAddr, 8) + ", LENGTH = 0x1EFE20\n"
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
	"\t} > arm9 AT > base\n\n"
	"\t.bss : ALIGN(4) {\n"
	"\t\t*(.bss)\n"
	"\t\t. = ALIGN(4);\n"
	"\t} > arm9 AT > base\n\n"
	"\t.ncp_setxxxx : ALIGN(4) {\n"
	"\t\t*(.ncp_setxxxx)\n"
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
