# =============================================================================
# SolverDependencies.cmake
# =============================================================================
#
# Application-side discovery of the sensor fusion solver (GTSAM and oneTBB,
# built by cmake/SolverSuperbuild.cmake) and the helpers that go with it.
#
# Usage (src/CMakeLists.txt, after GeographicLib has been found):
#   include("${CMAKE_CURRENT_LIST_DIR}/../cmake/SolverDependencies.cmake")
#
# Inputs (CACHE PATH variables defined in src/CMakeLists.txt):
#   GTSAM_ROOT   - GTSAM install prefix
#   ONETBB_ROOT  - oneTBB install prefix
#
# This module:
#   1. Refuses a GTSAM install that was built with Boost
#   2. Finds GTSAM through its package config and requires the exported
#      target "gtsam". Consumers link that target and nothing else: it carries
#      GTSAM's ABI definitions and its bundled Eigen headers, which must reach
#      every translation unit that uses GTSAM types. Never add another Eigen
#      to the include path of such a target.
#   3. Derives the runtime libraries to deploy from the target's link
#      interface (FLYSIGHT_SOLVER_RUNTIME_TARGETS), never from file globs:
#      an install prefix may contain unrelated libraries.
#   4. Defines the helper functions
#        flysight_solver_stack(<target>)
#        flysight_solver_test_environment(<test>)
#        flysight_install_solver_runtime(<destination>)
#        flysight_assert_solver_confinement()
#
# Nothing here links GTSAM to anything. Which targets link it is decided where
# they are defined; flysight_assert_solver_confinement() checks the decision.
#
# =============================================================================

include_guard(GLOBAL)

