#include "analysis/resolution/CallableAndLambdaResolver.h"

#include "analysis/index/LambdaTargetFacts.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/CallableResolutionInternal.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
std::vector<Symbol> CollectConstructorCandidates(const std::string& baseName, uint32_t byteOffset,
                                                 std::string_view sourceCode, const SymbolTable& symbolTable,
                                                 const NodeIndex* nodeIndex)
{
    std::vector<Symbol> candidateSymbols;
    auto ctorSyms = symbolTable.FindMemberSymbols(baseName, LastScopeSegment(baseName));
    for (const auto& s : ctorSyms)
    {
        if (s.type == SymbolType::Function)
        {
            candidateSymbols.push_back(s);
        }
    }

    if (candidateSymbols.empty())
    {
        auto scopeSyms = FindSymbolsInScope(baseName, byteOffset, sourceCode, symbolTable, nodeIndex);
        for (const auto& s : scopeSyms)
        {
            if (s.type == SymbolType::Class)
            {
                std::string qName = s.qualifiedName.empty() ? s.name : s.qualifiedName;
                auto qCtors = symbolTable.FindMemberSymbols(qName, s.name);
                for (const auto& cs : qCtors)
                {
                    if (cs.type == SymbolType::Function)
                    {
                        candidateSymbols.push_back(cs);
                    }
                }
            }
        }
    }

    if (candidateSymbols.empty())
    {
        auto fnSyms = symbolTable.FindSymbols(baseName);
        for (const auto& s : fnSyms)
        {
            if (s.type == SymbolType::Function)
            {
                candidateSymbols.push_back(s);
            }
        }
    }

    return candidateSymbols;
}

// --- A lambda against the funcdef it is being handed to -------------------------------

std::string LastSegmentOf(const std::string& name)
{
    return std::string(utils::LastScopeSegment(name));
}

/**
 * @brief True for a name that denotes a typedef, whose alias no spelling can see through.
 */
bool NamesATypedef(const std::string& name, const SymbolTable& table)
{
    const auto bucket = table.FindSymbolsPtr(name);
    if (!bucket)
    {
        return false;
    }
    for (const auto& sym : *bucket)
    {
        if (sym.type == SymbolType::Typedef)
        {
            return true;
        }
    }
    return false;
}

bool LambdaContradictsFuncdef(const std::vector<LambdaParameter>& lambdaParameters, const FuncdefSignature& funcdefSig,
                              const SymbolTable& table)
{
    if (lambdaParameters.size() != funcdefSig.parameters.size())
    {
        return true;
    }

    for (size_t i = 0; i < lambdaParameters.size(); ++i)
    {
        const LambdaParameter& written = lambdaParameters[i];
        if (!written.hasWrittenType)
        {
            continue;
        }

        const ParameterInformation& expected = funcdefSig.parameters[i];
        if (written.isHandle != expected.isHandle || written.isReference != expected.isReference ||
            written.modifier != expected.modifier)
        {
            return true;
        }

        // Compared by last `::` segment, so a name the funcdef writes bare and the lambda
        // writes qualified - or the other way round - is the same name. That costs the
        // `A::Foo` against `B::Foo` case, which is a rejection this misses rather than a legal
        // program it reports.
        const std::string writtenBase(CanonicalizeType(LastSegmentOf(written.typeName)));
        const std::string expectedBase(CanonicalizeType(LastSegmentOf(CleanBaseType(expected.typeName))));

        if (writtenBase.empty() || expectedBase.empty() || writtenBase == expectedBase)
        {
            continue;
        }
        if (NamesATypedef(writtenBase, table) || NamesATypedef(expectedBase, table))
        {
            continue;
        }
        return true;
    }

    return false;
}

std::optional<Symbol> FindFuncdefSymbol(std::string_view typeName, const SymbolTable& table)
{
    if (typeName.empty() || parser::primitives::IsPrimitive(typeName))
    {
        return std::nullopt;
    }

    if (const auto bucket = table.FindSymbolsPtr(typeName))
    {
        for (const auto& sym : *bucket)
        {
            if (sym.type == SymbolType::Funcdef)
            {
                return sym;
            }
        }
    }

    const std::string_view bare = LastScopeSegment(typeName);
    const auto matches = table.FindTypeSymbolsByShortName(bare);
    for (const auto& sym : matches)
    {
        if (sym.type == SymbolType::Funcdef)
        {
            return sym;
        }
    }
    if (const auto bucket = table.FindSymbolsPtr(bare))
    {
        for (const auto& sym : *bucket)
        {
            if (sym.type == SymbolType::Funcdef)
            {
                return sym;
            }
        }
    }
    return std::nullopt;
}

