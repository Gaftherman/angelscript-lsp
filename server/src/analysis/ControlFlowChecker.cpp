#include "analysis/ControlFlowChecker.h"
#include "analysis/SemanticHelpers.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

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

            if (type == "while_statement")
            {
                // `while (true)` has no normal exit, so whatever follows it is unreachable. The
                // condition has to be the literal itself, not merely text reading "true": gating on
                // the node type keeps an identifier that happens to be named `true` out of it.
                TSNode condition = ts_node_named_child(node, 0);
                return NodeType(condition) == node_types::BooleanLiteral &&
                       Trim(NodeText(condition, sourceCode)) == "true";
            }

            if (type == "do_while_statement")
            {
                return DefinitelyReturns(ts_node_child_by_field_name(node, "body", k_bodyFieldLength), sourceCode);
            }

            if (type == "for_statement")
            {
                // `for (;;)` - the condition slot holds only the semicolon.
                const std::string_view condition =
                    Trim(NodeText(ts_node_child_by_field_name(node, "condition", k_conditionFieldLength), sourceCode));
                return condition.empty() || condition == ";";
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

                std::string text(Trim(NodeText(value, sourceCode)));
                if (!text.empty())
                {
                    if (std::find(seenValues.begin(), seenValues.end(), text) != seenValues.end())
                    {
                        EmitAtNode(value, ctx, "as-err-duplicate-case-value", text);
                    }
                    else
                    {
                        seenValues.push_back(std::move(text));
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

        void Visit(TSNode node, std::string_view sourceCode, FlowState state, DiagnosticContext &ctx)
        {
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

            if (type == "func_declaration" || type == "lambda_expression")
            {
                // A nested function opens its own flow: a loop enclosing the declaration does not
                // make a `break` inside the nested body legal.
                state = FlowState{};

                TSNode body = ts_node_child_by_field_name(node, "body", k_bodyFieldLength);
                TSNode returnType = ts_node_child_by_field_name(node, "return_type", k_returnTypeFieldLength);
                if (!ts_node_is_null(body) && !ts_node_is_null(returnType) &&
                    Trim(NodeText(returnType, sourceCode)) != "void" &&
                    !DefinitelyReturns(body, sourceCode))
                {
                    TSNode name = ts_node_child_by_field_name(node, "name", k_nameFieldLength);
                    EmitAtNode(ts_node_is_null(name) ? node : name, ctx, "as-err-not-all-paths-return",
                               std::string(NodeText(name, sourceCode)));
                }
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

            const uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                Visit(ts_node_named_child(node, i), sourceCode, state, ctx);
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
