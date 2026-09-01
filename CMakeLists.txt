cmake_minimum_required(VERSION 3.20)

project(ncpatcher VERSION 2.0.0 LANGUAGES CXX)

include(GNUInstallDirs)

# The contract version between the binary and sdk/ncp.h. It is checked at
# run time against the header that is actually found, which may be an older one
# left behind by a previous install. The failure mode that makes this worth
# checking is silent, since a stale header compiles fine and simply stops
# emitting the sections the patcher looks for.
set(NCP_SDK_VERSION 1)

file(STRINGS "${PROJECT_SOURCE_DIR}/sdk/ncp.h" _ncp_stamp
	REGEX "^#define[ \t]+__ncp_sdk_version[ \t]+")
if (NOT _ncp_stamp MATCHES "__ncp_sdk_version[ \t]+([0-9]+)")
	message(FATAL_ERROR "sdk/ncp.h carries no __ncp_sdk_version stamp.")
elseif (NOT CMAKE_MATCH_1 STREQUAL "${NCP_SDK_VERSION}")
	message(FATAL_ERROR
		"sdk/ncp.h declares __ncp_sdk_version ${CMAKE_MATCH_1}, "
		"but NCP_SDK_VERSION here is ${NCP_SDK_VERSION}. "
		"Both have to say the same thing.")
endif()

# Windows and the portable release zips keep the whole program in one directory:
# the binary and its data files side by side, which is also how the release zips
# have always been laid out. Everywhere else follows the GNU layout, with the
# data files under share/ncpatcher.
option(NCP_PORTABLE_LAYOUT "Install everything into one flat directory" ${WIN32})
option(NCP_BUILD_TESTS "Build unit tests" OFF)

if (NCP_PORTABLE_LAYOUT)
	set(NCP_INSTALL_BINDIR ".")
	set(NCP_INSTALL_DATADIR ".")