# Directory of this file, for helpers that are called from other directories.
set(_FLYSIGHT_SOLVER_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# =============================================================================
# Locate the package config
# =============================================================================
#
# GTSAM installs its config to <prefix>/CMake on Windows and to
# <prefix>/lib/cmake/GTSAM elsewhere. Checking for it first turns CMake's
# generic "could not find package" into a message that says what to do.
#

set(_gtsam_config "")
foreach(_candidate
        "${GTSAM_ROOT}/CMake/GTSAMConfig.cmake"
        "${GTSAM_ROOT}/lib/cmake/GTSAM/GTSAMConfig.cmake")
    if(EXISTS "${_candidate}")
        set(_gtsam_config "${_candidate}")
        break()
    endif()
endforeach()

if(NOT _gtsam_config)
    message(FATAL_ERROR
        "GTSAM was not found: there is no GTSAMConfig.cmake under GTSAM_ROOT\n"
        "  GTSAM_ROOT = ${GTSAM_ROOT}\n"
        "Either build the solver dependencies with the superbuild (configure the "
        "repository root with FLYSIGHT_BUILD_THIRD_PARTY=ON and "
        "FLYSIGHT_BUILD_SOLVER_DEPS=ON, the defaults), or, if they are already "
        "installed somewhere else, configure with FLYSIGHT_BUILD_SOLVER_DEPS=OFF and "
        "point GTSAM_INSTALL_DIR / ONETBB_INSTALL_DIR (root project) or GTSAM_ROOT / "
        "ONETBB_ROOT (src/ configured directly) at the install prefixes.\n"
        "See \"Solver dependencies (GTSAM, oneTBB)\" in README.md.")
endif()

# =============================================================================
# Boost-free guard
# =============================================================================
#
# FlySight ships GTSAM built with GTSAM_ENABLE_BOOST_SERIALIZATION=OFF and
# GTSAM_USE_BOOST_FEATURES=OFF. config.h always defines both macros (as 0 or
# 1), so it says which build this is before any package config runs. A
# Boost-enabled install would otherwise be picked up silently, look for Boost
# at configure time, and need Boost libraries deployed at run time.
#

set(_gtsam_config_h "${GTSAM_ROOT}/include/gtsam/config.h")
if(NOT EXISTS "${_gtsam_config_h}")
    message(FATAL_ERROR
        "GTSAM install is incomplete: ${_gtsam_config_h} does not exist.\n"
        "  GTSAM_ROOT = ${GTSAM_ROOT}")
endif()

file(STRINGS "${_gtsam_config_h}" _gtsam_boost_macros
    REGEX "define GTSAM_(ENABLE_BOOST_SERIALIZATION|USE_BOOST_FEATURES) ")
foreach(_line IN LISTS _gtsam_boost_macros)
    if(_line MATCHES "define (GTSAM_[A-Z_]+) +1")
        message(FATAL_ERROR
            "The GTSAM install at GTSAM_ROOT was built with Boost (${CMAKE_MATCH_1} is 1 in "
            "include/gtsam/config.h).\n"
            "  GTSAM_ROOT = ${GTSAM_ROOT}\n"
            "FlySight requires the Boost-free GTSAM build made by the superbuild "
            "(cmake/SolverSuperbuild.cmake: GTSAM_ENABLE_BOOST_SERIALIZATION=OFF, "
            "GTSAM_USE_BOOST_FEATURES=OFF).\n"
            "Point the build at that install: -DGTSAM_INSTALL_DIR=<prefix> "
            "-DONETBB_INSTALL_DIR=<prefix> when configuring the repository root, or "
            "-DGTSAM_ROOT=<prefix> -DONETBB_ROOT=<prefix> when configuring src/ directly.\n"
            "See \"Solver dependencies (GTSAM, oneTBB)\" in README.md.")
    endif()
endforeach()

# =============================================================================
# Find GTSAM
# =============================================================================
#
# GTSAM's config looks up TBB itself (find_dependency), so both prefixes go on
# the search path. This must happen after GeographicLib has been resolved: a
# GTSAM install made with GTSAM_INSTALL_GEOGRAPHICLIB=ON contains a second
# geographiclib-config.cmake.
#

list(PREPEND CMAKE_PREFIX_PATH "${GTSAM_ROOT}" "${ONETBB_ROOT}")

find_package(GTSAM 4.3 CONFIG REQUIRED)

if(NOT TARGET gtsam)
    message(FATAL_ERROR "GTSAM was found (${GTSAM_DIR}) but does not provide the exported target \"gtsam\"")
endif()

get_target_property(_gtsam_interface gtsam INTERFACE_LINK_LIBRARIES)
if(_gtsam_interface MATCHES "Boost::")
    message(FATAL_ERROR
        "The exported gtsam target links Boost (${_gtsam_interface}).\n"
        "FlySight requires the Boost-free GTSAM build made by the superbuild; see "
        "\"Solver dependencies (GTSAM, oneTBB)\" in README.md.")
endif()

message(STATUS "GTSAM found: ${GTSAM_VERSION} (${GTSAM_DIR})")

# =============================================================================
# Runtime targets
# =============================================================================
#
# Walk the link interface of "gtsam" and record every target that is a shared
# library. Interface and static targets (gtsam_eigen3, metis-gtsam-if, ...) are
# walked through but not recorded.
#

function(_flysight_collect_solver_runtime target)
    # Entries of INTERFACE_LINK_LIBRARIES that are not targets (plain library
    # names, linker flags, other generator expressions) have no runtime file
    # we could name, so they are skipped.
    if(NOT TARGET "${target}")
        return()
    endif()

    # An ALIAS target has no properties of its own worth reading; continue
    # with the target it stands for.
    get_target_property(_aliased "${target}" ALIASED_TARGET)
    if(_aliased)
        set(target "${_aliased}")
    endif()

    get_property(_visited GLOBAL PROPERTY FLYSIGHT_SOLVER_VISITED)
    if("${target}" IN_LIST _visited)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY FLYSIGHT_SOLVER_VISITED "${target}")

    get_target_property(_type "${target}" TYPE)
    if(_type STREQUAL "SHARED_LIBRARY")
        set_property(GLOBAL APPEND PROPERTY FLYSIGHT_SOLVER_RUNTIME "${target}")
    elseif(_type STREQUAL "UNKNOWN_LIBRARY")
        # Find modules export UNKNOWN_LIBRARY targets that may still be shared
        # libraries. Not expected with the pinned libraries (every runtime
        # target is a SHARED_LIBRARY); kept so that such a dependency is
        # deployed rather than silently missed.
        get_target_property(_configs "${target}" IMPORTED_CONFIGURATIONS)
        set(_location_properties IMPORTED_LOCATION)
        if(_configs)
            foreach(_config IN LISTS _configs)
                string(TOUPPER "${_config}" _config)
                list(APPEND _location_properties "IMPORTED_LOCATION_${_config}")
            endforeach()
        endif()
        foreach(_property IN LISTS _location_properties)
            get_target_property(_location "${target}" "${_property}")
            if(_location MATCHES "\\.(so(\\.[0-9.]+)?|dylib|dll)$")
                set_property(GLOBAL APPEND PROPERTY FLYSIGHT_SOLVER_RUNTIME "${target}")
                break()
            endif()
        endforeach()
    endif()

    get_target_property(_links "${target}" INTERFACE_LINK_LIBRARIES)
    if(NOT _links)
        return()
    endif()
    foreach(_link IN LISTS _links)
        # Private dependencies of a static or imported library appear as
        # $<LINK_ONLY:target>.
        if(_link MATCHES "^\\$<LINK_ONLY:([^>]+)>$")
            set(_link "${CMAKE_MATCH_1}")
        endif()
        _flysight_collect_solver_runtime("${_link}")
    endforeach()
