#include "../source/app/project_init.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
	if (!condition)
	{
		std::cout << "FAIL: " << message << '\n';
		failures++;
	}
}

std::string read(const fs::path& path)
{
	std::ifstream file(path, std::ios::binary);
	std::ostringstream out;
	out << file.rdbuf();
	return out.str();
}

bool rejects(const fs::path& root, std::string_view templateName)
{
	try {
		ncp::project::initialize(root, templateName);
	} catch (const std::exception&) {
		return true;
	}
	return false;
}

} // namespace

int main()
{
	const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	const fs::path root = fs::temp_directory_path() / ("ncpatcher-init-test-" + std::to_string(nonce));
	const fs::path generic = root / "generic";
	const fs::path nsmb = root / "nsmb";

	ncp::project::initialize(generic, "default");
	const std::string genericConfig = read(generic / "ncpatcher.yaml");
	check(fs::is_directory(generic / "source"), "default creates source/");
	check(fs::is_directory(generic / "include"), "default creates include/");
	check(genericConfig.starts_with("# yaml-language-server: $schema="), "default emits the schema header");
	check(genericConfig.find("version: 2") != std::string::npos, "default emits v2");
	check(genericConfig.find("file: game.nds") != std::string::npos, "default names the generic ROM");

	check(rejects(generic, "default"), "a second init is refused");
	check(read(generic / "ncpatcher.yaml") == genericConfig, "a refused init does not overwrite the config");

	ncp::project::initialize(nsmb, "nsmb");
	const std::string nsmbConfig = read(nsmb / "ncpatcher.yaml");
	check(nsmbConfig.find("file: NSMB.nds") != std::string::npos, "nsmb names the conventional ROM");
	check(nsmbConfig.find("symbols: symbols9.x") != std::string::npos, "nsmb includes game symbols");
	check(nsmbConfig.find("-std=c++23") != std::string::npos, "nsmb keeps the template language level");

	check(rejects(root / "unknown", "other"), "an unknown template is refused");
	check(!fs::exists(root / "unknown" / "ncpatcher.yaml"), "an unknown template writes nothing");

	fs::remove_all(root);
	if (failures == 0)
		std::cout << "project_init_test: all checks passed\n";
	return failures == 0 ? 0 : 1;
}
