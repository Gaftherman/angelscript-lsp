#include "analysis/TypeConversionChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/BinaryOperatorChecker.h"
#include "analysis/ComparisonOperatorChecker.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/ExpressionTypeCache.h"
#include "analysis/HandleComparisonChecker.h"
#include "analysis/NodeIndex.h"
#include "analysis/RepeatedConversionChecker.h"

#include "analysis/SemanticHelpers.h"
#include "analysis/TypeSanitization.h"

#include "parser/GrammarNames.h"
#include "parser/Primitives.h"
#include "utils/LspLogger.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"
#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <array>
#include <functional>
#include <spdlog/fmt/fmt.h>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
/** @brief What a resolved expression is worth to this pass. */
struct ExpressionType
{
    std::string baseName;   ///< Cleaned base type name, e.g. "Money" or "int".
    bool known = false;     ///< False means "give up" - never a reason to diagnose.
    bool isLiteral = false; ///< A literal's type is exact, so it can be judged strictly.
};

/** @brief The little a rule needs to know about a type declaration.
 *  @note Deliberately not the Symbol itself: this is looked up for practically every call
 *        expression in a document, and copying a Symbol (strings, parameter vectors, a
 *        variant) that many times dominated the pass. */
struct TypeDeclarationInfo
{
    bool found = false;
    bool isClass = false;
    bool isTemplate = false;
};

struct ConversionTypes
{
    std::string from{};
    std::string to{};
};

bool IsSameType(const std::string& a, const std::string& b);

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

bool IsSameType(const std::string& a, const std::string& b)
{
    const std::string ca = CanonicalizeType(a);
    const std::string cb = CanonicalizeType(b);
    if (ca == cb)
    {
        return true;
    }
    const size_t aOpen = ca.find('<');
    const size_t bOpen = cb.find('<');
    if (aOpen != std::string::npos && ca.ends_with('>') && bOpen != std::string::npos && cb.ends_with('>'))
    {
        const std::string_view aCont = std::string_view(ca).substr(0, aOpen);
        const std::string_view bCont = std::string_view(cb).substr(0, bOpen);
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
        return AreTemplateArgumentsEqual(std::string_view(ca).substr(aOpen + 1, ca.size() - aOpen - 2),
                                         std::string_view(cb).substr(bOpen + 1, cb.size() - bOpen - 2));
    }
    if (utils::ContainsScopeSeparator(ca) && utils::ContainsScopeSeparator(cb))
    {
        return false;
    }
    return LastScopeSegment(ca) == LastScopeSegment(cb);
}

/** @brief Visits every symbol registered under a qualified name without copying the bucket.
 *  @param visitor Returns true to stop the walk. */
void ForEachSymbolNamed(std::string_view qualifiedName, const SymbolTable& table,
                        const std::function<bool(const Symbol&)>& visitor)
{
    const auto bucket = table.FindSymbolsPtr(qualifiedName);
    if (!bucket)
    {
        return;
    }
    for (const auto& sym : *bucket)
    {
        if (visitor(sym))
        {
            return;
        }
    }
}

/** @brief Visits every symbol registered under a member of a scope without copying the bucket.
 *  @param visitor Returns true to stop the walk. */
void ForEachMemberSymbolNamed(std::string_view scope, std::string_view member, const SymbolTable& table,
                              const std::function<bool(const Symbol&)>& visitor)
{
    const auto bucket = table.FindMemberSymbolPtr(scope, member);
    if (!bucket)
    {
        return;
    }
    for (const auto& sym : *bucket)
    {
        if (visitor(sym))
        {
            return;
        }
    }
}

/**
 * @brief Trims leading and trailing whitespace from a string.
 * @param[in] str Input string.
 * @return Trimmed string.
 */
static std::string TrimString(std::string str)
{
    while (!str.empty() && isspace(static_cast<unsigned char>(str.front())))
    {
        str.erase(str.begin());
    }
    while (!str.empty() && isspace(static_cast<unsigned char>(str.back())))
    {
        str.pop_back();
    }
    return str;
}

/**
 * @brief Collects candidate symbols of type Function matching a name.
 * @param[in] funcName Target function name or qualified name.
 * @param[in] table Workspace symbol table.
 * @return Vector of function symbols matching the name.
 */
static std::vector<Symbol> CollectFunctionCandidates(const std::string& funcName, const SymbolTable& table)
{
    std::vector<Symbol> candidates;
    auto addCandidates = [&](const std::string& name)
    {
        if (auto found = table.FindSymbolsPtr(name))
        {
            for (const auto& s : *found)
            {
                if (s.type == SymbolType::Function)
                {
                    candidates.push_back(s);
                }
            }
        }
    };

    addCandidates(funcName);
    if (candidates.empty())
    {
        addCandidates(std::string(LastScopeSegment(funcName)));
    }
    return candidates;
}

std::optional<Symbol> FindFuncdef(const std::string& name, const SymbolTable& table);

/** @brief Looks up what a type name denotes.
 *  @return found == false when the name resolves to nothing this analyzer can see - which
 *          is the signal to stay silent about anything involving it. */
TypeDeclarationInfo FindTypeDeclaration(const std::string& typeName, const SymbolTable& table)
{
    TypeDeclarationInfo info;
    if (typeName.empty())
    {
        return info;
    }

    ForEachSymbolNamed(typeName, table,
                       [&info](const Symbol& sym)
                       {
                           if (sym.type != SymbolType::Class && sym.type != SymbolType::Interface)
                           {
                               return false;
                           }
                           info.found = true;
                           info.isClass = sym.type == SymbolType::Class;
                           info.isTemplate = info.isClass && sym.GetClass().isTemplate;
                           return true;
                       });

    if (info.found)
    {
        return info;
    }

    const std::string_view bare = LastScopeSegment(typeName);
    const auto matches = table.FindTypeSymbolsByShortName(bare);
    for (const auto& sym : matches)
    {
        if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface)
        {
            info.found = true;
            info.isClass = sym.type == SymbolType::Class;
            info.isTemplate = info.isClass && sym.GetClass().isTemplate;
            return info;
        }
    }

    return info;
}

/** @brief True when a name resolves to an enum declared somewhere the analyzer can see. */
bool ResolvesToEnum(const std::string& typeName, const SymbolTable& table)
{
    bool found = false;
    const std::string_view bare = LastScopeSegment(typeName);

    for (const auto candidate : {std::string_view(typeName), bare})
    {
        ForEachSymbolNamed(candidate, table,
                           [&found](const Symbol& sym)
                           {
                               if (sym.type == SymbolType::Enum)
                               {
                                   found = true;
                                   return true;
                               }
                               return false;
                           });

        if (found || bare == typeName)
        {
            break;
        }
    }
    return found;
}

/** @brief True when a name resolves to an enum, typedef, funcdef or namespace.
 *  @note Keeps those out of the class-shaped rules below rather than letting them fall
 *        through to "not a class, therefore suspicious". */
bool ResolvesToNonClassDeclaration(const std::string& typeName, const SymbolTable& table)
{
    bool found = false;
    const std::string_view bare = LastScopeSegment(typeName);

    for (const auto candidate : {std::string_view(typeName), bare})
    {
        ForEachSymbolNamed(candidate, table,
                           [&found](const Symbol& sym)
                           {
                               if (sym.type == SymbolType::Enum || sym.type == SymbolType::Typedef ||
                                   sym.type == SymbolType::Funcdef || sym.type == SymbolType::Namespace)
                               {
                                   found = true;
                                   return true;
                               }
                               return false;
                           });

        if (found || bare == typeName)
        {
            break;
        }
    }
    return found;
}

/** @brief Visits every overload of a named method visible on a type, base types included.
 *  @param visitor Returns true to stop the walk. */
void ForEachMethod(const std::string& typeName, const std::string& memberName, const SymbolTable& table,
                   const std::function<bool(const Symbol&)>& visitor)
{
    bool stopped = false;
    for (const auto& cls : GetInheritedTypeHierarchy(typeName, table))
    {
        ForEachMemberSymbolNamed(cls, memberName, table,
                                 [&](const Symbol& sym)
                                 {
                                     if (sym.type != SymbolType::Function)
                                     {
                                         return false;
                                     }
                                     stopped = visitor(sym);
                                     return stopped;
                                 });

        if (stopped)
        {
            return;
        }
    }
}

/** @brief Visits the constructors declared directly on a type.
 *  @note Deliberately not hierarchy-wide: AngelScript does not inherit constructors. */

void ForEachConstructor(const std::string& typeName, const SymbolTable& table,
                        const std::function<bool(const Symbol&)>& visitor)
{
    // The arguments come off before the key is built. A constructor is stored under the
    // class's own name - `weakref::weakref` - so looking up `weakref<Node>` produced the key
    // `weakref<Node>::weakref<Node>`, which matches nothing: a template's constructors were
    // invisible here, and every `weakref<Node> w(node);` read as having none.
    const size_t open = typeName.find('<');
    const std::string unparameterized = (open == std::string::npos) ? typeName : typeName.substr(0, open);

    const std::string bare{LastScopeSegment(unparameterized)};
    bool sawAny = false;
    bool stopped = false;

    const auto visit = [&](const Symbol& sym)
    {
        if (sym.type != SymbolType::Function)
        {
            return false;
        }
        sawAny = true;
        stopped = visitor(sym);
        return stopped;
    };

    ForEachMemberSymbolNamed(unparameterized, bare, table, visit);
    if (!stopped && !sawAny && bare != unparameterized)
    {
        ForEachMemberSymbolNamed(bare, bare, table, visit);
    }

    // A class declared inside a namespace is keyed by its qualified name, so `Hook`'s
    // constructor is `Hooks::Hook::Hook` and neither lookup above reaches it - the call
    // site writes `Hook("OnMapActivate")` because it is inside the namespace, and the two
    // keys built from that spelling match nothing. The class itself was still found,
    // because FindTypeDeclaration already falls back to the last segment; the constructors
    // were not, so every construction of a namespaced class read as having none. Ten
    // corpus findings, all of them a class calling its own constructor.
    //
    // Matched on the last two segments together, so `Hooks::Hook::Hook` qualifies and a
    // stray `Hook` function somewhere else does not.
    if (!stopped && !sawAny)
    {
        auto shortMatches = table.FindTypeSymbolsByShortName(bare);
        for (const auto& cSym : shortMatches)
        {
            if (cSym.type == SymbolType::Class)
            {
                const std::string_view qCls = cSym.qualifiedName.empty() ? cSym.name : cSym.qualifiedName;
                ForEachMemberSymbolNamed(qCls, bare, table, visit);
                if (stopped || sawAny)
                {
                    break;
                }
            }
        }
    }
}

/**
 * @brief True when an overload can be called with exactly one argument.
 * @note Parameter count alone is not the test: `CLogger(const string &in name, bool
 *       isStatic = false)` is a one-argument converting constructor, and treating it as a
 *       two-argument one is what made the first corpus run flag a legitimate conversion.
 */
bool AcceptsSingleArgument(const std::vector<ParameterInformation>& parameters)
{
    if (parameters.empty())
    {
        return false;
    }
    return std::all_of(parameters.begin() + 1, parameters.end(),
                       [](const ParameterInformation& param) { return !param.defaultValue.empty(); });
}

/** @brief The declared type of the single argument such an overload converts from. */
std::string SingleArgumentType(const std::vector<ParameterInformation>& parameters)
{
    return parameters.empty() ? "" : CanonicalizeType(parameters[0].typeName);
}

/**
 * @brief The overload an assignment operator calls, or empty for one that has none.
 *
 * AngelScript spells each compound assignment as a method: `a += b` is `a.opAddAssign(b)`,
 * and the operand types are that method's business rather than a conversion between the two
 * sides. `@=` is left out on purpose - handle assignment is not an overloadable operator on
 * a script class, measured, and opHndlAssign is a behaviour of application-registered
 * types.
 */
std::string_view AssignmentOverloadName(std::string_view op)
{
    using namespace parser::keywords::operators;
    if (op == Assign)
        return "opAssign";
    if (op == PlusAssign)
        return "opAddAssign";
    if (op == MinusAssign)
        return "opSubAssign";
    if (op == StarAssign)
        return "opMulAssign";
    if (op == SlashAssign)
        return "opDivAssign";
    if (op == PercentAssign)
        return "opModAssign";
    if (op == StarStarAssign)
        return "opPowAssign";
    if (op == AmpAssign)
        return "opAndAssign";
    if (op == PipeAssign)
        return "opOrAssign";
    if (op == CaretAssign)
        return "opXorAssign";
    if (op == LtLtAssign)
        return "opShlAssign";
    if (op == GtGtAssign)
        return "opShrAssign";
    if (op == GtGtGtAssign)
        return "opUShrAssign";
    return {};
}

/** @brief Whether `typeName` declares the operator method `methodName` at all. */
bool DeclaresOperatorMethod(const std::string& typeName, std::string_view methodName, const SymbolTable& table)
{
    if (typeName.empty() || methodName.empty())
    {
        return false;
    }
    return table.FindMemberSymbolPtr(typeName, methodName) != nullptr;
}

/**
 * @brief True for a type no `cast<>` may name: a primitive, or an enum.
 *
 * Measured against the compiler, which rejects every one of them the same way -
 * `cast<int>(obj)`, `cast<int>(n)`, `cast<int>(enumValue)` and `cast<E>(n)` are all
 * "Illegal target type for reference cast", and `cast<A@>(n)` is "No conversion from 'int'
 * to 'A@' available". A reference cast is between reference types, and nothing else.
 */
bool IsScalarCastTarget(const std::string& typeName, const SymbolTable& table)
{
    if (parser::primitives::IsPrimitive(typeName))
    {
        return true;
    }

    const auto symbols = table.FindSymbolsPtr(typeName);
    return symbols && std::any_of(symbols->begin(), symbols->end(),
                                  [](const Symbol& sym) { return sym.type == SymbolType::Enum; });
}

/** @brief True when a type declares any cast operator overload.
 *  @note Deliberately coarse. Matching a cast operator to its result type means resolving
 *        the engine's template-ish opCast, which no declaration in the source states
 *        precisely enough to bet a diagnostic on. */
bool DeclaresAnyCastOperator(const std::string& typeName, const SymbolTable& table)
{
    bool found = false;
    const auto mark = [&found](const Symbol&)
    {
        found = true;
        return true;
    };

    ForEachMethod(typeName, "opCast", table, mark);
    if (!found)
    {
        ForEachMethod(typeName, "opImplCast", table, mark);
    }
    return found;
}

/** @brief True when a type declares a conversion operator producing the target type. */
bool DeclaresConversionTo(const std::string& fromType, const std::string& toType, const SymbolTable& table,
                          bool implicitOnly)
{
    bool found = false;
    TemplateBinding binding;
    if (fromType.find('<') != std::string::npos)
    {
        binding = BindTemplateArguments(fromType, table);
    }
    const auto matches = [&](const Symbol& sym)
    {
        std::string ret = sym.GetFunction().returnType;
        if (binding.usable)
        {
            ret = SubstituteTemplateParameters(ret, binding);
        }
        if (IsSameType(CleanBaseType(ret), toType))
        {
            found = true;
            return true;
        }
        return false;
    };

    static const char* k_implicit[] = {"opImplConv", "opImplCast"};
    static const char* k_explicit[] = {"opConv", "opCast"};

    for (const char* opName : k_implicit)
    {
        ForEachMethod(fromType, opName, table, matches);
        if (found)
        {
            return true;
        }
    }
    if (implicitOnly)
    {
        return false;
    }
    for (const char* opName : k_explicit)
    {
        ForEachMethod(fromType, opName, table, matches);
        if (found)
        {
            return true;
        }
    }
    return false;
}

/** @brief True when derived is derived from base (valid implicit upcast). */
bool IsDerivedFrom(const std::string& derived, const std::string& base, const SymbolTable& table)
{
    const std::string cleanDerived = CleanBaseType(derived);
    const std::string cleanBase = CleanBaseType(base);
    for (const auto& ancestor : GetInheritedTypeHierarchy(cleanDerived, table))
    {
        if (IsSameType(ancestor, cleanBase))
        {
            return true;
        }
    }
    return false;
}

/** @brief True when one type appears in the other's inheritance chain, either direction.
 *  @note Both directions count: an upcast is implicit, and a downcast is what cast<> is
 *        for, so neither is worth a diagnostic here. */
bool AreHierarchyRelated(const std::string& a, const std::string& b, const SymbolTable& table)
{
    const std::string cleanA = CleanBaseType(a);
    const std::string cleanB = CleanBaseType(b);
    for (const auto& base : GetInheritedTypeHierarchy(cleanA, table))
    {
        if (IsSameType(base, cleanB))
        {
            return true;
        }
    }
    for (const auto& base : GetInheritedTypeHierarchy(cleanB, table))
    {
        if (IsSameType(base, cleanA))
        {
            return true;
        }
    }
    return false;
}

