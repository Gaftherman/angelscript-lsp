#include "analysis/OverloadResolver.h"
#include "analysis/SemanticHelpers.h"
#include <algorithm>

namespace angel_lsp::analysis
{
    namespace
    {
        std::string NormalizeType(std::string_view typeName)
        {
            while (!typeName.empty() && (typeName.front() == ' ' || typeName.front() == '\t'))
            {
                typeName.remove_prefix(1);
            }
            while (!typeName.empty() && (typeName.back() == ' ' || typeName.back() == '\t'))
            {
                typeName.remove_suffix(1);
            }
            if (typeName.starts_with("const "))
            {
                typeName.remove_prefix(6);
            }
            while (!typeName.empty() && (typeName.front() == ' ' || typeName.front() == '\t'))
            {
                typeName.remove_prefix(1);
            }

            std::string result(typeName);
            bool modified = true;
            while (modified)
            {
                modified = false;
                while (!result.empty() && (result.back() == '@' || result.back() == '&' || result.back() == ' ' || result.back() == '\t'))
                {
                    result.pop_back();
                    modified = true;
                }
                if (result.ends_with(" const"))
                {
                    result.resize(result.size() - 6);
                    modified = true;
                }
                if (result.ends_with("&in") || result.ends_with("&out") || result.ends_with("&inout") ||
                    result.ends_with("& in") || result.ends_with("& out") || result.ends_with("& inout"))
                {
                    size_t amp = result.rfind('&');
                    if (amp != std::string::npos)
                    {
                        result.resize(amp);
                        modified = true;
                    }
                }
            }

            while (!result.empty() && (result.back() == ' ' || result.back() == '\t'))
            {
                result.pop_back();
            }

            if (result == "int32") { return "int"; }
            if (result == "uint32") { return "uint"; }
            return result;
        }

        bool HasHandleModifier(std::string_view typeName)
        {
            return typeName.find('@') != std::string_view::npos;
        }

        bool HasConstModifier(std::string_view typeName)
        {
            return typeName.starts_with("const ") || typeName.ends_with(" const");
        }

        std::string LastScopeSegment(const std::string &name)
        {
            const size_t pos = name.rfind("::");
            return pos == std::string::npos ? name : name.substr(pos + 2);
        }

        bool HasUserConversion(const std::string &fromType, const std::string &toType, const SymbolTable &symbolTable)
        {
            if (fromType.empty() || toType.empty())
            {
                return false;
            }

            // 1. Converting constructor on toType: toType(fromType)
            auto toSyms = symbolTable.FindSymbolsPtr(toType + "::" + toType);
            if (toSyms)
            {
                for (const auto &sym : *toSyms)
                {
                    if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
                    {
                        const auto &sig = sym.GetFunction();
                        if (sig.modifiers.isExplicit || sig.modifiers.isDelete)
                        {
                            continue;
                        }
                        if (!sig.parameters.empty())
                        {
                            std::string paramBase = NormalizeType(sig.parameters[0].typeName);
                            if (paramBase == fromType)
                            {
                                bool remainingDefault = true;
                                for (size_t i = 1; i < sig.parameters.size(); ++i)
                                {
                                    if (sig.parameters[i].defaultValue.empty())
                                    {
                                        remainingDefault = false;
                                        break;
                                    }
                                }
                                if (remainingDefault)
                                {
                                    return true;
                                }
                            }
                        }
                    }
                }
            }

            // 2. Implicit conversion method on fromType: opImplConv() -> toType or opImplCast() -> toType
            for (const char *opName : { "opImplConv", "opImplCast" })
            {
                auto opSyms = symbolTable.FindSymbolsPtr(fromType + "::" + opName);
                if (opSyms)
                {
                    for (const auto &sym : *opSyms)
                    {
                        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
                        {
                            const auto &sig = sym.GetFunction();
                            if (NormalizeType(sig.returnType) == toType)
                            {
                                return true;
                            }
                        }
                    }
                }
            }

            return false;
        }

        std::string UnwrapTypedef(const std::string &typeName, const SymbolTable &symbolTable)
        {
            std::string current = NormalizeType(typeName);
            const auto syms = symbolTable.FindSymbolsPtr(current);
            if (syms)
            {
                for (const auto &s : *syms)
                {
                    if (s.type == SymbolType::Typedef && std::holds_alternative<TypedefSignature>(s.signature))
                    {
                        return NormalizeType(s.GetTypedef().baseType);
                    }
                }
            }
            return current;
        }
    }

