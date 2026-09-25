cmake_minimum_required(VERSION 3.20)
project(CutsceneSkipMod LANGUAGES C)

set(MODERNGEKKO_SOURCE_DIR "" CACHE PATH "ModernGekko source directory")
if(NOT IS_DIRECTORY "${MODERNGEKKO_SOURCE_DIR}/include/moderngekko")
    message(FATAL_ERROR "Set MODERNGEKKO_SOURCE_DIR to the ModernGekko checkout")
endif()

# Target name must differ from mods/frame60-accum's `mod` target when both are
# built inside the main tree; the packaged artifact is still <id>.mgm/mod.dll.
add_library(cutscene_skip_mod SHARED mod.c)
target_include_directories(cutscene_skip_mod PRIVATE "${MODERNGEKKO_SOURCE_DIR}/include")
set_target_properties(cutscene_skip_mod PROPERTIES PREFIX "" OUTPUT_NAME "mod"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/cutscene-skip.mgm"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/cutscene-skip.mgm")
# Refresh the packaged copy next to the runner on every mod rebuild; the
# moderngekko-run POST_BUILD copy only fires when the runner relinks.
if(TARGET moderngekko-run)
    add_custom_command(TARGET cutscene_skip_mod POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/cutscene-skip.mgm"
                "$<TARGET_FILE_DIR:moderngekko-run>/Mods/cutscene-skip.mgm")
endif()