/** @brief True for the built-in scalar types plus the configured string type.
 *  @note Conversions among these are the engine's business, not a declaration's, so the
 *        rules below let every primitive-to-primitive pair through. */
/** @brief The workspace's string type, which defaults to `string` when unconfigured. */
bool IsStringType(const std::string& typeName, const DiagnosticContext& ctx)
{
    return typeName == ctx.request.GetEffectiveStringTypeName();
}

bool IsBuiltInValueType(const std::string& typeName, const DiagnosticContext& ctx)
{
    return IsPrimitiveTypeName(typeName) || IsStringType(typeName, ctx);
}

bool IsPoisonOrEmptyType(std::string_view from, std::string_view to)
{
    return from.empty() || to.empty() || ExpressionTypeCache::IsPoisonType(from) ||
           ExpressionTypeCache::IsPoisonType(to);
}

/**
 * @brief Decides whether a value of type 'from' can reach type 'to'.
 * @param depth Recursion guard. At depth > 0 only the direct relations are consulted, so
 *              matching a constructor parameter cannot recurse into another constructor.
 * @return True whenever a route exists - and also whenever this analyzer cannot see enough
 *         to rule one out.
 */
bool CanTriviallyConvert(const std::string& from, const std::string& to, const DiagnosticContext& ctx)
{
    if (IsPoisonOrEmptyType(from, to) || IsSameType(from, to))
    {
        return true;
    }

    if ((from == "null" || CanonicalizeType(from) == "null") && to.ends_with("@"))
    {
        return true;
    }

    // `?` is AngelScript's variable type, not a type name: a parameter declared `const ?&in`
    // or `?&out` takes a value of any type at all. dictionary::set/get, ref, Dispose and the
    // format/scan helpers are all declared that way, so without this the analyzer reported
    // "Cannot implicitly convert 'int' to '?'" on code the real compiler accepts.
    if (IsVariableType(to) || IsVariableType(from))
    {
        return true;
    }

    // `auto` is not a type either - it is a placeholder for whatever the initializer
    // produces, and the deduction happens in the compiler.
    if (from == "auto" || to == "auto")
    {
        return true;
    }

    if (ctx.request.IsRegisteredSymbol(from) || ctx.request.IsRegisteredSymbol(to))
    {
        return true;
    }

    return CanonicalizeType(from) == CanonicalizeType(to);
}

bool CanConvertBuiltins(const std::string& from, const std::string& to, const DiagnosticContext& ctx)
{
    if (from == to)
    {
        return true;
    }
    const bool fromNum = IsNumericPrimitive(from);
    const bool toNum = IsNumericPrimitive(to);
    if (fromNum && toNum)
    {
        return true;
    }
    // `bool` is deliberately absent from both directions. It is not a number and converts to none of them.
    if ((from == "bool" && toNum) || (fromNum && to == "bool"))
    {
        return false;
    }

    // `string` is a sink. The standard string add-on registers an opAssign for every scalar.
    if (IsStringType(to, ctx))
    {
        return true;
    }

    return false;
}

bool CanConvertEnumTarget(const std::string& from, const std::string& to, const SymbolTable& table,
                          const DiagnosticContext& ctx)
{
    const bool fromBuiltIn = IsBuiltInValueType(from, ctx);
    const TypeDeclarationInfo fromDecl = FindTypeDeclaration(from, table);

    // Decidable only when the source is something this analyzer can see. An unresolved
    // name is an engine-registered type, and engine types carry conversions declared
    // nowhere in the source.
    if (!fromBuiltIn && !fromDecl.found && !ResolvesToEnum(from, table))
    {
        return true;
    }
    // A class may declare an operator producing the enum, and the compiler accepts it:
    // `class W { Color opImplConv() const { … } } … Color c = w;` compiles.
    if (!fromBuiltIn && DeclaresConversionTo(from, to, table, true))
    {
        return true;
    }
    return false;
}

struct ConversionState
{
    size_t depth = 0;
    ankerl::unordered_dense::set<std::string> visitedEdges;
};

bool IsConvertible(const std::string& from, const std::string& to, const DiagnosticContext& ctx,
                   ConversionState& state);

bool CanConvertClasses(const std::string& from, const std::string& to, const DiagnosticContext& ctx,
                       ConversionState& state)
{
    const SymbolTable& table = ctx.request.symbolTable;
    if (state.depth >= TypeConversionChecker::MAX_CONVERSION_DEPTH)
    {
        return false;
    }

    const TypeDeclarationInfo toDecl = FindTypeDeclaration(to, table);
    if (toDecl.isTemplate)
    {
        return true;
    }

    TemplateBinding binding;
    if (to.find('<') != std::string::npos)
    {
        binding = BindTemplateArguments(to, table);
    }

    bool convertible = false;
    const auto acceptsFrom = [&](const Symbol& sym)
    {
        if (sym.GetFunction().modifiers.isExplicit || sym.GetFunction().modifiers.isDelete)
        {
            return false;
        }
        const auto& parameters = sym.GetFunction().parameters;
        if (AcceptsSingleArgument(parameters))
        {
            ConversionState nextState{state.depth + 1, state.visitedEdges};
            std::string paramType = SingleArgumentType(parameters);
            if (binding.usable)
            {
                paramType = SubstituteTemplateParameters(paramType, binding);
            }
            if (IsConvertible(from, paramType, ctx, nextState))
            {
                convertible = true;
                return true;
            }
        }
        return false;
    };

    ForEachConstructor(to, table, acceptsFrom);
    if (!convertible)
    {
        ForEachMethod(to, "opAssign", table, acceptsFrom);
    }
    return convertible;
}

/**
 * @brief Checks if a type is engine-registered, unresolved, or external to script analysis.
 * @param[in] typeName Base type name.
 * @param[in] isBuiltIn Whether the type is a known builtin value type.
 * @param[in] table Document and global symbol table.
 * @return True if the type should be assumed convertible without warning; false otherwise.
 */
bool IsUnresolvedOrExternalType(const std::string& typeName, bool isBuiltIn, const SymbolTable& table)
{
    if (!isBuiltIn && !FindTypeDeclaration(typeName, table).found && !ResolvesToEnum(typeName, table))
    {
        return true;
    }
    return ResolvesToNonClassDeclaration(typeName, table);
}

/**
 * @brief Checks user-defined class conversion routes including inheritance and operator methods.
 * @param[in] types Source and target base type names.
 * @param[in] ctx Diagnostic collection context.
 * @param[in,out] state Active conversion depth and cycle tracking state.
 * @return True if a conversion route exists; false otherwise.
 */
bool CanConvertUserTypes(const ConversionTypes& types, const DiagnosticContext& ctx, ConversionState& state)
{
    const bool fromBuiltIn = IsBuiltInValueType(types.from, ctx);
    const bool toBuiltIn = IsBuiltInValueType(types.to, ctx);
    const SymbolTable& table = ctx.request.symbolTable;

    if (!fromBuiltIn && !toBuiltIn && IsDerivedFrom(types.from, types.to, table))
    {
        return true;
    }
    if (!fromBuiltIn && DeclaresConversionTo(types.from, types.to, table, true))
    {
        return true;
    }
    return CanConvertClasses(types.from, types.to, ctx, state);
}

struct EdgeGuard
{
    ankerl::unordered_dense::set<std::string>& edges;
    std::string key;
    ~EdgeGuard()
    {
        edges.erase(key);
    }
};

bool IsConvertible(const std::string& from, const std::string& to, const DiagnosticContext& ctx, ConversionState& state)
{
    if (IsPoisonOrEmptyType(from, to))
    {
        return true;
    }

    if (state.depth >= TypeConversionChecker::MAX_CONVERSION_DEPTH)
    {
        return false;
    }

    if (CanTriviallyConvert(from, to, ctx))
    {
        return true;
    }

    std::string edgeKey = from + "->" + to;
    if (!state.visitedEdges.insert(edgeKey).second)
    {
        return false;
    }

    const SymbolTable& table = ctx.request.symbolTable;

    EdgeGuard guard{state.visitedEdges, std::move(edgeKey)};

    const bool fromBuiltIn = IsBuiltInValueType(from, ctx);
    const bool toBuiltIn = IsBuiltInValueType(to, ctx);
    if (fromBuiltIn && toBuiltIn)
    {
        return CanConvertBuiltins(from, to, ctx);
    }

    // Implicit widening from enum to numeric primitives (int, uint, int64, float, double, etc.)
    if (ResolvesToEnum(from, table) && parser::primitives::IsNumeric(CanonicalizeType(to)))
    {
        return true;
    }

    if (ResolvesToEnum(to, table))
    {
        return CanConvertEnumTarget(from, to, table, ctx);
    }

    if (IsSameType(from, to))
    {
        return true;
    }

    if (IsUnresolvedOrExternalType(from, fromBuiltIn, table) || IsUnresolvedOrExternalType(to, toBuiltIn, table))
    {
        return true;
    }

    if (CanConvertUserTypes({from, to}, ctx, state))
    {
        return true;
    }

    if (AreIncompatibleTemplateTypes(from, to))
    {
        return false;
    }

    return false;
}

bool IsConvertible(const std::string& from, const std::string& to, const DiagnosticContext& ctx, size_t depth = 0)
{
    ConversionState state;
    state.depth = depth;
    return IsConvertible(from, to, ctx, state);
}

/** @brief Classifies a numeric literal as integral or floating point. */
std::string ClassifyNumberLiteral(std::string_view text)
{
    const bool isHex = text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    if (!isHex)
    {
        for (const char c : text)
        {
            if (c == '.' || c == 'e' || c == 'E' || c == 'f' || c == 'F')
            {
                return "float";
            }
        }
    }
    return "int";
}

/** @brief Finds the class a 'this' expression refers to at a given node. */
std::string EnclosingClassName(TSNode node, std::string_view sourceCode)
{
    for (const auto& container : GetEnclosingContainers(node, sourceCode))
    {
        if (container.kind == ContainerKind::Class || container.kind == ContainerKind::Interface)
        {
            return container.name;
        }
    }
    return "";
}

ExpressionType ResolveValueType(TSNode node, const Scope* scope, const DiagnosticContext& ctx, int depth = 0);

/** @brief Resolves the type a bare (possibly scope-qualified) name denotes as a value. */
ExpressionType ResolveIdentifierValueType(TSNode node, const Scope* scope, const DiagnosticContext& ctx)
{
    const std::string name = NodeText(node, ctx.request.sourceCode);
    if (name == "this")
    {
        const std::string className = EnclosingClassName(node, ctx.request.sourceCode);
        return className.empty() ? ExpressionType{} : ExpressionType{className, true, false};
    }

    if (scope)
    {
        if (const LocalDefinition* def = ResolveInScopeAt(scope, name, ts_node_start_byte(node));
            def && !def->typeName.empty())
        {
            return ExpressionType{CleanBaseType(def->typeName), true, false};
        }
    }

    ExpressionType result;
    ForEachSymbolNamed(name, ctx.request.symbolTable,
                       [&result](const Symbol& sym)
                       {
                           // A bare function name is a function pointer, and a bare type name is not a value
                           // at all. Neither has a value type worth judging, so both stay unknown.
                           if (sym.type != SymbolType::Variable && sym.type != SymbolType::Property)
                           {
                               return true;
                           }
                           if (!sym.GetVariable().typeName.empty())
                           {
                               result = ExpressionType{CleanBaseType(sym.GetVariable().typeName), true, false};
                               return true;
                           }
                           return false;
                       });
    return result;
}

/** @brief Resolves what a call expression evaluates to: a constructed type, or a return type. */
ExpressionType ResolveCallValueType(TSNode node, const DiagnosticContext& ctx)
{
    TSNode callee = parser::GetCallCallee(node);
    if (ts_node_is_null(callee))
    {
        return ExpressionType{};
    }

    const std::string calleeName = CleanBaseType(NodeText(callee, ctx.request.sourceCode));
    if (calleeName.empty())
    {
        return ExpressionType{};
    }

    // Type(args) constructs a value of that type; anything else is an ordinary call.
    if (FindTypeDeclaration(calleeName, ctx.request.symbolTable).found)
    {
        return ExpressionType{calleeName, true, false};
    }
    if (IsBuiltInValueType(calleeName, ctx))
    {
        return ExpressionType{calleeName, true, false};
    }

    ExpressionType result;
    ForEachSymbolNamed(LastScopeSegment(calleeName), ctx.request.symbolTable,
                       [&result](const Symbol& sym)
                       {
                           if (sym.type == SymbolType::Function && !sym.GetFunction().returnType.empty())
                           {
                               result = ExpressionType{CleanBaseType(sym.GetFunction().returnType), true, false};
                               return true;
                           }
                           return false;
                       });
    return result;
}

ExpressionType ResolveLiteralValueType(TSNode node, const DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symNumberLiteral)
    {
        return ExpressionType{ClassifyNumberLiteral(NodeText(node, ctx.request.sourceCode)), true, true};
    }
    if (sym == gs.symStringLiteral)
    {
        return ExpressionType{std::string(ctx.request.GetEffectiveStringTypeName()), true, true};
    }
    if (sym == gs.symBooleanLiteral)
    {
        return ExpressionType{"bool", true, true};
    }
    return ExpressionType{};
}

static ExpressionType ResolveUnaryValueType(TSNode node, const Scope* scope, const DiagnosticContext& ctx, int depth)
{
    const std::string op =
        NodeText(parser::GetChildByField(node, parser::GetGrammarSymbols().fieldOperator), ctx.request.sourceCode);
    if (op == "!" || op == "not")
    {
        return ExpressionType{"bool", true, false};
    }
    auto val = ResolveValueType(parser::GetChildByField(node, parser::GetGrammarSymbols().fieldOperand), scope, ctx,
                                depth + 1);
    if (op == "-")
    {
        if (val.baseName == "uint" || val.baseName == "uint32")
        {
            val.baseName = "int";
        }
        else if (val.baseName == "uint8")
        {
            val.baseName = "int8";
        }
        else if (val.baseName == "uint16")
        {
            val.baseName = "int16";
        }
        else if (val.baseName == "uint64")
        {
            val.baseName = "int64";
        }
    }
    return val;
}

static ExpressionType ResolveCastValueType(TSNode node, const DiagnosticContext& ctx)
{
    const std::string typeText = CleanBaseType(
        NodeText(parser::GetChildByField(node, parser::GetGrammarSymbols().fieldType), ctx.request.sourceCode));
    return typeText.empty() ? ExpressionType{} : ExpressionType{typeText, true, false};
}

ExpressionType ResolveCompoundValueType(TSNode node, const Scope* scope, const DiagnosticContext& ctx, int depth)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symParenthesizedExpression)
    {
        return ts_node_named_child_count(node) > 0
                   ? ResolveValueType(ts_node_named_child(node, 0), scope, ctx, depth + 1)
                   : ExpressionType{};
    }

    if (sym == gs.symUnaryExpression)
    {
        return ResolveUnaryValueType(node, scope, ctx, depth);
    }

    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
    {
        return ResolveIdentifierValueType(node, scope, ctx);
    }

    if (sym == gs.symCallExpression || sym == gs.symConstructCallExpression)
    {
        return ResolveCallValueType(node, ctx);
    }

    if (sym == gs.symCastExpression || sym == gs.symFunctionalCastExpression)
    {
        return ResolveCastValueType(node, ctx);
    }

    if (sym == gs.symMemberExpression)
    {
        const std::string resolved = ResolveExpressionType(node, ExpressionTypeContext(scope, ctx));
        return resolved.empty() ? ExpressionType{} : ExpressionType{CleanBaseType(resolved), true, false};
    }

    return ExpressionType{};
}

ExpressionType ResolveValueType(TSNode node, const Scope* scope, const DiagnosticContext& ctx, int depth)
{
    // See k_maxAstDepth in ASTUtils.h. Returning the empty type is this file's established
    // "cannot see enough to judge" answer, which every caller already treats as silence.
    if (depth > k_maxAstDepth || ts_node_is_null(node))
    {
        return ExpressionType{};
    }
    if (ts_node_is_error(node) || ts_node_is_missing(node) ||
        (ctx.request.nodeIndex && ctx.request.nodeIndex->NodeHasSyntaxError(node)))
    {
        return ExpressionType{std::string(ExpressionTypeCache::k_poisonType), false, false};
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symNumberLiteral || sym == gs.symStringLiteral || sym == gs.symBooleanLiteral)
    {
        return ResolveLiteralValueType(node, ctx);
    }
    if (sym == gs.symNullLiteral)
    {
        // 'null' has its own rule (CheckNullAssignedToNonHandle) and no type of its own.
        return ExpressionType{};
    }

    return ResolveCompoundValueType(node, scope, ctx, depth);
}

