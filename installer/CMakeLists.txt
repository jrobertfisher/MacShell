# MacShell Installer: a 68k application carrying the (fat) MacShell.
#   cmake -S installer -B build-installer -DCMAKE_TOOLCHAIN_FILE=<68k toolchain> \
#         -DMACSHELL_BIN=build-fat/MacShell.bin
cmake_minimum_required(VERSION 3.9)
project(MacShellInstaller C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(MACSHELL_BIN ${ROOT}/build-fat/MacShell.bin CACHE FILEPATH "MacShell (MacBinary) to install")
set(MBED ${ROOT}/opentransport-mbedtls)

add_compile_definitions("MBEDTLS_USER_CONFIG_FILE=\"${ROOT}/macshell_mbedtls_config.h\"")
include_directories(${ROOT} ${MBED}/include ${MBED}/library)

# the forks of MacShell and the licences (Mac line endings) for Rez
add_custom_command(
    OUTPUT MacShell.data MacShell.rsrc licenses.txt
    COMMAND python3 ${ROOT}/tools/extract_fork.py ${MACSHELL_BIN} MacShell
    COMMAND python3 ${ROOT}/tools/to_mac_text.py ${ROOT}/docs/LICENSES.txt licenses.txt
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${MACSHELL_BIN} ${ROOT}/docs/LICENSES.txt ${ROOT}/tools/extract_fork.py)
add_custom_command(
    OUTPUT installer_res.rsrc.bin
    COMMAND ${REZ} ${CMAKE_CURRENT_SOURCE_DIR}/installer.r -I${REZ_INCLUDE_PATH} -I${ROOT}
            -o installer_res.rsrc.bin
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/installer.r ${ROOT}/icons.r ${ROOT}/constants.r
            MacShell.data MacShell.rsrc licenses.txt)
add_custom_target(installer_res DEPENDS installer_res.rsrc.bin)

add_application(MacShellInstaller
    installer.c
    ${ROOT}/macentropy.c
    ${MBED}/library/sha256.c
    ${MBED}/library/platform_util.c
    ${CMAKE_CURRENT_BINARY_DIR}/installer_res.rsrc.bin
    CREATOR "MShi")
set_target_properties(MacShellInstaller PROPERTIES
    COMPILE_OPTIONS "-ffunction-sections;-Os"
    LINK_FLAGS "-Wl,-gc-sections")
add_dependencies(MacShellInstaller_APPL installer_res)

add_custom_target(MacShellInstaller_finish ALL
    COMMAND python3 ${ROOT}/tools/scrub_build_paths.py MacShellInstaller.bin
    COMMAND python3 ${ROOT}/tools/set_bundle_bit.py MacShellInstaller.bin
    COMMAND python3 ${ROOT}/tools/rename_macbinary.py MacShellInstaller.bin "MacShell Installer"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(MacShellInstaller_finish MacShellInstaller_APPL)
