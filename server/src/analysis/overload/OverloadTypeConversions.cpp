#include "analysis/overload/OverloadTypeConversions.h"
#include "analysis/SemanticHelpers.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
std::string_view TrimWhitespace(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
    {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
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
} // namespace

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

std::string DesugarArrayBrackets(std::string result, std::string_view arrayTypeName)
{
    const std::string_view effectiveArrayName = arrayTypeName.empty() ? "array" : arrayTypeName;
    while (result.ends_with("[]"))
    {
        const std::string inner = DesugarArrayBrackets(result.substr(0, result.size() - 2), effectiveArrayName);
        result = std::string(effectiveArrayName) + "<" + inner + ">";
    }
    return result;
}

std::string NormalizeType(std::string_view typeName, std::string_view arrayTypeName)
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
    return DesugarArrayBrackets(std::move(result), arrayTypeName);
}

bool HasHandleModifier(std::string_view typeName)
{
    return typeName.find('@') != std::string_view::npos;
}

namespace
{
bool IsViableConvertingConstructor(const Symbol& sym, const std::string& fromType)
{
    if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return false;
    }
    const auto& sig = sym.GetFunction();
    if (sig.modifiers.isExplicit || sig.modifiers.isDelete || sig.parameters.empty())
    {
        return false;
    }
    const std::string paramType = NormalizeType(sig.parameters[0].typeName);
    if (paramType != fromType && !IsPrimitiveWidening(fromType, paramType))
    {
        return false;
    }
    return std::all_of(sig.parameters.begin() + 1, sig.parameters.end(),
                       [](const auto& p) { return !p.defaultValue.empty(); });
}
} // namespace

bool HasConvertingConstructor(const std::string& fromType, const std::string& toType, const SymbolTable& symbolTable,
                              std::string_view stringTypeName)
{
    const std::string cleanTo = NormalizeType(toType);
    const std::string_view effectiveStr = stringTypeName.empty() ? "string" : stringTypeName;
    if (cleanTo.empty() || cleanTo == effectiveStr || cleanTo == "string" || IsCorePrimitive(cleanTo))
    {
        return false;
    }
    const auto toSyms = symbolTable.FindSymbolsPtr(cleanTo + "::" + cleanTo);
    if (!toSyms)
    {
        return false;
    }
    for (const auto& sym : *toSyms)
    {
        if (IsViableConvertingConstructor(sym, fromType))
        {
            return true;
        }
    }
    return false;
}

UserConversionMatch CheckConversionMethod(const std::string& fromType, const std::string& toType,
                                          const SymbolTable& symbolTable)
{
    UserConversionMatch bestMatch;
    const std::string cleanFrom = NormalizeType(fromType);
    const std::string cleanTo = NormalizeType(toType);

    for (const char* opName : {"opImplConv", "opImplCast"})
    {
        const auto opSyms = symbolTable.FindSymbolsPtr(cleanFrom + "::" + opName);
        if (!opSyms)
        {
            continue;
        }
        for (const auto& sym : *opSyms)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                const std::string retType = NormalizeType(sym.GetFunction().returnType);
                if (retType == cleanTo)
                {
                    return UserConversionMatch{true, true};
                }
                if (IsPrimitiveWidening(retType, cleanTo))
                {
                    bestMatch.viable = true;
                    bestMatch.isExact = false;
                }
            }
        }
    }
    return bestMatch;
}

bool HasConversionMethod(const std::string& fromType, const std::string& toType, const SymbolTable& symbolTable)
{
    return CheckConversionMethod(fromType, toType, symbolTable).viable;
}

UserConversionMatch CheckUserConversion(const std::string& fromType, const std::string& toType,
                                        const SymbolTable& symbolTable, std::string_view stringTypeName)
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
    if (HasConvertingConstructor(fromType, toType, symbolTable, stringTypeName))
    {
        return UserConversionMatch{true, false};
    }
    return UserConversionMatch{};
}