else()
	set(NCP_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
	set(NCP_INSTALL_DATADIR "${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}")
endif()

# Where the data files sit relative to the binary. Computed rather than
# hardcoded so that a prefix using bin64/, or a distro with an unusual datadir,
# still resolves, and passed as a relative path rather than an absolute one so
# that the install can be moved, copied into a container or turned into an
# AppImage without the lookup breaking.
file(RELATIVE_PATH NCP_DATA_RELDIR
	"${CMAKE_INSTALL_PREFIX}/${NCP_INSTALL_BINDIR}"
	"${CMAKE_INSTALL_PREFIX}/${NCP_INSTALL_DATADIR}")
if (NCP_DATA_RELDIR STREQUAL "")
	set(NCP_DATA_RELDIR ".")
endif()

if (WIN32)
	add_compile_definitions(_CRT_SECURE_NO_DEPRECATE)
endif()

# MinGW, static so the result runs on a machine without the runtime DLLs. The
# old spelling tested CMAKE_C_COMPILER, which is never set here because C is not
# one of this project's languages, so the branch had been dead since it was
# written.
if (MINGW)
	set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++ -static")

	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
endif()

# yaml-cpp reads both configuration schemas: v2 is YAML, and JSON is a subset of
# YAML 1.2, so v1 comes through the same parser. Preferred from the system,
# because a build-time download is unusable offline and unusable for distro
# packaging; the fetch is the fallback for machines that do not have it.
# Distro packaging builds with this on: it turns a missing dependency into a
# configure error instead of a silent download, which is the difference between
# a package that builds reproducibly and one that reaches the network.
option(NCP_USE_SYSTEM_DEPS "Require dependencies from the system, never fetch them" OFF)

if (NCP_USE_SYSTEM_DEPS)
	find_package(yaml-cpp REQUIRED)
else()
	find_package(yaml-cpp QUIET)
endif()

if (NOT yaml-cpp_FOUND)
	message(STATUS "yaml-cpp not found on the system, fetching it")
	include(FetchContent)
	set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
	set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(yaml-cpp
		GIT_REPOSITORY "https://github.com/jbeder/yaml-cpp.git"
		GIT_TAG 0.8.0
		GIT_SHALLOW ON
	)
	# CMake 4 no longer accepts yaml-cpp 0.8's declared pre-3.5 policy set.
	set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
	FetchContent_MakeAvailable(yaml-cpp)

	# 0.8.0's emitterutils.cpp names uint16_t without including <cstdint>, which
	# GCC 13 stopped pulling in transitively. The fetch is what a machine with no
	# system package falls back to, so it has to build on a current compiler
	# rather than only on the one the CI image happens to ship.
	if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		target_compile_options(yaml-cpp PRIVATE -include cstdint)
	endif()
endif()

# 0.8 exports yaml-cpp::yaml-cpp; older packages export a bare yaml-cpp.
if (TARGET yaml-cpp::yaml-cpp)
	set(YAML_CPP_TARGET yaml-cpp::yaml-cpp)
else()
	set(YAML_CPP_TARGET yaml-cpp)
endif()

# BS::thread_pool and CLI11 are vendored in-tree, both header-only. Neither is
# packaged widely enough to rely on being installed, and both are small enough
# that carrying them costs less than an offline-hostile build-time download.
include_directories("${CMAKE_SOURCE_DIR}/vendor/thread-pool/include")

# Listed rather than globbed. A glob makes the build depend on the state of the
# filesystem at configure time, which breaks incremental reconfigure in ways
# that are hard to see, and is the first thing a distro packager objects to.
set(SOURCES
	source/main.cpp
	source/app/application.cpp
	source/app/cli.cpp
	source/app/config_dump.cpp
	source/app/project_init.cpp
	source/app/rom_command.cpp
	source/app/context.cpp
	source/build/buildlogger.cpp
	source/build/gcc_diagnostics.cpp
	source/build/objmaker.cpp
	source/config/buildtarget.cpp
	source/config/config_loader.cpp
	source/config/config_v1.cpp
	source/config/config_v2.cpp
	source/config/env_file.cpp
	source/config/expander.cpp
	source/config/migrate.cpp
	source/config/node.cpp
	source/config/project_config.cpp
	source/config/rebuild_store.cpp
	source/config/target_resolver.cpp
	source/core/compilation_unit.cpp
	source/core/compilation_unit_manager.cpp
	source/formats/archive.cpp
	source/formats/blz.cpp
	source/formats/elf.cpp
	source/modules/module_config.cpp
	source/modules/module_dump.cpp
	source/modules/module_graph.cpp
	source/modules/module_resolver.cpp
	source/modules/problems.cpp
	source/ndsbin/armbin.cpp
	source/ndsbin/overlaybin.cpp
	source/patch/arenalo_finder.cpp
	source/patch/asm_generator.cpp
	source/patch/dependency_resolver.cpp
	source/patch/filesystem_manager.cpp
	source/patch/library_manager.cpp
	source/patch/linker.cpp
	source/patch/overwrite_region_manager.cpp
	source/patch/patch_maker.cpp
	source/patch/patch_tracker.cpp
	source/patch/types.cpp
	source/rom/backup_store.cpp
	source/rom/dir_accessor.cpp
	source/rom/fat.cpp
	source/rom/file_manifest.cpp
	source/rom/narc.cpp
	source/rom/file_tree.cpp
	source/rom/header.cpp
	source/rom/nds_accessor.cpp
	source/rom/nds_rom.cpp
	source/rom/nitro_fs.cpp
	source/rom/overlay_table.cpp
	source/rom/plan_accessor.cpp
	source/system/ansi.cpp
	source/system/cache.cpp
	source/system/diagnostics.cpp
	source/system/except.cpp
	source/system/exit_code.cpp
	source/system/log.cpp
	source/system/log_sink.cpp
	source/system/message.cpp
	source/system/paths.cpp
	source/system/process.cpp
	source/utils/base32.cpp
	source/utils/endian.cpp
	source/utils/glob.cpp
	source/utils/json.cpp
	source/utils/unicode.cpp
	source/utils/util.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES} $<IF:$<BOOL:${WIN32}>,win_resources.rc,>)
