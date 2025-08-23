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
#include "types.hpp"

// Use the centralized types from patch::types
using patch::GenericPatchInfo;
using patch::RtReplPatchInfo;
using patch::NewcodePatch;
using patch::AutogenDataInfo;
using patch::PatchOperationContext;

// Forward declarations for component classes
class FileSystemManager;
class PatchInfoAnalyzer; 
class LibraryAnalyzer;
class OverwriteRegionManager;
class LinkerScriptGenerator;
class ElfAnalyzer;
class SectionUsageAnalyzer;

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
	std::unique_ptr<LibraryAnalyzer> m_libraryAnalyzer;
	std::unique_ptr<OverwriteRegionManager> m_overwriteRegionManager;
	std::unique_ptr<LinkerScriptGenerator> m_linkerScriptGenerator;
	std::unique_ptr<ElfAnalyzer> m_elfAnalyzer;
	std::unique_ptr<SectionUsageAnalyzer> m_sectionUsageAnalyzer;

	// Data managed by this coordinator
	std::unordered_map<int, u32> m_newcodeAddrForDest;
	int m_arenalo;

	// Pipeline phases - improved organization
	void initializeComponents();
	void setupFileSystem();
	void prepareBuildEnvironment();
	void generateElfFile();
	void processPatches();
	void finalizeBuild();

	// Core coordination methods
	void fetchNewcodeAddr();
	void applyPatchesToRom(const patch::PatchOperationContext& context);

	// Patch application methods - organized by type
	void applyJumpPatch(const std::unique_ptr<GenericPatchInfo>& patch, const patch::PatchOperationContext& context);
	void applyCallPatch(const std::unique_ptr<GenericPatchInfo>& patch, const patch::PatchOperationContext& context);
	void applyHookPatch(const std::unique_ptr<GenericPatchInfo>& patch, const patch::PatchOperationContext& context);
	void applyOverPatch(const std::unique_ptr<GenericPatchInfo>& patch, const patch::PatchOperationContext& context);
	
	// Bridge generation methods
	void createArm2ThumbJumpBridge(const std::unique_ptr<GenericPatchInfo>& patch, const patch::PatchOperationContext& context);
	void createHookBridge(const std::unique_ptr<GenericPatchInfo>& patch, const patch::PatchOperationContext& context);
	
	// Overwrite and newcode application
	void applyOverwriteRegions(const patch::PatchOperationContext& context);
	void applyNewcodeToDestinations(const patch::PatchOperationContext& context);
	
	// Newcode destination handlers
	void applyNewcodeToMainArm(int dest, const std::unique_ptr<NewcodePatch>& newcodeInfo, const PatchOperationContext& context);
	void applyNewcodeToOverlay(int dest, const std::unique_ptr<NewcodePatch>& newcodeInfo, const PatchOperationContext& context);
	
	// Overlay operation handlers
	void handleAppendModeOverlay(int dest, const std::unique_ptr<NewcodePatch>& newcodeInfo);
	void handleReplaceModeOverlay(int dest, const std::unique_ptr<NewcodePatch>& newcodeInfo);
	void handleCreateModeOverlay(int dest, const std::unique_ptr<NewcodePatch>& newcodeInfo);

	// Helper methods
	[[nodiscard]] ArmBin* getArm() const;
	[[nodiscard]] OverlayBin* getOverlay(std::size_t ovID) const;
	[[nodiscard]] ICodeBin* getBinaryForDestination(int destination) const;
	
	// Validation and error handling
	void validateThumbInterworking(const std::unique_ptr<GenericPatchInfo>& patch) const;
	void validateOverlaySize(int dest, std::size_t totalSize, const BuildTarget::Region& region) const;
	
	// Utility methods
	static void writeNewcodeData(u8* destination, const std::unique_ptr<NewcodePatch>& newcodeInfo, 
	                            const std::unique_ptr<AutogenDataInfo>* autogenInfo);
};