struct PropertyAccessInfo
{
    bool isProperty = false;
    bool hasGet = false;
    bool hasSet = false;
    bool isIndexed = false;
    std::string propName;
    std::string receiverType;
};

bool CheckVirtualPropertySymbol(const std::string& typeName, const std::string& propName, const SymbolTable& table,
                                PropertyAccessInfo& info)
{
    if (auto propSyms = table.FindMemberSymbolPtr(typeName, propName))
    {
        for (const auto& s : *propSyms)
        {
            if (s.type == SymbolType::Property && std::holds_alternative<VariableSignature>(s.signature))
            {
                const auto& vs = s.GetVariable();
                if (vs.isVirtualProperty)
                {
                    info.isProperty = true;
                    if (vs.hasGet)
                    {
                        info.hasGet = true;
                    }
                    if (vs.hasSet)
                    {
                        info.hasSet = true;
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

void CheckAccessorMethods(const std::string& typeName, const std::string& propName, const SymbolTable& table,
                          PropertyAccessInfo& info)
{
    if (!info.hasGet)
    {
        if (auto getSyms = table.FindMemberSymbolPtr(typeName, "get_" + propName); getSyms && !getSyms->empty())
        {
            info.isProperty = true;
            info.hasGet = true;
            for (const auto& gs : *getSyms)
            {
                if (gs.type == SymbolType::Function && !gs.GetFunction().parameters.empty())
                {
                    info.isIndexed = true;
                }
            }
        }
    }
    if (!info.hasSet)
    {
        if (auto setSyms = table.FindMemberSymbolPtr(typeName, "set_" + propName); setSyms && !setSyms->empty())
        {
            info.isProperty = true;
            info.hasSet = true;
            for (const auto& ss : *setSyms)
            {
                if (ss.type == SymbolType::Function && ss.GetFunction().parameters.size() > 1)
                {
                    info.isIndexed = true;
                }
            }
        }
    }
}

static void PopulateAccessorInfo(const std::vector<Symbol>& accessors, PropertyAccessInfo& info)
{
    for (const auto& sym : accessors)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            const auto& fn = sym.GetFunction();
            info.isProperty = true;
            if (sym.name.starts_with("get_"))
            {
                info.hasGet = true;
                if (!fn.parameters.empty())
                {
                    info.isIndexed = true;
                }
            }
            else if (sym.name.starts_with("set_"))
            {
                info.hasSet = true;
                if (fn.parameters.size() > 1)
                {
                    info.isIndexed = true;
                }
            }
        }
    }
}

static PropertyAccessInfo InspectBareOrScopedProperty(TSNode exprNode, const Scope* scope, const DiagnosticContext& ctx)
{
    PropertyAccessInfo info;
    std::string text = TrimString(NodeText(exprNode, ctx.request.sourceCode));
    const auto [scopeNameView, propNameView] = utils::SplitScopedName(text);
    if (!scopeNameView.empty())
    {
        std::string scopeName = TrimString(std::string(scopeNameView));
        info.propName = TrimString(std::string(propNameView));
        auto accessors = FindPropertyAccessors(scopeName, info.propName, ctx.request.symbolTable, false);
        PopulateAccessorInfo(accessors, info);
        return info;
    }

    if (scope != nullptr)
    {
        if (const auto* def = ResolveInScopeAt(scope, text, ts_node_start_byte(exprNode)))
        {
            // The identifier is a local variable or parameter; it shadows any outer property.
            return info;
        }
    }

    info.propName = text;
    auto accessors = FindGlobalPropertyAccessors(info.propName, ctx.request.symbolTable, false);
    PopulateAccessorInfo(accessors, info);
    if (info.isProperty)
    {
        return info;
    }

    for (const auto& cont : GetEnclosingContainers(exprNode, ctx.request.sourceCode))
    {
        const std::string cName = cont.qualifiedName.empty() ? cont.name : cont.qualifiedName;
        auto contAcc = FindPropertyAccessors(cName, info.propName, ctx.request.symbolTable, false);
        PopulateAccessorInfo(contAcc, info);
        if (info.isProperty)
        {
            return info;
        }
    }
    return info;
}

PropertyAccessInfo InspectPropertyAccess(TSNode exprNode, const Scope* scope, const DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(exprNode);
    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
    {
        return InspectBareOrScopedProperty(exprNode, scope, ctx);
    }

    PropertyAccessInfo info;
    if (sym != gs.symMemberExpression)
    {
        return info;
    }

    TSNode objNode = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldObject);
    TSNode memNode = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objNode) && ts_node_named_child_count(exprNode) > 0)
    {
        objNode = ts_node_named_child(exprNode, 0);
        if (ts_node_named_child_count(exprNode) > 1)
        {
            memNode = ts_node_named_child(exprNode, 1);
        }
    }
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return info;
    }

    info.propName = NodeText(memNode, ctx.request.sourceCode);
    info.receiverType = ResolveExpressionType(objNode, ExpressionTypeContext(scope, ctx));
    const std::string cleanObj = CleanBaseType(info.receiverType);
    if (cleanObj.empty())
    {
        return info;
    }

    const auto hierarchy = GetInheritedTypeHierarchy(cleanObj, ctx.request.symbolTable);
    for (const auto& typeName : hierarchy)
    {
        CheckVirtualPropertySymbol(typeName, info.propName, ctx.request.symbolTable, info);
        CheckAccessorMethods(typeName, info.propName, ctx.request.symbolTable, info);
        if (info.isProperty && info.hasGet && info.hasSet)
        {
            return info;
        }
    }
    return info;
}

/**
 * @brief Checks if an expression resolves to a bare function or method symbol.
 * @param[in] expr Expression syntax node.
 * @param[in] scope Enclosing lexical scope.
 * @param[in] ctx Diagnostic collection context.
 * @return True if expr denotes a bare function or method.
 */
static bool IsFunctionReference(TSNode expr, const Scope* scope, const DiagnosticContext& ctx)
{
    return IsFunctionReference(expr, scope, ctx.request.symbolTable, ctx.request.sourceCode);
}

struct DiagnosticArgs
{
    std::string first{};
    std::string second{};

    DiagnosticArgs() = default;
    DiagnosticArgs(std::string_view a1) : first(a1)
    {
    }
    DiagnosticArgs(std::string_view a1, std::string_view a2) : first(a1), second(a2)
    {
    }
    DiagnosticArgs(std::string a1) : first(std::move(a1))
    {
    }
    DiagnosticArgs(std::string a1, std::string a2) : first(std::move(a1)), second(std::move(a2))
    {
    }
    DiagnosticArgs(const char* a1) : first(a1)
    {
    }
    DiagnosticArgs(const char* a1, const char* a2) : first(a1), second(a2)
    {
    }
};

/** @brief Emits at the exact source range of a node. */
void EmitAtNode(TSNode node, DiagnosticContext& ctx, std::string_view code, const DiagnosticArgs& args = {})
{
    if (code == diagnostics::codes::ExpressionIsDataType)
    {
        if (ctx.request.nodeIndex && ctx.request.nodeIndex->NodeHasSyntaxError(node))
        {
            return;
        }
        const auto& gs = parser::GetGrammarSymbols();
        bool skip = false;
        parser::ForEachAncestor(node,
                                [&](TSNode walk) -> bool
                                {
                                    if (ts_node_has_error(walk) || ts_node_is_error(walk))
                                    {
                                        skip = true;
                                        return false;
                                    }
                                    TSSymbol sym = ts_node_symbol(walk);
                                    if (sym == gs.symIfStatement || sym == gs.symWhileStatement ||
                                        sym == gs.symForStatement)
                                    {
                                        skip = true;
                                        return false;
                                    }
                                    return true;
                                });
        if (skip)
        {
            return;
        }
    }
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    ctx.EmitAtRange({start.row, start.column, end.row, end.column}, code, {args.first, args.second},
                    DiagnosticSeverity::Error);
}

void EmitWarningAtNode(TSNode node, DiagnosticContext& ctx, std::string_view code, const DiagnosticArgs& args = {})
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    ctx.EmitAtRange({start.row, start.column, end.row, end.column}, code, {args.first, args.second},
                    DiagnosticSeverity::Warning);
}

// --- Numeric conversion warnings (TYPE-03) -------------------------------------------
//
// The compiler emits five numeric warnings. Two of them - these - it decides from the
// operand types alone. The other three ("Implicit conversion changed sign of value",
// "Value is too large for data type", "Implicit conversion of value is not exact") fire
// only on constant expressions, so answering them needs a constant folder this analyzer
// does not have. They are left unimplemented rather than approximated: a warning that is
// right about the shape and wrong about the value is worse than no warning.
//
// Everything below was measured against angelscript_oracle, not read off documentation,
// and the measurements corrected the backlog on three points:
//
//   * Signed/Unsigned mismatch fires ONLY on the six comparison operators. `i * u`,
//     `i & u` and every other arithmetic or bitwise pairing is silent, and so are
//     assignment, argument passing and return. The backlog implied it followed the
//     conversion, which it does not.
//   * Width is irrelevant - every signed integer paired with every unsigned one warns -
//     and float and double count as SIGNED: `float < uint` warns, `float < int` does not.
//   * A compile-time constant on either side folds the comparison away and it is silent.
//     `const int i = 1; i < u` is clean, a bare enum member is clean, and `i < 5` is
//     clean. Only a constant whose value is itself out of range warns, and then under one
//     of the three codes above.
//
// The last point is the whole false-positive risk, so the rule stays silent whenever
// either operand is constant. That costs `u < -5`, which the compiler does warn about;
// missing beats inventing, and the alternative is the folder again.
//
// Neither warning reuses IsPrimitiveWidening from OverloadResolver, and that is
// deliberate: it lists signed/unsigned pairs as SAFE on purpose, because removing them
// produced real false positives on `array<int> a(1)`. The compiler warns on exactly the
// pairs that table calls safe, so the two questions need two tables.

bool IsUnsignedIntegerPrimitive(std::string_view typeName) noexcept
{
    return typeName == "uint" || typeName == "uint8" || typeName == "uint16" || typeName == "uint32" ||
           typeName == "uint64";
}

/** @brief Signed for the purpose of the mismatch warning, which counts float and double. */
bool IsSignedNumericPrimitive(std::string_view typeName) noexcept
{
    return typeName == "int" || typeName == "int8" || typeName == "int16" || typeName == "int32" ||
           typeName == "int64" || typeName == "float" || typeName == "double";
}

bool IsComparisonOperator(std::string_view op) noexcept
{
    return op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!=";
}

