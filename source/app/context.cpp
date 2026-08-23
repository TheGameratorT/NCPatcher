#include "context.hpp"

#include "../config/project_config.hpp"

namespace fs = std::filesystem;

namespace ncp {

bool Options::isVerbose(VerboseTag tag) const
{
	return verboseTags.count(VerboseTag::All) > 0 || verboseTags.count(tag) > 0;
}

const std::string& Context::toolchain() const
{
	return config->toolchain.value;
}

int Context::threadCount() const
{
	return config->threadCount.value;
}

fs::path Context::backupDir() const
{
	return paths.work(config->backupDir.value);
}

} // namespace ncp
