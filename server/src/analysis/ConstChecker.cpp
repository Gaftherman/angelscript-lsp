#include "analysis/ConstChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/SemanticHelpers.h"
#include "utils/Utils.h"

#include <algorithm>
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

        constexpr uint32_t k_objectFieldLength = 6;   ///< "object"
        constexpr uint32_t k_memberFieldLength = 6;   ///< "member"
        constexpr uint32_t k_leftFieldLength = 4;     ///< "left"
        constexpr uint32_t k_functionFieldLength = 8; ///< "function"

        /** @brief Strips a namespace qualification, leaving the last segment ("G::A" -> "A"). */
        std::string LastScopeSegment(const std::string &name)
        {
            const size_t pos = name.rfind("::");
            return pos == std::string::npos ? name : name.substr(pos + 2);
        }

        /**
         * @brief True when a declared type's written form begins with `const`.
         *
         * The grammar puts `const` first in a type, and both the scope tree and the symbol table
         * keep the type's source text, so this is what a declaration's constness looks like from
         * here. `const Entity@` counts: in AngelScript that is a handle to a read-only object,
         * which is exactly the case the engine refuses a mutation through.
         */
        bool TypeTextIsConst(std::string_view typeText)
        {
            while (!typeText.empty() && (typeText.front() == ' ' || typeText.front() == '\t'))
            {
                typeText.remove_prefix(1);
            }
            return typeText.starts_with("const ") || typeText == "const";
        }

        /**
         * @brief Constness of a parameter, read off the enclosing declaration's parameter list.
         *
         * The scope tree records a parameter as a definition but not its written type - it fills
         * that in from a variable_declarator, which a parameter is not - so the answer has to come
         * from the tree. Kept here rather than added to LocalDefinition on purpose: typeName there
         * is documented as meaningful only for a variable, and populating it for parameters would
         * quietly change what isHandleType and typeKind mean for every other reader of the scope
         * tree, including the null-assignment rule.
         *
         * @return True only when a parameter of that name was found and its type begins with const;
         *         `found` says whether the question was answerable at all.
         */
        bool ParameterIsConst(TSNode node, const std::string &name, std::string_view sourceCode, bool &found)
        {
            found = false;

            TSNode owner = node;
            while (!ts_node_is_null(owner))
            {
                const std::string_view ownerType = ts_node_type(owner);
                if (ownerType == "func_declaration" || ownerType == "lambda_expression" ||
                    ownerType == "virtual_property" || ownerType == "accessor")
                {
                    break;
                }
                owner = ts_node_parent(owner);
            }
            if (ts_node_is_null(owner))
            {
                return false;
            }

            TSNode parameters = ts_node_child_by_field_name(owner, "parameters", 10);
            if (ts_node_is_null(parameters))
            {
                return false;
            }

            const uint32_t count = ts_node_named_child_count(parameters);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode parameter = ts_node_named_child(parameters, i);
                if (std::string_view(ts_node_type(parameter)) != "parameter")
                {
                    continue;
                }

                TSNode nameNode = ts_node_child_by_field_name(parameter, "name", 4);
                if (ts_node_is_null(nameNode) || NodeText(nameNode, sourceCode) != name)
                {
                    continue;
                }

                found = true;
                TSNode typeNode = ts_node_child_by_field_name(parameter, "param_type", 10);
                return !ts_node_is_null(typeNode) && TypeTextIsConst(NodeText(typeNode, sourceCode));
            }
            return false;
        }

        /** @brief What one expression is, as far as constness goes. */
        enum class Constness
        {
            Unknown,    ///< Nothing could be established. Stay silent.
            Mutable,
            Const
        };

        Constness ResolveConstness(TSNode node,
                                   const Scope *scope,
                                   const SymbolTable &table,
                                   std::string_view sourceCode,
                                   int depth = 0);

        /** @brief Constness of a plain name, whether it is a local, a parameter or a global. */
        Constness ResolveNameConstness(const std::string &name,
                                       TSNode node,
                                       const Scope *scope,
                                       const SymbolTable &table,
                                       std::string_view sourceCode)
        {
            if (name.empty())
            {
                return Constness::Unknown;
            }

            // A local or a parameter first, since either shadows a global of the same name.
            if (scope)
            {
                if (const LocalDefinition *def = ResolveInScope(scope, name))
                {
                    if (!def->typeName.empty())
                    {
                        return TypeTextIsConst(def->typeName) ? Constness::Const : Constness::Mutable;
                    }

                    // No recorded type means either a parameter, whose type the scope tree does not
                    // keep, or a foreach variable, whose type is not written anywhere at all. The
                    // first is answerable from the tree; the second is not, and Unknown is the only
                    // honest answer for it.
                    bool found = false;
                    const bool isConst = ParameterIsConst(node, name, sourceCode, found);
                    if (found)
                    {
                        return isConst ? Constness::Const : Constness::Mutable;
                    }
                    return Constness::Unknown;
                }
            }

            const auto symbols = table.FindSymbolsPtr(name);
            if (!symbols)
            {
                return Constness::Unknown;
            }

            for (const auto &sym : *symbols)
            {
                if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
                {
                    return sym.GetVariable().modifiers.isConst ? Constness::Const : Constness::Mutable;
                }
            }
            return Constness::Unknown;
        }

        Constness ResolveConstness(TSNode node,
                                   const Scope *scope,
                                   const SymbolTable &table,
                                   std::string_view sourceCode,
                                   int depth)
        {
            // See k_maxAstDepth in ASTUtils.h.
            if (depth > k_maxAstDepth)
            {
                return Constness::Unknown;
            }

            if (ts_node_is_null(node))
            {
                return Constness::Unknown;
            }

            const std::string_view nodeType = ts_node_type(node);

            if (nodeType == "identifier")
            {
                return ResolveNameConstness(NodeText(node, sourceCode), node, scope, table, sourceCode);
            }

            if (nodeType == "scoped_identifier")
            {
                // Asked for whole first, the way ResolveExpressionType does, since a qualified name
                // is the key the collector stored it under.
                const std::string whole = NodeText(node, sourceCode);
                const Constness qualified = ResolveNameConstness(whole, node, scope, table, sourceCode);
                if (qualified != Constness::Unknown)
                {
                    return qualified;
                }
                return ResolveNameConstness(LastScopeSegment(whole), node, scope, table, sourceCode);
            }

            // A member of a const object is const, which is what makes `e.field = 1` through a
            // `const Entity &in` an error. A member of a mutable object is judged on its own
            // declaration - and a const member is already reported where it is declared, so there
            // is nothing to conclude from one here.
            if (nodeType == "member_expression")
            {
                const Constness object = ResolveConstness(
                    ts_node_child_by_field_name(node, "object", k_objectFieldLength), scope, table, sourceCode, depth + 1);
                return object == Constness::Const ? Constness::Const : Constness::Unknown;
            }

            if (nodeType == "parenthesized_expression")
            {
                return ts_node_named_child_count(node) > 0
                           ? ResolveConstness(ts_node_named_child(node, 0), scope, table, sourceCode, depth + 1)
                           : Constness::Unknown;
            }

            // An index into a const container, a cast, a call result: each has an answer, and none
            // of them is one this pass can reach without more of the engine's rules than it has.
            // Unknown keeps the caller quiet, which is the only safe direction.
            return Constness::Unknown;
        }

        /** @brief What the pass concluded about a method name looked up on a type. */
        struct MethodLookup
        {
            bool found = false;        ///< False means no declaration was visible. Stay silent.
            bool hasConstOverload = false;
            std::string declaringClass;
        };

        /**
         * @brief Looks for a method by name across a type's whole visible hierarchy.
         *
         * Answers whether *any* declaration of that name is const, because that is the question the
         * engine asks. Its message - "No matching signatures to 'Entity::Mutate() const'" - is a
         * failed overload lookup, not a refusal of the call, so one const member in the set makes
         * the call legal and this pass silent.
         */
        MethodLookup FindMethod(const std::string &typeName,
                                const std::string &methodName,
                                const SymbolTable &table)
        {
            MethodLookup result;
            if (typeName.empty() || methodName.empty())
            {
                return result;
            }

            for (const auto &owner : GetInheritedTypeHierarchy(typeName, table))
            {
                const auto candidates = table.FindSymbolsPtr(owner + "::" + methodName);
                if (!candidates)
                {
                    continue;
                }

                for (const auto &sym : *candidates)
                {
                    if (sym.type != SymbolType::Function ||
                        !std::holds_alternative<FunctionSignature>(sym.signature))
                    {
                        continue;
                    }

                    if (!result.found)
                    {
                        result.found = true;
                        result.declaringClass = owner;
                    }
                    result.hasConstOverload = result.hasConstOverload || sym.GetFunction().modifiers.isConst;
                }
            }
            return result;
        }

        void EmitAtNode(TSNode node, DiagnosticContext &ctx, std::string_view code,
                        const std::string &arg1, const std::string &arg2)
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            ctx.EmitAtRange(start.row, start.column, end.row, end.column, code, arg1, arg2);
        }

        bool TypeTextIsHandleConst(std::string_view typeText)
        {
            while (!typeText.empty() && (typeText.front() == ' ' || typeText.front() == '\t'))
            {
                typeText.remove_prefix(1);
            }
            while (!typeText.empty() && (typeText.back() == ' ' || typeText.back() == '\t'))
            {
                typeText.remove_suffix(1);
            }
            return typeText.ends_with(" const");
        }

        Constness ResolveHandleConstness(TSNode node,
                                         const Scope *scope,
                                         const SymbolTable &table,
                                         std::string_view sourceCode)
        {
            if (ts_node_is_null(node))
            {
                return Constness::Unknown;
            }

            const std::string_view nodeType = ts_node_type(node);
            if (nodeType == "identifier" || nodeType == "scoped_identifier")
            {
                const std::string name = NodeText(node, sourceCode);
                if (scope)
                {
                    if (const LocalDefinition *def = ResolveInScope(scope, LastScopeSegment(name)))
                    {
                        if (!def->typeName.empty())
                        {
                            return TypeTextIsHandleConst(def->typeName) ? Constness::Const : Constness::Mutable;
                        }
                    }
                }
                if (const auto symbols = table.FindSymbolsPtr(name))
                {
                    for (const auto &sym : *symbols)
                    {
                        if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
                        {
                            return TypeTextIsHandleConst(sym.GetVariable().typeName) ? Constness::Const : Constness::Mutable;
                        }
                    }
                }
            }
            return Constness::Unknown;
        }

        void CheckAssignment(TSNode node, const ConstCheckRequest &request,
                             const Scope *scope, DiagnosticContext &ctx)
        {
            TSNode target = ts_node_child_by_field_name(node, "left", k_leftFieldLength);
            if (ts_node_is_null(target))
            {
                return;
            }

            bool isHandleAssignment = false;
            TSNode actualTarget = target;
            if (std::string_view(ts_node_type(target)) == "unary_expression")
            {
                TSNode opNode = ts_node_child_by_field_name(target, "operator", 8);
                if (!ts_node_is_null(opNode) && NodeText(opNode, request.sourceCode) == "@")
                {
                    isHandleAssignment = true;
                    TSNode operand = ts_node_child_by_field_name(target, "operand", 7);
                    if (!ts_node_is_null(operand))
                    {
                        actualTarget = operand;
                    }
                }
            }

            if (isHandleAssignment)
            {
                if (ResolveHandleConstness(actualTarget, scope, ctx.request.symbolTable, request.sourceCode) == Constness::Const)
                {
                    const std::string written = NodeText(target, request.sourceCode);
                    ctx.EmitAtRange(ts_node_start_point(target).row, ts_node_start_point(target).column,
                                    ts_node_end_point(target).row, ts_node_end_point(target).column,
                                    "as-err-const-assignment", written);
                }
                return;
            }

            if (ResolveConstness(target, scope, ctx.request.symbolTable, request.sourceCode) != Constness::Const)
            {
                return;
            }

            const std::string written = NodeText(target, request.sourceCode);
            ctx.EmitAtRange(ts_node_start_point(target).row, ts_node_start_point(target).column,
                            ts_node_end_point(target).row, ts_node_end_point(target).column,
                            "as-err-const-assignment", written);
        }

        void CheckMethodCall(TSNode node, const ConstCheckRequest &request,
                             const Scope *scope, DiagnosticContext &ctx)
        {
            TSNode callee = ts_node_child_by_field_name(node, "function", k_functionFieldLength);
            if (ts_node_is_null(callee) || std::string_view(ts_node_type(callee)) != "member_expression")
            {
                return;
            }

            TSNode objectNode = ts_node_child_by_field_name(callee, "object", k_objectFieldLength);
            TSNode memberNode = ts_node_child_by_field_name(callee, "member", k_memberFieldLength);
            if (ts_node_is_null(objectNode) || ts_node_is_null(memberNode))
            {
                return;
            }

            const SymbolTable &table = ctx.request.symbolTable;
            if (ResolveConstness(objectNode, scope, table, request.sourceCode) != Constness::Const)
            {
                return;
            }

            const std::string objectType = CleanBaseType(ResolveExpressionType(
                objectNode, scope, table, request.sourceCode, ctx.request.fileUri));
            if (objectType.empty() || !HierarchyIsFullyVisible(objectType, table))
            {
                return;
            }

            const std::string methodName = NodeText(memberNode, request.sourceCode);
            const MethodLookup method = FindMethod(objectType, methodName, table);
            if (!method.found || method.hasConstOverload)
            {
                return;
            }

            EmitAtNode(memberNode, ctx, "as-err-const-method-required",
                       LastScopeSegment(method.declaringClass), methodName);
        }

        void VisitNode(TSNode node, const ConstCheckRequest &request, DiagnosticContext &ctx, int depth = 0)
                {
            // Pathologically nested source would otherwise recurse until the stack gives out; see
            // k_maxAstDepth in ASTUtils.h.
            if (depth > k_maxAstDepth)
                return;

            const std::string_view nodeType = ts_node_type(node);
            if (nodeType == "assignment_expression" || nodeType == "call_expression")
            {
                const TSPoint start = ts_node_start_point(node);
                const Scope *scope = FindInnermostScope(request.scopeRoot, start.row, start.column);
                if (nodeType == "assignment_expression")
                {
                    CheckAssignment(node, request, scope, ctx);
                }
                else
                {
                    CheckMethodCall(node, request, scope, ctx);
                }
            }

            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                VisitNode(ts_node_named_child(node, i), request, ctx, depth + 1);
            }
        }
    }

    void CheckConstCorrectness(const ConstCheckRequest &request, DiagnosticContext &ctx)
    {
        if (ts_node_is_null(request.root) || request.sourceCode.empty())
        {
            return;
        }

        // A stub describes an API rather than using one, so it has no expressions worth judging -
        // the same exemption every other use-site pass carries.
        if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
        {
            return;
        }

        VisitNode(request.root, request, ctx);
    }
}
