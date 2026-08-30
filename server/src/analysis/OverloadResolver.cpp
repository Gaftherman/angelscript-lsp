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

            // `T[]` and `array<T>` are two spellings of one type - the language's bracket syntax is
            // sugar for whatever the engine registered as its default array - so they have to
            // compare equal. They did not, and the standard library is written in both: passing
            // `string::split`'s `array<string>@` to `join(const string[]&in, ...)` was reported as
            // "Cannot implicitly convert 'array<string>@' to 'const string[]'" on correct code.
            //
            // Applied innermost-first so `int[][]` folds all the way down to `array<array<int>>`.
            while (result.ends_with("[]"))
            {
                result = "array<" + result.substr(0, result.size() - 2) + ">";
            }
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

        std::string UnwrapTypedef(const std::string &typeName, const SymbolTable &symbolTable,
                                  int depth = 0)
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

            // A typedef names a primitive, and a primitive is most of what a template argument ever
            // is. `typedef uint8 byte;` makes `array<byte>` and `array<uint8>` the same
            // instantiation, and the compiler accepts a call between them in either direction -
            // but unwrapping only the outer name left them as two unrelated spellings, so a legal
            // call was reported. That is a false positive, which is the one failure mode this
            // project does not accept. tests/parity/doc_p19_typedef_template_argument.as.
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
            for (const auto &argument : SplitTemplateArguments(inner))
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
    }

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
    bool IsIntegerType(const std::string &typeName)
    {
        return IsIntegerPrimitive(NormalizeType(typeName));
    }

    /** @brief True for AngelScript's floating point primitives. */
    bool IsFloatingPointType(const std::string &typeName)
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
     */
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
            return to == "int16" || to == "int" || to == "int32" || to == "int64" ||
                   to == "uint8" || to == "uint16" || to == "uint" || to == "uint32" || to == "uint64" ||
                   to == "float" || to == "double";
        }
        if (from == "uint8")
        {
            return to == "uint16" || to == "int16" || to == "uint" || to == "int" ||
                   to == "uint64" || to == "int64" || to == "float" || to == "double";
        }
        if (from == "int16")
        {
            return to == "int" || to == "int32" || to == "int64" ||
                   to == "uint16" || to == "uint" || to == "uint32" || to == "uint64" ||
                   to == "float" || to == "double";
        }
        if (from == "uint16")
        {
            return to == "uint" || to == "int" || to == "uint64" || to == "int64" ||
                   to == "float" || to == "double";
        }
        if (from == "int" || from == "int32")
        {
            return to == "int32" || to == "int" || to == "int64" ||
                   to == "uint" || to == "uint32" || to == "uint64" ||
                   to == "float" || to == "double";
        }
        if (from == "uint" || from == "uint32")
        {
            return to == "uint32" || to == "uint" || to == "uint64" ||
                   to == "int" || to == "int32" || to == "int64" ||
                   to == "float" || to == "double";
        }
        if (from == "int64" || from == "uint64")
        {
            return to == "int64" || to == "uint64" || to == "double";
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

        const auto isNumeric = [](const std::string &t) { return IsNumericPrimitive(t); };

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

    /**
     * @brief True when two candidates declare the same signature, parameter for parameter.
     *
     * Used to tell a genuine overload ambiguity apart from the same declaration arriving twice,
     * which is what happens when two stubs describing the same standard library are both loaded.
     */
    bool HasSameSignature(const Symbol &left, const Symbol &right)
    {
        if (!std::holds_alternative<FunctionSignature>(left.signature) ||
            !std::holds_alternative<FunctionSignature>(right.signature))
        {
            return false;
        }

        const auto &a = left.GetFunction();
        const auto &b = right.GetFunction();

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

        // AngelScript's variable type: `const ?&in` / `?&out` accepts any type, so this parameter
        // can never be the reason an overload does not match. Scored like the ellipsis above -
        // viable, but a shade worse than a concrete parameter that matches exactly, so a typed
        // overload still wins over the wildcard one when both are candidates.
        if (IsVariableType(param.typeName) || IsVariableType(param.rawText))
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

        // 3b. An enum widens out to an integer, and `auto` is not a type at all.
        //
        // `Take(ModeOne)` against `void Take(int)` compiles - an enum is an integer with a name,
        // and only the *inward* direction is closed (`Color c = 1;` is the error, doc_r09). Eleven
        // corpus findings were flag arguments spelled as the enum they belong to, which is how
        // every one of them is meant to be written.
        //
        // `auto` reaches this as the resolved type of a deduced variable. The deduction happens in
        // the compiler and its result is not written anywhere the analyzer can read, so scoring it
        // against a parameter asks a question with no answer - four more findings, all of them
        // `auto@` handles passed to a function expecting the type they were deduced from.
        if (cleanArg == "auto" || cleanParam == "auto")
        {
            return static_cast<int>(OverloadMatchPenalty::UnknownTypes);
        }
        const auto namesAnEnum = [&symbolTable](const std::string &typeName)
        {
            const auto symbols = symbolTable.FindSymbolsPtr(typeName);
            return symbols && std::any_of(symbols->begin(), symbols->end(),
                                          [](const Symbol &sym) { return sym.type == SymbolType::Enum; });
        };

        if (IsIntegerType(cleanParam) && namesAnEnum(cleanArg))
        {
            return static_cast<int>(OverloadMatchPenalty::Widening);
        }

        // 4. Primitive widening conversion. Integer -> floating point is still safe but ranks
        //    below integer -> wider integer, so an overload set offering both is resolvable.
        if (IsPrimitiveWidening(cleanArg, cleanParam))
        {
            const bool crossesKind = IsIntegerType(cleanArg) && IsFloatingPointType(cleanParam);
            return static_cast<int>(crossesKind ? OverloadMatchPenalty::WideningAcrossKind
                                                : OverloadMatchPenalty::Widening);
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

        // 7. Neither side has a declaration to read, so "no relation found" is not a verdict.
        //
        // Engine-registered classes are the whole of this case: `CBasePlayer@` where
        // `CBaseEntity@` is expected is an upcast, and the hierarchy that makes it one is written
        // in C++. Step 3 above walks a hierarchy that stops at the name itself and concludes
        // nothing, and concluding nothing was scoring the same as concluding "incompatible".
        //
        // Both sides, deliberately. One unresolved name against a class this analyzer *can* read
        // is still judged, because the visible half is enough to answer.
        // A primitive is never "unknown": it has no symbol table entry either, but its conversions
        // are the engine's and are fully known, so `int` against a host class must stay
        // Incompatible rather than slip through this door.
        // A template instantiation is judged by its container: `array<uint8>` is never a symbol
        // table key, but `array` is, and a declared `array` is enough to decide that
        // `array<uint8>` and `array<string>` are different instantiations of a type this analyzer
        // can read. Without that, unwrapping one template argument into another would slip through
        // here - see "Unwrapping a template argument does not make unrelated types match".
        const auto isNamedAndUnresolved = [&symbolTable](const std::string &typeName)
        {
            if (typeName.empty() || IsCorePrimitive(typeName) || typeName == "string")
            {
                return false;
            }
            const std::string owner = MemberOwnerType(typeName);
            return !symbolTable.FindSymbolsPtr(typeName) &&
                   (owner.empty() || !symbolTable.FindSymbolsPtr(owner));
        };

        if (isNamedAndUnresolved(cleanArg) && isNamedAndUnresolved(cleanParam))
        {
            return static_cast<int>(OverloadMatchPenalty::UnknownTypes);
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
                // A tie between two *identical* signatures is not an ambiguity - it is the same
                // function declared twice. That happens routinely in a real configuration: a
                // --predefined-file and a built-in engine profile that both describe the standard
                // library will each declare `array<T>::insertLast(const T&in)`, and reporting every
                // call to it as ambiguous made the server unusable against that setup.
                if (result.bestCandidate && !HasSameSignature(*result.bestCandidate, sym))
                {
                    result.isAmbiguous = true;
                }
            }
        }

        return result;
    }
}
