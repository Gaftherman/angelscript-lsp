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
    # Apply warning suppression ONLY to C and C++ (prevents sending /wd to MASM .asm assembler)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C,CXX>:/FS>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4018>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4244>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4267>
    )
endif()

# ==============================================================================
# 1. DEPENDENCIES
# ==============================================================================
include(cmake/Dependencies.cmake)
include(cmake/AngelScript.cmake)
include(cmake/TreeSitter.cmake)

# ==============================================================================
# 2. CORE LIBRARY (Backend Code)
# ==============================================================================
file(GLOB_RECURSE BACKEND_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(REMOVE_ITEM BACKEND_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(angel_lsp_core STATIC ${BACKEND_SOURCES})
target_include_directories(angel_lsp_core PUBLIC 
    include
    src 
    "${tree_sitter_angelscript_SOURCE_DIR}/src" 
    "${tree_sitter_runtime_SOURCE_DIR}/lib/include"
    "${angelscript_repo_SOURCE_DIR}/sdk/angelscript/source"
)
target_link_libraries(angel_lsp_core PUBLIC 
    tree_sitter_angelscript_lib
    tree_sitter_doxygen_lib
    spdlog::spdlog
    AngelScriptLib
    nlohmann_json::nlohmann_json
    lsp
    unordered_dense::unordered_dense
)
if(MSVC)
    target_compile_options(angel_lsp_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()

# ==============================================================================
# 3. MAIN EXECUTABLE (LSP Server)
# ==============================================================================
add_executable(angel_lsp src/main.cpp)
target_link_libraries(angel_lsp PRIVATE angel_lsp_core)
if(MSVC)
    target_compile_options(angel_lsp PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()

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
set(LSP_DEPLOY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../client/bin")

add_custom_command(TARGET angel_lsp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LSP_DEPLOY_DEST}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LSP_DEPLOY_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:angel_lsp> "${LSP_DEPLOY_DEST}/angel_lsp.exe"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:angel_lsp> "${LSP_DEPLOY_ROOT}/angel_lsp.exe"
    COMMENT "Deploying compiled LSP server binary to client/bin"
)

# ==============================================================================
# 4. UNIT TESTS (doctest)
# ==============================================================================
enable_testing()
file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS "tests/*.cpp")
if(TEST_SOURCES)
    add_executable(angel_lsp_tests ${TEST_SOURCES})
    target_include_directories(angel_lsp_tests PRIVATE tests src)
    target_link_libraries(angel_lsp_tests PRIVATE angel_lsp_core doctest::doctest)
    target_compile_definitions(angel_lsp_tests PRIVATE
        ANGELSCRIPT_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../angelscript"
    )
    if(MSVC)
        target_compile_options(angel_lsp_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/FS>)
    endif()
    add_test(NAME angel_lsp_tests COMMAND angel_lsp_tests)

    # The full angelscript/ corpus audit test case is skip()-decorated in-code, so it is
    # excluded from the angel_lsp_tests run above. It is deliberately NOT registered as its
    # own add_test() - any add_test() runs on every plain `ctest` invocation regardless of
    # doctest-level skip(), which would silently make the default test run slow again. Run it
    # on demand instead:
    #   Debug/angel_lsp_tests.exe --no-skip --test-case="*Corpus Audit*"
endif()