cmake_minimum_required(VERSION 3.20)
project(Frame60AccumMod LANGUAGES C)

set(MODERNGEKKO_SOURCE_DIR "" CACHE PATH "ModernGekko source directory")
if(NOT IS_DIRECTORY "${MODERNGEKKO_SOURCE_DIR}/include/moderngekko")
    message(FATAL_ERROR "Set MODERNGEKKO_SOURCE_DIR to the ModernGekko checkout")
endif()

add_library(mod SHARED mod.c)
target_include_directories(mod PRIVATE "${MODERNGEKKO_SOURCE_DIR}/include")
set_target_properties(mod PROPERTIES PREFIX "" OUTPUT_NAME "mod"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/frame60-accum.mgm"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/frame60-accum.mgm")
# Refresh the packaged copy next to the runner on every mod rebuild; the
# moderngekko-run POST_BUILD copy only fires when the runner relinks.
if(TARGET moderngekko-run)
    add_custom_command(TARGET mod POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/frame60-accum.mgm"
                "$<TARGET_FILE_DIR:moderngekko-run>/Mods/frame60-accum.mgm")
endif()
