#include "analysis/OverloadResolver.h"
#include "analysis/SemanticHelpers.h"
#include "utils/MultiFileLogger.h"
#include <algorithm>
#include <initializer_list>
#include <optional>

namespace angel_lsp::analysis
{
namespace
{
std::string_view TrimWhitespace(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    {
        s.remove_suffix(1);
    }
    return s;
}

bool StripReferenceSuffix(std::string& s)
{
    static constexpr std::string_view kRefSuffixes[] = {"&in", "&out", "&inout", "& in", "& out", "& inout"};
    for (const auto& suffix : kRefSuffixes)
    {
        if (s.ends_with(suffix))
        {
            const size_t amp = s.rfind('&');
            if (amp != std::string::npos)
            {
                s.resize(amp);
                return true;
            }
        }
    }
    return false;
}

std::string StripTypeDecorations(std::string result)
{
    bool modified = true;
    while (modified)
    {
        modified = false;
        while (!result.empty() &&
               (result.back() == '@' || result.back() == '&' || result.back() == ' ' || result.back() == '\t'))
        {
            result.pop_back();
            modified = true;
        }
        if (result.ends_with(" const"))
        {
            result.resize(result.size() - 6);
            modified = true;
        }
        if (StripReferenceSuffix(result))
        {
            modified = true;
        }
    }
    return result;
}

std::string DesugarArrayBrackets(std::string result)
{
    while (result.ends_with("[]"))
    {
        result = "array<" + result.substr(0, result.size() - 2) + ">";
    }
    return result;
}

std::string NormalizeType(std::string_view typeName)
{
    typeName = TrimWhitespace(typeName);
    if (typeName.starts_with("const "))
    {
        typeName.remove_prefix(6);
    }
    typeName = TrimWhitespace(typeName);

    std::string result = StripTypeDecorations(std::string(typeName));
    while (!result.empty() && (result.back() == ' ' || result.back() == '\t'))
    {
        result.pop_back();
    }

    result = CanonicalizeType(result);
    return DesugarArrayBrackets(std::move(result));
}

bool HasHandleModifier(std::string_view typeName)
{
    return typeName.find('@') != std::string_view::npos;
}

bool HasConvertingConstructor(const std::string& fromType, const std::string& toType, const SymbolTable& symbolTable)
{
    auto toSyms = symbolTable.FindSymbolsPtr(toType + "::" + toType);
    if (!toSyms)
    {
        return false;
    }
    for (const auto& sym : *toSyms)
    {
        if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
        {
            continue;
        }
        const auto& sig = sym.GetFunction();
        if (sig.modifiers.isExplicit || sig.modifiers.isDelete || sig.parameters.empty())
        {
            continue;
        }
        if (NormalizeType(sig.parameters[0].typeName) != fromType)
        {
            continue;
        }
        const bool remainingDefault = std::all_of(sig.parameters.begin() + 1, sig.parameters.end(),
                                                  [](const auto& p) { return !p.defaultValue.empty(); });
        if (remainingDefault)
        {
            return true;
        }
    }
    return false;
}

bool HasConversionMethod(const std::string& fromType, const std::string& toType, const SymbolTable& symbolTable)
{
    for (const char* opName : {"opImplConv", "opImplCast"})
    {
        auto opSyms = symbolTable.FindSymbolsPtr(fromType + "::" + opName);
        if (!opSyms)
        {
            continue;
        }
        for (const auto& sym : *opSyms)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                if (NormalizeType(sym.GetFunction().returnType) == toType)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool HasUserConversion(const std::string& fromType, const std::string& toType, const SymbolTable& symbolTable)
{
    if (fromType.empty() || toType.empty())
    {
        return false;
    }
    return HasConvertingConstructor(fromType, toType, symbolTable) ||
           HasConversionMethod(fromType, toType, symbolTable);
}

/** @brief How deep a template argument may nest before unwrapping gives up. */
constexpr int k_maxTypedefDepth = 8;

/**
 * @brief Splits `int, array<string>` into its top-level arguments.
 *
 * Depth-counted rather than split on every comma: `dictionary<string, array<int>>` has two
 * arguments and three commas, and a plain split would produce `array<int` as a type name.
 */
std::vector<std::string> SplitTemplateArguments(std::string_view arguments)
{
    std::vector<std::string> parts;
    int depth = 0;
    size_t start = 0;

    for (size_t i = 0; i < arguments.size(); ++i)
    {
        const char c = arguments[i];
        if (c == '<')
        {
            ++depth;
        }
        else if (c == '>')
        {
            --depth;
        }
        else if (c == ',' && depth == 0)
        {
            parts.emplace_back(arguments.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.emplace_back(arguments.substr(start));
    return parts;
}

std::string UnwrapTypedef(const std::string& typeName, const SymbolTable& symbolTable, int depth = 0)
{
    std::string current = NormalizeType(typeName);
    const auto syms = symbolTable.FindSymbolsPtr(current);
    if (syms)
    {
        for (const auto& s : *syms)
        {
            if (s.type == SymbolType::Typedef && std::holds_alternative<TypedefSignature>(s.signature))
            {
                return NormalizeType(s.GetTypedef().baseType);
            }
        }
    }

    // A typedef names a primitive, and a primitive is often used as a template argument.
    // For example, `typedef uint8 byte;` makes `array<byte>` and `array<uint8>` the same
    // instantiation, and the compiler accepts a call between them in either direction.
    // Unwrapping template arguments ensures typedef aliases match their canonical base types.
    //
    // Rebuilding also canonicalises the separator, so `array<int,string>` and
    // `array<int, string>` stop being different types to a string comparison.
    if (depth >= k_maxTypedefDepth)
    {
        return current;
    }

    const size_t open = current.find('<');
    if (open == std::string::npos || open == 0 || current.back() != '>')
    {
        return current;
    }

    std::string rebuilt = current.substr(0, open) + "<";
    const std::string inner = current.substr(open + 1, current.size() - open - 2);
    bool first = true;
    for (const auto& argument : SplitTemplateArguments(inner))
    {
        if (!first)
        {
            rebuilt += ", ";
        }
        first = false;
        rebuilt += UnwrapTypedef(argument, symbolTable, depth + 1);
    }
    return rebuilt + ">";
}
} // namespace

// Both of these normalise first and then defer to the shared classifiers in SemanticHelpers.h.
// They used to carry their own lists, and those lists had drifted from the conversion rules':
// neither knew about `int32` or `uint32`, the explicit spellings of `int` and `uint`, which a
// source file may legitimately write and which nothing canonicalises away.
//
// No user-visible symptom was found for that divergence, and the honest reason is that the
// silent-unless-fully-visible policy absorbed it: an argument written as `int32` simply scored
// as having no viable candidate, and a call whose overloads cannot be resolved is passed over
// rather than reported. So it cost a diagnostic rather than producing a wrong one. Sharing the
// vocabulary is still worth doing - three lists of the same primitives, each maintained
// separately, is how the next one gains a real symptom.

/** @brief True for AngelScript's integer primitives, signed or unsigned. */
bool IsIntegerType(const std::string& typeName)
{
    return IsIntegerPrimitive(NormalizeType(typeName));
}

/**
 * @brief True for AngelScript's unsigned integer primitives.
 *
 * Both spellings of each width, for the reason IsIntegerPrimitive gives: the parser hands back
 * whichever the source wrote, so a classifier that knew only `uint` would answer differently
 * for `uint32`.
 */
bool IsUnsignedInteger(const std::string& typeName)
{
    const std::string normalized = NormalizeType(typeName);
    return normalized == "uint" || normalized == "uint8" || normalized == "uint16" || normalized == "uint32" ||
           normalized == "uint64";
}

/** @brief True for AngelScript's floating point primitives. */
bool IsFloatingPointType(const std::string& typeName)
{
    return IsFloatingPointPrimitive(NormalizeType(typeName));
}

/**
 * @brief Safe implicit numeric conversions, as the AngelScript compiler actually performs them.
 *
 * Signed/unsigned pairings are included on purpose. They were missing, and the gap was real:
 * `array<int> a(1)` passes a literal `int` to `array(uint initialSize)`, and with int -> uint
 * absent from this table that scored Incompatible and produced "No matching signatures to
 * 'array<int>(int)'" on entirely ordinary code. The real compiler accepts every case here -
 * verified against it directly, not read off the spec.
 *
 * The explicit `int32`/`uint32` spellings are listed beside `int`/`uint` because a source file
 * may write either and nothing canonicalises them away.
 *
 * @warning This is not the table for `as-warn-signed-unsigned-mismatch`, and reusing it there
 *          would reintroduce the bug above. This table answers "does the compiler accept the
 *          conversion", and it does; the warning answers "does the compiler complain while
 *          accepting it", and it complains about exactly the signed/unsigned pairs listed here
 *          as safe - but only across a comparison operator, and only when neither side folds
 *          to a constant. Two questions, two tables. See the numeric-warning section of
 *          TypeConversionChecker.cpp.
 */
bool IsInTypeList(std::string_view target, std::initializer_list<std::string_view> validTypes)
{
    return std::find(validTypes.begin(), validTypes.end(), target) != validTypes.end();
}

bool CheckWideningTarget(std::string_view from, std::string_view to)
{
    if (from == "int8")
    {
        return IsInTypeList(
            to, {"int16", "int", "int32", "int64", "uint8", "uint16", "uint", "uint32", "uint64", "float", "double"});
    }
    if (from == "uint8")
    {
        return IsInTypeList(to, {"uint16", "int16", "uint", "int", "uint64", "int64", "float", "double"});
    }
    if (from == "int16")
    {
        return IsInTypeList(to, {"int", "int32", "int64", "uint16", "uint", "uint32", "uint64", "float", "double"});
    }
    if (from == "uint16")
    {
        return IsInTypeList(to, {"uint", "int", "uint64", "int64", "float", "double"});
    }
    if (from == "int" || from == "int32")
    {
        return IsInTypeList(to, {"int32", "int", "int64", "uint", "uint32", "uint64", "float", "double"});
    }
    if (from == "uint" || from == "uint32")
    {
        return IsInTypeList(to, {"uint32", "uint", "uint64", "int", "int32", "int64", "float", "double"});
    }
    if (from == "int64" || from == "uint64")
    {
        return IsInTypeList(to, {"int64", "uint64", "double"});
    }
    return from == "float" && to == "double";
}

bool IsPrimitiveWidening(const std::string& fromType, const std::string& toType)
{
    const std::string from = NormalizeType(fromType);
    const std::string to = NormalizeType(toType);

    if (from == to || from == "bool" || to == "bool")
    {
        return false;
    }

    return CheckWideningTarget(from, to);
}

bool IsPrimitiveNarrowing(const std::string& fromType, const std::string& toType)
{
    const std::string from = NormalizeType(fromType);
    const std::string to = NormalizeType(toType);

    if (from == to)
    {
        return false;
    }

    const auto isNumeric = [](const std::string& t) { return IsNumericPrimitive(t); };

    if (isNumeric(from) && isNumeric(to))
    {
        return !IsPrimitiveWidening(from, to);
    }
    // `bool` against a number reaches here and falls through, which is the point - see the
    // note in IsPrimitiveWidening. It used to answer true in both directions, and narrowing is
    // a penalty rather than a refusal, so the pairing scored as viable. The compiler's answer
    // is "No matching signatures", so the honest score is Incompatible, and that is what the
    // caller produces when neither this nor IsPrimitiveWidening claims the pair.
    // IsNumericPrimitive excludes bool, so nothing above can have claimed it.
    return false;
}

/**
 * @brief True when two candidates declare the same signature, parameter for parameter.
 *
 * Used to tell a genuine overload ambiguity apart from the same declaration arriving twice,
 * which is what happens when two stubs describing the same standard library are both loaded.
 */
bool HasSameParameterList(const Symbol& left, const Symbol& right)
{
    if (!std::holds_alternative<FunctionSignature>(left.signature) ||
        !std::holds_alternative<FunctionSignature>(right.signature))
    {
        return false;
    }

    const auto& a = left.GetFunction();
    const auto& b = right.GetFunction();

    if (a.parameters.size() != b.parameters.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.parameters.size(); ++i)
    {
        if (NormalizeType(a.parameters[i].typeName) != NormalizeType(b.parameters[i].typeName))
        {
            return false;
        }
        if (a.parameters[i].modifier != b.parameters[i].modifier)
        {
            return false;
        }
    }
    return true;
}

bool HasSameSignature(const Symbol& left, const Symbol& right)
{
    if (!std::holds_alternative<FunctionSignature>(left.signature) ||
        !std::holds_alternative<FunctionSignature>(right.signature))
    {
        return false;
    }

    const auto& a = left.GetFunction();
    const auto& b = right.GetFunction();

    if (left.qualifiedName != right.qualifiedName)
        return false;

    if (a.parameters.size() != b.parameters.size())
        return false;

    if (NormalizeType(a.returnType) != NormalizeType(b.returnType))
        return false;

    for (size_t i = 0; i < a.parameters.size(); ++i)
    {
        if (NormalizeType(a.parameters[i].typeName) != NormalizeType(b.parameters[i].typeName))
            return false;
        if (a.parameters[i].modifier != b.parameters[i].modifier)
            return false;
    }

    return true;
}

namespace
{
bool IsSameType(const std::string& a, const std::string& b)
{
    return !a.empty() && (a == b || LastScopeSegment(a) == LastScopeSegment(b));
}

bool IsOutParameter(const ParameterInformation& param)
{
    return param.modifier == ParameterModifier::Out || param.modifier == ParameterModifier::InOut ||
           param.rawText.find("&out") != std::string::npos || param.rawText.find("&inout") != std::string::npos ||
           param.typeName.find("&out") != std::string::npos || param.typeName.find("&inout") != std::string::npos ||
           param.typeName.find("& out") != std::string::npos;
}

bool IsContainerParameter(const ParameterInformation& param)
{
    return param.typeName.find("array<") != std::string::npos || param.rawText.find("array<") != std::string::npos ||
           param.typeName.find("vector<") != std::string::npos;
}

bool IsWildcardParameter(const ParameterInformation& param)
{
    return param.rawText.find("...") != std::string::npos || IsVariableType(param.typeName) ||
           IsVariableType(param.rawText);
}

std::optional<ArgumentConversion> EvaluateSpecialArgumentMatch(const std::string& argType,
                                                               const ParameterInformation& param)
{
    if (IsWildcardParameter(param))
    {
        return ArgumentConversion{ConversionRank::StandardConv, 220, 0, false, 10};
    }
    if (argType.empty() || argType == "auto")
    {
        return ArgumentConversion{ConversionRank::Exact, 0, 0, false, 0};
    }
    if (argType == "null")
    {
        const bool paramIsHandle = param.isHandle || HasHandleModifier(param.typeName);
        if (paramIsHandle)
        {
            return ArgumentConversion{ConversionRank::Exact, 0, 0, false,
                                      static_cast<int>(OverloadMatchPenalty::Exact)};
        }
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }
    if (argType == "void")
    {
        if (IsOutParameter(param))
        {
            return ArgumentConversion{ConversionRank::Exact, 0, 0, false,
                                      static_cast<int>(OverloadMatchPenalty::Exact)};
        }
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }
    if (argType == "init_list")
    {
        if (IsContainerParameter(param))
        {
            return ArgumentConversion{ConversionRank::Exact, 0, 0, false,
                                      static_cast<int>(OverloadMatchPenalty::Exact)};
        }
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }
    return std::nullopt;
}

struct MatchContext
{
    std::string cleanArg;
    std::string cleanParam;
    bool argIsHandle = false;
    bool argIsConst = false;
    bool paramIsHandle = false;
    bool paramIsConst = false;
    bool isMutableRef = false;
    const SymbolTable& table;
};

static bool IsMutableRefParam(const ParameterInformation& param)
{
    return param.isReference || param.modifier == ParameterModifier::InOut || param.modifier == ParameterModifier::Out;
}

static std::optional<ArgumentConversion> EvaluateNumericMutableRef(const MatchContext& ctx)
{
    if (!IsNumericPrimitive(ctx.cleanArg) || !IsNumericPrimitive(ctx.cleanParam))
    {
        return std::nullopt;
    }
    if (IsPrimitiveWidening(ctx.cleanArg, ctx.cleanParam))
    {
        const bool crossesKind = IsIntegerType(ctx.cleanArg) && IsFloatingPointType(ctx.cleanParam);
        if (crossesKind)
        {
            return ArgumentConversion{ConversionRank::StandardConv, 30, 0, false,
                                      static_cast<int>(OverloadMatchPenalty::WideningAcrossKind)};
        }
        return ArgumentConversion{ConversionRank::Promotion, 0, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Widening)};
    }
    return ArgumentConversion{ConversionRank::StandardConv, 10, 0, false,
                              static_cast<int>(OverloadMatchPenalty::Narrowing)};
}

std::optional<ArgumentConversion> EvaluateMutableRefMatch(const ParameterInformation& param, const MatchContext& ctx)
{
    if (!ctx.isMutableRef)
    {
        return std::nullopt;
    }
    if (ctx.argIsConst)
    {
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }
    if (ctx.argIsHandle != ctx.paramIsHandle && !IsMutableRefParam(param))
    {
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }
    if (IsSameType(ctx.cleanArg, ctx.cleanParam))
    {
        const bool objectToHandleRef = !ctx.argIsHandle && ctx.paramIsHandle;
        return ArgumentConversion{ConversionRank::Exact, 0, 0, objectToHandleRef,
                                  objectToHandleRef ? static_cast<int>(OverloadMatchPenalty::ConstRef)
                                                    : static_cast<int>(OverloadMatchPenalty::Exact)};
    }
    if (auto numConv = EvaluateNumericMutableRef(ctx))
    {
        return numConv;
    }
    return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                              static_cast<int>(OverloadMatchPenalty::Incompatible)};
}

bool IsHandleToReferenceBinding(bool argIsHandle, bool paramIsHandle, const ParameterInformation& param)
{
    return argIsHandle && !paramIsHandle &&
           (param.isReference || param.modifier == ParameterModifier::In ||
            param.modifier == ParameterModifier::InOut || param.modifier == ParameterModifier::Out);
}

std::optional<ArgumentConversion> EvaluateSameTypeMatch(const MatchContext& ctx, bool isHandleToRef,
                                                        bool isValueToHandle)
{
    if (!IsSameType(ctx.cleanArg, ctx.cleanParam))
    {
        return std::nullopt;
    }
    if (ctx.argIsHandle == ctx.paramIsHandle)
    {
        const bool constDiff = (ctx.argIsConst != ctx.paramIsConst);
        return ArgumentConversion{ConversionRank::Exact, 0, 0, constDiff,
                                  constDiff ? static_cast<int>(OverloadMatchPenalty::ConstRef)
                                            : static_cast<int>(OverloadMatchPenalty::Exact)};
    }
    if (isHandleToRef || isValueToHandle)
    {
        if (ctx.paramIsConst || !ctx.argIsConst)
        {
            return ArgumentConversion{ConversionRank::Exact, 0, 0, false,
                                      static_cast<int>(OverloadMatchPenalty::Exact)};
        }
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }
    return ArgumentConversion{ConversionRank::Exact, 0, 0, true, static_cast<int>(OverloadMatchPenalty::ConstRef)};
}

std::optional<ArgumentConversion> EvaluateInheritedSubtypeMatch(const MatchContext& ctx, bool isValueToHandle,
                                                                bool isHandleToRef)
{
    if (ctx.argIsHandle != ctx.paramIsHandle && !isValueToHandle && !isHandleToRef)
    {
        return std::nullopt;
    }

    auto hierarchy = GetInheritedTypeHierarchy(ctx.cleanArg, ctx.table);
    for (size_t dist = 0; dist < hierarchy.size(); ++dist)
    {
        if (IsSameType(NormalizeType(hierarchy[dist]), ctx.cleanParam))
        {
            const uint8_t distance = static_cast<uint8_t>(std::min<size_t>(dist + 1, 255));
            const int basePenalty = static_cast<int>(OverloadMatchPenalty::Inheritance) + (isValueToHandle ? 1 : 0) +
                                    static_cast<int>(dist);
            return ArgumentConversion{ConversionRank::StandardConv, 0, distance, false, basePenalty};
        }
    }
    return std::nullopt;
}

std::optional<ArgumentConversion> EvaluateSameTypeOrSubtypeMatch(const ParameterInformation& param,
                                                                 const MatchContext& ctx)
{
    const bool isHandleToRef = IsHandleToReferenceBinding(ctx.argIsHandle, ctx.paramIsHandle, param);
    const bool isValueToHandle = !ctx.argIsHandle && ctx.paramIsHandle;

    if (auto sameConv = EvaluateSameTypeMatch(ctx, isHandleToRef, isValueToHandle))
    {
        return sameConv;
    }
    return EvaluateInheritedSubtypeMatch(ctx, isValueToHandle, isHandleToRef);
}

std::optional<ArgumentConversion> EvaluatePrimitiveOrEnumConversion(const MatchContext& ctx)
{
    const auto namesAnEnum = [&ctx](const std::string& typeName)
    {
        const auto symbols = ctx.table.FindSymbolsPtr(typeName);
        return symbols && std::any_of(symbols->begin(), symbols->end(),
                                      [](const Symbol& sym) { return sym.type == SymbolType::Enum; });
    };

    if (IsIntegerType(ctx.cleanParam) && namesAnEnum(ctx.cleanArg))
    {
        return ArgumentConversion{ConversionRank::Promotion, 0, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Widening)};
    }
    if (IsIntegerType(ctx.cleanArg) && IsIntegerType(ctx.cleanParam) &&
        IsUnsignedInteger(ctx.cleanArg) != IsUnsignedInteger(ctx.cleanParam))
    {
        return ArgumentConversion{ConversionRank::StandardConv, 20, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::SignednessChange)};
    }
    if (IsPrimitiveWidening(ctx.cleanArg, ctx.cleanParam))
    {
        const bool crossesKind = IsIntegerType(ctx.cleanArg) && IsFloatingPointType(ctx.cleanParam);
        if (crossesKind)
        {
            return ArgumentConversion{ConversionRank::StandardConv, 30, 0, false,
                                      static_cast<int>(OverloadMatchPenalty::WideningAcrossKind)};
        }
        return ArgumentConversion{ConversionRank::Promotion, 0, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Widening)};
    }
    if (IsPrimitiveNarrowing(ctx.cleanArg, ctx.cleanParam))
    {
        return ArgumentConversion{ConversionRank::StandardConv, 10, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Narrowing)};
    }
    return std::nullopt;
}

ArgumentConversion EvaluateCustomOrUnresolvedConversion(const MatchContext& ctx)
{
    if (HasUserConversion(ctx.cleanArg, ctx.cleanParam, ctx.table))
    {
        return ArgumentConversion{ConversionRank::UserDefined, 0, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::UserDefined)};
    }

