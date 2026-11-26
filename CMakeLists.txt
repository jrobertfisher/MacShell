# MacShell
#
# Based on MacShell by LXXero and ssheven by cy384 <cy384@cy384.com>
# See LICENSE file for details

cmake_minimum_required(VERSION 3.9)

project(MacShell)
# Manual pages (man/*.txt) become TEXT resources for the man command
file(GLOB MAN_PAGES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/man/*.txt)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/man.r
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_man.py ${CMAKE_CURRENT_BINARY_DIR}/man.r ${MAN_PAGES}
    DEPENDS ${MAN_PAGES} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_man.py)

add_application(MacShell CREATOR "MShl" app.c console.c resources.r symbolfont.r ${CMAKE_CURRENT_BINARY_DIR}/man.r net.c telnet.c debug.c shell.c macentropy.c sshd.c dns.c dnsmsg.c)

# MacShell's mbedTLS settings (applied on top of mbedTLS's defaults) and the
# Open Transport switch for libssh2. Both libraries are built with 68020 code.
add_compile_definitions("MBEDTLS_USER_CONFIG_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/macshell_mbedtls_config.h\"")
add_compile_definitions(LIBSSH2_OPENTRANSPORT)
if(CMAKE_SYSTEM_NAME MATCHES Retro68)
  add_compile_options(-mcpu=68020 -O2 -ffunction-sections)
elseif(CMAKE_SYSTEM_NAME MATCHES RetroPPC)
  add_compile_options(-mcpu=601 -O2 -ffunction-sections)
endif()

# set up and build mbedtls
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "newer GCC warnings must not break the build" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "disable mbedtls programs" FORCE)
set(UNSAFE_BUILD ON CACHE BOOL "ignore mac os jank for mbedtls" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "disable mbedtls tests" FORCE)
add_subdirectory(opentransport-mbedtls)
# classic Mac OS has no POSIX directories: give mbedTLS a stand-in <dirent.h>
target_include_directories(mbedx509 BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat)

# set up and build libssh2 (against the bundled mbedTLS, without BSD sockets)
set(CRYPTO_BACKEND mbedTLS)
set(MBEDTLS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/opentransport-mbedtls/include" CACHE PATH "" FORCE)
set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)
foreach(v HAVE_O_NONBLOCK HAVE_FIONBIO HAVE_IOCTLSOCKET_CASE HAVE_SO_NONBLOCK)
  set(${v} 0 CACHE INTERNAL "classic Mac OS: no BSD non-blocking sockets")
endforeach()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "disable libssh2 shared libs" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "disable libssh2 examples" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "disable libssh2 tests" FORCE)
add_subdirectory(opentransport-libssh2)

# set up and build libvterm
add_subdirectory(libvterm)

IF(CMAKE_SYSTEM_NAME MATCHES Retro68)
# for 68k
  set_target_properties(MacShell PROPERTIES COMPILE_FLAGS "-ffunction-sections -mcpu=68020 -O3 -Wall -Wextra -Wno-unused-parameter")
  set_target_properties(MacShell PROPERTIES LINK_FLAGS "-Wl,-gc-sections")
  target_link_libraries(MacShell libssh2 mbedtls mbedx509 mbedcrypto OpenTransportApp OpenTransport OpenTptInet vterm retrocrt)
ELSE()
# for PPC
  set_target_properties(MacShell PROPERTIES COMPILE_FLAGS "-ffunction-sections -mcpu=601 -O3 -Wall -Wextra -Wno-unused-parameter")
  set_target_properties(MacShell PROPERTIES LINK_FLAGS "-Wl,-gc-sections")
  target_link_libraries(MacShell ThreadsLib libssh2 mbedtls mbedx509 mbedcrypto OpenTransportAppPPC OpenTransportLib OpenTptInternetLib OpenTptInetPPC vterm retrocrt)
ENDIF()

# Set the Finder's hasBundle flag so MacShell shows its own icon
# (Retro68's Rez doesn't set it).
add_custom_target(MacShell_bundle_flag ALL
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/scrub_build_paths.py MacShell.bin
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/set_bundle_bit.py MacShell.bin
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(MacShell_bundle_flag MacShell_APPL)

# resources.r includes constants.r (the version) and icons.r: rebuild the
# resources when they change. (OBJECT_DEPENDS on the .r file isn't enough:
# Retro68 runs Rez as a custom command, so the dependency goes on its output.)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/resources.r.rsrc.bin APPEND
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/constants.r ${CMAKE_CURRENT_SOURCE_DIR}/icons.r)
