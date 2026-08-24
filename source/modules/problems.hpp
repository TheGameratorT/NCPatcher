#pragma once

// A list of configuration problems, reported together.
//
// Module resolution is the one part of this program where stopping at the first
// error is actively unhelpful: a project enables a dozen modules at once, and a
// mistake in the first of them says nothing about the other eleven. Finding
// them one build at a time is how a five-minute edit becomes an afternoon.
//
// Warnings go straight to the log as they are found, because they do not stop
// anything; errors accumulate here and are raised as one exception at the end
// of whichever pass was allowed to finish.

#include <string>
#include <string_view>
#include <vector>

#include "../config/node.hpp"

namespace ncp::modules {

class Problems
{
public:
	void add(std::string message);
	void add(const cfg::Node& where, std::string message);

	// For a problem found after the document that described it has been closed:
	// the project's module selections keep cfg::Node::location() rather than the
	// node, because a Node may not outlive its Document.
	void addAt(std::string_view location, std::string message);

	[[nodiscard]] bool empty() const { return m_messages.empty(); }
	[[nodiscard]] std::size_t size() const { return m_messages.size(); }

	// Throws if anything was collected; does nothing otherwise. `summary` opens
	// the message and should say what failed, not how many times.
	void raise(const char* summary) const;

private:
	std::vector<std::string> m_messages;
};

} // namespace ncp::modules
