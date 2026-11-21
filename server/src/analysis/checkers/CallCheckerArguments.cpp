#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/checkers/InitializerListChecker.h"
#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/resolution/AmbiguityDetector.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

bool ValidateArgumentOrdering(std::span<const CallArgumentFact> arguments, bool& sawNamedArg, DiagnosticContext& ctx)
{
    sawNamedArg = false;
    for (const auto& arg : arguments)
    {
        if (arg.isNamed)
        {
            sawNamedArg = true;
        }
        else if (sawNamedArg)
        {
            ctx.EmitAtRange(arg.range, "as-err-positional-after-named-arg");
            return false;
        }
    }
    return true;
}

static bool IsAssignableLValueSymbol(std::string_view name, const Scope* scope, const SymbolTable& table,
                                     uint32_t refByte = UINT32_MAX)
{
    if (scope)
    {
        const auto* def = ResolveInScopeAt(scope, name, refByte);
        if (def && (def->kind == LocalDefinitionKind::Variable || def->kind == LocalDefinitionKind::Parameter ||
                    def->kind == LocalDefinitionKind::Field))
        {
            return !HasConstModifier(def->typeName);
        }
    }
    auto syms = table.FindSymbolsPtr(name);
    if (syms)
    {
        for (const auto& s : *syms)
        {
            if (s.type == SymbolType::Variable && !s.GetVariable().modifiers.isConst)
            {
                return true;
            }
        }
    }
    return false;
}

static bool CheckArgIsLValue(const CallArgumentFact& arg, const Scope* scope, const SymbolTable& table,
                             std::string_view sourceCode)
{
    std::string_view text = CallSiteFacts::SliceText(sourceCode, arg.bytes);
    if (arg.isThis || text == parser::keywords::This)
    {
        return true;
    }
    TrimTypeWhitespace(text);
    if (text.starts_with('@'))
    {
        text.remove_prefix(1);
        TrimTypeWhitespace(text);
        if (text == parser::keywords::This)
        {
            return true;
        }
        if (text.ends_with(']'))
        {
            return true;
        }
        if (text.find('.') != std::string_view::npos)
        {
            return true;
        }
        return IsAssignableLValueSymbol(text, scope, table, arg.bytes.startByte);
    }
    if (arg.kind == CallArgumentKind::Identifier)
    {
        const auto name = CallSiteFacts::SliceText(sourceCode, arg.operandBytes);
        return IsAssignableLValueSymbol(name, scope, table, arg.operandBytes.startByte);
    }
    if (arg.kind == CallArgumentKind::Member)
    {
        return true;
    }
    if (text.ends_with(']'))
    {
        return true;
    }
    return false;
}

CallResolutionContext ResolveCallArguments(std::span<const CallArgumentFact> arguments, const Scope* scope,
                                           DiagnosticContext& ctx, const NodeIndex* nodeIndex)
{
    CallResolutionContext result;
    const size_t argCount = arguments.size();
    result.argTypes.reserve(argCount);
    result.argBindings.reserve(argCount);
    result.argNames.reserve(argCount);

    const ExpressionTypeContext exprCtx =
        (ctx.request.nodeIndex || !nodeIndex)
            ? ExpressionTypeContext(scope, ctx)
            : ExpressionTypeContext(scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.fileUri,
                                    nodeIndex);

    for (const auto& arg : arguments)
    {
        result.argNames.emplace_back(CallSiteFacts::SliceText(ctx.request.sourceCode, arg.nameBytes));
        const bool isRefCast = arg.kind == CallArgumentKind::ReferenceCast;
        const bool isLValue =
            !isRefCast && CheckArgIsLValue(arg, scope, ctx.request.symbolTable, ctx.request.sourceCode);
        result.argBindings.push_back(isLValue ? ArgumentReferenceBinding::LValue : ArgumentReferenceBinding::RValue);

        std::string_view trimmedArg = CallSiteFacts::SliceText(ctx.request.sourceCode, arg.bytes);
        TrimTypeWhitespace(trimmedArg);
        if (parser::primitives::IsVoidType(trimmedArg))
        {
            result.argTypes.push_back(std::string(parser::keywords::Void));
            continue;
        }

        const std::optional<std::string> dtOpt = IsBareDataType(arg.bytes, exprCtx);
        std::string_view dataTypeName;
        if (dtOpt)
        {
            dataTypeName = *dtOpt;
        }
        else if (IsPrimitiveTypeName(trimmedArg))
        {
            dataTypeName = trimmedArg;
        }

        if (!dataTypeName.empty())
        {
            if (parser::primitives::IsVoidType(dataTypeName))
            {
                result.argTypes.push_back(std::string(parser::keywords::Void));
                continue;
            }
            ctx.EmitAtRange(arg.range, diagnostics::codes::ExpressionIsDataType, dataTypeName);
            result.allArgsResolved = false;
            result.argTypes.push_back("");
            continue;
        }

        std::string argType = ResolveExpressionType(arg.bytes, exprCtx);
        if (argType.empty())
        {
            if (arg.isNullLiteral)
            {
                argType = parser::keywords::Null;
            }
            else if (arg.kind == CallArgumentKind::InitializerList)
            {
                argType = constants::k_initializerListTypeSentinel;
            }
            else
            {
                result.allArgsResolved = false;
            }
        }
        else if (!HasConstModifier(argType) &&
                 IsObjectOperandConst(arg.bytes, ctx.request.sourceCode, scope, ctx.request.symbolTable))
        {
            PrependConst(argType);
        }
        if (isRefCast && !argType.empty())
        {
            const bool isConst =
                arg.hasConstCastTarget || HasConstModifier(ResolveExpressionType(arg.operandBytes, exprCtx)) ||
                IsObjectOperandConst(arg.operandBytes, ctx.request.sourceCode, scope, ctx.request.symbolTable);
            if (isConst && !HasConstModifier(argType))
            {
                PrependConst(argType);
            }
            if (!HasConstModifier(argType) && !IsPrimitiveTypeName(argType))
            {
                result.argBindings.back() = ArgumentReferenceBinding::ReferenceCast;
            }
        }
        result.argTypes.push_back(std::move(argType));
    }
    return result;
}

