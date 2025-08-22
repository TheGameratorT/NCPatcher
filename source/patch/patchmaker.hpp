#pragma once

#include <memory>
#include <vector>
#include <filesystem>
#include <unordered_map>

#include "../types.hpp"
#include "../build/sourcefilejob.hpp"
#include "../config/buildtarget.hpp"
#include "../ndsbin/headerbin.hpp"
#include "../ndsbin/armbin.hpp"
#include "../ndsbin/overlaybin.hpp"

// Forward declarations for component classes
class FileSystemManager;
class PatchInfoAnalyzer; 
class OverwriteRegionManager;
class LinkerScriptGenerator;
class ElfAnalyzer;

// Forward declarations for data structures
struct GenericPatchInfo;
struct RtReplPatchInfo;
struct NewcodePatch;
struct AutogenDataInfo;

class PatchMaker
{
public:
	PatchMaker();
	~PatchMaker();

	void makeTarget(
		const BuildTarget& target,
		const std::filesystem::path& targetWorkDir,
		const std::filesystem::path& buildDir,
		const HeaderBin& header,
		std::vector<std::unique_ptr<SourceFileJob>>& srcFileJobs
	);

private:
	// Core data
	const BuildTarget* m_target;
	const std::filesystem::path* m_targetWorkDir;
	const std::filesystem::path* m_buildDir;
	const HeaderBin* m_header;
	std::vector<std::unique_ptr<SourceFileJob>>* m_srcFileJobs;
	
	// Component managers
	std::unique_ptr<FileSystemManager> m_fileSystemManager;
	std::unique_ptr<PatchInfoAnalyzer> m_patchInfoAnalyzer;
	std::unique_ptr<OverwriteRegionManager> m_overwriteRegionManager;
	std::unique_ptr<LinkerScriptGenerator> m_linkerScriptGenerator;
	std::unique_ptr<ElfAnalyzer> m_elfAnalyzer;

	// Data managed by this coordinator
	std::unordered_map<int, u32> m_newcodeAddrForDest;
	int m_arenalo;

	// Core coordination methods
	void fetchNewcodeAddr();
	void applyPatchesToRom(
		const std::vector<std::unique_ptr<GenericPatchInfo>>& patchInfo,
		const std::unordered_map<int, std::unique_ptr<NewcodePatch>>& newcodeDataForDest,
		const std::unordered_map<int, std::unique_ptr<AutogenDataInfo>>& autogenDataInfoForDest
	);

	// Helper methods
	[[nodiscard]] ArmBin* getArm() const;
	OverlayBin* getOverlay(std::size_t ovID) const;
};
