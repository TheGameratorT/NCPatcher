#include "cancel.hpp"

#include <atomic>
#include <csignal>

#include "except.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace ncp::cancel {

namespace {

// Written from a signal handler, so it has to be lock-free and it has to be the
// only thing the handler does. Everything that follows from a cancellation --
// the message, the result document, the exit code -- happens on the build's own
// thread, at the next checkpoint.
std::atomic<bool> s_requested{ false };

#ifdef _WIN32

BOOL WINAPI consoleHandler(DWORD event)
{
	switch (event)
	{
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
		s_requested.store(true, std::memory_order_relaxed);
		// Handled: without this the process is terminated where it stands, which
		// is the behaviour this exists to replace.
		return TRUE;
	default:
		return FALSE;
	}
}

#else

extern "C" void signalHandler(int)
{
	s_requested.store(true, std::memory_order_relaxed);
}

#endif

} // namespace

void install()
{
#ifdef _WIN32
	SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
	// SIGTERM as well as SIGINT: a caller that spawned this without a terminal
	// has no Ctrl-C to send, and asking it to look up SIGINT specifically would
	// be a trap. Both mean the same thing here.
	std::signal(SIGINT, signalHandler);
	std::signal(SIGTERM, signalHandler);
#endif
}

bool requested()
{
	return s_requested.load(std::memory_order_relaxed);
}

void checkpoint()
{
	if (requested())
		throw ncp::cancelled();
}

void reset()
{
	s_requested.store(false, std::memory_order_relaxed);
}

} // namespace ncp::cancel
