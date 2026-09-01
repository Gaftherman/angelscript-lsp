#include "analysis/SemanticAnalyzer.h"
#include "analysis/ASTUtils.h"
#include "analysis/AccessChecker.h"
#include "analysis/CallChecker.h"
#include "analysis/ConstChecker.h"
#include "analysis/ControlFlowChecker.h"
#include "analysis/DefiniteAssignmentChecker.h"
#include "analysis/InitializerListChecker.h"
#include "analysis/IsolationChecker.h"
#include "analysis/LValueChecker.h"
#include "analysis/NamespaceChecker.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeConversionChecker.h"
#include "analysis/rules/ClassRules.h"
#include "analysis/rules/FunctionRules.h"
#include "analysis/rules/OperatorRules.h"
#include "analysis/rules/TypeRules.h"
#include "analysis/rules/VariableRules.h"
#include "spdlog/fmt/fmt.h"

namespace angel_lsp::analysis
{
    SemanticAnalyzer::SemanticAnalyzer(angel_lsp::utils::LspLogger *logger)
        : m_logger(logger)
    {
    }

    std::vector<Diagnostic> SemanticAnalyzer::Analyze(const SemanticAnalysisRequest &request) const
    {
        std::vector<Diagnostic> diagnostics;

        // A dump of the document's own symbols, at Debug because that is what it is - the client's
        // log is not the place to narrate every declaration on every keystroke. Read through
        // ForEachSymbolInFile for the same reason the rules are: walking the whole workspace to
        // print one file's symbols was the last full-table walk left in the analysis path.
        // Gated on the level, not merely on the logger existing. m_logger is never null in the
        // server, and LspLogger had no threshold at all, so this loop formatted and sent one
        // window/logMessage notification per symbol in the file on every analysis - taking the log
        // mutex and then the connection's write mutex each time, contending with the message loop's
        // own responses. The check has to be here rather than inside LogDebug: passing an already
        // built string still pays for fmt::format.
        if (m_logger && m_logger->IsDebugEnabled())
        {
            m_logger->LogDebug(fmt::format("=== [SYMBOL COLLECTOR OUTPUT] Document: {} ===", request.fileUri));
            request.symbolTable.ForEachSymbolInFile(
                request.fileUri,
                [&](const std::string & /*qualifiedName*/, const std::vector<Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                    {
                        if (sym.fileUri == request.fileUri)
                        {
                            m_logger->LogDebug(fmt::format("  -> Symbol: [{}] Name: \"{}\" | Container: \"{}\" | Range: L{}:C{}-L{}:C{}",
                                SymbolTypeToString(sym.type),
                                sym.name,
                                sym.containerName,
                                sym.startLine + 1,
                                sym.startCharacter + 1,
                                sym.endLine + 1,
                                sym.endCharacter + 1));
                        }
                    }
                });
        }

        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            CheckNullAssignedToNonHandle(request.symbolTable, ctx);
            CheckDeclarationRules(request.symbolTable, ctx);
        }

