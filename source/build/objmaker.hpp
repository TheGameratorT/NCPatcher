#pragma once

#include <memory>
#include <vector>
#include <filesystem>

#include "../config/buildtarget.hpp"
#include "../app/context.hpp"
#include "../core/compilation_unit_manager.hpp"
#include "gcc_diagnostics.hpp"

class ObjMaker
{
public:
	ObjMaker();

	void makeTarget(
		const BuildTarget& target,
		const ncp::Context& ctx,
		core::CompilationUnitManager& compilationUnitMgr
	);

private:
	const BuildTarget* m_target;
	const ncp::Context* m_ctx;
	const ncp::PathContext* m_paths;
	std::string m_includeFlags;
	std::string m_defineFlags;
	core::CompilationUnitManager* m_compilationUnitMgr;

	// Which of the compiler's structured formats this toolchain accepts, or
	// Text when it accepts none and its output is passed through as it is.
	ncp::build::DiagnosticsFormat m_diagnosticsFormat = ncp::build::DiagnosticsFormat::Text;

	void getSourceFiles();
	void checkIfSourcesNeedRebuild();
	void compileSources();
};
