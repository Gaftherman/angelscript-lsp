#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/InterfaceImplementation.h"
#include "analysis/resolution/MixinInclusion.h"
#include "analysis/resolution/ReferenceCounting.h"
#include "analysis/resolution/ReferenceCountingScope.h"
#include "features/code_lens/CodeLensHandlerInternal.h"

#include <algorithm>
#include <vector>

namespace angel_lsp::features
{
static bool IsSymbolTypeEnabledForCodeLens(const analysis::Symbol& sym, const config::ServerConfig* config)
{
    if (!config)
    {
        return true;
    }
    if (sym.type == analysis::SymbolType::Function)
    {
        return config->features.codeLensShowOnFunctions;
    }
    if (sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface ||
        sym.type == analysis::SymbolType::Funcdef)
    {
        return config->features.codeLensShowOnClassesAndInterfaces;
    }
    if (sym.type == analysis::SymbolType::Enum)
    {
        return config->features.codeLensShowOnEnums;
    }
    if (sym.type == analysis::SymbolType::Variable)
    {
        if (std::holds_alternative<analysis::VariableSignature>(sym.signature) && sym.GetVariable().isEnumConstant)
        {
            return config->features.codeLensShowOnEnums;
        }
        return config->features.codeLensShowOnPropertiesAndVariables;
    }
    if (sym.type == analysis::SymbolType::Property)
    {
        return config->features.codeLensShowOnPropertiesAndVariables;
    }
    return true;
}

SymbolGroups CollectSymbolGroups(const CodeLensRequest& request)
{
    SymbolGroups result;
    request.symbolTable.ForEachSymbolInFile(
        request.uri,
        [&]([[maybe_unused]] std::string_view qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (sym.fileUri != request.uri || sym.isSynthesized ||
                    !IsSymbolTypeEnabledForCodeLens(sym, request.config))
                {
                    continue;
                }

                const analysis::SourceRange key{sym.startLine, sym.startCharacter, sym.endLine, sym.endCharacter};
                auto [it, inserted] = result.groups.try_emplace(key, std::vector<analysis::Symbol>{});
                if (inserted)
                {
                    result.rangeOrder.push_back(key);
                }
                it->second.push_back(sym);
            }
        });

    std::sort(result.rangeOrder.begin(), result.rangeOrder.end(),
              [](const analysis::SourceRange& a, const analysis::SourceRange& b)
              {
                  if (a.startLine != b.startLine)
                  {
                      return a.startLine < b.startLine;
                  }
                  return a.startCharacter < b.startCharacter;
              });

    return result;
}

const analysis::Symbol& SelectPrimarySymbol(const std::vector<analysis::Symbol>& symGroup)
{
    for (const auto& s : symGroup)
    {
        if (s.type == analysis::SymbolType::Variable &&
            std::holds_alternative<analysis::VariableSignature>(s.signature) && s.GetVariable().isEnumConstant &&
            s.containerKind == analysis::ContainerKind::Enum)
        {
            return s;
        }
    }
    for (const auto& s : symGroup)
    {
        if (s.type == analysis::SymbolType::Variable &&
            std::holds_alternative<analysis::VariableSignature>(s.signature) && s.GetVariable().isEnumConstant &&
            !s.containerName.empty())
        {
            return s;
        }
    }
    for (const auto& s : symGroup)
    {
        if (!s.isSynthesized)
        {
            return s;
        }
    }
    return symGroup.front();
}

