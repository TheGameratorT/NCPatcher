#include "exit_code.hpp"

namespace ncp {

ExitCode exitCodeFor(Diag code)
{
	switch (code)
	{
	case Diag::ConfigLoad:
	case Diag::TargetConfigLoad:
	case Diag::ConfigMigrate:
	case Diag::ProjectInit:
		return ExitCode::Config;

	// The SDK header ships with the program, so a missing or mismatched
	// one is the installation being wrong rather than the code being wrong --
	// the same kind of answer as a missing compiler, and not something a caller
	// should be tempted to treat as a compile error and show a source line for.
	case Diag::ToolchainMissing:
	case Diag::SdkHeaderMissing:
		return ExitCode::Toolchain;

	case Diag::ModuleResolve:
		return ExitCode::Modules;

	case Diag::PreBuildCommand:
	case Diag::PostBuildCommand:
		return ExitCode::Hook;

	case Diag::TargetCompile:
		return ExitCode::Compile;

	// Producing the ELF is the link step; everything else under patching is
	// the splice into the binaries.
	case Diag::PatchElfGeneration:
		return ExitCode::Link;

	case Diag::PatchInit:
	case Diag::PatchEnvironment:
	case Diag::PatchProcessing:
	case Diag::PatchApplication:
	case Diag::PatchFinalize:
		return ExitCode::Patch;

	// Filesystem trouble with the project's own files rather than with the
	// patch, which is what a caller wants to tell apart: a missing header.bin
	// is a broken input, not a broken patch.
	case Diag::PatchFileSystemSetup:
	case Diag::RomHeaderLoad:
	case Diag::RomAccess:
	case Diag::NitroFsInsert:
	case Diag::ArmBinLoad:
	case Diag::CleanFailed:
	case Diag::RestoreFailed:
		return ExitCode::RomIo;

	default:
		return ExitCode::Internal;
	}
}

} // namespace ncp
