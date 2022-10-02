#include "rebuildconfig.hpp"

#include <fstream>

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

	if (inputFileSize < 25)
		throw ncp::exception("rebuild.bin file is invalid, expected the file to have at least 25 bytes.");

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

	if ((3 * sizeof(std::time_t)) + 8 + (arm7PatchedOvCount * 4) + (arm9PatchedOvCount * 4) > inputFileSize)
		throw ncp::exception("rebuild.bin file is invalid, rebuild overlay count is more than it holds.");
	
	arm7PatchedOvs.resize(arm7PatchedOvCount);
	arm9PatchedOvs.resize(arm9PatchedOvCount);

	for (u32& ovID : arm7PatchedOvs)
		ovID = read.template operator()<u32>();
	for (u32& ovID : arm9PatchedOvs)
		ovID = read.template operator()<u32>();

	fs::current_path(curPath);
}

void save()
{
	fs::path curPath = fs::current_path();
	fs::current_path(Main::getWorkPath());
	const fs::path& rebFile = BuildConfig::getBackupDir() / "rebuild.bin";

	u32 arm7PatchedOvCount = arm7PatchedOvs.size();
	u32 arm9PatchedOvCount = arm9PatchedOvs.size();

	std::vector<u8> data;
	std::size_t dataSize = (3 * sizeof(std::time_t)) + 8 + (arm7PatchedOvCount * 4) + (arm9PatchedOvCount * 4);
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

	for (u32 ovID : arm7PatchedOvs)
		write.template operator()<u32>(ovID);
	for (u32 ovID : arm9PatchedOvs)
		write.template operator()<u32>(ovID);

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

void setBuildConfigWriteTime(std::time_t value) { buildConfigWriteTime = value; }
void setArm7TargetWriteTime(std::time_t value) { arm7TargetWriteTime = value; }
void setArm9TargetWriteTime(std::time_t value) { arm9TargetWriteTime = value; }

}
