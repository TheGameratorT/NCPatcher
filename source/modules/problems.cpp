#include "problems.hpp"

#include <sstream>

#include "../system/log.hpp"
#include "../system/except.hpp"

namespace ncp::modules {

void Problems::add(std::string message)
{
	m_messages.push_back(std::move(message));
}

void Problems::add(const cfg::Node& where, std::string message)
{
	addAt(where.location(), std::move(message));
}

void Problems::addAt(std::string_view location, std::string message)
{
	if (location.empty())
	{
		m_messages.push_back(std::move(message));
		return;
	}

	std::ostringstream oss;
	oss << message << OREASONNL "at " << OSTR(location);
	m_messages.push_back(oss.str());
}

void Problems::raise(const char* summary) const
{
	if (m_messages.empty())
		return;

	std::ostringstream oss;
	oss << summary;
	for (const std::string& message : m_messages)
		oss << "\n\n  " << message;

	throw ncp::exception(oss.str());
}

} // namespace ncp::modules