    if (AreIncompatibleTemplateTypes(ctx.cleanArg, ctx.cleanParam))
    {
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }

    const auto isNamedAndUnresolved = [&ctx](const std::string& typeName)
    {
        if (typeName.empty() || IsCorePrimitive(typeName) || typeName == "string")
        {
            return false;
        }
        const std::string owner = MemberOwnerType(typeName);
        return !ctx.table.FindSymbolsPtr(typeName) && (owner.empty() || !ctx.table.FindSymbolsPtr(owner));
    };

    if (isNamedAndUnresolved(ctx.cleanArg) && isNamedAndUnresolved(ctx.cleanParam))
    {
        return ArgumentConversion{ConversionRank::StandardConv, 100, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::UnknownTypes)};
    }
    return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                              static_cast<int>(OverloadMatchPenalty::Incompatible)};
}

ArgumentConversion EvaluateConversionMatch(const MatchContext& ctx)
{
    if (ctx.cleanArg == "auto" || ctx.cleanParam == "auto")
    {
        return ArgumentConversion{ConversionRank::StandardConv, 100, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::UnknownTypes)};
    }
    if (auto conv = EvaluatePrimitiveOrEnumConversion(ctx))
    {
        return *conv;
    }
    return EvaluateCustomOrUnresolvedConversion(ctx);
}

