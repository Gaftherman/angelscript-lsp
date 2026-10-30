#include "analysis/SignatureFormatter.h"
#include "parser/Keywords.h"
#include "utils/ScopedNameUtils.h"

#include <sstream>

namespace angel_lsp::analysis
{
namespace
{
/** @brief Re-attaches a modifier the captured type text may already carry.
 *  @note TypeExtraction normally folds 'const' and '@' into typeName, but the flags are the
 *        authority. Appending unconditionally would produce "const const string", so each
 *        marker is only added when the text does not already show it. */
std::string ApplyConstAndHandle(const std::string& typeName, bool isConst, bool isHandle)
{
    const bool hasConst =
        typeName.starts_with(parser::keywords::Const) &&
        (typeName.size() == parser::keywords::Const.size() || typeName[parser::keywords::Const.size()] == ' ');
    const bool needsConst = isConst && !hasConst;
    const bool needsHandle = isHandle && (typeName.empty() || typeName.back() != '@');

    size_t extraLen = 0;
    if (needsConst)
    {
        extraLen += parser::keywords::Const.size() + 1;
    }
    if (needsHandle)
    {
        extraLen += 1;
    }

    std::string result;
    result.reserve(typeName.size() + extraLen);

    if (needsConst)
    {
        result.append(parser::keywords::Const);
        result.push_back(' ');
    }
    result.append(typeName);
    if (needsHandle)
    {
        result.push_back('@');
    }
    return result;
}
} // namespace

std::string FormatAccessPrefix(const SymbolModifiers& modifiers)
{
    switch (modifiers.access)
    {
    case AccessModifier::Private:
        return std::string(parser::keywords::Private) + ' ';
    case AccessModifier::Protected:
        return std::string(parser::keywords::Protected) + ' ';
    case AccessModifier::Public:
    default:
        return "";
    }
}

namespace
{
void AppendModifier(std::string& result, std::string_view mod)
{
    result.append(mod);
    result.push_back(' ');
}
} // namespace

std::string FormatDeclarationPrefix(const SymbolModifiers& modifiers)
{
    std::string result = FormatAccessPrefix(modifiers);

    if (modifiers.isExternal)
    {
        AppendModifier(result, parser::keywords::External);
    }
    if (modifiers.isShared)
    {
        AppendModifier(result, parser::keywords::Shared);
    }
    if (modifiers.isMixin)
    {
        AppendModifier(result, parser::keywords::Mixin);
    }
    if (modifiers.isDeclarationAbstract)
    {
        AppendModifier(result, parser::keywords::Abstract);
    }
    if (modifiers.isDeclarationFinal)
    {
        AppendModifier(result, parser::keywords::Final);
    }
    return result;
}

std::string FormatParameterReference(const ParameterInformation& param)
{
    switch (param.modifier)
    {
    case ParameterModifier::In:
        return "&in";
    case ParameterModifier::Out:
        return "&out";
    case ParameterModifier::InOut:
        return "&inout";
    case ParameterModifier::None:
    default:
        // A reference written without a direction is still part of the declared type.
        return param.isReference ? "&" : "";
    }
}

std::string FormatParameter(const ParameterInformation& param)
{
    std::ostringstream oss;

    const std::string typeText = ApplyConstAndHandle(param.typeName, param.isConst, param.isHandle);
    if (!typeText.empty())
    {
        oss << typeText;
    }

    const std::string reference = FormatParameterReference(param);
    if (!reference.empty())
    {
        if (!typeText.empty())
        {
            oss << " ";
        }
        oss << reference;
    }

    if (!param.name.empty())
    {
        if (!typeText.empty() || !reference.empty())
        {
            oss << " ";
        }
        oss << param.name;
    }

    if (!param.defaultValue.empty())
    {
        oss << " = " << param.defaultValue;
    }

    return oss.str();
}

std::string FormatFunctionSuffix(const SymbolModifiers& modifiers)
{
    std::string result;

    if (modifiers.isConst)
    {
        result += ' ';
        result += parser::keywords::Const;
    }
    if (modifiers.isOverride)
    {
        result += ' ';
        result += parser::keywords::Override;
    }
    // A 'final' written before the return type was already emitted by FormatDeclarationPrefix.
    if (modifiers.isFinal && !modifiers.isDeclarationFinal)
    {
        result += ' ';
        result += parser::keywords::Final;
    }
    if (modifiers.isExplicit)
    {
        result += ' ';
        result += parser::keywords::Explicit;
    }
    if (modifiers.isProperty)
    {
        result += ' ';
        result += parser::keywords::Property;
    }
    if (modifiers.isDelete)
    {
        result += ' ';
        result += parser::keywords::Delete;
    }
    return result;
}

std::string FormatReturnType(const std::string& returnType, const SymbolModifiers& modifiers)
{
    if (returnType.empty())
    {
        return std::string(parser::keywords::Void);
    }

    if (modifiers.isReturnReference && returnType.back() != '&')
    {
        return returnType + "&";
    }
    return returnType;
}

void AppendParameters(std::ostringstream& oss, const std::vector<ParameterInformation>& parameters)
{
    for (size_t i = 0; i < parameters.size(); ++i)
    {
        if (i > 0)
        {
            oss << ", ";
        }
        oss << FormatParameter(parameters[i]);
    }
}

void AppendImportOrigin(std::ostringstream& oss, const Symbol& sym)
{
    if (sym.type == SymbolType::Function && sym.GetFunction().isImported && !sym.GetFunction().originModule.empty())
    {
        oss << " from \"" << sym.GetFunction().originModule << "\"";
    }
}

void AppendFunctionPreamble(std::ostringstream& oss, const Symbol& sym, bool isFuncdef)
{
    if (isFuncdef)
    {
        oss << "funcdef ";
    }
    else if (sym.type == SymbolType::Function && sym.GetFunction().isImported)
    {
        oss << "import ";
    }
}

void AppendFunctionName(std::ostringstream& oss, const Symbol& sym, bool qualified)
{
    if (qualified && !sym.containerName.empty())
    {
        oss << sym.containerName << "::";
    }
    oss << sym.name;
}

bool HasCachedDisplaySignature(const Symbol& sym, bool qualified) noexcept
{
    return qualified && sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature) &&
           !sym.GetFunction().displaySignature.empty();
}

