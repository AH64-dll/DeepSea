cmake_minimum_required(VERSION 3.20)
project(Widescreen16x9Mod LANGUAGES C)

set(MODERNGEKKO_SOURCE_DIR "" CACHE PATH "ModernGekko source directory")
if(NOT IS_DIRECTORY "${MODERNGEKKO_SOURCE_DIR}/include/moderngekko")
    message(FATAL_ERROR "Set MODERNGEKKO_SOURCE_DIR to the ModernGekko checkout")
endif()

# Target name must differ from the other mods' `mod`/`cutscene_skip_mod`
# targets when built inside the main tree; the packaged artifact is still
# <id>.mgm/mod.dll.
add_library(widescreen16x9_mod SHARED mod.c)
target_include_directories(widescreen16x9_mod PRIVATE "${MODERNGEKKO_SOURCE_DIR}/include")
set_target_properties(widescreen16x9_mod PROPERTIES PREFIX "" OUTPUT_NAME "mod"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/widescreen16x9.mgm"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/widescreen16x9.mgm")
# Refresh the packaged copy next to the runner on every mod rebuild; the
# moderngekko-run POST_BUILD copy only fires when the runner relinks.
if(TARGET moderngekko-run)
    add_custom_command(TARGET widescreen16x9_mod POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/widescreen16x9.mgm"
                "$<TARGET_FILE_DIR:moderngekko-run>/Mods/widescreen16x9.mgm")
endif()
