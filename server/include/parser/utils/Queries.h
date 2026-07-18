#pragma once
#include <string>

namespace queries
{

/**
 * @brief Tree-Sitter query string for syntax highlighting tokens.
 */
const std::string HIGHLIGHTS = R"(
    [
        "class" "interface" "namespace" "enum" "struct" "mixin" "funcdef"
    ] @keyword

    [
        "if" "else" "switch" "case" "default" "for" "while" "do" "break" "continue" "return" "yield"
    ] @keyword.control

    (primitive_type) @type.builtin
    
    (identifier) @variable

    (function_declaration 
        name: (identifier) @function
    )
    (method_declaration
        name: (identifier) @function.method
    )
    
    (number) @number
    (string) @string
    (comment) @comment
)";

/**
 * @brief Tree-Sitter query string for local variable definitions and references.
 */
const std::string LOCALS = R"(
    (variable_declaration
        name: (identifier) @local.definition
    )
    
    (parameter
        name: (identifier) @local.definition
    )
    
    (identifier) @local.reference
)";

} // namespace queries