    bool IsPrimitiveWidening(const std::string &fromType, const std::string &toType)
    {
        const std::string from = NormalizeType(fromType);
        const std::string to = NormalizeType(toType);

        if (from == to)
        {
            return false;
        }

        if (from == "bool")
        {
            return to == "int8" || to == "uint8" || to == "int16" || to == "uint16" ||
                   to == "int" || to == "uint" || to == "int64" || to == "uint64" ||
                   to == "float" || to == "double";
        }
        if (from == "int8")
        {
            return to == "int16" || to == "int" || to == "int64" || to == "float" || to == "double";
        }
        if (from == "uint8")
        {
            return to == "uint16" || to == "int16" || to == "uint" || to == "int" ||
                   to == "uint64" || to == "int64" || to == "float" || to == "double";
        }
        if (from == "int16")
        {
            return to == "int" || to == "int64" || to == "float" || to == "double";
        }
        if (from == "uint16")
        {
            return to == "uint" || to == "int" || to == "uint64" || to == "int64" ||
                   to == "float" || to == "double";
        }
        if (from == "int")
        {
            return to == "int64" || to == "float" || to == "double";
        }
        if (from == "uint")
        {
            return to == "uint64" || to == "int64" || to == "float" || to == "double";
        }
        if (from == "int64" || from == "uint64")
        {
            return to == "double";
        }
        if (from == "float")
        {
            return to == "double";
        }

        return false;
    }

    bool IsPrimitiveNarrowing(const std::string &fromType, const std::string &toType)
    {
        const std::string from = NormalizeType(fromType);
        const std::string to = NormalizeType(toType);

        if (from == to)
        {
            return false;
        }

        const auto isNumeric = [](const std::string &t)
        {
            return t == "int8" || t == "uint8" || t == "int16" || t == "uint16" ||
                   t == "int" || t == "uint" || t == "int64" || t == "uint64" ||
                   t == "float" || t == "double";
        };

        if (isNumeric(from) && isNumeric(to))
        {
            return !IsPrimitiveWidening(from, to);
        }
        if (isNumeric(from) && to == "bool")
        {
            return true;
        }
        if (from == "bool" && isNumeric(to))
        {
            return true;
        }
        return false;
    }

    int ScoreArgumentMatch(
        const std::string &argType,
        const ParameterInformation &param,
        const SymbolTable &symbolTable)
    {
        // Variadic parameter / ellipsis matches anything with a slight penalty
        if (param.rawText.find("...") != std::string::npos)
        {
            return 10;
        }

        // Unknown / uninferrable argument type -> neutral score (passable)
        if (argType.empty() || argType == "auto")
        {
            return 0;
        }

        const bool paramIsHandle = param.isHandle || HasHandleModifier(param.typeName);
        const bool paramIsConst = param.isConst || HasConstModifier(param.typeName);
        const bool isMutableRef = (param.isReference || param.modifier == ParameterModifier::Out ||
                                   param.modifier == ParameterModifier::InOut) &&
                                  !paramIsConst && param.modifier != ParameterModifier::In;

        // Null literal
        if (argType == "null")
        {
            if (paramIsHandle)
            {
                return static_cast<int>(OverloadMatchPenalty::Exact);
            }
            return static_cast<int>(OverloadMatchPenalty::Incompatible);
        }

        // Void specifier for &out parameter
        if (argType == "void")
        {
            if (param.modifier == ParameterModifier::Out ||
                param.rawText.find("&out") != std::string::npos ||
                param.typeName.find("&out") != std::string::npos ||
                param.typeName.find("& out") != std::string::npos)
            {
                return static_cast<int>(OverloadMatchPenalty::Exact);
            }
            return static_cast<int>(OverloadMatchPenalty::Incompatible);
        }

        // Init list for array / container parameter
        if (argType == "init_list")
        {
            if (param.typeName.find("array<") != std::string::npos ||
                param.rawText.find("array<") != std::string::npos ||
                param.typeName.find("vector<") != std::string::npos)
            {
                return static_cast<int>(OverloadMatchPenalty::Exact);
            }
            return static_cast<int>(OverloadMatchPenalty::Incompatible);
        }

        const std::string cleanArg = UnwrapTypedef(NormalizeType(argType), symbolTable);
        const std::string cleanParam = UnwrapTypedef(NormalizeType(param.typeName), symbolTable);
        const bool argIsHandle = HasHandleModifier(argType);
        const bool argIsConst = HasConstModifier(argType);
        const auto isMatchingType = [](const std::string &a, const std::string &b)
        {
            return a == b || LastScopeSegment(a) == LastScopeSegment(b);
        };

        // Mutable non-const reference parameter requires exact type and non-const lvalue
        if (isMutableRef)
        {
            if (!isMatchingType(cleanArg, cleanParam) || argIsHandle != paramIsHandle || argIsConst)
            {
                return static_cast<int>(OverloadMatchPenalty::Incompatible);
            }
            return static_cast<int>(OverloadMatchPenalty::Exact);
        }

        // 1. Exact match
        if (isMatchingType(cleanArg, cleanParam) && argIsHandle == paramIsHandle)
        {
            if (argIsConst == paramIsConst)
            {
                return static_cast<int>(OverloadMatchPenalty::Exact);
            }
            return static_cast<int>(OverloadMatchPenalty::ConstRef);
        }

        // 2. Same base type with const / handle qualification differences
        if (isMatchingType(cleanArg, cleanParam))
        {
            return static_cast<int>(OverloadMatchPenalty::ConstRef);
        }

        // 3. Inheritance / Subtype conversion (Derived -> Base, Derived -> Base@, Derived@ -> Base@)
        if (argIsHandle == paramIsHandle || (paramIsHandle && !argIsHandle))
        {
            auto hierarchy = GetInheritedTypeHierarchy(cleanArg, symbolTable);
            for (size_t dist = 0; dist < hierarchy.size(); ++dist)
            {
                if (isMatchingType(NormalizeType(hierarchy[dist]), cleanParam))
                {
                    int basePenalty = static_cast<int>(OverloadMatchPenalty::Inheritance);
                    if (!argIsHandle && paramIsHandle)
                    {
                        basePenalty += 1;
                    }
                    return basePenalty + static_cast<int>(dist);
                }
            }
        }

        // 4. Primitive widening conversion
        if (IsPrimitiveWidening(cleanArg, cleanParam))
        {
            return static_cast<int>(OverloadMatchPenalty::Widening);
        }

        // 5. Primitive narrowing / cross conversion
        if (IsPrimitiveNarrowing(cleanArg, cleanParam))
        {
            return static_cast<int>(OverloadMatchPenalty::Narrowing);
        }

        // 6. User-defined constructor or opImplConv
        if (HasUserConversion(cleanArg, cleanParam, symbolTable))
        {
            return static_cast<int>(OverloadMatchPenalty::UserDefined);
        }

        return static_cast<int>(OverloadMatchPenalty::Incompatible);
    }

