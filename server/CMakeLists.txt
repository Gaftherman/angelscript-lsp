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
set(LSP_BUILD_DEBUG_DEST "${CMAKE_CURRENT_SOURCE_DIR}/../build/Debug")
set(LSP_BUILD_RELEASE_DEST "${CMAKE_CURRENT_SOURCE_DIR}/../build/Release")

add_custom_command(TARGET angel_lsp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LSP_DEPLOY_DEST}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:angel_lsp> "${LSP_DEPLOY_DEST}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:angel_lsp> "${LSP_BUILD_DEBUG_DEST}/" || ${CMAKE_COMMAND} -E echo "Skipped locked Debug target"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:angel_lsp> "${LSP_BUILD_RELEASE_DEST}/" || ${CMAKE_COMMAND} -E echo "Skipped locked Release target"
    COMMENT "Deploying compiled LSP server binary to client and build outputs"
)

# ==============================================================================
# 4. MODULAR UNIT TESTS EXECUTABLES
# ==============================================================================
enable_testing()

add_library(doctest_main STATIC tests/test_main.cpp)
target_include_directories(doctest_main PUBLIC
    src
    tests
    "${tree_sitter_angelscript_SOURCE_DIR}/src"
    "${tree_sitter_runtime_SOURCE_DIR}/lib/include"
    "${angelscript_repo_SOURCE_DIR}/sdk/angelscript/source"
)
target_link_libraries(doctest_main PUBLIC doctest::doctest spdlog::spdlog)

function(add_lsp_test_suite TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN})
    target_include_directories(${TARGET_NAME} PRIVATE
        src
        tests
        "${tree_sitter_angelscript_SOURCE_DIR}/src"
        "${tree_sitter_runtime_SOURCE_DIR}/lib/include"
        "${angelscript_repo_SOURCE_DIR}/sdk/angelscript/source"
    )
    target_link_libraries(${TARGET_NAME} PRIVATE
        angel_lsp_core
        doctest_main
    )
    if(WIN32)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${TARGET_NAME}>
                $<TARGET_FILE:${TARGET_NAME}>
                $<TARGET_FILE_DIR:${TARGET_NAME}>
            COMMAND_EXPAND_LISTS
            COMMENT "Copying runtime DLLs for ${TARGET_NAME}..."
        )
    endif()
    add_test(NAME ${TARGET_NAME} COMMAND $<TARGET_FILE:${TARGET_NAME}>)
endfunction()

add_lsp_test_suite(test_core
    tests/angelscript/ScriptClassTests.cpp
    tests/angelscript/ScriptFuncdefTests.cpp
    tests/angelscript/ScriptNamespaceTests.cpp
    tests/document/DocumentTests.cpp
    tests/parser/TreeSitterTests.cpp
)

add_lsp_test_suite(test_analysis
    tests/analysis/DiagnosticCacheTests.cpp
    tests/analysis/PredefinedLoaderTests.cpp
    tests/analysis/SymbolCollectorTests.cpp
    tests/analysis/SymbolResolverTests.cpp
    tests/analysis/SymbolTableTests.cpp
    tests/analysis/ValidationOracleTests.cpp
)

add_lsp_test_suite(test_feature_hover
    tests/features/hover/HoverRenderTests.cpp
    tests/features/hover/HoverTemplateTest.cpp
    tests/features/hover/ComplexHoverTests.cpp
    tests/features/hover/ComplexHoverEdgeCases.cpp
)

add_lsp_test_suite(test_feature_definition
    tests/features/definition/DefinitionHandlerTests.cpp
)

add_lsp_test_suite(test_feature_semantic_tokens
    tests/features/semantic_tokens/SemanticTokensTests.cpp
)

add_lsp_test_suite(test_feature_completion
    tests/features/completion/CompletionTests.cpp
)

add_lsp_test_suite(test_feature_signature_help
    tests/features/signature_help/SignatureHelpTests.cpp
)

add_lsp_test_suite(test_i18n
    tests/i18n/LocalizationTests.cpp
)