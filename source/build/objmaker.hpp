#pragma once

#include <memory>
#include <vector>
#include <filesystem>

#include "../config/buildtarget.hpp"

#include "sourcefilejob.hpp"

class ObjMaker
{
public:
	ObjMaker();

	void makeTarget(
		const BuildTarget& target,
		const std::filesystem::path& targetWorkDir,
		const std::filesystem::path& buildDir,
		std::vector<std::unique_ptr<SourceFileJob>>& jobs
	);

private:
	const BuildTarget* m_target;
	const std::filesystem::path* m_targetWorkDir;
	const std::filesystem::path* m_buildDir;
	std::string m_includeFlags;
	std::string m_defineFlags;
	std::vector<std::unique_ptr<SourceFileJob>>* m_jobs;

	void getSourceFiles();
	void checkIfSourcesNeedRebuild();
	void compileSources();
};