/** @brief True for a written-out number, through any parentheses and unary sign. */
bool IsNumericLiteralExpression(TSNode node, int depth = 0)
{
    if (ts_node_is_null(node) || depth > k_maxAstDepth)
    {
        return false;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symNumberLiteral || sym == gs.symStringLiteral || sym == gs.symBooleanLiteral ||
        sym == gs.symNullLiteral)
    {
        return true;
    }
    if (sym != gs.symParenthesizedExpression && sym != gs.symUnaryExpression)
    {
        return false;
    }
    for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i)
    {
        if (IsNumericLiteralExpression(ts_node_named_child(node, i), depth + 1))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief True when the operand is something the compiler folds before it compares.
 *
 * Constants are what lets the mismatch rule stay silent instead of guessing: the compiler
 * knows the value, so it compares values rather than types and no mismatch arises. A name
 * counts as constant here when its declaration says `const` - the one spelling that
 * survives into both LocalDefinition::typeName and VariableSignature::typeName - or when
 * it is a literal.
 *
 * An enum member needs no case of its own. ResolveExpressionType answers a bare `A` with
 * its enum's name rather than `int`, so it never reaches the numeric test at all.
 *
 * An unrecognised shape answers false, which is the emitting side. That is what the
 * corpus audit in TypeConversionTest.cpp exists to hold honest.
 */
bool IsFoldedConstantOperand(TSNode node, const Scope* scope, const DiagnosticContext& ctx, int depth = 0)
{
    if (ts_node_is_null(node) || depth > k_maxAstDepth)
    {
        return false;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (IsNumericLiteralExpression(node))
    {
        return true;
    }

    if (sym == gs.symParenthesizedExpression)
    {
        for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i)
        {
            if (IsFoldedConstantOperand(ts_node_named_child(node, i), scope, ctx, depth + 1))
            {
                return true;
            }
        }
        return false;
    }

    if (sym != gs.symIdentifier && sym != gs.symScopedIdentifier)
    {
        return false;
    }

    const std::string name = NodeText(node, ctx.request.sourceCode);
    if (scope)
    {
        if (const LocalDefinition* def = ResolveInScopeAt(scope, LastScopeSegment(name), ts_node_start_byte(node)))
        {
            return def->typeName.starts_with("const ");
        }
    }

    bool folded = false;
    ForEachSymbolNamed(name, ctx.request.symbolTable,
                       [&folded](const Symbol& sym)
                       {
                           if (sym.type != SymbolType::Variable && sym.type != SymbolType::Property)
                           {
                               return false;
                           }
                           folded = sym.GetVariable().typeName.starts_with("const ");
                           return true;
                       });
    return folded;
}

/**
 * @brief `as-warn-signed-unsigned-mismatch`, anchored on the operator the way the compiler
 *        anchors it.
 */
void CheckSignedUnsignedComparison(TSNode node, const Scope* scope, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode opNode = parser::GetChildByField(node, gs.fieldOperator);
    if (ts_node_is_null(opNode) || !IsComparisonOpSym(ts_node_symbol(opNode), gs))
    {
        return;
    }

    TSNode left = parser::GetChildByField(node, gs.fieldLeft);
    TSNode right = parser::GetChildByField(node, gs.fieldRight);
    if (ts_node_is_null(left) || ts_node_is_null(right))
    {
        return;
    }

    const std::string leftType = CleanBaseType(ResolveExpressionType(left, ExpressionTypeContext(scope, ctx)));
    const std::string rightType = CleanBaseType(ResolveExpressionType(right, ExpressionTypeContext(scope, ctx)));

    const bool mismatched = (IsUnsignedIntegerPrimitive(leftType) && IsSignedNumericPrimitive(rightType)) ||
                            (IsUnsignedIntegerPrimitive(rightType) && IsSignedNumericPrimitive(leftType));
    if (!mismatched)
    {
        return;
    }

    if (IsFoldedConstantOperand(left, scope, ctx) || IsFoldedConstantOperand(right, scope, ctx))
    {
        return;
    }

    EmitWarningAtNode(opNode, ctx, "as-warn-signed-unsigned-mismatch", {leftType, rightType});
}

/**
 * @brief `as-warn-float-truncation` where a float value implicitly becomes an integer.
 */
void CheckFloatTruncation(TSNode valueNode, const ConversionTypes& types, const Scope* scope, DiagnosticContext& ctx)
{
    if (ts_node_is_null(valueNode))
    {
        return;
    }
    if (!IsFloatingPointPrimitive(types.from) || !IsIntegerPrimitive(types.to))
    {
        return;
    }
    if (IsFoldedConstantOperand(valueNode, scope, ctx))
    {
        return;
    }
    EmitWarningAtNode(valueNode, ctx, "as-warn-float-truncation", {types.from, types.to});
}

/** @brief Everything one declared type text says that the rules below need to know. */
struct DeclaredType
{
    std::string baseName;
    bool isHandle = false;
    bool usable = false; ///< False when the shape is out of scope (array, template, unknown).

    /**
     * @brief True for `array<T>` and `T[]`, where baseName is the *element* type.
     *
     * CleanBaseType answers the element type, which is what the initializer comparisons
     * below want - `array<int> a = other;` compares element to element. A construction
     * argument is a different question: `array<PlayerSlide> g(33);` passes 33 to the
     * container's initial-size constructor, and comparing it against `PlayerSlide` asked
     * whether an int can become a PlayerSlide. It cannot, so three corpus declarations of
     * exactly this shape were reported.
     */
    bool isTemplateOrArray = false;
};

bool IsDeclaredTypeUsable(const std::string& baseName, const DiagnosticContext& ctx)
{
    if (IsBuiltInValueType(baseName, ctx) || ResolvesToEnum(baseName, ctx.request.symbolTable))
    {
        return true;
    }

    const TypeDeclarationInfo declaration = FindTypeDeclaration(baseName, ctx.request.symbolTable);
    if (!declaration.found || !declaration.isClass || declaration.isTemplate)
    {
        return false;
    }
    return !ResolvesToNonClassDeclaration(baseName, ctx.request.symbolTable);
}

/** @brief Reads a 'type' node into the shape the conversion rules can act on.
 *  @note Arrays, templates and anything that does not resolve to a plain class are marked
 *        unusable: their conversion rules depend on element types this pass does not track. */
DeclaredType ReadDeclaredType(TSNode typeNode, const DiagnosticContext& ctx)
{
    DeclaredType result;
    if (ts_node_is_null(typeNode))
    {
        return result;
    }

    const std::string raw = NodeText(typeNode, ctx.request.sourceCode);
    if (raw.empty())
    {
        return result;
    }

    result.isHandle = raw.find('@') != std::string::npos;
    result.baseName = CleanBaseType(raw);
    if (result.baseName.empty() || ctx.request.IsRegisteredSymbol(result.baseName))
    {
        return result;
    }

    if (raw.find('<') != std::string::npos || raw.find('[') != std::string::npos)
    {
        result.usable = true;
        result.isTemplateOrArray = true;
        return result;
    }

    result.usable = IsDeclaredTypeUsable(result.baseName, ctx);
    return result;
}

void CheckUnknownInitializerSource(TSNode valueNode, const DeclaredType& declared, const Scope* scope,
                                   DiagnosticContext& ctx)
{
    const std::string identName = TrimString(NodeText(valueNode, ctx.request.sourceCode));
    if (identName.empty())
    {
        return;
    }

    bool isTypeOrTemplate = false;
    bool isNamespace = false;
    ForEachSymbolNamed(identName, ctx.request.symbolTable,
                       [&](const Symbol& s) -> bool
                       {
                           if (s.type == SymbolType::Class || s.type == SymbolType::Interface ||
                               s.type == SymbolType::Typedef || s.type == SymbolType::Enum)
                           {
                               isTypeOrTemplate = true;
                               return false;
                           }
                           if (s.type == SymbolType::Namespace)
                           {
                               isNamespace = true;
                               return false;
                           }
                           return true;
                       });
    if (isTypeOrTemplate)
    {
        EmitAtNode(valueNode, ctx, diagnostics::codes::ExpressionIsDataType, identName);
        return;
    }
    if (isNamespace)
    {
        EmitAtNode(valueNode, ctx, "as-err-undefined-identifier", identName);
        return;
    }

    if (!FindFuncdef(declared.baseName, ctx.request.symbolTable) && IsFunctionReference(valueNode, scope, ctx))
    {
        EmitAtNode(valueNode, ctx, "as-err-no-implicit-conversion", {identName, declared.baseName});
    }
}

void CheckHandleInitializer(TSNode valueNode, const ExpressionType& source, const DeclaredType& declared,
                            DiagnosticContext& ctx)
{
    if (source.baseName == "null")
    {
        return;
    }
    if (source.isLiteral)
    {
        EmitAtNode(valueNode, ctx, "as-err-no-implicit-conversion", {source.baseName, declared.baseName});
        return;
    }

    if (IsSameType(source.baseName, declared.baseName))
    {
        return;
    }

    // If declared is a derived class of source, that's an invalid downcast without cast<T>
    if (ctx.request.symbolTable.HasSymbolAnywhere(source.baseName) &&
        ctx.request.symbolTable.HasSymbolAnywhere(declared.baseName))
    {
        const auto hierarchy = GetInheritedTypeHierarchy(declared.baseName, ctx.request.symbolTable);
        if (std::find(hierarchy.begin(), hierarchy.end(), source.baseName) != hierarchy.end())
        {
            EmitAtNode(valueNode, ctx, "as-err-no-implicit-conversion",
                       {source.baseName + "@", declared.baseName + "@"});
        }
    }
}

/** @brief Rule for `T v = expr;` - the implicit conversion route. */
void CheckInitializer(TSNode declaratorNode, const DeclaredType& declared, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode valueNode = parser::GetChildByField(declaratorNode, parser::GetGrammarSymbols().fieldValue);
    if (ts_node_is_null(valueNode) || ts_node_symbol(valueNode) == parser::GetGrammarSymbols().symInitializerList)
    {
        return;
    }

    if (const auto dataTypeName = IsBareDataType(valueNode, scope, ctx.request.symbolTable, ctx.request.sourceCode))
    {
        EmitAtNode(valueNode, ctx, diagnostics::codes::ExpressionIsDataType, *dataTypeName);
        return;
    }

    const ExpressionType source = ResolveValueType(valueNode, scope, ctx);
    if (ExpressionTypeCache::IsPoisonType(source.baseName))
    {
        return;
    }
    if (!source.known || source.baseName.empty())
    {
        CheckUnknownInitializerSource(valueNode, declared, scope, ctx);
        return;
    }

    if (!declared.isHandle)
    {
        CheckFloatTruncation(valueNode, {source.baseName, declared.baseName}, scope, ctx);
    }
    else
    {
        CheckHandleInitializer(valueNode, source, declared, ctx);
        return;
    }

    if (!IsConvertible(source.baseName, declared.baseName, ctx))
    {
        EmitAtNode(valueNode, ctx, "as-err-no-implicit-conversion", {source.baseName, declared.baseName});
    }
}

/**
 * @brief The type `foreach`'s Nth loop variable takes, from the container's own declaration.
 *
 * AngelScript drives `foreach` through `opForBegin` / `opForEnd` / `opForNext` and one
 * `opForValue<N>` per loop variable, so the Nth variable's type is that method's return
 * type. Reading it back out of the stub is the general rule and needs no per-type knowledge:
 * `array<T>` declares `const T& opForValue0(uint)` and `uint opForValue1(uint)`, and
 * `dictionary` declares `const dictionaryValue& opForValue0(...)` and
 * `const string& opForValue1(...)`, which is exactly what the compiler hands the loop.
 *
 * Returns empty when the container declares no such method, which leaves the variable's
 * written `auto` in place rather than guessing at it.
 */
std::string ForeachValueType(const std::string& containerType, uint32_t variableIndex, const DiagnosticContext& ctx)
{
    const std::string cleaned = CleanExpressionType(containerType);
    if (cleaned.empty())
    {
        return {};
    }

    const size_t open = cleaned.find('<');
    const std::string bare{LastScopeSegment((open == std::string::npos) ? cleaned : cleaned.substr(0, open))};

    const TemplateBinding binding = BindTemplateArguments(cleaned, ctx.request.symbolTable);
    const std::string method = "opForValue" + std::to_string(variableIndex);

    for (const std::string& candidate : GetInheritedTypeHierarchy(bare, ctx.request.symbolTable))
    {
        const auto overloads = ctx.request.symbolTable.FindMemberSymbolPtr(candidate, method);
        if (!overloads)
        {
            continue;
        }
        for (const Symbol& overload : *overloads)
        {
            if (overload.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(overload.signature))
            {
                continue;
            }

            std::string returnType = std::get<FunctionSignature>(overload.signature).returnType;
            if (returnType.empty() || returnType == "void")
            {
                continue;
            }
            if (binding.usable)
            {
                for (size_t i = 0; i < binding.parameters.size(); ++i)
                {
                    returnType = SubstituteTypeParam(returnType, binding.parameters[i], binding.arguments[i]);
                }
            }
            // `const T&` names the same type as `T` for anything the scope tree does with
            // it, and the reference is not part of the variable's identity.
            return CleanExpressionType(returnType);
        }
    }

    return {};
}

bool CheckEnumConstruction(TSNode argument, const std::string& sourceBase, const std::string& targetType,
                           DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    if (!ResolvesToEnum(targetType, table))
    {
        return false;
    }
    if (!IsNumericPrimitive(sourceBase) && !ResolvesToEnum(sourceBase, table))
    {
        EmitAtNode(argument, ctx, "as-err-no-explicit-conversion", {sourceBase, targetType});
    }
    return true;
}

bool IsConstructibleFrom(const std::string& sourceBase, const std::string& targetType, const TemplateBinding& binding,
                         const DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    bool constructible = false;
    ForEachConstructor(targetType, table,
                       [&](const Symbol& sym)
                       {
                           const auto& parameters = sym.GetFunction().parameters;
                           if (!AcceptsSingleArgument(parameters))
                           {
                               return false;
                           }

                           std::string parameterType = SingleArgumentType(parameters);
                           for (size_t i = 0; i < binding.parameters.size(); ++i)
                           {
                               parameterType =
                                   SubstituteTypeParam(parameterType, binding.parameters[i], binding.arguments[i]);
                           }

                           if (IsConvertible(sourceBase, parameterType, ctx, 1))
                           {
                               constructible = true;
                               return true;
                           }
                           return false;
                       });
    return constructible;
}

/** @brief Rule for a one-argument construction: `T(expr)` or `T v(expr);`. */
static bool IsTargetTypeIgnoredForConstruction(const std::string& targetType, const SymbolTable& table,
                                               const DiagnosticContext& ctx)
{
    if (IsBuiltInValueType(targetType, ctx))
    {
        return true;
    }
    return !FindTypeDeclaration(targetType, table).found && !ResolvesToEnum(targetType, table);
}

static bool IsSourceCompatibleWithTarget(const std::string& sourceBaseName, const std::string& targetType,
                                         const SymbolTable& table)
{
    return AreHierarchyRelated(sourceBaseName, targetType, table) ||
           DeclaresConversionTo(sourceBaseName, targetType, table, false);
}

void CheckConstruction(TSNode argumentListNode, const std::string& targetType, const Scope* scope,
                       DiagnosticContext& ctx)
{
    // Only single-argument constructions are conversions. Anything else is overload
    // resolution over a full argument list, which this pass does not attempt.
    if (ts_node_is_null(argumentListNode) || ts_node_named_child_count(argumentListNode) != 1)
    {
        return;
    }

    TSNode argument = ts_node_named_child(argumentListNode, 0);
    const ExpressionType source = ResolveValueType(argument, scope, ctx);
    if (!source.known || source.baseName.empty() || IsSameType(source.baseName, targetType))
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    if (IsTargetTypeIgnoredForConstruction(targetType, table, ctx) ||
        IsSourceCompatibleWithTarget(source.baseName, targetType, table))
    {
        return;
    }

    const TemplateBinding binding = BindTemplateArguments(targetType, table);
    if (binding.isTemplate && !binding.usable)
    {
        return;
    }

    if (CheckEnumConstruction(argument, source.baseName, targetType, ctx))
    {
        return;
    }

    if (!IsConstructibleFrom(source.baseName, targetType, binding, ctx))
    {
        EmitAtNode(argument, ctx, "as-err-no-explicit-conversion", {source.baseName, targetType});
    }
}

struct ZeroArgConstructorStatus
{
    bool hasZeroArg = false;
    bool isDeleted = false;
};

/**
 * @brief Inspects constructors to determine whether a zero-argument default constructor exists.
 * @param[in] constructors List of candidate constructor symbols.
 * @return ZeroArgConstructorStatus indicating existence and deleted status.
 */
ZeroArgConstructorStatus InspectZeroArgConstructor(const std::vector<Symbol>& constructors)
{
    ZeroArgConstructorStatus status;
    for (const auto& ctor : constructors)
    {
        const auto& sig = ctor.GetFunction();
        bool canTakeZero = sig.parameters.empty();
        if (!canTakeZero)
        {
            canTakeZero = std::all_of(sig.parameters.begin(), sig.parameters.end(),
                                      [](const ParameterInformation& p) { return !p.defaultValue.empty(); });
        }
        if (canTakeZero)
        {
            status.hasZeroArg = true;
            status.isDeleted = sig.modifiers.isDelete;
            break;
        }
    }
    return status;
}

void CheckDefaultConstructor(TSNode declaratorNode, const std::string& typeName, DiagnosticContext& ctx)
{
    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    if (typeName.empty() || IsBuiltInValueType(typeName, ctx))
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const TypeDeclarationInfo decl = FindTypeDeclaration(typeName, table);
    if (!decl.found || !decl.isClass || decl.isTemplate)
    {
        return;
    }

    std::vector<Symbol> constructors;
    ForEachConstructor(typeName, table,
                       [&](const Symbol& sym)
                       {
                           constructors.push_back(sym);
                           return false;
                       });

    if (constructors.empty())
    {
        return;
    }

    const auto [hasZeroArg, isDeleted] = InspectZeroArgConstructor(constructors);

    TSNode nameNode = parser::GetChildByField(declaratorNode, parser::GetGrammarSymbols().fieldName);
    TSNode targetNode = ts_node_is_null(nameNode) ? declaratorNode : nameNode;

    if (isDeleted)
    {
        EmitAtNode(targetNode, ctx, "as-err-deleted-method-called", {typeName, typeName});
    }
    else if (!hasZeroArg && !ctx.request.AlwaysImplementsDefaultConstruct())
    {
        EmitAtNode(targetNode, ctx, "as-err-no-default-constructor", {typeName});
    }
}

std::optional<Symbol> FindFuncdef(const std::string& name, const SymbolTable& table)
{
    std::optional<Symbol> result;
    ForEachSymbolNamed(name, table,
                       [&](const Symbol& sym)
                       {
                           if (sym.type == SymbolType::Funcdef)
                           {
                               result = sym;
                               return true;
                           }
                           return false;
                       });
    if (result)
    {
        return result;
    }
    const std::string_view bare = LastScopeSegment(name);
    const auto matches = table.FindTypeSymbolsByShortName(bare);
    for (const auto& sym : matches)
    {
        if (sym.type == SymbolType::Funcdef)
        {
            return sym;
        }
    }
    return result;
}

bool MatchesFuncdefSignature(const FunctionSignature& fn, const FuncdefSignature& fd)
{
    if (CleanBaseType(fn.returnType) != CleanBaseType(fd.returnType))
    {
        return false;
    }
    if (fn.parameters.size() != fd.parameters.size())
    {
        return false;
    }
    for (size_t i = 0; i < fn.parameters.size(); ++i)
    {
        if (CleanBaseType(fn.parameters[i].typeName) != CleanBaseType(fd.parameters[i].typeName))
        {
            return false;
        }
        if (fn.parameters[i].modifier != fd.parameters[i].modifier)
        {
            return false;
        }
        if (fn.parameters[i].isReference != fd.parameters[i].isReference)
        {
            return false;
        }
        if (fn.parameters[i].isHandle != fd.parameters[i].isHandle)
        {
            return false;
        }
    }
    return true;
}

// --- Lambda against its target funcdef ------------------------------------------------
//
// A lambda takes its parameter types from the funcdef it is assigned to, so it may leave
// them out. What it writes, though, has to match exactly - the compiler compares the
// written signature, it does not convert it. Measured against angelscript_oracle, and the
// exactness is the surprising half:
//
//     funcdef void CB(const string &in);
//     CB@ cb = function(s) { };                    // accepted, type comes from CB
//     CB@ cb = function(const string &in s) { };    // accepted, written and identical
//     CB@ cb = function(string s) { };              // REJECTED - no const, no &in
//     CB@ cb = function(string &in s) { };          // REJECTED - no const
//     funcdef void CB(int);
//     CB@ cb = function(uint a) { };                // REJECTED - int does not widen here
//
// ARITY is a hard equality even when every parameter is untyped, and a funcdef's default
// argument does not relax it: `funcdef void CB(int a = 1)` still rejects `function()`.
// That is the whole of what this rule can check without risking a false positive, and it
// is also the mistake real code actually makes.
//
// The TYPE NAME is another matter, because the compiler resolves it and this rule only
// reads it. All of these are ACCEPTED, and a string comparison would report every one:
//
//     typedef float real;  funcdef void CB(real);   function(float a)   // typedef
//     funcdef void CB(array<int>@);                 function(int[]@ a)  // two spellings
//     namespace N { class Foo{} funcdef void CB(Foo@); }
//                                                   function(N::Foo@ f) // qualification
//
// So the name is compared by its last `::` segment, and not at all when either side names
// a typedef - the one alias no spelling comparison can see through. `array<int>` and
// `int[]` need no case of their own: CleanBaseType reduces both to the element type.
//
// The DECORATIONS - `const`, `&`, the in/out/inout modifier and `@` - are compared
// whatever the type name is, because none of them can be hidden by a spelling: AngelScript
// typedefs alias primitives only (see as-err-typedef-non-primitive), and a namespace
// qualifies a name without changing whether it is a handle.

TSNode UnwrapAddressOfOperator(TSNode valueNode, std::string_view sourceCode)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(valueNode) == gs.symUnaryExpression)
    {
        TSNode op = parser::GetChildByField(valueNode, gs.fieldOperator);
        if (!ts_node_is_null(op) && (ts_node_symbol(op) == gs.tokAt || NodeText(op, sourceCode) == "@"))
        {
            TSNode operand = parser::GetChildByField(valueNode, gs.fieldOperand);
            if (!ts_node_is_null(operand))
            {
                return operand;
            }
        }
    }
    return valueNode;
}

