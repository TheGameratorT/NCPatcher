#include "patchmaker.hpp"

#include <sstream>
#include <cstring>

#include "filesystem_manager.hpp"
#include "patch_info_analyzer.hpp"
#include "library_analyzer.hpp"
#include "overwrite_region_manager.hpp"
#include "linker_script_generator.hpp"
#include "elf_analyzer.hpp"
#include "section_usage_analyzer.hpp"
#include "asm_generator.hpp"

#include "arenalofinder.hpp"

#include "../app/application.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../config/rebuildconfig.hpp"
#include "../utils/util.hpp"
#include "../ndsbin/icodebin.hpp"

/*
 * TODO: Endianness checks
 * */

constexpr std::size_t SizeOfHookBridge = 20;
constexpr std::size_t SizeOfArm2ThumbJumpBridge = 8;

PatchMaker::PatchMaker() = default;
PatchMaker::~PatchMaker() = default;

void PatchMaker::makeTarget(
	const BuildTarget& target,
	const std::filesystem::path& targetWorkDir,
	const std::filesystem::path& buildDir,
	const HeaderBin& header,
	core::CompilationUnitManager& compilationUnitMgr
	)
{
	// Store core data references
	m_target = &target;
	m_targetWorkDir = &targetWorkDir;
	m_buildDir = &buildDir;
	m_header = &header;
	m_compilationUnitMgr = &compilationUnitMgr;

	// TODO: change this, the user might want to link only a library
	if (m_compilationUnitMgr->getUnits().empty())
		throw ncp::exception("There are no compilation units to link.");

	try {
		initializeComponents();
		setupFileSystem();
		prepareBuildEnvironment();
		generateElfFile();
		processPatches();
		finalizeBuild();
	}
	catch (const std::exception&) {
		// Cleanup on failure if needed
		throw;
	}
}

void PatchMaker::initializeComponents()
{
	ncp::Application::setErrorContext(m_target->getArm9() ?
		"Failed to initialize components for ARM9 target." :
		"Failed to initialize components for ARM7 target.");

	// Create component managers
	m_fileSystemManager = std::make_unique<FileSystemManager>();
	m_patchInfoAnalyzer = std::make_unique<PatchInfoAnalyzer>();
	m_libraryAnalyzer = std::make_unique<LibraryAnalyzer>();
	m_overwriteRegionManager = std::make_unique<OverwriteRegionManager>();
	m_linkerScriptGenerator = std::make_unique<LinkerScriptGenerator>();
	m_elfAnalyzer = std::make_unique<ElfAnalyzer>();
	m_sectionUsageAnalyzer = std::make_unique<SectionUsageAnalyzer>();

	// Initialize all components
	m_fileSystemManager->initialize(*m_target, *m_buildDir, *m_header);
	m_patchInfoAnalyzer->initialize(*m_target, *m_targetWorkDir, *m_compilationUnitMgr);
	m_libraryAnalyzer->initialize(*m_target, *m_buildDir, *m_compilationUnitMgr);
	m_overwriteRegionManager->initialize(*m_target);
	m_linkerScriptGenerator->initialize(*m_target, *m_buildDir, *m_compilationUnitMgr, m_newcodeAddrForDest);
	m_elfAnalyzer->initialize(*m_buildDir / (m_target->getArm9() ? "arm9.elf" : "arm7.elf"));
}

void PatchMaker::setupFileSystem()
{
	ncp::Application::setErrorContext(m_target->getArm9() ?
		"Failed to setup filesystem for ARM9 target." :
		"Failed to setup filesystem for ARM7 target.");

	m_fileSystemManager->createBuildDirectory();
	m_fileSystemManager->createBackupDirectory();
}

void PatchMaker::prepareBuildEnvironment()
{
	ncp::Application::setErrorContext(m_target->getArm9() ?
		"Failed to prepare build environment for ARM9 target." :
		"Failed to prepare build environment for ARM7 target.");

	m_fileSystemManager->loadArmBin();
	m_fileSystemManager->loadOverlayTableBin();

	// Load overlay files that will be patched
	std::vector<u32>& patchedOverlays = m_target->getArm9() ?
		RebuildConfig::getArm7PatchedOvs() :
		RebuildConfig::getArm9PatchedOvs();

	for (u32 ovID : patchedOverlays)
		m_fileSystemManager->loadOverlayBin(ovID);

	// Determine newcode addresses
	fetchNewcodeAddr();
}

