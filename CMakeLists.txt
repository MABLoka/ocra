cmake_minimum_required(VERSION 3.16)
project(awar C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)
pkg_get_variable(WL_PROTOCOLS_DIR wayland-protocols pkgdatadir)
pkg_get_variable(WL_SCANNER wayland-scanner wayland_scanner)
pkg_check_modules(WAYLAND_CURSOR REQUIRED IMPORTED_TARGET wayland-cursor)

# Generate client header + glue code for a protocol XML
function(wl_protocol xml name)
  add_custom_command(
        OUTPUT ${name}-client-protocol.h
        COMMAND ${WL_SCANNER} client-header ${xml} ${name}-client-protocol.h
        DEPENDS ${xml})
  add_custom_command(
        OUTPUT ${name}-protocol.c
        COMMAND ${WL_SCANNER} private-code ${xml} ${name}-protocol.c
        DEPENDS ${xml})
  set(PROTO_SRCS ${PROTO_SRCS}
        ${CMAKE_CURRENT_BINARY_DIR}/${name}-client-protocol.h
        ${CMAKE_CURRENT_BINARY_DIR}/${name}-protocol.c PARENT_SCOPE)
endfunction()

wl_protocol(${WL_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml xdg-shell)
wl_protocol(${CMAKE_SOURCE_DIR}/protocol/wlr-layer-shell-unstable-v1.xml
            wlr-layer-shell-unstable-v1)

add_executable(awar main.c ${PROTO_SRCS})
target_include_directories(awar PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(awar PRIVATE PkgConfig::WAYLAND PkgConfig::PANGOCAIRO)
target_link_libraries(awar PRIVATE
    PkgConfig::WAYLAND PkgConfig::WAYLAND_CURSOR PkgConfig::PANGOCAIRO)