bool IsConstructorOrDestructorSymbol(const Symbol& sym, bool isFuncdef, const std::string& returnType)
{
    return !isFuncdef && returnType.empty() && !sym.containerName.empty() &&
           (sym.name == sym.containerName || sym.name == ("~" + sym.containerName));
}

std::string FormatFunctionDeclaration(const Symbol& sym, bool qualified)
{
    if (sym.type != SymbolType::Function && sym.type != SymbolType::Funcdef)
    {
        return "";
    }

    if (HasCachedDisplaySignature(sym, qualified))
    {
        return sym.GetFunction().displaySignature;
    }

    const bool isFuncdef = sym.type == SymbolType::Funcdef;
    const SymbolModifiers& modifiers = isFuncdef ? sym.GetFuncdef().modifiers : sym.GetFunction().modifiers;
    const std::string& returnType = isFuncdef ? sym.GetFuncdef().returnType : sym.GetFunction().returnType;
    const auto& parameters = isFuncdef ? sym.GetFuncdef().parameters : sym.GetFunction().parameters;

    std::ostringstream oss;
    oss << FormatDeclarationPrefix(modifiers);
    AppendFunctionPreamble(oss, sym, isFuncdef);
    if (!IsConstructorOrDestructorSymbol(sym, isFuncdef, returnType))
    {
        oss << FormatReturnType(returnType, modifiers) << " ";
    }
    AppendFunctionName(oss, sym, qualified);
    oss << "(";
    AppendParameters(oss, parameters);
    oss << ")" << FormatFunctionSuffix(modifiers);
    if (!isFuncdef)
    {
        AppendImportOrigin(oss, sym);
    }

    return oss.str();
}