target_link_libraries(${PROJECT_NAME} PRIVATE ${YAML_CPP_TARGET})
target_compile_definitions(${PROJECT_NAME} PRIVATE
	NCP_VERSION="${PROJECT_VERSION}"
	NCP_SDK_VERSION=${NCP_SDK_VERSION}
	NCP_DATA_RELDIR="${NCP_DATA_RELDIR}"
)
if (NOT WIN32)
	find_package(Threads REQUIRED)
	target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)
endif()

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)

# The data files force-included into every cross-compiled translation unit.
# Not host headers, so not CMAKE_INSTALL_INCLUDEDIR: installing ncp.h into
# /usr/include would put it in the host compiler's search path, where it has no
# business being.
set(SDK_FILES
	"${PROJECT_SOURCE_DIR}/sdk/ncp.h"
	"${PROJECT_SOURCE_DIR}/sdk/ncp_ide.h"
	"${PROJECT_SOURCE_DIR}/sdk/ncprt.c"
)

# Copied next to the binary as well, so that a build tree can be run from
# without installing: that is what makes exeDir() the last entry of the lookup.
foreach(_file ${SDK_FILES})
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_file}" $<TARGET_FILE_DIR:${PROJECT_NAME}>
	)
endforeach()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "${NCP_INSTALL_BINDIR}")
install(FILES ${SDK_FILES} DESTINATION "${NCP_INSTALL_DATADIR}")
install(FILES
	"${PROJECT_SOURCE_DIR}/schema/ncpatcher.schema.json"
	"${PROJECT_SOURCE_DIR}/schema/module.schema.json"
	"${PROJECT_SOURCE_DIR}/schema/regions.schema.json"
	DESTINATION "${NCP_INSTALL_DATADIR}")

if (NOT NCP_PORTABLE_LAYOUT)
	install(FILES "${PROJECT_SOURCE_DIR}/docs/ncpatcher.1"
		DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
	install(FILES "${PROJECT_SOURCE_DIR}/completions/ncpatcher.bash"
		DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/bash-completion/completions"
		RENAME "${PROJECT_NAME}")
endif()

install(FILES "${PROJECT_SOURCE_DIR}/LICENSE.txt" "${PROJECT_SOURCE_DIR}/README.md"
	DESTINATION "${NCP_INSTALL_DATADIR}")

# Packaging =================================================================

set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VENDOR "TheGameratorT")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Nintendo DS ROM code patcher")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/TheGameratorT/NCPatcher")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE.txt")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME}")
set(CPACK_STRIP_FILES ON)

# One directory per archive, named after the archive, which is the shape the
# release zips have always had.
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "TheGameratorT")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0")
set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")

if (WIN32)
	set(CPACK_GENERATOR "ZIP")
else()
	set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)

# Tests =====================================================================

