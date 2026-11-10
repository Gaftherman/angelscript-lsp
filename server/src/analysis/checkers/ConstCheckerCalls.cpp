#include "analysis/checkers/ConstCheckerCommon.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <variant>

namespace angel_lsp::analysis
{

/**
 * @brief Checks whether a method signature returns a reference to its owning type.
 *        Methods returning `Type&` (e.g., `string& ToLowercase()`) are fluent / chaining operations.
 * @param[in] fn Function signature being inspected.
 * @param[in] owner Name of the owning class.
 * @return True if the method returns a reference to the owning class.
 */
static bool IsSelfReferenceReturn(const FunctionSignature& fn, const std::string& owner)
{
    const bool isRef = fn.modifiers.isReturnReference || IsReferenceTypeView(fn.returnType);
    return isRef && (CleanBaseType(fn.returnType) == owner || CleanBaseType(fn.returnBaseTypeName) == owner);
}

/**
 * @brief Looks for a method by name across a type's whole visible hierarchy.
 * @param[in] typeName Name of the type being inspected.
 * @param[in] methodName Name of the method being looked up.
 * @param[in] table Symbol table for hierarchical lookup.
 * @param[in,out] cache Optional expression type cache.
 * @return MemberLookupResult describing whether a const overload exists.
 */
static MemberLookupResult FindMethod(const std::string& typeName, const std::string& methodName,
                                     const SymbolTable& table, ExpressionTypeCache* cache = nullptr)
{
    MemberLookupResult result;
    if (typeName.empty() || methodName.empty())
    {
        return result;
    }

    for (const auto& owner : GetInheritedTypeHierarchy(typeName, table, cache))
    {
        const auto candidates = table.FindMemberSymbolPtr(owner, methodName);
        if (!candidates)
        {
            continue;
        }

        for (const auto& sym : *candidates)
        {
            if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
            {
                continue;
            }

            if (!result.found)
            {
                result.found = true;
                result.declaringClass = owner;
            }
            const auto& fn = sym.GetFunction();
            result.hasConstOverload = result.hasConstOverload || fn.modifiers.isConst;
            result.returnsSelfReference = result.returnsSelfReference || IsSelfReferenceReturn(fn, owner);
        }
    }
    return result;
}

/**
 * @brief Validates method calls on const receivers to ensure a const overload is available.
 * @param[in] node AST node of the call expression.
 * @param[in] request Contextual request bundle containing AST root and source text.
 * @param[in] scope Lexical scope active at the call site.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckMethodCall(TSNode node, const AstCheckContext& request, const Scope* scope, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode callee = parser::GetChildByField(node, gs.fieldFunction);
    if (ts_node_is_null(callee) || ts_node_symbol(callee) != gs.symMemberExpression)
    {
        return;
    }

    TSNode objectNode = parser::GetChildByField(callee, gs.fieldObject);
    TSNode memberNode = parser::GetChildByField(callee, gs.fieldMember);
    if (ts_node_is_null(objectNode) || ts_node_is_null(memberNode))
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const ExpressionTypeContext constCtx(scope, ctx);
    if (ResolveConstness(objectNode, constCtx) != Constness::Const)
    {
        return;
    }

    const std::string objectType = CleanBaseType(ResolveExpressionType(objectNode, ExpressionTypeContext(scope, ctx)));
    if (objectType.empty() || !HierarchyIsFullyVisible(objectType, table))
    {
        return;
    }

    const std::string methodName(parser::NodeText(memberNode, request.sourceCode));
    const MemberLookupResult method = FindMethod(objectType, methodName, table, ctx.request.exprCache);
    if (!method.found || method.hasConstOverload || method.returnsSelfReference)
    {
        return;
    }

    ctx.EmitAtNodeRange(ToOptionalSourceRange(memberNode), "as-err-const-method-required",
                        std::string(LastScopeSegment(method.declaringClass)), methodName);
}

} // namespace angel_lsp::analysis
