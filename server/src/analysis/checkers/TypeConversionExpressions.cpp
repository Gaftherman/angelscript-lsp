#include "analysis/checkers/BinaryOperatorChecker.h"
#include "analysis/checkers/ComparisonOperatorChecker.h"
#include "analysis/checkers/HandleComparisonChecker.h"
#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/ExpressionSiteFacts.h"
#include "analysis/resolution/AmbiguityDetector.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/IllegalHandleCheck.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

void ProcessBinarySite(const BinarySiteFact& site, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    if (!request.expressionSites || !request.binaryFacts)
    {
        return;
    }
    const BinaryExpressionFact* fact = request.binaryFacts->Find(site.bytes);
    if (!fact)
    {
        return;
    }
    const Scope* scope = ResolveNodeScope(request.expressionSites->RangeOf(site.bytes), request);

    if (fact->opClass == BinaryOperatorClass::Identity)
    {
        CheckHandleComparison(*fact, scope, ctx);
        return;
    }

    if (fact->opClass == BinaryOperatorClass::Equality || fact->opClass == BinaryOperatorClass::Relational)
    {
        CheckSignedUnsignedComparison(*fact, scope, ctx);
        CheckHandleComparison(*fact, scope, ctx);
        CheckComparisonOperatorCompatibility(*fact, scope, ctx);
        return;
    }

    CheckBinaryOperatorCompatibility(*fact, scope, ctx);
}

static bool CheckUnaryInvalidOperand(const UnaryPostfixSiteFact& site, const ExpressionSiteFacts& facts,
                                     const Scope* scope, DiagnosticContext& ctx)
{
    if (!site.hasOperand)
    {
        return false;
    }
    const ExpressionTypeContext exprCtx(scope, ctx);
    if (const auto dt = IsBareDataType(site.operandBytes, exprCtx))
    {
        EmitExpressionIsDataType(ConversionExpressionFact{site.operandBytes, facts.RangeOf(site.operandBytes),
                                                          facts.TextOf(site.operandBytes)},
                                 ctx, *dt);
        return true;
    }
    if (site.isAddressOfHandle)
    {
        return false;
    }
    if (IsFunctionReference(site.operandBytes, exprCtx))
    {
        ctx.EmitAtRange(facts.RangeOf(site.bytes), diagnostics::codes::IllegalOperation);
        return true;
    }
    return false;
}

/**
 * @brief Maps a unary or postfix operator spelling to the class method that implements it.
 * @param[in] site Unary or postfix expression site fact.
 * @return Method name such as `opNeg` or `opPostInc`, or empty when the operator is not overloadable.
 */
static std::string_view UnaryOperatorMethod(const UnaryPostfixSiteFact& site) noexcept
{
    if (site.isIncrementOrDecrement)
    {
        const bool inc = site.op == UnaryPostfixOp::PlusPlus;
        if (site.isPostfix)
        {
            return inc ? "opPostInc" : "opPostDec";
        }
        return inc ? "opPreInc" : "opPreDec";
    }
    if (site.isPostfix)
    {
        return {};
    }
    return site.op == UnaryPostfixOp::Minus ? "opNeg"
                                            : (site.op == UnaryPostfixOp::Tilde ? "opCom" : std::string_view{});
}

