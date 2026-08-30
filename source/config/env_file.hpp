#pragma once

// A project-local environment file, read before ${env.*} resolves.
//
// The problem it solves is that an environment variable is a property of the
// shell, not of the project, and a shell has exactly one of each. A machine
// building two projects against two revisions of the same code reference has
// nowhere to say so: whichever NSMBREF_ROOT the profile exports wins for both.
//
// So a project may carry a `.ncpatcher.env` naming the values it needs, and a
// tool that manages those trees can keep its own entry up to date in it.
// NCPatcher stays out of the business of knowing what the values mean, and
// reads KEY=VALUE and nothing more. No shell expansion, no `export`, no command substitution: a
// configuration file that can run commands is a configuration file that cannot
// be validated safely.
//
// Note the precedence, which is the opposite of the usual dotenv convention:
// these entries *override* the ambient environment rather than yielding to it.
// A stale global left in a shell profile is exactly the failure this exists to
// prevent, so letting it win would defeat the point. What still outranks the
// file is the command line (`--var` and the explicit CLI options) because
// that is the caller deliberately overriding the project, one invocation at a
// time.

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ncp::config {

class EnvFile
{
public:
	// The name looked for in the project directory.
	static constexpr std::string_view DEFAULT_NAME = ".ncpatcher.env";

	// Reads `file`. A file that is not there is not an error (most projects
	// have none) and leaves this empty with `loaded()` false. A file that is
	// there and malformed is an error naming the line, because a typo silently
	// dropping a variable would surface much later as a missing include path.
	void load(const std::filesystem::path& file);

	[[nodiscard]] bool loaded() const { return m_loaded; }
	[[nodiscard]] const std::filesystem::path& file() const { return m_file; }
	[[nodiscard]] bool empty() const { return m_entries.empty(); }

	// The value for `name`, or nullptr. Insertion order is preserved and a
	// later assignment of the same name replaces the earlier one.
	[[nodiscard]] const std::string* find(std::string_view name) const;

	[[nodiscard]] const std::vector<std::pair<std::string, std::string>>& entries() const
	{
		return m_entries;
	}

private:
	std::filesystem::path m_file;
	std::vector<std::pair<std::string, std::string>> m_entries;
	bool m_loaded = false;
};

} // namespace ncp::config
