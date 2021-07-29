#pragma once

#include <vector>
#include <string>

#include "arm.hpp"
#include "codemaker.hpp"
#include "buildtarget.hpp"

class CodePatcher
{
public:
	CodePatcher(const BuildTarget& target, const std::vector<CodeMaker::BuiltRegion>& builtRegions, int proc);
	~CodePatcher();

private:
	void loadArmBin();
	void buildElf(const CodeMaker::BuiltRegion& builtRegion, const std::filesystem::path& lsPath, const std::filesystem::path& elfPath);
	std::string createLinkerScript(const CodeMaker::BuiltRegion& builtRegion, const std::vector<std::string>& overPatches, const std::vector<std::string>& rtreplPatches);

	const BuildTarget& target;
	const std::vector<CodeMaker::BuiltRegion>& builtRegions;
	int proc;
	ARM* arm;
	u32 newcodeAddr;
};
