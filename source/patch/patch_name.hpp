#pragma once

#include <string>
#include <string_view>

#include "../utils/types.hpp"
#include "types.hpp"

namespace ncp::patch {

// What a patch name encodes, before the T and SET forms are folded into the
// base patch type.
struct PatchName
{
	PatchType type = PatchType::Invalid;
	u32 destAddress = 0;
	int destAddressOv = -1;

	// The runtime group the patch belongs to, empty for a build-time patch.
	std::string group;
};

enum class PatchNameForm
{
	// <type>_<address>[_ov<overlay>][_g<group>], from a section or label name.
	// The group is the last token and may contain underscores.
	Label,

	// The label form followed by three ids (counter, line and file) that keep
	// the symver names unique. The ids are ignored.
	Symver
};

// Parses a patch name with its "ncp_", ".ncp_" or "__ncp_" prefix removed.
// Returns false when the name is not a patch; `error` then holds a warning to
// show, or is empty when the name is not meant to be one.
bool parsePatchName(std::string_view name, PatchNameForm form, PatchName& out, std::string& error);

} // namespace ncp::patch
