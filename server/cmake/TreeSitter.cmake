# ── Tree-Sitter Runtime ───────────────────────────────────────────────────
FetchContent_Declare(tree_sitter_runtime GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git GIT_TAG v0.26.11)
FetchContent_MakeAvailable(tree_sitter_runtime)
add_library(tree_sitter_runtime STATIC "${tree_sitter_runtime_SOURCE_DIR}/lib/src/lib.c")
target_include_directories(tree_sitter_runtime PUBLIC "${tree_sitter_runtime_SOURCE_DIR}/lib/include"
                                               PRIVATE "${tree_sitter_runtime_SOURCE_DIR}/lib/src")
set_target_properties(tree_sitter_runtime PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(tree_sitter_runtime PRIVATE /W0)
else()
    target_compile_options(tree_sitter_runtime PRIVATE -w)
endif()

# ── Tree-Sitter AngelScript Grammar ───────────────────────────────────────
# 23cb160 adds single quote digit separator support (e.g. 1'000'000).
# aa14847 adds metadata blocks and omitted initializer elements. `[Property, Category="Weapons"]`
# before a declaration is stripped by CScriptBuilder before the compiler sees it, and `{ 0, 1, , 4 }`
# gives the omitted element the type's default - both compile, and neither had a grammar rule, so
# each turned its whole declaration into an ERROR node and the symbol left the index with it.
#
# 017b0d3, its parent, adds `array<T>::less` / `T[]::less` - a name nested in a template or array
# type, which is how the array add-on registers its sort comparator and how every predefined stub
# declaring `sort` spells it. Before it, that declaration produced an ERROR node and the server
# reported a syntax error in a stub the user did not write.
#
# ANGELLSP_TREE_SITTER_ANGELSCRIPT_SOURCE builds against a local checkout instead of fetching one.
# A grammar change and the analyzer change that depends on it land together, and the pin above
# cannot name a commit that has not been pushed yet; this is how the two are developed side by side:
#
#   cmake -B build -S . -DANGELLSP_TREE_SITTER_ANGELSCRIPT_SOURCE=E:/Github/src/tree-sitter-angelscript
set(ANGELLSP_TREE_SITTER_ANGELSCRIPT_SOURCE "" CACHE PATH
    "Local tree-sitter-angelscript checkout to build against instead of the pinned commit")

if(ANGELLSP_TREE_SITTER_ANGELSCRIPT_SOURCE)
    if(NOT EXISTS "${ANGELLSP_TREE_SITTER_ANGELSCRIPT_SOURCE}/src/parser.c")
        message(FATAL_ERROR
            "ANGELLSP_TREE_SITTER_ANGELSCRIPT_SOURCE is set to "
            "'${ANGELLSP_TREE_SITTER_ANGELSCRIPT_SOURCE}', which has no src/parser.c. "
            "Point it at a tree-sitter-angelscript checkout, and run `tree-sitter generate` in it "
            "if the generated parser is missing.")
    endif()
    set(tree_sitter_angelscript_SOURCE_DIR "${ANGELLSP_TREE_SITTER_ANGELSCRIPT_SOURCE}")
    message(STATUS "tree-sitter-angelscript: local checkout at ${tree_sitter_angelscript_SOURCE_DIR}")
else()
    FetchContent_Declare(tree_sitter_angelscript GIT_REPOSITORY https://github.com/Gaftherman/tree-sitter-angelscript.git GIT_TAG 23cb1607d068d74e410c866258e4957aa63082ce)
    FetchContent_MakeAvailable(tree_sitter_angelscript)
endif()

add_library(tree_sitter_angelscript_lib STATIC 
    "${tree_sitter_angelscript_SOURCE_DIR}/src/parser.c"
    "${tree_sitter_angelscript_SOURCE_DIR}/src/scanner.c"
)
target_include_directories(tree_sitter_angelscript_lib PUBLIC "${tree_sitter_angelscript_SOURCE_DIR}/src")
target_link_libraries(tree_sitter_angelscript_lib PUBLIC tree_sitter_runtime)
set_target_properties(tree_sitter_angelscript_lib PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(tree_sitter_angelscript_lib PRIVATE /W0)
else()
    target_compile_options(tree_sitter_angelscript_lib PRIVATE -w)
endif()

# ── Tree-Sitter Doxygen Grammar ───────────────────────────────────────────
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_Declare(tree_sitter_doxygen GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-doxygen.git GIT_TAG 6069b1815b139080d6c562b5ff9ae2296cbc6602)
FetchContent_GetProperties(tree_sitter_doxygen)
if(NOT tree_sitter_doxygen_POPULATED)
    FetchContent_Populate(tree_sitter_doxygen)
endif()

add_library(tree_sitter_doxygen_lib STATIC 
    "${tree_sitter_doxygen_SOURCE_DIR}/src/parser.c"
    "${tree_sitter_doxygen_SOURCE_DIR}/src/scanner.c"
)
target_include_directories(tree_sitter_doxygen_lib PUBLIC "${tree_sitter_doxygen_SOURCE_DIR}/src")
target_link_libraries(tree_sitter_doxygen_lib PUBLIC tree_sitter_runtime)
set_target_properties(tree_sitter_doxygen_lib PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(tree_sitter_doxygen_lib PRIVATE /W0)
else()
    target_compile_options(tree_sitter_doxygen_lib PRIVATE -w)
endif()
