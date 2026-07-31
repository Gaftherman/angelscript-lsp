#include "SymbolTable.h"
#include "spdlog/fmt/fmt.h"

#include <functional>

namespace angel_lsp::analysis
{
    namespace
    {
        std::string AccessModifierToString(AccessModifier access)
        {
            switch (access)
            {
            case AccessModifier::Public:
                return "public";
            case AccessModifier::Private:
                return "private";
            case AccessModifier::Protected:
                return "protected";
            default:
                return "public";
            }
        }

        std::string ParameterModifierToString(ParameterModifier mod)
        {
            switch (mod)
            {
            case ParameterModifier::In:
                return "&in";
            case ParameterModifier::Out:
                return "&out";
            case ParameterModifier::InOut:
                return "&inout";
            case ParameterModifier::None:
                return "";
            default:
                return "";
            }
        }

        std::string JoinStrings(const std::vector<std::string> &items, const char *sep = ", ")
        {
            std::string result;
            for (size_t i = 0; i < items.size(); ++i)
            {
                if (i > 0)
                    result += sep;
                result += items[i];
            }
            return result;
        }

        std::string JoinFlags(const std::vector<std::string> &flags)
        {
            if (flags.empty())
                return "[]";
            return "[" + JoinStrings(flags) + "]";
        }

        void BuildParamStrings(const std::vector<ParameterInformation> &params,
                               std::string &outParamsStr,
                               std::vector<std::string> &outParamLines)
        {
            outParamsStr = "(";
            for (size_t i = 0; i < params.size(); ++i)
            {
                const auto &param = params[i];
                if (i > 0)
                    outParamsStr += ", ";

                std::string modStr = ParameterModifierToString(param.modifier);
                std::string paramTypeStr = modStr.empty() ? param.typeName : param.typeName + " " + modStr;

                outParamsStr += paramTypeStr;
                if (!param.name.empty())
                    outParamsStr += " " + param.name;
                if (!param.defaultValue.empty())
                    outParamsStr += " = " + param.defaultValue;

                std::vector<std::string> pFlags;
                if (param.isConst)
                    pFlags.push_back("const: true");
                if (param.isHandle)
                    pFlags.push_back("handle: true");
                if (param.modifier == ParameterModifier::In)
                    pFlags.push_back("in: true");
                else if (param.modifier == ParameterModifier::Out)
                    pFlags.push_back("out: true");
                else if (param.modifier == ParameterModifier::InOut)
                    pFlags.push_back("inout: true");

                std::string defaultStr = param.defaultValue.empty()
                                             ? ""
                                             : fmt::format(" | Default: \"{}\"", param.defaultValue);

                outParamLines.push_back(fmt::format("      \u2514\u2500 Parameter: Type: \"{}\" | Name: \"{}\"{}  | Flags: {}",
                                                    paramTypeStr, param.name, defaultStr, JoinFlags(pFlags)));
            }
            outParamsStr += ")";
        }
    }