bool CheckLambdaFuncdefAssignment(TSNode targetNode, const FuncdefSignature& funcdefSig, TSNode actualVal,
                                  DiagnosticContext& ctx)
{
    if (ts_node_symbol(actualVal) != parser::GetGrammarSymbols().symLambdaExpression)
    {
        return false;
    }
    TSNode listNode = parser::GetChildByField(actualVal, parser::GetGrammarSymbols().fieldParameters);
    if (!ts_node_is_null(listNode) && LambdaContradictsFuncdef(ReadLambdaParameters(listNode, ctx.request.sourceCode),
                                                               funcdefSig, ctx.request.symbolTable))
    {
        EmitAtNode(targetNode, ctx, "as-err-signature-mismatch-func-handle");
    }
    return true;
}

static bool HasMatchingFuncdefCandidate(const std::vector<Symbol>& candidates, const FuncdefSignature& funcdefSig)
{
    for (const auto& cand : candidates)
    {
        if (MatchesFuncdefSignature(cand.GetFunction(), funcdefSig))
        {
            return true;
        }
    }
    return false;
}

void CheckNamedFunctionFuncdefAssignment(TSNode targetNode, const FuncdefSignature& funcdefSig, TSNode actualVal,
                                         DiagnosticContext& ctx)
{
    const std::string funcName = TrimString(NodeText(actualVal, ctx.request.sourceCode));
    if (funcName.empty() || funcName == "null")
    {
        return;
    }

    const std::vector<Symbol> candidates = CollectFunctionCandidates(funcName, ctx.request.symbolTable);
    if (candidates.empty())
    {
        return;
    }

    if (!HasMatchingFuncdefCandidate(candidates, funcdefSig))
    {
        EmitAtNode(targetNode, ctx, "as-err-signature-mismatch-func-handle");
    }
}

void CheckFuncdefAssignment(TSNode targetNode, const FuncdefSignature& funcdefSig, TSNode valueNode,
                            DiagnosticContext& ctx)
{
    if (ts_node_is_null(valueNode))
    {
        return;
    }

    TSNode actualVal = UnwrapAddressOfOperator(valueNode, ctx.request.sourceCode);
    if (CheckLambdaFuncdefAssignment(targetNode, funcdefSig, actualVal, ctx))
    {
        return;
    }

    CheckNamedFunctionFuncdefAssignment(targetNode, funcdefSig, actualVal, ctx);
}

void CheckConstructorDelegationStatement(TSNode stmt, const std::string& className, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(stmt) != gs.symExpressionStatement || ts_node_named_child_count(stmt) == 0)
    {
        return;
    }

    TSNode expr = ts_node_named_child(stmt, 0);
    const TSSymbol exprSym = ts_node_symbol(expr);
    if (exprSym == gs.symCallExpression || exprSym == gs.symConstructCallExpression)
    {
        TSNode callee = parser::GetCallCallee(expr);
        if (!ts_node_is_null(callee) && NodeText(callee, ctx.request.sourceCode) == className)
        {
            EmitAtNode(expr, ctx, "as-err-constructor-delegation-disallowed");
        }
    }
}

void CheckConstructorDelegation(TSNode funcNode, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode parent = parser::FindAncestorWithSymbol(parser::GetParent(funcNode), gs.symClassDeclaration);
    if (ts_node_is_null(parent))
    {
        return;
    }

    TSNode classNameNode = parser::GetChildByField(parent, parser::GetGrammarSymbols().fieldName);
    if (ts_node_is_null(classNameNode))
    {
        return;
    }
    const std::string className = NodeText(classNameNode, ctx.request.sourceCode);

    TSNode funcNameNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldName);
    if (ts_node_is_null(funcNameNode) || NodeText(funcNameNode, ctx.request.sourceCode) != className)
    {
        return;
    }

    TSNode bodyNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldBody);
    if (ts_node_is_null(bodyNode))
    {
        return;
    }

    const uint32_t stmtCount = ts_node_named_child_count(bodyNode);
    for (uint32_t i = 0; i < stmtCount; ++i)
    {
        CheckConstructorDelegationStatement(ts_node_named_child(bodyNode, i), className, ctx);
    }
}

/**
 * @brief Evaluates visibility guards before reporting invalid cast diagnostics.
 * @param[in] types Source and target base type names.
 * @param[in] sourceIsScalar Whether source type is a scalar cast target.
 * @param[in] targetIsScalar Whether target type is a scalar cast target.
 * @param[in] ctx Diagnostic collection context.
 * @return True if cast should be evaluated further; false if conversion should be silently assumed.
 */
bool CheckCastVisibilityGuards(const ConversionTypes& types, bool sourceIsScalar, bool targetIsScalar,
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

/** @brief Rule for `cast<T>(expr)` - the reinterpreting route. */
void CheckCast(TSNode castNode, const Scope* scope, DiagnosticContext& ctx)
{
    const std::string targetName = CleanBaseType(
        NodeText(parser::GetChildByField(castNode, parser::GetGrammarSymbols().fieldType), ctx.request.sourceCode));

    TSNode valueNode = parser::GetChildByField(castNode, parser::GetGrammarSymbols().fieldValue);
    if (ts_node_is_null(valueNode) || targetName.empty())
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const bool targetIsScalar = IsScalarCastTarget(targetName, table);

    const ExpressionType source = ResolveValueType(valueNode, scope, ctx);
    if (!source.known || source.baseName.empty() || IsSameType(source.baseName, targetName))
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

    EmitAtNode(castNode, ctx, "as-err-invalid-cast", {source.baseName, targetName});
}

bool ClassDeclaresMethodReturningBool(const std::string& cls, const std::string& method, const SymbolTable& table)
{
    if (auto ptr = table.FindMemberSymbolPtr(cls, method))
    {
        for (const auto& sym : *ptr)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                if (CleanBaseType(sym.GetFunction().returnType) == "bool")
                {
                    return true;
                }
            }
        }
    }
    return false;
}

std::string FindVisibleClassQualifiedName(const std::string& typeName, const SymbolTable& table)
{
    if (auto ptr = table.FindSymbolsPtr(typeName))
    {
        for (const auto& sym : *ptr)
        {
            if (sym.type == SymbolType::Class)
            {
                return typeName;
            }
        }
    }

    for (const auto& sym : table.FindTypeSymbolsByShortName(typeName))
    {
        if (sym.type == SymbolType::Class)
        {
            return sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
        }
    }
    return {};
}

const std::string* BoolConversionOperator(const std::string& typeName, const SymbolTable& table)
{
    if (typeName.empty())
    {
        return nullptr;
    }

    static const std::string implicitName = "opImplConv";
    static const std::string explicitName = "opConv";

    const std::string matchedCls = FindVisibleClassQualifiedName(typeName, table);
    if (matchedCls.empty())
    {
        return nullptr;
    }

    if (ClassDeclaresMethodReturningBool(matchedCls, implicitName, table))
    {
        return &implicitName;
    }
    if (ClassDeclaresMethodReturningBool(matchedCls, explicitName, table))
    {
        return &explicitName;
    }
    return nullptr;
}

/**
 * @brief The sub-expressions a condition actually evaluates for truth.
 *
 * `if (h)` is one operand; `if (h && other)` is two, and `if (!h)` is one behind a negation.
 * Collecting them handles compound conditions like `if (h && true)` - resolving the
 * type of the whole condition there answers `bool`, because `&&` yields one, and the class
 * that cannot convert sits underneath.
 *
 * Only the logical operators recurse. `a == b` also yields a bool but its operands are
 * compared, not converted to bool, and the engine's rules for that are a different question.
 */
static bool IsLogicalBinaryOperator(TSSymbol sym, const parser::GrammarSymbols& gs)
{
    return sym == gs.tokAmpAmp || sym == gs.tokAnd || sym == gs.tokPipePipe || sym == gs.tokOr ||
           sym == gs.tokCaretCaret || sym == gs.tokXor;
}

static bool IsLogicalUnaryOperator(TSSymbol sym, const parser::GrammarSymbols& gs)
{
    return sym == gs.tokBang || sym == gs.tokNot;
}

void CollectBooleanOperands(TSNode expr, std::vector<TSNode>& operands, int depth = 0)
{
    if (ts_node_is_null(expr) || depth > k_maxAstDepth)
        return;

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(expr);

    if (sym == gs.symBinaryExpression)
    {
        const TSNode op = parser::GetChildByField(expr, gs.fieldOperator);
        const TSSymbol opSym = ts_node_is_null(op) ? 0 : ts_node_symbol(op);
        if (IsLogicalBinaryOperator(opSym, gs))
        {
            CollectBooleanOperands(parser::GetChildByField(expr, gs.fieldLeft), operands, depth + 1);
            CollectBooleanOperands(parser::GetChildByField(expr, gs.fieldRight), operands, depth + 1);
            return;
        }
    }
    else if (sym == gs.symUnaryExpression)
    {
        const TSNode op = parser::GetChildByField(expr, gs.fieldOperator);
        const TSSymbol opSym = ts_node_is_null(op) ? 0 : ts_node_symbol(op);
        if (IsLogicalUnaryOperator(opSym, gs))
        {
            CollectBooleanOperands(parser::GetChildByField(expr, gs.fieldOperand), operands, depth + 1);
            return;
        }
    }

    operands.push_back(expr);
}

struct AssignmentOperands
{
    TSNode left;
    TSNode right;
};

/**
 * @brief Resolves the lexical scope enclosing a syntax tree node.
 * @param[in] node AST node to resolve scope for.
 * @param[in] request Analysis request carrying the scope root.
 * @return Enclosing scope pointer, or nullptr if none found.
 */
const Scope* ResolveNodeScope(TSNode node, const TypeConversionCheckRequest& request)
{
    const TSPoint start = ts_node_start_point(node);
    return FindInnermostScope(request.scopeRoot, start.row, start.column);
}

/**
 * @brief Extracts the condition node from control-flow or ternary expressions.
 * @param[in] node Condition-bearing expression or statement.
 * @param[in] sym Tree-Sitter grammar symbol of the node.
 * @return Extracted condition node, or a null node if absent.
 */
TSNode ExtractConditionNode(TSNode node, TSSymbol sym)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode condition = parser::GetChildByField(node, gs.fieldCondition);
    if (!ts_node_is_null(condition) && ts_node_symbol(condition) == gs.symExpressionStatement)
    {
        condition = ts_node_named_child(condition, 0);
    }
    if (ts_node_is_null(condition))
    {
        condition = ts_node_named_child(node, sym == gs.symDoWhileStatement ? 1 : 0);
    }
    return condition;
}

/**
 * @brief Emits a diagnostic if an operand cannot satisfy boolean condition requirements.
 * @param[in] operand Condition operand expression node.
 * @param[in] operandType Resolved base type of the operand.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ValidateConditionOperand(TSNode operand, const std::string& operandType, DiagnosticContext& ctx)
{
    const bool isKnown = IsKnownType(operandType, ctx);

    if (isKnown && !IsTruthyCondition(operandType, ctx.request.symbolTable))
    {
        const TSPoint start = ts_node_start_point(operand);
        const TSPoint end = ts_node_end_point(operand);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-condition-not-boolean", operandType);
        return;
    }

    if (!ctx.request.diagnostics || !ctx.request.diagnostics->reportBoolConversion ||
        ctx.request.BoolConversionMode() != 0)
    {
        return;
    }

    if (const std::string* conversion = BoolConversionOperator(operandType, ctx.request.symbolTable))
    {
        const TSPoint start = ts_node_start_point(operand);
        const TSPoint end = ts_node_end_point(operand);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-hint-bool-conversion",
                        {operandType, *conversion}, DiagnosticSeverity::Hint);
    }
}

/**
 * @brief Validates boolean operands in a condition expression.
 * @param[in] condition Condition syntax node.
 * @param[in] scope Lexical scope at the condition node.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckBooleanOperands(TSNode condition, const Scope* scope, DiagnosticContext& ctx)
{
    std::vector<TSNode> operands;
    CollectBooleanOperands(condition, operands);

    for (const TSNode& operand : operands)
    {
        const std::string operandType =
            CleanBaseType(ResolveExpressionType(operand, ExpressionTypeContext(scope, ctx)));

        if (operandType.empty() || operandType == "auto" || operandType == "void")
        {
            continue;
        }

        ValidateConditionOperand(operand, operandType, ctx);
    }
}

/**
 * @brief Checks if a condition expression evaluates a reference type with disallowed bool conversion.
 * @param[in] condNode Condition syntax node.
 * @param[in] scope Lexical scope at the condition node.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckRefTypeBoolConversion(TSNode condNode, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode current = condNode;
    const auto& gs = parser::GetGrammarSymbols();
    while (!ts_node_is_null(current) && ts_node_symbol(current) == gs.symParenthesizedExpression &&
           ts_node_named_child_count(current) > 0)
    {
        current = ts_node_named_child(current, 0);
    }

    if (ts_node_is_null(current))
    {
        return;
    }

    bool isHandle = false;
    if (scope)
    {
        const std::string name = NodeText(current, ctx.request.sourceCode);
        const LocalDefinition* def = ResolveInScopeAt(scope, name, ts_node_start_byte(current));
        if (def)
        {
            isHandle = def->typeName.find('@') != std::string::npos;
        }
    }

    const std::string condType = ResolveExpressionType(current, ExpressionTypeContext(scope, ctx));
    if (!isHandle && condType.find('@') == std::string::npos)
    {
        return;
    }

    const std::string baseClass = CleanBaseType(condType);
    auto opSyms = ctx.request.symbolTable.FindMemberSymbolPtr(baseClass, "opImplConv");
    if (!opSyms)
    {
        return;
    }

    for (const auto& sym : *opSyms)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            if (CleanBaseType(sym.GetFunction().returnType) == "bool")
            {
                EmitAtNode(current, ctx, "as-err-ref-type-bool-conv-disallowed", {baseClass, "bool"});
                break;
            }
        }
    }
}

/**
 * @brief Validates condition semantics for control flow statements.
 * @param[in] node Control flow syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessConditionNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    TSNode condition = ExtractConditionNode(node, sym);
    if (ts_node_is_null(condition))
    {
        return;
    }

    const Scope* scope = ResolveNodeScope(node, request);
    CheckBooleanOperands(condition, scope, ctx);

    if (sym != gs.symTernaryExpression)
    {
        CheckRefTypeBoolConversion(condition, scope, ctx);
    }

    if (sym == gs.symIfStatement)
    {
        TSNode parent = parser::GetParent(node);
        if (ts_node_is_null(parent) || ts_node_symbol(parent) != gs.symIfStatement)
        {
            CheckRepeatedConversions(node, scope, ctx);
        }
    }
}

/**
 * @brief Validates ternary expression branches for type compatibility.
 * @param[in] node Ternary expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
static bool IsIncompleteOrIgnoredBranchType(std::string_view type)
{
    return type.empty() || type == "auto";
}

static bool AreTernaryBranchesIncompatible(const std::string& clean1, const std::string& clean2, DiagnosticContext& ctx)
{
    if ((clean1 == "null" && clean2.ends_with("@")) || (clean2 == "null" && clean1.ends_with("@")))
    {
        return false;
    }
    if (IsStringType(clean1, ctx) != IsStringType(clean2, ctx))
    {
        return true;
    }
    if (ResolvesToEnum(clean1, ctx.request.symbolTable) && ResolvesToEnum(clean2, ctx.request.symbolTable) &&
        clean1 != clean2)
    {
        return true;
    }
    return !IsConvertible(clean1, clean2, ctx) && !IsConvertible(clean2, clean1, ctx);
}

void ProcessTernaryNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode consequence = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldConsequence);
    TSNode alternative = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldAlternative);
    if (ts_node_is_null(consequence) || ts_node_is_null(alternative))
    {
        return;
    }

    const Scope* scope = ResolveNodeScope(node, request);
    if (const auto dt1 = IsBareDataType(consequence, scope, ctx.request.symbolTable, request.sourceCode))
    {
        EmitAtNode(consequence, ctx, diagnostics::codes::ExpressionIsDataType, *dt1);
        return;
    }
    if (const auto dt2 = IsBareDataType(alternative, scope, ctx.request.symbolTable, request.sourceCode))
    {
        EmitAtNode(alternative, ctx, diagnostics::codes::ExpressionIsDataType, *dt2);
        return;
    }

    const std::string t1 = ResolveExpressionType(consequence, ExpressionTypeContext(scope, ctx));
    const std::string t2 = ResolveExpressionType(alternative, ExpressionTypeContext(scope, ctx));

    const std::string clean1 = CanonicalizeType(CleanExpressionType(t1));
    const std::string clean2 = CanonicalizeType(CleanExpressionType(t2));

    if (IsFunctionReference(consequence, scope, ctx))
    {
        EmitAtNode(consequence, ctx, "as-err-no-implicit-conversion",
                   {TrimString(NodeText(consequence, request.sourceCode)), clean2});
        return;
    }
    if (IsFunctionReference(alternative, scope, ctx))
    {
        EmitAtNode(alternative, ctx, "as-err-no-implicit-conversion",
                   {TrimString(NodeText(alternative, request.sourceCode)), clean1});
        return;
    }

    if (IsIncompleteOrIgnoredBranchType(clean1) || IsIncompleteOrIgnoredBranchType(clean2))
    {
        return;
    }

    if (AreTernaryBranchesIncompatible(clean1, clean2, ctx))
    {
        EmitAtNode(alternative, ctx, "as-err-no-implicit-conversion", {clean2, clean1});
    }
}

/**
 * @brief Validates expression statements that bare data type names.
 * @param[in] node Expression statement syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessExpressionStatementNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode expr = ts_node_named_child(node, 0);
    if (ts_node_is_null(expr))
    {
        return;
    }

    const Scope* scope = ResolveNodeScope(node, request);
    if (const auto dataTypeName = IsBareDataType(expr, scope, ctx.request.symbolTable, request.sourceCode))
    {
        EmitAtNode(expr, ctx, diagnostics::codes::ExpressionIsDataType, *dataTypeName);
        return;
    }

    if (IsFunctionReference(expr, scope, ctx))
    {
        EmitAtNode(expr, ctx, diagnostics::codes::IllegalOperation);
        return;
    }

    const std::string text = TrimString(NodeText(expr, request.sourceCode));
    if (!text.empty())
    {
        if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(text))
        {
            bool onlyNs = true;
            for (const auto& s : *syms)
            {
                if (s.type != SymbolType::Namespace)
                {
                    onlyNs = false;
                    break;
                }
            }
            if (onlyNs)
            {
                EmitAtNode(expr, ctx, "as-err-undefined-identifier", text);
            }
        }
    }
}

/**
 * @brief Infers loop variable types in foreach statements.
 * @param[in] node Foreach statement node.
 * @param[in] containerType Resolved type of the iterated container.
 * @param[in] request Analysis request.
 * @param[in,out] ctx Diagnostic collection context.
 */
