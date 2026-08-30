#pragma once

// `ncpatcher migrate`: converting a v1 project to the v2 schema.
//
// The conversion is not a transcription. Reading the JSON and writing the same
// shape back in YAML would leave every project carrying the boilerplate v1
// forced on it: the same flag strings pasted across two files, the same
// $${var} concatenation standing in for inheritance it did not have, the same
// -D switches buried inside those strings. Undoing that by hand, in every
// downstream project, is the migration cost this command exists to remove.
//
// So it re-factors: flags common to both targets are hoisted to the project,
// flags common to a target's three languages are hoisted to that target, and
// every -D becomes an entry in `defines:`.
//
// That reorders flags, which is safe for the flags involved but not something
// to take on trust. The conversion therefore checks itself: it resolves the v1
// configuration and the emitted v2 configuration and compares them setting by
// setting, and refuses to write anything if they disagree.

#include <filesystem>

namespace ncp::config {

// Converts `projectFile` and writes ncpatcher.yaml beside it when `write` is
// set; otherwise prints what it would write. Returns false on failure, with the
// reason already reported.
bool migrate(const std::filesystem::path& projectFile,
             const std::filesystem::path& projectRoot,
             bool write);

} // namespace ncp::config
