#pragma once

#include <memory>
#include <vector>
#include <filesystem>
#include <unordered_map>
#include <sstream>
#include <functional>

#include "../utils/types.hpp"
#include "../core/compilation_unit_manager.hpp"
#include "../config/buildtarget.hpp"
#include "../config/rebuild_store.hpp"
#include "../app/context.hpp"
#include "../rom/accessor.hpp"
#include "../ndsbin/armbin.hpp"
#include "../ndsbin/overlaybin.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "types.hpp"
#include "dependency_resolver.hpp"
#include "patch_group.hpp"

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

	// `runtimeUnitMgrs` holds the compiled units of each runtime module, in
	// the order of BuildTarget::runtimeRegions.
	void makeTarget(
		const BuildTarget& target,
		const ncp::Context& ctx,
		ncp::rom::RomAccessor& rom,
		core::CompilationUnitManager& compilationUnitMgr,
		std::vector<std::unique_ptr<core::CompilationUnitManager>>& runtimeUnitMgrs
	);

private:
	// Core data
	const BuildTarget* m_target;
	const ncp::Context* m_ctx;
	const ncp::PathContext* m_paths;
	ncp::rom::RomAccessor* m_rom;
	core::CompilationUnitManager* m_compilationUnitMgr;
	std::vector<std::unique_ptr<core::CompilationUnitManager>>* m_runtimeUnitMgrs = nullptr;
	
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

	struct GroupEntry
	{
		RuntimeKind kind;
		u32 value;
	};
	std::unordered_map<const PatchInfo*, GroupEntry> m_groupEntries;

	// The runtime module being linked, if any. Its patches all go into its
	// patch table, and its hook bridges run wherever it is loaded.
	const BuildTarget::Region* m_runtimeModule = nullptr;

	// Pipeline phases - improved organization
	void initializeComponents();
	void setupFileSystem();
	void prepareBuildEnvironment();
	void generateElfFile();
	void processPatches();
	void finalizeBuild();
	void logLinkingSummary();

	// Links each runtime module at two bases, compares the images to find its
	// relocations, and writes the result to its outputs.
	void makeRuntimeModules(std::vector<std::unique_ptr<core::CompilationUnitManager>>& runtimeUnitMgrs);

	// The symbols the runtime modules use that the project's own objects
	// define. The main link keeps them, so a module can call what the main
	// binary itself never does.
	std::vector<std::string> collectRuntimeImports() const;

	struct RuntimeModuleLink
	{
		std::vector<u8> image;
		u32 bssSize = 0;
		std::size_t patchCount = 0;

		// The module's symbols by offset, to name what a word belongs to.
		std::vector<std::pair<u32, std::string>> symbols;
	};
	RuntimeModuleLink linkRuntimeModule(const BuildTarget::Region& region, core::CompilationUnitManager& units, u32 base);
	void writeRuntimeModule(const BuildTarget::Region& region, const std::vector<u8>& image);

	struct PatchOperationContext
	{
		const Elf32* elf;
		const std::vector<std::unique_ptr<PatchInfo>>* patchInfo;
		const std::unordered_map<int, std::unique_ptr<NewcodeInfo>>* newcodeInfoForDest;
		const std::unordered_map<int, std::unique_ptr<AutogenDataInfo>>* autogenDataInfoForDest;
		const std::unordered_map<int, u32>* newcodeAddrForDest;
		const void* sectionHeaderTable; // Elf32_Shdr*, but avoiding ELF include here
		
		PatchOperationContext(
			const Elf32& elfRef,
			const std::vector<std::unique_ptr<PatchInfo>>& patches,
			const std::unordered_map<int, std::unique_ptr<NewcodeInfo>>& newcode,
			const std::unordered_map<int, std::unique_ptr<AutogenDataInfo>>& autogen,
			const std::unordered_map<int, u32>& newcodeAddr,
			const void* shTable
		) : elf(&elfRef),
			patchInfo(&patches),
			newcodeInfoForDest(&newcode),
			autogenDataInfoForDest(&autogen),
			newcodeAddrForDest(&newcodeAddr),
			sectionHeaderTable(shTable) {}
	};

	// Core coordination methods
	void fetchNewcodeAddr();
	void applyPatchesToRom(const PatchOperationContext& context);
	void applyEachPatch(const PatchOperationContext& context);

	// Patch application methods - organized by type
	void applyJumpPatch(const std::unique_ptr<PatchInfo>& patch, const PatchOperationContext& context);
	void applyCallPatch(const std::unique_ptr<PatchInfo>& patch, const PatchOperationContext& context);
	void applyHookPatch(const std::unique_ptr<PatchInfo>& patch, const PatchOperationContext& context);
	void applyOverPatch(const std::unique_ptr<PatchInfo>& patch, const PatchOperationContext& context);
	
	// Bridge generation methods, returning the bridge address
	u32 createArm2ThumbJumpBridge(const std::unique_ptr<PatchInfo>& patch, const PatchOperationContext& context);
	u32 createHookBridge(const std::unique_ptr<PatchInfo>& patch, const PatchOperationContext& context);

	// Writes a patch to its binary, or for a grouped patch records the kind
	// and value of its entry in the group table instead. A runtime module's
	// patch is only recorded, and its bytes never made: the module's address
	// is not known, so a branch to it cannot be encoded.
	void writePatch(const std::unique_ptr<PatchInfo>& patch, RuntimeKind kind, u32 value, const u8* data, u32 size);
	void writePatchOpCode(const std::unique_ptr<PatchInfo>& patch, RuntimeKind kind, u32 value, const std::function<u32()>& makeOpCode);

	// Fills the group tables reserved in the main region's autogenerated data.
	void writeGroupTables(const PatchOperationContext& context);

	// Writes one ncprt_patch per patch at the start of `block`, which is at
	// `entriesAddr`, followed by the bytes each one saves.
	void writeGroupEntries(std::span<u8> block, u32 entriesAddr, const std::vector<const PatchInfo*>& patches) const;
	
	// Overwrite and newcode application
	void applyOverwriteRegions(const PatchOperationContext& context);
	void applyNewcodeToDestinations(const PatchOperationContext& context);
	
	// Newcode destination handlers
	void applyNewcodeToMainArm(int dest, const std::unique_ptr<NewcodeInfo>& newcodeInfo, const PatchOperationContext& context);
	void applyNewcodeToOverlay(int dest, const std::unique_ptr<NewcodeInfo>& newcodeInfo, const PatchOperationContext& context);
	
	// Overlay operation handlers
	void handleAppendModeOverlay(int dest, const std::unique_ptr<NewcodeInfo>& newcodeInfo,
		const std::unique_ptr<AutogenDataInfo>* autogenInfo);
	void handleReplaceModeOverlay(int dest, const std::unique_ptr<NewcodeInfo>& newcodeInfo,
		const std::unique_ptr<AutogenDataInfo>* autogenInfo);
	void handleCreateModeOverlay(int dest, const std::unique_ptr<NewcodeInfo>& newcodeInfo,
		const std::unique_ptr<AutogenDataInfo>* autogenInfo);

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