/**
 * @brief Is handle type.
 * @param[in] typeName Type name to inspect or resolve.
 * @param[in] symbolTable Symbol or grammar symbol.
 * @return True if the condition is satisfied; false otherwise.
 */
bool IsHandleType(std::string_view typeName, const SymbolTable& symbolTable) noexcept
{
    if (typeName.empty())
    {
        return false;
    }
    if (IsHandleTypeView(typeName))
    {
        return true;
    }
    const std::string clean = CleanBaseType(typeName);
    return !clean.empty() && FindFuncdefSymbol(clean, symbolTable).has_value();
}

/**
 * @brief Matches candidate function parameters at position against funcdef symbols.
 */
static bool MatchCandidateFuncdef(const SymbolTable& table, std::string_view name, uint32_t position,
                                  FuncdefAgreement& agreement)
{
    if (agreement.conflict)
    {
        return false;
    }
    const auto bucket = table.FindSymbolsPtr(name);
    if (!bucket)
    {
        return false;
    }
    for (const auto& sym : *bucket)
    {
        if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
        {
            continue;
        }
        const auto& parameters = sym.GetFunction().parameters;
        if (position >= parameters.size())
        {
            continue;
        }
        auto funcdef = FindFuncdefSymbol(CleanBaseType(parameters[position].typeName), table);
        if (!funcdef)
        {
            continue;
        }
        if (agreement.sawCandidate && agreement.agreed && agreement.agreed->name != funcdef->name)
        {
            agreement.agreed = std::nullopt;
            agreement.conflict = true;
            return false;
        }
        agreement.agreed = std::move(funcdef);
        agreement.sawCandidate = true;
    }
    return agreement.sawCandidate && !agreement.conflict;
}

/**
 * @brief Finds the target funcdef symbol for the assigned expression of an assignment.
 * @param[in] leftText Source spelling of the assignment left side.
 * @param[in] table Symbol table searched for the assigned variable.
 * @return Funcdef symbol of the variable type, or std::nullopt when none matches.
 */
static std::optional<Symbol> FindAssignmentFuncdefTarget(std::string_view leftText, const SymbolTable& table)
{
    std::string varName = CleanBaseType(std::string(leftText));
    const size_t dot = varName.rfind('.');
    if (dot != std::string::npos)
    {
        varName = varName.substr(dot + 1);
    }
    const auto syms = table.FindSymbolsPtr(varName);
    if (!syms)
    {
        return std::nullopt;
    }
    for (const auto& s : *syms)
    {
        if (s.type == SymbolType::Variable && std::holds_alternative<VariableSignature>(s.signature))
        {
            if (auto funcdef = FindFuncdefSymbol(CleanBaseType(s.GetVariable().typeName), table))
            {
                return funcdef;
            }
        }
    }
    return std::nullopt;
}

/**
 * @brief Resolves target funcdef symbol across possible callee member/function names.
 */
static std::optional<Symbol> FindCallArgumentFuncdef(const SymbolTable& table, std::string_view calleeName,
                                                     uint32_t position)
{
    auto checkCandidate = [&](std::string_view name) -> std::pair<bool, std::optional<Symbol>>
    {
        FuncdefAgreement agreement;
        if (MatchCandidateFuncdef(table, name, position, agreement))
        {
            return {true, agreement.agreed};
        }
        if (agreement.conflict)
        {
            return {true, std::nullopt};
        }
        return {false, std::nullopt};
    };

    auto [done, sym] = checkCandidate(calleeName);
    if (done)
    {
        return sym;
    }

    const size_t dot = calleeName.rfind('.');
    if (dot != std::string_view::npos)
    {
        auto [dotDone, dotSym] = checkCandidate(calleeName.substr(dot + 1));
        if (dotDone)
        {
            return dotSym;
        }
    }

    if (!utils::ContainsScopeSeparator(calleeName))
    {
        auto [scopeDone, scopeSym] = checkCandidate(utils::JoinScopedName(calleeName, calleeName));
        if (scopeDone)
        {
            return scopeSym;
        }
    }

    const std::string_view leaf = utils::LastScopeSegment(calleeName);
    if (leaf != calleeName)
    {
        auto [leafDone, leafSym] = checkCandidate(leaf);
        if (leafDone)
        {
            return leafSym;
        }
    }

    return std::nullopt;
}

