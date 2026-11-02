#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"
#include <algorithm>
#include <cctype>

namespace angel_lsp::analysis
{
std::string SubstituteTypeParam(std::string_view typeStr, std::string_view paramName, std::string_view concreteType)
{
    if (typeStr.empty() || paramName.empty())
    {
        return std::string(typeStr);
    }
    std::string result;
    size_t i = 0;
    while (i < typeStr.size())
    {
        if (typeStr.substr(i).starts_with(paramName))
        {
            bool leftBoundary =
                (i == 0) || (!isalnum(static_cast<unsigned char>(typeStr[i - 1])) && typeStr[i - 1] != '_');
            size_t nextIdx = i + paramName.size();
            bool rightBoundary = (nextIdx >= typeStr.size()) ||
                                 (!isalnum(static_cast<unsigned char>(typeStr[nextIdx])) && typeStr[nextIdx] != '_');
            if (leftBoundary && rightBoundary)
            {
                result.append(concreteType);
                i = nextIdx;
                continue;
            }
        }
        result.push_back(typeStr[i]);
        ++i;
    }
    return result;
}

bool IsTypeSymbolList(const std::vector<Symbol>& syms) noexcept
{
    for (const auto& s : syms)
    {
        if (s.type == SymbolType::Class || s.type == SymbolType::Interface || s.type == SymbolType::Enum ||
            s.type == SymbolType::Typedef || s.type == SymbolType::Funcdef)
        {
            return true;
        }
    }
    return false;
}

std::optional<std::string> TryQualifyIdentifier(std::string_view ident, std::string_view containerScope,
                                                const SymbolTable& table)
{
    if (parser::primitives::IsPrimitive(ident))
    {
        return std::nullopt;
    }
    std::string candidate;
    candidate.reserve(containerScope.size() + 2 + ident.size());
    candidate.append(containerScope);
    candidate.append("::");
    candidate.append(ident);
    if (const auto syms = table.FindSymbolsPtr(candidate))
    {
        if (IsTypeSymbolList(*syms))
        {
            return candidate;
        }
    }
    return std::nullopt;
}

bool IsColonQualified(std::string_view s, size_t start, size_t end) noexcept
{
    const bool preceded = (start >= 2 && s[start - 1] == ':' && s[start - 2] == ':');
    const bool followed = (end + 1 < s.size() && s[end] == ':' && s[end + 1] == ':');
    return preceded || followed;
}

std::string QualifyTypeInContainer(std::string_view typeStr, std::string_view containerScope, const SymbolTable& table)
{
    if (typeStr.empty() || containerScope.empty())
    {
        return std::string(typeStr);
    }
    std::string result;
    result.reserve(typeStr.size() + containerScope.size() + 2);
    size_t i = 0;
    while (i < typeStr.size())
    {
        const char c = typeStr[i];
        if (!isalpha(static_cast<unsigned char>(c)) && c != '_')
        {
            result.push_back(c);
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < typeStr.size() && (isalnum(static_cast<unsigned char>(typeStr[i])) || typeStr[i] == '_'))
        {
            ++i;
        }
        const std::string_view ident = typeStr.substr(start, i - start);
        if (!IsColonQualified(typeStr, start, i))
        {
            if (auto qualified = TryQualifyIdentifier(ident, containerScope, table))
            {
                result.append(*qualified);
                continue;
            }
        }
        result.append(ident);
    }
    return result;
}

TemplateBinding BindTemplateArguments(const std::string& writtenType, const SymbolTable& table)
{
    TemplateBinding binding;
    const auto decomp = DecomposeTemplateType(writtenType);

    const auto declarations = table.FindSymbolsPtr(LastScopeSegment(decomp.containerName));
    if (!declarations)
    {
        return binding;
    }

    for (const auto& declaration : *declarations)
    {
        if (declaration.type != SymbolType::Class || !std::holds_alternative<ClassSignature>(declaration.signature))
        {
            continue;
        }
        const auto& cls = std::get<ClassSignature>(declaration.signature);
        if (!cls.isTemplate || cls.templateParams.empty())
        {
            continue;
        }

        binding.isTemplate = true;
        binding.parameters = cls.templateParams;
        break;
    }

    if (!binding.isTemplate || !decomp.isTemplate)
    {
        return binding;
    }

    binding.arguments = SplitTemplateArguments(decomp.innerArguments);

    if (!binding.arguments.empty() && binding.arguments.back().empty())
    {
        binding.arguments.pop_back();
    }

    binding.usable = binding.arguments.size() == binding.parameters.size();
    return binding;
}

std::string SubstituteTemplateParameters(std::string_view typeStr, const TemplateBinding& binding)
{
    std::string result(typeStr);
    if (binding.usable)
    {
        for (size_t i = 0; i < binding.parameters.size(); ++i)
        {
            result = SubstituteTypeParam(result, binding.parameters[i], binding.arguments[i]);
        }
    }
    return result;
}

static std::vector<Symbol> CollectOpIndexCandidates(const std::vector<std::string>& hierarchy,
                                                    const SymbolTable& symbolTable)
{
    std::vector<Symbol> candidates;
    for (const auto& typeInHierarchy : hierarchy)
    {
        auto found = symbolTable.FindMemberSymbolPtr(typeInHierarchy, "opIndex");
        if (!found)
        {
            continue;
        }
        for (const auto& sym : *found)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                if (sym.GetFunction().returnType != "void" && sym.GetFunction().parameters.size() == 1)
                {
                    candidates.push_back(sym);
                }
            }
        }
    }
    return candidates;
}

