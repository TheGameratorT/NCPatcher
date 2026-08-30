#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "../utils/types.hpp"

// One target, resolved: inheritance applied, variables expanded, globs matched,
// flag lists joined into the strings that go on a command line.
//
// This is the boundary between configuration and building. Everything upstream
// of it knows about YAML, v1 JSON, inheritance and merge policy; everything
// downstream (all of source/build and source/patch) knows only this. That
// is why the config rewrite could replace both readers without touching either.
//
// It is built by config::TargetResolver and is read-only afterwards, apart from
// the rebuild flag.
class BuildTarget
{
public:
	enum class Mode
	{
		Append = 0,
		Replace,
		Create
	};

	struct Overwrites
	{
		u32 startAddress;
		u32 endAddress;
	};

	struct Region
	{
		std::vector<std::filesystem::path> sources;
		int destination;
		Mode mode;
		bool compress;
		u32 address;
		int maxsize;
		std::string cFlags;
		std::string cppFlags;
		std::string asmFlags;
		std::vector<Overwrites> overwrites;
	};

	int arenaLo{};
	std::vector<std::filesystem::path> includes;
	std::vector<Region> regions;
	std::filesystem::path symbols;
	std::string cFlags;
	std::string cppFlags;
	std::string asmFlags;
	std::string ldFlags;

	[[nodiscard]] constexpr bool getArm9() const { return m_isArm9; }
	[[nodiscard]] constexpr bool getForceRebuild() const { return m_forceRebuild; }

	// Identifies this target's resolved configuration. A build compares it
	// against the one recorded by the previous build to decide whether the
	// objects on disk were compiled under the same rules.
	[[nodiscard]] const std::string& getConfigHash() const { return m_configHash; }

	constexpr void setForceRebuild(bool forceRebuild) { m_forceRebuild = forceRebuild; }

	[[nodiscard]] bool hasOverwrites() const;

	// Helper functions for region access
	[[nodiscard]] const Region* getRegionByDestination(int destination) const;
	[[nodiscard]] Region* getRegionByDestination(int destination);
	[[nodiscard]] const Region* getMainRegion() const;
	[[nodiscard]] Region* getMainRegion();

	BuildTarget();

private:
	friend struct BuildTargetBuilder;

	bool m_isArm9{};
	bool m_forceRebuild{};
	std::string m_configHash;
};

// Grants the resolver write access to the fields a built target must not change
// afterwards. Declared here rather than making them public so that "who is
// allowed to set this" is answered by the type system instead of by convention.
struct BuildTargetBuilder
{
	static void setArm9(BuildTarget& target, bool arm9) { target.m_isArm9 = arm9; }
	static void setConfigHash(BuildTarget& target, std::string hash) { target.m_configHash = std::move(hash); }
};
