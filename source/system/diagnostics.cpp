#include "diagnostics.hpp"

#include <exception>

namespace ncp {

namespace {

// Thread-local: worker threads do not push contexts of their own, and an
// exception raised on one is rethrown on the main thread, whose own context is
// the one worth reporting.
thread_local std::vector<DiagContext> t_stack;
thread_local std::vector<DiagContext> t_failure;
thread_local bool t_frozen = false;

} // namespace

std::string diagCode(Diag code)
{
	if (code == Diag::None)
		return {};

	std::string digits = std::to_string(static_cast<unsigned>(code));
	return "NCP" + std::string(digits.size() < 4 ? 4 - digits.size() : 0, '0') + digits;
}

ScopedContext::ScopedContext(Diag code, std::string description)
	: m_uncaught(std::uncaught_exceptions())
{
	// A context opening at the outermost level means the previous failure, if
	// any, has been dealt with and a new phase has begun.
	if (t_stack.empty())
	{
		t_frozen = false;
		t_failure.clear();
	}

	t_stack.push_back({ code, std::move(description) });
}

ScopedContext::~ScopedContext()
{
	// More exceptions in flight than when this scope opened means it is being
	// unwound. The innermost such scope snapshots the stack; the outer ones it
	// propagates through must not overwrite that with their shallower view.
	if (!t_frozen && std::uncaught_exceptions() > m_uncaught)
	{
		t_failure.assign(t_stack.rbegin(), t_stack.rend());
		t_frozen = true;
	}

	t_stack.pop_back();
}

namespace diagnostics {

const std::vector<DiagContext>& failureContext()
{
	return t_failure;
}

void clearFailureContext()
{
	t_failure.clear();
	t_frozen = false;
}

} // namespace diagnostics

} // namespace ncp
