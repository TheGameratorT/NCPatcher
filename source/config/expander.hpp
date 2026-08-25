#pragma once

// ${...} expansion for v2 configuration files.
//
// Two things distinguish this from the v1 string templating it replaces.
//
// It is lazy: a `vars:` entry is expanded the first time something reads it,
// not when its line is parsed. v1 expanded at parse time, which meant a
// variable could only refer to one declared above it -- the reason every
// shipped project's target JSON opens with the same fixed ordering of
// $arm_flags, $c_flags, $cpp_flags. Order stops mattering here, and a cycle is
// reported as a cycle instead of running out of stack.
//
// And it is namespaced: ${vars.x}, ${env.HOME}, ${project.root}. v1 had three
// syntaxes -- ${x} for a local variable, $${x} for a project one, ${env:X} for
// the environment -- because there was no other way to say which scope was
// meant. Scoping comes from the inheritance chain now, so the sigils are gone.

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "node.hpp"

namespace ncp::config {

class EnvFile;

class Expander
{
public:
	// A value that needs no expansion of its own: project.root, target.name and
	// friends.
	void setConstant(std::string name, std::string value);

	// A `vars:` entry. Its own text may reference other variables, so it is kept
	// raw and expanded on demand; `origin` is where an error inside it points.
	void setVariable(std::string name, std::string rawValue, cfg::Node origin);

	// A value supplied from outside the file, by --var. It wins over a `vars:`
	// entry of the same name and is used exactly as written: an override is a
	// final answer, not another template to resolve against the thing it is
	// overriding.
	void setOverride(std::string name, std::string value);

	// A project-local .ncpatcher.env, consulted by ${env.NAME} before the real
	// environment. Not owned; it must outlive this. Null means there is none.
	void setEnvFile(const EnvFile* envFile);

	// A name whose value is not known yet: it expands to itself, so a later
	// pass can finish the job. `${variant.name}` is the motivating case -- the
	// configuration is read once, before a variant has been chosen, but a hook
	// command wants to name the variant it is running for. Registering it here
	// is what keeps it from being reported as an unknown reference, and keeps
	// the deferred set closed rather than letting any unresolved text through.
	void setDeferred(std::string name);

	[[nodiscard]] bool hasVariable(std::string_view name) const;

	// Expands every reference in `text`. `where` is the node being expanded, so
	// that an unresolvable reference is reported against the line that used it.
	[[nodiscard]] std::string expand(std::string_view text, const cfg::Node& where) const;

	// The names currently known, sorted, for "did you mean" reporting.
	[[nodiscard]] std::vector<std::string> knownNames() const;

private:
	struct Variable
	{
		std::string raw;
		cfg::Node origin;
		mutable std::string resolved;
		mutable bool done = false;
	};

	[[nodiscard]] std::string lookup(std::string_view name, const cfg::Node& where) const;
	[[nodiscard]] std::string resolveVariable(const std::string& name, const cfg::Node& where) const;

	const EnvFile* m_envFile = nullptr;

	std::vector<std::string> m_deferred;

	std::unordered_map<std::string, std::string> m_constants;
	std::unordered_map<std::string, std::string> m_overrides;
	std::unordered_map<std::string, Variable> m_variables;

	// Names currently being expanded, innermost last. A name that appears twice
	// is a cycle, and the stack is what lets the error name the loop.
	mutable std::vector<std::string> m_resolving;
};

} // namespace ncp::config
