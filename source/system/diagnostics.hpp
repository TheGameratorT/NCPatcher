#pragma once

#include <string>
#include <vector>

namespace ncp {

// Stable diagnostic identifiers.
//
// The number is the contract: tools match on "NCP1001", never on the English
// text, which is free to be reworded. A retired number is never reused.
//
//   NCP0xxx  configuration and driver
//   NCP1xxx  build (compile and link)
//   NCP2xxx  patching
//   NCP3xxx  ROM and binary I/O
enum class Diag : unsigned
{
	None = 0,

	ConfigLoad           = 1,   // reading ncpatcher.json
	TargetConfigLoad     = 2,   // reading a target's json
	PreBuildCommand      = 3,
	PostBuildCommand     = 4,

	TargetCompile        = 1001,

	PatchInit            = 2001,
	PatchFileSystemSetup = 2002,
	PatchEnvironment     = 2003,
	PatchElfGeneration   = 2004,
	PatchProcessing      = 2005,
	PatchApplication     = 2006,
	PatchFinalize        = 2007,

	RomHeaderLoad        = 3001,
	ArmBinLoad           = 3002,
};

// "NCP1001". Diag::None renders as an empty string.
[[nodiscard]] std::string diagCode(Diag code);

struct DiagContext
{
	Diag code;
	std::string description;
};

// Marks the phase the surrounding scope belongs to, so a failure anywhere
// inside it can be reported as "this is what was being attempted".
//
// Unlike the setErrorContext/setErrorContext(nullptr) pair it replaces, the
// scope always ends -- including when it ends by throwing, which is precisely
// the case the manual pairing got wrong. Leaving normally pops the context so
// it cannot be misattributed to a later phase; leaving by exception freezes the
// whole stack first, so the handler at the top still sees where the throw came
// from.
class ScopedContext
{
public:
	ScopedContext(Diag code, std::string description);
	~ScopedContext();

	ScopedContext(const ScopedContext&) = delete;
	ScopedContext& operator=(const ScopedContext&) = delete;

private:
	int m_uncaught;
};

namespace diagnostics {

// The context stack as it stood when the in-flight exception was thrown,
// innermost first. Empty if the throw happened outside any context.
[[nodiscard]] const std::vector<DiagContext>& failureContext();

// Called by whoever reported the failure, so a later unrelated one does not
// inherit this stack.
void clearFailureContext();

} // namespace diagnostics

} // namespace ncp
