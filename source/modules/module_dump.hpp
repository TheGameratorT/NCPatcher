#pragma once

// The machine-readable module graph, and the two human views of it.
//
// The dump is a contract. Everything game-specific that the Python prototype
// did -- allocating object ids, writing profile tables, mapping filesystem
// entries -- is downstream of exactly this file, so that NCPatcher never has to
// know what an "actor" is and the generator never has to parse a module.yaml.
// Which is also why unknown component keys survive the round trip: `objects:`
// is meaningless here and load-bearing there.

#include <ostream>
#include <string>

#include "module_graph.hpp"

namespace ncp::modules {

// `ncpatcher modules dump`. Schema "ncpatcher.modules/1".
void writeDump(std::ostream& out, const ModuleGraph& graph);

// `ncpatcher modules list`.
void writeList(std::ostream& out, const ModuleGraph& graph);

// `ncpatcher modules explain <Module>` or `<Module.Component>`.
// Throws ncp::exception naming the near misses when nothing matches.
void writeExplanation(std::ostream& out, const ModuleGraph& graph, const std::string& what);

} // namespace ncp::modules