if (NCP_BUILD_TESTS)
	enable_testing()

	function(ncp_add_test name)
		add_executable(${name} ${ARGN})
		set_property(TARGET ${name} PROPERTY CXX_STANDARD 20)
		set_property(TARGET ${name} PROPERTY CXX_STANDARD_REQUIRED ON)

		# Every test, whether or not it parses anything: several of them pull in
		# a header that includes yaml-cpp's, and the link is what carries its
		# include directory. A system package hides the omission by sitting in
		# /usr/include, so it only surfaces where yaml-cpp was fetched, which
		# is every machine without the package, and every cross-build.
		target_link_libraries(${name} PRIVATE ${YAML_CPP_TARGET})

		add_test(NAME ${name} COMMAND ${name})
	endfunction()

	ncp_add_test(glob_test tests/glob_test.cpp source/utils/glob.cpp)
	ncp_add_test(diagnostics_test tests/diagnostics_test.cpp
		source/system/diagnostics.cpp source/system/exit_code.cpp)
	ncp_add_test(json_test tests/json_test.cpp source/utils/json.cpp)
	ncp_add_test(blz_test tests/blz_test.cpp
		source/formats/blz.cpp source/utils/endian.cpp)
	ncp_add_test(ansi_test tests/ansi_test.cpp source/system/ansi.cpp)
	ncp_add_test(cli_test tests/cli_test.cpp source/app/cli.cpp)
	target_link_libraries(cli_test PRIVATE ${YAML_CPP_TARGET})
	ncp_add_test(process_test tests/process_test.cpp
		source/system/process.cpp source/utils/unicode.cpp)
	if (NOT WIN32)
		target_link_libraries(process_test PRIVATE Threads::Threads)
	endif()
	ncp_add_test(project_init_test tests/project_init_test.cpp source/app/project_init.cpp)

	# The ROM tests reach into the container, which reports failures the same
	# way the rest of the program does, so they need the diagnostics and log
	# machinery along with source/rom itself.
	ncp_add_test(rom_test
		tests/rom_test.cpp
		source/formats/blz.cpp
		source/rom/dir_accessor.cpp
		source/utils/endian.cpp
		source/rom/fat.cpp
		source/rom/header.cpp
		source/rom/nds_rom.cpp
		source/rom/nitro_fs.cpp
		source/rom/overlay_table.cpp
		source/rom/plan_accessor.cpp
		source/system/ansi.cpp
		source/system/diagnostics.cpp
		source/system/except.cpp
		source/system/log.cpp
		source/system/log_sink.cpp)

	# The file-tree resolver pulls in config/project_config for FileConfig and
	# the NitroFS path rule, and Glob for the component patterns.
	ncp_add_test(file_tree_test
		tests/file_tree_test.cpp
		source/config/project_config.cpp
		source/rom/file_tree.cpp
		source/system/ansi.cpp
		source/system/except.cpp
		source/system/log.cpp
		source/system/log_sink.cpp
		source/utils/glob.cpp)

	# The archive reader borrows nitro_fs for the name table inside a NARC, so
	# it needs the same endian and diagnostics support that parser does.
	ncp_add_test(narc_test
		tests/narc_test.cpp
		source/utils/endian.cpp
		source/rom/narc.cpp
		source/rom/nitro_fs.cpp
		source/system/ansi.cpp
		source/system/diagnostics.cpp
		source/system/except.cpp
		source/system/log.cpp
		source/system/log_sink.cpp)

	# The manifest folds the accessor's file list together with the insertion
	# list, so it needs project_config for FileConfig and the Json writer.
	ncp_add_test(file_manifest_test
		tests/file_manifest_test.cpp
		source/config/project_config.cpp
		source/utils/endian.cpp
		source/rom/file_manifest.cpp
		source/utils/unicode.cpp
		source/rom/header.cpp
		source/system/ansi.cpp
		source/system/diagnostics.cpp
		source/system/except.cpp
		source/system/log.cpp
		source/system/log_sink.cpp
		source/utils/json.cpp)

	ncp_add_test(overlay_create_test
		tests/overlay_create_test.cpp
		source/app/context.cpp
		source/config/buildtarget.cpp
		source/formats/blz.cpp
		source/ndsbin/armbin.cpp
		source/ndsbin/overlaybin.cpp
		source/patch/filesystem_manager.cpp
		source/rom/backup_store.cpp
		source/utils/endian.cpp
		source/rom/header.cpp
		source/rom/overlay_table.cpp
		source/system/ansi.cpp
		source/system/diagnostics.cpp
		source/system/except.cpp
		source/system/log.cpp
		source/system/log_sink.cpp
		source/system/message.cpp
		source/utils/json.cpp
		source/utils/util.cpp)
	target_link_libraries(overlay_create_test PRIVATE ${YAML_CPP_TARGET})

	ncp_add_test(arenalo_finder_test
		tests/arenalo_finder_test.cpp
		source/formats/blz.cpp
		source/ndsbin/armbin.cpp
		source/patch/arenalo_finder.cpp
		source/utils/endian.cpp
		source/system/ansi.cpp
		source/system/diagnostics.cpp
		source/system/except.cpp
		source/system/log.cpp
		source/system/log_sink.cpp
		source/utils/util.cpp)

	ncp_add_test(gcc_diagnostics_test
		tests/gcc_diagnostics_test.cpp
		source/build/gcc_diagnostics.cpp)
	target_link_libraries(gcc_diagnostics_test PRIVATE ${YAML_CPP_TARGET})

	# The config test exercises the whole reader stack (both schemas, the
	# expander, inheritance and migration) so it needs the same sources the
	# program does, minus everything that would drag in the build and patch
	# phases.
	ncp_add_test(config_test
		tests/config_test.cpp
		source/app/config_dump.cpp
		source/config/node.cpp
		source/config/project_config.cpp
		source/config/env_file.cpp
		source/config/expander.cpp
		source/config/config_loader.cpp
		source/config/config_v1.cpp
		source/config/config_v2.cpp
		source/utils/unicode.cpp
		source/config/target_resolver.cpp
		source/config/buildtarget.cpp
		source/config/migrate.cpp
		source/config/rebuild_store.cpp
		source/modules/module_config.cpp
		source/modules/module_dump.cpp
		source/modules/module_graph.cpp
		source/modules/module_resolver.cpp
		source/modules/problems.cpp
		source/utils/glob.cpp
		source/utils/json.cpp
		source/utils/util.cpp
		source/system/log.cpp
		source/system/log_sink.cpp
		source/system/ansi.cpp
		source/system/except.cpp
	)
	target_link_libraries(config_test PRIVATE ${YAML_CPP_TARGET})
	if (NOT WIN32)
		find_package(Threads REQUIRED)
		target_link_libraries(config_test PRIVATE Threads::Threads)
	endif()

	# The module tests go through the project reader and the target resolver as
	# well, because half of what they check is how a module lands in a region.
	ncp_add_test(modules_test
		tests/modules_test.cpp
		source/config/node.cpp
		source/config/project_config.cpp
		source/config/env_file.cpp
		source/config/expander.cpp
		source/config/config_loader.cpp
		source/config/config_v1.cpp
		source/config/config_v2.cpp
		source/utils/unicode.cpp
		source/config/target_resolver.cpp
		source/config/buildtarget.cpp
		source/config/migrate.cpp
		source/config/rebuild_store.cpp
		source/modules/module_config.cpp
		source/modules/module_dump.cpp
		source/modules/module_graph.cpp
		source/modules/module_resolver.cpp
		source/modules/problems.cpp
		source/utils/glob.cpp
		source/utils/json.cpp
		source/utils/util.cpp
		source/system/log.cpp
		source/system/log_sink.cpp
		source/system/ansi.cpp
		source/system/except.cpp
	)
	target_link_libraries(modules_test PRIVATE ${YAML_CPP_TARGET})
	if (NOT WIN32)
		target_link_libraries(modules_test PRIVATE Threads::Threads)
	endif()

	# Runs the installed binary from an unrelated directory: what it proves is
	# that ncp.h is found through the install layout rather than because the
	# build tree happened to be next door.
	add_test(NAME install_layout
		COMMAND "${CMAKE_COMMAND}"
			-D "NCP_EXE=$<TARGET_FILE:${PROJECT_NAME}>"
			-D "NCP_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
			-D "NCP_BINARY_DIR=${PROJECT_BINARY_DIR}"
			-D "NCP_BUILD_TYPE=$<CONFIG>"
			-P "${PROJECT_SOURCE_DIR}/tests/install_layout.cmake")
endif()