void PatchMaker::generateElfFile()
{
	ncp::Application::setErrorContext(m_target->getArm9() ?
		"Failed to generate ELF files for ARM9 target." :
		"Failed to generate ELF files for ARM7 target.");
	
	m_overwriteRegionManager->setupOverwriteRegions();
	
	// Generate library compilation units and merge them with user units
	m_libraryAnalyzer->analyzeLibraryDependencies();
	m_libraryAnalyzer->generateLibraryUnits();

	// Initialize the user object ELFs
	for (const auto& unit : m_compilationUnitMgr->getUserUnits())
	{
		auto elf = ncp::cache::CacheManager::getInstance().getOrLoadElf(unit->getObjectPath());
        // Cache the ELF pointer in the unit for future use
        unit->setElf(elf);
	}

	// Analyze patches and sections
	m_patchInfoAnalyzer->gatherInfoFromObjects();
	
	// Now analyze all sections from both user and library objects
	auto candidateSections = m_patchInfoAnalyzer->takeOverwriteCandidateSections();

    Log::out << OINFO << "Analyzing unreferenced sections..." << std::endl;
	
	// Initialize and run the section usage analyzer on all jobs
	m_sectionUsageAnalyzer->initialize(
		m_patchInfoAnalyzer->getPatchInfo(),
		m_patchInfoAnalyzer->getExternSymbols(),
		*m_compilationUnitMgr
	);
	
	// Analyze all object files (user + library) to determine section usage
	m_sectionUsageAnalyzer->analyzeObjectFiles();
	
	// Filter candidate sections to only include those that would survive linking
	m_sectionUsageAnalyzer->filterUsedSections(candidateSections);
	
	// Assign only the actually used sections to overwrite regions
	m_overwriteRegionManager->assignSectionsToOverwrites(candidateSections);

    Log::out << OLINK << "Generating the linker script..." << std::endl;
	
	// Generate the final ELF with properly filtered sections (no strip ELF needed)
	m_linkerScriptGenerator->createLinkerScript(
		m_patchInfoAnalyzer->getPatchInfo(),
		m_patchInfoAnalyzer->getRtreplPatches(),
		m_patchInfoAnalyzer->getExternSymbols(),
		m_patchInfoAnalyzer->getDestWithNcpSet(),
		m_patchInfoAnalyzer->getUnitsWithNcpSet(),
		m_overwriteRegionManager->getOverwriteRegions()
	);
	m_linkerScriptGenerator->linkElfFile();
}

void PatchMaker::processPatches()
{
	ncp::Application::setErrorContext(m_target->getArm9() ?
		"Failed to process patches for ARM9 target." :
		"Failed to process patches for ARM7 target.");

	// Analyze the ELF and apply patches
	m_elfAnalyzer->loadElfFile();
	
	auto patchInfo = m_patchInfoAnalyzer->takePatchInfo();
	auto rtreplPatches = m_patchInfoAnalyzer->takeRtreplPatches();
	m_elfAnalyzer->gatherInfoFromElf(patchInfo, m_overwriteRegionManager->getOverwriteRegions());
	
	auto newcodeDataForDest = m_elfAnalyzer->takeNewcodeDataForDest();
	auto autogenDataInfoForDest = m_elfAnalyzer->takeAutogenDataInfoForDest();
	
	// Create operation context for improved parameter passing
	patch::PatchOperationContext context(patchInfo, rtreplPatches, newcodeDataForDest, autogenDataInfoForDest,
		m_newcodeAddrForDest, m_elfAnalyzer->getElf()->getSectionHeaderTable());
	
	applyPatchesToRom(context);
	m_elfAnalyzer->unloadElfFile();
}