std::vector<const Symbol*> FilterMatchingArityCandidates(const std::vector<Symbol>& candidates, uint32_t argumentCount)
{
    std::vector<const Symbol*> matching;
    matching.reserve(candidates.size());
    for (const auto& sym : candidates)
    {
        if (std::holds_alternative<FunctionSignature>(sym.signature))
        {
            const auto arity = InspectFunctionArity(sym.GetFunction());
            if (IsArityCompatible(arity, argumentCount))
            {
                matching.push_back(&sym);
            }
        }
    }
    return matching;
}

void CheckInitializerListArgs(std::span<const CallArgumentFact> arguments, const CallResolutionContext& args,
                              const FunctionSignature& fn, const InitializerListFacts& initFacts, const Scope* scope,
                              DiagnosticContext& ctx)
{
    for (size_t i = 0; i < arguments.size(); ++i)
    {
        if (arguments[i].kind != CallArgumentKind::InitializerList)
        {
            continue;
        }

        const std::string_view argName =
            (i < args.argNames.size()) ? std::string_view(args.argNames[i]) : std::string_view{};
        size_t paramIdx = size_t(-1);
        if (!argName.empty())
        {
            for (size_t p = 0; p < fn.parameters.size(); ++p)
            {
                if (fn.parameters[p].name == argName)
                {
                    paramIdx = p;
                    break;
                }
            }
        }
        else if (i < fn.parameters.size())
        {
            paramIdx = i;
        }

        if (paramIdx < fn.parameters.size())
        {
            if (const auto* listFact = initFacts.FindList(arguments[i].bytes))
            {
                CheckInitializerListAgainstType(
                    InitializerListTargetRequest{
                        *listFact, initFacts, fn.parameters[paramIdx].typeName, {ctx.request.sourceCode, scope}},
                    ctx);
            }
        }
    }
}

void ValidateOutArguments(const Symbol& candidate, std::span<const CallArgumentFact> arguments,
                          std::span<const ArgumentReferenceBinding> bindings, DiagnosticContext& ctx)
{
    const auto& fn = candidate.GetFunction();
    for (size_t i = 0; i < arguments.size() && i < fn.parameters.size(); ++i)
    {
        const auto& param = fn.parameters[i];
        const bool isMutableRef = !param.isConst && (param.isOutRef || param.isInOutRef || param.isStandaloneRef);
        if (!isMutableRef)
        {
            continue;
        }

        std::string_view aText = CallSiteFacts::SliceText(ctx.request.sourceCode, arguments[i].bytes);
        TrimTypeWhitespace(aText);
        if (parser::primitives::IsVoidType(aText) && (param.isOutRef || param.modifier == ParameterModifier::Out))
        {
            continue;
        }

        if (!CanBindMutableReference(bindings[i], param))
        {
            ctx.EmitAtRange(arguments[i].range, "as-err-lvalue-required-for-out-param");
        }
    }
}

} // namespace angel_lsp::analysis
