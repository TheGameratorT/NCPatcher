#include "project_config.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

#include "../system/log.hpp"

namespace ncp::config {

const char* sourceName(Source source)
{
	switch (source)
	{
	case Source::Default:       return "default";
	case Source::ProjectFile:   return "project";
	case Source::TargetSection: return "target";
	case Source::RegionSection: return "region";
	case Source::Environment:   return "environment";
	case Source::CommandLine:   return "command line";
	}
	return "unknown";
}

const char* regionModeName(RegionMode mode)
{
	switch (mode)
	{
	case RegionMode::Append:  return "append";
	case RegionMode::Replace: return "replace";
	case RegionMode::Create:  return "create";
	}
	return "unknown";
}

// ListOp =============================

bool ListOp::empty() const
{
	return !hasSet && set.empty() && remove.empty() && append.empty();
}

std::vector<std::string> ListOp::applyTo(std::vector<std::string> inherited) const
{
	std::vector<std::string> out = hasSet ? set : std::move(inherited);

	if (!remove.empty())
	{
		out.erase(std::remove_if(out.begin(), out.end(),
			[this](const std::string& entry) {
				return std::find(remove.begin(), remove.end(), entry) != remove.end();
			}), out.end());
	}

	out.insert(out.end(), append.begin(), append.end());
	return out;
}

// DefineSet =============================

void DefineSet::add(std::string_view text, std::string origin, bool warnOnConflict)
{
	Define define;
	const std::size_t equals = text.find('=');
	if (equals == std::string_view::npos)
	{
		define.name = std::string(text);
	}
	else
	{
		define.name = std::string(text.substr(0, equals));
		define.value = std::string(text.substr(equals + 1));
		define.hasValue = true;
	}
	define.origin = std::move(origin);
	add(std::move(define), warnOnConflict);
}

void DefineSet::add(Define define, bool warnOnConflict)
{
	for (Define& existing : m_defines)
	{
		if (existing.name != define.name)
			continue;

		// Redefining to the same thing is how inheritance is expected to behave;
		// only a changed value is worth interrupting for, and only where the
		// caller says a change is unexpected.
		if (warnOnConflict && (existing.hasValue != define.hasValue || existing.value != define.value))
		{
			std::ostringstream oss;
			oss << OWARN << "Define " << OSTR(define.name) << " redefined";
			if (!define.origin.empty())
				oss << " by " << define.origin;
			oss << ", was " << OSTRa(existing.hasValue ? existing.value : std::string("(no value)"));
			if (!existing.origin.empty())
				oss << " from " << existing.origin;
			oss << ".";
			Log::out << oss.str() << std::endl;
		}

		existing = std::move(define);
		return;
	}

	m_defines.push_back(std::move(define));
}

void DefineSet::merge(const DefineSet& other, bool warnOnConflict)
{
	for (const Define& define : other.m_defines)
		add(define, warnOnConflict);
}

void DefineSet::remove(std::string_view name)
{
	m_defines.erase(std::remove_if(m_defines.begin(), m_defines.end(),
		[name](const Define& define) { return define.name == name; }), m_defines.end());
}

const DefineSet::Define* DefineSet::find(std::string_view name) const
{
	for (const Define& define : m_defines)
	{
		if (define.name == name)
			return &define;
	}
	return nullptr;
}

std::vector<std::string> DefineSet::toFlags() const
{
	std::vector<std::string> out;
	out.reserve(m_defines.size());
	for (const Define& define : m_defines)
		out.push_back(define.hasValue ? "-D" + define.name + "=" + define.value : "-D" + define.name);
	return out;
}

std::vector<std::string> DefineSet::toStrings() const
{
	std::vector<std::string> out;
	out.reserve(m_defines.size());
	for (const Define& define : m_defines)
		out.push_back(define.hasValue ? define.name + "=" + define.value : define.name);
	return out;
}

// FlagOps / FlagLists =============================

bool FlagOps::empty() const
{
	return common.empty() && c.empty() && cpp.empty() && asm_.empty() && ld.empty();
}

FlagLists FlagLists::inheritedBy(const FlagOps& ops) const
{
	FlagLists out;
	out.common = ops.common.applyTo(common);
	out.c = ops.c.applyTo(c);
	out.cpp = ops.cpp.applyTo(cpp);
	out.asm_ = ops.asm_.applyTo(asm_);
	out.ld = ops.ld.applyTo(ld);
	return out;
}

} // namespace ncp::config