/**
 * @brief Checks whether a parameter denotes a mutable reference or out parameter.
 * @param[in] param Parameter to inspect.
 * @param[in] paramIsConst Flag indicating if the parameter type has const modifier.
 * @return True if mutable reference.
 */
bool IsMutableReferenceParam(const ParameterInformation& param, bool paramIsConst)
{
    const bool isRefOrOut =
        param.isReference || param.modifier == ParameterModifier::Out || param.modifier == ParameterModifier::InOut ||
        param.rawText.find("&inout") != std::string::npos || param.typeName.find("&inout") != std::string::npos ||
        param.rawText.find("&out") != std::string::npos || param.typeName.find("&out") != std::string::npos;
    return isRefOrOut && !paramIsConst && param.modifier != ParameterModifier::In &&
           param.rawText.find("&in") == std::string::npos;
}

/**
 * @brief Evaluates type conversion between argument and parameter within given match context.
 * @param[in] param Target parameter information.
 * @param[in] ctx Match evaluation context.
 * @return Evaluated ArgumentConversion.
 */
ArgumentConversion EvaluateCandidateTypeMatch(const ParameterInformation& param, const MatchContext& ctx)
{
    if (auto refScore = EvaluateMutableRefMatch(param, ctx))
    {
        return *refScore;
    }
    if (auto exactScore = EvaluateSameTypeOrSubtypeMatch(param, ctx))
    {
        return *exactScore;
    }
    return EvaluateConversionMatch(ctx);
}
} // namespace