void PatchMaker::finalizeBuild()
{
	ncp::Application::setErrorContext(m_target->getArm9() ?
		"Failed to finalize build for ARM9 target." :
		"Failed to finalize build for ARM7 target.");

	// Update patched overlays list
	std::vector<u32>& patchedOverlays = m_target->getArm9() ?
		RebuildConfig::getArm7PatchedOvs() :
		RebuildConfig::getArm9PatchedOvs();

	patchedOverlays.clear();
	for (const auto& [id, ov] : m_fileSystemManager->getLoadedOverlays())
	{
		if (ov->getDirty())
			patchedOverlays.push_back(id);
	}

	// Save all modified files
	m_fileSystemManager->saveOverlayBins();
	m_fileSystemManager->saveOverlayTableBin();
	m_fileSystemManager->saveArmBin();

	ncp::Application::setErrorContext(nullptr);
}

void PatchMaker::fetchNewcodeAddr()
{
	ArmBin* arm = getArm();
	m_arenalo = m_target->arenaLo;

	auto newcodeAddrFromMissingArenaLo = [&](){
		ArenaLoFinder::findArenaLo(arm, m_arenalo, m_newcodeAddrForDest[-1]);
		Log::out << OINFO << "Found ArenaLo at: 0x" << std::uppercase << std::hex << m_arenalo << std::endl;
	};

	if (m_arenalo == 0)
	{
		if (!m_target->getArm9())
		{
			std::ostringstream oss;
			oss << OSTR("arenaLo") << " was not set and finding it automatically for ARM7 is not yet supported.";
			throw ncp::exception(oss.str());
		}

		Log::out << OINFO << OSTR("arenaLo") << " not specified, searching..." << std::endl;
		newcodeAddrFromMissingArenaLo();
	}
	else
	{
		if (m_target->getArm9())
		{
			u32 addr = arm->read<u32>(m_arenalo);
			if (arm->sanityCheckAddress(addr))
			{
				m_newcodeAddrForDest[-1] = addr;
			}
			else
			{
				Log::out << OWARN << "Invalid " << OSTR("arenaLo") << " provided, searching..." << std::endl;
				newcodeAddrFromMissingArenaLo();
			}
		}
		else
		{
			// ARM7 sanity check address not yet supported
			m_newcodeAddrForDest[-1] = arm->read<u32>(m_arenalo);
		}
	}
	
	auto& ovtEntries = m_fileSystemManager->getOvtEntries();
	for (auto& region : m_target->regions)
	{
		int dest = region.destination;
		if (dest != -1)
		{
			u32 addr;
			switch (region.mode)
			{
			case BuildTarget::Mode::Append:
			{
				auto& ovtEntry = ovtEntries[dest];
				addr = ovtEntry.ramAddress + ovtEntry.ramSize + ovtEntry.bssSize;
				break;
			}
			case BuildTarget::Mode::Replace:
			{
				addr = (region.address == 0xFFFFFFFF) ? ovtEntries[dest].ramAddress : region.address;
				break;
			}
			case BuildTarget::Mode::Create:
			{
				addr = region.address;
				break;
			}
			}
			m_newcodeAddrForDest[dest] = addr;
		}
	}
}

void PatchMaker::applyPatchesToRom(const PatchOperationContext& context)
{
	ncp::Application::setErrorContext(m_target->getArm9() ?
		"Failed to apply patches for ARM9 target." :
		"Failed to apply patches for ARM7 target.");

	Log::info("Applying patches to ROM binaries...");

	// Process individual patches by type
	for (const auto& patch : *context.patchInfo)
	{
		switch (patch->patchType)
		{
		case patch::PatchType::Jump:
			applyJumpPatch(patch, context);
			break;
		case patch::PatchType::Call:
			applyCallPatch(patch, context);
			break;
		case patch::PatchType::Hook:
			applyHookPatch(patch, context);
			break;
		case patch::PatchType::Over:
			applyOverPatch(patch, context);
			break;
		default:
			throw ncp::exception("Unsupported patch type: " + std::to_string(patch->patchType));
		}
	}

	// Apply overwrite regions and newcode
	applyOverwriteRegions(context);
	applyNewcodeToDestinations(context);

	ncp::Application::setErrorContext(nullptr);
}

