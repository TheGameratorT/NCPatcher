cmake_minimum_required(VERSION 3.20)

# Set the project name and version
project(ncpatcher VERSION 1.0.4)

if (WIN32)
add_compile_definitions(_CRT_SECURE_NO_DEPRECATE)
endif()

# Get RapidJson
include("${CMAKE_SOURCE_DIR}/vendor/rapidjson.cmake")
include_directories(${RAPIDJSON_INCLUDE_DIR})
# Get thread-pool
include("${CMAKE_SOURCE_DIR}/vendor/thread-pool.cmake")
include_directories(${THREADPOOL_INCLUDE_DIR})

# Find the source files
file(GLOB_RECURSE SOURCES "source/*.cpp")

# Add the executable
add_executable(${PROJECT_NAME} ${SOURCES} $<IF:$<BOOL:${WIN32}>,win_resources.rc,>)
add_dependencies(${PROJECT_NAME} rapidjson thread-pool)
if (NOT WIN32)
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)
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
