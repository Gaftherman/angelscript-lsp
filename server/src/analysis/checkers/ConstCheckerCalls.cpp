#include "analysis/checkers/ConstCheckerCommon.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <string_view>
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
static bool IsSelfReferenceReturn(const FunctionSignature& fn, std::string_view owner)
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
static MemberLookupResult FindMethod(std::string_view typeName, std::string_view methodName, const SymbolTable& table,
                                     ExpressionTypeCache* cache = nullptr)
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

void CheckMethodCall(const ConstMethodCallFact& call, const ConstSiteFacts& facts, const Scope* scope,
                     DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    const ExpressionTypeContext constCtx(scope, ctx);
    if (ResolveConstness(call, facts, constCtx) != Constness::Const)
    {
        return;
    }

    const std::string objectType =
        CleanBaseType(ResolveExpressionType(call.object.bytes, ExpressionTypeContext(scope, ctx)));
    if (objectType.empty() || !HierarchyIsFullyVisible(objectType, table))
    {
        return;
    }

    const MemberLookupResult method = FindMethod(objectType, call.methodName, table, ctx.request.exprCache);
    if (!method.found || method.hasConstOverload || method.returnsSelfReference)
    {
        return;
    }

    ctx.EmitAtRange(call.methodRange, "as-err-const-method-required", LastScopeSegment(method.declaringClass),
                    call.methodName);
}

} // namespace angel_lsp::analysis