void InferForeachVariables(TSNode node, const std::string& containerType, const TypeConversionCheckRequest& request,
                           DiagnosticContext& ctx)
{
    uint32_t variableIndex = 0;
    const uint32_t childCount = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        TSNode child = ts_node_named_child(node, i);
        if (ts_node_symbol(child) != parser::GetGrammarSymbols().symForeachVariable)
        {
            continue;
        }

        TSNode nameNode = parser::GetChildByField(child, parser::GetGrammarSymbols().fieldName);
        const std::string valueType = ForeachValueType(containerType, variableIndex, ctx);
        ++variableIndex;

        if (ts_node_is_null(nameNode) || valueType.empty())
        {
            continue;
        }

        const TSPoint namePoint = ts_node_start_point(nameNode);
        const Scope* bodyScope = FindEnclosingScope(request.scopeRoot, namePoint.row, namePoint.column);
        const LocalDefinition* def = ResolveInScope(
            bodyScope ? bodyScope : FindInnermostScope(request.scopeRoot, namePoint.row, namePoint.column),
            NodeText(nameNode, request.sourceCode));

        if (def && CleanBaseType(def->typeName) == "auto")
        {
            const_cast<LocalDefinition*>(def)->typeName = valueType;
        }
    }
}

/**
 * @brief Validates container iteration in foreach statements.
 * @param[in] node Foreach statement node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessForeachNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode collection = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldCollection);
    if (ts_node_is_null(collection))
    {
        return;
    }

    const Scope* scope = ResolveNodeScope(node, request);
    const std::string containerType = ResolveExpressionType(collection, ExpressionTypeContext(scope, ctx));

    const std::string containerBase = CleanExpressionType(containerType);
    if (IsCorePrimitive(containerBase) && containerBase != "auto" && containerBase != "void")
    {
        EmitAtNode(collection, ctx, "as-err-invalid-foreach-container", containerBase);
    }

    if (!containerType.empty() && request.mutableScopeRoot)
    {
        InferForeachVariables(node, containerType, request, ctx);
    }
}

/**
 * @brief Checks if an AST node is the member field of a member expression.
 * @param[in] parent Candidate member expression node.
 * @param[in] target Target AST node.
 * @return True if target matches the member field.
 */
static bool IsMemberFieldMatch(TSNode parent, TSNode target)
{
    if (ts_node_symbol(parent) != parser::GetGrammarSymbols().symMemberExpression)
    {
        return false;
    }
    TSNode memberNode = parser::GetChildByField(parent, parser::GetGrammarSymbols().fieldMember);
    return !ts_node_is_null(memberNode) &&
           (ts_node_eq(memberNode, target) || ts_node_start_byte(memberNode) == ts_node_start_byte(target));
}

/**
 * @brief Checks if a scoped identifier is qualified or accessed as a member.
 * @param[in] parent Scoped identifier AST node.
 * @param[in] sourceCode Document source text.
 * @return True if qualified or member access.
 */
static bool IsScopedIdentifierQualified(TSNode parent, std::string_view sourceCode)
{
    const uint32_t pStart = ts_node_start_byte(parent);
    const uint32_t pEnd = ts_node_end_byte(parent);
    if (pStart < sourceCode.size() && pEnd <= sourceCode.size() && pStart < pEnd)
    {
        const std::string_view scopedText = sourceCode.substr(pStart, pEnd - pStart);
        if (utils::ContainsScopeSeparator(scopedText))
        {
            return true;
        }
    }
    TSNode grandParent = parser::GetParent(parent);
    return !ts_node_is_null(grandParent) && IsMemberFieldMatch(grandParent, parent);
}

/**
 * @brief Checks if an identifier node is a qualified name or member access.
 * @param[in] node Identifier syntax node.
 * @param[in] sourceCode Document source text.
 * @return True if qualified or member access.
 */
static bool IsQualifiedOrMemberIdentifier(TSNode node, std::string_view sourceCode)
{
    TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent))
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol parentSym = ts_node_symbol(parent);
    if (parentSym == gs.symParameter || IsMemberFieldMatch(parent, node))
    {
        return true;
    }
    return (parentSym == gs.symScopedIdentifier) && IsScopedIdentifierQualified(parent, sourceCode);
}

static bool AdvanceCursorSkipChildren(TSTreeCursor& cursor, TSNode rootNode)
{
    if (ts_tree_cursor_goto_next_sibling(&cursor))
    {
        return true;
    }
    while (!ts_node_eq(ts_tree_cursor_current_node(&cursor), rootNode) && ts_tree_cursor_goto_parent(&cursor))
    {
        if (ts_node_eq(ts_tree_cursor_current_node(&cursor), rootNode))
        {
            return false;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            return true;
        }
    }
    return false;
}

static bool AdvanceCursor(TSTreeCursor& cursor, TSNode rootNode)
{
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        return true;
    }
    return AdvanceCursorSkipChildren(cursor, rootNode);
}

static bool IsMatchingLocalDefinition(const Scope* scope, std::string_view name, TSPoint nameStart)
{
    if (scope == nullptr)
    {
        return false;
    }
    const LocalDefinition* def = ResolveInScope(scope, name);
    return def != nullptr && def->name == name && def->startLine == nameStart.row &&
           def->startCharacter == nameStart.column;
}

/**
 * @brief Detects cyclic dependencies in auto initializers via lexical scope resolution.
 * @param[in] valueNode Initializer value syntax node.
 * @param[in] nameNode Declared variable name syntax node.
 * @param[in] scope Enclosing lexical scope.
 * @param[in] sourceCode Document source text.
 * @return True if a cyclic reference to the declared variable is detected.
 */
static bool IsCyclicAutoDependency(TSNode valueNode, TSNode nameNode, const Scope* scope, std::string_view sourceCode)
{
    if (ts_node_is_null(valueNode) || ts_node_is_null(nameNode))
    {
        return false;
    }
    const std::string varName = NodeText(nameNode, sourceCode);
    if (varName.empty())
    {
        return false;
    }

    const TSPoint nameStart = ts_node_start_point(nameNode);
    TSTreeCursor cursor = ts_tree_cursor_new(valueNode);
    bool cyclic = false;

    while (!cyclic)
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol sym = ts_node_symbol(node);
        if (sym == gs.symComment || sym == gs.symStringLiteral || sym == gs.symConcatenatedString)
        {
            if (!AdvanceCursorSkipChildren(cursor, valueNode))
            {
                break;
            }
            continue;
        }

        if (sym == gs.symIdentifier && NodeText(node, sourceCode) == varName &&
            !IsQualifiedOrMemberIdentifier(node, sourceCode))
        {
            if (scope == nullptr || IsMatchingLocalDefinition(scope, varName, nameStart))
            {
                cyclic = true;
                break;
            }
        }

        if (!AdvanceCursor(cursor, valueNode))
        {
            break;
        }
    }
    ts_tree_cursor_delete(&cursor);
    return cyclic;
}

/**
 * @brief Validates an auto variable declarator and writes back deduced types.
 * @param[in] child Variable declarator node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessAutoDeclarator(TSNode child, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode nameNode = parser::GetChildByField(child, parser::GetGrammarSymbols().fieldName);
    const std::string varName = NodeText(nameNode, request.sourceCode);
    TSNode valueNode = FindDeclaratorValueNode(child, request.sourceCode);

    if (ts_node_is_null(valueNode))
    {
        EmitAtNode(child, ctx, "as-err-auto-requires-initializer");
        return;
    }

    const Scope* scope = ResolveNodeScope(child, request);
    if (IsCyclicAutoDependency(valueNode, nameNode, scope, request.sourceCode))
    {
        EmitAtNode(valueNode, ctx, "as-err-cyclic-auto-dependency", varName);
        return;
    }

    if (const auto dataTypeName = IsBareDataType(valueNode, scope, ctx.request.symbolTable, request.sourceCode))
    {
        EmitAtNode(valueNode, ctx, diagnostics::codes::ExpressionIsDataType, *dataTypeName);
        return;
    }

    const std::string rhsType = ResolveExpressionType(valueNode, ExpressionTypeContext(scope, ctx));

    if (rhsType == "void")
    {
        EmitAtNode(valueNode, ctx, "as-err-cannot-infer-void");
    }
    else if (rhsType == "null")
    {
        EmitAtNode(valueNode, ctx, "as-err-cannot-infer-null");
    }
    else if (!rhsType.empty() && request.mutableScopeRoot && scope)
    {
        const LocalDefinition* def = ResolveInScope(scope, varName);
        if (def && CleanBaseType(def->typeName) == "auto")
        {
            std::string finalType = rhsType;
            if ((def->isHandleType || def->typeName.find('@') != std::string::npos) && !finalType.ends_with('@'))
            {
                finalType += '@';
            }
            const_cast<LocalDefinition*>(def)->typeName = finalType;
        }
    }
}

/**
 * @brief Validates auto-inferred variable declarations.
 * @param[in] node Variable declaration syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessAutoVariableDeclarators(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_named_child(node, i);
        if (ts_node_symbol(child) == gs.symVariableDeclarator)
        {
            ProcessAutoDeclarator(child, request, ctx);
        }
    }
}

/**
 * @brief Validates non-handle declarator construction semantics.
 * @param[in] child Variable declarator syntax node.
 * @param[in] declared Declared type details.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckDeclaratorConstruction(TSNode child, const DeclaredType& declared, const Scope* scope, DiagnosticContext& ctx)
{
    const NonInstantiableKind nonInst = ClassifyNonInstantiable(declared.baseName, ctx.request.symbolTable);
    if (nonInst == NonInstantiableKind::Abstract)
    {
        EmitAtNode(child, ctx, "as-err-abstract-instantiated", {declared.baseName, declared.baseName});
        return;
    }

    TSNode argsNode = parser::GetChildByField(child, parser::GetGrammarSymbols().fieldArguments);
    TSNode valNode = parser::GetChildByField(child, parser::GetGrammarSymbols().fieldValue);
    if (ts_node_is_null(argsNode) && ts_node_is_null(valNode))
    {
        CheckDefaultConstructor(child, declared.baseName, ctx);
    }
    else if (!ts_node_is_null(argsNode) && !declared.isTemplateOrArray)
    {
        CheckConstruction(argsNode, declared.baseName, scope, ctx);
    }
}

/**
 * @brief Validates explicitly typed variable declarations.
 * @param[in] node Variable declaration syntax node.
 * @param[in] typeNode Type specifier syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessDeclaredVariableDeclarators(TSNode node, TSNode typeNode, const TypeConversionCheckRequest& request,
                                        DiagnosticContext& ctx)
{
    const std::string fullType = NodeText(typeNode, request.sourceCode);
    const std::string baseType = CleanBaseType(fullType);
    auto funcdefSym = FindFuncdef(baseType, ctx.request.symbolTable);

    const auto& gs = parser::GetGrammarSymbols();
    const uint32_t count = ts_node_named_child_count(node);
    if (funcdefSym)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_named_child(node, i);
            if (ts_node_symbol(child) == gs.symVariableDeclarator)
            {
                TSNode valNode = parser::GetChildByField(child, gs.fieldValue);
                CheckFuncdefAssignment(child, funcdefSym->GetFuncdef(), valNode, ctx);
            }
        }
    }

    const DeclaredType declared = ReadDeclaredType(typeNode, ctx);
    if (declared.usable && !IsMixinClass(declared.baseName, ctx.request.symbolTable))
    {
        const Scope* scope = ResolveNodeScope(node, request);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_named_child(node, i);
            if (ts_node_symbol(child) != gs.symVariableDeclarator)
            {
                continue;
            }

            CheckInitializer(child, declared, scope, ctx);
            if (!declared.isHandle)
            {
                CheckDeclaratorConstruction(child, declared, scope, ctx);
            }
        }
    }
}

/**
 * @brief Validates variable declaration nodes across auto and explicit types.
 * @param[in] node Variable declaration syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessVariableDeclarationNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode typeNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldVarType);
    if (ts_node_is_null(typeNode))
    {
        typeNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldType);
    }

    const std::string rawType = CleanBaseType(NodeText(typeNode, request.sourceCode));
    if (rawType == "auto")
    {
        ProcessAutoVariableDeclarators(node, request, ctx);
    }
    else
    {
        ProcessDeclaredVariableDeclarators(node, typeNode, request, ctx);
    }
}

/**
 * @brief Validates property access mutations on assignment LHS.
 * @param[in] node Full assignment node.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckAssignmentPropertyAccess(TSNode node, const Scope* scope, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode left = parser::GetChildByField(node, gs.fieldLeft);
    if (ts_node_is_null(left))
    {
        return;
    }
    TSNode opNode = parser::GetChildByField(node, gs.fieldOperator);
    const TSSymbol opSym = ts_node_symbol(opNode);

    const TSSymbol leftSym = ts_node_symbol(left);
    if (leftSym == gs.symIndexExpression)
    {
        TSNode arrayNode = parser::GetChildByField(left, gs.fieldObject);
        if (ts_node_is_null(arrayNode) && ts_node_named_child_count(left) > 0)
        {
            arrayNode = ts_node_named_child(left, 0);
        }
        PropertyAccessInfo pInfo = InspectPropertyAccess(arrayNode, scope, ctx);
        if (pInfo.isProperty && pInfo.isIndexed)
        {
            if (opSym != gs.tokAssign)
            {
                EmitAtNode(node, ctx, "as-err-compound-assign-on-indexed-prop", pInfo.propName);
            }
            if (!pInfo.hasSet && pInfo.hasGet)
            {
                EmitAtNode(left, ctx, "as-err-read-only-property", pInfo.propName);
            }
        }
    }
    else
    {
        PropertyAccessInfo pInfo = InspectPropertyAccess(left, scope, ctx);
        if (pInfo.isProperty)
        {
            if (!pInfo.hasSet && pInfo.hasGet)
            {
                EmitAtNode(left, ctx, "as-err-read-only-property", pInfo.propName);
            }
            else if (opSym != gs.tokAssign && pInfo.receiverType.find('@') == std::string::npos)
            {
                EmitAtNode(node, ctx, "as-err-compound-assign-on-value-prop", pInfo.propName);
            }
        }
    }
}

/**
 * @brief Resolves variable or property type from scope or symbol table.
 * @param[in] node Expression node representing identifier or property.
 * @param[in] scope Enclosing lexical scope.
 * @param[in] ctx Diagnostic collection context.
 * @return Type string if found; empty otherwise.
 */
