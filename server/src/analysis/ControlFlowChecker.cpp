#include "analysis/ControlFlowChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/SemanticHelpers.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <optional>
#include <string_view>
#include <vector>
#include "parser/Primitives.h"

namespace angel_lsp::analysis
{
    namespace
    {
        constexpr uint32_t k_bodyFieldLength = 4;         ///< "body"
        constexpr uint32_t k_nameFieldLength = 4;         ///< "name"
        constexpr uint32_t k_conditionFieldLength = 9;    ///< "condition"
        constexpr uint32_t k_returnTypeFieldLength = 11;  ///< "return_type"
        constexpr uint32_t k_alternativeFieldLength = 11; ///< "alternative"
        constexpr uint32_t k_consequenceFieldLength = 11; ///< "consequence"

        std::string_view NodeType(TSNode node)
        {
            return ts_node_is_null(node) ? std::string_view{} : std::string_view(ts_node_type(node));
        }

        std::string_view NodeText(TSNode node, std::string_view sourceCode)
        {
            if (ts_node_is_null(node))
            {
                return {};
            }
            const uint32_t start = ts_node_start_byte(node);
            const uint32_t end = ts_node_end_byte(node);
            if (start >= end || end > sourceCode.size())
            {
                return {};
            }
            return sourceCode.substr(start, end - start);
        }

        std::string_view Trim(std::string_view text)
        {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            {
                text.remove_suffix(1);
            }
            return text;
        }

        /**
         * @brief The keyword a switch clause opens with: "case", "default", or empty if malformed.
         *
         * Read as a node type rather than by matching the clause's source text. The grammar makes
         * both keywords their own anonymous token, so the type is exact; the text is not. Anything
         * the parser steps over on the way to the keyword - a fall-through comment written just
         * before `default:`, say - shifts the text, and the old test then answered "case" for a
         * default clause. That silenced the default-must-be-last rule on exactly the switch
         * statements most likely to carry the bug.
         */
        std::string_view ClauseKeyword(TSNode clause)
        {
            return NodeType(ts_node_child(clause, 0));
        }

        /** @brief True when the clause is spelled `default:` rather than `case <expr>:`. */
        bool IsDefaultClause(TSNode clause)
        {
            return ClauseKeyword(clause) == "default";
        }

        /**
         * @brief Index of the clause's first statement among its named children.
         *
         * A `case` clause's label expression is a named child sitting where a statement would, so
         * counting children directly reads `case 2:` as a clause with one statement when it is
         * really an empty one falling through to the next.
         */
        uint32_t FirstStatementIndex(TSNode clause)
        {
            return IsDefaultClause(clause) ? 0u : 1u;
        }

        void EmitAtNode(TSNode node, DiagnosticContext &ctx, std::string_view code,
                        DiagnosticSeverity severity = DiagnosticSeverity::Error)
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            ctx.EmitAtRange(start.row, start.column, end.row, end.column, code, severity);
        }

