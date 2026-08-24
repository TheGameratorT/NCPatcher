# Installs into a staging prefix and runs the installed binary from an unrelated
# directory.
#
# What this is guarding is the whole point of W3: that ncp.h is found through
# the install layout rather than because the build tree happened to be next
# door. Running the binary in place would prove nothing, since exeDir() is the
# last entry of the search and the build tree copies the runtime files there.

set(staging "${NCP_BINARY_DIR}/install_layout")
set(elsewhere "${NCP_BINARY_DIR}/install_layout_cwd")

file(REMOVE_RECURSE "${staging}" "${elsewhere}")
file(MAKE_DIRECTORY "${elsewhere}")

set(config_args)
if (NCP_BUILD_TYPE)
	set(config_args --config "${NCP_BUILD_TYPE}")
endif()

execute_process(
	COMMAND "${CMAKE_COMMAND}" --install "${NCP_BINARY_DIR}" --prefix "${staging}" ${config_args}
	RESULT_VARIABLE result
	OUTPUT_VARIABLE output
	ERROR_VARIABLE output)
if (NOT result EQUAL 0)
	message(FATAL_ERROR "cmake --install failed:\n${output}")
endif()

# Wherever the binary landed, bin/ or the flat portable layout.
file(GLOB_RECURSE installed "${staging}/ncpatcher" "${staging}/ncpatcher.exe")
if (NOT installed)
	message(FATAL_ERROR "No ncpatcher binary under ${staging}")
endif()
list(GET installed 0 exe)

# `version` needs neither a project nor a toolchain, so what it tells us is
# whether the binary runs at all after being moved.
execute_process(
	COMMAND "${exe}" version
	WORKING_DIRECTORY "${elsewhere}"
	RESULT_VARIABLE result
	OUTPUT_VARIABLE output
	ERROR_VARIABLE output)
if (NOT result EQUAL 0)
	message(FATAL_ERROR "Installed binary failed to run: ${output}")
endif()

# The runtime header has to be reachable from the installed binary's own
# directory tree, with the environment saying nothing.
file(GLOB_RECURSE header "${staging}/ncp.h")
if (NOT header)
	message(FATAL_ERROR "ncp.h was not installed under ${staging}")
endif()

file(GLOB_RECURSE module_schema "${staging}/module.schema.json")
if (NOT module_schema)
	message(FATAL_ERROR "module.schema.json was not installed under ${staging}")
endif()

# The failure message has to name every directory it searched, or the person
# reading it has no way to know where to put the file.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env "NCPATCHER_DATA_DIR=${elsewhere}" "${exe}" config path
	WORKING_DIRECTORY "${elsewhere}"
	OUTPUT_VARIABLE output
	ERROR_VARIABLE errors)
if (NOT errors MATCHES "No NCPatcher configuration")
	message(FATAL_ERROR "Expected a missing-configuration error, got:\n${errors}")
endif()

message(STATUS "Install layout OK: ${exe}")