void PatchMaker::applyJumpPatch(const std::unique_ptr<GenericPatchInfo>& patch, const PatchOperationContext& context)
{
	ICodeBin* bin = getBinaryForDestination(patch->destAddressOv);

	if (!patch->destThumb && !patch->srcThumb) 
	{
		// ARM -> ARM
		bin->write<u32>(patch->destAddress, callAsmGeneratorWithContext(patch, [&]() {
			return AsmGenerator::makeJumpOpCode(AsmGenerator::armOpcodeB, patch->destAddress, patch->srcAddress);
		}));
	}
	else if (!patch->destThumb && patch->srcThumb) 
	{
		// ARM -> THUMB: Create bridge
		createArm2ThumbJumpBridge(patch, context);
	}
	else if (patch->destThumb && !patch->srcThumb) 
	{
		// THUMB -> ARM
		u16 patchData[4];
		patchData[0] = AsmGenerator::thumbOpCodePushLR;
		Util::write<u32>(&patchData[1], callAsmGeneratorWithContext(patch, [&]() {
			return AsmGenerator::makeThumbCallOpCode(true, patch->destAddress + 2, patch->srcAddress);
		}));
		patchData[3] = AsmGenerator::thumbOpCodePopPC;
		bin->writeBytes(patch->destAddress, patchData, 8);
	}
	else 
	{
		// THUMB -> THUMB
		u16 patchData[4];
		patchData[0] = AsmGenerator::thumbOpCodePushLR;
		Util::write<u32>(&patchData[1], callAsmGeneratorWithContext(patch, [&]() {
			return AsmGenerator::makeThumbCallOpCode(false, patch->destAddress + 2, patch->srcAddress);
		}));
		patchData[3] = AsmGenerator::thumbOpCodePopPC;
		bin->writeBytes(patch->destAddress, patchData, 8);
	}
}

void PatchMaker::applyCallPatch(const std::unique_ptr<GenericPatchInfo>& patch, const PatchOperationContext& context)
{
	validateThumbInterworking(patch);
	
	ICodeBin* bin = getBinaryForDestination(patch->destAddressOv);

	if (!patch->destThumb && !patch->srcThumb) 
	{
		// ARM -> ARM
		bin->write<u32>(patch->destAddress, callAsmGeneratorWithContext(patch, [&]() {
			return AsmGenerator::makeJumpOpCode(AsmGenerator::armOpcodeBL, patch->destAddress, patch->srcAddress);
		}));
	}
	else if (!patch->destThumb && patch->srcThumb) 
	{
		// ARM -> THUMB
		bin->write<u32>(patch->destAddress, callAsmGeneratorWithContext(patch, [&]() {
			return AsmGenerator::makeBLXOpCode(patch->destAddress, patch->srcAddress);
		}));
	}
	else if (patch->destThumb && !patch->srcThumb) 
	{
		// THUMB -> ARM
		bin->write<u32>(patch->destAddress, callAsmGeneratorWithContext(patch, [&]() {
			return AsmGenerator::makeThumbCallOpCode(true, patch->destAddress, patch->srcAddress);
		}));
	}
	else 
	{
		// THUMB -> THUMB
		bin->write<u32>(patch->destAddress, callAsmGeneratorWithContext(patch, [&]() {
			return AsmGenerator::makeThumbCallOpCode(false, patch->destAddress, patch->srcAddress);
		}));
	}
}

void PatchMaker::applyHookPatch(const std::unique_ptr<GenericPatchInfo>& patch, const PatchOperationContext& context)
{
	if (patch->destThumb)
	{
		std::ostringstream oss;
		oss << "Injecting hook from " << (patch->destThumb ? "THUMB" : "ARM") << " to "
			<< (patch->srcThumb ? "THUMB" : "ARM") << " is not supported, at ";
		oss << OSTRa(patch->formatPatchDescriptor()) << " (" << OSTR(patch->unit->getSourcePath().string()) << ")";
		throw ncp::exception(oss.str());
	}

	createHookBridge(patch, context);
}