        void EmitAtNode(TSNode node, DiagnosticContext &ctx, std::string_view code, const std::string &arg)
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            ctx.EmitAtRange(start.row, start.column, end.row, end.column, code, arg);
        }

        /**
         * @brief `if (c);` and `else;` - a bare semicolon where the branch body belongs.
         *
         * AngelScript is stricter here than C++ and stricter than its own loops, which is the whole
         * reason this is worth a rule. Measured, six probes:
         *
         *     if (c);          ERROR   If with empty statement
         *     else;            ERROR   Else with empty statement
         *     if (c) {}        accepted - an empty *block* is fine
         *     while (c);       accepted
         *     for (;;);        accepted
         *     do; while (c);   accepted
         *
         * So it is `if` and `else` and nothing else. The mistake it catches is the one that started
         * this: `if (cond); return true; else return false;` reads as a three-branch decision and is
         * really an empty `if`, a `return` that always runs, and an `else` belonging to no `if`.
         *
         * Skipped inside a parse error, where a `;` in this position is as likely to be tree-sitter
         * recovering as it is to be what somebody typed.
         */
        void CheckEmptyBranch(TSNode node, DiagnosticContext &ctx)
        {
            if (ts_node_has_error(node))
            {
                return;
            }

            const TSNode consequence = ts_node_child_by_field_name(node, "consequence", k_consequenceFieldLength);
            if (!ts_node_is_null(consequence) && std::string_view(ts_node_type(consequence)) == ";")
            {
                const TSPoint start = ts_node_start_point(consequence);
                const TSPoint end = ts_node_end_point(consequence);
                ctx.EmitAtRange(start.row, start.column, end.row, end.column, "as-err-if-empty-statement");
            }

            const TSNode alternative = ts_node_child_by_field_name(node, "alternative", k_alternativeFieldLength);
            if (!ts_node_is_null(alternative) && std::string_view(ts_node_type(alternative)) == ";")
            {
                const TSPoint start = ts_node_start_point(alternative);
                const TSPoint end = ts_node_end_point(alternative);
                ctx.EmitAtRange(start.row, start.column, end.row, end.column, "as-err-else-empty-statement");
            }
        }

        // =====================================================================
        // Definite return
        // =====================================================================

        /**
         * @brief True when control cannot fall off the end of this statement.
         *
         * Deliberately one-sided. Answering "yes" wrongly hides a real missing return, which costs
         * nothing; answering "no" wrongly reports a function that is perfectly fine, which is the
         * failure that matters. So every construct whose exit conditions this pass cannot settle -
         * a try block, a loop with a computed condition - answers "no" only where "no" is also the
         * honest reading, and the loops whose condition is literally true answer "yes" because they
         * have no normal exit at all.
         */
        /**
         * @brief True for a return type that null cannot convert to.
         *
         * The primitives, by name. Deliberately not "anything that is not a handle": a value type
         * the host registered may accept null through a conversion this analyzer never sees, and
         * inventing an error there costs more than missing one.
         */
        bool IsNonNullablePrimitiveName(std::string_view typeName)
        {
            return parser::primitives::IsNonNullable(typeName);
        }

        bool DefinitelyReturns(TSNode node, std::string_view sourceCode)
        {
            const std::string_view type = NodeType(node);

            if (type == "return_statement")
            {
                return true;
            }

            if (type == "statement_block" || type == "case_clause")
            {
                const uint32_t first = type == "case_clause" ? FirstStatementIndex(node) : 0u;
                const uint32_t count = ts_node_named_child_count(node);
                for (uint32_t i = first; i < count; ++i)
                {
                    if (DefinitelyReturns(ts_node_named_child(node, i), sourceCode))
                    {
                        return true;
                    }
                }
                return false;
            }

            if (type == "if_statement")
            {
                TSNode alternative = ts_node_child_by_field_name(node, "alternative", k_alternativeFieldLength);
                if (ts_node_is_null(alternative))
                {
                    return false;
                }
                TSNode consequence = ts_node_child_by_field_name(node, "consequence", k_consequenceFieldLength);
                return DefinitelyReturns(consequence, sourceCode) && DefinitelyReturns(alternative, sourceCode);
            }

            if (type == "switch_statement")
            {
                bool hasDefault = false;
                bool allReturn = true;
                const uint32_t count = ts_node_named_child_count(node);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode clause = ts_node_named_child(node, i);
                    if (NodeType(clause) != "case_clause")
                    {
                        continue;
                    }
                    if (IsDefaultClause(clause))
                    {
                        hasDefault = true;
                    }
                    // An empty clause falls through to the next one, which is ordinary and says
                    // nothing about whether the switch returns.
                    const bool hasStatements =
                        ts_node_named_child_count(clause) > FirstStatementIndex(clause);
                    if (hasStatements && !DefinitelyReturns(clause, sourceCode))
                    {
                        allReturn = false;
                    }
                }
                return hasDefault && allReturn;
            }

            // No loop counts, whatever its condition says. This used to reason that `while (true)`
            // has no normal exit and so ends the function - true of the program, and not the rule
            // the compiler applies. Measured, all three:
            //
            //     int f() { while (true) { return 1; } }        Not all paths return a value
            //     int f() { for (;;) { return 1; } }            Not all paths return a value
            //     int f() { do { return 1; } while (true); }    Not all paths return a value
            //
            // Its analysis is structural: a loop body may run zero times as far as it is concerned,
            // so a return inside one is not a return on every path. Believing otherwise cost three
            // errors the compiler gives and this analyzer did not.
            if (type == "while_statement" || type == "do_while_statement" || type == "for_statement" ||
                type == "foreach_statement")
            {
                return false;
            }

            if (type == "try_statement")
            {
                // Every block has to return, because either one of them can be the path taken: the
                // try block runs to its end, or an exception hands control to the catch block. The
                // real compiler answers `try { return 1; } catch { }` with "Not all paths return a
                // value" and accepts it once the catch returns too.
                //
                // The grammar gives `try` and `catch` a `statement_block` each and no fields, so the
                // named children are exactly the blocks to check.
                const uint32_t blockCount = ts_node_named_child_count(node);
                if (blockCount == 0)
                {
                    return false;
                }
                for (uint32_t i = 0; i < blockCount; ++i)
                {
                    if (!DefinitelyReturns(ts_node_named_child(node, i), sourceCode))
                    {
                        return false;
                    }
                }
                return true;
            }

            return false;
        }

        // =====================================================================
        // Switch clauses
        // =====================================================================

        /** @brief True for a case value shape AngelScript cannot use as a label. */
        bool IsUnusableCaseValue(TSNode expression, std::string_view sourceCode)
        {
            const std::string_view type = NodeType(expression);

            if (type == "string_literal")
            {
                return true;
            }

            if (type == "number_literal")
            {
                // A case label has to be an integral constant, so a fractional literal is out.
                // Identifiers are left alone: an enum member and a `const int` both arrive as one
                // and both are legal.
                const std::string_view text = NodeText(expression, sourceCode);
                return text.find('.') != std::string_view::npos;
            }

            return false;
        }

        /**
         * @brief The number a case label stands for, when that can be known for certain.
         *
         * `enum E { A = 1, B = 1 }` gives two names to one number, and a switch dispatches on the
         * number - so `case A:` and `case B:` are the same label twice and the compiler says so:
         * "Duplicate switch case", measured. Comparing the labels as written never sees it.
         *
         * Narrow on purpose. Only a member whose value is written as a plain decimal integer is
         * resolved; an implicit value (`enum E { A, B }` counts up from zero), an expression
         * (`1 << 2`), or a hex literal returns nothing and the caller falls back to comparing the
         * text. Getting this wrong in the other direction would mean reporting a duplicate that is
         * not one, on code that compiles.
         *
         * Accepts both `A` and `E::A`; the qualifier is dropped before the lookup.
         */
        std::optional<long long> EnumeratorValue(std::string_view label, const SymbolTable &table)
        {
            // `meta_api::json::Type::Undefined` -> enum `Type`, member `Undefined`. The qualifier is
            // not decoration: matching on the member name alone reported a duplicate in four real
            // scripts, because some other enum in the same workspace happened to declare a member of
            // the same name with a value that collided. Found by the corpus audit, on code that
            // compiles.
            std::string_view enumName;
            if (const size_t sep = label.rfind("::"); sep != std::string_view::npos)
            {
                enumName = label.substr(0, sep);
                label = label.substr(sep + 2);

                if (const size_t outer = enumName.rfind("::"); outer != std::string_view::npos)
                    enumName = enumName.substr(outer + 2);
            }

            if (label.empty())
                return std::nullopt;

            std::optional<long long> found;
            size_t declaringEnums = 0;

            table.ForEachSymbol([&](const std::string &, const std::vector<Symbol> &symbols)
            {
                for (const auto &sym : symbols)
                {
                    if (sym.type != SymbolType::Enum || !std::holds_alternative<EnumSignature>(sym.signature))
                        continue;

                    // Written with a qualifier: only the enum it names may answer.
                    if (!enumName.empty() && sym.name != enumName)
                        continue;

                    for (const auto &member : sym.GetEnum().members)
                    {
                        if (member.name != label)
                            continue;

                        ++declaringEnums;

                        // No value written means the compiler counts it up from the previous one.
                        // Following that count is possible and is not done here: the value then
                        // depends on every member before it, and a wrong number would mean claiming
                        // a duplicate that is not one.
                        if (member.value.empty())
                            return;

                        const std::string_view text(member.value);
                        size_t index = 0;
                        bool negative = false;
                        if (text[index] == '-' || text[index] == '+')
                        {
                            negative = text[index] == '-';
                            ++index;
                        }
                        if (index >= text.size())
                            return;

                        long long parsed = 0;
                        for (; index < text.size(); ++index)
                        {
                            if (text[index] < '0' || text[index] > '9')
                                return;
                            parsed = parsed * 10 + (text[index] - '0');
                        }

                        found = negative ? -parsed : parsed;
                    }
                }
            });

            // Written without a qualifier and declared by more than one enum: which one this label
            // means is the compiler's business and not knowable from here.
            if (declaringEnums != 1)
                return std::nullopt;

            return found;
        }

        void CheckSwitch(TSNode node, std::string_view sourceCode, DiagnosticContext &ctx)
        {
            std::vector<std::string> seenValues;
            TSNode defaultClause = {};
            bool haveDefault = false;
            bool defaultIsLast = true;

            const uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode clause = ts_node_named_child(node, i);
                if (NodeType(clause) != "case_clause")
                {
                    continue;
                }

                if (IsDefaultClause(clause))
                {
                    defaultClause = clause;
                    haveDefault = true;
                    defaultIsLast = true;
                    continue;
                }

                if (haveDefault)
                {
                    defaultIsLast = false;
                }

                // The label's expression is the clause's first named child; anything after it is a
                // statement of the clause body.
                TSNode value = ts_node_named_child(clause, 0);
                if (ts_node_is_null(value))
                {
                    continue;
                }

                // A clause body's first statement sits where the label would when the label is
                // absent, which only happens on a malformed switch the parser already reported.
                if (ClauseKeyword(clause) != "case")
                {
                    continue;
                }

                if (IsUnusableCaseValue(value, sourceCode))
                {
                    EmitAtNode(value, ctx, "as-err-invalid-case-type");
                }

                if (NodeType(value) == "identifier" || NodeType(value) == "scoped_identifier")
                {
                    std::string idText(Trim(NodeText(value, sourceCode)));

                    bool reported = false;
                    auto syms = FindSymbolsInScope(idText, value, sourceCode, ctx.request.symbolTable);
                    for (const auto &s : syms)
                    {
                        if (s.type == SymbolType::Variable && !s.GetVariable().modifiers.isConst)
                        {
                            EmitAtNode(value, ctx, "as-err-case-not-constant");
                            reported = true;
                            break;
                        }
                    }

                    // A LOCAL is not in the symbol table - locals live in the scope tree - so
                    // `void f() { int y = 2; switch (x) { case y: ... } }` went unreported while the
                    // same mistake with a global was caught. Measured: "Case expressions must be
                    // literal constants".
                    //
                    // Only a name that resolves to a local variable is reported. One that resolves
                    // to nothing at all stays silent, because an enum member the host registered in
                    // C++ and declared in no stub looks exactly like it from here - the same reason
                    // the undeclared-identifier rule is a warning rather than an error.
                    if (!reported && ctx.request.scopeRoot)
                    {
                        const TSPoint at = ts_node_start_point(value);
                        if (const Scope *scope = FindInnermostScope(ctx.request.scopeRoot.get(), at.row, at.column))
                        {
                            const Scope *owner = nullptr;
                            const LocalDefinition *def = ResolveInScope(scope, idText, &owner);

                            // Inside a function body, and only there. The scope tree also holds
                            // module-scope declarations, and it does not record `const` - so a
                            // `const int` global used as a case label, which is legal and idiomatic,
                            // looked exactly like a local from here and was reported. Requiring a
                            // function scope is what tells the two apart.
                            bool insideFunction = false;
                            for (const Scope *current = owner; current != nullptr; current = current->parent)
                            {
                                if (current->isFunctionScope)
                                {
                                    insideFunction = true;
                                    break;
                                }
                            }

                            if (def && insideFunction && def->kind == LocalDefinitionKind::Variable)
                            {
                                EmitAtNode(value, ctx, "as-err-case-not-constant");
                            }
                        }
                    }
                }
                else if (NodeType(value) == "call_expression")
                {
                    EmitAtNode(value, ctx, "as-err-case-not-constant");
                }

                std::string text(Trim(NodeText(value, sourceCode)));
                if (!text.empty())
                {
                    // Compared by the number where the number is knowable, by the text otherwise.
                    // The key carries a marker so a label spelled `1` and a label named `A` that
                    // happens to be 1 collide - which they do, in the compiler.
                    std::string key = text;
                    if (const auto resolved = EnumeratorValue(text, ctx.request.symbolTable))
                        key = "#" + std::to_string(*resolved);
                    else if (text.find_first_not_of("-+0123456789") == std::string::npos)
                        key = "#" + std::to_string(std::stoll(text));

                    if (std::find(seenValues.begin(), seenValues.end(), key) != seenValues.end())
                    {
                        EmitAtNode(value, ctx, "as-err-duplicate-case-value", text);
                    }
                    else
                    {
                        seenValues.push_back(std::move(key));
                    }
                }
            }

            if (haveDefault && !defaultIsLast)
            {
                EmitAtNode(defaultClause, ctx, "as-err-default-must-be-last");
            }
        }

        // =====================================================================
        // Traversal
        // =====================================================================

        struct FlowState
        {
            uint32_t loopDepth = 0;
            uint32_t switchDepth = 0;

            /**
             * @brief What the enclosing function must return, and the name to say it under.
             *
             * Empty means there is nothing to check against - a constructor, or a lambda whose
             * funcdef this analyzer cannot see - and the rule stays silent, which is the policy
             * everywhere else the world is only partly visible.
             */
            std::string requiredReturn;
            std::string functionName;

            /**
             * @brief True for a function the grammar gives no return type: a constructor or a
             *        destructor.
             *
             * Both return void, and `return 42;` in one is "Can't return value when return type is
             * 'void'" - measured. as-err-void-return-value covers the same mistake in a function
             * that spells `void` out, and it reads the return type node, so it never fired here.
             */
            bool implicitVoid = false;
        };

        /**
         * @brief Reports the first statement a block can never reach.
         *
         * Purely structural, and that is the whole rule: anything written after a `return`, a
         * `break` or a `continue` *in the same block* is dead. Compiled against a real engine,
         * which warns - not errors - at exactly those three, and does not warn after an `if` whose
         * body returns, because the false branch still falls through. So no path analysis is
         * involved and none is wanted; DefinitelyReturns exists for the question that does need it.
         *
         * One report per block, at the first dead statement. The engine says it once too, and a
         * warning per line of a dead tail would bury the one that matters.
         */
        void CheckUnreachable(TSNode block, DiagnosticContext &ctx)
        {
            // A block the parser could not make sense of has a shape that is error recovery's
            // guess, not the author's, and reading a terminator out of it says nothing. Found by
            // the corpus audit on a file whose `if(...); return true; else return false;` is not
            // AngelScript at all.
            if (ts_node_has_error(block))
            {
                return;
            }

            const uint32_t count = ts_node_named_child_count(block);
            bool terminated = false;

            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_named_child(block, i);
                const std::string_view childType = NodeType(child);

                // Comments are extras rather than statements, and a comment after a return is
                // ordinary - it is usually what explains the return.
                if (childType == "comment")
                {
                    continue;
                }

                // A conditional compilation directive is an opaque line to this grammar, so which
                // statements around it are live is not something this pass can know. The corpus
                // has `#if FALSE ... return value; #endif` followed by the real code, and every
                // statement after it read as dead. One directive anywhere in the block retires the
                // question for the whole block, because a `#endif` alone gives no clue what its
                // `#if` decided.
                if (childType == "preproc_directive")
                {
                    return;
                }

                if (terminated)
                {
                    EmitAtNode(child, ctx, "as-warn-unreachable-code", DiagnosticSeverity::Warning);
                    return;
                }

                terminated = childType == "return_statement" || childType == "break_statement" ||
                             childType == "continue_statement";
            }
        }

        void Visit(TSNode node, std::string_view sourceCode, FlowState state, DiagnosticContext &ctx, int depth = 0)
        {
            // See k_maxAstDepth in ASTUtils.h.
            if (depth > k_maxAstDepth)
                return;

            const std::string_view type = NodeType(node);

            if (type == "break_statement")
            {
                if (state.loopDepth == 0 && state.switchDepth == 0)
                {
                    EmitAtNode(node, ctx, "as-err-break-outside-loop");
                }
                return;
            }

            if (type == "continue_statement")
            {
                if (state.loopDepth == 0)
                {
                    EmitAtNode(node, ctx, "as-err-continue-outside-loop");
                }
                return;
            }

            if (type == "statement_block" || type == "case_clause")
            {
                CheckUnreachable(node, ctx);
            }

            // A bare `return;` in a function that owes a value. Measured: the compiler answers
            // "Must return a value" and rejects the file, and nothing here saw it -
            // as-err-not-all-paths-return asks only whether a return is *reached*, so
            // `float FS(float f) { return; }` passed every check this analyzer had.
            //
            // The opposite direction - a value returned from a void function - is
            // as-err-void-return-value, in TypeConversionChecker, and stays there.
            //
            // Not an early return: a returned expression can contain a lambda with a body of its
            // own, and that body still has to be walked.
            if (type == "return_statement" && !state.requiredReturn.empty() &&
                state.requiredReturn != "void" && ts_node_named_child_count(node) == 0)
            {
                EmitAtNode(node, ctx, "as-err-return-value-required", state.functionName);
            }

            // The constructor half of the same rule. Reported here rather than in
            // TypeConversionChecker because that one asks the return type node what is required,
            // and a constructor has none - so the one function in the language that can only
            // return void was the one nothing checked.
            if (type == "return_statement" && state.implicitVoid && ts_node_named_child_count(node) > 0)
            {
                EmitAtNode(node, ctx, "as-err-void-return-value");
            }

            // `int f() { return null; }` - "No conversion from '<null handle>' to 'int' available."
            // as-err-null-non-handle says exactly this about a variable and had nothing to say
            // about a return. Restricted to the primitives, and to a return type carrying no `@`,
            // so a class or a handle - where null may well be legal, and where the host may have
            // registered a conversion this analyzer cannot see - is left alone.
            if (type == "return_statement" && !state.requiredReturn.empty() &&
                state.requiredReturn.find('@') == std::string::npos &&
                IsNonNullablePrimitiveName(state.requiredReturn) &&
                ts_node_named_child_count(node) == 1 &&
                NodeType(ts_node_named_child(node, 0)) == node_types::NullLiteral)
            {
                EmitAtNode(node, ctx, "as-err-null-non-handle", state.requiredReturn);
            }

            if (type == "func_declaration" || type == "lambda_expression")
            {
                // A nested function opens its own flow: a loop enclosing the declaration does not
                // make a `break` inside the nested body legal.
                state = FlowState{};

                TSNode body = ts_node_child_by_field_name(node, "body", k_bodyFieldLength);
                TSNode returnType = ts_node_child_by_field_name(node, "return_type", k_returnTypeFieldLength);
                TSNode name = ts_node_child_by_field_name(node, "name", k_nameFieldLength);

                // What this body is required to return, and under whose name to say so.
                //
                // A lambda has neither: the grammar gives `lambda_expression` a parameter list and
                // a body and nothing else, so `return_type` is null and this check simply never
                // ran for one. The requirement comes from the funcdef it is being handed to -
                // `funcdef int CB(); CB@ cb = function() { };` is "Not all paths return a value"
                // to the real compiler - and that funcdef is also the only name there is to
                // report, so the message names it and the diagnostic is anchored on the lambda.
                std::string requiredReturn;
                std::string reportedName;
                if (!ts_node_is_null(returnType))
                {
                    requiredReturn = Trim(NodeText(returnType, sourceCode));
                    reportedName = std::string(NodeText(name, sourceCode));
                }
                else if (type == node_types::LambdaExpression)
                {
                    if (const auto target = FuncdefTargetOfLambda(node, ctx.request.symbolTable, sourceCode))
                    {
                        requiredReturn = CleanBaseType(target->GetFuncdef().returnType);
                        reportedName = target->name;
                    }
                }

                if (!ts_node_is_null(body) && !requiredReturn.empty() && requiredReturn != "void" &&
                    !DefinitelyReturns(body, sourceCode))
                {
                    EmitAtNode(ts_node_is_null(name) ? node : name, ctx, "as-err-not-all-paths-return",
                               reportedName);
                }

                // Carried into the body, so every `return` inside it knows what it owes. A lambda
                // whose funcdef target could not be resolved leaves this empty and the check below
                // does nothing, which is the same silence the rule above keeps.
                state.requiredReturn = requiredReturn;
                state.functionName = reportedName;
                state.implicitVoid = ts_node_is_null(returnType) && type != node_types::LambdaExpression;
            }
            else if (type == "while_statement" || type == "for_statement" ||
                     type == "foreach_statement" || type == "do_while_statement")
            {
                ++state.loopDepth;
            }
            else if (type == "switch_statement")
            {
                CheckSwitch(node, sourceCode, ctx);
                ++state.switchDepth;
            }
            else if (type == "if_statement")
            {
                CheckEmptyBranch(node, ctx);
            }

            const uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                Visit(ts_node_named_child(node, i), sourceCode, state, ctx, depth + 1);
            }
        }
    }

    void CheckControlFlow(const ControlFlowCheckRequest &request, DiagnosticContext &ctx)
    {
        if (ts_node_is_null(request.root) || request.sourceCode.empty())
        {
            return;
        }

        Visit(request.root, request.sourceCode, FlowState{}, ctx);
    }
}
