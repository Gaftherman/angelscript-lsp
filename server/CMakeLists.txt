cmake_minimum_required(VERSION 3.14)
set(CMAKE_POLICY_VERSION_MINIMUM 3.14)

# One version, read from the manifest that actually publishes it.
#
# The server hardcoded its own string and the extension carried another, with nothing tying them
# together: the server said 1.0.0 while client/package.json said 0.1.0. A release tagged from one
# would have shipped a .vsix declaring the other - `vsce package` reads package.json and VS Code
# shows what the .vsix declares, while `angel_lsp --version` printed something else entirely.
#
# A check that the two agree would have caught that drift. Reading one from the other makes the
# drift unrepresentable, which is better. The manifest wins because it is the file that reaches
# users. Parsed with a regex rather than string(JSON) so this keeps working on CMake 3.14; the
# manifest is ours and its shape is not going to surprise us.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../client/package.json" ANGELLSP_MANIFEST)
string(REGEX MATCH "\"version\"[ \t]*:[ \t]*\"([^\"]+)\"" ANGELLSP_VERSION_MATCH "${ANGELLSP_MANIFEST}")
set(ANGELLSP_VERSION "${CMAKE_MATCH_1}")

if(NOT ANGELLSP_VERSION)
    message(FATAL_ERROR
        "Could not read \"version\" from client/package.json. The server takes its version from the "
        "extension manifest so the two can never disagree, and building the server without the "
        "client tree beside it is not a supported configuration.")
endif()

# project() takes only the numeric part, so a pre-release suffix like 0.1.0-rc.1 does not break the
# configure. The server still reports the full string, suffix and all.
string(REGEX MATCH "^[0-9]+(\\.[0-9]+)*" ANGELLSP_VERSION_NUMERIC "${ANGELLSP_VERSION}")

message(STATUS "AngelLSP version: ${ANGELLSP_VERSION} (from client/package.json)")

# Read at configure time, so without this an incremental build after a version bump keeps compiling
# the old number in - which is exactly when it matters, because a version bump is the last thing
# that happens before a release is tagged. Measured: bumping to 0.2.0 and rebuilding still printed
# 0.1.0 from --version.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/../client/package.json")

project(AngelLSP VERSION ${ANGELLSP_VERSION_NUMERIC})

# Require C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The AngelScript SDK is not linked: nothing in server/src references asIScriptEngine or any
# other symbol from it (verified by grep), and building the whole SDK on every configure cost
# real time for nothing. Re-add cmake/AngelScript.cmake, the include directory and the
# AngelScriptLib link - plus the MASM language below, needed by as_callfunc_x64_msvc_asm.asm -
# when real-compiler diagnostics are actually built.

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    # Suppress spdlog/fmt C++20 deprecation warnings on Windows
    add_compile_definitions(_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)
    add_compile_definitions(_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS)
    # Apply warning suppression ONLY to C and C++ (prevents sending /wd to MASM .asm assembler)
    #
    # Scope note: /wd4018 (signed/unsigned mismatch), /wd4244 and /wd4267 (narrowing conversions)
    # used to be set here, globally. They are the vendored dependencies' noise - tree-sitter's C in
    # particular - but applying them project-wide silenced them in our own code too, which is
    # exactly where they matter: this server narrows size_t byte offsets to uint32_t on every
    # incremental edit. They now live on the third-party targets only, and our targets are compiled
    # at ANGELLSP_WARNING_FLAGS below.
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/FS>)
endif()

# ------------------------------------------------------------------------------
# Warning level for this project's own targets (never for vendored dependencies).
# ------------------------------------------------------------------------------
if(MSVC)
    set(ANGELLSP_WARNING_FLAGS $<$<COMPILE_LANGUAGE:CXX>:/W4>)
else()
    set(ANGELLSP_WARNING_FLAGS $<$<COMPILE_LANGUAGE:CXX>:-Wall>
                               $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
                               $<$<COMPILE_LANGUAGE:CXX>:-Wmisleading-indentation>)
endif()