void PatchMaker::applyOverPatch(const std::unique_ptr<GenericPatchInfo>& patch, const PatchOperationContext& context)
{
	ICodeBin* bin = getBinaryForDestination(patch->destAddressOv);
	const char* sectionData = m_elfAnalyzer->getElf()->getSection<char>(static_cast<const Elf32_Shdr*>(context.sectionHeaderTable)[patch->sectionIdx]);
	bin->writeBytes(patch->destAddress, sectionData, patch->sectionSize);
}

void PatchMaker::createArm2ThumbJumpBridge(const std::unique_ptr<GenericPatchInfo>& patch, const PatchOperationContext& context)
{
	/*
	 * ARM to THUMB jump bridge:
	 * arm2thumb_jump_bridge:
	 *     LDR   PC, [PC,#-4]
	 *     .int: srcAddr+1
	 */
	
	auto infoIt = context.autogenDataInfoForDest->find(patch->srcAddressOv);
	if (infoIt == context.autogenDataInfoForDest->end())
		throw ncp::exception("Unexpected patch->srcAddressOv for autogenDataInfoForDest encountered.");

	auto& info = infoIt->second;
	std::vector<u8>& bridgeData = info->data;
	std::size_t offset = bridgeData.size();
	bridgeData.resize(offset + SizeOfArm2ThumbJumpBridge);

	u32 bridgeAddr = info->curAddress;

	if (ncp::Application::isVerbose(ncp::VerboseTag::Patch))
	{
		Log::out << "ARM->THUMB BRIDGE: " << Util::intToAddr(bridgeAddr, 8) 
		         << " for " << patch->formatPatchDescriptor()
		         << " from " << patch->unit->getObjectPath().filename().string() << std::endl;
	}

	ICodeBin* bin = getBinaryForDestination(patch->destAddressOv);
	bin->write<u32>(patch->destAddress, callAsmGeneratorWithContext(patch, [&]() {
		return AsmGenerator::makeJumpOpCode(AsmGenerator::armOpcodeB, patch->destAddress, bridgeAddr);
	}));

	u8* bridgeDataPtr = bridgeData.data() + offset;

	Util::write<u32>(bridgeDataPtr, 0xE51FF004);            // LDR PC, [PC,#-4]
	Util::write<u32>(bridgeDataPtr + 4, patch->srcAddress | 1); // int value to jump to

	if (ncp::Application::isVerbose(ncp::VerboseTag::Patch))
		Util::printDataAsHex(bridgeData.data() + offset, SizeOfArm2ThumbJumpBridge, 32);

	info->curAddress += SizeOfArm2ThumbJumpBridge;
}

void PatchMaker::createHookBridge(const std::unique_ptr<GenericPatchInfo>& patch, const PatchOperationContext& context)
{
	/*
	 * Hook bridge:
	 * hook_bridge:
	 *     PUSH {R0-R3,R12}
	 *     BL   srcAddr        @ BLX if srcAddr is THUMB
	 *     POP  {R0-R3,R12}
	 *     <unpatched destAddr's instruction>
	 *     B    (destAddr + 4)
	 */

	ICodeBin* bin = getBinaryForDestination(patch->destAddressOv);
	u32 ogOpCode = bin->read<u32>(patch->destAddress);

	auto infoIt = context.autogenDataInfoForDest->find(patch->srcAddressOv);
	if (infoIt == context.autogenDataInfoForDest->end())
		throw ncp::exception("Unexpected patch->srcAddressOv for autogenDataInfoForDest encountered.");

	auto& info = infoIt->second;
	std::vector<u8>& hookData = info->data;
	std::size_t offset = hookData.size();
	hookData.resize(offset + SizeOfHookBridge);

	u32 hookBridgeAddr = info->curAddress;

	if (ncp::Application::isVerbose(ncp::VerboseTag::Patch))
	{
		Log::out << "HOOK BRIDGE: " << Util::intToAddr(hookBridgeAddr, 8) 
		         << " for " << patch->formatPatchDescriptor()
		         << " from " << patch->unit->getObjectPath().filename().string() << std::endl;
	}

	bin->write<u32>(patch->destAddress, callAsmGeneratorWithContext(patch, [&]() {
		return AsmGenerator::makeJumpOpCode(AsmGenerator::armOpcodeB, patch->destAddress, hookBridgeAddr);
	}));

	u8* hookDataPtr = hookData.data() + offset;

	u32 jmpOpCode = callAsmGeneratorWithContext(patch, [&]() {
		return patch->srcThumb ? 
			AsmGenerator::makeBLXOpCode(hookBridgeAddr + 4, patch->srcAddress) : 
			AsmGenerator::makeJumpOpCode(AsmGenerator::armOpcodeBL, hookBridgeAddr + 4, patch->srcAddress);
	});

	Util::write<u32>(hookDataPtr, AsmGenerator::armHookPush);
	Util::write<u32>(hookDataPtr + 4, jmpOpCode);
	Util::write<u32>(hookDataPtr + 8, AsmGenerator::armHookPop);
	Util::write<u32>(hookDataPtr + 12, callAsmGeneratorWithContext(patch, [&]() {
		return AsmGenerator::fixupOpCode(ogOpCode, patch->destAddress, hookBridgeAddr + 12, m_target->getArm9());
	}));
	Util::write<u32>(hookDataPtr + 16, callAsmGeneratorWithContext(patch, [&]() {
		return AsmGenerator::makeJumpOpCode(AsmGenerator::armOpcodeB, hookBridgeAddr + 16, patch->destAddress + 4);
	}));

	if (ncp::Application::isVerbose(ncp::VerboseTag::Patch))
		Util::printDataAsHex(hookData.data() + offset, SizeOfHookBridge, 32);

	info->curAddress += SizeOfHookBridge;
}