endfunction()

_flysight_collect_solver_runtime(gtsam)
get_property(FLYSIGHT_SOLVER_RUNTIME_TARGETS GLOBAL PROPERTY FLYSIGHT_SOLVER_RUNTIME)
message(STATUS "Solver runtime targets: ${FLYSIGHT_SOLVER_RUNTIME_TARGETS}")

foreach(_runtime IN LISTS FLYSIGHT_SOLVER_RUNTIME_TARGETS)
    if(_runtime MATCHES "^Boost::")
        message(FATAL_ERROR
            "Solver runtime target ${_runtime}: FlySight requires a GTSAM built without Boost "
            "(cmake/SolverSuperbuild.cmake).")
    endif()
endforeach()

# A missing entry means GTSAM was built static or without TBB. Either breaks
# deployment, and the second changes the numerical configuration.
foreach(_required gtsam metis-gtsam cephes-gtsam TBB::tbb TBB::tbbmalloc)
    if(NOT "${_required}" IN_LIST FLYSIGHT_SOLVER_RUNTIME_TARGETS)
        message(FATAL_ERROR
            "The GTSAM install at ${GTSAM_ROOT} does not provide the shared library target "
            "\"${_required}\" through the link interface of \"gtsam\".\n"
            "FlySight requires GTSAM built as shared libraries with GTSAM_WITH_TBB=ON "
            "(cmake/SolverSuperbuild.cmake).")
    endif()
endforeach()

# =============================================================================
# flysight_solver_stack(<target>)
# =============================================================================
#
# Gives the MAIN THREAD of an executable a 64 MiB stack. GTSAM's elimination
# of a large factor graph recurses deeply enough to overflow a default stack
# (1 MiB on Windows, 8 MiB on macOS and Linux).
#
# For test executables that run a fit on their main thread. The application
# does not need it: its fits run on the job queue's worker thread, which is
# created with a stack of this size.
#
# Usage:
#   flysight_solver_stack(tst_my_fusion_test)
#
function(flysight_solver_stack target)
    if(MSVC)
        target_link_options(${target} PRIVATE /STACK:67108864)
    elseif(MINGW)
        target_link_options(${target} PRIVATE -Wl,--stack,67108864)
    elseif(APPLE)
        # Unverified until the first CI run. Hexadecimal, a multiple of the
        # 16 KiB page size; ld accepts -stack_size for main executables only.
        target_link_options(${target} PRIVATE "LINKER:-stack_size,0x4000000")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Unverified until the first CI run. No link option sizes the main
        # thread's stack on Linux; see the comment in the source file.
        target_sources(${target} PRIVATE "${_FLYSIGHT_SOLVER_CMAKE_DIR}/solver_stack_linux.cpp")
    else()
        get_property(_warned GLOBAL PROPERTY FLYSIGHT_SOLVER_STACK_WARNED)
        if(NOT _warned)
            set_property(GLOBAL PROPERTY FLYSIGHT_SOLVER_STACK_WARNED TRUE)
            message(WARNING
                "flysight_solver_stack: no way to set a 64 MiB main-thread stack is known for "
                "${CMAKE_SYSTEM_NAME}; executables that run a fit on their main thread may overflow it.")
        endif()
    endif()
endfunction()

