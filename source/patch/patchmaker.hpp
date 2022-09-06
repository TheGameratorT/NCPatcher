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

struct PatchSymbolInfo;

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
	u32 m_newcodeAddr;
	std::vector<std::unique_ptr<PatchSymbolInfo>> m_patchInfo;
	std::vector<std::string> m_externSymbols;

	[[nodiscard]] inline ArmBin* getArm() const { return m_arm.get(); }

	void gatherInfoFromObjects();

	void createBackupDirectory();
	void loadArmBin();
	void saveArmBin();
	void loadOverlayTableBin();
	void saveOverlayTableBin();
	OverlayBin* loadOverlayBin(std::size_t ovID);
	OverlayBin* getOverlay(std::size_t ovID);
	void saveOverlayBins();

	std::string createLinkerScript();
};