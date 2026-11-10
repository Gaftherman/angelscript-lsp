#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

std::string FormatSignatureAttempt(const std::string& targetType, const std::vector<std::string>& argTypes)
{
    std::string sig = targetType + "(";
    for (size_t a = 0; a < argTypes.size(); ++a)
    {
        if (a > 0)
        {
            sig += ", ";
        }
        sig += argTypes[a];
    }
    sig += ")";
    return sig;
}

std::vector<Symbol> LookupRawConstructors(const std::string& baseName, const SymbolTable& table)
{
    std::vector<Symbol> rawConstructors;
    auto collectMemberFunctions = [&](std::string_view scope, std::string_view member)
    {
        if (const auto found = table.FindMemberSymbolPtr(scope, member))
        {
            for (const auto& s : *found)
            {
                if (s.type == SymbolType::Function)
                {
                    rawConstructors.push_back(s);
                }
            }
        }
    };
    auto collectFunctions = [&](std::string_view key)
    {
        if (const auto found = table.FindSymbolsPtr(key))
        {
            for (const auto& s : *found)
            {
                if (s.type == SymbolType::Function)
                {
                    rawConstructors.push_back(s);
                }
            }
        }
    };

    const std::string_view shortName = LastScopeSegment(baseName);
    collectMemberFunctions(baseName, shortName);
    if (shortName != baseName)
    {
        collectMemberFunctions(baseName, baseName);
    }
    if (rawConstructors.empty())
    {
        collectFunctions(baseName);
    }
    // cppcheck-suppress duplicateCondition
    if (rawConstructors.empty())
    {
        for (const auto& sym : table.FindTypeSymbolsByShortName(shortName))
        {
            if (sym.type == SymbolType::Class)
            {
                const std::string_view fullPrefix = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                collectMemberFunctions(fullPrefix, shortName);
                if (fullPrefix != sym.name)
                {
                    collectMemberFunctions(sym.name, shortName);
                }
            }
        }
    }
    return rawConstructors;
}

bool IsClassDeclarationVisible(const std::string& baseName, const SymbolTable& table, const std::string& predefinedExt)
{
    auto checkBucket = [&](const std::vector<Symbol>& syms)
    {
        for (const auto& declaration : syms)
        {
            if (declaration.type == SymbolType::Class &&
                std::holds_alternative<ClassSignature>(declaration.signature) &&
                !utils::IsPredefinedFile(declaration.fileUri, predefinedExt))
            {
                return true;
            }
        }
        return false;
    };

    if (const auto declarations = table.FindSymbolsPtr(baseName))
    {
        if (checkBucket(*declarations))
        {
            return true;
        }
    }
    const std::string_view shortName = LastScopeSegment(baseName);
    return checkBucket(table.FindTypeSymbolsByShortName(shortName));
}

static std::vector<std::string> GetClassTemplateParams(const std::string& baseName, const SymbolTable& table)
{
    std::vector<std::string> templateParams;
    if (auto classSymbols = table.FindSymbolsPtr(baseName))
    {
        for (const auto& cs : *classSymbols)
        {
            if (cs.type == SymbolType::Class && std::holds_alternative<ClassSignature>(cs.signature))
            {
                templateParams = cs.GetClass().templateParams;
                break;
            }
        }
    }
    if (templateParams.empty())
    {
        for (const auto& cs : table.FindTypeSymbolsByShortName(LastScopeSegment(baseName)))
        {
            if (cs.type == SymbolType::Class && std::holds_alternative<ClassSignature>(cs.signature))
            {
                templateParams = cs.GetClass().templateParams;
                break;
            }
        }
    }
    if (templateParams.empty())
    {
        templateParams.push_back("T");
    }
    return templateParams;
}

static void SpecializeConstructorParam(ParameterInformation& param, const std::string& paramName,
                                       const std::string& concreteArg)
{
    if (param.baseTypeName == paramName)
    {
        param.baseTypeName = concreteArg;
    }
    size_t pos = 0;
    while ((pos = param.typeName.find(paramName, pos)) != std::string::npos)
    {
        const bool beforeOk = (pos == 0 || !isalnum(static_cast<unsigned char>(param.typeName[pos - 1])));
        const bool afterOk = (pos + paramName.size() >= param.typeName.size() ||
                              !isalnum(static_cast<unsigned char>(param.typeName[pos + paramName.size()])));
        if (beforeOk && afterOk)
        {
            param.typeName.replace(pos, paramName.size(), concreteArg);
            pos += concreteArg.size();
        }
        else
        {
            pos += paramName.size();
        }
    }
}

std::vector<Symbol> SpecializeConstructors(const std::vector<Symbol>& rawConstructors, const TemplateTypeInfo& tmplInfo,
                                           const std::string& baseName, const SymbolTable& table)
{
    if (tmplInfo.templateArgs.empty())
    {
        return rawConstructors;
    }

    const std::vector<std::string> templateParams = GetClassTemplateParams(baseName, table);
    std::vector<Symbol> candidates;

    for (const auto& sym : rawConstructors)
    {
        Symbol specSym = sym;
        if (std::holds_alternative<FunctionSignature>(specSym.signature))
        {
            auto& fn = specSym.GetFunction();
            for (auto& param : fn.parameters)
            {
                for (size_t t = 0; t < templateParams.size() && t < tmplInfo.templateArgs.size(); ++t)
                {
                    SpecializeConstructorParam(param, templateParams[t], tmplInfo.templateArgs[t]);
                }
            }
        }
        candidates.push_back(std::move(specSym));
    }
    return candidates;
}

void CheckConstructorOverload(TSNode argListNode, const std::vector<Symbol>& candidates,
                              const std::vector<std::string>& argTypes, const VarInitContext& vctx)
{
    const uint32_t argCount = static_cast<uint32_t>(argTypes.size());
    const std::vector<const Symbol*> matchingArity = FilterMatchingArityCandidates(candidates, argCount);

    if (matchingArity.empty())
    {
        const TSPoint aStart = ts_node_start_point(argListNode);
        const TSPoint aEnd = ts_node_end_point(argListNode);
        vctx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column}, "as-err-no-matching-constructor",
                             FormatSignatureAttempt(vctx.declaredType, argTypes));
        return;
    }

    auto match = ResolveBestOverload(matchingArity, argTypes, vctx.ctx.request.symbolTable);
    if (match.bestCandidate == nullptr)
    {
        const TSPoint aStart = ts_node_start_point(argListNode);
        const TSPoint aEnd = ts_node_end_point(argListNode);
        vctx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column}, "as-err-no-matching-constructor",
                             FormatSignatureAttempt(vctx.declaredType, argTypes));
    }
}

} // namespace angel_lsp::analysis
