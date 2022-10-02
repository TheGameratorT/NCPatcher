#include "main.hpp"

#include <vector>
#include <filesystem>

#include "types.hpp"
#include "process.hpp"
#include "log.hpp"
#include "config/buildconfig.hpp"
#include "config/buildtarget.hpp"
#include "config/rebuildconfig.hpp"
#include "ndsbin/headerbin.hpp"
#include "ndsbin/armbin.hpp"
#include "build/sourcefilejob.hpp"
#include "build/objmaker.hpp"
#include "patch/patchmaker.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#elif __linux__
#include <unistd.h>
#else
#error Unsupported operating system
#endif

namespace fs = std::filesystem;

namespace Main {

static std::filesystem::path s_appPath;
static std::filesystem::path s_workPath;
static std::filesystem::path s_romPath;
static const char* s_errorContext = nullptr;

const std::filesystem::path& getAppPath() { return s_appPath; }
const std::filesystem::path& getWorkPath() { return s_workPath; }
const std::filesystem::path& getRomPath() { return s_romPath; }
void setErrorContext(const char* errorContext) { s_errorContext = errorContext; }

}

static void ncpMain()
{
	Log::out << ANSI_bWHITE " ----- Nitro Code Patcher -----" ANSI_RESET << std::endl;

	BuildConfig::load();
	RebuildConfig::load();

	const fs::path& workDir = Main::getWorkPath();
	Main::s_romPath = fs::absolute(BuildConfig::getFilesystemDir());

	HeaderBin header;
	header.load(Main::s_romPath / "header.bin");

	bool forceRebuild = false;

	auto doWorkOnTarget = [&](bool isArm9){
		Log::info(isArm9 ?
			"Loading ARM9 target configuration..." :
			"Loading ARM7 target configuration...");

		const fs::path& targetPath = fs::absolute(isArm9 ? BuildConfig::getArm9Target() : BuildConfig::getArm7Target());

		Main::setErrorContext(isArm9 ?
			"Could not load the ARM9 target configuration." :
			"Could not load the ARM7 target configuration.");
		BuildTarget buildTarget;
		buildTarget.load(targetPath, isArm9);
		Main::setErrorContext(nullptr);

		std::time_t lastTargetWriteTimeNew = buildTarget.getLastWriteTime();
		std::time_t lastTargetWriteTimeOld = isArm9 ?
			RebuildConfig::getArm9TargetWriteTime() :
			RebuildConfig::getArm7TargetWriteTime();
		buildTarget.setForceRebuild(forceRebuild || (lastTargetWriteTimeNew > lastTargetWriteTimeOld));

		Main::setErrorContext(isArm9 ?
			"Could not compile the ARM9 target." :
			"Could not compile the ARM7 target.");

		fs::path targetDir = targetPath.parent_path();
		fs::path buildPath = fs::absolute(isArm9 ? BuildConfig::getArm9BuildDir() : BuildConfig::getArm7BuildDir());

		std::vector<std::unique_ptr<SourceFileJob>> srcFileJobs;

		ObjMaker objMaker;
		objMaker.makeTarget(buildTarget, targetDir, buildPath, srcFileJobs);

		PatchMaker patchMaker;
		patchMaker.makeTarget(buildTarget, targetDir, buildPath, header, srcFileJobs);

		isArm9 ?
			RebuildConfig::setArm9TargetWriteTime(lastTargetWriteTimeNew) :
			RebuildConfig::setArm7TargetWriteTime(lastTargetWriteTimeNew);

		Main::setErrorContext(nullptr);
	};

	fs::current_path(workDir);

	if (BuildConfig::getLastWriteTime() > RebuildConfig::getBuildConfigWriteTime())
		forceRebuild = true;

	if (BuildConfig::getBuildArm7())
		doWorkOnTarget(false);

	if (BuildConfig::getBuildArm9())
		doWorkOnTarget(true);

	RebuildConfig::setBuildConfigWriteTime(BuildConfig::getLastWriteTime());
	RebuildConfig::save();

	Log::info("All tasks finished.");
}

static std::filesystem::path fetchAppPath()
{
	// Copied from arclight.filesystem

#ifdef _WIN32

	u32 length = 0x200;
	std::vector<wchar_t> filename;

	try
	{
		filename.resize(length);
		while (GetModuleFileNameW(nullptr, filename.data(), length) == length)
		{
			if (length < 0x8000)
			{
				length *= 2;
				filename.resize(length);
			}
			else
			{
				/*
					Ideally, this cannot happen because the windows path limit is specified to be 0x7FFF (excl. null terminator byte)
					If this changes in future windows versions, long path names could fail since it would require to allocate fairly large buffers
					This is why we stop here with an error.
				*/
				throw std::runtime_error("Could not query application directory path: Path too long");
			}
		}

		std::wstring str(filename.data());
		return std::filesystem::path(str).parent_path();
	}
	catch (std::exception& e)
	{
		throw std::runtime_error(std::string("Could not query application directory path: ") + e.what());
	}

#elif __linux__

	constexpr const char* symlinkName = "/proc/self/exe";
	SizeT length = 0x200;

	std::vector<char> filename(length);

	try
	{
		while(true)
		{
			ssize_t readLength = readlink(symlinkName, filename.data(), filename.size());

			if (readLength == length)
			{
				//If length exceeds 0x10000 bytes, cancel
				if(length >= 0x10000)
					throw std::runtime_error("Could not query application directory path: Path name exceeds 0x10000 bytes");

				//Double buffer and retry
				length *= 2;
				filename.resize(length);
			}
			else if (readLength == -1)
			{
				//Error occured while reading the symlink
				throw std::runtime_error("Could not query application directory path: Cannot read symbolic link");
			}
			else
			{
				//Read was successful, return filename
				std::string str(filename.data(), readLength);
				return std::filesystem::path(str).parent_path();
			}
		}
	}
	catch (std::exception& e)
	{
		throw std::runtime_error(std::string("Could not query application directory path: ") + e.what());
	}

#endif
}

int main(int argc, char* argv[])
{
	Log::init();

	try {
		Main::s_appPath = fetchAppPath();
	} catch (std::exception& ex) {
		Log::error(ex.what());
		return 1;
	}

	try {
		Main::s_workPath = fs::current_path();
	} catch (std::exception& ex) {
		Log::error("Could not query the application work directory path.");
		return 1;
	}

	try {
		Log::openLogFile(Main::s_appPath / "log.txt");
	} catch (std::exception& ex) {
		Log::error("Could not open the log file for writing.");
		return 1;
	}

	try
	{
		ncpMain();
	}
	catch (std::exception& e)
	{
		Log::out << OERROR;
		if (Main::s_errorContext)
			Log::out << Main::s_errorContext << "\n" << OREASON;
		Log::out << e.what();
		return 1;
	}

	return 0;
}
