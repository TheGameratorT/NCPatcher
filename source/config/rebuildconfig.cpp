#include "rebuildconfig.hpp"

#include <fstream>
#include <cstring>

#include "buildconfig.hpp"
#include "../main.hpp"
#include "../except.hpp"
#include "../util.hpp"

namespace fs = std::filesystem;

namespace RebuildConfig {

static std::time_t buildConfigWriteTime;
static std::time_t arm7TargetWriteTime;
static std::time_t arm9TargetWriteTime;
static std::vector<u32> arm7PatchedOvs;
static std::vector<u32> arm9PatchedOvs;
static std::vector<std::string> defines;

void load()
{
	fs::path curPath = fs::current_path();
	fs::current_path(Main::getWorkPath());
	const fs::path& rebFile = BuildConfig::getBackupDir() / "rebuild.bin";

	if (!fs::exists(rebFile))
	{
		buildConfigWriteTime = std::numeric_limits<std::time_t>::max();
		arm7TargetWriteTime = std::numeric_limits<std::time_t>::max();
		arm9TargetWriteTime = std::numeric_limits<std::time_t>::max();
		fs::current_path(curPath);
		return;
	}

	std::vector<u8> data;
	std::ifstream inputFile(rebFile, std::ios::binary);
	if (!inputFile.is_open())
		throw ncp::file_error(rebFile, ncp::file_error::read);
	std::uintmax_t inputFileSize = fs::file_size(rebFile);
	data.resize(inputFileSize);
	u8* pData = data.data();
	inputFile.read(reinterpret_cast<char*>(pData), inputFileSize);
	inputFile.close();

	if (inputFileSize < 29)
		throw ncp::exception("rebuild.bin file is invalid, expected the file to have at least 29 bytes.");

	u8* curDataPtr = pData;
	auto read = [&curDataPtr]<typename T>(){
		T value = Util::read<T>(curDataPtr);
		curDataPtr += sizeof(T);
		return value;
	};

	buildConfigWriteTime = read.template operator()<std::time_t>();
	arm7TargetWriteTime = read.template operator()<std::time_t>();
	arm9TargetWriteTime = read.template operator()<std::time_t>();

	u32 arm7PatchedOvCount = read.template operator()<u32>();
	u32 arm9PatchedOvCount = read.template operator()<u32>();
	u32 definesCount = read.template operator()<u32>();

	// Calculate required size for overlays and defines
	std::size_t requiredSize = (3 * sizeof(std::time_t)) + 12 + (arm7PatchedOvCount * 4) + (arm9PatchedOvCount * 4);
	
	// Check if we have enough data for the overlay counts and defines count
	if (requiredSize > inputFileSize)
		throw ncp::exception("rebuild.bin file is invalid, overlay count is more than it holds.");
	
	arm7PatchedOvs.resize(arm7PatchedOvCount);
	arm9PatchedOvs.resize(arm9PatchedOvCount);

	for (u32& ovID : arm7PatchedOvs)
		ovID = read.template operator()<u32>();
	for (u32& ovID : arm9PatchedOvs)
		ovID = read.template operator()<u32>();

	// Read defines
	defines.clear();
	defines.reserve(definesCount);
	for (u32 i = 0; i < definesCount; ++i) {
		u32 defineLength = read.template operator()<u32>();
		
		// Check if we have enough remaining data for this string
		if (curDataPtr + defineLength > pData + inputFileSize)
			throw ncp::exception("rebuild.bin file is invalid, define string length exceeds file size.");
		
		std::string define(reinterpret_cast<const char*>(curDataPtr), defineLength);
		curDataPtr += defineLength;
		defines.push_back(std::move(define));
	}

	fs::current_path(curPath);
}

void save()
{
	fs::path curPath = fs::current_path();
	fs::current_path(Main::getWorkPath());
	const fs::path& rebFile = BuildConfig::getBackupDir() / "rebuild.bin";

	u32 arm7PatchedOvCount = arm7PatchedOvs.size();
	u32 arm9PatchedOvCount = arm9PatchedOvs.size();
	u32 definesCount = defines.size();

	// Calculate total size needed for defines
	std::size_t definesSize = 0;
	for (const std::string& define : defines) {
		definesSize += 4 + define.length(); // 4 bytes for length + string data
	}

	std::vector<u8> data;
	std::size_t dataSize = (3 * sizeof(std::time_t)) + 12 + (arm7PatchedOvCount * 4) + (arm9PatchedOvCount * 4) + definesSize;
	data.resize(dataSize);
	u8* pData = data.data();

	u8* curDataPtr = pData;
	auto write = [&curDataPtr]<typename T>(T value){
		Util::write<T>(curDataPtr, value);
		curDataPtr += sizeof(T);
	};

	write.template operator()<std::time_t>(buildConfigWriteTime);
	write.template operator()<std::time_t>(arm7TargetWriteTime);
	write.template operator()<std::time_t>(arm9TargetWriteTime);

	write.template operator()<u32>(arm7PatchedOvCount);
	write.template operator()<u32>(arm9PatchedOvCount);
	write.template operator()<u32>(definesCount);

	for (u32 ovID : arm7PatchedOvs)
		write.template operator()<u32>(ovID);
	for (u32 ovID : arm9PatchedOvs)
		write.template operator()<u32>(ovID);

	// Write defines
	for (const std::string& define : defines) {
		write.template operator()<u32>(static_cast<u32>(define.length()));
		std::memcpy(curDataPtr, define.data(), define.length());
		curDataPtr += define.length();
	}

	std::ofstream outputFile(rebFile, std::ios::binary);
	if (!outputFile.is_open())
		throw ncp::file_error(rebFile, ncp::file_error::write);
	outputFile.write(reinterpret_cast<const char*>(pData), std::streamsize(dataSize));
	outputFile.close();

	fs::current_path(curPath);
}

std::time_t getBuildConfigWriteTime() { return buildConfigWriteTime; }
std::time_t getArm7TargetWriteTime() { return arm7TargetWriteTime; }
std::time_t getArm9TargetWriteTime() { return arm9TargetWriteTime; }
std::vector<u32>& getArm7PatchedOvs() { return arm7PatchedOvs; }
std::vector<u32>& getArm9PatchedOvs() { return arm9PatchedOvs; }
const std::vector<std::string>& getDefines() { return defines; }

void setBuildConfigWriteTime(std::time_t value) { buildConfigWriteTime = value; }
void setArm7TargetWriteTime(std::time_t value) { arm7TargetWriteTime = value; }
void setArm9TargetWriteTime(std::time_t value) { arm9TargetWriteTime = value; }
void setDefines(const std::vector<std::string>& value) { defines = value; }

}
