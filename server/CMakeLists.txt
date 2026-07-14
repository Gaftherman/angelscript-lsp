cmake_minimum_required(VERSION 3.14)
set(CMAKE_POLICY_VERSION_MINIMUM 3.14)
project(AngelLSP VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    enable_language(ASM_MASM)
endif()

# ── Vendor libraries (tree-sitter runtime + AngelScript grammar) ────────────
add_subdirectory(vendor)

include(FetchContent)

FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_MakeAvailable(json)

FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.13.0)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(angelscript_repo GIT_REPOSITORY https://github.com/anjo76/angelscript.git GIT_TAG master)
FetchContent_MakeAvailable(angelscript_repo)

file(GLOB AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/angelscript/source/*.cpp")
list(APPEND AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/add_on/scriptbuilder/scriptbuilder.cpp")
list(APPEND AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/add_on/scriptstdstring/scriptstdstring.cpp")
list(APPEND AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/add_on/scriptstdstring/scriptstdstring_utils.cpp")
list(APPEND AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/add_on/scriptarray/scriptarray.cpp")
list(APPEND AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/add_on/scriptdictionary/scriptdictionary.cpp")
list(APPEND AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/add_on/scriptmath/scriptmath.cpp")

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/angelscript/source/as_callfunc_x64_msvc_asm.asm")
endif()

add_library(AngelScriptLib STATIC ${AS_SOURCES})
target_include_directories(AngelScriptLib PUBLIC 
    "${angelscript_repo_SOURCE_DIR}/sdk/angelscript/include"
    "${angelscript_repo_SOURCE_DIR}/sdk/add_on"
)

# ── Tests ───────────────────────────────────────────────────────────────────
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
)
FetchContent_MakeAvailable(doctest)

# We will compile all our source files into a static library so they can be
# shared easily between the LSP executable and the Tests executable.
file(GLOB_RECURSE BACKEND_SOURCES "src/*.cpp")

# Remove test_main.cpp or main.cpp from the shared sources if we have them
list(REMOVE_ITEM BACKEND_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(angel_lsp_core STATIC ${BACKEND_SOURCES})
target_include_directories(angel_lsp_core PUBLIC 
    src 
    vendor/tree-sitter-angelscript/src 
    vendor/tree-sitter-runtime/lib/include
)
target_link_libraries(angel_lsp_core PUBLIC 
    tree_sitter_angelscript
    nlohmann_json::nlohmann_json 
    spdlog::spdlog
    AngelScriptLib
)

# ── Main Executable ─────────────────────────────────────────────────────────
add_executable(angel_lsp src/main.cpp)
target_link_libraries(angel_lsp PRIVATE angel_lsp_core)

if(WIN32)
    set(TARGET_OS "win32")
elseif(APPLE)
    set(TARGET_OS "darwin")
else()
    set(TARGET_OS "linux")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(TARGET_ARCH "x64")
else()
    set(TARGET_ARCH "x86")
endif()

set(LSP_DEPLOY_DEST "${CMAKE_CURRENT_SOURCE_DIR}/../client/bin/${TARGET_OS}-${TARGET_ARCH}")

add_custom_command(TARGET angel_lsp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LSP_DEPLOY_DEST}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:angel_lsp> "${LSP_DEPLOY_DEST}/"
    COMMENT "Deploying compiled LSP server binary to client: bin/${TARGET_OS}-${TARGET_ARCH}"
)

# ── Test Executable ─────────────────────────────────────────────────────────
file(GLOB_RECURSE TEST_SOURCES "tests/*.cpp")
add_executable(angel_lsp_tests ${TEST_SOURCES})
target_link_libraries(angel_lsp_tests PRIVATE 
    angel_lsp_core
    doctest::doctest
)
enable_testing()
add_test(NAME AngelLSPTests COMMAND angel_lsp_tests)