    void SymbolTable::AddSymbol(const Symbol &symbol)
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        const std::string &key = symbol.qualifiedName.empty() ? symbol.name : symbol.qualifiedName;
        m_symbols[key].push_back(symbol);
    }

    void SymbolTable::ClearDocumentSymbols(const std::string &fileUri)
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);

        for (auto it = m_symbols.begin(); it != m_symbols.end();)
        {
            auto &vec = it->second;

            std::erase_if(vec, [&fileUri](const Symbol &sym)
                          { return sym.fileUri == fileUri; });

            if (vec.empty())
                it = m_symbols.erase(it);
            else
                ++it;
        }
    }

    bool SymbolTable::HasSymbol(const std::string &qualifiedName) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_symbols.contains(qualifiedName);
    }

    const std::vector<Symbol> *SymbolTable::FindSymbolsPtr(const std::string &qualifiedName) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_symbols.find(qualifiedName);
        return it != m_symbols.end() ? &it->second : nullptr;
    }

    std::vector<Symbol> SymbolTable::FindSymbols(const std::string &qualifiedName) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_symbols.find(qualifiedName);
        return it != m_symbols.end() ? it->second : std::vector<Symbol>{};
    }

    std::optional<Symbol> SymbolTable::FindFirstSymbol(const std::string &qualifiedName) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_symbols.find(qualifiedName);
        if (it != m_symbols.end() && !it->second.empty())
            return it->second.front();
        return std::nullopt;
    }

    bool SymbolTable::HasSymbolAnywhere(const std::string &name) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        std::string searchName = name;
        if (searchName.rfind("::", 0) == 0)
            searchName = searchName.substr(2);

        if (m_symbols.find(searchName) != m_symbols.end() || m_symbols.find(name) != m_symbols.end())
            return true;

        for (const auto &[key, symbols] : m_symbols)
        {
            for (const auto &sym : symbols)
            {
                if (sym.name == searchName || sym.name == name || sym.qualifiedName == searchName || sym.qualifiedName == name)
                    return true;
            }
        }
        return false;
    }

    void SymbolTable::ForEachSymbol(const std::function<void(const std::string &, const std::vector<Symbol> &)> &visitor) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        for (const auto &[key, symbols] : m_symbols)
            visitor(key, symbols);
    }

    void SymbolTable::PrintSymbols(angel_lsp::utils::LspLogger *logger) const
    {
        if (!logger)
            return;

        std::shared_lock<std::shared_mutex> lock(m_mutex);
        logger->LogInfo(fmt::format("=== SYMBOL TABLE ({} unique keys) ===", m_symbols.size()));

        for (const auto &[key, symbols] : m_symbols)
        {
            for (const auto &sym : symbols)
            {
                const std::string typeStr = SymbolTypeToString(sym.type);
                const std::string nameStr = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                const std::string range = fmt::format("[L{}:C{}-L{}:C{}]",
                                                      sym.startLine + 1, sym.startCharacter + 1,
                                                      sym.endLine + 1, sym.endCharacter + 1);

                switch (sym.type)
                {
                case SymbolType::Function:
                {
                    const auto &sig = sym.GetFunction();
                    std::vector<std::string> flags;
                    if (sig.modifiers.isShared)
                        flags.push_back("shared");
                    if (sig.modifiers.isOverride)
                        flags.push_back("override");
                    if (sig.modifiers.isFinal)
                        flags.push_back("final");
                    if (sig.modifiers.isReturnReference)
                        flags.push_back("ref_return");
                    if (sig.modifiers.isDelete)
                        flags.push_back("delete");
                    if (sig.modifiers.isExternal)
                        flags.push_back("external");
                    if (sig.modifiers.isExplicit)
                        flags.push_back("explicit");

                    std::string paramsStr;
                    std::vector<std::string> paramLines;
                    BuildParamStrings(sig.parameters, paramsStr, paramLines);

                    logger->LogInfo(fmt::format("  \u2022 [{}] Access: {} | Ret: \"{}\" | Name: \"{}\" | Params: \"{}\" | Flags: {} | {}",
                                                typeStr,
                                                AccessModifierToString(sig.modifiers.access),
                                                sig.returnType, nameStr, paramsStr,
                                                JoinFlags(flags), range));

                    for (const auto &line : paramLines)
                        logger->LogInfo(line);

                    break;
                }
                case SymbolType::Variable:
                {
                    const auto &sig = sym.GetVariable();
                    std::vector<std::string> flags;
                    if (sig.modifiers.isShared)
                        flags.push_back("shared");
                    if (sig.modifiers.isConst)
                        flags.push_back("const");
                    if (sig.modifiers.isHandle)
                        flags.push_back("handle");

                    std::string defaultStr = sig.defaultValue.empty()
                                                 ? ""
                                                 : fmt::format(" | Default: \"{}\"", sig.defaultValue);

                    logger->LogInfo(fmt::format("  \u2022 [{}] Access: {} | Type: \"{}\" | Name: \"{}\"{}  | Flags: {} | {}",
                                                typeStr,
                                                AccessModifierToString(sig.modifiers.access),
                                                sig.typeName, nameStr, defaultStr,
                                                JoinFlags(flags), range));
                    break;
                }
                case SymbolType::Class:
                {
                    const auto &sig = sym.GetClass();
                    std::vector<std::string> flags;
                    if (sig.modifiers.isShared)
                        flags.push_back("shared");
                    if (sig.modifiers.isMixin)
                        flags.push_back("mixin");
                    if (sig.modifiers.isAbstract)
                        flags.push_back("abstract");
                    if (sig.modifiers.isFinal)
                        flags.push_back("final");

                    std::string basesStr = sig.bases.empty()
                                               ? ""
                                               : fmt::format(" | Bases: \"{}\"", JoinStrings(sig.bases));

                    logger->LogInfo(fmt::format("  \u2022 [{}] Access: {} | Name: \"{}\"{}  | Flags: {} | {}",
                                                typeStr,
                                                AccessModifierToString(sig.modifiers.access),
                                                nameStr, basesStr, JoinFlags(flags), range));
                    break;
                }
                case SymbolType::Interface:
                {
                    const auto &sig = sym.GetInterface();
                    std::vector<std::string> flags;
                    if (sig.modifiers.isShared)
                        flags.push_back("shared");

                    std::string basesStr = sig.inheritedInterfaces.empty()
                                               ? ""
                                               : fmt::format(" | Extends: \"{}\"", JoinStrings(sig.inheritedInterfaces));

                    logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\"{}  | Flags: {} | {}",
                                                typeStr, nameStr, basesStr, JoinFlags(flags), range));
                    break;
                }
                case SymbolType::Enum:
                {
                    const auto &sig = sym.GetEnum();
                    std::vector<std::string> flags;
                    if (sig.modifiers.isShared)
                        flags.push_back("shared");

                    logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\" | Flags: {} | {}",
                                                typeStr, nameStr, JoinFlags(flags), range));
                    break;
                }
                case SymbolType::Typedef:
                {
                    logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\" | {}",
                                                typeStr, nameStr, range));
                    break;
                }
                case SymbolType::Funcdef:
                {
                    logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\" | {}",
                                                typeStr, nameStr, range));
                    break;
                }
                case SymbolType::Property:
                {
                    logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\" | {}",
                                                typeStr, nameStr, range));
                    break;
                }
                case SymbolType::Namespace:
                {
                    logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\" | {}",
                                                typeStr, nameStr, range));
                    break;
                }
                default:
                {
                    logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\" | {}",
                                                typeStr, nameStr, range));
                    break;
                }
                }
            }
        }
    }
}