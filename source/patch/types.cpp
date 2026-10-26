#include "types.hpp"

#include "../utils/util.hpp"

namespace ncp::patch {

const char* patchTypeNames[] = {
	"jump", "call", "hook",
	"setjump", "setcall", "sethook",
	"tjump", "tcall", "thook",
	"settjump", "settcall", "setthook",
	"over"
};

const char* patchOriginNames[] = {
	"section",
	"symbol",
	"symver"
};

const char* toString(PatchType patchType)
{
	return patchTypeNames[static_cast<size_t>(patchType)];
}

const char* toString(PatchOrigin patchOrigin)
{
	return patchOriginNames[static_cast<size_t>(patchOrigin)];
}

PatchType patchTypeFromString(std::string_view patchTypeName)
{
    std::size_t patchType = Util::indexOf(patchTypeName, patchTypeNames, PatchTypeCount);
    if (patchType == -1)
    {
		return PatchType::Invalid;
    }
	return static_cast<PatchType>(patchType);
}

std::string PatchInfo::getPrettyName() const
{
    std::string result = group.empty() ? "ncp_" : "ncprt_";

	if (isNcpSet)
		result += "set_";

	if (destThumb)
		result += "t";

    // Add the patch type name
    result += toString(type);
    result += "(";

    if (!group.empty())
    {
        result += group;
        result += ", ";
    }

    // Add the destination address
    result += Util::intToAddr(destAddress, 8);

    // Add overlay if it's not the main ARM binary
    if (destAddressOv != -1)
	{
        result += ", ";
        result += std::to_string(destAddressOv);
	}

    result += ")";
    return result;
}

u32 PatchInfo::getOverwriteAmount() const
{
    if (type == PatchType::Over)
        return sectionSize;
    if (type == PatchType::Jump && destThumb)
        return 8;
    return 4;
}

} // namespace ncp::patch