std::string FormatVariableDeclaration(const VariableSignature& var, const std::string& name)
{
    std::ostringstream oss;
    oss << FormatAccessPrefix(var.modifiers);

    const std::string typeText = ApplyConstAndHandle(var.typeName, var.modifiers.isConst, var.modifiers.isHandle);
    oss << (typeText.empty() ? "auto" : typeText) << " " << name;

    // A virtual property is a variable to the symbol table and an accessor pair to the reader.
    // Rendering only the former gave hover `string Name` for `string Name { get const {...} }`,
    // with nothing to say it was a property at all, let alone which half of it exists.
    if (var.isVirtualProperty)
    {
        const auto accessor = [](const char* keyword, bool isConst, bool isOverride, bool isFinal)
        {
            std::string text = keyword;
            if (isConst)
            {
                text += " const";
            }
            if (isOverride)
            {
                text += " override";
            }
            if (isFinal)
            {
                text += " final";
            }
            return text + "; ";
        };

        oss << " { ";
        if (var.hasGet)
        {
            oss << accessor("get", var.isGetConst, var.isGetOverride, var.isGetFinal);
        }
        if (var.hasSet)
        {
            oss << accessor("set", false, var.isSetOverride, var.isSetFinal);
        }
        oss << "}";
    }
    else if (!var.defaultValue.empty())
    {
        oss << " = " << var.defaultValue;
    }

    return oss.str();
}

void FormatClassDeclaration(std::ostringstream& oss, const Symbol& sym, bool qualified = true)
{
    const auto& sig = sym.GetClass();
    oss << FormatAccessPrefix(sig.modifiers);

    if (sig.modifiers.isExternal)
    {
        oss << "external ";
    }
    if (sig.modifiers.isShared)
    {
        oss << "shared ";
    }
    if (sig.modifiers.isMixin)
    {
        oss << "mixin ";
    }
    if (sig.modifiers.isAbstract)
    {
        oss << "abstract ";
    }
    if (sig.modifiers.isFinal)
    {
        oss << "final ";
    }

    const std::string displayName =
        (qualified && !sym.containerName.empty()) ? utils::JoinScopedName(sym.containerName, sym.name) : sym.name;
    oss << "class " << displayName;

    if (sig.isTemplate && !sig.templateParams.empty())
    {
        oss << "<";
        for (size_t i = 0; i < sig.templateParams.size(); ++i)
        {
            if (i > 0)
            {
                oss << ", ";
            }
            oss << sig.templateParams[i];
        }
        oss << ">";
    }

    for (size_t i = 0; i < sig.bases.size(); ++i)
    {
        oss << (i == 0 ? " : " : ", ") << sig.bases[i];
    }
}

void FormatInterfaceDeclaration(std::ostringstream& oss, const Symbol& sym, bool qualified = true)
{
    const auto& sig = sym.GetInterface();
    oss << FormatAccessPrefix(sig.modifiers);

    if (sig.modifiers.isExternal)
    {
        oss << "external ";
    }
    if (sig.modifiers.isShared)
    {
        oss << "shared ";
    }

    const std::string displayName =
        (qualified && !sym.containerName.empty()) ? utils::JoinScopedName(sym.containerName, sym.name) : sym.name;
    oss << "interface " << displayName;

    for (size_t i = 0; i < sig.inheritedInterfaces.size(); ++i)
    {
        oss << (i == 0 ? " : " : ", ") << sig.inheritedInterfaces[i];
    }
}

std::string FormatTypeDeclaration(const Symbol& sym, bool qualified)
{
    std::ostringstream oss;
    if (sym.type == SymbolType::Class)
    {
        FormatClassDeclaration(oss, sym, qualified);
    }
    else if (sym.type == SymbolType::Interface)
    {
        FormatInterfaceDeclaration(oss, sym, qualified);
    }
    return oss.str();
}
} // namespace angel_lsp::analysis
