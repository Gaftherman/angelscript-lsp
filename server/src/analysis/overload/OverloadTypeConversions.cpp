#include "analysis/overload/OverloadTypeConversions.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
static bool StripReferenceSuffix(std::string& s)
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

static bool MatchesConstructorParameter(const FunctionSignature& sig, const TemplateBinding& binding,
                                        const std::string& fromType, std::string_view arrayTypeName = {})
{
    if (sig.modifiers.isExplicit || sig.modifiers.isDelete || sig.parameters.empty())
    {
        return false;
    }
    std::string paramType = sig.parameters[0].typeName;
    if (binding.usable)
    {
        paramType = SubstituteTemplateParameters(paramType, binding);
    }
    paramType = NormalizeType(paramType, arrayTypeName);
    if (paramType != fromType && !IsPrimitiveWidening(fromType, paramType) && !IsSameType(fromType, paramType))
    {
        return false;
    }
    return std::all_of(sig.parameters.begin() + 1, sig.parameters.end(),
                       [](const auto& p) { return !p.defaultValue.empty(); });
}

static bool CheckConstructorList(const std::shared_ptr<const std::vector<Symbol>>& syms, const TemplateBinding& binding,
                                 const std::string& fromType, std::string_view arrayTypeName = {})
{
    if (!syms)
    {
        return false;
    }
    for (const auto& sym : *syms)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            if (MatchesConstructorParameter(sym.GetFunction(), binding, fromType, arrayTypeName))
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Checks if two comma-separated template argument lists have identical types.
 * @param[in] aInner First template argument list string (e.g. "int, float").
 * @param[in] bInner Second template argument list string.
 * @return True if argument counts and corresponding types match.
 */
bool AreTemplateArgumentsEqual(std::string_view aInner, std::string_view bInner)
{
    const auto aArgs = SplitTemplateArguments(aInner);
    const auto bArgs = SplitTemplateArguments(bInner);
    if (aArgs.size() != bArgs.size())
    {
        return false;
    }
    for (size_t i = 0; i < aArgs.size(); ++i)
    {
        std::string_view va = aArgs[i];
        std::string_view vb = bArgs[i];
        TrimTypeWhitespace(va);
        TrimTypeWhitespace(vb);
        if (!IsSameType(std::string(va), std::string(vb)))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Iteratively strips trailing handle '@' and reference '&' decorations from a type string.
 * @param[in] result Type name string to strip.
 * @return Stripped type name without reference or handle suffixes.
 * @note [REFACTOR-CANDIDATE] Manually re-evaluates AST for isHandle; candidate for direct Pass 1 flag read via
 * TypeExtraction::ExtractTypeInfo / Symbol::modifiers.isHandle / LocalDefinition::isHandleType.
 */
std::string StripTypeDecorations(std::string result)
{
    bool modified = true;
    while (modified)
    {
        modified = false;
        std::string_view sv = StripTrailingDecorationsView(result);
        if (sv.size() != result.size())
        {
            result = std::string(sv);
            modified = true;
        }
        if (StripReferenceSuffix(result))
        {
            modified = true;
        }
    }
    return result;
}

std::string DesugarArrayBrackets(std::string result, std::string_view arrayTypeName)
{
    if (arrayTypeName.empty())
    {
        return result;
    }
    while (result.ends_with("[]"))
    {
        const std::string inner = DesugarArrayBrackets(result.substr(0, result.size() - 2), arrayTypeName);
        result = std::string(arrayTypeName) + "<" + inner + ">";
    }
    return result;
}

/**
 * @brief Normalizes a type name by stripping const, references, handles, and desugaring array brackets.
 * @param[in] typeName Raw type name to normalize.
 * @param[in] arrayTypeName Generic array template name for desugaring bracket syntax.
 * @return Canonical base type name string.
 * @note [REFACTOR-CANDIDATE] Manually re-evaluates AST for isConst; candidate for direct Pass 1 flag read via
 * TypeExtraction::ExtractTypeInfo / Symbol::modifiers.isConst.
 */
std::string NormalizeType(std::string_view typeName, std::string_view arrayTypeName)
{
    typeName = StripLeadingConst(typeName);
    TrimTypeWhitespace(typeName);

    std::string result = StripTypeDecorations(std::string(typeName));
    while (!result.empty() && (result.back() == ' ' || result.back() == '\t'))
    {
        result.pop_back();
    }

    result = CanonicalizeType(result);
    return DesugarArrayBrackets(std::move(result), arrayTypeName);
}

/**
 * @brief Has handle modifier.
 * @param[in] typeName Type name to inspect or resolve.
 * @return True if the condition is satisfied; false otherwise.
 * @note [REFACTOR-CANDIDATE] Manually re-evaluates AST for isHandle; candidate for direct Pass 1 flag read via
 * TypeExtraction::ExtractTypeInfo / Symbol::modifiers.isHandle / LocalDefinition::isHandleType.
 */
bool HasHandleModifier(std::string_view typeName)
{
    return IsHandleTypeView(typeName);
}

bool HasConvertingConstructor(const std::string& fromType, const std::string& toType, const SymbolTable& symbolTable)
{
    const std::string cleanTo = NormalizeType(toType, symbolTable.GetDefaultArrayType());
    if (cleanTo.empty() || IsCorePrimitive(cleanTo))
    {
        return false;
    }
    const size_t openBracket = cleanTo.find('<');
    const std::string baseName = (openBracket == std::string::npos) ? cleanTo : cleanTo.substr(0, openBracket);
    const std::string shortName{LastScopeSegment(baseName)};
    const TemplateBinding binding =
        (openBracket != std::string::npos) ? BindTemplateArguments(cleanTo, symbolTable) : TemplateBinding{};
    const std::string_view defaultArrayType = symbolTable.GetDefaultArrayType();
    if (CheckConstructorList(symbolTable.FindMemberSymbolPtr(baseName, shortName), binding, fromType, defaultArrayType))
    {
        return true;
    }
    return shortName != baseName && CheckConstructorList(symbolTable.FindMemberSymbolPtr(shortName, shortName), binding,
                                                         fromType, defaultArrayType);
}

UserConversionMatch CheckConversionMethod(const std::string& fromType, const std::string& toType,
                                          const SymbolTable& symbolTable)
{
    UserConversionMatch bestMatch;
    const std::string cleanFrom = NormalizeType(fromType, symbolTable.GetDefaultArrayType());
    const std::string cleanTo = NormalizeType(toType, symbolTable.GetDefaultArrayType());
    const size_t openBracket = cleanFrom.find('<');
    const std::string baseName = (openBracket == std::string::npos) ? cleanFrom : cleanFrom.substr(0, openBracket);
    const std::string shortName{LastScopeSegment(baseName)};
    TemplateBinding binding;
    if (openBracket != std::string::npos)
    {
        binding = BindTemplateArguments(cleanFrom, symbolTable);
    }
    for (const char* opName : {"opImplConv", "opImplCast"})
    {
        const auto checkOps = [&](const std::shared_ptr<const std::vector<Symbol>>& opSyms) -> bool
        {
            if (!opSyms)
            {
                return false;
            }
            for (const auto& sym : *opSyms)
            {
                if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
                {
                    std::string retType = sym.GetFunction().returnType;
                    if (binding.usable)
                    {
                        retType = SubstituteTemplateParameters(retType, binding);
                    }
                    retType = NormalizeType(retType, symbolTable.GetDefaultArrayType());
                    if (retType == cleanTo || IsSameType(retType, cleanTo))
                    {
                        bestMatch = UserConversionMatch{true, true};
                        return true;
                    }
                    if (IsPrimitiveWidening(retType, cleanTo))
                    {
                        bestMatch.viable = true;
                        bestMatch.isExact = false;
                    }
                }
            }
            return false;
        };
        if (checkOps(symbolTable.FindMemberSymbolPtr(baseName, opName)) ||
            (shortName != baseName && checkOps(symbolTable.FindMemberSymbolPtr(shortName, opName))))
        {
            return bestMatch;
        }
    }
    return bestMatch;
}

bool HasConversionMethod(const std::string& fromType, const std::string& toType, const SymbolTable& symbolTable)
{
    return CheckConversionMethod(fromType, toType, symbolTable).viable;
}

UserConversionMatch CheckUserConversion(const std::string& fromType, const std::string& toType,
                                        const SymbolTable& symbolTable)
{
    if (fromType.empty() || toType.empty())
    {
        return UserConversionMatch{};
    }
    auto methodMatch = CheckConversionMethod(fromType, toType, symbolTable);
    if (methodMatch.viable)
    {
        return methodMatch;
    }
    if (HasConvertingConstructor(fromType, toType, symbolTable))
    {
        return UserConversionMatch{true, false};
    }
    return UserConversionMatch{};
}

bool HasUserConversion(const std::string& fromType, const std::string& toType, const SymbolTable& symbolTable)
{
    return CheckUserConversion(fromType, toType, symbolTable).viable;
}

constexpr int k_maxTypedefDepth = 8;

std::string UnwrapTypedef(const std::string& typeName, const SymbolTable& symbolTable, int depth)
{
    std::string current = NormalizeType(typeName, symbolTable.GetDefaultArrayType());
    const auto syms = symbolTable.FindSymbolsPtr(current);
    if (syms)
    {
        for (const auto& s : *syms)
        {
            if (s.type == SymbolType::Typedef && std::holds_alternative<TypedefSignature>(s.signature))
            {
                return NormalizeType(s.GetTypedef().baseType, symbolTable.GetDefaultArrayType());
            }
        }
    }

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

bool IsIntegerType(const std::string& typeName)
{
    return IsIntegerPrimitive(CleanBaseTypeView(typeName));
}

bool IsUnsignedInteger(const std::string& typeName)
{
    const std::string_view normalized = CleanBaseTypeView(typeName);
    return normalized == "uint" || normalized == "uint8" || normalized == "uint16" || normalized == "uint32" ||
           normalized == "uint64";
}

bool IsFloatingPointType(const std::string& typeName)
{
    return IsFloatingPointPrimitive(CleanBaseTypeView(typeName));
}

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
    const std::string_view from = CleanBaseTypeView(fromType);
    const std::string_view to = CleanBaseTypeView(toType);

    if (from == to || from == "bool" || to == "bool")
    {
        return false;
    }

    return CheckWideningTarget(from, to);
}

bool IsPrimitiveNarrowing(const std::string& fromType, const std::string& toType)
{
    const std::string_view from = CleanBaseTypeView(fromType);
    const std::string_view to = CleanBaseTypeView(toType);

    if (from == to)
    {
        return false;
    }

    if (IsNumericPrimitive(from) && IsNumericPrimitive(to))
    {
        return !IsPrimitiveWidening(fromType, toType);
    }
    return false;
}

/**
 * @brief Has const modifier.
 * @param[in] typeName Type name to inspect or resolve.
 * @return True if the condition is satisfied; false otherwise.
 * @note [REFACTOR-CANDIDATE] Manually re-evaluates AST for isConst; candidate for direct Pass 1 flag read via
 * TypeExtraction::ExtractTypeInfo / Symbol::modifiers.isConst.
 */
bool HasConstModifier(std::string_view typeName)
{
    return IsConstTypeView(typeName);
}

bool IsWildcardParameter(const ParameterInformation& param)
{
    return param.typeName == "?" || param.typeName.find('?') != std::string::npos;
}

/**
 * @brief Is out parameter.
 * @param[in] param Function parameter information.
 * @return True if the condition is satisfied; false otherwise.
 * @note [REFACTOR-CANDIDATE] Manually re-evaluates AST for isRef; candidate for direct Pass 1 flag read via
 * TypeExtraction::ExtractTypeInfo / ParameterInformation::isReference / ParameterInformation::modifier /
 * Symbol::modifiers.isReturnReference.
 */
bool IsOutParameter(const ParameterInformation& param)
{
    return param.modifier == ParameterModifier::Out || param.modifier == ParameterModifier::InOut || param.isOutRef ||
           param.isInOutRef;
}

/**
 * @brief Extract template base name.
 * @param[in] typeName Type name to inspect or resolve.
 * @return Found item if present; std::nullopt otherwise.
 * @note [REFACTOR-CANDIDATE] Manually re-evaluates AST for isConst; candidate for direct Pass 1 flag read via
 * TypeExtraction::ExtractTypeInfo / Symbol::modifiers.isConst.
 */
static std::optional<std::string_view> ExtractTemplateBaseName(std::string_view typeName)
{
    const size_t openBracket = typeName.find('<');
    const size_t closeBracket = typeName.rfind('>');
    if (openBracket == std::string_view::npos || closeBracket == std::string_view::npos || closeBracket <= openBracket)
    {
        return std::nullopt;
    }
    std::string_view base = typeName.substr(0, openBracket);
    base = StripLeadingConst(base);
    TrimTypeWhitespace(base);
    return base;
}

static bool HasListConstructorMember(std::string_view qualifiedName, std::string_view name,
                                     const SymbolTable& symbolTable)
{
    const auto ctors = symbolTable.FindMemberSymbolPtr(qualifiedName, name);
    if (!ctors)
    {
        return false;
    }
    for (const auto& ctor : *ctors)
    {
        if (IsListConstructor(ctor))
        {
            return true;
        }
    }
    return false;
}

static bool CheckClassSymbolListSupport(const Symbol& sym, const SymbolTable& symbolTable)
{
    if (sym.type != SymbolType::Class)
    {
        return false;
    }
    if (std::holds_alternative<ClassSignature>(sym.signature))
    {
        const auto& clsSig = std::get<ClassSignature>(sym.signature);
        if (clsSig.hasListPattern)
        {
            return true;
        }
    }
    return HasListConstructorMember(sym.qualifiedName, sym.name, symbolTable);
}

static bool ClassAcceptsInitializerList(const std::string& cleanType, const SymbolTable& symbolTable)
{
    const auto toSyms = symbolTable.FindMemberSymbolPtr(cleanType, cleanType);
    if (toSyms)
    {
        for (const auto& sym : *toSyms)
        {
            if (IsListConstructor(sym))
            {
                return true;
            }
        }
    }
    const auto shortName = LastScopeSegment(cleanType);
    const auto shortSyms = symbolTable.FindTypeSymbolsByShortName(shortName);
    for (const auto& sym : shortSyms)
    {
        if (CheckClassSymbolListSupport(sym, symbolTable))
        {
            return true;
        }
    }
    return false;
}

bool ParameterAcceptsInitializerList(const ParameterInformation& param, const SymbolTable* symbolTable,
                                     std::string_view arrayTypeName)
{
    if (param.typeName.find("[]") != std::string::npos)
    {
        return true;
    }

    const std::string cleanType = NormalizeType(param.typeName, arrayTypeName);
    if (cleanType.empty() || IsCorePrimitive(cleanType))
    {
        return false;
    }

    const std::string_view effectiveArrayName =
        !arrayTypeName.empty() ? arrayTypeName
                               : (symbolTable ? symbolTable->GetDefaultArrayType() : std::string_view{});
    const auto templateName = ExtractTemplateBaseName(cleanType);
    if (templateName)
    {
        if (*templateName == effectiveArrayName)
        {
            return true;
        }
        return symbolTable ? ClassAcceptsInitializerList(std::string(*templateName), *symbolTable) : false;
    }

    if (cleanType == effectiveArrayName)
    {
        return true;
    }

    return symbolTable ? ClassAcceptsInitializerList(cleanType, *symbolTable) : false;
}

bool IsContainerParameter(const ParameterInformation& param, const SymbolTable* symbolTable,
                          std::string_view arrayTypeName)
{
    return ParameterAcceptsInitializerList(param, symbolTable, arrayTypeName);
}

bool IsSameType(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty())
    {
        return false;
    }
    if (a == b)
    {
        return true;
    }
    const size_t aOpen = a.find('<');
    const size_t bOpen = b.find('<');
    if (aOpen != std::string::npos && a.ends_with('>') && bOpen != std::string::npos && b.ends_with('>'))
    {
        const std::string_view aCont = std::string_view(a).substr(0, aOpen);
        const std::string_view bCont = std::string_view(b).substr(0, bOpen);
        if (aCont != bCont)
        {
            if (utils::ContainsScopeSeparator(aCont) && utils::ContainsScopeSeparator(bCont))
            {
                return false;
            }
            if (LastScopeSegment(aCont) != LastScopeSegment(bCont))
            {
                return false;
            }
        }
        return AreTemplateArgumentsEqual(std::string_view(a).substr(aOpen + 1, a.size() - aOpen - 2),
                                         std::string_view(b).substr(bOpen + 1, b.size() - bOpen - 2));
    }
    if (utils::ContainsScopeSeparator(a) && utils::ContainsScopeSeparator(b))
    {
        return false;
    }
    return LastScopeSegment(a) == LastScopeSegment(b);
}

} // namespace angel_lsp::analysis
