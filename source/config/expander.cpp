#include "expander.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <utility>

#include "../system/log.hpp"

namespace ncp::config {

void Expander::setConstant(std::string name, std::string value)
{
	m_constants[std::move(name)] = std::move(value);
}

void Expander::setVariable(std::string name, std::string rawValue, cfg::Node origin)
{
	Variable variable;
	variable.raw = std::move(rawValue);
	variable.origin = std::move(origin);
	m_variables[std::move(name)] = std::move(variable);
}

void Expander::setOverride(std::string name, std::string value)
{
	m_overrides[std::move(name)] = std::move(value);
}

bool Expander::hasVariable(std::string_view name) const
{
	return m_variables.find(std::string(name)) != m_variables.end();
}

std::vector<std::string> Expander::knownNames() const
{
	std::vector<std::string> out;
	out.reserve(m_constants.size() + m_variables.size() + m_overrides.size());
	for (const auto& [name, value] : m_constants)
		out.push_back(name);
	for (const auto& [name, variable] : m_variables)
		out.push_back("vars." + name);
	for (const auto& [name, value] : m_overrides)
		out.push_back("vars." + name);
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

std::string Expander::resolveVariable(const std::string& name, const cfg::Node& where) const
{
	const auto override_ = m_overrides.find(name);
	if (override_ != m_overrides.end())
		return override_->second;

	const auto it = m_variables.find(name);
	if (it == m_variables.end())
	{
		std::ostringstream oss;
		oss << "Unknown variable " << OSTR("vars." + name) << "." OREASONNL "Known names: ";
		const std::vector<std::string> names = knownNames();
		for (std::size_t i = 0; i < names.size(); i++)
			oss << (i ? ", " : "") << names[i];
		where.fail(oss.str());
	}

	const Variable& variable = it->second;
	if (variable.done)
		return variable.resolved;

	if (std::find(m_resolving.begin(), m_resolving.end(), name) != m_resolving.end())
	{
		std::ostringstream oss;
		oss << "Variable " << OSTR("vars." + name) << " refers to itself." OREASONNL "Cycle: ";
		for (const std::string& step : m_resolving)
			oss << "vars." << step << " -> ";
		oss << "vars." << name;
		where.fail(oss.str());
	}

	m_resolving.push_back(name);
	// The variable's own text is expanded against the node it was declared on,
	// so an error inside it points at the declaration rather than at whichever
	// setting happened to read it first.
	std::string value;
	try {
		value = expand(variable.raw, variable.origin);
	} catch (...) {
		m_resolving.pop_back();
		throw;
	}
	m_resolving.pop_back();

	variable.resolved = std::move(value);
	variable.done = true;
	return variable.resolved;
}

std::string Expander::lookup(std::string_view name, const cfg::Node& where) const
{
	// ${env.NAME} and ${env.NAME:-fallback}. The fallback form is what keeps a
	// project buildable by someone who has not set the variable, without the
	// config having to hard-code somebody's home directory.
	if (name.starts_with("env."))
	{
		std::string_view rest = name.substr(4);
		std::string_view fallback;
		bool hasFallback = false;

		const std::size_t sep = rest.find(":-");
		if (sep != std::string_view::npos)
		{
			fallback = rest.substr(sep + 2);
			rest = rest.substr(0, sep);
			hasFallback = true;
		}

		if (rest.empty())
			where.fail("Empty environment variable name in " ANSI_bCYAN "${env.}" ANSI_RESET ".");

		const char* value = std::getenv(std::string(rest).c_str());
		if (value != nullptr)
			return value;
		if (hasFallback)
			return std::string(fallback);

		std::ostringstream oss;
		oss << "Environment variable " << OSTR(std::string(rest)) << " is not set."
		    OREASONNL "Write " ANSI_bCYAN "${env." << rest << ":-default}" ANSI_RESET
		    " to give it a fallback.";
		where.fail(oss.str());
	}

	if (name.starts_with("vars."))
		return resolveVariable(std::string(name.substr(5)), where);

	const auto constant = m_constants.find(std::string(name));
	if (constant != m_constants.end())
		return constant->second;

	std::ostringstream oss;
	oss << "Unknown reference " << OSTR("${" + std::string(name) + "}") << ".";
	if (name.find('.') == std::string_view::npos)
		oss << OREASONNL "Project variables are spelled " ANSI_bCYAN "${vars." << name << "}" ANSI_RESET ".";
	oss << OREASONNL "Known names: ";
	const std::vector<std::string> names = knownNames();
	for (std::size_t i = 0; i < names.size(); i++)
		oss << (i ? ", " : "") << names[i];
	where.fail(oss.str());
}

std::string Expander::expand(std::string_view text, const cfg::Node& where) const
{
	std::string out;
	out.reserve(text.size());

	for (std::size_t i = 0; i < text.size(); i++)
	{
		if (text[i] != '$')
		{
			out += text[i];
			continue;
		}

		// "$$" is the escape for a literal '$'; the second one is consumed and
		// never examined, so "$${x}" comes out as the text "${x}".
		if (i + 1 < text.size() && text[i + 1] == '$')
		{
			out += '$';
			i++;
			continue;
		}

		// A '$' that does not open a reference is just a dollar sign. Shell
		// snippets in hooks are full of them.
		if (i + 1 >= text.size() || text[i + 1] != '{')
		{
			out += '$';
			continue;
		}

		const std::size_t end = text.find('}', i + 2);
		if (end == std::string_view::npos)
		{
			std::ostringstream oss;
			oss << "Unterminated " ANSI_bCYAN "${" ANSI_RESET " in " << OSTR(std::string(text)) << ".";
			where.fail(oss.str());
		}

		out += lookup(text.substr(i + 2, end - (i + 2)), where);
		i = end;
	}

	return out;
}

} // namespace ncp::config