bool HasUserConversion(const std::string& fromType, const std::string& toType, const SymbolTable& symbolTable,
                       std::string_view stringTypeName)
{
    return CheckUserConversion(fromType, toType, symbolTable, stringTypeName).viable;
}

constexpr int k_maxTypedefDepth = 8;

std::string UnwrapTypedef(const std::string& typeName, const SymbolTable& symbolTable, int depth)
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
    return IsIntegerPrimitive(NormalizeType(typeName));
}

bool IsUnsignedInteger(const std::string& typeName)
{
    const std::string normalized = NormalizeType(typeName);
    return normalized == "uint" || normalized == "uint8" || normalized == "uint16" || normalized == "uint32" ||
           normalized == "uint64";
}

bool IsFloatingPointType(const std::string& typeName)
{
    return IsFloatingPointPrimitive(NormalizeType(typeName));
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
    return false;
}

bool HasConstModifier(std::string_view typeName)
{
    return typeName.starts_with("const ") || typeName.ends_with(" const") ||
           typeName.find(" const ") != std::string_view::npos;
}

bool IsWildcardParameter(const ParameterInformation& param)
{
    return param.typeName == "?" || param.rawText.find('?') != std::string::npos;
}

bool IsOutParameter(const ParameterInformation& param)
{
    return param.modifier == ParameterModifier::Out || param.modifier == ParameterModifier::InOut ||
           param.rawText.find("&out") != std::string::npos || param.rawText.find("&inout") != std::string::npos ||
           param.typeName.find("&out") != std::string::npos || param.typeName.find("&inout") != std::string::npos ||
           param.typeName.find("& out") != std::string::npos;
}

static std::optional<std::string_view> ExtractTemplateBaseName(std::string_view typeName)
{
    const size_t openBracket = typeName.find('<');
    const size_t closeBracket = typeName.rfind('>');
    if (openBracket == std::string_view::npos || closeBracket == std::string_view::npos ||
        closeBracket <= openBracket)
    {
        return std::nullopt;
    }
    std::string_view base = typeName.substr(0, openBracket);
    base = TrimWhitespace(base);
    if (base.starts_with("const "))
    {
        base.remove_prefix(6);
        base = TrimWhitespace(base);
    }
    return base;
}

static bool IsSymbolTableTemplateClass(std::string_view templateName, const SymbolTable& symbolTable)
{
    const std::string nameStr(templateName);
    const auto syms = symbolTable.FindSymbolsPtr(nameStr);
    const auto typeSymbols = syms ? *syms : symbolTable.FindTypeSymbolsByShortName(nameStr);
    for (const auto& sym : typeSymbols)
    {
        if (sym.type == SymbolType::Class && std::holds_alternative<ClassSignature>(sym.signature))
        {
            const auto& clsSig = std::get<ClassSignature>(sym.signature);
            if (clsSig.isTemplate || !clsSig.templateParams.empty())
            {
                return true;
            }
        }
    }
    return false;
}

bool IsContainerParameter(const ParameterInformation& param, const SymbolTable* symbolTable,
                          std::string_view arrayTypeName)
{
    if (param.typeName.ends_with("[]") || param.rawText.find("[]") != std::string::npos)
    {
        return true;
    }

    const auto templateName = ExtractTemplateBaseName(param.typeName);
    if (!templateName)
    {
        return false;
    }

    const std::string_view effectiveArrayName = arrayTypeName.empty() ? "array" : arrayTypeName;
    if (*templateName == effectiveArrayName || *templateName == "array")
    {
        return true;
    }

    return symbolTable ? IsSymbolTableTemplateClass(*templateName, *symbolTable) : true;
}

bool IsSameType(const std::string& a, const std::string& b)
{
    return !a.empty() && (a == b || LastScopeSegment(a) == LastScopeSegment(b));
}

} // namespace angel_lsp::analysis