void PatchMaker::applyOverwriteRegions(const PatchOperationContext& context)
{
	for (const auto& overwrite : m_overwriteRegionManager->getOverwriteRegions())
	{
		if (overwrite->assignedSections.empty())
			continue;

		ICodeBin* bin = getBinaryForDestination(overwrite->destination);
		const char* sectionData = m_elfAnalyzer->getElf()->getSection<char>(static_cast<const Elf32_Shdr*>(context.sectionHeaderTable)[overwrite->sectionIdx]);

		bin->writeBytes(overwrite->startAddress, sectionData, overwrite->sectionSize);
		
		if (ncp::Application::isVerbose(ncp::VerboseTag::Patch))
		{
			Log::out << OINFO << "Applied overwrite region " << OSTR(overwrite->memName) 
				<< " at 0x" << std::hex << std::uppercase << overwrite->startAddress
				<< " (size: " << std::dec << overwrite->sectionSize << " bytes)" << std::endl;
		}
		
		// Mark overlay as dirty if it's an overlay
		if (overwrite->destination != -1)
		{
			static_cast<OverlayBin*>(bin)->setDirty(true);
		}
	}
}

void PatchMaker::applyNewcodeToDestinations(const PatchOperationContext& context)
{
	for (const auto& [dest, newcodeInfo] : *context.newcodeDataForDest)
	{
		if (dest == -1)
		{
			applyNewcodeToMainArm(dest, newcodeInfo, context);
		}
		else
		{
			applyNewcodeToOverlay(dest, newcodeInfo, context);
		}
	}
}