std::optional<Symbol> FuncdefTargetOfLambda(const LambdaTargetFact& target, const SymbolTable& table)
{
    switch (target.kind)
    {
    case LambdaTargetKind::DeclaratorType:
        return FindFuncdefSymbol(CleanBaseType(std::string(target.text)), table);
    case LambdaTargetKind::AssignmentLeft:
        return FindAssignmentFuncdefTarget(target.text, table);
    case LambdaTargetKind::CallArgument:
        break;
    case LambdaTargetKind::None:
        return std::nullopt;
    }

    const std::string calleeName = CleanBaseType(std::string(target.text));
    if (target.soleArgument)
    {
        if (auto asConversion = FindFuncdefSymbol(calleeName, table))
        {
            return asConversion;
        }
    }

    return FindCallArgumentFuncdef(table, calleeName, target.position);
}

Symbol FuncdefToFunctionSymbol(const Symbol& funcdefSym, std::string_view callName)
{
    Symbol sym;
    sym.type = SymbolType::Function;
    sym.name = std::string(callName);
    sym.containerName = funcdefSym.containerName;
    sym.containerKind = funcdefSym.containerKind;
    sym.qualifiedName = funcdefSym.qualifiedName;
    sym.fileUri = funcdefSym.fileUri;
    sym.startLine = funcdefSym.startLine;
    sym.startCharacter = funcdefSym.startCharacter;
    sym.endLine = funcdefSym.endLine;
    sym.endCharacter = funcdefSym.endCharacter;

    if (std::holds_alternative<FuncdefSignature>(funcdefSym.signature))
    {
        const auto& fd = funcdefSym.GetFuncdef();
        FunctionSignature fnSig;
        fnSig.returnType = fd.returnType;
        fnSig.returnBaseTypeName = fd.returnBaseTypeName;
        fnSig.returnTemplateName = fd.returnTemplateName;
        fnSig.returnTypeKind = fd.returnTypeKind;
        fnSig.returnIsArray = fd.returnIsArray;
        fnSig.returnIsConst = fd.returnIsConst;
        fnSig.returnHasPrimitiveHandle = fd.returnHasPrimitiveHandle;
        fnSig.returnArrayDepth = fd.returnArrayDepth;
        fnSig.modifiers = fd.modifiers;
        fnSig.parameters = fd.parameters;
        sym.signature = std::move(fnSig);
    }
    return sym;
}

std::optional<Symbol> TryResolveCallableFuncdef(std::string_view name, const Scope* scope, const SymbolTable& table,
                                                uint32_t refByte)
{
    if (!scope || name.empty())
    {
        return std::nullopt;
    }
    const LocalDefinition* def = ResolveInScopeAt(scope, name, refByte);
    if (!def || (def->kind != LocalDefinitionKind::Variable && def->kind != LocalDefinitionKind::Parameter))
    {
        return std::nullopt;
    }
    const std::string clean = CleanBaseType(def->typeName);
    if (clean.empty())
    {
        return std::nullopt;
    }
    auto funcdefSym = FindFuncdefSymbol(clean, table);
    if (!funcdefSym)
    {
        return std::nullopt;
    }
    return FuncdefToFunctionSymbol(*funcdefSym, name);
}

static bool HasListConstructorParam(const ParameterInformation& p)
{
    const std::string norm = StripTypeDecorations(p.baseTypeName.empty() ? p.typeName : p.baseTypeName);
    const bool isInt = (norm == "int" || norm == "uint" || norm == "int64" || norm == "uint64");
    return isInt && (p.isReference || p.modifier == ParameterModifier::In || p.modifier == ParameterModifier::InOut ||
                     p.isInOutRef || IsReferenceTypeView(p.typeName));
}

bool IsListConstructorSignature(const FunctionSignature& fn)
{
    if (fn.parameters.size() == 1)
    {
        return HasListConstructorParam(fn.parameters[0]);
    }
    if (fn.parameters.size() == 2)
    {
        return HasListConstructorParam(fn.parameters[0]) && HasListConstructorParam(fn.parameters[1]);
    }
    return false;
}

bool IsListConstructor(const Symbol& sym)
{
    if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return false;
    }
    return IsListConstructorSignature(sym.GetFunction());
}



} // namespace angel_lsp::analysis