static void CheckPrefixUnaryOperator(const UnaryPostfixSiteFact& site, const ExpressionSiteFacts& facts,
                                     const Scope* scope, DiagnosticContext& ctx)
{
    const std::string_view opMethod = UnaryOperatorMethod(site);
    if (opMethod.empty() || !site.hasOperand || site.operandIsNumber)
    {
        return;
    }
    const std::string rawType = ResolveExpressionType(site.operandBytes, ExpressionTypeContext(scope, ctx));
    const SymbolTable& table = ctx.request.symbolTable;
    const std::string cleanType = UnwrapTypedef(CleanBaseType(rawType), table);
    if (cleanType.empty() || cleanType == "auto" || IsVariableTypeToken(cleanType) ||
        IsBuiltInValueType(cleanType, ctx) || ResolvesToEnum(cleanType, table) ||
        cleanType.find('<') != std::string::npos || rawType.find('[') != std::string::npos ||
        ctx.request.IsRegisteredSymbol(cleanType) || !HierarchyIsFullyVisible(cleanType, table))
    {
        return;
    }

    const bool allowsImplConv = !site.isIncrementOrDecrement;
    bool foundClass = false;
    for (const auto& ancestor : GetInheritedTypeHierarchy(cleanType, table))
    {
        if (const auto syms = table.FindSymbolsPtr(ancestor))
        {
            for (const auto& s : *syms)
            {
                if (s.type == SymbolType::Class || s.type == SymbolType::Interface)
                {
                    foundClass = true;
                    if ((!ctx.request.predefinedFileExtension.empty() &&
                         s.fileUri.ends_with(ctx.request.predefinedFileExtension)) ||
                        (s.type == SymbolType::Class && s.GetClass().isTemplate))
                    {
                        return;
                    }
                }
            }
        }
        if (const auto ops = table.FindMemberSymbolPtr(ancestor, opMethod))
        {
            for (const auto& sym : *ops)
            {
                if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature) &&
                    sym.GetFunction().parameters.empty())
                {
                    return;
                }
            }
        }
        if (const auto convs = allowsImplConv ? table.FindMemberSymbolPtr(ancestor, "opImplConv") : nullptr)
        {
            for (const auto& sym : *convs)
            {
                if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature) &&
                    sym.GetFunction().parameters.empty())
                {
                    const std::string ret = UnwrapTypedef(CleanBaseType(sym.GetFunction().returnType), table);
                    if (IsNumericPrimitive(ret) || ResolvesToEnum(ret, table))
                    {
                        return;
                    }
                }
            }
        }
    }
    if (foundClass)
    {
        ctx.EmitAtRange(facts.RangeOf(site.bytes), diagnostics::codes::UnaryOperatorNotFound, opMethod);
    }
}

/**
 * @brief Reports `as-err-handle-on-primitive` or `as-err-handle-on-no-handle-type` when unary handle-of `@x`
 *        is applied to a primitive or configured no-handle operand.
 * @param[in] site Unary or postfix expression site fact.
 * @param[in] facts Document expression facts owning range and property pools.
 * @param[in] scope Innermost lexical scope at `site.bytes`.
 * @param[in,out] ctx Diagnostic sink.
 */
static void CheckAddressOfHandleOnPrimitive(const UnaryPostfixSiteFact& site, const ExpressionSiteFacts& facts,
                                            const Scope* scope, DiagnosticContext& ctx)
{
    if (site.isPostfix || !site.isAddressOfHandle || !site.hasOperand)
    {
        return;
    }
    const std::string rawType = ResolveExpressionType(site.operandBytes, ExpressionTypeContext(scope, ctx));
    if (rawType.empty())
    {
        return;
    }
    std::string_view stripped = StripLeadingConst(rawType);
    TrimTypeWhitespace(stripped);
    stripped = StripTrailingDecorationsView(stripped, false);
    TrimTypeWhitespace(stripped);
    const std::string unwrapped = UnwrapTypedef(stripped, ctx.request.symbolTable);
    const std::string_view enclosingNs = scope ? scope->GetEnclosingNamespace() : std::string_view{};
    const IllegalHandleCheckResult handleResult = CheckIllegalHandleType(
        IllegalHandleCheckRequest{
            .baseTypeName = unwrapped,
            .enclosingNs = enclosingNs,
            .isAddressOfOperand = true,
        },
        ctx);
    if (handleResult.HasError())
    {
        ctx.EmitAtRange(facts.RangeOf(site.bytes), handleResult.diagnosticCode, handleResult.offendingTypeName);
    }
}

