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

        ankerl::unordered_dense::set<std::string> IntersectSets(
            const ankerl::unordered_dense::set<std::string> &a,
            const ankerl::unordered_dense::set<std::string> &b)
        {
            ankerl::unordered_dense::set<std::string> result;
            for (const auto &item : a)
            {
                if (b.contains(item))
                {
                    result.insert(item);
                }
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
                            m_ctx.EmitAtRange(start.row, start.column, end.row, end.column,
                                              "as-err-uninitialized-variable-read", name);
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
                        bool isOutParam = false;
                        if (sig && i < sig->parameters.size())
                        {
                            const auto &param = sig->parameters[i];
                            if (param.modifier == ParameterModifier::Out ||
                                param.typeName.find("&out") != std::string::npos ||
                                param.rawText.find("&out") != std::string::npos)
                            {
                                isOutParam = true;
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
                    TSNode cond = ts_node_child_by_field_name(node, "condition", 9);
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
                        state.assignedVars = IntersectSets(thenState.assignedVars, elseState.assignedVars);
                    }
                    return;
                }

                if (type == "while_statement")
                {
                    TSNode cond = ts_node_child_by_field_name(node, "condition", 9);
                    TSNode body = ts_node_child_by_field_name(node, "body", 4);

                    CheckExpressionReads(cond, state);

                    FlowState bodyState = state;
                    AnalyzeStatement(body, bodyState);

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
                    TSNode cond = ts_node_child_by_field_name(node, "condition", 9);

                    AnalyzeStatement(body, state);
                    CheckExpressionReads(cond, state);
                    return;
                }

                if (type == "for_statement")
                {
                    TSNode init = ts_node_child_by_field_name(node, "initializer", 11);
                    TSNode cond = ts_node_child_by_field_name(node, "condition", 9);
                    TSNode step = ts_node_child_by_field_name(node, "update", 6);
                    TSNode body = ts_node_child_by_field_name(node, "body", 4);

                    if (!ts_node_is_null(init))
                    {
                        AnalyzeStatement(init, state);
                    }

                    CheckExpressionReads(cond, state);

                    FlowState bodyState = state;
                    AnalyzeStatement(body, bodyState);
                    CheckExpressionReads(step, bodyState);

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
                    TSNode cond = ts_node_child_by_field_name(node, "condition", 9);
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

                    if (hasDefault && !caseStates.empty())
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
                                merged = IntersectSets(merged, liveCaseSets[i]);
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