    OverloadMatchResult ResolveBestOverload(
        const std::vector<Symbol> &candidates,
        const std::vector<std::string> &argumentTypes,
        const SymbolTable &symbolTable)
    {
        OverloadMatchResult result;
        const uint32_t argCount = static_cast<uint32_t>(argumentTypes.size());

        for (const auto &sym : candidates)
        {
            if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
            {
                continue;
            }

            const auto &sig = sym.GetFunction();
            uint32_t requiredParams = 0;
            uint32_t maxParams = 0;
            bool isVariadic = false;

            for (const auto &param : sig.parameters)
            {
                if (param.rawText.find("...") != std::string::npos)
                {
                    isVariadic = true;
                    continue;
                }
                ++maxParams;
                if (param.defaultValue.empty())
                {
                    ++requiredParams;
                }
            }

            if (!isVariadic)
            {
                if (argCount < requiredParams || argCount > maxParams)
                {
                    continue;
                }
            }
            else
            {
                if (argCount < requiredParams)
                {
                    continue;
                }
            }

            int currentScore = 0;
            bool incompatible = false;

            for (uint32_t i = 0; i < argCount; ++i)
            {
                if (i < sig.parameters.size())
                {
                    int paramScore = ScoreArgumentMatch(argumentTypes[i], sig.parameters[i], symbolTable);
                    if (paramScore >= static_cast<int>(OverloadMatchPenalty::Incompatible))
                    {
                        incompatible = true;
                        break;
                    }
                    currentScore += paramScore;
                }
                else if (isVariadic)
                {
                    currentScore += 10;
                }
            }

            if (incompatible)
            {
                continue;
            }

            // Penalty for default arguments used to prefer exact arity over defaults
            if (argCount < sig.parameters.size())
            {
                currentScore += static_cast<int>(sig.parameters.size() - argCount) * 1;
            }

            result.viableCandidates.push_back(&sym);

            if (currentScore < result.bestScore)
            {
                result.bestScore = currentScore;
                result.bestCandidate = &sym;
                result.isAmbiguous = false;
            }
            else if (currentScore == result.bestScore)
            {
                result.isAmbiguous = true;
            }
        }

        return result;
    }
}
