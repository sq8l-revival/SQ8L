# The version of this build: the number from project(), the channel from git.
#
#   cmake -DSQ8L_NUMBER=1.0.0 -DSQ8L_SRC=<repo> [-DSQ8L_IN=<in> -DSQ8L_OUT=<header>] -P cmake/Version.cmake
#
# Prints SQ8L_VERSION=<string>, writes the header when IN/OUT are given. Run from a build rule,
# not at configure time, so the commit is the one being built.

find_package(Git QUIET)
if(NOT GIT_EXECUTABLE)
    set(GIT_EXECUTABLE git)
endif()

function(sq8l_git out_var)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${SQ8L_SRC} ${ARGN}
                    RESULT_VARIABLE code OUTPUT_VARIABLE text ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(${out_var} "${text}" PARENT_SCOPE)
    set(${out_var}_failed "${code}" PARENT_SCOPE)
endfunction()

set(SQ8L_VERSION "${SQ8L_NUMBER}")
sq8l_git(commit rev-parse --short HEAD)
if(NOT commit_failed)
    # Tracked files only, so untracked scratch does not mark a build.
    sq8l_git(ignored diff --quiet HEAD --)
    # --match v*: the rolling `nightly` tag would otherwise answer --exact-match.
    sq8l_git(tag describe --tags --exact-match --match "v*")
    if(ignored_failed)
        set(SQ8L_VERSION "${SQ8L_NUMBER} (${commit}) LOCAL")
    elseif(NOT tag_failed)
        set(SQ8L_VERSION "${SQ8L_NUMBER}")
    else()
        set(SQ8L_VERSION "${SQ8L_NUMBER} (${commit})")
    endif()
endif()

message(STATUS "SQ8L_VERSION=${SQ8L_VERSION}")

if(SQ8L_IN AND SQ8L_OUT)
    string(REPLACE "." ";" parts "${SQ8L_NUMBER}")
    list(GET parts 0 SQ8L_VERSION_MAJOR)
    list(GET parts 1 SQ8L_VERSION_MINOR)
    list(GET parts 2 SQ8L_VERSION_PATCH)
    configure_file("${SQ8L_IN}" "${SQ8L_OUT}" @ONLY)
endif()