static analysis::ReferenceCountTarget CreateFunctionOrVariableTarget(const analysis::SourceRange& key,
                                                                     const analysis::Symbol& sym,
                                                                     const std::vector<analysis::Symbol>& symGroup,
                                                                     const analysis::MixinClassContext& ctx)
{
    analysis::AccessModifier targetAccess = analysis::AccessModifier::Public;
    bool isFunction = false;
    size_t minArgs = 0;
    size_t maxArgs = 0;
    bool isEnumConstant = false;
    if (std::holds_alternative<analysis::FunctionSignature>(sym.signature))
    {
        const auto& fn = sym.GetFunction();
        targetAccess = fn.modifiers.access;
        isFunction = true;
        maxArgs = fn.parameters.size();
        for (const auto& p : fn.parameters)
        {
            if (p.defaultValue.empty())
            {
                minArgs++;
            }
        }
    }
    else if (std::holds_alternative<analysis::VariableSignature>(sym.signature))
    {
        const auto& var = sym.GetVariable();
        targetAccess = var.modifiers.access;
        isEnumConstant = var.isEnumConstant;
    }

    auto compatibleClasses = analysis::CollectCompatibleClasses(symGroup, sym, targetAccess, ctx);
    auto allDeclRanges = analysis::CollectAllDeclRanges(sym.name, symGroup, compatibleClasses, ctx.symbolTable);

    analysis::ReferenceCountTarget target{
        .key = key,
        .targetName = sym.name,
        .group = symGroup,
        .compatibleClasses = std::move(compatibleClasses),
        .allDeclRanges = std::move(allDeclRanges),
        .targetNamespace = {},
        .targetEnumContainer = analysis::ComputeEnumContainer(sym),
        .isEnumConstant = isEnumConstant,
        .targetAccess = targetAccess,
        .isFunction = isFunction,
        .minArgs = minArgs,
        .maxArgs = maxArgs,
        .seenRefs = {},
    };
    target.targetNamespace =
        analysis::ComputeTargetNamespace(isEnumConstant, target.targetEnumContainer, sym, ctx.symbolTable);
    return target;
}

static analysis::ReferenceCountTarget CreateClassTarget(const analysis::SourceRange& key, const analysis::Symbol& sym,
                                                        const std::vector<analysis::Symbol>& symGroup,
                                                        const analysis::SymbolTable& symbolTable)
{
    analysis::DeclRangeSet allDeclRanges;
    analysis::AddSymbolDeclRanges(symGroup, allDeclRanges);

    analysis::ReferenceCountTarget target{
        .key = key,
        .targetName = sym.name,
        .group = symGroup,
        .compatibleClasses = {},
        .allDeclRanges = std::move(allDeclRanges),
        .targetNamespace = analysis::ComputeTargetNamespace(false, {}, sym, symbolTable),
        .targetEnumContainer = {},
        .isEnumConstant = false,
        .targetAccess = analysis::AccessModifier::Public,
        .isFunction = false,
        .minArgs = 0,
        .maxArgs = 0,
        .seenRefs = {},
    };
    return target;
}

void CollectReferenceTargets(const SymbolGroups& symGroups, const TargetCollectionContext& ctx,
                             std::vector<analysis::ReferenceCountTarget>& targets)
{
    for (const auto& key : symGroups.rangeOrder)
    {
        auto it = symGroups.groups.find(key);
        if (it == symGroups.groups.end() || it->second.empty())
        {
            continue;
        }

        const auto& symGroup = it->second;
        const auto& sym = SelectPrimarySymbol(symGroup);

        if (sym.type == analysis::SymbolType::Function)
        {
            if (analysis::IsInterfaceMethod(sym, ctx.mixinCtx.symbolTable) ||
                analysis::IsDestructorDeclaration(sym, ctx.sourceCode))
            {
                continue;
            }
            ctx.keyToTarget[key] = targets.size();
            targets.push_back(CreateFunctionOrVariableTarget(key, sym, symGroup, ctx.mixinCtx));
        }
        else if (sym.type == analysis::SymbolType::Variable || sym.type == analysis::SymbolType::Property)
        {
            ctx.keyToTarget[key] = targets.size();
            targets.push_back(CreateFunctionOrVariableTarget(key, sym, symGroup, ctx.mixinCtx));
        }
        else if (sym.type == analysis::SymbolType::Class)
        {
            ctx.keyToTarget[key] = targets.size();
            targets.push_back(CreateClassTarget(key, sym, symGroup, ctx.mixinCtx.symbolTable));
        }
    }
}
} // namespace angel_lsp::features