option(ANGELLSP_WERROR "Treat warnings in angel_lsp's own code as errors" OFF)
if(ANGELLSP_WERROR)
    if(MSVC)
        list(APPEND ANGELLSP_WARNING_FLAGS $<$<COMPILE_LANGUAGE:CXX>:/WX>)
    else()
        list(APPEND ANGELLSP_WARNING_FLAGS $<$<COMPILE_LANGUAGE:CXX>:-Werror>)
    endif()
endif()

# ------------------------------------------------------------------------------
# Sanitizers. Off by default; the thread sanitizer is the only practical way to
# catch the cross-thread state this server shares between the message loop, the
# analysis thread and the workspace scan - none of which a unit test can observe.
# ------------------------------------------------------------------------------
set(ANGELLSP_SANITIZE "" CACHE STRING "Sanitizer to build with: address, undefined, thread, or empty")
set_property(CACHE ANGELLSP_SANITIZE PROPERTY STRINGS "" address undefined thread)

if(ANGELLSP_SANITIZE)
    if(MSVC)
        if(NOT ANGELLSP_SANITIZE STREQUAL "address")
            message(FATAL_ERROR "MSVC supports only ANGELLSP_SANITIZE=address; use Clang for thread/undefined.")
        endif()
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=${ANGELLSP_SANITIZE} -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=${ANGELLSP_SANITIZE})
    endif()
    message(STATUS "Building with sanitizer: ${ANGELLSP_SANITIZE}")
endif()

# ==============================================================================
# 1. DEPENDENCIES
# ==============================================================================
include(cmake/Dependencies.cmake)
include(cmake/TreeSitter.cmake)
include(cmake/Oracle.cmake)

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
)
target_link_libraries(angel_lsp_core PUBLIC 
    tree_sitter_angelscript_lib
    tree_sitter_doxygen_lib
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    lsp
    unordered_dense::unordered_dense
)
if(MSVC)
    target_compile_options(angel_lsp_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()
target_compile_options(angel_lsp_core PRIVATE ${ANGELLSP_WARNING_FLAGS})

# PUBLIC so the executable and the test suite report the same version the manifest declares. There
# is no default: a missing definition should fail to compile rather than quietly ship a wrong one.
target_compile_definitions(angel_lsp_core PUBLIC ANGELLSP_VERSION="${ANGELLSP_VERSION}")

# ==============================================================================
# 3. MAIN EXECUTABLE (LSP Server)
# ==============================================================================
add_executable(angel_lsp src/main.cpp)
target_link_libraries(angel_lsp PRIVATE angel_lsp_core)
if(MSVC)
    target_compile_options(angel_lsp PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()
target_compile_options(angel_lsp PRIVATE ${ANGELLSP_WARNING_FLAGS})

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
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:angel_lsp> "${LSP_DEPLOY_DEST}/$<TARGET_FILE_NAME:angel_lsp>"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:angel_lsp> "${LSP_DEPLOY_ROOT}/$<TARGET_FILE_NAME:angel_lsp>"
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
        ANGELSCRIPT_PARITY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/parity"
        ANGELSCRIPT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"

        # The repository root, so the stub sweep can find every predefined file that ships with the
        # project *and* any a developer has dropped in their own checkout. The server reads those
        # the moment the workspace is opened; nothing was checking they parse.
        ANGELSCRIPT_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
    )
    if(MSVC)
        target_compile_options(angel_lsp_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/FS> $<$<COMPILE_LANGUAGE:CXX>:/wd5285>)
    endif()
    target_compile_options(angel_lsp_tests PRIVATE ${ANGELLSP_WARNING_FLAGS})
    add_test(NAME angel_lsp_tests COMMAND angel_lsp_tests)

    # The full angelscript/ corpus audit test case is skip()-decorated in-code, so it is
    # excluded from the angel_lsp_tests run above. It is deliberately NOT registered as its
    # own add_test() - any add_test() runs on every plain `ctest` invocation regardless of
    # doctest-level skip(), which would silently make the default test run slow again. Run it
    # on demand instead:
    #   Debug/angel_lsp_tests.exe --no-skip --test-case="*Corpus Audit*"
endif()