#include "analysis/LValueChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/SemanticHelpers.h"
#include "utils/Utils.h"
#include <string>
#include <string_view>
#include <vector>
#include "parser/GrammarNames.h"

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


        void EmitAtNode(TSNode node, DiagnosticContext &ctx, std::string_view code,
                        const std::string &arg1 = "", const std::string &arg2 = "")
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            ctx.EmitAtRange(start.row, start.column, end.row, end.column, code, arg1, arg2);
        }

        void CheckCallLValue(TSNode callNode, const LValueCheckRequest &request,
                             const Scope *scope, DiagnosticContext &ctx)
        {
            TSNode funcNode = parser::GetChildByField(callNode, parser::fields::Function);
            if (ts_node_is_null(funcNode) && ts_node_child_count(callNode) > 0)
            {
                funcNode = ts_node_child(callNode, 0);
            }
            if (ts_node_is_null(funcNode))
            {
                return;
            }

            std::vector<Symbol> candidates;
            std::string_view funcNodeType = ts_node_type(funcNode);

            if (funcNodeType == "member_expression")
            {
                TSNode objNode = parser::GetChildByField(funcNode, parser::fields::Object);
                TSNode memNode = parser::GetChildByField(funcNode, parser::fields::Member);
                if (!ts_node_is_null(objNode) && !ts_node_is_null(memNode))
                {
                    std::string objType = ResolveExpressionType(
                        objNode, scope, ctx.request.symbolTable, request.sourceCode, ctx.request.fileUri);
                    std::string cleanObj = CleanBaseType(objType);
                    std::string memName = NodeText(memNode, request.sourceCode);

                    if (!cleanObj.empty() && !memName.empty())
                    {
                        auto hierarchy = GetInheritedTypeHierarchy(cleanObj, ctx.request.symbolTable);
                        for (const auto &typeName : hierarchy)
                        {
                            auto found = ctx.request.symbolTable.FindSymbols(typeName + "::" + memName);
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
            }
            else if (funcNodeType == "scoped_identifier")
            {
                std::string qName = NodeText(funcNode, request.sourceCode);
                auto found = ctx.request.symbolTable.FindSymbols(qName);
                for (const auto &sym : found)
                {
                    if (sym.type == SymbolType::Function)
                    {
                        candidates.push_back(sym);
                    }
                }
            }
            else if (funcNodeType == "identifier")
            {
                std::string name = NodeText(funcNode, request.sourceCode);
                auto inScope = FindSymbolsInScope(name, funcNode, request.sourceCode, ctx.request.symbolTable);
                for (const auto &sym : inScope)
                {
                    if (sym.type == SymbolType::Function)
                    {
                        candidates.push_back(sym);
                    }
                }
            }

            if (candidates.empty())
            {
                return;
            }

            bool allVoid = true;
            bool anyReference = false;

            for (const auto &sym : candidates)
            {
                if (!std::holds_alternative<FunctionSignature>(sym.signature))
                {
                    continue;
                }

                const auto &fn = sym.GetFunction();
                std::string retClean = CleanBaseType(fn.returnType);
                if (fn.returnTypeKind == TypeKind::Void || retClean == "void")
                {
                    // void return
                }
                else
                {
                    allVoid = false;
                }

                if (fn.modifiers.isReturnReference ||
                    (!fn.returnType.empty() && fn.returnType.back() == '&'))
                {
                    anyReference = true;
                }
            }

            if (allVoid)
            {
                EmitAtNode(callNode, ctx, "as-err-assign-void");
            }
            else if (!anyReference)
            {
                EmitAtNode(callNode, ctx, "as-err-assign-non-ref-call");
            }
        }

        bool IsAssignableLValueNode(TSNode node, const Scope *scope, const LValueCheckRequest &request, const SymbolTable &table, int depth = 0)
        {
            // See k_maxAstDepth in ASTUtils.h.
            if (depth > k_maxAstDepth)
                return false;


            if (ts_node_is_null(node))
            {
                return false;
            }

            std::string_view nodeType = ts_node_type(node);

            // Parenthesized expression
            if (nodeType == "parenthesized_expression")
            {
                uint32_t count = ts_node_named_child_count(node);
                if (count > 0)
                {
                    return IsAssignableLValueNode(ts_node_named_child(node, 0), scope, request, table, depth + 1);
                }
                for (uint32_t i = 0; i < ts_node_child_count(node); ++i)
                {
                    TSNode ch = ts_node_child(node, i);
                    std::string_view ct = ts_node_type(ch);
                    if (ct != "(" && ct != ")")
                    {
                        return IsAssignableLValueNode(ch, scope, request, table, depth + 1);
                    }
                }
                return false;
            }

            // Ternary expression (e.g. cond ? a : b)
            if (nodeType == "ternary_expression")
            {
                TSNode consequence = parser::GetChildByField(node, parser::fields::Consequence);
                TSNode alternative = parser::GetChildByField(node, parser::fields::Alternative);
                if (ts_node_is_null(consequence) || ts_node_is_null(alternative))
                {
                    if (ts_node_named_child_count(node) >= 3)
                    {
                        consequence = ts_node_named_child(node, 1);
                        alternative = ts_node_named_child(node, 2);
                    }
                }
                return IsAssignableLValueNode(consequence, scope, request, table, depth + 1) &&
                       IsAssignableLValueNode(alternative, scope, request, table, depth + 1);
            }

            // Literals are not lvalues
            if (nodeType == "number_literal" || nodeType == "string_literal" ||
                nodeType == "boolean_literal" || nodeType == "null_literal")
            {
                return false;
            }

            // Unary expression: only @handle is lvalue
            if (nodeType == "unary_expression")
            {
                TSNode opNode = parser::GetChildByField(node, parser::fields::Operator);
                if (!ts_node_is_null(opNode) && NodeText(opNode, request.sourceCode) == "@")
                {
                    TSNode operand = parser::GetChildByField(node, parser::fields::Operand);
                    return IsAssignableLValueNode(operand, scope, request, table, depth + 1);
                }
                return false;
            }

            // Binary and cast expressions are not lvalues
            if (nodeType == "binary_expression" || nodeType == "cast_expression")
            {
                return false;
            }

            // Member access, indexing, subscripts
            if (nodeType == "member_expression" || nodeType == "index_expression")
            {
                return true;
            }

            // Identifiers
            if (nodeType == "identifier" || nodeType == "scoped_identifier")
            {
                std::string name = NodeText(node, request.sourceCode);
                if (scope)
                {
                    const LocalDefinition *localDef = ResolveInScope(scope, name);
                    if (localDef)
                    {
                        if (localDef->kind == LocalDefinitionKind::Function ||
                            localDef->kind == LocalDefinitionKind::Method)
                        {
                            return false;
                        }
                        return true;
                    }
                }

                auto symbols = table.FindSymbols(name);
                if (!symbols.empty())
                {
                    bool onlyFunctions = true;
                    for (const auto &sym : symbols)
                    {
                        if (sym.type != SymbolType::Function && sym.type != SymbolType::Funcdef)
                        {
                            onlyFunctions = false;
                            break;
                        }
                    }
                    if (onlyFunctions)
                    {
                        return false;
                    }
                }
                return true;
            }

            return false;
        }

        void CheckAssignmentTarget(TSNode node, const LValueCheckRequest &request,
                                   const Scope *scope, DiagnosticContext &ctx)
        {
            TSNode target = parser::GetChildByField(node, parser::fields::Left);
            if (ts_node_is_null(target))
            {
                return;
            }

            std::string_view targetType = ts_node_type(target);
            if (targetType == "call_expression")
            {
                CheckCallLValue(target, request, scope, ctx);
                return;
            }

            if (!IsAssignableLValueNode(target, scope, request, ctx.request.symbolTable))
            {
                EmitAtNode(target, ctx, "as-err-not-lvalue");
            }
        }

        void VisitNode(TSNode node, const LValueCheckRequest &request, DiagnosticContext &ctx, int depth = 0)
                {
            // Pathologically nested source would otherwise recurse until the stack gives out; see
            // k_maxAstDepth in ASTUtils.h.
            if (depth > k_maxAstDepth)
                return;

            std::string_view nodeType = ts_node_type(node);
            if (nodeType == "assignment_expression")
            {
                const TSPoint start = ts_node_start_point(node);
                const Scope *scope = FindInnermostScope(request.scopeRoot, start.row, start.column);
                CheckAssignmentTarget(node, request, scope, ctx);
            }

            uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                VisitNode(ts_node_named_child(node, i), request, ctx, depth + 1);
            }
        }
    }

    void CheckLValues(const LValueCheckRequest &request, DiagnosticContext &ctx)
    {
        if (ts_node_is_null(request.root) || request.sourceCode.empty())
        {
            return;
        }

        if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
        {
            return;
        }

        VisitNode(request.root, request, ctx);
    }
}