void PatchMaker::applyNewcodeToMainArm(int dest, const std::unique_ptr<NewcodePatch>& newcodeInfo, const PatchOperationContext& context)
{
	u32 newcodeAddr = m_newcodeAddrForDest[dest];

	// If more data needs to be added
	if ((newcodeInfo->binSize + newcodeInfo->bssSize) != 0)
	{
		ArmBin* bin = getArm();
		std::vector<u8>& data = bin->data();

		// Extend the ARM binary
		data.resize(data.size() + newcodeInfo->binSize + 12);

		// Write the new relocated code address
		u32 heapReloc = newcodeAddr + newcodeInfo->binSize + 
			(newcodeInfo->bssAlign - newcodeInfo->binSize % newcodeInfo->bssAlign) + newcodeInfo->bssSize;
		bin->write<u32>(m_arenalo, heapReloc);

		ArmBin::ModuleParams* moduleParams = bin->getModuleParams();
		u32 ramAddress = bin->getRamAddress();

		u32 autoloadListStart = moduleParams->autoloadListStart;
		u32 autoloadListEnd = moduleParams->autoloadListEnd;
		u32 binAutoloadListStart = moduleParams->autoloadListStart - ramAddress;
		u32 binAutoloadListEnd = moduleParams->autoloadListEnd - ramAddress;
		u32 binAutoloadStart = moduleParams->autoloadStart - ramAddress;

		std::vector<ArmBin::AutoLoadEntry>& autoloadList = bin->getAutoloadList();
		autoloadList.insert(autoloadList.begin(), ArmBin::AutoLoadEntry{
			.address = newcodeAddr,
			.size = u32(newcodeInfo->binSize),
			.bssSize = u32(newcodeInfo->bssSize),
			.dataOff = binAutoloadStart
		});

		// Write the new data
		if (newcodeInfo->binSize != 0)
		{
			// Move/offset the old code by the size of our patch
			std::memcpy(&data[binAutoloadStart + newcodeInfo->binSize], &data[binAutoloadStart], 
				binAutoloadListStart - binAutoloadStart);

			auto autogenDataIt = context.autogenDataInfoForDest->find(dest);
			const std::unique_ptr<AutogenDataInfo>* autogenPtr = 
				autogenDataIt != context.autogenDataInfoForDest->end() ? &autogenDataIt->second : nullptr;
			writeNewcodeData(&data[binAutoloadStart], newcodeInfo, autogenPtr);
		}

		// Set the new autoload list location
		moduleParams->autoloadListStart = autoloadListStart + newcodeInfo->binSize;
		moduleParams->autoloadListEnd = autoloadListEnd + newcodeInfo->binSize + 12;

		// Write the new autoload list after the new code
		u8* writeAutoloadPtr = data.data() + binAutoloadListStart + newcodeInfo->binSize;
		for (ArmBin::AutoLoadEntry& entry : autoloadList)
		{
			u32 entryData[3];
			entryData[0] = entry.address;
			entryData[1] = entry.size;
			entryData[2] = entry.bssSize;
			std::memcpy(writeAutoloadPtr, entryData, 12);
			writeAutoloadPtr += 12;
		}
	}
}

void PatchMaker::applyNewcodeToOverlay(int dest, const std::unique_ptr<NewcodePatch>& newcodeInfo, const PatchOperationContext& context)
{
	const BuildTarget::Region* region = m_target->getRegionByDestination(dest);
	if (region == nullptr)
		throw ncp::exception("region of overlay " + std::to_string(dest) + " set to add code could not be found!");

	switch (region->mode)
	{
	case BuildTarget::Mode::Append:
		handleAppendModeOverlay(dest, newcodeInfo);
		break;
	case BuildTarget::Mode::Replace:
		handleReplaceModeOverlay(dest, newcodeInfo);
		break;
	case BuildTarget::Mode::Create:
		handleCreateModeOverlay(dest, newcodeInfo);
		break;
	}
}

void PatchMaker::handleAppendModeOverlay(int dest, const std::unique_ptr<NewcodePatch>& newcodeInfo)
{
	OverlayBin* bin = getOverlay(dest);
	auto& ovtEntries = m_fileSystemManager->getOvtEntries();
	auto& ovtEntry = ovtEntries[dest];

	ovtEntry.compressed = 0;
	ovtEntry.flag = 0;

	std::vector<u8>& data = bin->data();
	std::size_t szData = data.size();

	std::size_t totalOvSize = szData + ovtEntry.bssSize + newcodeInfo->binSize + newcodeInfo->bssSize;
	
	// Find the region for this destination
	const BuildTarget::Region* region = m_target->getRegionByDestination(dest);
	if (region) {
		validateOverlaySize(dest, totalOvSize, *region);
	}

	if (newcodeInfo->binSize > 0)
	{
		std::size_t newSzData = szData + ovtEntry.bssSize + newcodeInfo->binSize;
		data.resize(newSzData);
		u8* pData = data.data();
		std::memset(&pData[szData], 0, ovtEntry.bssSize); // Keep original BSS as data
		
		// Write new code after BSS
		writeNewcodeData(&pData[szData + ovtEntry.bssSize], newcodeInfo, nullptr);
		
		ovtEntry.ramSize = newSzData;
		ovtEntry.bssSize = newcodeInfo->bssSize; // Set the BSS to our new code BSS
	}
	else
	{
		ovtEntry.bssSize += newcodeInfo->bssSize;
	}

	bin->setDirty(true);
}