void ProcessUnaryOrPostfixSite(const UnaryPostfixSiteFact& site, const TypeConversionCheckRequest& request,
                               DiagnosticContext& ctx)
{
    if (!request.expressionSites)
    {
        return;
    }
    const auto& facts = *request.expressionSites;
    const SourceRange siteRange = facts.RangeOf(site.bytes);
    const Scope* scope = ResolveNodeScope(siteRange, request);
    if (CheckUnaryInvalidOperand(site, facts, scope, ctx))
    {
        return;
    }

    CheckAddressOfHandleOnPrimitive(site, facts, scope, ctx);
    CheckPrefixUnaryOperator(site, facts, scope, ctx);

    if (!site.isIncrementOrDecrement || !site.hasOperand)
    {
        return;
    }

    const PropertyAccessInfo pInfo = InspectPropertyAccess(facts.OperandPropertyOf(site), scope, ctx);
    if (site.operandIsIndexed)
    {
        if (pInfo.isProperty && pInfo.isIndexed)
        {
            ctx.EmitAtRange(siteRange, "as-err-inc-dec-on-virtual-prop", pInfo.propName);
        }
    }
    else if (pInfo.isProperty)
    {
        ctx.EmitAtRange(siteRange, "as-err-inc-dec-on-virtual-prop", pInfo.propName);
    }
}

/**
 * @brief Reports a member access that reads a write-only property.
 * @param[in] site Member-expression fact; call callees and write or mutation sites are skipped.
 * @param[in] request Check request giving access to the scope tree.
 * @param[in,out] ctx Diagnostic sink that receives as-err-write-only-property.
 */
void ProcessMemberExpressionSite(const MemberExpressionSiteFact& site, const TypeConversionCheckRequest& request,
                                 DiagnosticContext& ctx)
{
    if (site.isCallCallee || site.isWriteOrMutation || !request.expressionSites)
    {
        return;
    }

    const SourceRange siteRange = request.expressionSites->RangeOf(site.bytes);
    const Scope* scope = ResolveNodeScope(siteRange, request);
    const PropertyAccessInfo pInfo = InspectPropertyAccess(request.expressionSites->PropertyAccessOf(site), scope, ctx);
    if (pInfo.isProperty && !pInfo.hasGet && pInfo.hasSet)
    {
        ctx.EmitAtRange(siteRange, "as-err-write-only-property", pInfo.propName);
    }
}

static std::string FormatOpIndexCall(std::string_view owner, const std::vector<std::string>& argTypes)
{
    std::string out(owner);
    out += "::opIndex(";
    for (size_t i = 0; i < argTypes.size(); ++i)
    {
        if (i > 0)
            out += ", ";
        out += argTypes[i].empty() ? "?" : argTypes[i];
    }
    return out + ")";
}

/**
 * @brief Collects the indexing candidates a type offers for `argCount` index arguments.
 *        `opIndex` takes the arguments as written. The index property accessors `get_opIndex` and `set_opIndex`
 *        index as well: the setter's last parameter is the assigned value, so it is not an index argument.
 * @param[in] owner Type whose hierarchy is searched.
 * @param[in] normalizedObj Full receiver type, used to bind template arguments.
 * @param[in] argCount Number of index arguments in the expression.
 * @param[in] table Symbol table to search.
 * @param[out] outOverloads Candidates whose arity admits `argCount`, with template parameters substituted.
 * @return Number of indexing members found before the arity filter; zero means the type is not indexable.
 */
