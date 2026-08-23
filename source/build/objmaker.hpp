#pragma once

#include <memory>
#include <vector>
#include <filesystem>

#include "../config/buildtarget.hpp"
#include "../system/path_context.hpp"
#include "../core/compilation_unit_manager.hpp"

class ObjMaker
{
public:
	ObjMaker();

	void makeTarget(
		const BuildTarget& target,
		const ncp::PathContext& paths,
		core::CompilationUnitManager& compilationUnitMgr
	);

private:
	const BuildTarget* m_target;
	const ncp::PathContext* m_paths;
	std::string m_includeFlags;
	std::string m_defineFlags;
	core::CompilationUnitManager* m_compilationUnitMgr;

	void getSourceFiles();
	void checkIfSourcesNeedRebuild();
	void compileSources();
};