bool HasConstModifier(std::string_view typeName)
{
    return typeName == "const" || typeName.starts_with("const ") || typeName.ends_with(" const") ||
           typeName.ends_with("const");
}

ArgumentConversion EvaluateArgumentConversion(const std::string& argType, const ParameterInformation& param,
                                              const SymbolTable& symbolTable, bool argIsLValue)
{
    if (auto specialScore = EvaluateSpecialArgumentMatch(argType, param))
    {
        if (!argIsLValue && IsOutParameter(param))
        {
            return ArgumentConversion{ConversionRank::StandardConv, 200, 0, false,
                                      static_cast<int>(OverloadMatchPenalty::RValueToOutParam)};
        }
        return *specialScore;
    }

    const std::string cleanArg = UnwrapTypedef(NormalizeType(argType), symbolTable);
    const std::string cleanParam = UnwrapTypedef(NormalizeType(param.typeName), symbolTable);
    if (cleanArg.empty() || cleanParam.empty())
    {
        return ArgumentConversion{ConversionRank::StandardConv, 100, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::UnknownTypes)};
    }
    if (AreIncompatibleTemplateTypes(cleanArg, cleanParam))
    {
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }

    const bool paramIsConst = param.isConst || HasConstModifier(param.typeName);
    const bool isMutableRef = IsMutableReferenceParam(param, paramIsConst);

    const MatchContext ctx{cleanArg,
                           cleanParam,
                           HasHandleModifier(argType),
                           HasConstModifier(argType),
                           param.isHandle || HasHandleModifier(param.typeName),
                           paramIsConst,
                           isMutableRef,
                           symbolTable};

    ArgumentConversion conv = EvaluateCandidateTypeMatch(param, ctx);
    if (!argIsLValue && (isMutableRef || IsOutParameter(param)) && conv.IsViable())
    {
        return ArgumentConversion{ConversionRank::StandardConv, 200, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::RValueToOutParam)};
    }
    return conv;
}