static std::string ApplyTemplateBindingToReturn(std::string ret, const std::string& canonical,
                                                const SymbolTable& symbolTable)
{
    auto binding = BindTemplateArguments(canonical, symbolTable);
    if (binding.usable)
    {
        return CleanExpressionType(SubstituteTemplateParameters(ret, binding));
    }
    auto tmpl = ParseTemplateType(canonical);
    if (tmpl.templateArgs.size() == 1)
    {
        ret = SubstituteTypeParam(ret, "T", tmpl.templateArgs[0]);
    }
    else if (tmpl.templateArgs.size() >= 2)
    {
        ret = SubstituteTypeParam(ret, "T", tmpl.templateArgs.back());
        ret = SubstituteTypeParam(ret, "V", tmpl.templateArgs[1]);
        ret = SubstituteTypeParam(ret, "K", tmpl.templateArgs[0]);
    }
    return CleanExpressionType(ret);
}

std::string FindOpIndexInHierarchy(const std::string& owner, const std::string& canonical,
                                   const SymbolTable& symbolTable, std::string_view indexArgType = "")
{
    auto hierarchy = GetInheritedTypeHierarchy(owner, symbolTable);
    auto candidates = CollectOpIndexCandidates(hierarchy, symbolTable);
    const Symbol* bestSym = nullptr;
    if (!candidates.empty())
    {
        if (!indexArgType.empty())
        {
            auto match = ResolveBestOverload(candidates, {std::string(indexArgType)}, symbolTable);
            bestSym = match.bestCandidate;
        }
        if (!bestSym)
        {
            bestSym = &candidates.front();
        }
    }
    if (bestSym)
    {
        return ApplyTemplateBindingToReturn(bestSym->GetFunction().returnType, canonical, symbolTable);
    }
    return "";
}

std::string ResolveIndexFallback(const std::string& canonical)
{
    auto tmpl = ParseTemplateType(canonical);
    if (!tmpl.templateArgs.empty())
    {
        return CleanExpressionType(tmpl.templateArgs.size() >= 2 ? tmpl.templateArgs[1] : tmpl.templateArgs[0]);
    }
    if (canonical.ends_with("[]"))
    {
        return CleanExpressionType(canonical.substr(0, canonical.size() - 2));
    }
    return "";
}

std::string ResolveSingleIndexStep(std::string_view current, const SymbolTable& symbolTable,
                                   std::string_view arrayTypeName, std::string_view indexArgType = "")
{
    const std::string canonical = CanonicalizeArrayType(current, arrayTypeName);
    std::string owner = MemberOwnerType(canonical, arrayTypeName);
    if (owner.empty())
    {
        owner = CleanExpressionType(canonical);
    }

    std::string indexedType = FindOpIndexInHierarchy(owner, canonical, symbolTable, indexArgType);
    if (!indexedType.empty())
    {
        return indexedType;
    }
    return ResolveIndexFallback(canonical);
}

std::string ResolveIndexedTypeWithArg(std::string_view typeName, const SymbolTable& symbolTable,
                                      std::string_view arrayTypeName, std::string_view indexArgType)
{
    return ResolveSingleIndexStep(typeName, symbolTable, arrayTypeName, indexArgType);
}

