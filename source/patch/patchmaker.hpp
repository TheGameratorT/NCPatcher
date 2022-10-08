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

class Elf32;
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
	const BuildTarget* m_target;
	const std::filesystem::path* m_targetWorkDir;
	const std::filesystem::path* m_buildDir;
	const HeaderBin* m_header;
	std::vector<std::unique_ptr<SourceFileJob>>* m_srcFileJobs;
	std::unique_ptr<ArmBin> m_arm;
	std::unordered_map<std::size_t, std::unique_ptr<OverlayBin>> m_loadedOverlays;
	std::vector<std::unique_ptr<OvtEntry>> m_ovtEntries;
	std::vector<std::unique_ptr<GenericPatchInfo>> m_patchInfo;
	std::vector<std::unique_ptr<RtReplPatchInfo>> m_rtreplPatches;
	std::vector<int> m_destWithNcpSet;
	std::vector<const SourceFileJob*> m_jobsWithNcpSet;
	std::vector<std::string> m_externSymbols;
	std::filesystem::path m_ldscriptPath;
	std::filesystem::path m_elfPath;
	std::unique_ptr<Elf32> m_elf;
	std::unordered_map<int, u32> m_newcodeAddrForDest;
	std::unordered_map<int, std::unique_ptr<NewcodePatch>> m_newcodeDataForDest;
	std::unordered_map<int, std::unique_ptr<AutogenDataInfo>> m_autogenDataInfoForDest;

	[[nodiscard]] inline ArmBin* getArm() const { return m_arm.get(); }

	void fetchNewcodeAddr();
	void gatherInfoFromObjects();
	void linkElfFile();
	u32 makeJumpOpCode(u32 opCode, u32 fromAddr, u32 toAddr);
	u32 makeThumbJumpOpCode(u16 opCode, u32 fromAddr, u32 toAddr);
	u32 fixupOpCode(u32 opCode, u32 ogAddr, u32 newAddr);
	void applyPatchesToRom();
	void gatherInfoFromElf();

	void loadElfFile();
	void unloadElfFile();

	void createBackupDirectory();
	void loadArmBin();
	void saveArmBin();
	void loadOverlayTableBin();
	void saveOverlayTableBin();
	OverlayBin* loadOverlayBin(std::size_t ovID);
	OverlayBin* getOverlay(std::size_t ovID);
	void saveOverlayBins();

	void createLinkerScript();
};
