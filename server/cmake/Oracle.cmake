# The parity oracle: a real AngelScript compiler, built from source.
#
# Opt-in, because it fetches and compiles the whole AngelScript SDK and most script writers building
# this server have no reason to pay for that. CI turns it on so the parity audit can run there -
# until now that audit needed a binary from an unpublished repository, which meant the only check
# that compares this analyzer against the actual language ran on one machine.
#
#   cmake -B build -S . -DANGELLSP_BUILD_ORACLE=ON
#   ctest --test-dir build -R Parity
#
# See server/tools/oracle/main.cpp for what it registers, and
# server/tests/fixtures/sdk-addons.as.predefined for the stub that describes the same surface.

option(ANGELLSP_BUILD_ORACLE "Build the AngelScript compiler used as the parity oracle" OFF)

if(NOT ANGELLSP_BUILD_ORACLE)
    return()
endif()

include(FetchContent)

# The same fork AS-Harness builds against, so the oracle answers what that harness answered.
FetchContent_Declare(
    angelscript_sdk
    GIT_REPOSITORY https://github.com/anjo76/angelscript.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(angelscript_sdk)

set(ANGELSCRIPT_SDK_DIR "${angelscript_sdk_SOURCE_DIR}/sdk")
set(ANGELSCRIPT_ADDON_DIR "${ANGELSCRIPT_SDK_DIR}/add_on")

if(NOT EXISTS "${ANGELSCRIPT_SDK_DIR}/angelscript/include/angelscript.h")
    message(FATAL_ERROR
        "The AngelScript SDK was fetched but does not look like one: no "
        "angelscript/include/angelscript.h under ${ANGELSCRIPT_SDK_DIR}.")
endif()

# The library. Its own CMakeLists lives under angelscript/projects/cmake and defines the target
# `angelscript`; using it keeps the platform-specific assembly (as_callfunc_*) selection out of here.
add_subdirectory("${ANGELSCRIPT_SDK_DIR}/angelscript/projects/cmake" angelscript_lib EXCLUDE_FROM_ALL)

# One translation unit per add-on this oracle registers. Kept explicit rather than globbed: the set
# is the contract with the stub, and a glob would quietly widen it when the SDK gains an add-on.
set(ANGELLSP_ORACLE_ADDONS
    scriptarray/scriptarray.cpp
    scriptany/scriptany.cpp
    scriptbuilder/scriptbuilder.cpp
    scriptdictionary/scriptdictionary.cpp
    scriptfile/scriptfile.cpp
    scriptfile/scriptfilesystem.cpp
    scriptgrid/scriptgrid.cpp
    scripthandle/scripthandle.cpp
    scripthelper/scripthelper.cpp
    scriptmath/scriptmath.cpp
    scriptmath/scriptmathcomplex.cpp
    scriptstdstring/scriptstdstring.cpp
    scriptstdstring/scriptstdstring_utils.cpp
    datetime/datetime.cpp
    weakref/weakref.cpp
)

list(TRANSFORM ANGELLSP_ORACLE_ADDONS PREPEND "${ANGELSCRIPT_ADDON_DIR}/")

add_executable(angelscript_oracle
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/oracle/main.cpp"
    ${ANGELLSP_ORACLE_ADDONS}
)

target_include_directories(angelscript_oracle PRIVATE
    "${ANGELSCRIPT_SDK_DIR}/angelscript/include"
    "${ANGELSCRIPT_ADDON_DIR}"
)

target_link_libraries(angelscript_oracle PRIVATE angelscript)

set_target_properties(angelscript_oracle PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Third-party code, and not ours to keep warning-clean.
if(MSVC)
    target_compile_options(angelscript_oracle PRIVATE /W0)
else()
    target_compile_options(angelscript_oracle PRIVATE -w)
endif()

message(STATUS "Parity oracle: angelscript_oracle will be built from ${ANGELSCRIPT_SDK_DIR}")
