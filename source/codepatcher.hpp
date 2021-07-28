#pragma once

#include <vector>
#include <string>

#include "arm.hpp"
#include "codemaker.hpp"
#include "buildtarget.hpp"

class CodePatcher
{
public:
	CodePatcher(const BuildTarget& target, const std::vector<CodeMaker::BuiltRegion>& builtRegions, ARM& arm);

private:
	std::string createLinkerScript(const CodeMaker::BuiltRegion& builtRegion, const std::vector<std::string>& overPatches, const std::vector<std::string>& rtreplPatches);

	const BuildTarget& target;
	const std::vector<CodeMaker::BuiltRegion>& builtRegions;
	ARM& arm;
	u32 newcodeAddr;
};