void PatchMaker::handleReplaceModeOverlay(int dest, const std::unique_ptr<NewcodePatch>& newcodeInfo)
{
	OverlayBin* bin = getOverlay(dest);
	auto& ovtEntries = m_fileSystemManager->getOvtEntries();
	auto& ovtEntry = ovtEntries[dest];

	u32 newcodeAddr = m_newcodeAddrForDest[dest];
	
	ovtEntry.ramAddress = newcodeAddr;
	ovtEntry.ramSize = newcodeInfo->binSize;
	ovtEntry.bssSize = newcodeInfo->bssSize;
	ovtEntry.sinitStart = 0;
	ovtEntry.sinitEnd = 0;
	ovtEntry.compressed = 0;
	ovtEntry.flag = 0;

	std::size_t totalOvSize = newcodeInfo->binSize + newcodeInfo->bssSize;
	
	// Find the region for this destination
	const BuildTarget::Region* region = m_target->getRegionByDestination(dest);
	if (region) {
		validateOverlaySize(dest, totalOvSize, *region);
	}

	std::vector<u8>& data = bin->data();
	
	// Write the new data
	if (newcodeInfo->binSize == 0)
	{
		data.clear();
	}
	else
	{
		data.resize(newcodeInfo->binSize);
		writeNewcodeData(data.data(), newcodeInfo, nullptr);
	}

	bin->setDirty(true);
}

void PatchMaker::handleCreateModeOverlay(int dest, const std::unique_ptr<NewcodePatch>& newcodeInfo)
{
	// TO BE DESIGNED.
	throw ncp::exception("Creating new overlays is not yet supported.");
}

// Helper methods implementation
ArmBin* PatchMaker::getArm() const
{
	return m_fileSystemManager->getArm();
}

OverlayBin* PatchMaker::getOverlay(std::size_t ovID) const
{
	return m_fileSystemManager->getOverlay(ovID);
}

ICodeBin* PatchMaker::getBinaryForDestination(int destination) const
{
	return (destination == -1) ?
		static_cast<ICodeBin*>(getArm()) :
		static_cast<ICodeBin*>(getOverlay(destination));
}

void PatchMaker::validateThumbInterworking(const std::unique_ptr<GenericPatchInfo>& patch) const
{
	if (patch->destThumb != patch->srcThumb && !m_target->getArm9())
	{
		std::ostringstream oss;
		oss << "Cannot create thumb-interworking veneer: BLX not supported on armv4. At ";
		oss << OSTRa(patch->formatPatchDescriptor()) << " (" << OSTR(patch->unit->getSourcePath().string()) << ")";
		throw ncp::exception(oss.str());
	}
}

void PatchMaker::validateOverlaySize(int dest, std::size_t totalSize, const BuildTarget::Region& region) const
{
	if (totalSize > region.length)
	{
		throw ncp::exception("Overlay " + std::to_string(dest) + " exceeds max length of "
			+ std::to_string(region.length) + " bytes, got " + std::to_string(totalSize) + " bytes.");
	}
}

void PatchMaker::writeNewcodeData(u8* destination, const std::unique_ptr<NewcodePatch>& newcodeInfo, 
                                  const std::unique_ptr<AutogenDataInfo>* autogenInfo)
{
	std::size_t autogenDataSize = 0;
	if (autogenInfo != nullptr && *autogenInfo != nullptr)
		autogenDataSize = (*autogenInfo)->data.size();

	// Write the main patch data
	std::memcpy(destination, newcodeInfo->binData, newcodeInfo->binSize - autogenDataSize);
	
	// Write the autogenerated data if present
	if (autogenDataSize != 0)
	{
		std::memcpy(&destination[newcodeInfo->binSize - autogenDataSize], 
			(*autogenInfo)->data.data(), autogenDataSize);
	}
}
