#==============================================================================
# @file    CMakeLists.txt
# @brief   Build script for the ESP32-S3 Swiss Army Server (SAS) firmware.
#
# Built with GNU Make, not Ninja. The first build selects it, later builds
# reuse it:  idf.py -G "Unix Makefiles" build   (tools/run.sh does this)
#
#   cmake --build build --target check    cppcheck, fails on any finding
#   cmake --build build --target format   clang-format + Prettier (runs before builds)
#   cmake --build build --target docs     Doxygen HTML in build/docs/html
#==============================================================================

cmake_minimum_required(VERSION 3.16)

# A CMakeLists.txt cannot pick the generator (CMake fixes it before reading this
# file), so a build directory configured for Ninja is refused instead. idf.py
# defaults to Ninja whenever it is installed.
if(CMAKE_GENERATOR MATCHES "Ninja")
    message(FATAL_ERROR
        "This project is built with GNU Make, not Ninja.\n"
        "Delete the build directory and configure it again:\n"
        "  rm -rf build\n"
        "  idf.py -G \"Unix Makefiles\" build")
endif()

# Project version lives in version.txt as a Semantic Version (semver.org):
# MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]. Re-configure when it changes.
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/version.txt FW_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS version.txt)

set(SEMVER_REGEX
    "^((0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*))(-[0-9A-Za-z.-]+)?(\\+[0-9A-Za-z.-]+)?$")
if(NOT FW_VERSION MATCHES "${SEMVER_REGEX}")
    message(FATAL_ERROR "version.txt: '${FW_VERSION}' is not a semantic "
                        "version (MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD])")
endif()

# ESP-IDF embeds PROJECT_VER in the application image (esp_app_desc_t).
set(PROJECT_VER ${FW_VERSION})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(SAS)

set(PROJECT_DESCRIPTION "ESP32-S3 Swiss Army Server")
set(SRCDIR      ${CMAKE_SOURCE_DIR}/main/src)
set(TEMPLATEDIR ${CMAKE_SOURCE_DIR}/templates)

# The application sources are the "main" component; see main/CMakeLists.txt.
idf_component_get_property(MAIN_LIB main COMPONENT_LIB)

#------------------------------------------------------------------------------
# Flags
#------------------------------------------------------------------------------
set(WARNINGS
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion
    -Wstrict-prototypes -Wmissing-prototypes -Wundef -pedantic
)

# Compile the blank module templates too, so they always meet the standard.
add_library(template_check OBJECT ${TEMPLATEDIR}/template.c)
set_target_properties(template_check PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(template_check PRIVATE ${WARNINGS})

#------------------------------------------------------------------------------
# Tools
#------------------------------------------------------------------------------
# A target whose tool is missing still exists, but fails with a clear message.
function(add_missing_tool_target name tool hint)
    add_custom_target(${name}
        COMMAND ${CMAKE_COMMAND} -E echo "error: ${tool} not found (e.g. '${hint}')"
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
    message(WARNING "${tool} not found - the '${name}' target will fail")
endfunction()

# unusedFunction is suppressed: callbacks and public APIs have no caller here.
# missingInclude is suppressed: ESP-IDF headers are not on cppcheck's path.
file(GLOB SRC_SUBDIRS LIST_DIRECTORIES true ${SRCDIR}/*)
set(CPPCHECK_INCLUDES -I${SRCDIR} -I${TEMPLATEDIR})
foreach(dir ${SRC_SUBDIRS})
    if(IS_DIRECTORY ${dir})
        list(APPEND CPPCHECK_INCLUDES -I${dir})
    endif()
endforeach()

find_program(CPPCHECK cppcheck)
if(CPPCHECK)
    add_custom_target(check
        COMMAND ${CPPCHECK} --enable=all --std=c99 --inline-suppr --error-exitcode=1
                --suppress=missingIncludeSystem --suppress=missingInclude
                --suppress=unusedFunction --suppress=checkersReport
                ${CPPCHECK_INCLUDES} ${SRCDIR} ${TEMPLATEDIR}
        VERBATIM
    )
else()
    add_missing_tool_target(check cppcheck "sudo apt install cppcheck")
endif()

add_custom_target(format
    COMMAND ${CMAKE_SOURCE_DIR}/tools/format.sh
    VERBATIM
)

# Format the sources before every build. Files that are already formatted are
# left untouched, so this does not trigger needless recompiles.
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    add_dependencies(${MAIN_LIB}    format)
    add_dependencies(template_check format)
else()
    message(WARNING "clang-format not found - sources will not be formatted "
                    "during the build")
endif()

configure_file(tools/config/Doxyfile.in ${CMAKE_BINARY_DIR}/Doxyfile @ONLY)

find_program(DOXYGEN doxygen)
if(DOXYGEN)
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/docs
        COMMAND ${DOXYGEN} ${CMAKE_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM
    )
else()
    add_missing_tool_target(docs doxygen "sudo apt install doxygen")
endif()