# =============================================================================
# flysight_solver_test_environment(<test>)
# =============================================================================
#
# Lets a CTest test find the solver libraries where they are installed, without
# copying anything next to the test executable (the same rule the test suite
# follows for Qt and GeographicLib). <test> must have been created with
# add_test(NAME ...), which flysight_add_test() does; generator expressions
# are not evaluated for the old add_test signature.
#
# On Linux this is needed even though the executable has a build RPATH:
# distributions link with RUNPATH, which does not apply to libgtsam's own
# dependency on libtbb in a different prefix.
#
# Usage:
#   flysight_solver_test_environment(tst_my_fusion_test)
#
function(flysight_solver_test_environment test)
    if(CMAKE_VERSION VERSION_LESS 3.22)
        message(WARNING "CMake < 3.22: add the GTSAM and oneTBB library directories to the "
                        "library search path before running ctest")
        return()
    endif()

    if(WIN32)
        set(_variable PATH)
    elseif(APPLE)
        set(_variable DYLD_LIBRARY_PATH)   # unverified until the first CI run
    else()
        set(_variable LD_LIBRARY_PATH)     # unverified until the first CI run
    endif()

    foreach(_runtime IN LISTS FLYSIGHT_SOLVER_RUNTIME_TARGETS)
        set_property(TEST ${test} APPEND PROPERTY
            ENVIRONMENT_MODIFICATION "${_variable}=path_list_prepend:$<TARGET_FILE_DIR:${_runtime}>")
    endforeach()
endfunction()

