# ── Tree-Sitter Runtime ───────────────────────────────────────────────────
FetchContent_Declare(tree_sitter_runtime GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git GIT_TAG v0.26.11)
FetchContent_MakeAvailable(tree_sitter_runtime)
add_library(tree_sitter_runtime STATIC "${tree_sitter_runtime_SOURCE_DIR}/lib/src/lib.c")
target_include_directories(tree_sitter_runtime PUBLIC "${tree_sitter_runtime_SOURCE_DIR}/lib/include")
set_target_properties(tree_sitter_runtime PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(tree_sitter_runtime PRIVATE /w) # Disable all warnings for tree_sitter_runtime
endif()

# ── Tree-Sitter AngelScript Grammar ───────────────────────────────────────
FetchContent_Declare(tree_sitter_angelscript GIT_REPOSITORY https://github.com/Gaftherman/tree-sitter-angelscript.git GIT_TAG 98b5f4dd890085bf9750e7c699dbc1c21bb8c54e)
FetchContent_MakeAvailable(tree_sitter_angelscript)

add_library(tree_sitter_angelscript_lib STATIC 
    "${tree_sitter_angelscript_SOURCE_DIR}/src/parser.c"
    "${tree_sitter_angelscript_SOURCE_DIR}/src/scanner.c"
)
target_include_directories(tree_sitter_angelscript_lib PUBLIC "${tree_sitter_angelscript_SOURCE_DIR}/src")
target_link_libraries(tree_sitter_angelscript_lib PUBLIC tree_sitter_runtime)
set_target_properties(tree_sitter_angelscript_lib PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(tree_sitter_angelscript_lib PRIVATE /w)
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
    target_compile_options(tree_sitter_doxygen_lib PRIVATE /w)
endif()
