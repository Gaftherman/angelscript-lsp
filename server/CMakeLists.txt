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
    # /utf-8 applied only to our sources — third-party deps may have non-UTF-8 encoded files
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
set(LSP_BUILD_DEBUG_DEST "${CMAKE_CURRENT_SOURCE_DIR}/../build/Debug")
set(LSP_BUILD_RELEASE_DEST "${CMAKE_CURRENT_SOURCE_DIR}/../build/Release")

add_custom_command(TARGET angel_lsp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LSP_DEPLOY_DEST}"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:angel_lsp> "${LSP_DEPLOY_DEST}/angel_lsp.exe"
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
    if(MSVC)
        target_compile_options(angel_lsp_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
    endif()
    add_test(NAME angel_lsp_tests COMMAND angel_lsp_tests)
endif()

# ==============================================================================
# 5. OPTIONAL DOXYGEN TARGET
# ==============================================================================
find_package(Doxygen OPTIONAL_COMPONENTS dot)
if(DOXYGEN_FOUND)
    set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/docs)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating Doxygen API documentation..."
        VERBATIM
    )
endif()