// Tests for source/system/diagnostics.{hpp,cpp}.
//
// The point of ScopedContext is that it behaves correctly on *both* exits, so
// each case below checks what a failure would be attributed to afterwards.
// Run via ctest, or directly: ./diagnostics_test

#include "../source/system/diagnostics.hpp"

#include <iostream>
#include <string>
#include <vector>

using ncp::Diag;
using ncp::DiagContext;
using ncp::ScopedContext;
namespace diagnostics = ncp::diagnostics;

static int g_failures = 0;

static void check(bool condition, const std::string& what)
{
	if (!condition)
	{
		std::cout << "FAIL: " << what << "\n";
		g_failures++;
	}
}

// The chain a handler would print, innermost first: "NCP1001:desc|NCP2003:desc".
static std::string renderFailure()
{
	std::string out;
	for (const DiagContext& frame : diagnostics::failureContext())
	{
		if (!out.empty())
			out += '|';
		out += ncp::diagCode(frame.code) + ":" + frame.description;
	}
	return out;
}

static void testCodeFormatting()
{
	check(ncp::diagCode(Diag::None).empty(), "Diag::None renders as nothing");
	check(ncp::diagCode(Diag::ConfigLoad) == "NCP0001", "single digit is zero padded to four");
	check(ncp::diagCode(Diag::PostBuildCommand) == "NCP0004", "NCP0004");
	check(ncp::diagCode(Diag::TargetCompile) == "NCP1001", "four digits are not padded");
	check(ncp::diagCode(Diag::RomHeaderLoad) == "NCP3001", "NCP3001");
}

static void testNormalExitLeavesNothing()
{
	diagnostics::clearFailureContext();
	{
		ScopedContext ctx(Diag::TargetCompile, "compiling");
	}
	check(renderFailure().empty(), "a scope that returns normally leaves no failure context");
}

// The bug this class exists to fix: setErrorContext(x) ... setErrorContext(nullptr)
// only cleared on the success path, so a phase that returned early -- or one that
// simply forgot the pairing -- left its description attached to whatever failed
// next, in a completely unrelated phase.
static void testNoLeakIntoLaterPhase()
{
	diagnostics::clearFailureContext();
	{
		ScopedContext ctx(Diag::RomHeaderLoad, "loading header");
	}
	try
	{
		ScopedContext ctx(Diag::PostBuildCommand, "running post-build");
		throw std::runtime_error("boom");
	}
	catch (const std::exception&) {}

	check(renderFailure() == "NCP0004:running post-build",
		"a later failure is not attributed to an earlier, completed phase");
}

static void testThrowCapturesWholeChain()
{
	diagnostics::clearFailureContext();
	try
	{
		ScopedContext outer(Diag::TargetCompile, "compiling");
		{
			ScopedContext inner(Diag::PatchEnvironment, "preparing");
			throw std::runtime_error("boom");
		}
	}
	catch (const std::exception&) {}

	check(renderFailure() == "NCP2003:preparing|NCP1001:compiling",
		"the whole stack is captured, innermost first");
}

// Every enclosing scope's destructor also runs while unwinding, and each sees a
// shallower stack than the one before. Only the first snapshot may stand.
static void testOuterFramesDoNotOverwriteSnapshot()
{
	diagnostics::clearFailureContext();
	try
	{
		ScopedContext a(Diag::TargetCompile, "a");
		ScopedContext b(Diag::PatchInit, "b");
		ScopedContext c(Diag::PatchProcessing, "c");
		throw std::runtime_error("boom");
	}
	catch (const std::exception&) {}

	check(renderFailure() == "NCP2005:c|NCP2001:b|NCP1001:a",
		"unwinding through outer scopes does not truncate the snapshot");
}

// A destructor running during someone else's unwinding must not be mistaken for
// its own scope failing.
static void testDestructorDuringUnrelatedUnwinding()
{
	diagnostics::clearFailureContext();
	struct Unwinder
	{
		~Unwinder()
		{
			ScopedContext ctx(Diag::PatchFinalize, "cleanup");
		}
	};

	try
	{
		Unwinder u;
		throw std::runtime_error("boom");
	}
	catch (const std::exception&) {}

	check(renderFailure().empty(),
		"a scope opened and closed during unwinding is not recorded as the failure");
}

static void testHandledFailureDoesNotStickToNextRun()
{
	diagnostics::clearFailureContext();
	try
	{
		ScopedContext ctx(Diag::ConfigLoad, "first");
		throw std::runtime_error("boom");
	}
	catch (const std::exception&) {}
	check(renderFailure() == "NCP0001:first", "first failure recorded");

	// A new outermost phase starting is enough; the handler is not obliged to
	// have called clearFailureContext().
	{
		ScopedContext ctx(Diag::TargetCompile, "second");
	}
	check(renderFailure().empty(), "opening a new outermost phase drops the old failure");
}

int main()
{
	testCodeFormatting();
	testNormalExitLeavesNothing();
	testNoLeakIntoLaterPhase();
	testThrowCapturesWholeChain();
	testOuterFramesDoNotOverwriteSnapshot();
	testDestructorDuringUnrelatedUnwinding();
	testHandledFailureDoesNotStickToNextRun();

	if (g_failures == 0)
	{
		std::cout << "All diagnostics tests passed.\n";
		return 0;
	}
	std::cout << g_failures << " diagnostics test(s) failed.\n";
	return 1;
}