# =============================================================================
# flysight_install_solver_runtime(<destination>)
# =============================================================================
#
# Installs the solver runtime libraries, selected by exported target, into
# <destination> (relative to CMAKE_INSTALL_PREFIX, or absolute).
#
#   Windows: the DLL of each target, for the configuration being installed.
#   Unix:    exactly one real file per library, named by the name the loader
#            asks for (the soname, e.g. libgtsam.so.4 / libgtsam.4.dylib). No
#            symlinks and no second copy: GTSAM is tens of megabytes.
#   Linux:   additionally sets RPATH to $ORIGIN on the installed files so they
#            find each other inside the AppDir.
#   macOS:   nothing further here. Call this before the install step that runs
#            fix_macos_rpaths.sh, which rewrites the ids and references of
#            every dylib in Contents/Frameworks to @rpath.
#
# There is no dependency closure step. A Boost-free GTSAM and oneTBB depend
# only on each other and on system libraries, so the files selected by target
# are the complete set. The CI deployment checks prove that.
#
# The macOS and Linux branches are unverified until the first CI run.
#
# Usage:
#   flysight_install_solver_runtime(".")
#
function(flysight_install_solver_runtime destination)
    message(STATUS "")
    message(STATUS "----------------------------------------")
    message(STATUS "Solver Runtime Deployment Configuration")
    message(STATUS "----------------------------------------")
    message(STATUS "  Destination: ${destination}")

    if(WIN32)
        foreach(_runtime IN LISTS FLYSIGHT_SOLVER_RUNTIME_TARGETS)
            install(FILES "$<TARGET_FILE:${_runtime}>" DESTINATION "${destination}")
            _flysight_solver_imported_file(${_runtime} _location _soname)
            message(STATUS "  ${_runtime}: ${_location}")
        endforeach()
        message(STATUS "----------------------------------------")
        message(STATUS "")
        return()
    endif()

    set(_installed_names "")
    foreach(_runtime IN LISTS FLYSIGHT_SOLVER_RUNTIME_TARGETS)
        _flysight_solver_imported_file(${_runtime} _location _soname)
        if(NOT _location)
            message(FATAL_ERROR "Solver runtime target ${_runtime} has no imported location")
        endif()

        # The source is always the fully resolved file, so that a symlink
        # chain (libgtsam.so -> libgtsam.so.4 -> libgtsam.so.4.3a0) becomes
        # one regular file in the destination.
        get_filename_component(_real_file "${_location}" REALPATH)

        # The loader asks for the soname. It may carry a directory or an
        # @rpath/ prefix (macOS install names); only the file name matters.
        # An UNKNOWN_LIBRARY target has no soname property: its resolved file
        # name is the best available answer.
        if(_soname)
            get_filename_component(_installed_name "${_soname}" NAME)
        else()
            get_filename_component(_installed_name "${_real_file}" NAME)
        endif()

        install(FILES "${_real_file}"
            DESTINATION "${destination}"
            RENAME "${_installed_name}"
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                        GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
        )
        list(APPEND _installed_names "${_installed_name}")
        message(STATUS "  ${_runtime}: ${_real_file} -> ${_installed_name}")
    endforeach()

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Only the files installed above are touched; other libraries in the
        # destination belong to other deployment modules. patchelf is already
        # a hard requirement of DeployThirdPartyLinux.cmake.
        if(IS_ABSOLUTE "${destination}")
            set(_install_dir "${destination}")
        else()
            set(_install_dir "\${CMAKE_INSTALL_PREFIX}/${destination}")
        endif()
        install(CODE "
            message(STATUS \"Setting RPATH on solver runtime libraries...\")
            find_program(PATCHELF_EXE patchelf)
            if(NOT PATCHELF_EXE)
                message(FATAL_ERROR \"patchelf not found! Install with: apt-get install patchelf\")
            endif()
            foreach(_name ${_installed_names})
                execute_process(
                    COMMAND \"\${PATCHELF_EXE}\" --set-rpath \"\\\$ORIGIN\" \"${_install_dir}/\${_name}\"
                    RESULT_VARIABLE _patchelf_result
                    ERROR_VARIABLE _patchelf_error
                )
                if(NOT _patchelf_result EQUAL 0)
                    message(FATAL_ERROR \"patchelf failed on \${_name}: \${_patchelf_error}\")
                endif()
                message(STATUS \"  Set RPATH on \${_name}: \\\$ORIGIN\")
            endforeach()
        ")
    endif()

    message(STATUS "----------------------------------------")
    message(STATUS "")
endfunction()

# Reads the imported file and soname of a runtime target for the configuration
# being built: CMAKE_BUILD_TYPE, else the first imported configuration, else
# the unsuffixed properties. Exported targets only carry the configurations
# that were actually built.
function(_flysight_solver_imported_file target location_var soname_var)
    get_target_property(_aliased "${target}" ALIASED_TARGET)
    if(_aliased)
        set(target "${_aliased}")
    endif()

    get_target_property(_configs "${target}" IMPORTED_CONFIGURATIONS)
    set(_suffixes "")
    if(CMAKE_BUILD_TYPE)
        string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_type)
        if(_configs AND "${_build_type}" IN_LIST _configs)
            list(APPEND _suffixes "_${_build_type}")
        endif()
    endif()
    if(_configs)
        list(GET _configs 0 _first_config)
        string(TOUPPER "${_first_config}" _first_config)
        list(APPEND _suffixes "_${_first_config}")
    endif()
    # "NONE" stands for the unsuffixed properties (an empty list element would
    # be dropped).
    list(APPEND _suffixes "NONE")

    set(_location "")
    set(_soname "")
    foreach(_suffix IN LISTS _suffixes)
        if(_suffix STREQUAL "NONE")
            set(_suffix "")
        endif()
        get_target_property(_candidate "${target}" "IMPORTED_LOCATION${_suffix}")
        if(_candidate)
            set(_location "${_candidate}")
            get_target_property(_candidate_soname "${target}" "IMPORTED_SONAME${_suffix}")
            if(_candidate_soname)
                set(_soname "${_candidate_soname}")
            endif()
            break()
        endif()
    endforeach()

    set(${location_var} "${_location}" PARENT_SCOPE)
    set(${soname_var} "${_soname}" PARENT_SCOPE)
endfunction()

# =============================================================================
# flysight_assert_solver_confinement()
# =============================================================================
#
# "Only the code that needs GTSAM links it. The engine, session model, and job
# queue do not depend on GTSAM, and their tests build without it."
#
# Checked here, at configure time, on the real link closures: a text search
# cannot see link items that come from variables or from the link interface of
# another target. (The text half - who includes GTSAM headers - is in
# tests/audit/cleanup_audit.cmake, group solver-confinement.) The function only
# reads target properties; it changes nothing.
#
# Call it once from src/CMakeLists.txt, after every target of the application
# project and of tests/ has been defined.
#
# Rules, each a FATAL_ERROR naming the target and the path by which it reaches
# gtsam:
#   1. only these targets NAME gtsam on their own link line
set(_FLYSIGHT_GTSAM_NAMERS
    flysight_fusion tst_solver_smoke solver_deploy_probe tst_fusion_kernel)
#   2. only those, plus these, REACH gtsam through anything they link
set(_FLYSIGHT_GTSAM_REACHERS
    ${_FLYSIGHT_GTSAM_NAMERS}
    FlySightViewer
    flysight_fusion_test_support flysight_fusion_session_support
    tst_fusion_parity tst_fusion_session tst_fusion_jobs tst_fusion_rows)
#   3. stated separately for a clear message, although implied by 2: the
#      widget-free core, the Python bridge and the plot library never reach it
set(_FLYSIGHT_GTSAM_NEVER
    flysight_model flysight_core flysight_test_support flysight_cpp_bridge qcustomplot)

# _flysight_link_items(<out> <target> <property>): the link items of a target
# property as plain names. $<LINK_ONLY:x> is unwrapped (the private dependency
# of a static library still ends up on the consumer's link line); every other
# generator expression is dropped; ALIAS targets are resolved.
function(_flysight_link_items out target property)
    set(_items "")
    get_target_property(_links "${target}" "${property}")
    if(_links)
        foreach(_link IN LISTS _links)
            if(_link MATCHES "^\\$<LINK_ONLY:([^>]+)>$")
                set(_link "${CMAKE_MATCH_1}")
            endif()
            if(_link MATCHES "\\$<")
                continue()
            endif()
            if(TARGET "${_link}")
                get_target_property(_aliased "${_link}" ALIASED_TARGET)
                if(_aliased)
                    set(_link "${_aliased}")
                endif()
            endif()
            list(APPEND _items "${_link}")
        endforeach()
    endif()
    set(${out} "${_items}" PARENT_SCOPE)
endfunction()

# _flysight_path_to_gtsam(<out> <target> <property>): "a -> b -> gtsam", or
# empty when the target does not reach gtsam. <property> is LINK_LIBRARIES for
# the target being checked and INTERFACE_LINK_LIBRARIES below it.
function(_flysight_path_to_gtsam out target property)
    set(${out} "" PARENT_SCOPE)
    get_property(_visited GLOBAL PROPERTY _FLYSIGHT_CONFINEMENT_VISITED)
    if("${target}" IN_LIST _visited)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY _FLYSIGHT_CONFINEMENT_VISITED "${target}")

    _flysight_link_items(_items "${target}" "${property}")
    foreach(_item IN LISTS _items)
        if(_item STREQUAL "gtsam")
            set(${out} "${target} -> gtsam" PARENT_SCOPE)
            return()
        endif()
        if(TARGET "${_item}")
            _flysight_path_to_gtsam(_below "${_item}" INTERFACE_LINK_LIBRARIES)
            if(_below)
                set(${out} "${target} -> ${_below}" PARENT_SCOPE)
                return()
            endif()
        endif()
    endforeach()
endfunction()

function(flysight_assert_solver_confinement)
    # Every target of the application project, and of tests/ when it was added
    get_property(_targets DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    set(_tests_dir "${CMAKE_SOURCE_DIR}/../tests")
    if(FLYSIGHT_BUILD_TESTS AND IS_DIRECTORY "${_tests_dir}")
        get_property(_test_targets DIRECTORY "${_tests_dir}" PROPERTY BUILDSYSTEM_TARGETS)
        list(APPEND _targets ${_test_targets})
    endif()

    set(_reaching 0)
    set(_errors "")
    foreach(_target IN LISTS _targets)
        get_target_property(_type "${_target}" TYPE)
        if(_type STREQUAL "UTILITY" OR _type STREQUAL "INTERFACE_LIBRARY")
            continue()
        endif()

        _flysight_link_items(_own "${_target}" LINK_LIBRARIES)
        if("gtsam" IN_LIST _own AND NOT "${_target}" IN_LIST _FLYSIGHT_GTSAM_NAMERS)
            string(APPEND _errors
                "\n  ${_target} names gtsam on its link line. Only ${_FLYSIGHT_GTSAM_NAMERS} may.")
        endif()

        set_property(GLOBAL PROPERTY _FLYSIGHT_CONFINEMENT_VISITED "")
        _flysight_path_to_gtsam(_path "${_target}" LINK_LIBRARIES)
        if(NOT _path)
            continue()
        endif()
        math(EXPR _reaching "${_reaching} + 1")
        if("${_target}" IN_LIST _FLYSIGHT_GTSAM_NEVER)
            string(APPEND _errors
                "\n  ${_target} must never depend on GTSAM, but reaches it: ${_path}")
        elseif(NOT "${_target}" IN_LIST _FLYSIGHT_GTSAM_REACHERS)
            string(APPEND _errors
                "\n  ${_target} reaches GTSAM (${_path}) and is not one of: ${_FLYSIGHT_GTSAM_REACHERS}")
        endif()
    endforeach()

    if(_errors)
        message(FATAL_ERROR
            "GTSAM link confinement violated. Only the fusion library, the application "
            "and the fusion tests may link GTSAM (cmake/SolverDependencies.cmake, "
            "flysight_assert_solver_confinement):${_errors}")
    endif()
    message(STATUS "GTSAM link confinement: OK (${_reaching} targets reach gtsam)")
endfunction()
