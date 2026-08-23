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

# Get RapidJson
include("${CMAKE_SOURCE_DIR}/vendor/rapidjson.cmake")
include_directories(${RAPIDJSON_INCLUDE_DIR})
# BS::thread_pool is vendored in-tree; see vendor/thread-pool/README.md
include_directories("${CMAKE_SOURCE_DIR}/vendor/thread-pool/include")

# Find the source files
# CONFIGURE_DEPENDS re-runs the glob when the source tree changes; without it a
# newly added .cpp is silently left out of the build until CMake is re-run.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "source/*.cpp")

# Add the executable
add_executable(${PROJECT_NAME} ${SOURCES} $<IF:$<BOOL:${WIN32}>,win_resources.rc,>)
add_dependencies(${PROJECT_NAME} rapidjson)
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

	add_executable(diagnostics_test tests/diagnostics_test.cpp source/system/diagnostics.cpp)
	set_property(TARGET diagnostics_test PROPERTY CXX_STANDARD 20)
	set_property(TARGET diagnostics_test PROPERTY CXX_STANDARD_REQUIRED ON)
	add_test(NAME diagnostics COMMAND diagnostics_test)

	add_executable(ansi_test tests/ansi_test.cpp source/system/ansi.cpp)
	set_property(TARGET ansi_test PROPERTY CXX_STANDARD 20)
	set_property(TARGET ansi_test PROPERTY CXX_STANDARD_REQUIRED ON)
	add_test(NAME ansi COMMAND ansi_test)
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
