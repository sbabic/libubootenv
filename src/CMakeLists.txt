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
SET_TARGET_PROPERTIES(ubootenv PROPERTIES SOVERSION ${SOVERSION})

ADD_LIBRARY(ubootenv_static STATIC ${libubootenv_SOURCES} ${include_HEADERS})
SET_TARGET_PROPERTIES(ubootenv_static PROPERTIES OUTPUT_NAME ubootenv)
add_executable(fw_printenv fw_printenv.c)
add_executable(fw_setenv fw_setenv.c)
target_link_libraries(fw_printenv ubootenv z)
target_link_libraries(fw_setenv ubootenv z)

install (TARGETS ubootenv ubootenv_static DESTINATION ${CMAKE_INSTALL_LIBDIR})
install (FILES libuboot.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install (TARGETS fw_printenv DESTINATION ${CMAKE_INSTALL_BINDIR})
install (TARGETS fw_setenv DESTINATION ${CMAKE_INSTALL_BINDIR})

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