static size_t CollectMatchingOpIndexOverloads(std::string_view owner, std::string_view normalizedObj, uint32_t argCount,
                                              const SymbolTable& table, std::vector<Symbol>& outOverloads)
{
    static constexpr std::string_view k_indexMembers[] = {"opIndex", "get_opIndex", "set_opIndex"};

    TemplateBinding binding;
    if (normalizedObj.find('<') != std::string::npos)
        binding = BindTemplateArguments(normalizedObj, table);

    size_t totalOpIndex = 0;
    for (const auto& cls : GetInheritedTypeHierarchy(owner, table))
    {
        for (const auto memberName : k_indexMembers)
        {
            const auto found = table.FindMemberSymbolPtr(cls, memberName);
            if (!found)
                continue;
            for (const auto& sym : *found)
            {
                if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
                    continue;
                ++totalOpIndex;
                Symbol copy = sym;
                if (memberName == "set_opIndex" && !copy.GetFunction().parameters.empty())
                    copy.GetFunction().parameters.pop_back();
                const auto arity = InspectFunctionArity(copy.GetFunction());
                if (argCount < arity.requiredParams || (!arity.isVariadic && argCount > arity.maxParams))
                    continue;
                if (binding.usable)
                {
                    for (auto& param : copy.GetFunction().parameters)
                        param.typeName = SubstituteTemplateParameters(param.typeName, binding);
                }
                outOverloads.push_back(std::move(copy));
            }
        }
    }
    return totalOpIndex;
}

static void ValidateOpIndexInvocation(const IndexExpressionSiteFact& site, const TypeConversionCheckRequest& request,
                                      const Scope* scope, DiagnosticContext& ctx)
{
    if (!request.expressionSites)
        return;
    const auto args = request.expressionSites->IndexArgsOf(site);
    if (site.hasSyntaxError || site.objectBytes.endByte == 0 || args.empty())
        return;

    const ExpressionTypeContext exprCtx(scope, ctx);
    if (IsBareDataType(site.objectBytes, exprCtx) || IsFunctionReference(site.objectBytes, exprCtx))
        return;

    const std::string rawObjType = ResolveExpressionType(site.objectBytes, exprCtx);
    if (rawObjType.empty())
        return;

    const SymbolTable& table = ctx.request.symbolTable;
    const std::string_view arrayTypeName = ctx.request.GetEffectiveArrayTypeName();
    const std::string normalizedObj = UnwrapTypedef(NormalizeType(rawObjType, arrayTypeName), table);
    if (normalizedObj.empty() || normalizedObj == "auto" || IsVariableTypeToken(normalizedObj) ||
        ctx.request.IsRegisteredSymbol(normalizedObj))
        return;

    const SourceRange siteRange = request.expressionSites->RangeOf(site.bytes);
    if (IsPrimitiveTypeName(normalizedObj) || ResolvesToEnum(normalizedObj, table) ||
        FindFuncdef(normalizedObj, table).has_value())
    {
        ctx.EmitAtRange(siteRange, diagnostics::codes::TypeNotIndexable, normalizedObj);
        return;
    }

    std::string owner = MemberOwnerType(normalizedObj, arrayTypeName);
    if (owner.empty())
        owner = CleanBaseType(normalizedObj);
    if (ctx.request.IsRegisteredSymbol(owner) || !HierarchyIsFullyVisible(owner, table))
        return;

    std::vector<Symbol> overloads;
    if (CollectMatchingOpIndexOverloads(owner, normalizedObj, static_cast<uint32_t>(args.size()), table, overloads) ==
        0)
    {
        ctx.EmitAtRange(siteRange, diagnostics::codes::TypeNotIndexable, owner);
        return;
    }

    std::vector<std::string> argTypes;
    argTypes.reserve(args.size());
    bool allResolved = true;
    for (const IndexedByteRange argBytes : args)
    {
        std::string argType = ResolveExpressionType(argBytes, exprCtx);
        const std::string_view cleanArg = CleanBaseTypeView(argType);
        if (argType.empty() || cleanArg == "auto" || IsVariableTypeToken(cleanArg) ||
            ctx.request.IsRegisteredSymbol(cleanArg))
            allResolved = false;
        argTypes.push_back(std::move(argType));
    }

    if (overloads.empty())
    {
        ctx.EmitAtRange(siteRange, diagnostics::codes::CallNoMatchingSignature, FormatOpIndexCall(owner, argTypes));
        return;
    }
    if (!allResolved)
        return;

    std::vector<const Symbol*> matchingArity;
    matchingArity.reserve(overloads.size());
    for (const auto& s : overloads)
        matchingArity.push_back(&s);

    const OverloadMatchResult match = ResolveBestOverload(matchingArity, argTypes, table);
    // Const and non-const opIndex pairs share their parameters, so several viable candidates without a
    // single best one is a valid container, not a mismatch.
    if (match.viableCandidates.empty())
        ctx.EmitAtRange(siteRange, diagnostics::codes::CallNoMatchingSignature, FormatOpIndexCall(owner, argTypes));
}

