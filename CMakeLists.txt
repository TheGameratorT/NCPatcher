cmake_minimum_required(VERSION 3.20)

# Set the project name and version
project(ncpatcher VERSION 1.0.4)

if (WIN32)
	add_compile_definitions(_CRT_SECURE_NO_DEPRECATE)
endif()

if (CMAKE_C_COMPILER MATCHES "mingw")
	set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++ -static")

	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")	
endif()

# yaml-cpp reads both configuration schemas: v2 is YAML, and JSON is a subset of
# YAML 1.2, so v1 comes through the same parser. Preferred from the system,
# because a build-time download is unusable offline and unusable for distro
# packaging; the fetch is the fallback for machines that do not have it.
find_package(yaml-cpp QUIET)
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
	FetchContent_MakeAvailable(yaml-cpp)
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

# Find the source files
# CONFIGURE_DEPENDS re-runs the glob when the source tree changes; without it a
# newly added .cpp is silently left out of the build until CMake is re-run.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "source/*.cpp")

# Add the executable
add_executable(${PROJECT_NAME} ${SOURCES} $<IF:$<BOOL:${WIN32}>,win_resources.rc,>)
target_link_libraries(${PROJECT_NAME} PRIVATE ${YAML_CPP_TARGET})
target_compile_definitions(${PROJECT_NAME} PRIVATE NCP_VERSION="${PROJECT_VERSION}")
if (NOT WIN32)
	find_package(Threads REQUIRED)
	target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)
endif()

# Tests
option(NCP_BUILD_TESTS "Build unit tests" OFF)
if (NCP_BUILD_TESTS)
	enable_testing()
	add_executable(glob_test tests/glob_test.cpp source/utils/glob.cpp)
	set_property(TARGET glob_test PROPERTY CXX_STANDARD 20)
	set_property(TARGET glob_test PROPERTY CXX_STANDARD_REQUIRED ON)
	add_test(NAME glob COMMAND glob_test)

	add_executable(diagnostics_test tests/diagnostics_test.cpp
		source/system/diagnostics.cpp source/system/exit_code.cpp)
	set_property(TARGET diagnostics_test PROPERTY CXX_STANDARD 20)
	set_property(TARGET diagnostics_test PROPERTY CXX_STANDARD_REQUIRED ON)
	add_test(NAME diagnostics COMMAND diagnostics_test)

	add_executable(json_test tests/json_test.cpp source/utils/json.cpp)
	set_property(TARGET json_test PROPERTY CXX_STANDARD 20)
	set_property(TARGET json_test PROPERTY CXX_STANDARD_REQUIRED ON)
	add_test(NAME json COMMAND json_test)

	add_executable(ansi_test tests/ansi_test.cpp source/system/ansi.cpp)
	set_property(TARGET ansi_test PROPERTY CXX_STANDARD 20)
	set_property(TARGET ansi_test PROPERTY CXX_STANDARD_REQUIRED ON)
	add_test(NAME ansi COMMAND ansi_test)

	# The config test exercises the whole reader stack -- both schemas, the
	# expander, inheritance and migration -- so it needs the same sources the
	# program does, minus everything that would drag in the build and patch
	# phases.
	add_executable(config_test
		tests/config_test.cpp
		source/config/node.cpp
		source/config/project_config.cpp
		source/config/expander.cpp
		source/config/config_loader.cpp
		source/config/config_v1.cpp
		source/config/config_v2.cpp
		source/config/target_resolver.cpp
		source/config/buildtarget.cpp
		source/config/migrate.cpp
		source/config/rebuild_store.cpp
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
	set_property(TARGET config_test PROPERTY CXX_STANDARD 20)
	set_property(TARGET config_test PROPERTY CXX_STANDARD_REQUIRED ON)
	add_test(NAME config COMMAND config_test)
endif()

# Specify the C++ standard
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)

# Copy headers to the executable output directory
set(DEPLOY_HEADERS
	"ncp.h"
	"ncp_ide.h"
	"ncprt.c"
)

if (MAKE_PACK)

set(PACK_DIR "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}_pack")

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E echo "Making pack..."
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory ${PACK_DIR}
)

foreach(_file ${DEPLOY_HEADERS})
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/runtime/${_file}" ${PACK_DIR}
)
endforeach()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E rename $<TARGET_FILE:${PROJECT_NAME}> "${PACK_DIR}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
)

else()

foreach(_file ${DEPLOY_HEADERS})
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/runtime/${_file}" $<TARGET_FILE_DIR:${PROJECT_NAME}>
)
endforeach()

endif()
