# ── tree-sitter runtime ─────────────────────────────────────────────────────
# Compiled from the single amalgamated lib.c
add_library(tree_sitter_runtime STATIC
    tree-sitter-runtime/lib/src/lib.c
)
target_include_directories(tree_sitter_runtime PUBLIC
    tree-sitter-runtime/lib/include   # exposes tree_sitter/api.h
)
set_target_properties(tree_sitter_runtime PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

# ── tree-sitter AngelScript grammar ─────────────────────────────────────────
# parser.c  — generated LALR(1) parser tables
# scanner.c — external scanner for template <> disambiguation
add_library(tree_sitter_angelscript STATIC
    tree-sitter-angelscript/src/parser.c
    tree-sitter-angelscript/src/scanner.c
)
target_include_directories(tree_sitter_angelscript PUBLIC
    tree-sitter-angelscript/src        # exposes tree_sitter/parser.h (internal)
)
target_link_libraries(tree_sitter_angelscript PUBLIC
    tree_sitter_runtime
)
set_target_properties(tree_sitter_angelscript PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)
