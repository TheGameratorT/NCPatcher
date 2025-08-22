#include "patchmaker.hpp"

#include <sstream>
#include <cstring>

#include "filesystem_manager.hpp"
#include "patch_info_analyzer.hpp"
#include "overwrite_region_manager.hpp"
#include "linker_script_generator.hpp"
#include "elf_analyzer.hpp"
#include "assembly_code_generator.hpp"

#include "arenalofinder.hpp"

#include "../main.hpp"
#include "../log.hpp"
#include "../except.hpp"
#include "../config/rebuildconfig.hpp"
#include "../util.hpp"
#include "../ndsbin/icodebin.hpp"

/*
 * TODO: Endianness checks
 * */

constexpr std::size_t SizeOfHookBridge = 20;
constexpr std::size_t SizeOfArm2ThumbJumpBridge = 8;

struct PatchType {
    enum {
        Jump, Call, Hook, Over,
        SetJump, SetCall, SetHook,
        RtRepl,
        TJump, TCall, THook,
        SetTJump, SetTCall, SetTHook,
    };
};

PatchMaker::PatchMaker() = default;
PatchMaker::~PatchMaker() = default;

void PatchMaker::makeTarget(
	const BuildTarget& target,
	const std::filesystem::path& targetWorkDir,
	const std::filesystem::path& buildDir,
	const HeaderBin& header,
	std::vector<std::unique_ptr<SourceFileJob>>& srcFileJobs
	)
{
	m_target = &target;
	m_targetWorkDir = &targetWorkDir;
	m_buildDir = &buildDir;
	m_header = &header;
	m_srcFileJobs = &srcFileJobs;

	if (m_srcFileJobs->empty())
		throw ncp::exception("There are no source files to link.");

	// Initialize component managers
	m_fileSystemManager = std::make_unique<FileSystemManager>();
	m_patchInfoAnalyzer = std::make_unique<PatchInfoAnalyzer>();
	m_overwriteRegionManager = std::make_unique<OverwriteRegionManager>();
	m_linkerScriptGenerator = std::make_unique<LinkerScriptGenerator>();
	m_elfAnalyzer = std::make_unique<ElfAnalyzer>();

	// Initialize all components
	m_fileSystemManager->initialize(target, buildDir, header);
	m_patchInfoAnalyzer->initialize(target, srcFileJobs, targetWorkDir);
	m_overwriteRegionManager->initialize(target);
	m_linkerScriptGenerator->initialize(target, buildDir, srcFileJobs, m_newcodeAddrForDest);
	m_elfAnalyzer->initialize(buildDir / (target.getArm9() ? "arm9.elf" : "arm7.elf"));

	// Execute the patch creation pipeline
	m_fileSystemManager->createBuildDirectory();
	m_fileSystemManager->createBackupDirectory();

	m_fileSystemManager->loadArmBin();
	m_fileSystemManager->loadOverlayTableBin();

	std::vector<u32>& patchedOverlays = m_target->getArm9() ?
		RebuildConfig::getArm7PatchedOvs() :
		RebuildConfig::getArm9PatchedOvs();

	for (u32 ovID : patchedOverlays)
		m_fileSystemManager->loadOverlayBin(ovID);

	fetchNewcodeAddr();
	
	// Analyze patches and sections
	m_patchInfoAnalyzer->gatherInfoFromObjects();
	m_overwriteRegionManager->setupOverwriteRegions();
	
	// Get data from analyzers
	auto candidateSections = m_patchInfoAnalyzer->takeOverwriteCandidateSections();
	m_overwriteRegionManager->assignSectionsToOverwrites(candidateSections);
	
	// Generate linker script and link
	m_linkerScriptGenerator->createLinkerScript(
		m_patchInfoAnalyzer->getPatchInfo(),
		m_patchInfoAnalyzer->getRtreplPatches(),
		m_patchInfoAnalyzer->getExternSymbols(),
		m_patchInfoAnalyzer->getDestWithNcpSet(),
		m_patchInfoAnalyzer->getJobsWithNcpSet(),
		m_overwriteRegionManager->getOverwriteRegions()
	);
	m_linkerScriptGenerator->linkElfFile();
	
	// Analyze the linked ELF and apply patches
	m_elfAnalyzer->loadElfFile();
	
	auto patchInfo = m_patchInfoAnalyzer->takePatchInfo();
	m_elfAnalyzer->gatherInfoFromElf(patchInfo, m_overwriteRegionManager->getOverwriteRegions());
	
	auto newcodeDataForDest = m_elfAnalyzer->takeNewcodeDataForDest();
	auto autogenDataInfoForDest = m_elfAnalyzer->takeAutogenDataInfoForDest();
	
	applyPatchesToRom(patchInfo, newcodeDataForDest, autogenDataInfoForDest);
	m_elfAnalyzer->unloadElfFile();

	patchedOverlays.clear();
	for (const auto& [id, ov] : m_fileSystemManager->getLoadedOverlays())
	{
		if (ov->getDirty())
			patchedOverlays.push_back(id);
	}

	m_fileSystemManager->saveOverlayBins();
	m_fileSystemManager->saveOverlayTableBin();
	m_fileSystemManager->saveArmBin();
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

void PatchMaker::applyPatchesToRom(
	const std::vector<std::unique_ptr<GenericPatchInfo>>& patchInfo,
	const std::unordered_map<int, std::unique_ptr<NewcodePatch>>& newcodeDataForDest,
	const std::unordered_map<int, std::unique_ptr<AutogenDataInfo>>& autogenDataInfoForDest
)
{
	Main::setErrorContext(m_target->getArm9() ?
		"Failed to apply patches for ARM9 target." :
		"Failed to apply patches for ARM7 target.");

	Log::info("Patching the binaries...");

	auto failInject = [](const std::unique_ptr<GenericPatchInfo>& p, bool srcThumb, bool destThumb, const char* injectType){
		std::ostringstream oss;
		oss << "Injecting " << injectType << " from " << (destThumb ? "THUMB" : "ARM") << " to "
			<< (srcThumb ? "THUMB" : "ARM") << " is not supported, at "
			<< OSTRa(p->symbol) << " (" << OSTR(p->job->srcFilePath.string()) << ")";
		throw ncp::exception(oss.str());
	};

	auto sh_tbl = m_elfAnalyzer->getElf()->getSectionHeaderTable();

	for (const auto& p : patchInfo)
	{
		ICodeBin* bin = (p->destAddressOv == -1) ?
						static_cast<ICodeBin*>(getArm()) :
						static_cast<ICodeBin*>(getOverlay(p->destAddressOv));

		switch (p->patchType)
		{
		case PatchType::Jump:
		{
			if (!p->destThumb && !p->srcThumb) // ARM -> ARM
			{
				bin->write<u32>(p->destAddress, AssemblyCodeGenerator::makeJumpOpCode(AssemblyCodeGenerator::armOpcodeB, p->destAddress, p->srcAddress));
			}
			else if (!p->destThumb && p->srcThumb) // ARM -> THUMB
			{
				/*
				 * If the patch type is a ARM to THUMB jump, the instruction at
				 * destAddr must become a jump to a ARM to THUMB jump bridge generated
				 * by NCPatcher and it should look as such:
				 *
				 * arm2thumb_jump_bridge:
				 *     LDR   PC, [PC,#-4]
				 *     .int: srcAddr+1
				 * */
				
				auto infoIt = autogenDataInfoForDest.find(p->srcAddressOv);
				if (infoIt == autogenDataInfoForDest.end())
					throw ncp::exception("Unexpected p->srcAddressOv for autogenDataInfoForDest encountered.");

				auto& info = infoIt->second;
				std::vector<u8>& bridgeData = info->data;
				std::size_t offset = bridgeData.size();
				bridgeData.resize(offset + SizeOfArm2ThumbJumpBridge);

				u32 bridgeAddr = info->curAddress;

				if (Main::getVerbose())
					Log::out << "ARM->THUMB BRIDGE: " << Util::intToAddr(bridgeAddr, 8) << std::endl;

				bin->write<u32>(p->destAddress, AssemblyCodeGenerator::makeJumpOpCode(AssemblyCodeGenerator::armOpcodeB, p->destAddress, bridgeAddr));

				u8* bridgeDataPtr = bridgeData.data() + offset;

				Util::write<u32>(bridgeDataPtr, 0xE51FF004);            // LDR PC, [PC,#-4]
				Util::write<u32>(bridgeDataPtr + 4, p->srcAddress | 1); // int value to jump to

				if (Main::getVerbose())
					Util::printDataAsHex(bridgeData.data() + offset, SizeOfArm2ThumbJumpBridge, 32);

				info->curAddress += SizeOfArm2ThumbJumpBridge;
			}
			else if (p->destThumb && !p->srcThumb) // THUMB -> ARM
			{
				u16 patchData[4];
				patchData[0] = AssemblyCodeGenerator::thumbOpCodePushLR;
				Util::write<u32>(&patchData[1], AssemblyCodeGenerator::makeThumbCallOpCode(true, p->destAddress + 2, p->srcAddress));
				patchData[3] = AssemblyCodeGenerator::thumbOpCodePopPC;
				bin->writeBytes(p->destAddress, patchData, 8);
			}
			else // THUMB -> THUMB
			{
				u16 patchData[4];
				patchData[0] = AssemblyCodeGenerator::thumbOpCodePushLR;
				Util::write<u32>(&patchData[1], AssemblyCodeGenerator::makeThumbCallOpCode(false, p->destAddress + 2, p->srcAddress));
				patchData[3] = AssemblyCodeGenerator::thumbOpCodePopPC;
				bin->writeBytes(p->destAddress, patchData, 8);
			}
			break;
		}
		case PatchType::Call:
		{
			if (p->destThumb != p->srcThumb && !m_target->getArm9())
			{
				std::ostringstream oss;
				oss << "Cannot create thumb-interworking veneer: BLX not supported on armv4. At "
					<< OSTRa(p->symbol) << " (" << OSTR(p->job->srcFilePath.string()) << ")";
				throw ncp::exception(oss.str());
			}

			if (!p->destThumb && !p->srcThumb) // ARM -> ARM
			{
				bin->write<u32>(p->destAddress, AssemblyCodeGenerator::makeJumpOpCode(AssemblyCodeGenerator::armOpcodeBL, p->destAddress, p->srcAddress));
			}
			else if (!p->destThumb && p->srcThumb) // ARM -> THUMB
			{
				bin->write<u32>(p->destAddress, AssemblyCodeGenerator::makeBLXOpCode(p->destAddress, p->srcAddress));
			}
			else if (p->destThumb && !p->srcThumb) // THUMB -> ARM
			{
				bin->write<u32>(p->destAddress, AssemblyCodeGenerator::makeThumbCallOpCode(true, p->destAddress, p->srcAddress));
			}
			else // THUMB -> THUMB
			{
				bin->write<u32>(p->destAddress, AssemblyCodeGenerator::makeThumbCallOpCode(false, p->destAddress, p->srcAddress));
			}
			break;
		}
		case PatchType::Hook:
		{
			/*
			 * If the patch type is a hook, the instruction at
			 * destAddr must become a jump to a hook bridge generated
			 * by NCPatcher and it should look as such:
			 *
			 * hook_bridge:
			 *     PUSH {R0-R3,R12}
			 *     BL   srcAddr        @ BLX if srcAddr is THUMB
			 *     POP  {R0-R3,R12}
			 *     <unpatched destAddr's instruction>
			 *     B    (destAddr + 4)
			 * */

			if (p->destThumb)
				failInject(p, p->srcThumb, p->destThumb, "hook");

			// ARM -> ARM && ARM -> THUMB

			u32 ogOpCode = bin->read<u32>(p->destAddress);

			auto infoIt = autogenDataInfoForDest.find(p->srcAddressOv);
			if (infoIt == autogenDataInfoForDest.end())
				throw ncp::exception("Unexpected p->srcAddressOv for autogenDataInfoForDest encountered.");

			auto& info = infoIt->second;
			std::vector<u8>& hookData = info->data;
			std::size_t offset = hookData.size();
			hookData.resize(offset + SizeOfHookBridge);

			u32 hookBridgeAddr = info->curAddress;

			if (Main::getVerbose())
				Log::out << "HOOK BRIDGE: " << Util::intToAddr(hookBridgeAddr, 8) << std::endl;

			bin->write<u32>(p->destAddress, AssemblyCodeGenerator::makeJumpOpCode(AssemblyCodeGenerator::armOpcodeB, p->destAddress, hookBridgeAddr));

			u8* hookDataPtr = hookData.data() + offset;

			u32 jmpOpCode = p->srcThumb ? AssemblyCodeGenerator::makeBLXOpCode(hookBridgeAddr + 4, p->srcAddress) : AssemblyCodeGenerator::makeJumpOpCode(AssemblyCodeGenerator::armOpcodeBL, hookBridgeAddr + 4, p->srcAddress);

			Util::write<u32>(hookDataPtr, AssemblyCodeGenerator::armHookPush);
			Util::write<u32>(hookDataPtr + 4, jmpOpCode);
			Util::write<u32>(hookDataPtr + 8, AssemblyCodeGenerator::armHookPop);
			Util::write<u32>(hookDataPtr + 12, AssemblyCodeGenerator::fixupOpCode(ogOpCode, p->destAddress, hookBridgeAddr + 12));
			Util::write<u32>(hookDataPtr + 16, AssemblyCodeGenerator::makeJumpOpCode(AssemblyCodeGenerator::armOpcodeB, hookBridgeAddr + 16, p->destAddress + 4));

			if (Main::getVerbose())
				Util::printDataAsHex(hookData.data() + offset, SizeOfHookBridge, 32);

			info->curAddress += SizeOfHookBridge;
			break;
		}
		case PatchType::Over:
		{
			const char* sectionData = m_elfAnalyzer->getElf()->getSection<char>(sh_tbl[p->sectionIdx]);
			bin->writeBytes(p->destAddress, sectionData, p->sectionSize);
			break;
		}
		}
	}

	// Apply overwrite regions using sections with runtime data
	for (const auto& overwrite : m_overwriteRegionManager->getOverwriteRegions())
	{
		if (overwrite->assignedSections.empty())
			continue;

		ICodeBin* bin = (overwrite->destination == -1) ?
						static_cast<ICodeBin*>(getArm()) :
						static_cast<ICodeBin*>(getOverlay(overwrite->destination));

		const char* sectionData = m_elfAnalyzer->getElf()->getSection<char>(sh_tbl[overwrite->sectionIdx]);

		bin->writeBytes(overwrite->startAddress, sectionData, overwrite->sectionSize);
		
		if (Main::getVerbose())
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
	
	for (const auto& [dest, newcodeInfo] : newcodeDataForDest)
	{
		u32 newcodeAddr = m_newcodeAddrForDest[dest];

		// Fixes clang bug: https://marc.info/?l=llvm-bugs&m=154455815918394
		const auto& clang_dest = dest;
		const auto& clang_newcodeInfo = newcodeInfo;

		auto writeNewcode = [&](u8* addr){
			std::size_t autogenDataSize = 0;
			auto autogenDataIt = autogenDataInfoForDest.find(clang_dest);
			if (autogenDataIt != autogenDataInfoForDest.end())
				autogenDataSize = autogenDataIt->second->data.size();

			// Write the patch data
			std::memcpy(addr, clang_newcodeInfo->binData, clang_newcodeInfo->binSize - autogenDataSize);
			if (autogenDataSize != 0)
				std::memcpy(&addr[clang_newcodeInfo->binSize - autogenDataSize], autogenDataIt->second->data.data(), autogenDataSize);
		};

		if (dest == -1)
		{
			// If more data needs to be added
			if ((newcodeInfo->binSize + newcodeInfo->bssSize) != 0)
			{
				ArmBin* bin = getArm();
				std::vector<u8>& data = bin->data();

				// Extend the ARM binary
				data.resize(data.size() + newcodeInfo->binSize + 12);

				// Write the new relocated code address
				u32 heapReloc = newcodeAddr + newcodeInfo->binSize + (newcodeInfo->bssAlign - newcodeInfo->binSize % newcodeInfo->bssAlign) + newcodeInfo->bssSize;
				bin->write<u32>(m_arenalo, heapReloc);

				ArmBin::ModuleParams* moduleParams = bin->getModuleParams();
				u32 ramAddress = bin->getRamAddress();

				u32 autoloadListStart = moduleParams->autoloadListStart;
				u32 autoloadListEnd = moduleParams->autoloadListEnd;
				u32 binAutoloadListStart = moduleParams->autoloadListStart - ramAddress; // Where our new code will be placed
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
					std::memcpy(&data[binAutoloadStart + newcodeInfo->binSize], &data[binAutoloadStart], binAutoloadListStart - binAutoloadStart);

					writeNewcode(&data[binAutoloadStart]);
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
		else
		{
			const BuildTarget::Region* region = nullptr;
			for (const BuildTarget::Region& r : m_target->regions)
			{
				if (r.destination == dest)
				{
					region = &r;
					break;
				}
			}
			if (region == nullptr)
				throw ncp::exception("region of overlay " + std::to_string(dest) + " set to add code could not be found!");

			auto& ovtEntries = m_fileSystemManager->getOvtEntries();
			switch (region->mode)
			{
			case BuildTarget::Mode::Append:
			{
				OverlayBin* bin = getOverlay(dest);
				auto& ovtEntry = ovtEntries[dest];

				ovtEntry.compressed = 0; // size of compressed "ramSize"
				ovtEntry.flag = 0;

				std::vector<u8>& data = bin->data();
				std::size_t szData = data.size();

				std::size_t totalOvSize = szData + ovtEntry.bssSize + newcodeInfo->binSize + newcodeInfo->bssSize;
				if (totalOvSize > region->length)
				{
					throw ncp::exception("Overlay " + std::to_string(dest) + " exceeds max length of "
						+ std::to_string(region->length) + " bytes, got " + std::to_string(totalOvSize) + " bytes.");
				}

				if (newcodeInfo->binSize > 0)
				{
					std::size_t newSzData = szData + ovtEntry.bssSize + newcodeInfo->binSize;
					data.resize(newSzData);
					u8* pData = data.data();
					std::memset(&pData[szData], 0, ovtEntry.bssSize); // Keep original BSS as data
					writeNewcode(&pData[szData + ovtEntry.bssSize]); // Write new code after BSS
					ovtEntry.ramSize = newSzData;
					ovtEntry.bssSize = newcodeInfo->bssSize; // Set the BSS to our new code BSS
				}
				else
				{
					ovtEntry.bssSize += newcodeInfo->bssSize;
				}

				bin->setDirty(true);
				break;
			}
			case BuildTarget::Mode::Replace:
			{
				OverlayBin* bin = getOverlay(dest);
				auto& ovtEntry = ovtEntries[dest];

				ovtEntry.ramAddress = newcodeAddr;
				ovtEntry.ramSize = newcodeInfo->binSize;
				ovtEntry.bssSize = newcodeInfo->bssSize;
				ovtEntry.sinitStart = 0;
				ovtEntry.sinitEnd = 0;
				ovtEntry.compressed = 0; // size of compressed "ramSize"
				ovtEntry.flag = 0;

				std::size_t totalOvSize = newcodeInfo->binSize + newcodeInfo->bssSize;
				if (totalOvSize > region->length)
				{
					throw ncp::exception("Overlay " + std::to_string(dest) + " exceeds max length of "
						+ std::to_string(region->length) + " bytes, got " + std::to_string(totalOvSize) + " bytes.");
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
					writeNewcode(data.data());
				}

				bin->setDirty(true);
				break;
			}
			case BuildTarget::Mode::Create:
			{
				// TO BE DESIGNED.
				throw ncp::exception("Creating new overlays is not yet supported.");
				break;
			}
			}
		}
	}

	Main::setErrorContext(nullptr);
}

ArmBin* PatchMaker::getArm() const
{
	return m_fileSystemManager->getArm();
}

OverlayBin* PatchMaker::getOverlay(std::size_t ovID) const
{
	return m_fileSystemManager->getOverlay(ovID);
}
