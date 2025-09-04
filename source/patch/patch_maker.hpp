#pragma once

#include <memory>
#include <vector>
#include <filesystem>
#include <unordered_map>
#include <sstream>

#include "../utils/types.hpp"
#include "../core/compilation_unit_manager.hpp"
#include "../config/buildtarget.hpp"
#include "../ndsbin/headerbin.hpp"
#include "../ndsbin/armbin.hpp"
#include "../ndsbin/overlaybin.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "types.hpp"
#include "dependency_resolver.hpp"

namespace ncp::patch {

// Forward declarations for component classes
class FileSystemManager;
class PatchTracker; 
class LibraryManager;
class OverwriteRegionManager;
class Linker;
class DependencyResolver;

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
		core::CompilationUnitManager& compilationUnitMgr
	);

private:
	// Core data
	const BuildTarget* m_target;
	const std::filesystem::path* m_targetWorkDir;
	const std::filesystem::path* m_buildDir;
	const HeaderBin* m_header;
	core::CompilationUnitManager* m_compilationUnitMgr;
	
	// Component managers
	std::unique_ptr<FileSystemManager> m_fileSystemManager;
	std::unique_ptr<PatchTracker> m_patchTracker;
	std::unique_ptr<LibraryManager> m_libraryManager;
	std::unique_ptr<OverwriteRegionManager> m_overwriteRegionManager;
	std::unique_ptr<Linker> m_linker;
	std::unique_ptr<DependencyResolver> m_dependencyResolver;

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

	struct PatchOperationContext
	{
		const Elf32* elf;
		const std::vector<std::unique_ptr<PatchInfo>>* patchInfo;
		const std::vector<std::unique_ptr<PatchInfo>>* rtreplPatches;
		const std::unordered_map<int, std::unique_ptr<NewcodeInfo>>* newcodeInfoForDest;
		const std::unordered_map<int, std::unique_ptr<AutogenDataInfo>>* autogenDataInfoForDest;
		const std::unordered_map<int, u32>* newcodeAddrForDest;
		const void* sectionHeaderTable; // Elf32_Shdr*, but avoiding ELF include here
		
		PatchOperationContext(
			const Elf32& elfRef,
			const std::vector<std::unique_ptr<PatchInfo>>& patches,
			const std::vector<std::unique_ptr<PatchInfo>>& rtreplPatchesRef,
			const std::unordered_map<int, std::unique_ptr<NewcodeInfo>>& newcode,
			const std::unordered_map<int, std::unique_ptr<AutogenDataInfo>>& autogen,
			const std::unordered_map<int, u32>& newcodeAddr,
			const void* shTable
		) : elf(&elfRef),
			patchInfo(&patches),
			rtreplPatches(&rtreplPatchesRef),
			newcodeInfoForDest(&newcode),
			autogenDataInfoForDest(&autogen),
			newcodeAddrForDest(&newcodeAddr),
			sectionHeaderTable(shTable) {}
	};

	// Core coordination methods
	void fetchNewcodeAddr();
	void applyPatchesToRom(const PatchOperationContext& context);
	std::vector<std::unique_ptr<DependencyResolver::UnitEntryPoints>> createEntryPointsFromPatches();

	// Patch application methods - organized by type
	void applyJumpPatch(const std::unique_ptr<PatchInfo>& patch, const PatchOperationContext& context);
	void applyCallPatch(const std::unique_ptr<PatchInfo>& patch, const PatchOperationContext& context);
	void applyHookPatch(const std::unique_ptr<PatchInfo>& patch, const PatchOperationContext& context);
	void applyOverPatch(const std::unique_ptr<PatchInfo>& patch, const PatchOperationContext& context);
	
	// Bridge generation methods
	void createArm2ThumbJumpBridge(const std::unique_ptr<PatchInfo>& patch, const PatchOperationContext& context);
	void createHookBridge(const std::unique_ptr<PatchInfo>& patch, const PatchOperationContext& context);
	
	// Overwrite and newcode application
	void applyOverwriteRegions(const PatchOperationContext& context);
	void applyNewcodeToDestinations(const PatchOperationContext& context);
	
	// Newcode destination handlers
	void applyNewcodeToMainArm(int dest, const std::unique_ptr<NewcodeInfo>& newcodeInfo, const PatchOperationContext& context);
	void applyNewcodeToOverlay(int dest, const std::unique_ptr<NewcodeInfo>& newcodeInfo, const PatchOperationContext& context);
	
	// Overlay operation handlers
	void handleAppendModeOverlay(int dest, const std::unique_ptr<NewcodeInfo>& newcodeInfo);
	void handleReplaceModeOverlay(int dest, const std::unique_ptr<NewcodeInfo>& newcodeInfo);
	void handleCreateModeOverlay(int dest, const std::unique_ptr<NewcodeInfo>& newcodeInfo);

	// Helper methods
	[[nodiscard]] ArmBin* getArm() const;
	[[nodiscard]] OverlayBin* getOverlay(std::size_t ovID) const;
	[[nodiscard]] ICodeBin* getBinaryForDestination(int destination) const;
	
	// Validation and error handling
	void validateThumbInterworking(const std::unique_ptr<PatchInfo>& patch) const;
	void validateOverlaySize(int dest, std::size_t totalSize, const BuildTarget::Region& region) const;
	
	// Helper method to wrap AsmGenerator calls with patch context
	template<typename Func>
	auto callAsmGeneratorWithContext(const std::unique_ptr<PatchInfo>& patch, Func&& func) const -> decltype(func())
	{
		try {
			return func();
		}
		catch (const std::exception& e) {
			std::ostringstream oss;
			oss << e.what() << " at " << OSTRa(patch->getPrettyName()) << " (" << OSTR(patch->unit->getSourcePath().string()) << ")";
			throw ncp::exception(oss.str());
		}
	}
	
	// Utility methods
	static void writeNewcodeData(u8* destination, const std::unique_ptr<NewcodeInfo>& newcodeInfo, 
	                            const std::unique_ptr<AutogenDataInfo>* autogenInfo);
};

} // namespace ncp::patch