std::string ResolveIndexedType(std::string_view typeName, size_t indexCount, const SymbolTable& symbolTable,
                               std::string_view arrayTypeName)
{
    std::string current(typeName);
    for (size_t idx = 0; idx < indexCount && !current.empty(); ++idx)
    {
        std::string next = ResolveSingleIndexStep(current, symbolTable, arrayTypeName, "");
        if (next.empty())
        {
            break;
        }
        current = std::move(next);
    }
    return current;
}

/**
 * @brief Strip template decorations.
 * @param[in,out] typeName Type name to inspect or resolve.
 */
void StripTemplateDecorations(std::string_view& typeName)
{
    TrimTypeWhitespace(typeName);
    typeName = StripTrailingDecorationsView(typeName);
    typeName = StripLeadingConst(typeName);
    TrimTypeWhitespace(typeName);
}

TemplateTypeInfo ParseTemplateType(std::string_view typeName)
{
    TemplateTypeInfo info;
    StripTemplateDecorations(typeName);

    const size_t openBracket = typeName.find('<');
    if (openBracket == std::string_view::npos || !typeName.ends_with('>'))
    {
        info.containerName = std::string(typeName);
        return info;
    }

    info.containerName = std::string(typeName.substr(0, openBracket));
    const std::string_view inner = typeName.substr(openBracket + 1, typeName.size() - openBracket - 2);

    for (std::string& argument : SplitTemplateArguments(inner))
    {
        if (!argument.empty())
        {
            info.templateArgs.push_back(std::move(argument));
        }
    }

    return info;
}

/**
 * @brief Checks if a type name should be ignored when comparing template compatibility.
 * @param[in] s Type name to check.
 * @return True if the type name is empty, wildcard, or untyped.
 */
bool IsIgnoredTemplateType(std::string_view s)
{
    return s.empty() || s == "null" || s == "auto" || s == "void";
}

bool AreCompatibleTemplateContainers(std::string_view fromContainer, std::string_view toContainer) noexcept
{
    if (fromContainer == toContainer)
    {
        return true;
    }
    if (utils::ContainsScopeSeparator(fromContainer) && utils::ContainsScopeSeparator(toContainer))
    {
        return false;
    }
    return LastScopeSegment(fromContainer) == LastScopeSegment(toContainer);
}

bool AreCompatibleTemplateArgPair(std::string_view va, std::string_view vb)
{
    TrimTypeWhitespace(va);
    TrimTypeWhitespace(vb);
    if (va == vb)
    {
        return true;
    }
    const bool vaIsTmpl = (va.find('<') != std::string_view::npos && va.ends_with('>'));
    const bool vbIsTmpl = (vb.find('<') != std::string_view::npos && vb.ends_with('>'));
    if (vaIsTmpl && vbIsTmpl)
    {
        return !AreIncompatibleTemplateTypes(va, vb);
    }
    if (utils::ContainsScopeSeparator(va) && utils::ContainsScopeSeparator(vb))
    {
        return false;
    }
    return LastScopeSegment(va) == LastScopeSegment(vb);
}

bool AreCompatibleTemplateArguments(const std::vector<std::string>& fromArgs, const std::vector<std::string>& toArgs)
{
    if (fromArgs.size() != toArgs.size())
    {
        return false;
    }
    for (size_t i = 0; i < fromArgs.size(); ++i)
    {
        if (!AreCompatibleTemplateArgPair(fromArgs[i], toArgs[i]))
        {
            return false;
        }
    }
    return true;
}

bool AreIncompatibleTemplateTypes(std::string_view from, std::string_view to)
{
    StripTemplateDecorations(from);
    StripTemplateDecorations(to);
    if (IsIgnoredTemplateType(from) || IsIgnoredTemplateType(to))
    {
        return false;
    }
    const bool fromIsTmpl = (from.find('<') != std::string_view::npos && from.ends_with('>'));
    const bool toIsTmpl = (to.find('<') != std::string_view::npos && to.ends_with('>'));
    if (fromIsTmpl != toIsTmpl)
    {
        return true;
    }
    if (!fromIsTmpl)
    {
        return false;
    }
    const size_t fromOpen = from.find('<');
    const size_t toOpen = to.find('<');
    if (!AreCompatibleTemplateContainers(from.substr(0, fromOpen), to.substr(0, toOpen)))
    {
        return true;
    }
    const auto fromInner = from.substr(fromOpen + 1, from.size() - fromOpen - 2);
    const auto toInner = to.substr(toOpen + 1, to.size() - toOpen - 2);
    return !AreCompatibleTemplateArguments(SplitTemplateArguments(fromInner), SplitTemplateArguments(toInner));
}

} // namespace angel_lsp::analysis
