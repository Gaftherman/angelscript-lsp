cmake_minimum_required(VERSION 3.14)
set(CMAKE_POLICY_VERSION_MINIMUM 3.14)
project(AngelLSP VERSION 1.0)

# Require C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    enable_language(ASM_MASM)
endif()

if(MSVC)
    # Suppress spdlog/fmt C++20 deprecation warnings on Windows
    add_compile_definitions(_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)
    add_compile_definitions(_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS)
    add_compile_options(/wd4018 /wd4244 /wd4267) # Suppress tree-sitter specific warnings
endif()

# ==============================================================================
# 1. DEPENDENCIES (FetchContent)
# ==============================================================================
include(FetchContent)

# ── 1.1 JSON Parser (nlohmann/json) ───────────────────────────────────────────
FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_MakeAvailable(json)

# ── 1.2 Logger (spdlog) ───────────────────────────────────────────────────────
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.13.0)
FetchContent_MakeAvailable(spdlog)

# ── 1.3 Unit Testing (doctest) ────────────────────────────────────────────────
FetchContent_Declare(doctest GIT_REPOSITORY https://github.com/doctest/doctest.git GIT_TAG v2.4.11)
FetchContent_MakeAvailable(doctest)

# ── 1.4 AngelScript Compiler ──────────────────────────────────────────────────
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

# ── 1.5 Tree-Sitter Runtime ───────────────────────────────────────────────────
FetchContent_Declare(tree_sitter_runtime GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git GIT_TAG v0.26.11)
FetchContent_Populate(tree_sitter_runtime)
add_library(tree_sitter_runtime STATIC "${tree_sitter_runtime_SOURCE_DIR}/lib/src/lib.c")
target_include_directories(tree_sitter_runtime PUBLIC "${tree_sitter_runtime_SOURCE_DIR}/lib/include")
set_target_properties(tree_sitter_runtime PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(tree_sitter_runtime PRIVATE /w) # Disable all warnings for tree_sitter_runtime
endif()

# ── 1.6 Tree-Sitter AngelScript Grammar ───────────────────────────────────────
set(tree_sitter_angelscript_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tree-sitter-angelscript")

# ---------------------------------------------------------
# lsp-framework (Language Server Protocol C++20 Framework)
# ---------------------------------------------------------
set(LSP_BUILD_EXAMPLES OFF CACHE BOOL "Disable Examples")
set(LSP_INSTALL OFF CACHE BOOL "Disable Install")

FetchContent_Declare(
    lsp_framework
    GIT_REPOSITORY https://github.com/leon-bckl/lsp-framework.git
    GIT_TAG master
)
FetchContent_MakeAvailable(lsp_framework)
add_library(tree_sitter_angelscript_lib STATIC 
    "${tree_sitter_angelscript_SOURCE_DIR}/src/parser.c"
    "${tree_sitter_angelscript_SOURCE_DIR}/src/scanner.c"
)
target_include_directories(tree_sitter_angelscript_lib PUBLIC "${tree_sitter_angelscript_SOURCE_DIR}/src")
target_link_libraries(tree_sitter_angelscript_lib PUBLIC tree_sitter_runtime)
set_target_properties(tree_sitter_angelscript_lib PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

# ── 1.7 Fast Hash Map (unordered_dense) ───────────────────────────────────────
FetchContent_Declare(
    unordered_dense
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG v4.4.0
)
FetchContent_MakeAvailable(unordered_dense)


# ==============================================================================
# 2. CORE LIBRARY (Backend Code)
# ==============================================================================
# We compile all our source files into a static library so they can be
# shared easily between the LSP executable and the Tests executable.
file(GLOB_RECURSE BACKEND_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(REMOVE_ITEM BACKEND_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(angel_lsp_core STATIC ${BACKEND_SOURCES})
target_include_directories(angel_lsp_core PUBLIC 
    include
    src 
    "${tree_sitter_angelscript_SOURCE_DIR}/src" 
    "${tree_sitter_runtime_SOURCE_DIR}/lib/include"
)
target_link_libraries(angel_lsp_core PUBLIC 
    tree_sitter_angelscript_lib
    spdlog::spdlog
    AngelScriptLib
    nlohmann_json::nlohmann_json
    lsp
    unordered_dense::unordered_dense
)


# ==============================================================================
# 3. MAIN EXECUTABLE (LSP Server)
# ==============================================================================
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


# ==============================================================================
# 4. UNIT TESTS EXECUTABLE
# ==============================================================================
file(GLOB_RECURSE TEST_SOURCES "tests/*.cpp")
add_executable(angel_lsp_tests ${TEST_SOURCES})
target_link_libraries(angel_lsp_tests PRIVATE 
    angel_lsp_core
    doctest::doctest
)
enable_testing()
add_test(NAME AngelLSPTests COMMAND angel_lsp_tests)