int ScoreArgumentMatch(const std::string& argType, const ParameterInformation& param, const SymbolTable& symbolTable,
                       bool argIsLValue)
{
    return EvaluateArgumentConversion(argType, param, symbolTable, argIsLValue).legacyScore;
}

namespace
{
struct EvaluatedCandidate
{
    const Symbol* symbol = nullptr;
    std::vector<ArgumentConversion> conversions;
    std::vector<int> costVector;
    int defaultArgs = 0;
    int totalCost = 0;
};

struct ArityInfo
{
    uint32_t requiredParams = 0;
    uint32_t maxParams = 0;
    bool isVariadic = false;
};

ArityInfo InspectFunctionArity(const FunctionSignature& sig)
{
    ArityInfo info;
    for (const auto& param : sig.parameters)
    {
        if (param.rawText.find("...") != std::string::npos || param.typeName.find("...") != std::string::npos)
        {
            info.isVariadic = true;
            continue;
        }
        ++info.maxParams;
        if (param.defaultValue.empty() && param.rawText.find('=') == std::string::npos)
        {
            ++info.requiredParams;
        }
    }
    return info;
}

bool IsArityCompatible(const ArityInfo& arity, uint32_t argCount)
{
    if (!arity.isVariadic)
    {
        return argCount >= arity.requiredParams && argCount <= arity.maxParams;
    }
    return argCount >= arity.requiredParams;
}

struct CandidateConversions
{
    std::vector<ArgumentConversion> conversions;
    std::vector<int> costVector;
    int totalScore = 0;
};

std::optional<CandidateConversions> ScoreCandidateArguments(const FunctionSignature& sig,
                                                            const std::vector<std::string>& argumentTypes,
                                                            const SymbolTable& symbolTable,
                                                            const std::vector<bool>& argIsLValue)
{
    CandidateConversions scores;
    scores.conversions.reserve(argumentTypes.size());
    scores.costVector.reserve(argumentTypes.size());
    for (uint32_t i = 0; i < argumentTypes.size(); ++i)
    {
        if (i < sig.parameters.size())
        {
            const bool isLVal = i < argIsLValue.size() ? argIsLValue[i] : true;
            ArgumentConversion conv =
                EvaluateArgumentConversion(argumentTypes[i], sig.parameters[i], symbolTable, isLVal);
            if (!conv.IsViable())
            {
                return std::nullopt;
            }
            scores.costVector.push_back(conv.legacyScore);
            scores.totalScore += conv.legacyScore;
            scores.conversions.push_back(conv);
        }
        else
        {
            ArgumentConversion varargConv{ConversionRank::StandardConv, 220, 0, false, 10};
            scores.costVector.push_back(varargConv.legacyScore);
            scores.totalScore += varargConv.legacyScore;
            scores.conversions.push_back(varargConv);
        }
    }
    return scores;
}

std::optional<EvaluatedCandidate> EvaluateCandidate(const Symbol& sym, const std::vector<std::string>& argumentTypes,
                                                    const SymbolTable& symbolTable,
                                                    const std::vector<bool>& argIsLValue)
{
    if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return std::nullopt;
    }

