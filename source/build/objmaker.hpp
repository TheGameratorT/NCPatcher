#pragma once

#include <memory>
#include <vector>
#include <filesystem>

#include "../config/buildtarget.hpp"
#include "../core/compilation_unit_manager.hpp"

class ObjMaker
{
public:
	ObjMaker();

	void makeTarget(
		const BuildTarget& target,
		const std::filesystem::path& targetWorkDir,
		const std::filesystem::path& buildDir,
		core::CompilationUnitManager& compilationUnitMgr
	);

private:
	const BuildTarget* m_target;
	const std::filesystem::path* m_targetWorkDir;
	const std::filesystem::path* m_buildDir;
	std::string m_includeFlags;
	std::string m_defineFlags;
	core::CompilationUnitManager* m_compilationUnitMgr;

	void getSourceFiles();
	void checkIfSourcesNeedRebuild();
	void compileSources();
};
