cmake_minimum_required (VERSION 2.6)
# Sources and private headers
set(libubootenv_SOURCES
  uboot_env.c
  uboot_private.h
)

# Public headers
set(include_HEADERS
  libuboot.h
)

add_library(ubootenv SHARED ${libubootenv_SOURCES} ${include_HEADERS})
set_target_properties(ubootenv PROPERTIES SOVERSION ${SOVERSION})

add_library(ubootenv_static STATIC ${libubootenv_SOURCES} ${include_HEADERS})
add_executable(fw_printenv fw_printenv.c)
add_executable(fw_setenv fw_setenv.c)
target_link_libraries(fw_printenv ubootenv z)
target_link_libraries(fw_setenv ubootenv z)

install (TARGETS ubootenv DESTINATION lib)
install (FILES libuboot.h DESTINATION include)
install (TARGETS fw_printenv DESTINATION bin)
install (TARGETS fw_setenv DESTINATION bin)