    const auto& sig = sym.GetFunction();
    const uint32_t argCount = static_cast<uint32_t>(argumentTypes.size());
    const auto arity = InspectFunctionArity(sig);
    if (!IsArityCompatible(arity, argCount))
    {
        return std::nullopt;
    }

    auto scores = ScoreCandidateArguments(sig, argumentTypes, symbolTable, argIsLValue);
    if (!scores)
    {
        return std::nullopt;
    }

    int defaultArgs = 0;
    if (argCount < sig.parameters.size())
    {
        defaultArgs = static_cast<int>(sig.parameters.size() - argCount);
        scores->totalScore += defaultArgs * 1;
    }

    return EvaluatedCandidate{&sym, std::move(scores->conversions), std::move(scores->costVector), defaultArgs,
                              scores->totalScore};
}

bool IsStrictlyBetter(const EvaluatedCandidate& a, const EvaluatedCandidate& b)
{
    if (a.conversions.size() != b.conversions.size())
    {
        return false;
    }
    bool hasStrictlyBetterArg = false;
    for (size_t i = 0; i < a.conversions.size(); ++i)
    {
        if (a.conversions[i] > b.conversions[i])
        {
            return false;
        }
        if (a.conversions[i] < b.conversions[i])
        {
            hasStrictlyBetterArg = true;
        }
    }
    if (hasStrictlyBetterArg)
    {
        return true;
    }
    return a.defaultArgs < b.defaultArgs;
}

