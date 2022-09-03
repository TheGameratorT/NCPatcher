#include "main.hpp"

#include <filesystem>
#include <vector>

#include "types.hpp"
#include "process.hpp"
#include "log.hpp"
#include "config/buildconfig.hpp"
#include "config/buildtarget.hpp"
#include "ndsbin/headerbin.hpp"
#include "ndsbin/armbin.hpp"
#include "build/objmaker.hpp"

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
static const char* s_errorContext = nullptr;

const std::filesystem::path& getAppPath() { return s_appPath; }
const std::filesystem::path& getWorkPath() { return s_workPath; }
void setErrorContext(const char* errorContext) { s_errorContext = errorContext; }

}

static void ncpMain()
{
	Log::out << ANSI_bWHITE " ----- Nitro Code Patcher -----" ANSI_RESET << std::endl;

	BuildConfig::load();

	const fs::path& workDir = Main::getWorkPath();
	const fs::path& fsDir = BuildConfig::getFilesystemDir();

	HeaderBin header;
	header.load(fsDir / "header.bin");

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

		const char* armFile = isArm9 ? "arm9.bin" : "arm7.bin";
		u32 entryAddress = isArm9 ? header.arm9.entryAddress : header.arm7.entryAddress;
		u32 ramAddress = isArm9 ? header.arm9.ramAddress : header.arm7.ramAddress;
		u32 autoLoadListHookOff = isArm9 ? header.arm9AutoLoadListHookOffset : header.arm7AutoLoadListHookOffset;

		ArmBin armBin;
		armBin.load(fsDir / armFile, entryAddress, ramAddress, autoLoadListHookOff, isArm9);

		Main::setErrorContext(isArm9 ?
			"Could not compile the ARM9 target." :
			"Could not compile the ARM7 target.");

		const fs::path& buildPath = isArm9 ? BuildConfig::getArm9BuildDir() : BuildConfig::getArm7BuildDir();

		ObjMaker maker;
		maker.makeTarget(buildTarget, targetPath.parent_path(), fs::absolute(buildPath));

		Main::setErrorContext(nullptr);
	};

	fs::current_path(workDir);

	if (BuildConfig::getBuildArm7())
		doWorkOnTarget(false);

	if (BuildConfig::getBuildArm9())
		doWorkOnTarget(true);

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
