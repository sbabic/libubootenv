# SPDX-FileCopyrightText: 2019-2021 Stefano Babic <sbabic@denx.de>
#
# SPDX-License-Identifier:     LGPL-2.1-or-later
cmake_minimum_required (VERSION 2.6)
# Sources and private headers
SET(libubootenv_SOURCES
  uboot_env.c
  uboot_private.h
)

# Public headers
SET(include_HEADERS
  libuboot.h
)

include(GNUInstallDirs) # for the CMAKE_INSTALL_LIBDIR variable

add_library(ubootenv SHARED ${libubootenv_SOURCES} ${include_HEADERS})
SET_TARGET_PROPERTIES(ubootenv PROPERTIES VERSION ${VERSION} SOVERSION ${SOVERSION})

ADD_LIBRARY(ubootenv_static STATIC ${libubootenv_SOURCES} ${include_HEADERS})
SET_TARGET_PROPERTIES(ubootenv_static PROPERTIES OUTPUT_NAME ubootenv)
add_executable(fw_printenv fw_printenv.c)
target_link_libraries(ubootenv z yaml)
target_link_libraries(fw_printenv ubootenv)
add_custom_target(fw_setenv ALL ${CMAKE_COMMAND} -E create_symlink fw_printenv fw_setenv)

install (TARGETS ubootenv ubootenv_static DESTINATION ${CMAKE_INSTALL_LIBDIR})
install (FILES libuboot.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install (TARGETS fw_printenv DESTINATION ${CMAKE_INSTALL_BINDIR})
install (PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/fw_setenv DESTINATION ${CMAKE_INSTALL_BINDIR})

# Handle pkg-config files
set(prefix      ${CMAKE_INSTALL_PREFIX})
set(exec_prefix ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR})
set(includedir  ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR})
set(libdir      ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR})

configure_file(${CMAKE_SOURCE_DIR}/src/libubootenv.pc.in ${CMAKE_BINARY_DIR}/libubootenv.pc @ONLY)
set(LIBUBOOTENV_PKG_CONFIG_FILES ${CMAKE_BINARY_DIR}/libubootenv.pc)
message(STATUS "Generate ${LIBUBOOTENV_PKG_CONFIG_FILES}")
# Install pkg-config files
install(FILES ${LIBUBOOTENV_PKG_CONFIG_FILES} DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
