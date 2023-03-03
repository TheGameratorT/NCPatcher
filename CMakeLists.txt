cmake_minimum_required(VERSION 3.20)

# Set the project name and version
project(ncpatcher VERSION 1.0)

# Specify the C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED true)

# Get RapidJson
include("${CMAKE_SOURCE_DIR}/vendor/rapidjson.cmake")
include_directories(${RAPIDJSON_INCLUDE_DIR})
# Get thread-pool
include("${CMAKE_SOURCE_DIR}/vendor/thread-pool.cmake")
include_directories(${THREADPOOL_INCLUDE_DIR})

#add pthread to the libraries because that's required apparently
add_link_options(-pthread)

# Find the source files
file(GLOB_RECURSE SOURCES "source/*.cpp")

# Add the executable
add_executable(${PROJECT_NAME} ${SOURCES} $<IF:$<BOOL:${WIN32}>,win_resources.rc,>)
add_dependencies(${PROJECT_NAME} rapidjson thread-pool)

# Copy ncp.h and ncp_h.s to the executable output directory
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/ncp.h" ${PROJECT_BINARY_DIR}
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/ncp_asm.h" ${PROJECT_BINARY_DIR}
)
