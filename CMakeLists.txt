# SPDX-FileCopyrightText: 2019-2021 Stefano Babic stefano.babic@swupdate.org.
#
# SPDX-License-Identifier:     LGPL-2.1-or-later

cmake_minimum_required (VERSION 3.5)
project (libubootenv C)

# The version number.
# set (libubootenv_VERSION_MAJOR 1)
# set (libubootenv_VERSION_MAJOR_VERSION_MINOR 0)
set(VERSION	"0.3.6")
SET(SOVERSION "0")
add_definitions(-DVERSION="${VERSION}")

option(NO_YML_SUPPORT "YML Support")

if(DEFAULT_CFG_FILE)
    add_definitions(-DDEFAULT_CFG_FILE="${DEFAULT_CFG_FILE}")
endif(DEFAULT_CFG_FILE)

if(DEFAULT_ENV_FILE)
    add_definitions(-DDEFAULT_ENV_FILE="${DEFAULT_ENV_FILE}")
endif(DEFAULT_ENV_FILE)

if(NO_YML_SUPPORT)
  add_definitions(-DNO_YAML_SUPPORT)
endif(NO_YML_SUPPORT)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99")

#set(CMAKE_C_FLAGS_DEBUG "-g")
include_directories ("${PROJECT_SOURCE_DIR}/src")
add_subdirectory (src)

# first we can indicate the documentation build as an option and set it to ON by default
option(BUILD_DOC "Build documentation" ON)

if(BUILD_DOC)
    # check if Doxygen is installed
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        # set input and output files
        set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in)
        set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)

        # request to configure the file
        configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)
        message("Doxygen build started")

        # note the option ALL which allows to build the docs together with the application
        add_custom_target( doc_doxygen ALL
            COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM )
    else(DOXYGEN_FOUND)
        message("Doxygen need to be installed to generate the doxygen documentation")
    endif(DOXYGEN_FOUND)
endif(BUILD_DOC)
