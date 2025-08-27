#include "types.hpp"

#include "../utils/util.hpp"

namespace patch {

std::string GenericPatchInfo::formatPatchDescriptor() const
{
    std::string result = "ncp_";

	if (isNcpSet)
		result += "set_";
    
    // Add the patch type name
    result += PatchTypeUtils::getName(patchType);
    result += "(";
    
    // Add the destination address
    result += Util::intToAddr(destAddress, 8);
    
    // Add overlay if it's not the main ARM binary (-1)
    if (destAddressOv != -1)
    {
        result += ", ";
        result += std::to_string(destAddressOv);
    }
    
    result += ")";
    return result;
}

} // namespace patch
