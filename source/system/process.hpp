#pragma once

#include <ostream>

namespace Process
{
	int start(const char* cmd, std::ostream* out = nullptr);
	bool exists(const char* app);
}
