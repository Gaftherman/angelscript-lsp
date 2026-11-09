#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/ClassRuleFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/InterfaceImplementation.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/ClassRules.h"
#include "analysis/types/TypeSanitization.h"
#include "document/Document.h"
#include "spdlog/fmt/fmt.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Reports interface methods a class declares nowhere in its hierarchy.
 */
void CheckInterfaceImplementation(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    if (sig.modifiers.isMixin)
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    if (!HierarchyIsFullyVisible(sym.qualifiedName.empty() ? sym.name : sym.qualifiedName, table))
    {
        return;
    }

    for (const auto& missing : FindMissingInterfaceMethods(sym, table))
    {
        ctx.LogRule("CheckInterfaceImplementation", "as-err-interface-impl-missing", sym);
        ctx.Emit(sym, "as-err-interface-impl-missing", {sym.name, missing.method.name, missing.interfaceName});
    }
}

/**
 * @brief Checks if a method symbol matches a final base function.
 * @param[in] mSym Method symbol being checked for override validity.
 * @param[in] cleanBase Sanitized base class name.
 * @param[in] methodName Name of the method being examined.
 * @param[in] table Symbol table used for member resolution.
 * @return True if method matches a final function in the base.
 */
static bool MatchesFinalBaseFunction(const Symbol& mSym, std::string_view cleanBase, std::string_view methodName,
                                     const SymbolTable& table)
{
    auto basePtr = table.FindMemberSymbolPtr(cleanBase, methodName);
    if (!basePtr)
    {
        return false;
    }
    for (const auto& baseSym : *basePtr)
    {
        if (baseSym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(baseSym.signature))
        {
            continue;
        }
        if (baseSym.GetFunction().modifiers.isFinal && HasSameParameterList(mSym, baseSym))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if an accessor matches a final base property accessor.
 * @param[in] cleanBase Sanitized base class name.
 * @param[in] methodName Name of the accessor method.
 * @param[in] table Symbol table used for member resolution.
 * @return True if accessor matches a final property in the base.
 */
static bool MatchesFinalBaseProperty(std::string_view cleanBase, std::string_view methodName, const SymbolTable& table)
{
    if (methodName.size() <= 4)
    {
        return false;
    }
    const bool isGet = methodName.starts_with("get_");
    const bool isSet = methodName.starts_with("set_");
    if (!isGet && !isSet)
    {
        return false;
    }

    const std::string_view propName = methodName.substr(4);
    auto basePropPtr = table.FindMemberSymbolPtr(cleanBase, propName);
    if (!basePropPtr)
    {
        return false;
    }

    for (const auto& bp : *basePropPtr)
    {
        if (bp.type != SymbolType::Property || !std::holds_alternative<VariableSignature>(bp.signature))
        {
            continue;
        }
        const auto& varSig = std::get<VariableSignature>(bp.signature);
        if ((isGet && varSig.isGetFinal) || (isSet && varSig.isSetFinal))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if a method symbol matches any final base method or property accessor candidate.
 * @param[in] mSym Method symbol being checked.
 * @param[in] cleanBase Sanitized base class name.
 * @param[in] methodName Name of the method or accessor.
 * @param[in] table Symbol table used for member resolution.
 * @return True if method matches any final base member.
 */
static bool MatchesFinalBaseMethod(const Symbol& mSym, std::string_view cleanBase, std::string_view methodName,
                                   const SymbolTable& table)
{
    return MatchesFinalBaseFunction(mSym, cleanBase, methodName, table) ||
           MatchesFinalBaseProperty(cleanBase, methodName, table);
}

/**
 * @brief Checks if a container declares a property matching a final accessor.
 * @param[in] container Name of the containing class or interface.
 * @param[in] methodName Accessor method name (e.g. get_X or set_X).
 * @param[in] reportingBase Name of base class for diagnostic emission.
 * @param[in] ctx Diagnostic context.
 */
static void CheckFinalPropertyOverride(std::string_view container, std::string_view methodName,
                                       std::string_view reportingBase, const DiagnosticContext& ctx)
{
    if (methodName.size() <= 4 || (!methodName.starts_with("get_") && !methodName.starts_with("set_")))
    {
        return;
    }

    const std::string_view propName = methodName.substr(4);
    auto propPtr = ctx.request.symbolTable.FindMemberSymbolPtr(container, propName);
    if (!propPtr)
    {
        return;
    }

    const bool isGet = methodName.starts_with("get_");
    for (const auto& pSym : *propPtr)
    {
        if (pSym.fileUri != ctx.request.fileUri || pSym.isSynthesized || pSym.type != SymbolType::Property)
        {
            continue;
        }
        if (std::holds_alternative<VariableSignature>(pSym.signature))
        {
            const auto& varSig = std::get<VariableSignature>(pSym.signature);
            if ((isGet && varSig.hasGet) || (!isGet && varSig.hasSet))
            {
                ctx.LogRule("CheckFinalOverrides", "as-err-override-final-method", pSym);
                ctx.Emit(pSym, "as-err-override-final-method", methodName, reportingBase);
            }
        }
    }
}

/**
 * @brief Reports methods overriding final methods of a base class or its mixins.
 * @param[in] container Name of the derived container class.
 * @param[in] cleanBase Sanitized base class name.
 * @param[in] reportingBase Name of base class reported in diagnostics.
 * @param[in] ctx Diagnostic context.
 */
static void ReportFinalMethodOverrides(std::string_view container, std::string_view cleanBase,
                                       std::string_view reportingBase, const DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    for (const auto& methodName : ctx.request.GetRuleIndex().Members(cleanBase).finalMethodNames)
    {
        if (auto methodsPtr = table.FindMemberSymbolPtr(container, methodName))
        {
            for (const auto& mSym : *methodsPtr)
            {
                if (mSym.fileUri != ctx.request.fileUri || mSym.isSynthesized || mSym.type != SymbolType::Function)
                {
                    continue;
                }
                if (MatchesFinalBaseMethod(mSym, cleanBase, methodName, table))
                {
                    ctx.LogRule("CheckFinalOverrides", "as-err-override-final-method", mSym);
                    ctx.Emit(mSym, "as-err-override-final-method", methodName, reportingBase);
                }
            }
        }

        CheckFinalPropertyOverride(container, methodName, reportingBase, ctx);
    }
}

/**
 * @brief Reports a method that replaces one a visible base class declared final.
 */
void CheckFinalOverrides(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    const std::string_view container =
        sym.qualifiedName.empty() ? std::string_view(sym.name) : std::string_view(sym.qualifiedName);

    ankerl::unordered_dense::set<std::string_view> visited;
    std::vector<std::string_view> queue;
    for (const auto& baseName : sig.bases)
    {
        const std::string_view cleanBase = CleanBaseTypeView(baseName);
        if (!cleanBase.empty() && !IsMixinClass(cleanBase, table))
        {
            queue.push_back(cleanBase);
        }
    }

    while (!queue.empty())
    {
        const std::string_view currentBase = queue.back();
        queue.pop_back();

        if (!visited.insert(currentBase).second)
        {
            continue;
        }

        ReportFinalMethodOverrides(container, currentBase, currentBase, ctx);

        if (auto baseSyms = table.FindSymbolsPtr(currentBase))
        {
            for (const auto& bSym : *baseSyms)
            {
                if (bSym.type == SymbolType::Class && std::holds_alternative<ClassSignature>(bSym.signature))
                {
                    const auto& bClass = bSym.GetClass();
                    for (const auto& mixinName : bClass.includedMixins)
                    {
                        ReportFinalMethodOverrides(container, mixinName, currentBase, ctx);
                    }
                    for (const auto& nextBase : bClass.bases)
                    {
                        const std::string_view cleanNext = CleanBaseTypeView(nextBase);
                        if (!cleanNext.empty() && !IsMixinClass(cleanNext, table))
                        {
                            queue.push_back(cleanNext);
                        }
                    }
                }
            }
        }
    }
}

} // namespace angel_lsp::analysis::rules