std::string GetVariableOrPropertyType(TSNode node, const Scope* scope, const DiagnosticContext& ctx)
{
    const std::string name = NodeText(node, ctx.request.sourceCode);
    if (scope)
    {
        if (const auto* def = ResolveInScopeAt(scope, LastScopeSegment(name), ts_node_start_byte(node)))
        {
            return def->typeName;
        }
    }
    if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(name))
    {
        for (const auto& s : *syms)
        {
            if (s.type == SymbolType::Variable || s.type == SymbolType::Property)
            {
                return s.GetVariable().typeName;
            }
        }
    }
    return {};
}

/**
 * @brief Unwraps handle operator `@` from an expression node.
 * @param[in] expr Expression node.
 * @param[in,out] isHandleAssignment Flag set to true if `@` operator is encountered.
 * @return Unwrapped operand node if handle operator was present; expr otherwise.
 */
TSNode UnwrapHandleOperand(TSNode expr, bool& isHandleAssignment)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(expr) == gs.symUnaryExpression)
    {
        TSNode op = parser::GetChildByField(expr, gs.fieldOperator);
        if (!ts_node_is_null(op) && ts_node_symbol(op) == gs.tokAt)
        {
            isHandleAssignment = true;
            TSNode operand = parser::GetChildByField(expr, gs.fieldOperand);
            if (!ts_node_is_null(operand))
            {
                return operand;
            }
        }
    }
    return expr;
}

/**
 * @brief Validates const qualifier preservation during handle assignment.
 * @param[in] operands Left and right assignment operand nodes.
 * @param[in] types Left and right base types.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckHandleConstAssignment(AssignmentOperands operands, const ConversionTypes& types, const Scope* scope,
                                DiagnosticContext& ctx)
{
    bool isHandleAssignment = false;
    TSNode actualLeft = UnwrapHandleOperand(operands.left, isHandleAssignment);
    TSNode actualRight = UnwrapHandleOperand(operands.right, isHandleAssignment);

    if (!isHandleAssignment)
    {
        return;
    }

    const std::string leftFull = GetVariableOrPropertyType(actualLeft, scope, ctx);
    const std::string rightFull = GetVariableOrPropertyType(actualRight, scope, ctx);
    const bool rightConstTarget = rightFull.starts_with("const ");
    const bool leftConstTarget = leftFull.starts_with("const ");
    if (rightConstTarget && !leftConstTarget && !types.from.empty() && !types.to.empty())
    {
        EmitAtNode(operands.right, ctx, "as-err-no-implicit-conversion", {"const " + types.from + "@", types.to + "@"});
    }
}

/**
 * @brief Validates operator overload or implicit conversion on assignment.
 * @param[in] node Assignment expression syntax node.
 * @param[in] right Right-hand side expression node.
 * @param[in] types Left and right base types.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckAssignmentOperatorConversion(TSNode node, TSNode right, const ConversionTypes& types, DiagnosticContext& ctx)
{
    if (types.to.empty())
    {
        return;
    }

    auto opSyms = ctx.request.symbolTable.FindMemberSymbolPtr(types.to, "opAssign");
    if (opSyms)
    {
        for (const auto& sym : *opSyms)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature) &&
                sym.GetFunction().modifiers.isDelete)
            {
                EmitAtNode(node, ctx, "as-err-deleted-method-called", {types.to, "opAssign"});
                break;
            }
        }
    }

    if (!types.from.empty() && types.to != types.from)
    {
        const TSNode assignOp = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldOperator);
        const std::string_view overload = ts_node_is_null(assignOp)
                                              ? std::string_view()
                                              : AssignmentOverloadName(NodeText(assignOp, ctx.request.sourceCode));

        const bool operatorHandlesIt = DeclaresOperatorMethod(types.to, overload, ctx.request.symbolTable);
        if (!operatorHandlesIt && !IsConvertible(types.from, types.to, ctx))
        {
            EmitAtNode(right, ctx, "as-err-no-implicit-conversion", {types.from, types.to});
        }
    }
}

/**
 * @brief Checks if a custom class/value type accepts handle assignment via constructor or opAssign.
 * @param[in] typeName Name of target data type.
 * @param[in] table Symbol table.
 * @return True if type declares converting constructor or opAssign taking a handle.
 */
static bool HasHandleOpAssign(std::string_view clean, const SymbolTable& table)
{
    if (auto opSyms = table.FindMemberSymbolPtr(clean, "opAssign"))
    {
        for (const auto& sym : *opSyms)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                const auto& fn = sym.GetFunction();
                if (!fn.parameters.empty() &&
                    (fn.parameters[0].isHandle || fn.parameters[0].typeName.find('@') != std::string::npos))
                {
                    return true;
                }
            }
        }
    }
    return table.FindMemberSymbolPtr(clean, "opHndlAssign") != nullptr;
}

static bool HasHandleConvertingCtor(std::string_view clean, const SymbolTable& table)
{
    if (auto ctorSyms = table.FindMemberSymbolPtr(clean, clean))
    {
        for (const auto& sym : *ctorSyms)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                const auto& fn = sym.GetFunction();
                if (!fn.modifiers.isExplicit && fn.parameters.size() == 1 &&
                    (fn.parameters[0].isHandle || fn.parameters[0].typeName.find('@') != std::string::npos))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool TypeAcceptsNullHandle(std::string_view typeName, const SymbolTable& table)
{
    const std::string_view clean = CleanBaseTypeView(typeName);
    if (clean.empty() || IsCorePrimitive(clean))
    {
        return false;
    }
    return HasHandleOpAssign(clean, table) || HasHandleConvertingCtor(clean, table);
}

static TSNode UnwrapParentheses(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    while (!ts_node_is_null(node) && ts_node_symbol(node) == gs.symParenthesizedExpression &&
           ts_node_named_child_count(node) > 0)
    {
        node = ts_node_named_child(node, 0);
    }
    return node;
}

/**
 * @brief Validates assignment of null literal to non-handle types.
 * @param[in] node Assignment expression AST node.
 * @param[in] right Right-hand side expression node.
 * @param[in] rawLeftType Unstripped expression type of left-hand side.
 * @param[in,out] ctx Diagnostic context.
 * @return True if right-hand side is null literal and was processed.
 */
static bool CheckNullAssignmentToNonHandle(TSNode node, TSNode right, const std::string& rawLeftType,
                                           DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode unwrappedRight = UnwrapParentheses(right);
    if (ts_node_symbol(unwrappedRight) != gs.symNullLiteral)
    {
        return false;
    }

    const TSNode assignOp = ts_node_child_by_field_id(node, gs.fieldOperator);
    const std::string_view opText = ts_node_is_null(assignOp)
                                        ? std::string_view("=")
                                        : std::string_view(NodeText(assignOp, ctx.request.sourceCode));
    if (opText != "=")
    {
        return false;
    }

    if (rawLeftType.empty() || ExpressionTypeCache::IsPoisonType(rawLeftType))
    {
        return false;
    }

    const TSNode left = ts_node_child_by_field_id(node, gs.fieldLeft);
    if (IsHandleAssignmentTarget(left))
    {
        if (rawLeftType.find('@') != std::string::npos || rawLeftType == "auto" || rawLeftType == "?" ||
            rawLeftType == "any")
        {
            return true;
        }
    }

    if (rawLeftType == "auto" || rawLeftType == "?" || rawLeftType == "any")
    {
        return true;
    }

    if (!TypeAcceptsNullHandle(rawLeftType, ctx.request.symbolTable))
    {
        EmitAtNode(right, ctx, "as-err-null-non-handle", rawLeftType);
        return true;
    }
    return true;
}

static bool CheckRightSideNamespaceOnly(TSNode right, DiagnosticContext& ctx)
{
    const std::string rightIdent = TrimString(NodeText(right, ctx.request.sourceCode));
    if (rightIdent.empty())
    {
        return false;
    }
    if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(rightIdent))
    {
        for (const auto& s : *syms)
        {
            if (s.type != SymbolType::Namespace)
            {
                return false;
            }
        }
        EmitAtNode(right, ctx, "as-err-undefined-identifier", rightIdent);
        return true;
    }
    return false;
}

/**
 * @brief Validates assignment expression semantics and type compatibility.
 * @param[in] node Assignment expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessAssignmentNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode left = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldLeft);
    TSNode right = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldRight);
    if (ts_node_is_null(left) || ts_node_is_null(right))
    {
        return;
    }

    const Scope* scope = ResolveNodeScope(node, request);
    if (const auto dt = IsBareDataType(left, scope, ctx.request.symbolTable, ctx.request.sourceCode))
    {
        EmitAtNode(left, ctx, diagnostics::codes::ExpressionIsDataType, *dt);
        return;
    }
    if (const auto dt = IsBareDataType(right, scope, ctx.request.symbolTable, ctx.request.sourceCode))
    {
        EmitAtNode(right, ctx, diagnostics::codes::ExpressionIsDataType, *dt);
        return;
    }

    if (CheckRightSideNamespaceOnly(right, ctx))
    {
        return;
    }

    CheckAssignmentPropertyAccess(node, scope, ctx);

    const std::string leftType = ResolveExpressionType(left, ExpressionTypeContext(scope, ctx));
    if (CheckNullAssignmentToNonHandle(node, right, leftType, ctx))
    {
        return;
    }

    const std::string rightType = ResolveExpressionType(right, ExpressionTypeContext(scope, ctx));
    if (ExpressionTypeCache::IsPoisonType(leftType) || ExpressionTypeCache::IsPoisonType(rightType))
    {
        return;
    }
    const ConversionTypes types{CleanBaseType(rightType), CleanBaseType(leftType)};

    CheckFloatTruncation(right, types, scope, ctx);

    auto leftFuncdef = FindFuncdef(types.to, ctx.request.symbolTable);
    if (leftFuncdef)
    {
        CheckFuncdefAssignment(node, leftFuncdef->GetFuncdef(), right, ctx);
    }
    else if (types.from.empty() && IsFunctionReference(right, scope, ctx))
    {
        EmitAtNode(right, ctx, "as-err-no-implicit-conversion",
                   {TrimString(NodeText(right, ctx.request.sourceCode)), types.to});
    }

    CheckHandleConstAssignment({left, right}, types, scope, ctx);
    CheckAssignmentOperatorConversion(node, right, types, ctx);
}

/**
 * @brief Validates signedness compatibility in binary comparison expressions.
 * @param[in] node Binary expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessBinaryNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode opNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldOperator);
    if (ts_node_is_null(opNode))
    {
        return;
    }
    const std::string_view op = GetNodeTextView(opNode, ctx.request.sourceCode);
    const Scope* scope = ResolveNodeScope(node, request);

    if (op == "is" || op == "!is")
    {
        CheckHandleComparison(node, scope, ctx);
        return;
    }

    if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=")
    {
        CheckSignedUnsignedComparison(node, scope, ctx);
        CheckHandleComparison(node, scope, ctx);
        CheckComparisonOperatorCompatibility(node, scope, ctx);
        return;
    }

    CheckBinaryOperatorCompatibility(node, scope, ctx);
}

static TSNode FindUnaryOperand(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode operand = parser::GetChildByField(node, gs.fieldOperand);
    if (!ts_node_is_null(operand))
    {
        return operand;
    }
    const TSNode opNode = parser::GetChildByField(node, gs.fieldOperator);
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode ch = ts_node_named_child(node, i);
        if (!ts_node_is_null(opNode) && ts_node_eq(ch, opNode))
        {
            continue;
        }
        const TSSymbol sym = ts_node_symbol(ch);
        if (sym != gs.tokPlusPlus && sym != gs.tokDashDash && sym != gs.tokBang && sym != gs.tokNot &&
            sym != gs.tokDash && sym != gs.tokPlus && sym != gs.tokTilde)
        {
            return ch;
        }
    }
    return TSNode{};
}

static bool CheckUnaryInvalidOperand(TSNode node, TSNode operand, const Scope* scope, DiagnosticContext& ctx)
{
    if (ts_node_is_null(operand))
    {
        return false;
    }
    if (const auto dt = IsBareDataType(operand, scope, ctx.request.symbolTable, ctx.request.sourceCode))
    {
        EmitAtNode(operand, ctx, diagnostics::codes::ExpressionIsDataType, *dt);
        return true;
    }
    const auto& gs = parser::GetGrammarSymbols();
    TSNode opNode = parser::GetChildByField(node, gs.fieldOperator);
    if (!ts_node_is_null(opNode) && ts_node_symbol(opNode) == gs.tokAt)
    {
        return false;
    }
    if (IsFunctionReference(operand, scope, ctx))
    {
        EmitAtNode(node, ctx, diagnostics::codes::IllegalOperation);
        return true;
    }
    return false;
}

/**
 * @brief Validates increment and decrement expressions on virtual properties.
 * @param[in] node Unary or postfix expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessUnaryOrPostfixNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const Scope* scope = ResolveNodeScope(node, request);
    TSNode operand = FindUnaryOperand(node);
    if (CheckUnaryInvalidOperand(node, operand, scope, ctx))
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    TSNode opNode = parser::GetChildByField(node, gs.fieldOperator);
    const TSSymbol opSym = ts_node_symbol(opNode);
    if (opSym != gs.tokPlusPlus && opSym != gs.tokDashDash)
    {
        return;
    }

    if (!ts_node_is_null(operand))
    {
        TSNode unwrapped = UnwrapParentheses(operand);
        if (ts_node_symbol(unwrapped) == gs.symIndexExpression)
        {
            TSNode arrayNode = parser::GetChildByField(unwrapped, gs.fieldObject);
            if (ts_node_is_null(arrayNode) && ts_node_named_child_count(unwrapped) > 0)
            {
                arrayNode = ts_node_named_child(unwrapped, 0);
            }
            PropertyAccessInfo pInfo = InspectPropertyAccess(arrayNode, scope, ctx);
            if (pInfo.isProperty && pInfo.isIndexed)
            {
                EmitAtNode(node, ctx, "as-err-inc-dec-on-virtual-prop", pInfo.propName);
                return;
            }
        }
        else
        {
            PropertyAccessInfo pInfo = InspectPropertyAccess(unwrapped, scope, ctx);
            if (pInfo.isProperty)
            {
                EmitAtNode(node, ctx, "as-err-inc-dec-on-virtual-prop", pInfo.propName);
                return;
            }
        }
    }
}

/**
 * @brief Validates member expressions against write-only properties.
 * @param[in] node Member expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessMemberExpressionNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode parent = parser::GetParent(node);
    bool isLhsAssignment = false;
    bool isUnaryArg = false;
    if (!ts_node_is_null(parent))
    {
        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol parentSym = ts_node_symbol(parent);
        if (parentSym == gs.symCallExpression)
        {
            TSNode callee = parser::GetChildByField(parent, gs.fieldFunction);
            if (ts_node_eq(callee, node))
            {
                return;
            }
        }
        else if (parentSym == gs.symAssignmentExpression)
        {
            TSNode leftChild = parser::GetChildByField(parent, parser::GetGrammarSymbols().fieldLeft);
            if (ts_node_eq(leftChild, node))
            {
                isLhsAssignment = true;
            }
        }
        else if (parentSym == gs.symUnaryExpression || parentSym == gs.symPostfixExpression)
        {
            isUnaryArg = true;
        }
    }

    if (!isLhsAssignment && !isUnaryArg)
    {
        const Scope* scope = ResolveNodeScope(node, request);
        PropertyAccessInfo pInfo = InspectPropertyAccess(node, scope, ctx);
        if (pInfo.isProperty && !pInfo.hasGet && pInfo.hasSet)
        {
            EmitAtNode(node, ctx, "as-err-write-only-property", pInfo.propName);
        }
    }
}

/**
 * @brief Validates index expressions against write-only indexed properties.
 * @param[in] node Index expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessIndexExpressionNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode parent = parser::GetParent(node);
    bool isLhsAssignment = false;
    bool isUnaryArg = false;
    if (!ts_node_is_null(parent))
    {
        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol parentSym = ts_node_symbol(parent);
        if (parentSym == gs.symAssignmentExpression)
        {
            TSNode leftChild = parser::GetChildByField(parent, gs.fieldLeft);
            if (ts_node_eq(leftChild, node))
            {
                isLhsAssignment = true;
            }
        }
        else if (parentSym == gs.symUnaryExpression || parentSym == gs.symPostfixExpression)
        {
            isUnaryArg = true;
        }
    }

    if (!isLhsAssignment && !isUnaryArg)
    {
        const Scope* scope = ResolveNodeScope(node, request);
        TSNode arrayNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldObject);
        if (ts_node_is_null(arrayNode) && ts_node_named_child_count(node) > 0)
        {
            arrayNode = ts_node_named_child(node, 0);
        }
        PropertyAccessInfo pInfo = InspectPropertyAccess(arrayNode, scope, ctx);
        if (pInfo.isProperty && pInfo.isIndexed && !pInfo.hasGet && pInfo.hasSet)
        {
            EmitAtNode(node, ctx, "as-err-write-only-property", pInfo.propName);
        }
    }
}

/**
 * @brief Validates explicit cast expression safety.
 * @param[in] node Cast expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessCastNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const Scope* scope = ResolveNodeScope(node, request);
    CheckCast(node, scope, ctx);
}

/**
 * @brief Validates return statements inside lambda bodies.
 * @param[in] expr Returned expression syntax node.
 * @param[in] lambdaNode Parent lambda expression node.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckLambdaReturnStatement(TSNode expr, TSNode lambdaNode, const Scope* scope, DiagnosticContext& ctx)
{
    const auto target = FuncdefTargetOfLambda(lambdaNode, ctx.request.symbolTable, ctx.request.sourceCode);
    if (!target)
    {
        return;
    }

    const std::string expected = CleanBaseType(target->GetFuncdef().returnType);
    if (expected == "void")
    {
        EmitAtNode(expr, ctx, "as-err-void-return-value");
        return;
    }

    if (!expected.empty())
    {
        if (const auto dataTypeName = IsBareDataType(expr, scope, ctx.request.symbolTable, ctx.request.sourceCode))
        {
            EmitAtNode(expr, ctx, diagnostics::codes::ExpressionIsDataType, *dataTypeName);
            return;
        }
        if (IsFunctionReference(expr, scope, ctx))
        {
            EmitAtNode(expr, ctx, "as-err-no-implicit-conversion",
                       {TrimString(NodeText(expr, ctx.request.sourceCode)), expected});
            return;
        }

        const std::string actual = CleanBaseType(ResolveExpressionType(expr, ExpressionTypeContext(scope, ctx)));
        CheckFloatTruncation(expr, {actual, expected}, scope, ctx);
        if (!actual.empty() && actual != expected && !IsConvertible(actual, expected, ctx))
        {
            EmitAtNode(expr, ctx, "as-err-no-implicit-conversion", {actual, expected});
        }
    }
}

/**
 * @brief Checks if a function declaration specifies a reference return type.
 * @param[in] funcNode Function declaration AST node.
 * @param[in] retTypeNode Return type specifier AST node.
 * @param[in] sourceCode Document source text.
 * @return True if return type is by-reference; false otherwise.
 */
bool IsFunctionReturnRef(TSNode funcNode, TSNode retTypeNode, std::string_view sourceCode)
{
    TSNode nameNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldName);
    const uint32_t headEnd = ts_node_is_null(nameNode) ? ts_node_end_byte(retTypeNode) : ts_node_start_byte(nameNode);
    const uint32_t headStart = ts_node_start_byte(funcNode);
    const std::string headText = (headEnd > headStart && headEnd <= sourceCode.size())
                                     ? std::string(sourceCode.substr(headStart, headEnd - headStart))
                                     : "";
    const std::string rawRetText = NodeText(retTypeNode, sourceCode);
    return headText.find('&') != std::string::npos || rawRetText.find('&') != std::string::npos;
}

