/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

/**
 * @param {RuleOrLiteral} rule
 * @returns {SeqRule}
 */
function commaSep1(rule) {
  return seq(rule, repeat(seq(",", rule)));
}

/**
 * @param {RuleOrLiteral} rule
 * @returns {ChoiceRule}
 */
function commaSep(rule) {
  return optional(commaSep1(rule));
}

module.exports = grammar({
  name: "angelscript",

  extras: $ => [
    /\s+/,
    $.comment,
    $.preproc_directive,
  ],

  word: $ => $.identifier,

  externals: $ => [
    $._template_open,
    $._template_close,
    $._error_sentinel,
  ],

  conflicts: $ => [
    // argument_list vs parameter_list: both match '(' ... ')'
    [$.parameter_list, $.argument_list],
    // identifier in argument/parameter context can be expression or type
    [$.datatype, $._expression],
    // identifier followed by '<' could be datatype + template_type_list or scope_resolution + template
    [$._scope_resolution, $.datatype],
    // scope and types vs scoped identifier conflicts
    [$.scoped_identifier, $.scoped_type],
  ],

  rules: {
    // =========================================================================
    // SCRIPT (top-level)
    // =========================================================================
    script: $ => repeat(choice(
      $.import_declaration,
      $.enum_declaration,
      $.typedef_declaration,
      $.class_declaration,
      $.mixin_declaration,
      $.interface_declaration,
      $.funcdef_declaration,
      $.virtual_property,
      $.variable_declaration,
      $.func_declaration,
      $.namespace_declaration,
      $.using_declaration,
      ";",
    )),

    // =========================================================================
    // IMPORT
    // =========================================================================
    import_declaration: $ => seq(
      "import",
      field("return_type", $.type),
      optional("&"),
      field("name", $.identifier),
      field("parameters", $.parameter_list),
      optional($.func_attributes),
      "from",
      field("source", $.string_literal),
      ";",
    ),

    // =========================================================================
    // USING
    // =========================================================================
    using_declaration: $ => seq(
      "using",
      "namespace",
      choice($.identifier, $.scoped_identifier),
      ";",
    ),

    _scope_resolution: $ => seq(
      optional(choice(
        $.identifier,
        seq($.identifier, $.template_type_list)
      )),
      "::"
    ),

    scoped_identifier: $ => seq(
      $._scope_resolution,
      choice(
        $.identifier,
        $.scoped_identifier
      )
    ),

    // =========================================================================
    // NAMESPACE
    // =========================================================================
    namespace_declaration: $ => seq(
      "namespace",
      field("name", choice($.identifier, $.scoped_identifier)),
      field("body", $.namespace_body),
    ),

    namespace_body: $ => seq(
      "{",
      repeat(choice(
        $.import_declaration,
        $.enum_declaration,
        $.typedef_declaration,
        $.class_declaration,
        $.mixin_declaration,
        $.interface_declaration,
        $.funcdef_declaration,
        $.virtual_property,
        $.variable_declaration,
        $.func_declaration,
        $.namespace_declaration,
        $.using_declaration,
        ";",
      )),
      "}",
    ),

    // =========================================================================
    // ENUM
    // =========================================================================
    enum_declaration: $ => seq(
      repeat(field("modifier", $.declaration_modifier)),
      "enum",
      field("name", $.identifier),
      choice(
        ";",
        seq(
          "{",
          commaSep($.enum_member),
          optional(","),
          "}",
        ),
      ),
    ),

    enum_member: $ => seq(
      field("name", $.identifier),
      optional(seq("=", field("value", $._expression))),
    ),

    // =========================================================================
    // TYPEDEF
    // =========================================================================
    typedef_declaration: $ => seq(
      "typedef",
      field("base_type", choice($.primitive_type, $.identifier)),
      field("name", $.identifier),
      ";",
    ),

    // =========================================================================
    // CLASS
    // =========================================================================
    class_declaration: $ => seq(
      repeat(field("modifier", $.declaration_modifier)),
      "class",
      field("name", $.identifier),
      choice(
        ";",
        seq(
          optional($.base_class_list),
          field("body", $.class_body),
        ),
      ),
    ),

    base_class_list: $ => seq(
      ":",
      commaSep1(field("base", $.identifier)),
    ),

    class_body: $ => seq(
      "{",
      repeat(choice(
        $.virtual_property,
        $.func_declaration,
        $.variable_declaration,
        $.funcdef_declaration,
      )),
      "}",
    ),

    // =========================================================================
    // MIXIN
    // =========================================================================
    mixin_declaration: $ => seq(
      "mixin",
      repeat(field("modifier", $.declaration_modifier)),
      "class",
      field("name", $.identifier),
      choice(
        ";",
        seq(
          optional($.base_class_list),
          field("body", $.class_body),
        ),
      ),
    ),

    // =========================================================================
    // INTERFACE
    // =========================================================================
    interface_declaration: $ => seq(
      repeat(field("modifier", $.declaration_modifier)),
      "interface",
      field("name", $.identifier),
      choice(
        ";",
        seq(
          optional($.base_class_list),
          field("body", $.interface_body),
        ),
      ),
    ),

    interface_body: $ => seq(
      "{",
      repeat(choice(
        $.virtual_property,
        $.interface_method,
      )),
      "}",
    ),

    interface_method: $ => seq(
      field("return_type", $.type),
      optional("&"),
      field("name", $.identifier),
      field("parameters", $.parameter_list),
      optional("const"),
      ";",
    ),

    // =========================================================================
    // FUNCDEF
    // =========================================================================
    funcdef_declaration: $ => seq(
      repeat(field("modifier", $.declaration_modifier)),
      "funcdef",
      field("return_type", $.type),
      optional("&"),
      field("name", $.identifier),
      field("parameters", $.parameter_list),
      ";",
    ),

    // =========================================================================
    // FUNC
    // =========================================================================
    func_declaration: $ => prec.dynamic(2, seq(
      repeat(field("modifier", $.declaration_modifier)),
      optional(choice("private", "protected")),
      optional(
        choice(
          seq(field("return_type", $.type), optional("&")),
          "~",
        ),
      ),
      field("name", $.identifier),
      field("parameters", $.parameter_list),
      optional("const"),
      optional($.func_attributes),
      choice(";", field("body", $.statement_block)),
    )),

    func_attributes: _ => repeat1(
      choice("override", "final", "explicit", "property", "delete"),
    ),

    declaration_modifier: _ => choice("shared", "external", "abstract", "final"),

    // =========================================================================
    // VIRTUAL PROPERTY
    // =========================================================================
    virtual_property: $ => prec.dynamic(3, seq(
      optional(choice("private", "protected")),
      field("prop_type", $.type),
      optional("&"),
      field("name", $.identifier),
      "{",
      repeat($.accessor),
      "}",
    )),

    accessor: $ => seq(
      field("kind", choice("get", "set")),
      optional("const"),
      optional($.func_attributes),
      choice(";", field("body", $.statement_block)),
    ),

    // =========================================================================
    // VAR — supports multiple declarators: int a = 1, b = 2, c;
    // =========================================================================
    variable_declaration: $ => prec.dynamic(1, seq(
      optional(choice("private", "protected")),
      field("var_type", $.type),
      $.variable_declarator,
      repeat(seq(",", $.variable_declarator)),
      ";",
    )),

    variable_declarator: $ => seq(
      field("name", $.identifier),
      optional(choice(
        seq("=", choice($.initializer_list, $._expression)),
        $.argument_list,
      )),
    ),

    // =========================================================================
    // STATEMENT BLOCK (stub — expanded in Step 3)
    // =========================================================================
    statement_block: $ => seq(
      "{",
      repeat(choice(
        $.variable_declaration,
        $._statement,
        $.using_declaration,
      )),
      "}",
    ),

    _statement: $ => choice(
      $.if_statement,
      $.for_statement,
      $.foreach_statement,
      $.while_statement,
      $.do_while_statement,
      $.switch_statement,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.try_statement,
      $.statement_block,
      $.expression_statement,
      ";",
    ),

    expression_statement: $ => seq(
      $._expression,
      ";",
    ),

    return_statement: $ => seq(
      "return",
      optional(choice($.initializer_list, $._expression)),
      ";",
    ),

    // =========================================================================
    // CONTROL FLOW STATEMENTS
    // =========================================================================

    // prec.right resolves dangling-else: else binds to innermost if
    if_statement: $ => prec.right(seq(
      "if", "(", $._expression, ")",
      field("consequence", $._statement),
      optional(seq("else", field("alternative", $._statement))),
    )),

    for_statement: $ => seq(
      "for", "(",
      field("init", choice($.variable_declaration, $.expression_statement, ";")),
      field("condition", choice($.expression_statement, ";")),
      field("update", optional(commaSep1($._expression))),
      ")",
      field("body", $._statement),
    ),

    foreach_statement: $ => seq(
      "foreach", "(",
      commaSep1($.foreach_variable),
      ":",
      field("collection", $._expression),
      ")",
      field("body", $._statement),
    ),

    foreach_variable: $ => seq(
      field("type", $.type),
      field("name", $.identifier),
    ),

    while_statement: $ => seq(
      "while", "(", $._expression, ")",
      field("body", $._statement),
    ),

    do_while_statement: $ => seq(
      "do",
      field("body", $._statement),
      "while", "(", $._expression, ")", ";",
    ),

    switch_statement: $ => seq(
      "switch", "(", $._expression, ")",
      "{", repeat($.case_clause), "}",
    ),

    case_clause: $ => seq(
      choice(seq("case", $._expression), "default"),
      ":",
      repeat(choice($.variable_declaration, $._statement)),
    ),

    break_statement: _ => seq("break", ";"),

    continue_statement: _ => seq("continue", ";"),

    try_statement: $ => seq(
      "try", $.statement_block,
      "catch", $.statement_block,
    ),

    // =========================================================================
    // PARAMETER LIST
    // =========================================================================
    parameter_list: $ => seq(
      "(",
      optional(commaSep1($.parameter)),
      ")",
    ),

    parameter: $ => seq(
      field("param_type", $.type),
      optional(seq("&", optional(choice("in", "out", "inout")))),
      choice(
        "...",
        seq(
          optional(field("name", $.identifier)),
          optional(seq("=", field("default_value", $._expression))),
        ),
      ),
    ),

    // =========================================================================
    // TYPE SYSTEM
    // =========================================================================
    type: $ => seq(
      optional("const"),
      choice(
        seq($.datatype, optional($.template_type_list)),
        $.scoped_type
      ),
      repeat(choice(
        seq("[", "]"),
        seq("@", optional("const")),
      )),
    ),

    scoped_type: $ => seq(
      $._scope_resolution,
      choice(
        $.identifier,
        seq($.identifier, $.template_type_list),
        $.scoped_type
      )
    ),

    template_type_list: $ => seq(
      $._template_open,
      $.type,
      repeat(seq(",", $.type)),
      $._template_close,
    ),

    scope: $ => prec.left(choice(
      "::",
      seq(optional("::"), repeat1(seq($.identifier, optional($.template_type_list), "::"))),
    )),

    datatype: $ => choice(
      $.identifier,
      $.primitive_type,
      "?",
      "auto",
    ),

    // =========================================================================
    // EXPRESSIONS — Full system with correct operator precedence
    //
    // Precedence table (low to high):
    //   1  = += -= *= /= %= **= |= &= ^= <<= >>= >>>=  (right)
    //   2  ?: ternary                                     (right)
    //   3  || or                                          (left)
    //   4  ^^ xor                                        (left)
    //   5  && and                                         (left)
    //   6  | (bitwise OR)                                 (left)
    //   7  ^ (bitwise XOR)                                (left)
    //   8  & (bitwise AND)                                (left)
    //   9  == != is !is                                   (left)
    //  10  < <= > >=                                      (left)
    //  11  << >> >>>                                      (left)
    //  12  + - (binary)                                   (left)
    //  13  * / %                                          (left)
    //  14  ** (exponent)                                  (right)
    //  15  unary prefix: - + ! ~ @ ++ --                  (right)
    //  16  postfix: .member [index] () ++ --              (left)
    // =========================================================================
    _expression: $ => choice(
      $.assignment_expression,
      $.ternary_expression,
      $.binary_expression,
      $.unary_expression,
      $.postfix_expression,
      $.call_expression,
      $.member_expression,
      $.index_expression,
      $.cast_expression,
      $.lambda_expression,
      $.parenthesized_expression,
      $.number_literal,
      $.string_literal,
      $.boolean_literal,
      $.null_literal,
      $.identifier,
      $.scoped_identifier,
    ),

    parenthesized_expression: $ => seq("(", $._expression, ")"),

    // --- Assignment (prec 1, right-associative) ---
    assignment_expression: $ => prec.right(1, seq(
      field("left", $._expression),
      field("operator", choice(
        "=", "+=", "-=", "*=", "/=", "%=", "**=",
        "&=", "|=", "^=", "<<=", ">>=", ">>>=", "@=",
      )),
      field("right", $._expression),
    )),

    // --- Ternary (prec 2, right-associative) ---
    ternary_expression: $ => prec.right(2, seq(
      field("condition", $._expression),
      "?",
      field("consequence", $._expression),
      ":",
      field("alternative", $._expression),
    )),

    // --- Binary operators (prec 3–14) ---
    binary_expression: $ => {
      const table = [
        // prec 3: logical OR
        ["||", 3],
        ["or", 3],
        // prec 4: logical XOR
        ["^^", 4],
        ["xor", 4],
        // prec 5: logical AND
        ["&&", 5],
        ["and", 5],
        // prec 6: bitwise OR
        ["|", 6],
        // prec 7: bitwise XOR
        ["^", 7],
        // prec 8: bitwise AND
        ["&", 8],
        // prec 9: equality / identity
        ["==", 9],
        ["!=", 9],
        ["is", 9],
        ["!is", 9],
        // prec 10: relational
        ["<", 10],
        [">", 10],
        ["<=", 10],
        [">=", 10],
        // prec 11: shift
        ["<<", 11],
        [">>", 11],
        [">>>", 11],
        // prec 12: additive
        ["+", 12],
        ["-", 12],
        // prec 13: multiplicative
        ["*", 13],
        ["/", 13],
        ["%", 13],
      ];

      return choice(
        ...table.map(([op, precedence]) =>
          prec.left(precedence, seq(
            field("left", $._expression),
            field("operator", op),
            field("right", $._expression),
          )),
        ),
        // prec 14: exponentiation (right-associative)
        prec.right(14, seq(
          field("left", $._expression),
          field("operator", "**"),
          field("right", $._expression),
        )),
      );
    },

    // --- Unary prefix (prec 15, right-associative) ---
    // Includes @ (handle-of operator)
    unary_expression: $ => prec.right(15, seq(
      field("operator", choice("-", "+", "!", "~", "@", "++", "--")),
      field("operand", $._expression),
    )),

    // --- Postfix (prec 16, left-associative) ---
    postfix_expression: $ => prec.left(16, seq(
      field("operand", $._expression),
      field("operator", choice("++", "--")),
    )),

    // --- Call expression (prec 16) ---
    // Handles both function calls and constructor calls (e.g. MyType(args)).
    // Constructor vs function distinction is semantic, not syntactic.
    call_expression: $ => prec.left(16, seq(
      field("function", $._expression),
      field("arguments", $.argument_list),
    )),

    // --- Member access (prec 16) ---
    member_expression: $ => prec.left(16, seq(
      field("object", $._expression),
      ".",
      field("member", $.identifier),
    )),

    // --- Index expression with optional named indexing (prec 16) ---
    // Supports: a[0], a[key: value], a[k1: v1, k2: v2]
    index_expression: $ => prec.left(16, seq(
      field("object", $._expression),
      "[",
      commaSep1(seq(
        optional(seq(field("index_name", $.identifier), ":")),
        field("index", $._expression),
      )),
      "]",
    )),

    // --- Cast expression ---
    // Uses external scanner tokens to disambiguate < > from comparison operators
    cast_expression: $ => seq(
      "cast",
      $._template_open,
      field("type", $.type),
      $._template_close,
      "(",
      field("value", $._expression),
      ")",
    ),

    // --- Lambda expression ---
    // Reuses parameter_list to stay DRY with func_declaration
    lambda_expression: $ => seq(
      "function",
      field("parameters", $.parameter_list),
      field("body", $.statement_block),
    ),

    // =========================================================================
    // INITIALIZER LIST & ARGUMENT LIST
    // =========================================================================
    // initializer_list is in _expression, so nesting ({1, {2, 3}}) works
    // automatically without special-casing.
    // initializer_list is NOT in _expression to avoid {}-vs-statement_block
    // ambiguity. It is reachable from variable_declaration and return_statement
    // RHS. Nesting works via choice($.initializer_list, $._expression).
    _initializer_element: $ => choice($.initializer_list, $._expression),

    initializer_list: $ => seq(
      "{",
      commaSep($._initializer_element),
      optional(","),
      "}",
    ),

    // Supports named arguments: foo(arg1: val1, arg2: val2)
    argument_list: $ => seq(
      "(",
      commaSep(seq(
        optional(seq(field("arg_name", $.identifier), ":")),
        $._expression,
      )),
      ")",
    ),

    // =========================================================================
    // LITERALS
    // =========================================================================
    boolean_literal: _ => choice("true", "false"),

    null_literal: _ => "null",

    string_literal: _ => token(choice(
      // Triple-quoted heredoc strings (no escape processing, multiline)
      seq('"""', /([^"]|"[^"]|""[^"])*/, '"""'),
      // Single and double quoted strings with escape sequences
      seq("'", repeat(choice(/[^'\\]/, /\\./)), "'"),
      seq('"', repeat(choice(/[^"\\]/, /\\./)), '"'),
    )),

    number_literal: _ => {
      const hex = /0[xX][0-9a-fA-F]+/;
      const octal = /0[oO][0-7]+/;
      const binary = /0[bB][01]+/;
      const explicit_decimal = /0[dD][0-9]+/;
      const decimal_float = choice(
        /[0-9]+\.[0-9]*([eE][+-]?[0-9]+)?[fFdD]?/,
        /[0-9]*\.[0-9]+([eE][+-]?[0-9]+)?[fFdD]?/,
        /[0-9]+[eE][+-]?[0-9]+[fFdD]?/,
        /[0-9]+[fFdD]/,
      );
      const decimal_int = /[0-9]+/;
      return token(choice(hex, octal, binary, explicit_decimal, decimal_float, decimal_int));
    },

    // =========================================================================
    // PRIMITIVES
    // =========================================================================
    primitive_type: _ => choice(
      "void", "int", "int8", "int16", "int32", "int64", "uint",
      "uint8", "uint16", "uint32", "uint64", "float", "double", "bool",
    ),

    comment: _ => token(choice(
      seq("//", /(\\+(.|\r?\n)|[^\\\n])*/),
      seq("/*", /[^*]*\*+([^/*][^*]*\*+)*/, "/"),
    )),

    // Preprocessor directive: one full line starting with '#', e.g.
    // #include "file.as", #if EDITOR, #ifdef WITH_SERVER, #else, #endif,
    // #pragma, etc.
    preproc_directive: _ => token(seq("#", /[^\r\n]*/)),

    identifier: _ => /[A-Za-z_][A-Za-z0-9_]*/,
  },
});
