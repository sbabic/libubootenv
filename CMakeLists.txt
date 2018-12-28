cmake_minimum_required (VERSION 2.6)
project (libubootenv)

# The version number.
set (libubootenv_VERSION_MAJOR 1)
set (libubootenv_VERSION_MAJOR_VERSION_MINOR 0)

include_directories ("${PROJECT_SOURCE_DIR}/src")
add_subdirectory (src)

