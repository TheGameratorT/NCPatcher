#pragma once

#include <ostream>

namespace process
{
	int start(const char* cmd, std::ostream* out = nullptr);
}
