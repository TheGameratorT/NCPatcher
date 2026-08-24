#include "config_dump.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

#include "../system/log.hpp"
#include "../utils/json.hpp"

namespace fs = std::filesystem;

namespace ncp {

namespace {

using config::Source;

std::string regionName(int destination)
{
	return destination < 0 ? std::string("main") : "ov" + std::to_string(destination);
}

const char* modeName(BuildTarget::Mode mode)
{
	switch (mode)
	{
	case BuildTarget::Mode::Replace: return "replace";
	case BuildTarget::Mode::Create:  return "create";
	default:                         return "append";
	}
}

std::string hex(u32 value, int digits = 8)
{
	std::ostringstream oss;
	oss << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(digits) << value;
	return oss.str();
}

// Splits a joined command-line string back into tokens for display. The
// resolver joins flags with spaces because that is what the compiler driver
// wants; one flag per line is what a person reading a diff wants.
std::vector<std::string> tokenize(const std::string& flags, char separator = ' ')
{
	std::vector<std::string> out;
	std::string current;
	for (char c : flags)
	{
		if (c == separator)
		{
			if (!current.empty())
				out.push_back(std::exchange(current, std::string()));
		}
		else
		{
			current += c;
		}
	}
	if (!current.empty())
		out.push_back(current);
	return out;
}

// JSON ==================================================================

void writeTargetJson(Json::Writer& writer, const ResolvedTarget& entry, bool explain)
{
	const config::TargetConfig& cfg = *entry.config;
	const BuildTarget& target = entry.target;

	writer.beginObject();
	writer.field("enabled", cfg.enabled);
	writer.field("file", cfg.file.string());
	writer.key("build-dir").value(cfg.buildDir.value.string());
	writer.key("work-dir").value(cfg.workDir.value.string());
	writer.field("symbols", target.symbols.string());
	writer.key("arena-lo").hex(static_cast<u32>(target.arenaLo));

	writer.key("includes").beginArray();
	for (const fs::path& include : target.includes)
		writer.value(include.string());
	writer.endArray();

	writer.key("flags").beginObject();
	writer.field("c", tokenize(target.cFlags));
	writer.field("cpp", tokenize(target.cppFlags));
	writer.field("asm", tokenize(target.asmFlags));
	// ld flags are comma-joined, because that is how they reach -Wl,.
	writer.field("ld", tokenize(target.ldFlags, ','));
	writer.endObject();

	writer.key("regions").beginArray();
	for (const BuildTarget::Region& region : target.regions)
	{
		writer.beginObject();
		writer.field("dest", regionName(region.destination));
		writer.field("destination", region.destination);
		writer.field("mode", modeName(region.mode));
		writer.field("compress", region.compress);
		writer.key("address").hex(region.address);
		writer.field("maxsize", region.maxsize);

		writer.key("flags").beginObject();
		writer.field("c", tokenize(region.cFlags));
		writer.field("cpp", tokenize(region.cppFlags));
		writer.field("asm", tokenize(region.asmFlags));
		writer.endObject();

		writer.key("overwrites").beginArray();
		for (const BuildTarget::Overwrites& overwrite : region.overwrites)
		{
			writer.beginObject();
			writer.key("start").hex(overwrite.startAddress);
			writer.key("end").hex(overwrite.endAddress);
			writer.endObject();
		}
		writer.endArray();

		writer.key("sources").beginArray();
		for (const fs::path& source : region.sources)
			writer.value(source.string());
		writer.endArray();

		writer.endObject();
	}
	writer.endArray();

	if (explain)
	{
		writer.key("origins").beginObject();
		writer.key("build-dir").value(config::sourceName(cfg.buildDir.source));
		writer.key("work-dir").value(config::sourceName(cfg.workDir.source));
		writer.field("symbols", config::sourceName(cfg.symbols.source));
		writer.key("arena-lo").value(config::sourceName(cfg.arenaLo.source));
		writer.endObject();
	}

	writer.endObject();
}

void dumpJson(std::ostream& out,
              const config::ProjectConfig& config,
              const PathContext& paths,
              const std::vector<ResolvedTarget>& targets,
              const DumpOptions& options)
{
	Json::Writer writer(out, 2);
	writer.beginObject();
	writer.field("schema", "ncpatcher.config/1");
	writer.field("version", config.version);
	writer.field("file", config.file.string());
	writer.key("project-root").value(config.projectRoot.string());
	// Both keys are always present, one of them empty: a consumer should not
	// have to test for a key's existence to find out which mode a project is in.
	writer.key("rom-dir").value(paths.romDir.string());
	writer.key("rom-file").value(
		config.romFile.configured() ? paths.work(config.romFile.value).string() : std::string());
	writer.key("rom-output").value(
		config.romOutput.configured() ? paths.work(config.romOutput.value).string() : std::string());
	writer.key("backup-dir").value(paths.work(config.backupDir.value).string());
	writer.field("toolchain", config.toolchain.value);
	writer.field("threads", config.threadCount.value);

	writer.key("vars").beginObject();
	{
		// Sorted, so that two dumps of the same project compare cleanly; the
		// map itself is unordered.
		std::vector<std::string> names;
		names.reserve(config.vars.size());
		for (const auto& [name, value] : config.vars)
			names.push_back(name);
		std::sort(names.begin(), names.end());
		for (const std::string& name : names)
			writer.field(name, config.vars.at(name));
	}
	writer.endObject();

	writer.key("hooks").beginArray();
	for (const config::HookConfig& hook : config.hooks)
	{
		writer.beginObject();
		writer.field("name", hook.name);
		writer.field("run", hook.run);
		writer.field("cwd", hook.cwd.string());
		writer.field("when", config::hookWhenName(hook.when));
		writer.key("env").beginObject();
		for (const auto& [name, value] : hook.env)
			writer.field(name, value);
		writer.endObject();
		writer.endObject();
	}
	writer.endArray();

	writer.key("files").beginObject();
	for (const config::FileConfig& file : config.files)
		writer.field(file.path, file.source.string());
	writer.endObject();

	writer.key("variants").beginObject();
	for (const config::VariantConfig& variant : config.variants)
	{
		writer.key(variant.name).beginObject();
		writer.field("defines", variant.defines);
		writer.key("files").beginObject();
		for (const config::FileConfig& file : variant.files)
			writer.field(file.path, file.source.string());
		writer.endObject();
		writer.endObject();
	}
	writer.endObject();

	writer.key("targets").beginObject();
	for (const ResolvedTarget& entry : targets)
	{
		writer.key(entry.config->name);
		writeTargetJson(writer, entry, options.explain);
	}
	writer.endObject();

	if (options.explain)
	{
		writer.key("origins").beginObject();
		writer.field("toolchain", config::sourceName(config.toolchain.source));
		writer.field("threads", config::sourceName(config.threadCount.source));
		writer.key("backup-dir").value(config::sourceName(config.backupDir.source));
		writer.key("rom-dir").value(config::sourceName(config.filesystemDir.source));
		writer.key("rom-file").value(config::sourceName(config.romFile.source));
		writer.endObject();
	}

	writer.endObject();
	out << '\n';
}

// Human =================================================================

std::string origin(Source source, bool explain)
{
	if (!explain)
		return {};
	return std::string("  ") + ANSI_bBLACK "(from " + config::sourceName(source) + ")" ANSI_RESET;
}

void writeList(std::ostream& out, const char* label, const std::vector<std::string>& entries)
{
	if (entries.empty())
		return;
	out << "    " << label << ":\n";
	for (const std::string& entry : entries)
		out << "      " << entry << '\n';
}

void dumpHuman(std::ostream& out,
               const config::ProjectConfig& config,
               const PathContext& paths,
               const std::vector<ResolvedTarget>& targets,
               const DumpOptions& options)
{
	const bool explain = options.explain;

	out << ANSI_bWHITE "Project" ANSI_RESET "\n";
	out << "  file:         " << config.file.string() << '\n';
	out << "  schema:       version " << config.version << '\n';
	out << "  root:         " << config.projectRoot.string() << '\n';
	if (config.romFile.configured())
	{
		out << "  rom file:     " << paths.work(config.romFile.value).string()
		    << origin(config.romFile.source, explain) << '\n';
		if (config.romOutput.configured())
		{
			out << "  rom output:   " << paths.work(config.romOutput.value).string()
			    << origin(config.romOutput.source, explain) << '\n';
		}
	}
	else
	{
		out << "  rom dir:      " << paths.romDir.string()
		    << origin(config.filesystemDir.source, explain) << '\n';
	}
	out << "  backup dir:   " << paths.work(config.backupDir.value).string()
	    << origin(config.backupDir.source, explain) << '\n';
	out << "  toolchain:    " << config.toolchain.value
	    << origin(config.toolchain.source, explain) << '\n';
	out << "  threads:      " << config.threadCount.value
	    << origin(config.threadCount.source, explain) << '\n';

	if (!config.vars.empty())
	{
		std::vector<std::string> names;
		names.reserve(config.vars.size());
		for (const auto& [name, value] : config.vars)
			names.push_back(name);
		std::sort(names.begin(), names.end());

		out << "  vars:\n";
		for (const std::string& name : names)
			out << "    " << name << " = " << config.vars.at(name) << '\n';
	}

	if (!config.variants.empty())
	{
		out << "  variants:\n";
		for (const config::VariantConfig& variant : config.variants)
		{
			out << "    " << variant.name << '\n';
			for (const std::string& define : variant.defines)
				out << "      define: " << define << '\n';
			for (const config::FileConfig& file : variant.files)
				out << "      file: " << file.path << " <- " << file.source.string() << '\n';
		}
	}

	for (const ResolvedTarget& entry : targets)
	{
		const config::TargetConfig& cfg = *entry.config;
		const BuildTarget& target = entry.target;

		out << '\n' << ANSI_bWHITE << "Target " << cfg.name << ANSI_RESET;
		if (!cfg.enabled)
			out << " (disabled)";
		out << '\n';
		out << "  file:         " << cfg.file.string() << '\n';
		out << "  build dir:    " << cfg.buildDir.value.string()
		    << origin(cfg.buildDir.source, explain) << '\n';
		out << "  arena lo:     " << hex(static_cast<u32>(target.arenaLo))
		    << origin(cfg.arenaLo.source, explain) << '\n';
		if (!target.symbols.empty())
			out << "  symbols:      " << target.symbols.string()
			    << origin(cfg.symbols.source, explain) << '\n';

		if (!target.includes.empty())
		{
			out << "  includes:\n";
			for (const fs::path& include : target.includes)
				out << "    " << include.string() << '\n';
		}

		out << "  flags:\n";
		writeList(out, "c", tokenize(target.cFlags));
		writeList(out, "cpp", tokenize(target.cppFlags));
		writeList(out, "asm", tokenize(target.asmFlags));
		writeList(out, "ld", tokenize(target.ldFlags, ','));

		for (const BuildTarget::Region& region : target.regions)
		{
			out << "  region " << ANSI_bYELLOW << regionName(region.destination) << ANSI_RESET
			    << "  mode=" << modeName(region.mode);
			if (region.address != 0)
				out << " address=" << hex(region.address);
			out << " maxsize=" << hex(static_cast<u32>(region.maxsize), 1);
			if (region.compress)
				out << " compress";
			out << '\n';

			for (const BuildTarget::Overwrites& overwrite : region.overwrites)
			{
				out << "    overwrite " << hex(overwrite.startAddress)
				    << " .. " << hex(overwrite.endAddress) << '\n';
			}

			writeList(out, "c", tokenize(region.cFlags));
			writeList(out, "cpp", tokenize(region.cppFlags));
			writeList(out, "asm", tokenize(region.asmFlags));

			out << "    sources: " << region.sources.size() << '\n';
			for (const fs::path& source : region.sources)
				out << "      " << source.string() << '\n';
		}
	}

	out << std::flush;
}

} // namespace

void dumpConfig(std::ostream& out,
                const config::ProjectConfig& config,
                const PathContext& paths,
                const std::vector<ResolvedTarget>& targets,
                const DumpOptions& options)
{
	if (options.json)
		dumpJson(out, config, paths, targets, options);
	else
		dumpHuman(out, config, paths, targets, options);
}

} // namespace ncp