        if (request.scopeRoot)
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};

            // Taken from the version-cached index rather than rebuilt here. The set has to hold
            // every name in the workspace for this rule to be right, but it does not have to be
            // built afresh for each document - which is what it was, one full walk and fifty
            // thousand insertions per analysis.
            CheckUndefinedIdentifiers(request.scopeRoot.get(), request.GetRuleIndex().allNames, ctx);

            ankerl::unordered_dense::set<const LocalDefinition *> used;
            CollectUsedDefinitions(request.scopeRoot.get(), used);
            CheckUnusedVariables(request.scopeRoot.get(), used, ctx);

            CheckNullAssignedToNonHandleInScope(request.scopeRoot.get(), ctx);
            CheckLocalVariableDeclarations(request.scopeRoot.get(), ctx);
        }

        // Statements, not declarations: whether a break sits inside a loop or a path falls off the
        // end of a function is nowhere in the symbol table.
        if (request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            const ControlFlowCheckRequest flowRequest{ts_tree_root_node(request.tree), request.sourceCode};
            CheckControlFlow(flowRequest, ctx);
        }

        // The one pass that judges a use rather than a declaration, so it needs both the tree that
        // holds the expression and the table that holds what the expression reaches.
        if (request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            const AccessCheckRequest accessRequest{
                ts_tree_root_node(request.tree),
                request.sourceCode,
                request.scopeRoot.get()
            };
            CheckMemberAccess(accessRequest, ctx);
        }

        if (request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            const ConstCheckRequest constRequest{
                ts_tree_root_node(request.tree),
                request.sourceCode,
                request.scopeRoot.get()
            };
            CheckConstCorrectness(constRequest, ctx);
        }

        if (request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            const LValueCheckRequest lvalueRequest{
                ts_tree_root_node(request.tree),
                request.sourceCode,
                request.scopeRoot.get()
            };
            CheckLValues(lvalueRequest, ctx);
        }

        if (request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            const CallCheckRequest callRequest{
                ts_tree_root_node(request.tree),
                request.sourceCode,
                request.scopeRoot.get()
            };
            CheckCallArguments(callRequest, ctx);
        }

        if (request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            const DefiniteAssignmentCheckRequest assignRequest{
                ts_tree_root_node(request.tree),
                request.sourceCode,
                request.scopeRoot.get()
            };
            CheckDefiniteAssignment(assignRequest, ctx);
            CheckEngineDialectRules(ts_tree_root_node(request.tree), ctx);
        }

        // Needs the tree, not just the symbol table: an initializer or a cast is an expression, and
        // expressions are exactly what the symbol table does not record.
        if (request.enableTypeConversionChecks && request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            const TypeConversionCheckRequest conversionRequest{
                ts_tree_root_node(request.tree),
                request.sourceCode,
                request.scopeRoot.get(),
                request.mutableScopeRoot
            };
            CheckTypeConversions(conversionRequest, ctx);
        }

        if (request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            const IsolationCheckRequest isolationRequest{
                ts_tree_root_node(request.tree),
                request.sourceCode,
                request.scopeRoot.get()
            };
            CheckSharedIsolation(isolationRequest, ctx);
        }

        if (request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            CheckNamespacesAndScopes(NamespaceCheckRequest{ ts_tree_root_node(request.tree), request.sourceCode }, ctx);
        }

        if (request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            CheckInitializerLists(InitializerListCheckRequest{ ts_tree_root_node(request.tree), request.sourceCode,
                                                              request.scopeRoot.get() }, ctx);
        }

        // Nothing inside an excluded `#if` block is real code - CScriptBuilder blanks it out before
        // the compiler ever sees it - so a diagnostic there describes text that does not exist.
        // Filtered here, at the single exit, rather than in each rule.
        if (!request.excludedLineRanges.empty())
        {
            std::erase_if(diagnostics, [&request](const Diagnostic &d)
                          { return utils::IsLineExcluded(request.excludedLineRanges, d.range.start.line); });
        }

        return diagnostics;
    }

    void SemanticAnalyzer::CheckEngineDialectRules(TSNode node, DiagnosticContext &ctx, int depth) const
    {
        if (ts_node_is_null(node) || depth > k_maxAstDepth)
        {
            return;
        }

        const std::string_view engineNodeType = ts_node_type(node);

        // `foreach (T v : c)` is a compile error - "Expected '('" at the loop variable - when the
        // host built its engine with asEP_FOREACH_SUPPORT off. On by the engine's default, so this
        // fires only for a host that says otherwise.
        if (engineNodeType == "foreach_statement" && !ctx.request.SupportsForeach())
        {
            const TSPoint start = ts_node_start_point(node);
            ctx.EmitAtRange(start.row, start.column, start.row, start.column + 7,
                            "as-err-foreach-unsupported", DiagnosticSeverity::Error);
        }

        // A hole in an initializer list - `{1, , 3}` - which asEP_DISALLOW_EMPTY_LIST_ELEMENTS
        // rejects with "Empty list element is not allowed". Allowed by the engine's default, so
        // again this speaks only for a host that turned it off.
        if (engineNodeType == "initializer_list" && ctx.request.DisallowsEmptyListElements())
        {
            const uint32_t childCount = ts_node_child_count(node);
            for (uint32_t i = 1; i < childCount; ++i)
            {
                // Two commas in a row, or a comma immediately before the closing brace: either way
                // the element between them is missing. Read off the punctuation rather than the
                // named children, because the missing element has no node to find.
                const std::string_view previous = ts_node_type(ts_node_child(node, i - 1));
                const std::string_view current = ts_node_type(ts_node_child(node, i));
                if (previous == "," && (current == "," || current == "}"))
                {
                    const TSPoint at = ts_node_start_point(ts_node_child(node, i - 1));
                    ctx.EmitAtRange(at.row, at.column, at.row, at.column + 1,
                                    "as-err-empty-list-element", DiagnosticSeverity::Error);
                }
            }
        }

        // `'x'` is a one-character STRING under the engine's default and an integer only when the
        // host sets asEP_USE_CHARACTER_LITERALS. So `int c = 'x';` is rejected by default - "Can't
        // implicitly convert from 'const string' to 'int'" - and legal for a host that set it.
        // Verified both ways against angelscript_oracle.
        //
        // Read at the declaration rather than at the literal: the literal alone says nothing about
        // which reading was intended, and the declared type is what makes the two distinguishable.
        if (engineNodeType == "variable_declaration" && ctx.request.CharacterLiteralMode() == 0)
        {
            const TSNode typeNode = ts_node_child_by_field_name(node, "var_type", 8);
            if (!ts_node_is_null(typeNode))
            {
                const std::string declared = CleanBaseType(GetNodeText(typeNode, ctx.request.sourceCode));
                if (IsPrimitiveTypeName(declared) && declared != "auto" && declared != "void")
                {
                    const uint32_t declaratorCount = ts_node_child_count(node);
                    for (uint32_t i = 0; i < declaratorCount; ++i)
                    {
                        const TSNode declarator = ts_node_child(node, i);
                        if (std::string_view(ts_node_type(declarator)) != "variable_declarator")
                            continue;

                        const TSNode value = ts_node_child_by_field_name(declarator, "value", 5);
                        if (ts_node_is_null(value) ||
                            std::string_view(ts_node_type(value)) != "string_literal")
                            continue;

                        const uint32_t from = ts_node_start_byte(value);
                        // The opening delimiter is what separates 'x' from "x": the grammar gives
                        // both the same string_literal node type.
                        if (from < ctx.request.sourceCode.size() && ctx.request.sourceCode[from] == '\'')
                        {
                            const TSPoint start = ts_node_start_point(value);
                            const TSPoint end = ts_node_end_point(value);
                            ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                            "as-err-character-literal-is-string", declared,
                                            DiagnosticSeverity::Error);
                        }
                    }
                }
            }
        }

        // Integer division under asEP_DISABLE_INTEGER_DIVISION. Not a compile error either way -
        // all three probes compile - but the VALUE differs: `float f = 1 / 2;` is 0.0 by the
        // engine's default and 0.5 when the host disables integer division. That is the classic
        // `1/2 == 0` surprise, and here it is configuration-dependent, so it is worth a hint and
        // not worth an error.
        //
        // Only when the host has NOT disabled integer division, because only then does the
        // truncation happen; and only opt-in, because a codebase that means integer division
        // writes exactly this and wants no comment on it.
        if (engineNodeType == "binary_expression" &&
            ctx.request.diagnostics && ctx.request.diagnostics->reportIntegerDivision &&
            !ctx.request.DisablesIntegerDivision())
        {
            const TSNode op = ts_node_child_by_field_name(node, "operator", 8);
            if (!ts_node_is_null(op) && std::string_view(ts_node_type(op)) == "/")
            {
                const TSNode left = ts_node_child_by_field_name(node, "left", 4);
                const TSNode right = ts_node_child_by_field_name(node, "right", 5);

                // Integer LITERALS on both sides. Deliberately not variables: their types would have
                // to be resolved, and a false hint here is noise on every division in the file.
                const auto isIntegerLiteral = [&ctx](TSNode candidate)
                {
                    if (ts_node_is_null(candidate) ||
                        std::string_view(ts_node_type(candidate)) != "number_literal")
                        return false;
                    const std::string text = GetNodeText(candidate, ctx.request.sourceCode);
                    return text.find('.') == std::string::npos &&
                           text.find('e') == std::string::npos &&
                           text.find('E') == std::string::npos &&
                           text.find('f') == std::string::npos &&
                           text.find('F') == std::string::npos;
                };

                if (isIntegerLiteral(left) && isIntegerLiteral(right))
                {
                    const TSPoint start = ts_node_start_point(node);
                    const TSPoint end = ts_node_end_point(node);
                    ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                    "as-hint-integer-division", DiagnosticSeverity::Hint);
                }
            }
        }

        // `f(name = value)` under asEP_ALTER_SYNTAX_NAMED_ARGS. AngelScript's own named-argument
        // syntax is `name: value`; the `=` spelling is a compile error under the engine's default
        // ("No matching symbol 'width'"), a warning under mode 1 and silent under mode 2. Measured
        // all three ways against angelscript_oracle.
        //
        // Gated on the engine mode alone, with no separate opt-in: unlike the integer-division
        // hint, this is not advice about legal code - under mode 0 it does not compile.
        if (engineNodeType == "argument_list" && ctx.request.NamedArgumentSyntaxMode() != 2)
        {
            const uint32_t argCount = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < argCount; ++i)
            {
                const TSNode argument = ts_node_named_child(node, i);
                if (std::string_view(ts_node_type(argument)) != "assignment_expression")
                    continue;

                // Only a bare name on the left. `f(obj.field = 1)` is an ordinary assignment
                // expression and a legal argument; a lone name is what a named argument looks like.
                //
                // `scoped_identifier` as well as `identifier`, because the grammar wraps every bare
                // identifier expression in one - matching only the inner node found nothing, which
                // is how the first version of this rule silently never fired.
                const TSNode target = ts_node_child_by_field_name(argument, "left", 4);
                if (ts_node_is_null(target))
                    continue;
                const std::string_view targetType = ts_node_type(target);
                if (targetType != "identifier" && targetType != "scoped_identifier")
                    continue;
                // A qualified name is not a parameter name.
                if (targetType == "scoped_identifier" && ts_node_named_child_count(target) != 1)
                    continue;

                const TSPoint start = ts_node_start_point(argument);
                const TSPoint end = ts_node_end_point(argument);
                const std::string name = GetNodeText(target, ctx.request.sourceCode);

                ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                "as-err-named-argument-syntax", name,
                                ctx.request.NamedArgumentSyntaxMode() == 1 ? DiagnosticSeverity::Warning
                                                                           : DiagnosticSeverity::Error);
            }
        }

        // `a = b` on a reference type under asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE, which the
        // engine answers with "Value assignment on reference types is not allowed. Did you mean to
        // do a handle assignment?". Off by the engine's default, so this speaks only for a host
        // that turned it on.
        if (engineNodeType == "assignment_expression" && ctx.request.DisallowsValueAssignForRef())
        {
            const TSNode op = ts_node_child_by_field_name(node, "operator", 8);
            const TSNode target = ts_node_child_by_field_name(node, "left", 4);

            // Every compound operator is arithmetic on a value, so only a bare `=` is a candidate.
            if (!ts_node_is_null(op) && std::string_view(ts_node_type(op)) == "=" &&
                !ts_node_is_null(target))
            {
                // `@a = @b` is the handle assignment the compiler's own message asks for, and it is
                // accepted under the property - verified against the oracle. Its operator is also a
                // bare `=`; the `@` is a unary prefix on each side, which is the only thing telling
                // the two forms apart. Matching on the operator alone reported the very fix the
                // diagnostic recommends.
                bool isHandleAssignment = false;
                if (std::string_view(ts_node_type(target)) == "unary_expression")
                {
                    const TSNode prefix = ts_node_child_by_field_name(target, "operator", 8);
                    isHandleAssignment = !ts_node_is_null(prefix) &&
                                         std::string_view(ts_node_type(prefix)) == "@";
                }
                // The TYPE of the assignment target, not its text. Reading the text compared the
                // variable's NAME against the symbol table, which never matches a local - the first
                // version of this rule was silent for exactly that reason.
                const Scope *scope = ctx.request.scopeRoot
                                         ? FindInnermostScope(ctx.request.scopeRoot.get(),
                                                              ts_node_start_point(target).row,
                                                              ts_node_start_point(target).column)
                                         : nullptr;
                const std::string targetType = CleanBaseType(ResolveExpressionType(
                    target, scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.fileUri));

                // Silent unless fully visible: only a class this analyzer can find the declaration
                // of is known to be a reference type. A host type it cannot see might be a value
                // type, and reporting one would be a false positive on working code.
                bool isVisibleClass = false;
                if (const auto symbols = ctx.request.symbolTable.FindSymbolsPtr(targetType))
                {
                    for (const auto &sym : *symbols)
                    {
                        if (sym.type == SymbolType::Class)
                        {
                            isVisibleClass = true;
                            break;
                        }
                    }
                }

                if (isVisibleClass && !isHandleAssignment)
                {
                    const TSPoint start = ts_node_start_point(node);
                    const TSPoint end = ts_node_end_point(node);
                    ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                    "as-err-value-assign-for-ref", targetType, DiagnosticSeverity::Error);
                }
            }
        }

        if (engineNodeType == "string_literal")
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);

            if (end.row > start.row && !ctx.request.AllowsMultilineStrings())
            {
                // A heredoc spans lines under every setting; only the plain quote form is governed
                // by asEP_ALLOW_MULTILINE_STRINGS. The grammar gives both the same node type, so
                // the opening delimiter is what tells them apart.
                const uint32_t from = ts_node_start_byte(node);
                const bool isHeredoc = from + 3 <= ctx.request.sourceCode.size() &&
                                       ctx.request.sourceCode.compare(from, 3, "\"\"\"") == 0;

                if (!isHeredoc)
                {
                    // Anchored to the opening quote rather than the whole literal: a string running
                    // away over twenty lines would otherwise underline all twenty, and the defect is
                    // at the quote that was never closed.
                    ctx.EmitAtRange(start.row, start.column, start.row, start.column + 1,
                                    "as-err-multiline-string", DiagnosticSeverity::Error);
                }
            }

            // Nothing inside a string literal is worth walking.
            return;
        }

        const uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            CheckEngineDialectRules(ts_node_child(node, i), ctx, depth + 1);
        }
    }

    void SemanticAnalyzer::CheckDeclarationRules(const SymbolTable &symbolTable, DiagnosticContext &ctx) const
    {
        // Only the buckets this document touches. Every rule below either filters to the analysed
        // file or, in ValidateDuplicates' case, needs the whole bucket - which it still gets. The
        // rest of the workspace's fifty thousand symbols have nothing to contribute here.
        symbolTable.ForEachSymbolInFile(
            ctx.request.fileUri,
            [&](const std::string &, const std::vector<Symbol> &symbols)
            {
                // Whether a name is redeclared is a property of the whole overload bucket, so this
                // one is handed the set rather than each member of it.
                rules::ValidateDuplicates(symbols, ctx);

                for (const auto &sym : symbols)
                {
                    // Only the document under analysis is reported on. Its module's other files are
                    // indexed alongside it so their declarations resolve, but diagnosing them here
                    // would attach findings to files the user did not open.
                    if (sym.fileUri != ctx.request.fileUri)
                    {
                        continue;
                    }

                    switch (sym.type)
                    {
                    case SymbolType::Class:
                        rules::ValidateClass(sym, ctx);
                        break;
                    case SymbolType::Interface:
                        rules::ValidateClass(sym, ctx);
                        rules::ValidateInterfaceMembers(sym, ctx);
                        break;
                    case SymbolType::Typedef:
                        rules::ValidateTypedef(sym, ctx);
                        break;
                    case SymbolType::Function:
                        rules::ValidateFunction(sym, ctx);
                        rules::ValidateOperator(sym, ctx);
                        break;
                    case SymbolType::Funcdef:
                        rules::ValidateFuncdef(sym, ctx);
                        // A funcdef's parameter list obeys the same rules as a function's, minus
                        // everything that presumes a body or a container.
                        rules::ValidateParameters(sym, sym.GetFuncdef().parameters, true, ctx);
                        break;
                    case SymbolType::Enum:
                        rules::ValidateEnum(sym, ctx);
                        break;
                    case SymbolType::Variable:
                    case SymbolType::Property:
                        rules::ValidateVariable(sym, ctx);
                        break;
                    default:
                        break;
                    }
                }
            });
    }

    void SemanticAnalyzer::CheckUndefinedIdentifiers(const Scope *scope, const ankerl::unordered_dense::set<std::string> &knownGlobalNames, DiagnosticContext &ctx, int depth) const
    {
        // Scope trees nest as deeply as the source blocks do; see k_maxAstDepth in ASTUtils.h.
        if (depth > k_maxAstDepth)
            return;


        std::vector<std::pair<uint32_t, uint32_t>> mixinRanges;
        ctx.request.symbolTable.ForEachSymbolInFile(ctx.request.fileUri, [&](const std::string &, const std::vector<Symbol> &symbols) {
            for (const auto &sym : symbols)
            {
                if (sym.type == SymbolType::Class && sym.GetClass().modifiers.isMixin)
                {
                    mixinRanges.push_back({ sym.startLine, sym.endLine });
                }
            }
        });

        for (const auto &ref : scope->references)
        {
            if (ref.isMemberAccess || ref.isTypeSpecifier)
                continue;

            if (ref.name == "this" || ref.name == "value")
                continue;

            // `super` names no symbol, by design. In `class D : B { D() { super(1); } }` it is the
            // base-constructor call and the code compiles; anywhere else - `super.F()`,
            // `super::F()` - the compiler agrees with this rule and reports it. So the shape is
            // tested, not the name. NamespaceChecker makes the same distinction for the error it
            // raises on the same line.
            if (ref.name == "super")
            {
                if (ctx.request.tree)
                {
                    const TSPoint at{ ref.startLine, ref.startCharacter };
                    const TSNode node = ts_node_descendant_for_point_range(
                        ts_tree_root_node(ctx.request.tree), at, at);
                    if (IsBaseConstructorCall(node, ctx.request.sourceCode))
                        continue;
                }
                else
                {
                    // No tree to ask. Staying silent is the policy when the analyzer cannot see
                    // enough to be sure.
                    continue;
                }
            }

            bool isInsideMixin = false;
            for (const auto &r : mixinRanges)
            {
                if (ref.startLine >= r.first && ref.endLine <= r.second)
                {
                    isInsideMixin = true;
                    break;
                }
            }
            if (isInsideMixin)
                continue;

            if (ResolveInScope(scope, ref.name) != nullptr)
                continue;

            if (knownGlobalNames.contains(ref.name))
                continue;

            // A virtual property is reached by a name nothing declares: the member behind `Up` is
            // `C::get_Up`, so the table has never heard of `Up`. Which accessors count is the
            // engine's asEP_PROPERTY_ACCESSOR_MODE - 2 takes any get_/set_ member, 3 (the engine's
            // own default) only one carrying the `property` keyword. This server defaults to 2; see
            // EngineProperties::propertyAccessorMode for why it does not follow the engine here.
            //
            // Registered workspace-wide rather than per class, the same trade RuleIndex makes for
            // template parameters: a bare `Up` in a class that has no such accessor goes unreported,
            // which costs nothing, where the alternative was reporting every legal use of one.
            {
                const auto &index = ctx.request.GetRuleIndex();
                const auto &accessorNames = ctx.request.RequiresAccessorKeyword()
                                                ? index.keywordAccessorPropertyNames
                                                : index.accessorPropertyNames;
                if (accessorNames.contains(ref.name))
                    continue;
            }

            ctx.EmitAtRange(ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter,
                             "as-err-undeclared-identifier", ref.name, DiagnosticSeverity::Warning);
        }

        for (const auto &child : scope->children)
            CheckUndefinedIdentifiers(child.get(), knownGlobalNames, ctx, depth + 1);
    }

    void SemanticAnalyzer::CollectUsedDefinitions(const Scope *scope, ankerl::unordered_dense::set<const LocalDefinition *> &used, int depth) const
    {
        // Scope trees nest as deeply as the source blocks do; see k_maxAstDepth in ASTUtils.h.
        if (depth > k_maxAstDepth)
            return;


        for (const auto &ref : scope->references)
        {
            if (ref.isMemberAccess)
                continue;

            const LocalDefinition *def = ResolveInScope(scope, ref.name);
            if (!def)
                continue;

            bool isSelfOccurrence = ref.startLine == def->startLine && ref.startCharacter == def->startCharacter
                                  && ref.endLine == def->endLine && ref.endCharacter == def->endCharacter;
            if (isSelfOccurrence)
                continue;

            used.insert(def);
        }

        for (const auto &child : scope->children)
            CollectUsedDefinitions(child.get(), used, depth + 1);
    }

    void SemanticAnalyzer::CheckUnusedVariables(const Scope *scope, const ankerl::unordered_dense::set<const LocalDefinition *> &used, DiagnosticContext &ctx, int depth) const
    {
        // Scope trees nest as deeply as the source blocks do; see k_maxAstDepth in ASTUtils.h.
        if (depth > k_maxAstDepth)
            return;


        // A Variable-kind definition only counts as a true local (as opposed to a module/
        // namespace-scope global, which LOCALS_QUERY's @local.definition.var captures under the
        // identical kind - its own comment says "Variables (locals and globals)") if it or some
        // ancestor scope was opened by a function/lambda. Globals need cross-file visibility
        // (another file in the same workspace, or the engine itself, may reference them) that a
        // single document's Scope tree can never see - confirmed by corpus spot-check, where
        // module-level declarations like "string WPN_NAME = ...;" were being flagged "unused"
        // even though "unused" isn't decidable from this file alone for those.
        bool isFunctionNested = false;
        for (const Scope *ancestor = scope; ancestor != nullptr; ancestor = ancestor->parent)
        {
            if (ancestor->isFunctionScope)
            {
                isFunctionNested = true;
                break;
            }
        }

        if (isFunctionNested)
        {
            for (const auto &def : scope->definitions)
            {
                if (def.kind != LocalDefinitionKind::Variable)
                    continue;

                if (used.contains(&def))
                    continue;

                ctx.EmitAtRange(def.startLine, def.startCharacter, def.endLine, def.endCharacter,
                                 "as-warn-unused-variable", def.name, DiagnosticSeverity::Warning);
            }
        }

        for (const auto &child : scope->children)
            CheckUnusedVariables(child.get(), used, ctx, depth + 1);
    }

    namespace
    {
        /**
         * @brief True only for the VM-intrinsic numeric/bool kinds, where null is unconditionally
         *        invalid under any AngelScript engine configuration - unlike a class/object type
         *        (TypeKind::Object/Unknown/String/...), which could be app-registered with a
         *        converting constructor or opAssign(int) that legitimately accepts null (e.g. this
         *        corpus's SvenCoop "EHandle eView = null;" - confirmed by corpus audit and by
         *        reading the real declaration: EHandle(pView) is a registered converting
         *        constructor, invisible to this analyzer since no predefined stub exists for it).
         */
        bool IsUnconditionallyNonNullablePrimitive(TypeKind kind)
        {
            switch (kind)
            {
                case TypeKind::Int8: case TypeKind::Int16: case TypeKind::Int32: case TypeKind::Int64:
                case TypeKind::UInt16: case TypeKind::UInt32: case TypeKind::UInt64:
                case TypeKind::Double: case TypeKind::Bool:
                    return true;
                default:
                    return false;
            }
        }
    }

    void SemanticAnalyzer::CheckNullAssignedToNonHandle(const SymbolTable &symbolTable, DiagnosticContext &ctx) const
    {
        symbolTable.ForEachSymbolInFile(
            ctx.request.fileUri,
            [&](const std::string &, const std::vector<Symbol> &symbols)
            {
                for (const auto &sym : symbols)
                {
                    if (sym.fileUri != ctx.request.fileUri)
                        continue;

                    if (!std::holds_alternative<VariableSignature>(sym.signature))
                        continue;

                    const VariableSignature &varSig = sym.GetVariable();
                    if (varSig.isVirtualProperty || !varSig.hasNullInitializer || varSig.modifiers.isHandle)
                        continue;

                    if (!IsUnconditionallyNonNullablePrimitive(varSig.typeKind))
                        continue;

                    ctx.Emit(sym, "as-err-null-non-handle", varSig.typeName, DiagnosticSeverity::Warning);
                }
            });
    }

    void SemanticAnalyzer::CheckNullAssignedToNonHandleInScope(const Scope *scope, DiagnosticContext &ctx, int depth) const
    {
        // Scope trees nest as deeply as the source blocks do; see k_maxAstDepth in ASTUtils.h.
        if (depth > k_maxAstDepth)
            return;


        // Same isFunctionScope ancestor-walk as CheckUnusedVariables: a Variable-kind definition
        // only counts as a true function-body local (not a module/namespace/class-scope global or
        // field, which LOCALS_QUERY's @local.definition.var captures under the identical kind) if
        // it or some ancestor scope was opened by a function/lambda. Module/class-scope
        // declarations are already covered by CheckNullAssignedToNonHandle via SymbolTable.
        bool isFunctionNested = false;
        for (const Scope *ancestor = scope; ancestor != nullptr; ancestor = ancestor->parent)
        {
            if (ancestor->isFunctionScope)
            {
                isFunctionNested = true;
                break;
            }
        }

        if (isFunctionNested)
        {
            for (const auto &def : scope->definitions)
            {
                if (def.kind != LocalDefinitionKind::Variable)
                    continue;

                if (!def.hasNullInitializer || def.isHandleType)
                    continue;

                if (!IsUnconditionallyNonNullablePrimitive(def.typeKind))
                    continue;

                ctx.EmitAtRange(def.startLine, def.startCharacter, def.endLine, def.endCharacter,
                                 "as-err-null-non-handle", def.typeName, DiagnosticSeverity::Warning);
            }
        }

        for (const auto &child : scope->children)
            CheckNullAssignedToNonHandleInScope(child.get(), ctx, depth + 1);
    }

    void SemanticAnalyzer::CheckLocalVariableDeclarations(const Scope *scope, DiagnosticContext &ctx, int depth) const
    {
        // Scope trees nest as deeply as the source blocks do; see k_maxAstDepth in ASTUtils.h.
        if (depth > k_maxAstDepth)
            return;


        if (!scope)
        {
            return;
        }

        bool isFunctionNested = false;
        for (const Scope *ancestor = scope; ancestor != nullptr; ancestor = ancestor->parent)
        {
            if (ancestor->isFunctionScope)
            {
                isFunctionNested = true;
                break;
            }
        }

        if (isFunctionNested)
        {
            for (const auto &def : scope->definitions)
            {
                if (def.kind == LocalDefinitionKind::Variable)
                {
                    uint32_t tStartLine = (def.typeEndCharacter > def.typeStartCharacter || def.typeEndLine > def.typeStartLine) ? def.typeStartLine : def.startLine;
                    uint32_t tStartChar = (def.typeEndCharacter > def.typeStartCharacter || def.typeEndLine > def.typeStartLine) ? def.typeStartCharacter : def.startCharacter;
                    uint32_t tEndLine = (def.typeEndCharacter > def.typeStartCharacter || def.typeEndLine > def.typeStartLine) ? def.typeEndLine : def.endLine;
                    uint32_t tEndChar = (def.typeEndCharacter > def.typeStartCharacter || def.typeEndLine > def.typeStartLine) ? def.typeEndCharacter : def.endCharacter;

                    std::string base = CleanBaseType(def.typeName);
                    if (def.typeName == "void" || base == "void")
                    {
                        ctx.EmitAtRange(tStartLine, tStartChar, tEndLine, tEndChar,
                                        "as-err-void-variable", def.name, DiagnosticSeverity::Error);
                    }
                    else if (IsMixinClass(base, ctx.request.symbolTable))
                    {
                        ctx.EmitAtRange(tStartLine, tStartChar, tEndLine, tEndChar,
                                        "as-err-mixin-not-a-type", base, DiagnosticSeverity::Error);
                    }
                    // A type position naming a FUNCTION. `void Foo(int) {}` then `Foo@ h = @Foo;`
                    // is rejected - "Identifier 'Foo' is not a data type", verified against the
                    // oracle - because a function handle needs a funcdef to name its signature. The
                    // intent is unmistakable and the funcdef is derivable from the function itself,
                    // which is what makes this worth saying rather than leaving as silence.
                    //
                    // Opt-in and a Hint: the name could equally belong to a host type this analyzer
                    // cannot see, and a workspace whose engine registers one would otherwise be told
                    // its own type does not exist.
                    else if (ctx.request.diagnostics && ctx.request.diagnostics->reportMissingFuncdef &&
                             base != "auto" && NamesAFunctionNotAType(base, ctx.request.symbolTable))
                    {
                        ctx.EmitAtRange(tStartLine, tStartChar, tEndLine, tEndChar,
                                        "as-hint-funcdef-missing", base, DiagnosticSeverity::Hint);
                    }
                    // `auto` is in IsCorePrimitive's list, and it is not a primitive - it is not a
                    // type at all, but a placeholder for whatever the initializer produces, so
                    // whether a handle is allowed is decided by *that* type. The compiler accepts
                    // `auto@ g = MakeFoo();` and rejects `int@ x;`, and this reported both. Found
                    // by the corpus audit: two of the 1,061 scripts declare a deduced handle, and
                    // both were flagged on code that compiles.
                    // tests/parity/doc_p22_auto_handle.as, doc_r24_handle_on_primitive.as.
                    else if (def.isHandleType && def.typeKind != TypeKind::Array &&
                             base != "auto" && IsPrimitiveTypeName(base))
                    {
                        ctx.EmitAtRange(tStartLine, tStartChar, tEndLine, tEndChar,
                                        "as-err-handle-on-primitive", base, DiagnosticSeverity::Error);
                    }
                    else
                    {
                        TemplateTypeInfo tmplInfo = ParseTemplateType(def.typeName);
                        if (!tmplInfo.templateArgs.empty())
                        {
                            if (!IsKnownType(tmplInfo.containerName, ctx))
                            {
                                ctx.EmitAtRange(tStartLine, tStartChar, tEndLine, tEndChar,
                                                "as-err-unresolved-type", tmplInfo.containerName, DiagnosticSeverity::Error);
                            }
                            for (size_t i = 0; i < tmplInfo.templateArgs.size(); ++i)
                            {
                                std::string cleanArg = CleanBaseType(tmplInfo.templateArgs[i]);
                                if (!IsKnownType(cleanArg, ctx))
                                {
                                    uint32_t sLine = tStartLine;
                                    uint32_t sChar = tStartChar;
                                    uint32_t eLine = tEndLine;
                                    uint32_t eChar = tEndChar;

                                    if (i < def.templateArgPositions.size())
                                    {
                                        sLine = def.templateArgPositions[i].startLine;
                                        sChar = def.templateArgPositions[i].startCharacter;
                                        eLine = def.templateArgPositions[i].endLine;
                                        eChar = def.templateArgPositions[i].endCharacter;
                                    }

                                    ctx.EmitAtRange(sLine, sChar, eLine, eChar,
                                                    "as-err-unresolved-type", cleanArg, DiagnosticSeverity::Error);
                                }
                            }
                        }
                        else if (!base.empty() && base != "auto" && !IsKnownType(base, ctx))
                        {
                            ctx.EmitAtRange(tStartLine, tStartChar, tEndLine, tEndChar,
                                            "as-err-unresolved-type", base, DiagnosticSeverity::Error);
                        }
                    }
                }
            }
        }

        for (const auto &child : scope->children)
        {
            CheckLocalVariableDeclarations(child.get(), ctx, depth + 1);
        }
    }
}