std::vector<EvaluatedCandidate> FilterNonDominatedCandidates(const std::vector<EvaluatedCandidate>& evaluated)
{
    std::vector<EvaluatedCandidate> nonDominated;
    for (const auto& cand : evaluated)
    {
        bool dominated = false;
        for (const auto& other : evaluated)
        {
            if (&cand != &other && IsStrictlyBetter(other, cand))
            {
                dominated = true;
                break;
            }
        }
        if (!dominated)
        {
            nonDominated.push_back(cand);
        }
    }
    return nonDominated;
}

bool CheckOverloadAmbiguity(const std::vector<EvaluatedCandidate>& nonDominated,
                            const std::vector<std::string>& argumentTypes)
{
    if (nonDominated.size() <= 1)
    {
        return false;
    }

    const bool anyArgumentUnknown = std::any_of(argumentTypes.begin(), argumentTypes.end(),
                                                [](const std::string& argType)
                                                {
                                                    const std::string bare = NormalizeType(argType);
                                                    return bare.empty() || bare == "auto";
                                                });
    if (anyArgumentUnknown)
    {
        return false;
    }

    for (size_t i = 1; i < nonDominated.size(); ++i)
    {
        if (!HasSameSignature(*nonDominated.front().symbol, *nonDominated[i].symbol))
        {
            return true;
        }
    }
    return false;
}
} // namespace

void OverloadResolver::addFunction(const FunctionSymbol& sym)
{
    if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
    {
        const auto& sig = sym.GetFunction();
        const auto arity = InspectFunctionArity(sig);
        for (size_t a = arity.requiredParams; a <= arity.maxParams; ++a)
        {
            functionIndex_[sym.name][a].push_back(sym);
        }
        if (arity.isVariadic)
        {
            for (size_t a = arity.maxParams + 1; a <= std::max<size_t>(arity.maxParams + 16, 20); ++a)
            {
                functionIndex_[sym.name][a].push_back(sym);
            }
        }
    }
}

void OverloadResolver::indexCandidates(std::span<const FunctionSymbol> symbols)
{
    for (const auto& sym : symbols)
    {
        addFunction(sym);
    }
}

void OverloadResolver::clear()
{
    functionIndex_.clear();
}

std::span<const FunctionSymbol> OverloadResolver::findCandidates(std::string_view name, size_t argCount) const
{
    auto itName = functionIndex_.find(std::string(name));
    if (itName != functionIndex_.end())
    {
        auto itArity = itName->second.find(argCount);
        if (itArity != itName->second.end())
        {
            return itArity->second;
        }
    }
    return {};
}

std::string FormatCandidateSignature(const Symbol& sym)
{
    if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return sym.name;
    }
    const auto& sig = sym.GetFunction();
    std::string res = sym.name + "(";
    for (size_t i = 0; i < sig.parameters.size(); ++i)
    {
        if (i > 0)
        {
            res += ", ";
        }
        res += sig.parameters[i].typeName;
        if (!sig.parameters[i].name.empty())
        {
            res += " " + sig.parameters[i].name;
        }
    }
    res += ")";
    return res;
}

std::string FindRejectionReason(const Symbol& sym, const std::vector<std::string>& argumentTypes)
{
    if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return "Not a function";
    }
    const auto& sig = sym.GetFunction();
    const auto arity = InspectFunctionArity(sig);
    if (!IsArityCompatible(arity, static_cast<uint32_t>(argumentTypes.size())))
    {
        return "Arity mismatch: expected " + std::to_string(arity.requiredParams) + ".." +
               std::to_string(arity.maxParams) + ", got " + std::to_string(argumentTypes.size());
    }
    for (size_t i = 0; i < argumentTypes.size() && i < sig.parameters.size(); ++i)
    {
        if (AreIncompatibleTemplateTypes(argumentTypes[i], sig.parameters[i].typeName))
        {
            return "Template arg mismatch: '" + argumentTypes[i] + "' vs '" + sig.parameters[i].typeName + "'";
        }
    }
    return "Type conversion incompatible";
}

