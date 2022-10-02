#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <unordered_map>

#include "json.hpp"
#include "../types.hpp"

class BuildTarget
{
public:
	enum class Mode
	{
		append = 0,
		replace,
		create
	};

	struct Region
	{
		std::vector<std::filesystem::path> sources;
		int destination;
		Mode mode;
		bool compress;
		u32 address;
		int length;
		std::string cFlags;
		std::string cppFlags;
		std::string asmFlags;
		//std::string ldFlags;
	};

	std::unordered_map<std::string, std::string> varmap;
	int arenaLo{};
	std::vector<std::filesystem::path> includes;
	std::vector<Region> regions;
	std::filesystem::path symbols;
	std::string cFlags;
	std::string cppFlags;
	std::string asmFlags;
	std::string ldFlags;

	[[nodiscard]] constexpr bool getArm9() const { return m_isArm9; }
	[[nodiscard]] constexpr std::time_t getLastWriteTime() { return m_lastWriteTime; }
	[[nodiscard]] constexpr bool getForceRebuild() const { return m_forceRebuild; }

	constexpr void setForceRebuild(bool forceRebuild) { m_forceRebuild = forceRebuild; }

	BuildTarget();
	void load(const std::filesystem::path& targetFilePath, bool isArm9);

private:
	const std::string& getVariable(const std::string& value);
	void expandTemplates(std::string& val);
	std::string getString(const JsonMember& member);
	static void addPathRecursively(const std::filesystem::path& path, std::vector<std::filesystem::path>& out);
	void getDirectoryArray(const JsonMember& member, std::vector<std::filesystem::path>& out);
	static void readDestination(BuildTarget::Region& region, const JsonMember& member);
	static void readRegionMode(BuildTarget::Region& region, const JsonMember& member);

	bool m_isArm9{};
	std::time_t m_lastWriteTime;
	bool m_forceRebuild;
};