/**
 * @brief Reports an index expression that reads a write-only property or calls an invalid opIndex.
 * @param[in] site Index-expression fact.
 * @param[in] request Check request giving access to the scope tree.
 * @param[in,out] ctx Diagnostic sink.
 */
void ProcessIndexExpressionSite(const IndexExpressionSiteFact& site, const TypeConversionCheckRequest& request,
                                DiagnosticContext& ctx)
{
    if (!request.expressionSites)
    {
        return;
    }
    const SourceRange siteRange = request.expressionSites->RangeOf(site.bytes);
    const Scope* scope = ResolveNodeScope(siteRange, request);
    const PropertyAccessInfo pInfo = InspectPropertyAccess(request.expressionSites->ObjectPropertyOf(site), scope, ctx);
    if (pInfo.isProperty && pInfo.isIndexed)
    {
        if (!site.isWriteOrMutation && !pInfo.hasGet && pInfo.hasSet)
        {
            ctx.EmitAtRange(siteRange, "as-err-write-only-property", pInfo.propName);
        }
        return;
    }

    ValidateOpIndexInvocation(site, request, scope, ctx);
}

static bool CheckCastVisibilityGuards(const ConversionTypePair& types, bool sourceIsScalar, bool targetIsScalar,
                                      const DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    if (!targetIsScalar && !FindTypeDeclaration(types.to, table).found)
    {
        return false;
    }
    if (!sourceIsScalar && !FindTypeDeclaration(types.from, table).found)
    {
        return false;
    }
    if (ctx.request.IsRegisteredSymbol(types.from) || ctx.request.IsRegisteredSymbol(types.to))
    {
        return false;
    }
    return true;
}

void ProcessCastSite(const CastExpressionSiteFact& site, const TypeConversionCheckRequest& request,
                     DiagnosticContext& ctx)
{
    if (!request.expressionSites || !site.hasValue)
    {
        return;
    }
    const auto& facts = *request.expressionSites;
    const auto targetName = CleanBaseType(facts.TextOf(site.targetTypeBytes));
    if (targetName.empty())
    {
        return;
    }

    const SourceRange siteRange = facts.RangeOf(site.bytes);
    const Scope* scope = ResolveNodeScope(siteRange, request);
    const SymbolTable& table = ctx.request.symbolTable;
    const bool targetIsScalar = IsScalarCastTarget(targetName, table);

    const ExpressionType source = ResolveValueType(facts.ValueOf(site), scope, ctx);
    if (!source.known || source.baseName.empty())
    {
        return;
    }

    if (targetIsScalar)
    {
        if (!ctx.request.IsRegisteredSymbol(source.baseName) && !ctx.request.IsRegisteredSymbol(targetName))
        {
            ctx.EmitAtRange(siteRange, "as-err-invalid-cast", {source.baseName, targetName});
        }
        return;
    }

    if (IsSameType(source.baseName, targetName))
    {
        return;
    }

    const bool sourceIsScalar = IsScalarCastTarget(source.baseName, table);
    if (!CheckCastVisibilityGuards({source.baseName, targetName}, sourceIsScalar, targetIsScalar, ctx))
    {
        return;
    }

    if (AreHierarchyRelated(source.baseName, targetName, table))
    {
        return;
    }

    if (!sourceIsScalar && !targetIsScalar)
    {
        return;
    }

    if (DeclaresAnyCastOperator(source.baseName, table) || DeclaresAnyCastOperator(targetName, table))
    {
        return;
    }

    ctx.EmitAtRange(siteRange, "as-err-invalid-cast", {source.baseName, targetName});
}

} // namespace angel_lsp::analysis
