#include "project_init.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace ncp::project {

namespace {

constexpr std::string_view DEFAULT_PROJECT = R"YAML(# yaml-language-server: $schema=https://raw.githubusercontent.com/TheGameratorT/NCPatcher/main/schema/ncpatcher.schema.json
version: 2

rom:
  file: game.nds
  output: build/game.nds
  backup: backup

toolchain: arm-none-eabi-

flags:
  common: [-masm-syntax-unified, -mno-unaligned-access, -mfloat-abi=soft, -mabi=aapcs]
  c: [-Os, -fomit-frame-pointer, -fno-builtin, -nostdlib, -nodefaultlibs, -nostartfiles]
  cpp: [-Os, -fomit-frame-pointer, -fno-builtin, -fno-rtti, -fno-exceptions, -std=c++20, -nostdlib, -nodefaultlibs, -nostartfiles]
  asm: [-Os, "-x assembler-with-cpp", -fomit-frame-pointer]
  ld: [-lgcc, -lc, -lstdc++, --use-blx]

targets:
  arm9:
    build: build/arm9
    includes: [include, source]
    flags:
      common: [-mcpu=arm946e-s, -marm]
    regions:
      - dest: main
        sources: source/**
)YAML";

constexpr std::string_view NSMB_PROJECT = R"YAML(# yaml-language-server: $schema=https://raw.githubusercontent.com/TheGameratorT/NCPatcher/main/schema/ncpatcher.schema.json
version: 2

rom:
  file: NSMB.nds
  output: build/NSMB.nds
  backup: backup

# NSMB's code holds arrays of file ids ended by a sentinel, and the sentinel is
# the id one past the last file the retail ROM shipped with, which is the id
# the first added file would otherwise be given. This spends it on an empty
# file so nothing loadable sits at an id the game reads as "stop".
files-reserve: z_new/reserved

toolchain: arm-none-eabi-

defines: [SDK_GCC, SDK_FINALROM]
flags:
  common: [-masm-syntax-unified, -mno-unaligned-access, -mfloat-abi=soft, -mabi=aapcs]
  c: [-Os, -fno-short-enums, -fomit-frame-pointer, -ffast-math, -fno-builtin, -nostdlib, -nodefaultlibs, -nostartfiles]
  cpp: [-Os, -fno-short-enums, -fomit-frame-pointer, -ffast-math, -fno-builtin, -fno-rtti, -fno-exceptions, -std=c++23, -nostdlib, -nodefaultlibs, -nostartfiles]
  asm: [-Os, "-x assembler-with-cpp", -fomit-frame-pointer]
  ld: [-lgcc, -lc, -lstdc++, --use-blx]

targets:
  arm9:
    build: build/arm9
    includes: ["${env.NSMB_NITRO_ROOT}/include", "${env.NSMBREF_ROOT}/include", source]
    defines: [SDK_ARM9, arm9_start=0x021901E0]
    flags:
      common: [-mcpu=arm946e-s, -marm]
    regions:
      - dest: main
        sources: [source/**, "${env.NSMBREF_ROOT}/symbols9.c"]
)YAML";

struct ProjectTemplate
{
	std::string_view document;
	// Whether the project keeps its own headers. The nsmb template takes them
	// from the SDK and the code reference instead, so an include directory
	// there would sit outside the include path and never be searched.
	bool hasIncludeDirectory;
};

ProjectTemplate projectTemplate(std::string_view name)
{
	if (name == "default")
		return { DEFAULT_PROJECT, true };
	if (name == "nsmb")
		return { NSMB_PROJECT, false };
	throw std::runtime_error("Unknown project template \"" + std::string(name) + "\".");
}

void requireDirectoryOrMissing(const fs::path& path)
{
	std::error_code error;
	if (fs::exists(path, error) && !fs::is_directory(path, error))
		throw std::runtime_error("Cannot initialize the project because " + path.string() + " is not a directory.");
	if (error)
		throw std::runtime_error("Could not inspect " + path.string() + ": " + error.message() + '.');
}

void makeDirectory(const fs::path& path)
{
	std::error_code error;
	fs::create_directories(path, error);
	if (error)
		throw std::runtime_error("Could not create " + path.string() + ": " + error.message() + '.');
}

} // namespace

void initialize(const fs::path& root, std::string_view templateName)
{
	const ProjectTemplate project = projectTemplate(templateName);
	requireDirectoryOrMissing(root);

	for (const char* name : { "ncpatcher.yaml", "ncpatcher.yml", "ncpatcher.json" })
	{
		const fs::path existing = root / name;
		std::error_code error;
		if (fs::exists(existing, error))
			throw std::runtime_error("A project configuration already exists at " + existing.string() + '.');
		if (error)
			throw std::runtime_error("Could not inspect " + existing.string() + ": " + error.message() + '.');
	}

	requireDirectoryOrMissing(root / "source");
	if (project.hasIncludeDirectory)
		requireDirectoryOrMissing(root / "include");

	makeDirectory(root);
	makeDirectory(root / "source");
	if (project.hasIncludeDirectory)
		makeDirectory(root / "include");

	const fs::path config = root / "ncpatcher.yaml";
	std::ofstream file(config, std::ios::binary);
	if (!file.is_open())
		throw std::runtime_error("Could not create " + config.string() + '.');
	file << project.document;
	if (!file)
		throw std::runtime_error("Could not write " + config.string() + '.');
}

} // namespace ncp::project
