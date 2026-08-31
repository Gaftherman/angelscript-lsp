#include "analysis/DefiniteAssignmentChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeExtraction.h"
#include "analysis/OverloadResolver.h"
#include "utils/Utils.h"
#include <ankerl/unordered_dense.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
    namespace
    {
        /**
         * @brief Node text as an owning string.
         *
         * Kept per translation unit rather than shared with ASTUtils::NodeText, which returns a
         * string_view. The two are not interchangeable: callers here store the result, concatenate
         * it, and use it after the node has gone out of scope, so handing them a view would trade a
         * duplicated three-line function for a lifetime question at several dozen call sites.
         * Deduplicating it was attempted and reverted for exactly that reason.
         */
        std::string NodeText(TSNode node, std::string_view sourceCode)
        {
            if (ts_node_is_null(node))
            {
                return "";
            }

            const uint32_t start = ts_node_start_byte(node);
            const uint32_t end = ts_node_end_byte(node);
            if (start >= end || end > sourceCode.size())
            {
                return "";
            }
            return std::string(sourceCode.substr(start, end - start));
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

        std::string GetSimpleIdentifierName(TSNode node, std::string_view sourceCode)
        {
            if (ts_node_is_null(node))
            {
                return "";
            }
            std::string_view type = ts_node_type(node);
            if (type == "identifier")
            {
                return NodeText(node, sourceCode);
            }
            if (type == "scoped_identifier")
            {
                uint32_t count = ts_node_named_child_count(node);
                if (count == 1)
                {
                    TSNode child = ts_node_named_child(node, 0);
                    if (std::string_view(ts_node_type(child)) == "identifier")
                    {
                        return NodeText(child, sourceCode);
                    }
                }
            }
            return "";
        }

        struct FlowState
        {
            ankerl::unordered_dense::set<std::string> assignedVars;
            bool hasReturned = false;
            bool isTerminated = false;
        };

        /**
         * @brief Merges two branch states by UNION, which is AngelScript's rule and not C#'s.
         *
         * This intersected, implementing "definitely assigned on every path" - the rule C# and
         * Java enforce as an error. AngelScript's is far weaker, and it is a warning: it fires only
         * when NO assignment precedes the read at all. Measured, and the two middle lines are the
         * ones that settle it:
         *
         *     uint c; Use(c);                        WARNING: 'c' is not initialized.
         *     uint c; Use(c); c = 5;                 WARNING - the read comes first
         *     uint c; if (true)  { c = 5; } Use(c);  clean
         *     uint c; if (false) { c = 5; } Use(c);  clean - conditional, and even unreachable
         *     uint c; for (...) { if (...) c = 5; } Use(c);   clean
         *     uint c; while (true) { c = 5; break; } Use(c);  clean
         *
         * So an assignment in one arm is enough, and the join has to keep it rather than require
         * both arms to agree. Intersecting produced 400 findings over the corpus against the
         * compiler's own 7, on the ordinary shape of assigning inside a loop and reading after it.
         */
        ankerl::unordered_dense::set<std::string> MergeBranchStates(
            const ankerl::unordered_dense::set<std::string> &a,
            const ankerl::unordered_dense::set<std::string> &b)
        {
            ankerl::unordered_dense::set<std::string> result = a;
            for (const auto &item : b)
            {
                result.insert(item);
            }
            return result;
        }

        class DefiniteAssignmentVisitor
        {
        public:
            DefiniteAssignmentVisitor(
                const DefiniteAssignmentCheckRequest &request,
                DiagnosticContext &ctx)
                : m_request(request), m_ctx(ctx)
            {
            }

            void AnalyzeFunction(TSNode funcNode)
            {
                m_trackedLocals.clear();
                m_reportedReads.clear();

                TSNode bodyNode = ts_node_child_by_field_name(funcNode, "body", 4);
                if (ts_node_is_null(bodyNode))
                {
                    uint32_t count = ts_node_named_child_count(funcNode);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        TSNode child = ts_node_named_child(funcNode, i);
                        if (std::string_view(ts_node_type(child)) == "statement_block")
                        {
                            bodyNode = child;
                            break;
                        }
                    }
                }

                if (ts_node_is_null(bodyNode))
                {
                    return;
                }

                FlowState state;
                AnalyzeStatement(bodyNode, state);
            }

        private:
            const DefiniteAssignmentCheckRequest &m_request;
            DiagnosticContext &m_ctx;
            ankerl::unordered_dense::set<std::string> m_trackedLocals;
            ankerl::unordered_dense::set<std::string> m_reportedReads;

            void CheckExpressionReads(TSNode node, FlowState &state, int depth = 0)
            {
                    // See k_maxAstDepth in ASTUtils.h.
                    if (depth > k_maxAstDepth)
                        return;


                if (ts_node_is_null(node))
                {
                    return;
                }

                std::string_view type = ts_node_type(node);

                if (type == "identifier" || type == "scoped_identifier")
                {
                    std::string name = GetSimpleIdentifierName(node, m_request.sourceCode);
                    if (!name.empty() && m_trackedLocals.contains(name) && !state.assignedVars.contains(name))
                    {
                        TSPoint start = ts_node_start_point(node);
                        TSPoint end = ts_node_end_point(node);
                        std::string locationKey = name + ":" + std::to_string(start.row) + ":" + std::to_string(start.column);
                        if (m_reportedReads.insert(locationKey).second)
                        {
                            // A WARNING, which is what the compiler answers. Measured, seven
                            // shapes, every one accepted with `WARNING: 'n' is not initialized.`
                            // and exit 0: a plain read, a read inside an expression, a by-value
                            // argument, and a `const &in` argument. This was emitted as an ERROR
                            // over 749 sites in the corpus - errors are what the parity audit
                            // counts and what a build gate would stop on, so the severity was not
                            // a cosmetic detail.
                            m_ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                              "as-warn-uninitialized-variable-read", name,
                                              DiagnosticSeverity::Warning);
                        }
                    }
                    return;
                }

                if (type == "assignment_expression")
                {
                    TSNode left = ts_node_child_by_field_name(node, "left", 4);
                    TSNode opNode = ts_node_child_by_field_name(node, "operator", 8);
                    TSNode right = ts_node_child_by_field_name(node, "right", 5);
                    std::string op = NodeText(opNode, m_request.sourceCode);

                    // Evaluate right-hand side reads first
                    CheckExpressionReads(right, state);

                    if (op == "=")
                    {
                        std::string varName = GetSimpleIdentifierName(left, m_request.sourceCode);
                        if (!varName.empty() && m_trackedLocals.contains(varName))
                        {
                            state.assignedVars.insert(varName);
                            return;
                        }
                        // Non-bare identifier on left (e.g. obj.x = val, arr[i] = val)
                        CheckExpressionReads(left, state);
                    }
                    else
                    {
                        // Compound assignment (+=, -=, etc.): left is read before being written
                        CheckExpressionReads(left, state);
                    }
                    return;
                }

                if (type == "call_expression")
                {
                    TSNode funcNode = ts_node_child_by_field_name(node, "function", 8);
                    TSNode argsNode = ts_node_child_by_field_name(node, "arguments", 9);

                    // Check callee expression reads
                    CheckExpressionReads(funcNode, state);

                    if (ts_node_is_null(argsNode))
                    {
                        return;
                    }

                    std::vector<Symbol> candidates;
                    std::string_view funcType = ts_node_type(funcNode);
                    if (funcType == "member_expression")
                    {
                        TSNode objNode = ts_node_child_by_field_name(funcNode, "object", 6);
                        TSNode memNode = ts_node_child_by_field_name(funcNode, "member", 6);
                        if (!ts_node_is_null(objNode) && !ts_node_is_null(memNode))
                        {
                            // Resolved in the scope the call is written in, not at the root.
                            //
                            // The receiver of a method call is almost always a local - `Reader
                            // reader; reader.Get(value)` - and a local is not in the root scope, so
                            // resolving there answered nothing, the hierarchy came back empty, no
                            // candidate was found and the `&out` parameter below was never
                            // recognised. Every out-parameter of a METHOD was therefore read as a
                            // read, and `int n; obj.Get(n);` - which is how you initialise `n` -
                            // was reported as using it uninitialised. Free functions were fine,
                            // because their lookup takes the node and not the root.
                            const TSPoint objStart = ts_node_start_point(objNode);
                            const Scope *callScope =
                                m_request.scopeRoot
                                    ? FindEnclosingScope(m_request.scopeRoot, objStart.row, objStart.column)
                                    : nullptr;
                            std::string objType = ResolveExpressionType(objNode, callScope ? callScope : m_request.scopeRoot, m_ctx.request.symbolTable, m_request.sourceCode, m_ctx.request.fileUri);
                            std::string cleanObj = CleanBaseType(objType);
                            std::string memName = NodeText(memNode, m_request.sourceCode);
                            auto hierarchy = GetInheritedTypeHierarchy(cleanObj, m_ctx.request.symbolTable);
                            for (const auto &typeName : hierarchy)
                            {
                                auto found = m_ctx.request.symbolTable.FindSymbols(typeName + "::" + memName);
                                for (const auto &sym : found)
                                {
                                    if (sym.type == SymbolType::Function)
                                    {
                                        candidates.push_back(sym);
                                    }
                                }
                            }
                        }
                    }
                    else
                    {
                        std::string funcName = NodeText(funcNode, m_request.sourceCode);
                        auto inScope = FindSymbolsInScope(funcName, node, m_request.sourceCode, m_ctx.request.symbolTable);
                        for (const auto &sym : inScope)
                        {
                            if (sym.type == SymbolType::Function)
                            {
                                candidates.push_back(sym);
                            }
                        }
                    }

                    std::vector<std::string> argTypes;
                    std::vector<TSNode> argNodes;
                    uint32_t rawChildCount = ts_node_child_count(argsNode);
                    for (uint32_t i = 0; i < rawChildCount; ++i)
                    {
                        TSNode child = ts_node_child(argsNode, i);
                        std::string_view ct = ts_node_type(child);
                        if (ct != "(" && ct != ")" && ct != "," && ct != "comment")
                        {
                            argNodes.push_back(child);
                            argTypes.push_back(ResolveExpressionType(child, m_request.scopeRoot, m_ctx.request.symbolTable, m_request.sourceCode, m_ctx.request.fileUri));
                        }
                    }

                    auto best = ResolveBestOverload(candidates, argTypes, m_ctx.request.symbolTable);
                    const FunctionSignature *sig = (best.bestCandidate && std::holds_alternative<FunctionSignature>(best.bestCandidate->signature))
                                                       ? &best.bestCandidate->GetFunction()
                                                       : (!candidates.empty() && std::holds_alternative<FunctionSignature>(candidates[0].signature) ? &candidates[0].GetFunction() : nullptr);

                    for (size_t i = 0; i < argNodes.size(); ++i)
                    {
                        // `&out` if ANY candidate declares one at this position, not only the one
                        // overload resolution happened to pick.
                        //
                        // Picking one is a guess whenever the candidates disagree, and a wrong
                        // guess here reports the line that initialises the variable. The corpus
                        // shape is `class json : meta_api::json::v2::json` restating its methods,
                        // so a member lookup finds the base's `Get` beside the derived one and the
                        // resolver has no reason to prefer either; reading `&out` off the loser
                        // produced the last six findings this rule made. Asking "could this be an
                        // out-parameter" instead of "is the chosen one" is the same
                        // silence-over-guessing the unknown-callee case above applies.
                        bool isOutParam = false;
                        const auto declaresOutAt = [i](const FunctionSignature &candidate)
                        {
                            if (i >= candidate.parameters.size())
                            {
                                return false;
                            }
                            const auto &param = candidate.parameters[i];
                            return param.modifier == ParameterModifier::Out ||
                                   param.typeName.find("&out") != std::string::npos ||
                                   param.rawText.find("&out") != std::string::npos;
                        };

                        if (sig && declaresOutAt(*sig))
                        {
                            isOutParam = true;
                        }
                        else
                        {
                            for (const auto &candidate : candidates)
                            {
                                if (std::holds_alternative<FunctionSignature>(candidate.signature) &&
                                    declaresOutAt(candidate.GetFunction()))
                                {
                                    isOutParam = true;
                                    break;
                                }
                            }
                        }

                        if (isOutParam)
                        {
                            std::string varName = GetSimpleIdentifierName(argNodes[i], m_request.sourceCode);
                            if (!varName.empty() && m_trackedLocals.contains(varName))
                            {
                                state.assignedVars.insert(varName);
                                continue;
                            }
                        }

                        // No visible declaration for the callee, so whether this parameter is
                        // `&out` is exactly what cannot be established - and `&out` is
                        // AngelScript's only way to return a second value, so it is what a bare
                        // local passed to an unknown function most often is.
                        //
                        // `g_Utility.GetCircularGaussianSpread(x, y)` writes both of them, and the
                        // analyzer cannot know that: g_Utility is registered by the game engine in
                        // C++ and declared in no script. Treating the argument as a read reported
                        // the line that initialises the variable. That, and the same shape
                        // repeated, is most of the 749 findings this rule produced over the corpus.
                        //
                        // Only a bare identifier is spared: `f(x + 1)` cannot be an out-argument -
                        // an out-parameter needs an l-value - so a compound expression is a read
                        // whoever the callee turns out to be, and stays judged.
                        if (!sig)
                        {
                            const std::string bareName =
                                GetSimpleIdentifierName(argNodes[i], m_request.sourceCode);
                            if (!bareName.empty() && m_trackedLocals.contains(bareName))
                            {
                                // Marked assigned, not merely unread. The same ignorance runs both
                                // ways: if the parameter may be `&out`, the variable may now hold a
                                // value, and every later read of it is equally undecidable. Skipping
                                // only this line left `g_Utility.GetCircularGaussianSpread(x, y);`
                                // silent and then reported the next line that used x.
                                state.assignedVars.insert(bareName);
                                continue;
                            }
                        }

                        CheckExpressionReads(argNodes[i], state);
                    }
                    return;
                }

                if (type == "member_expression")
                {
                    TSNode objNode = ts_node_child_by_field_name(node, "object", 6);
                    CheckExpressionReads(objNode, state);
                    return;
                }

                uint32_t count = ts_node_named_child_count(node);
                for (uint32_t i = 0; i < count; ++i)
                {
                    CheckExpressionReads(ts_node_named_child(node, i), state, depth + 1);
                }
            }

            void AnalyzeStatement(TSNode node, FlowState &state, int depth = 0)
            {
                    // See k_maxAstDepth in ASTUtils.h.
                    if (depth > k_maxAstDepth)
                        return;


                if (ts_node_is_null(node) || state.isTerminated || state.hasReturned)
                {
                    return;
                }

                std::string_view type = ts_node_type(node);

                if (type == "statement_block")
                {
                    uint32_t count = ts_node_named_child_count(node);
                    for (uint32_t i = 0; i < count && !state.isTerminated && !state.hasReturned; ++i)
                    {
                        AnalyzeStatement(ts_node_named_child(node, i), state, depth + 1);
                    }
                    return;
                }

                if (type == "expression_statement")
                {
                    if (ts_node_named_child_count(node) > 0)
                    {
                        CheckExpressionReads(ts_node_named_child(node, 0), state);
                    }
                    return;
                }

                if (type == "variable_declaration" || type == "declaration_statement")
                {
                    TSNode typeNode = ts_node_child_by_field_name(node, "var_type", 8);
                    if (ts_node_is_null(typeNode))
                    {
                        typeNode = ts_node_child_by_field_name(node, "type", 4);
                    }

                    bool isPrimitive = false;
                    if (!ts_node_is_null(typeNode))
                    {
                        TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, std::string(m_request.sourceCode));
                        isPrimitive = (IsPrimitiveTypeName(typeInfo.baseTypeName) && !typeInfo.isArray && !typeInfo.isHandle && typeInfo.templateName.empty());
                    }

                    uint32_t count = ts_node_named_child_count(node);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        TSNode child = ts_node_named_child(node, i);
                        std::string_view childType = ts_node_type(child);
                        if (childType == "variable_declarator")
                        {
                            TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
                            if (ts_node_is_null(nameNode) && ts_node_named_child_count(child) > 0)
                            {
                                nameNode = ts_node_named_child(child, 0);
                            }
                            if (ts_node_is_null(nameNode))
                            {
                                continue;
                            }

                            std::string varName = NodeText(nameNode, m_request.sourceCode);
                            TSNode initNode = ts_node_child_by_field_name(child, "value", 5);
                            if (ts_node_is_null(initNode))
                            {
                                initNode = ts_node_child_by_field_name(child, "initializer", 11);
                            }
                            if (ts_node_is_null(initNode))
                            {
                                uint32_t rawCount = ts_node_child_count(child);
                                bool sawEq = false;
                                for (uint32_t j = 0; j < rawCount; ++j)
                                {
                                    TSNode rawChild = ts_node_child(child, j);
                                    if (sawEq)
                                    {
                                        initNode = rawChild;
                                        break;
                                    }
                                    if (std::string_view(ts_node_type(rawChild)) == "=")
                                    {
                                        sawEq = true;
                                    }
                                }
                            }

                            if (!ts_node_is_null(initNode))
                            {
                                CheckExpressionReads(initNode, state);
                                state.assignedVars.insert(varName);
                            }
                            else
                            {
                                if (isPrimitive)
                                {
                                    m_trackedLocals.insert(varName);
                                }
                                else
                                {
                                    state.assignedVars.insert(varName);
                                }
                            }
                        }
                    }
                    return;
                }

                if (type == "if_statement")
                {
                    // `if_statement` has NO `condition` field - the expression is an unnamed child
                    // sitting before the `consequence`, which is a field. Asking for a field that
                    // does not exist returned null every time, so the condition was never analysed
                    // at all: reads inside it went unchecked, and - the visible half - an `&out`
                    // argument written there never marked its variable assigned. That is the
                    // `if (dict.get(key, value) && value != 0)` shape, which then reported `value`
                    // in the body. Same story in `while_statement`, below.
                    TSNode cond = ts_node_named_child(node, 0);
                    TSNode consequence = ts_node_child_by_field_name(node, "consequence", 11);
                    TSNode alternative = ts_node_child_by_field_name(node, "alternative", 11);

                    CheckExpressionReads(cond, state);

                    FlowState thenState = state;
                    AnalyzeStatement(consequence, thenState);

                    FlowState elseState = state;
                    if (!ts_node_is_null(alternative))
                    {
                        AnalyzeStatement(alternative, elseState);
                    }

                    if (thenState.hasReturned && elseState.hasReturned)
                    {
                        state.hasReturned = true;
                    }
                    else if (thenState.hasReturned)
                    {
                        state = elseState;
                    }
                    else if (elseState.hasReturned)
                    {
                        state = thenState;
                    }
                    else
                    {
                        state.assignedVars = MergeBranchStates(thenState.assignedVars, elseState.assignedVars);
                    }
                    return;
                }

                if (type == "while_statement")
                {
                    // No `condition` field either - see the note in the if_statement branch.
                    TSNode cond = ts_node_named_child(node, 0);
                    TSNode body = ts_node_child_by_field_name(node, "body", 4);

                    CheckExpressionReads(cond, state);

                    FlowState bodyState = state;
                    AnalyzeStatement(body, bodyState);

                    // The body's assignments survive the loop, whether or not the loop provably
                    // runs. That is AngelScript's rule and not C#'s - `int x; while (c) { x = 5; }
                    // Print(x);` is clean to the compiler, and so is the same shape with a `for`.
                    // Only propagating them for `while (true)` was the "definitely assigned"
                    // reading, and it accounted for most of what this rule reported over the
                    // corpus: assigning inside a loop and reading after it is how ordinary code is
                    // written.
                    state.assignedVars = MergeBranchStates(state.assignedVars, bodyState.assignedVars);

                    std::string condText = NodeText(cond, m_request.sourceCode);
                    if (condText == "true" && !bodyState.hasReturned && !bodyState.isTerminated)
                    {
                        state = bodyState;
                    }
                    return;
                }

                if (type == "do_while_statement")
                {
                    TSNode body = ts_node_child_by_field_name(node, "body", 4);
                    // No `condition` field here either; `body` is one, so the condition is the
                    // second named child. See the note in the if_statement branch.
                    TSNode cond = ts_node_named_child(node, 1);

                    AnalyzeStatement(body, state);
                    CheckExpressionReads(cond, state);
                    return;
                }

                if (type == "for_statement")
                {
                    // The field is `init`, not `initializer` - see for_statement in grammar.js.
                    // Asking for the wrong name returned null every time, so the loop header was
                    // never analysed at all and `for (i = 0; i < n; i++)` reported the `i` in the
                    // condition as a read of an uninitialised variable. The header is where `i` is
                    // initialised.
                    TSNode init = ts_node_child_by_field_name(node, "init", 4);
                    TSNode cond = ts_node_child_by_field_name(node, "condition", 9);
                    TSNode step = ts_node_child_by_field_name(node, "update", 6);
                    TSNode body = ts_node_child_by_field_name(node, "body", 4);

                    if (!ts_node_is_null(init))
                    {
                        // `for (i = 0; ...)` initialises a variable declared earlier, and its
                        // initializer is an ASSIGNMENT rather than a declaration - the grammar puts
                        // the expression straight in the field, with no statement around it.
                        // AnalyzeStatement had no case for that, so it recursed into the children
                        // and reported the `i` on the left as a read of an uninitialised variable:
                        // the loop header that initialises it. CheckExpressionReads is the branch
                        // that understands an assignment, so an expression goes there.
                        if (std::string_view(ts_node_type(init)) == "assignment_expression")
                        {
                            CheckExpressionReads(init, state);
                        }
                        else
                        {
                            AnalyzeStatement(init, state);
                        }
                    }

                    CheckExpressionReads(cond, state);

                    FlowState bodyState = state;
                    AnalyzeStatement(body, bodyState);
                    CheckExpressionReads(step, bodyState);

                    // See the note in the `while` branch: an assignment in the body precedes any
                    // read after the loop, and that is all the compiler asks for.
                    state.assignedVars = MergeBranchStates(state.assignedVars, bodyState.assignedVars);

                    std::string rawCond = NodeText(cond, m_request.sourceCode);
                    std::string_view condText = Trim(rawCond);
                    if ((condText.empty() || condText == "true" || condText == ";") && !bodyState.hasReturned && !bodyState.isTerminated)
                    {
                        state = bodyState;
                    }
                    return;
                }

                if (type == "switch_statement")
                {
                    // First named child, for the same reason as the others: the switched
                    // expression carries no field name.
                    TSNode cond = ts_node_named_child(node, 0);
                    CheckExpressionReads(cond, state);

                    bool hasDefault = false;
                    std::vector<FlowState> caseStates;
                    uint32_t count = ts_node_named_child_count(node);

                    for (uint32_t i = 0; i < count; ++i)
                    {
                        TSNode child = ts_node_named_child(node, i);
                        if (std::string_view(ts_node_type(child)) == "case_clause")
                        {
                            TSNode kw = ts_node_child(child, 0);
                            if (std::string_view(ts_node_type(kw)) == "default")
                            {
                                hasDefault = true;
                            }

                            FlowState caseState = state;
                            uint32_t stmtCount = ts_node_named_child_count(child);
                            uint32_t first = (std::string_view(ts_node_type(kw)) == "default") ? 0u : 1u;
                            for (uint32_t j = first; j < stmtCount && !caseState.hasReturned; ++j)
                            {
                                TSNode stmtChild = ts_node_named_child(child, j);
                                if (std::string_view(ts_node_type(stmtChild)) == "break_statement")
                                {
                                    break;
                                }
                                AnalyzeStatement(stmtChild, caseState);
                            }
                            caseStates.push_back(std::move(caseState));
                        }
                    }

                    // No `hasDefault` requirement, for the same reason the if/else join unions
                    // rather than intersects: a `default:` matters to "assigned on every path",
                    // which is not the question. Measured - both of these are clean:
                    //
                    //     int x; switch (v) { case 1: x=10; break; case 2: x=20; break; } Print(x);
                    //     int x; switch (v) { case 1: x=10; break; case 2: break;
                    //                         default: x=30; break; } Print(x);
                    //
                    // An assignment in one arm precedes the read, and that is all the compiler
                    // asks. Two tests asserted the opposite and have been inverted with these
                    // lines recorded beside them.
                    if (!caseStates.empty())
                    {
                        bool allReturn = true;
                        std::vector<ankerl::unordered_dense::set<std::string>> liveCaseSets;

                        for (const auto &cs : caseStates)
                        {
                            if (!cs.hasReturned)
                            {
                                allReturn = false;
                                liveCaseSets.push_back(cs.assignedVars);
                            }
                        }

                        if (allReturn)
                        {
                            state.hasReturned = true;
                        }
                        else if (!liveCaseSets.empty())
                        {
                            ankerl::unordered_dense::set<std::string> merged = liveCaseSets[0];
                            for (size_t i = 1; i < liveCaseSets.size(); ++i)
                            {
                                merged = MergeBranchStates(merged, liveCaseSets[i]);
                            }
                            state.assignedVars = std::move(merged);
                        }
                    }
                    return;
                }

                if (type == "return_statement")
                {
                    if (ts_node_named_child_count(node) > 0)
                    {
                        CheckExpressionReads(ts_node_named_child(node, 0), state);
                    }
                    state.hasReturned = true;
                    state.isTerminated = true;
                    return;
                }

                if (type == "break_statement" || type == "continue_statement")
                {
                    state.isTerminated = true;
                    return;
                }

                CheckExpressionReads(node, state);
            }
        };

        void TraverseFunctions(TSNode node, DefiniteAssignmentVisitor &visitor, int depth = 0)
        {
            // See k_maxAstDepth in ASTUtils.h.
            if (depth > k_maxAstDepth)
                return;


            std::string_view type = ts_node_type(node);
            if (type == "func_declaration" || type == "lambda_expression")
            {
                visitor.AnalyzeFunction(node);
            }

            uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TraverseFunctions(ts_node_named_child(node, i), visitor, depth + 1);
            }
        }
    }

    void CheckDefiniteAssignment(const DefiniteAssignmentCheckRequest &request, DiagnosticContext &ctx)
    {
        if (ts_node_is_null(request.root) || request.sourceCode.empty())
        {
            return;
        }

        if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
        {
            return;
        }

        DefiniteAssignmentVisitor visitor(request, ctx);
        TraverseFunctions(request.root, visitor);
    }
}
