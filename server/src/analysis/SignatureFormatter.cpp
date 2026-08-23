#include "analysis/SignatureFormatter.h"

#include <sstream>

namespace angel_lsp::analysis
{
    namespace
    {
        /** @brief Re-attaches a modifier the captured type text may already carry.
         *  @note TypeExtraction normally folds 'const' and '@' into typeName, but the flags are the
         *        authority. Appending unconditionally would produce "const const string", so each
         *        marker is only added when the text does not already show it. */
        std::string ApplyConstAndHandle(const std::string &typeName, bool isConst, bool isHandle)
        {
            std::string result = typeName;

            if (isHandle && (result.empty() || result.back() != '@'))
            {
                result += '@';
            }
            if (isConst && !result.starts_with("const "))
            {
                result.insert(0, "const ");
            }
            return result;
        }
    }

    std::string FormatAccessPrefix(const SymbolModifiers &modifiers)
    {
        switch (modifiers.access)
        {
        case AccessModifier::Private:
            return "private ";
        case AccessModifier::Protected:
            return "protected ";
        case AccessModifier::Public:
        default:
            return "";
        }
    }

    std::string FormatDeclarationPrefix(const SymbolModifiers &modifiers)
    {
        std::string result = FormatAccessPrefix(modifiers);

        if (modifiers.isExternal)
        {
            result += "external ";
        }
        if (modifiers.isShared)
        {
            result += "shared ";
        }
        if (modifiers.isMixin)
        {
            result += "mixin ";
        }
        if (modifiers.isDeclarationAbstract)
        {
            result += "abstract ";
        }
        if (modifiers.isDeclarationFinal)
        {
            result += "final ";
        }
        return result;
    }

    std::string FormatParameterReference(const ParameterInformation &param)
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

    std::string FormatParameter(const ParameterInformation &param)
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

    std::string FormatFunctionSuffix(const SymbolModifiers &modifiers)
    {
        std::string result;

        if (modifiers.isConst)
        {
            result += " const";
        }
        if (modifiers.isOverride)
        {
            result += " override";
        }
        // A 'final' written before the return type was already emitted by FormatDeclarationPrefix.
        if (modifiers.isFinal && !modifiers.isDeclarationFinal)
        {
            result += " final";
        }
        if (modifiers.isExplicit)
        {
            result += " explicit";
        }
        if (modifiers.isProperty)
        {
            result += " property";
        }
        if (modifiers.isDelete)
        {
            result += " delete";
        }
        return result;
    }

    std::string FormatReturnType(const std::string &returnType, const SymbolModifiers &modifiers)
    {
        if (returnType.empty())
        {
            return "void";
        }

        if (modifiers.isReturnReference && returnType.back() != '&')
        {
            return returnType + "&";
        }
        return returnType;
    }

    std::string FormatFunctionDeclaration(const Symbol &sym, bool qualified)
    {
        if (sym.type != SymbolType::Function && sym.type != SymbolType::Funcdef)
        {
            return "";
        }

        const bool isFuncdef = sym.type == SymbolType::Funcdef;
        const SymbolModifiers &modifiers = isFuncdef ? sym.GetFuncdef().modifiers : sym.GetFunction().modifiers;
        const std::string &returnType = isFuncdef ? sym.GetFuncdef().returnType : sym.GetFunction().returnType;
        const std::vector<ParameterInformation> &parameters =
            isFuncdef ? sym.GetFuncdef().parameters : sym.GetFunction().parameters;

        std::ostringstream oss;
        oss << FormatDeclarationPrefix(modifiers);

        if (isFuncdef)
        {
            oss << "funcdef ";
        }

        oss << FormatReturnType(returnType, modifiers) << " ";

        if (qualified && !sym.containerName.empty())
        {
            oss << sym.containerName << "::";
        }
        oss << sym.name << "(";

        for (size_t i = 0; i < parameters.size(); ++i)
        {
            if (i > 0)
            {
                oss << ", ";
            }
            oss << FormatParameter(parameters[i]);
        }

        oss << ")" << FormatFunctionSuffix(modifiers);
        return oss.str();
    }

    std::string FormatVariableDeclaration(const VariableSignature &var, const std::string &name)
    {
        std::ostringstream oss;
        oss << FormatAccessPrefix(var.modifiers);

        const std::string typeText = ApplyConstAndHandle(var.typeName, var.modifiers.isConst, var.modifiers.isHandle);
        oss << (typeText.empty() ? "auto" : typeText) << " " << name;
        return oss.str();
    }

    std::string FormatTypeDeclaration(const Symbol &sym)
    {
        std::ostringstream oss;

        if (sym.type == SymbolType::Class)
        {
            const auto &sig = sym.GetClass();
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

            oss << "class " << sym.name;

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
        else if (sym.type == SymbolType::Interface)
        {
            const auto &sig = sym.GetInterface();
            oss << FormatAccessPrefix(sig.modifiers);

            if (sig.modifiers.isExternal)
            {
                oss << "external ";
            }
            if (sig.modifiers.isShared)
            {
                oss << "shared ";
            }

            oss << "interface " << sym.name;

            for (size_t i = 0; i < sig.inheritedInterfaces.size(); ++i)
            {
                oss << (i == 0 ? " : " : ", ") << sig.inheritedInterfaces[i];
            }
        }

        return oss.str();
    }
}
