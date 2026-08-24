// Focused command-line checks for options whose values drive build orchestration.

#include "../source/app/cli.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace ncp;

static int g_failures = 0;

static CommandLine parse(std::vector<std::string> arguments)
{
	std::vector<char*> argv;
	for (std::string& argument : arguments)
		argv.push_back(argument.data());

	CommandLine out;
	const std::optional<int> stopped = parseCommandLine(int(argv.size()), argv.data(), out);
	if (stopped)
	{
		std::cout << "FAIL: command line stopped with " << *stopped << '\n';
		g_failures++;
	}
	return out;
}

int main()
{
	const CommandLine one = parse({ "ncpatcher", "build", "--variant", "fr" });
	if (one.command != Command::Build || one.variant != "fr" || one.allVariants)
	{
		std::cout << "FAIL: --variant selects one named build\n";
		g_failures++;
	}

	// A global, not a build-subcommand option: the form the level editor and
	// every script use puts it before the command.
	const CommandLine before = parse({ "ncpatcher", "--variant", "fr", "build" });
	if (before.command != Command::Build || before.variant != "fr")
	{
		std::cout << "FAIL: --variant is accepted before the command\n";
		g_failures++;
	}

	const CommandLine all = parse({ "ncpatcher", "build", "--all-variants" });
	if (all.command != Command::Build || !all.allVariants || !all.variant.empty())
	{
		std::cout << "FAIL: --all-variants selects the matrix\n";
		g_failures++;
	}

	const CommandLine init = parse({ "ncpatcher", "init", "--template", "nsmb", "-C", "new-project" });
	if (init.command != Command::Init || init.initTemplate != "nsmb" || init.projectPath != "new-project")
	{
		std::cout << "FAIL: init selects a named template and destination\n";
		g_failures++;
	}

	if (g_failures == 0)
		std::cout << "cli_test: all checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