/**
 * @brief Validates returned reference lifetime against local variables and parameters.
 * @param[in] expr Returned expression node.
 * @param[in] funcNode Function declaration node.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckReturnReference(TSNode expr, TSNode funcNode, const Scope* scope, DiagnosticContext& ctx)
{
    if (!scope)
    {
        return;
    }

    std::string exprText = NodeText(expr, ctx.request.sourceCode);
    while (!exprText.empty() && isspace(static_cast<unsigned char>(exprText.front())))
    {
        exprText.erase(exprText.begin());
    }
    while (!exprText.empty() && isspace(static_cast<unsigned char>(exprText.back())))
    {
        exprText.pop_back();
    }

    const LocalDefinition* def = ResolveInScopeAt(scope, exprText, ts_node_start_byte(expr));
    if (!def)
    {
        return;
    }

    const TSPoint funcStart = ts_node_start_point(funcNode);
    const TSPoint funcEnd = ts_node_end_point(funcNode);
    const bool isInsideFunction = (def->startLine > funcStart.row && def->startLine < funcEnd.row) ||
                                  (def->startLine == funcStart.row && def->startCharacter >= funcStart.column);
    if (!isInsideFunction)
    {
        return;
    }

    if (def->kind == LocalDefinitionKind::Parameter)
    {
        EmitAtNode(expr, ctx, "as-err-cannot-return-param-ref", exprText);
    }
    else if (def->kind == LocalDefinitionKind::Variable)
    {
        EmitAtNode(expr, ctx, "as-err-cannot-return-local-ref", exprText);
    }
}

/**
 * @brief Validates return statements inside function declarations.
 * @param[in] expr Returned expression syntax node.
 * @param[in] funcNode Parent function declaration syntax node.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckFuncDeclarationReturnStatement(TSNode expr, TSNode funcNode, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode retTypeNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldReturnType);
    if (ts_node_is_null(retTypeNode))
    {
        retTypeNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldType);
    }
    if (ts_node_is_null(retTypeNode))
    {
        return;
    }

    if (IsFunctionReturnRef(funcNode, retTypeNode, ctx.request.sourceCode))
    {
        CheckReturnReference(expr, funcNode, scope, ctx);
    }

    const std::string rawRetText = NodeText(retTypeNode, ctx.request.sourceCode);
    const std::string expected = CleanBaseType(rawRetText);
    if (expected == "void")
    {
        EmitAtNode(expr, ctx, "as-err-void-return-value");
        return;
    }

    if (!expected.empty())
    {
        if (const auto dataTypeName = IsBareDataType(expr, scope, ctx.request.symbolTable, ctx.request.sourceCode))
        {
            EmitAtNode(expr, ctx, diagnostics::codes::ExpressionIsDataType, *dataTypeName);
            return;
        }
        if (IsFunctionReference(expr, scope, ctx))
        {
            EmitAtNode(expr, ctx, "as-err-no-implicit-conversion",
                       {TrimString(NodeText(expr, ctx.request.sourceCode)), expected});
            return;
        }

        const std::string rawActual = ResolveExpressionType(expr, ExpressionTypeContext(scope, ctx));
        if (ExpressionTypeCache::IsPoisonType(rawActual))
        {
            return;
        }
        const std::string actual = CleanBaseType(rawActual);
        CheckFloatTruncation(expr, {actual, expected}, scope, ctx);
        if (!actual.empty() && actual != expected && !IsConvertible(actual, expected, ctx))
        {
            EmitAtNode(expr, ctx, "as-err-no-implicit-conversion", {actual, expected});
        }
    }
}

/**
 * @brief Validates return statements across enclosing function or lambda bodies.
 * @param[in] node Return statement syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessReturnStatementNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode expr = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldValue);
    if (ts_node_is_null(expr))
    {
        if (ts_node_named_child_count(node) == 0)
        {
            return;
        }
        expr = ts_node_named_child(node, 0);
    }
    const Scope* scope = ResolveNodeScope(node, request);
    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 2> returnTargets = {gs.symLambdaExpression, gs.symFuncDeclaration};
    TSNode parent = parser::FindAncestorWithAnySymbol(parser::GetParent(node), returnTargets);
    if (!ts_node_is_null(parent))
    {
        const TSSymbol pSym = ts_node_symbol(parent);
        if (pSym == gs.symLambdaExpression)
        {
            CheckLambdaReturnStatement(expr, parent, scope, ctx);
        }
        else if (pSym == gs.symFuncDeclaration)
        {
            CheckFuncDeclarationReturnStatement(expr, parent, scope, ctx);
        }
    }
}

/**
 * @brief Resolves callee node from call or construct expression.
 * @param[in] node Call or construct expression node.
 * @return Callee node, or null node if none found.
 */
TSNode ResolveCalleeNode(TSNode node)
{
    return parser::GetCallCallee(node);
}

/**
 * @brief Validates call and explicit construct call expressions.
 * @param[in] node Call expression syntax node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessCallOrConstructNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode callee = ResolveCalleeNode(node);
    const std::string calleeName = CleanBaseType(NodeText(callee, request.sourceCode));

    TSNode argsNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldArguments);
    TSNode soleArgument = {};
    if (!ts_node_is_null(argsNode) && ts_node_named_child_count(argsNode) == 1)
    {
        soleArgument = ts_node_named_child(argsNode, 0);
    }
    const bool soleArgumentIsLambda = (!ts_node_is_null(soleArgument) &&
                                       ts_node_symbol(soleArgument) == parser::GetGrammarSymbols().symLambdaExpression);

    std::optional<Symbol> calleeFuncdef;
    if (soleArgumentIsLambda)
    {
        calleeFuncdef = FindFuncdef(calleeName, ctx.request.symbolTable);
    }

    const NonInstantiableKind calleeNonInst = ClassifyNonInstantiable(calleeName, ctx.request.symbolTable);
    if (calleeNonInst == NonInstantiableKind::Abstract)
    {
        EmitAtNode(node, ctx, "as-err-abstract-instantiated", {calleeName, calleeName});
    }
    else if (calleeNonInst == NonInstantiableKind::Mixin)
    {
        EmitAtNode(node, ctx, "as-err-mixin-not-a-type", calleeName);
    }
    else if (calleeFuncdef)
    {
        CheckFuncdefAssignment(node, calleeFuncdef->GetFuncdef(), soleArgument, ctx);
    }
    else
    {
        const DeclaredType target = ReadDeclaredType(callee, ctx);
        if (target.usable && !target.isHandle && IsSameType(calleeName, target.baseName))
        {
            const Scope* scope = ResolveNodeScope(node, request);
            CheckConstruction(argsNode, target.baseName, scope, ctx);
        }
    }
}

/**
 * @brief Processes statement nodes during type conversion analysis.
 * @param[in] sym Tree-Sitter grammar symbol.
 * @param[in] node Syntax tree node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 * @return True if node was handled as a statement; false otherwise.
 */
bool ProcessStatementNode(TSSymbol sym, TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symExpressionStatement)
    {
        ProcessExpressionStatementNode(node, request, ctx);
        return true;
    }
    if (sym == gs.symForeachStatement)
    {
        ProcessForeachNode(node, request, ctx);
        return true;
    }
    if (sym == gs.symVariableDeclaration)
    {
        ProcessVariableDeclarationNode(node, request, ctx);
        return true;
    }
    if (sym == gs.symReturnStatement)
    {
        ProcessReturnStatementNode(node, request, ctx);
        return true;
    }
    if (sym == gs.symFuncDeclaration)
    {
        CheckConstructorDelegation(node, ctx);
        return true;
    }
    return false;
}

/**
 * @brief Processes expression nodes during type conversion analysis.
 * @param[in] sym Tree-Sitter grammar symbol.
 * @param[in] node Syntax tree node.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void ProcessExpressionNode(TSSymbol sym, TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symAssignmentExpression)
    {
        ProcessAssignmentNode(node, request, ctx);
    }
    else if (sym == gs.symBinaryExpression)
    {
        ProcessBinaryNode(node, request, ctx);
    }
    else if (sym == gs.symUnaryExpression || sym == gs.symPostfixExpression)
    {
        ProcessUnaryOrPostfixNode(node, request, ctx);
    }
    else if (sym == gs.symMemberExpression)
    {
        ProcessMemberExpressionNode(node, request, ctx);
    }
    else if (sym == gs.symIndexExpression)
    {
        ProcessIndexExpressionNode(node, request, ctx);
    }
    else if (sym == gs.symCastExpression)
    {
        ProcessCastNode(node, request, ctx);
    }
    else if (sym == gs.symCallExpression || sym == gs.symConstructCallExpression)
    {
        ProcessCallOrConstructNode(node, request, ctx);
    }
}

/**
 * @brief Checks if a symbol represents a node relevant to type conversion analysis.
 * @param[in] sym Tree-Sitter grammar symbol to inspect.
 * @param[in] gs Pre-resolved grammar symbols reference.
 * @return True if conversion analysis applies to the node type.
 */
bool IsConversionRelevantSymbol(TSSymbol sym, const parser::GrammarSymbols& gs)
{
    return std::find(gs.conversionTargetSymbols.begin(), gs.conversionTargetSymbols.end(), sym) !=
           gs.conversionTargetSymbols.end();
}

void ProcessNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (!IsConversionRelevantSymbol(sym, gs))
    {
        return;
    }

    if (sym == gs.symIfStatement || sym == gs.symWhileStatement || sym == gs.symDoWhileStatement ||
        sym == gs.symForStatement)
    {
        ProcessConditionNode(node, request, ctx);
        return;
    }
    if (sym == gs.symTernaryExpression)
    {
        ProcessConditionNode(node, request, ctx);
        ProcessTernaryNode(node, request, ctx);
        return;
    }

    if (!ProcessStatementNode(sym, node, request, ctx))
    {
        ProcessExpressionNode(sym, node, request, ctx);
    }
}

/**
 * @brief Iterates syntax sub-trees iteratively without stack recursion.
 * @param[in] root Root node of syntax tree or subtree.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic collection context.
 */
void VisitNode(TSNode root, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    bool hasChild = true;
    while (hasChild)
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        if (ts_node_is_named(node))
        {
            ProcessNode(node, request, ctx);
        }

        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }
        hasChild = false;
        while (ts_tree_cursor_goto_parent(&cursor))
        {
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                hasChild = true;
                break;
            }
        }
    }
    ts_tree_cursor_delete(&cursor);
}
} // namespace

bool IsTruthyCondition(const std::string& typeName, const SymbolTable& table)
{
    if (typeName.empty())
    {
        return false;
    }

    if (typeName.ends_with('@') || typeName.find('@') != std::string::npos)
    {
        return true;
    }

    const std::string clean = CleanBaseType(typeName);
    if (clean == "bool")
    {
        return true;
    }

    return BoolConversionOperator(clean, table) != nullptr;
}

bool CanConvertImplicitly(const std::string& fromType, const std::string& toType, const DiagnosticContext& ctx)
{
    return IsConvertible(CleanBaseType(fromType), CleanBaseType(toType), ctx);
}

void CheckTypeConversions(const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.root) || request.sourceCode.empty())
    {
        return;
    }

    if (ctx.logger && ctx.logger->IsDebugEnabled())
    {
        ctx.logger->LogDebug(
            fmt::format("[TypeConversionChecker] Running CheckTypeConversions for URI: {}", ctx.request.fileUri));
    }

    if (request.nodeIndex)
    {
        const auto& gs = parser::GetGrammarSymbols();
        request.nodeIndex->ForEachNodeOrdered(
            std::span<const TSSymbol>(gs.conversionTargetSymbols.data(), gs.conversionTargetSymbols.size()),
            [&](TSNode node) { ProcessNode(node, request, ctx); });
    }
    else
    {
        VisitNode(request.root, request, ctx);
    }

    if (ctx.logger && ctx.logger->IsTraceEnabled())
    {
        ctx.logger->LogTrace(
            fmt::format("[TypeConversionChecker] Finished CheckTypeConversions for URI: {}", ctx.request.fileUri));
    }
}

namespace TypeConversionChecker
{
bool evaluateUserConversions(const TypeInfo& source, const TypeInfo& target, size_t depth,
                             ankerl::unordered_dense::set<std::string>& visitedEdges)
{
    if (depth >= MAX_CONVERSION_DEPTH)
    {
        return false;
    }
    if (source == target)
    {
        return true;
    }
    const std::string edgeKey = source.name + "->" + target.name;
    return visitedEdges.contains(edgeKey);
}

bool canConvertImplicitly(const TypeInfo& source, const TypeInfo& target, size_t depth,
                          ankerl::unordered_dense::set<std::string>& visitedEdges)
{
    if (depth >= MAX_CONVERSION_DEPTH)
    {
        return false;
    }

    if (source == target)
    {
        return true;
    }

    std::string edgeKey = source.name + "->" + target.name;
    if (!visitedEdges.insert(edgeKey).second)
    {
        return false;
    }

    bool convertible = evaluateUserConversions(source, target, depth + 1, visitedEdges);
    visitedEdges.erase(edgeKey);
    return convertible;
}
} // namespace TypeConversionChecker
} // namespace angel_lsp::analysis