struct OverloadLogRequest
{
    std::span<const Symbol* const> candidates;
    const std::vector<std::string>& argumentTypes;
    const OverloadMatchResult& result;
    const std::vector<EvaluatedCandidate>& evaluated;
};

void LogOverloadTelemetry(const OverloadLogRequest& req)
{
    if (!utils::MultiFileLogger::Instance().IsInitialized() || req.candidates.empty())
    {
        return;
    }
    std::string fnName = req.candidates.front() ? req.candidates.front()->name : "unknown";
    std::string argsStr;
    for (size_t i = 0; i < req.argumentTypes.size(); ++i)
    {
        if (i > 0)
        {
            argsStr += ", ";
        }
        argsStr += req.argumentTypes[i];
    }
    utils::MultiFileLogger::Instance().LogOverload(utils::MultiFileLogLevel::Info,
                                                   "Resolving '" + fnName + "' with args: (" + argsStr + ")");

    for (size_t i = 0; i < req.candidates.size(); ++i)
    {
        const Symbol* sym = req.candidates[i];
        if (!sym)
        {
            continue;
        }
        std::string candSig = FormatCandidateSignature(*sym);
        auto it = std::find_if(req.evaluated.begin(), req.evaluated.end(),
                               [sym](const EvaluatedCandidate& e) { return e.symbol == sym; });
        if (it != req.evaluated.end())
        {
            utils::MultiFileLogger::Instance().LogOverload(
                utils::MultiFileLogLevel::Info, "  Candidate " + std::to_string(i + 1) + " '" + candSig +
                                                    "': ACCEPTED (Score=" + std::to_string(it->totalCost) + ")");
        }
        else
        {
            std::string reason = FindRejectionReason(*sym, req.argumentTypes);
            utils::MultiFileLogger::Instance().LogOverload(utils::MultiFileLogLevel::Info,
                                                           "  Candidate " + std::to_string(i + 1) + " '" + candSig +
                                                               "': REJECTED (" + reason + ")");
        }
    }
    if (req.result.bestCandidate)
    {
        utils::MultiFileLogger::Instance().LogOverload(
            utils::MultiFileLogLevel::Info, "Winner: " + FormatCandidateSignature(*req.result.bestCandidate));
    }
    else
    {
        utils::MultiFileLogger::Instance().LogOverload(utils::MultiFileLogLevel::Info,
                                                       "Winner: None (no viable overload)");
    }
}

OverloadMatchResult ResolveBestOverload(std::span<const Symbol* const> candidates,
                                        const std::vector<std::string>& argumentTypes, const SymbolTable& symbolTable,
                                        const std::vector<bool>& argIsLValue)
{
    OverloadMatchResult result;
    std::vector<EvaluatedCandidate> evaluated;

    for (const Symbol* sym : candidates)
    {
        if (!sym)
        {
            continue;
        }
        if (auto cand = EvaluateCandidate(*sym, argumentTypes, symbolTable, argIsLValue))
        {
            result.viableCandidates.push_back(sym);
            evaluated.push_back(std::move(*cand));
        }
    }

    if (evaluated.empty())
    {
        OverloadLogRequest logReq{candidates, argumentTypes, result, evaluated};
        LogOverloadTelemetry(logReq);
        return result;
    }

    if (evaluated.size() == 1)
    {
        result.bestCandidate = evaluated.front().symbol;
        result.bestScore = evaluated.front().totalCost;
        result.bestCostVector = std::move(evaluated.front().costVector);
        result.bestConversions = std::move(evaluated.front().conversions);
        OverloadLogRequest logReq{candidates, argumentTypes, result, evaluated};
        LogOverloadTelemetry(logReq);
        return result;
    }

    auto nonDominated = FilterNonDominatedCandidates(evaluated);
    if (nonDominated.empty())
    {
        OverloadLogRequest logReq{candidates, argumentTypes, result, evaluated};
        LogOverloadTelemetry(logReq);
        return result;
    }

    result.isAmbiguous = CheckOverloadAmbiguity(nonDominated, argumentTypes);
    if (result.isAmbiguous)
    {
        result.bestCandidate = nullptr;
    }
    else
    {
        result.bestCandidate = nonDominated.front().symbol;
        result.bestScore = nonDominated.front().totalCost;
        result.bestCostVector = nonDominated.front().costVector;
        result.bestConversions = nonDominated.front().conversions;
    }

    OverloadLogRequest logReq{candidates, argumentTypes, result, evaluated};
    LogOverloadTelemetry(logReq);
    return result;
}

OverloadMatchResult ResolveBestOverload(const std::vector<Symbol>& candidates,
                                        const std::vector<std::string>& argumentTypes, const SymbolTable& symbolTable,
                                        const std::vector<bool>& argIsLValue)
{
    std::vector<const Symbol*> ptrs;
    ptrs.reserve(candidates.size());
    for (const auto& sym : candidates)
    {
        ptrs.push_back(&sym);
    }
    return ResolveBestOverload(ptrs, argumentTypes, symbolTable, argIsLValue);
}
} // namespace angel_lsp::analysis
