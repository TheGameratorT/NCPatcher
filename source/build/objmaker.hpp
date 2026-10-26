#pragma once

#include <memory>
#include <vector>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "../config/buildtarget.hpp"
#include "../app/context.hpp"
#include "../core/compilation_unit_manager.hpp"
#include "gcc_diagnostics.hpp"

class ObjMaker
{
public:
	ObjMaker();

	// Compiles the target's regions into `compilationUnitMgr`, and each of its
	// runtime modules into the matching entry of `runtimeUnitMgrs`, which
	// must have one per BuildTarget::runtimeRegions entry.
	void makeTarget(
		const BuildTarget& target,
		const ncp::Context& ctx,
		core::CompilationUnitManager& compilationUnitMgr,
		std::vector<std::unique_ptr<core::CompilationUnitManager>>& runtimeUnitMgrs
	);

private:
	const BuildTarget* m_target;
	const ncp::Context* m_ctx;
	const ncp::PathContext* m_paths;
	std::string m_includeFlags;
	std::string m_defineFlags;
	core::CompilationUnitManager* m_compilationUnitMgr;
	std::vector<std::unique_ptr<core::CompilationUnitManager>>* m_runtimeUnitMgrs;

	// Which of the compiler's structured formats this toolchain accepts, or
	// Text when it accepts none and its output is passed through as it is.
	ncp::build::DiagnosticsFormat m_diagnosticsFormat = ncp::build::DiagnosticsFormat::Text;

	void getSourceFiles();
	core::CompilationUnit* addSourceUnit(const std::filesystem::path& srcPath, const BuildTarget::Region& region, std::size_t fileType,
		core::CompilationUnitManager& unitMgr, const std::filesystem::path& buildRoot);

	// The user units of the target and of its runtime modules, which are
	// compiled together.
	std::vector<core::CompilationUnit*> allUserUnits() const;
	bool addRuntimeIfUsed();
	void checkIfSourcesNeedRebuild();
	void checkIfUnitNeedsRebuild(core::CompilationUnit* unit, std::unordered_map<std::string, std::filesystem::file_time_type>& timeForDep);
	void compileSources();
};
