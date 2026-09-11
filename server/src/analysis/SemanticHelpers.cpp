#include "analysis/SemanticHelpers.h"
#include "analysis/ASTUtils.h"
#include "analysis/OverloadResolver.h"
#include "analysis/SymbolTable.h"
#include "analysis/DiagnosticContext.h"
#include "analysis/rules/RuleIndex.h"
#include "utils/Utils.h"
#include "parser/Keywords.h"

#include <optional>
#include "parser/GrammarNames.h"
#include "utils/LspLogger.h"
#include <spdlog/fmt/fmt.h>

namespace angel_lsp::analysis
{
    std::string CleanExpressionType(std::string_view typeName)
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
        while (!typeName.empty() && (typeName.back() == '&' || typeName.back() == ' ' || typeName.back() == '\t'))
        {
            typeName.remove_suffix(1);
        }
        return std::string(typeName);
    }

    std::string GetNodeText(TSNode node, std::string_view sourceCode)
    {
        if (ts_node_is_null(node) || sourceCode.empty())
        {
            return "";
        }
        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);
        if (start < sourceCode.size() && end <= sourceCode.size() && start < end)
        {
            return std::string(sourceCode.substr(start, end - start));
        }
        return "";
    }

    bool IsReservedKeyword(const std::string &name)
    {
        // The list this used to hold was measured against the compiler word by word and came back
        // exactly right - 53, no additions, no removals. It moved to parser/Keywords.h so the
        // formatter and completion could stop keeping their own, differing, copies.
        return parser::keywords::IsReserved(name);
    }

    bool IsKeyword(std::string_view word) noexcept
    {
        return parser::keywords::IsKeyword(word);
    }

    bool IsPrimitiveTypeName(const std::string &name)
    {
        return IsCorePrimitive(name);
    }

    InitializerItemKind ClassifyInitializerItem(std::string_view item)
    {
        if (item.empty())
        {
            return InitializerItemKind::NumericOrExpression;
        }

        if (item.starts_with("\"") || item.starts_with("'"))
        {
            return InitializerItemKind::StringLiteral;
        }

        if (item == "true" || item == "false")
        {
            return InitializerItemKind::BooleanLiteral;
        }

        if (item == "null")
        {
            return InitializerItemKind::NullLiteral;
        }

        if (item.starts_with("{"))
        {
            return InitializerItemKind::NestedInitializer;
        }

        return InitializerItemKind::NumericOrExpression;
    }

    bool IsFromPredefinedStub(const Symbol &sym, const DiagnosticContext &ctx)
    {
        return utils::IsPredefinedFile(sym.fileUri, ctx.request.predefinedFileExtension);
    }

    bool IsDestructorSymbol(const Symbol &sym)
    {
        if (sym.type != SymbolType::Function)
        {
            return false;
        }
        return sym.name.starts_with("~");
    }

    bool IsDestructorDeclaration(const Symbol &sym, const DiagnosticContext &ctx)
    {
        const std::string_view source = ctx.request.sourceCode;
        if (source.empty() || sym.fileUri != ctx.request.fileUri)
        {
            return false;
        }

        // Walk to the start of the declaration's line, then to the identifier.
        size_t offset = 0;
        for (uint32_t line = 0; line < sym.selectionRange.startLine; ++line)
        {
            offset = source.find('\n', offset);
            if (offset == std::string_view::npos)
            {
                return false;
            }
            ++offset;
        }

        offset += sym.selectionRange.startCharacter;
        if (offset == 0 || offset > source.size())
        {
            return false;
        }

        size_t back = offset;
        while (back > 0 && (source[back - 1] == ' ' || source[back - 1] == '\t'))
        {
            --back;
        }
        return back > 0 && source[back - 1] == '~';
    }

    std::string_view FirstAttributeName(const SymbolModifiers &modifiers)
    {
        // Grammar order: choice("override", "final", "explicit", "property", "delete").
        if (modifiers.isOverride) { return "override"; }
        if (modifiers.isFinal)    { return "final"; }
        if (modifiers.isExplicit) { return "explicit"; }
        if (modifiers.isProperty) { return "property"; }
        if (modifiers.isDelete)   { return "delete"; }
        return {};
    }

    bool NamesAFunctionNotAType(std::string_view name, const SymbolTable &table)
    {
        if (name.empty())
        {
            return false;
        }

        const auto symsPtr = table.FindSymbolsPtr(std::string(name));
        if (!symsPtr)
        {
            // Resolves to nothing. That is an unresolved type, assumed engine-registered, and not
            // this rule's business - see the header.
            return false;
        }

        bool isFunction = false;
        for (const auto &sym : *symsPtr)
        {
            switch (sym.type)
            {
            case SymbolType::Function:
                // A method is reached through its container and cannot be written bare in a type
                // position, so it says nothing about what the user meant here.
                if (sym.containerName.empty())
                {
                    isFunction = true;
                }
                break;

            // Anything that CAN stand in a type position settles it: the name is a type, whatever
            // else it also is. Funcdef in particular - a funcdef and a function may share a name,
            // and then `Foo@` is already valid.
            case SymbolType::Class:
            case SymbolType::Interface:
            case SymbolType::Enum:
            case SymbolType::Typedef:
            case SymbolType::Funcdef:
                return false;

            default:
                break;
            }
        }

        return isFunction;
    }

    bool IsBareDataType(
        TSNode node,
        const Scope *scope,
        const SymbolTable &symbolTable,
        std::string_view sourceCode,
        std::string &outTypeName)
    {
        if (ts_node_is_null(node))
        {
            return false;
        }

        std::string_view nodeType = ts_node_type(node);
        if (nodeType != parser::nodes::Identifier && nodeType != parser::nodes::ScopedIdentifier)
        {
            return false;
        }

        std::string name = GetNodeText(node, sourceCode);
        while (!name.empty() && isspace(static_cast<unsigned char>(name.front())))
        {
            name.erase(name.begin());
        }
        while (!name.empty() && isspace(static_cast<unsigned char>(name.back())))
        {
            name.pop_back();
        }
        if (name.empty())
        {
            return false;
        }

        if (IsPrimitiveTypeName(name))
        {
            outTypeName = name;
            return true;
        }

        if (scope)
        {
            if (const LocalDefinition *def = ResolveInScope(scope, name))
            {
                if (def->kind == LocalDefinitionKind::Type)
                {
                    outTypeName = name;
                    return true;
                }
                return false;
            }
        }

        auto syms = symbolTable.FindSymbols(name);
        if (syms.empty())
        {
            std::string shortName = LastScopeSegment(name);
            if (shortName != name)
            {
                syms = symbolTable.FindSymbols(shortName);
            }
        }
        if (syms.empty())
        {
            auto typeSyms = symbolTable.FindTypeSymbolsByShortName(LastScopeSegment(name));
            if (!typeSyms.empty())
            {
                syms = std::move(typeSyms);
            }
        }

        if (syms.empty())
        {
            return false;
        }

        for (const auto &s : syms)
        {
            if (s.type == SymbolType::Variable ||
                s.type == SymbolType::Function ||
                s.type == SymbolType::Property)
            {
                return false;
            }
        }

        for (const auto &s : syms)
        {
            if (s.type == SymbolType::Class ||
                s.type == SymbolType::Interface ||
                s.type == SymbolType::Enum ||
                s.type == SymbolType::Typedef ||
                s.type == SymbolType::Funcdef)
            {
                outTypeName = name;
                return true;
            }
        }

        return false;
    }

    bool IsMixinClass(std::string_view baseTypeName, const SymbolTable &table)
    {
        if (baseTypeName.empty())
        {
            return false;
        }

        std::string searchName(baseTypeName);
        auto symsPtr = table.FindSymbolsPtr(searchName);
        if (!symsPtr)
        {
            return false;
        }

        for (const auto &sym : *symsPtr)
        {
            if (sym.type == SymbolType::Class && sym.GetClass().modifiers.isMixin)
            {
                return true;
            }
        }
        return false;
    }

    NonInstantiableKind ClassifyNonInstantiable(std::string_view baseTypeName, const SymbolTable &table)
    {
        if (baseTypeName.empty())
        {
            return NonInstantiableKind::None;
        }

        auto symsPtr = table.FindSymbolsPtr(std::string(baseTypeName));
        if (!symsPtr)
        {
            return NonInstantiableKind::None;
        }

        for (const auto &sym : *symsPtr)
        {
            if (sym.type == SymbolType::Interface)
            {
                return NonInstantiableKind::Interface;
            }
            if (sym.type == SymbolType::Class)
            {
                if (sym.GetClass().modifiers.isMixin)
                {
                    return NonInstantiableKind::Mixin;
                }
                if (sym.GetClass().modifiers.isAbstract)
                {
                    return NonInstantiableKind::Abstract;
                }
            }
        }
        return NonInstantiableKind::None;
    }

    bool IsKnownType(const std::string &baseName, const DiagnosticContext &ctx)
    {
        if (ctx.logger && ctx.logger->IsTraceEnabled())
        {
            ctx.logger->LogTrace(fmt::format("[SemanticHelpers] IsKnownType: checking '{}'", baseName));
        }
        if (baseName.empty()) return true;
        if (IsCorePrimitive(baseName)) return true;
        if (!ctx.request.GetStringTypeName().empty() && baseName == ctx.request.GetStringTypeName()) return true;
        if (!ctx.request.GetArrayTypeName().empty() && baseName == ctx.request.GetArrayTypeName()) return true;
        if (ctx.request.IsRegisteredSymbol(baseName)) return true;
        if (ctx.request.symbolTable.HasSymbolAnywhere(baseName)) return true;
        return false;
    }

    std::vector<std::string> SplitTemplateArguments(std::string_view inner)
    {
        std::vector<std::string> arguments;
        int depth = 0;
        size_t start = 0;

        const auto push = [&](std::string_view piece)
        {
            while (!piece.empty() && (piece.front() == ' ' || piece.front() == '	')) piece.remove_prefix(1);
            while (!piece.empty() && (piece.back() == ' ' || piece.back() == '	')) piece.remove_suffix(1);
            arguments.emplace_back(piece);
        };

        for (size_t i = 0; i < inner.size(); ++i)
        {
            if (inner[i] == '<')
                ++depth;
            else if (inner[i] == '>')
                --depth;
            else if (inner[i] == ',' && depth == 0)
            {
                push(inner.substr(start, i - start));
                start = i + 1;
            }
        }

        if (start <= inner.size())
            push(inner.substr(start));

        return arguments;
    }

    std::string LastScopeSegment(const std::string &name)
    {
        const size_t pos = name.rfind("::");
        return pos == std::string::npos ? name : name.substr(pos + 2);
    }

    std::string CleanBaseType(std::string_view typeName)
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
            else if (result.ends_with("[]"))
            {
                result.resize(result.size() - 2);
                modified = true;
            }
            else if (!result.empty() && result.back() == ']')
            {
                size_t bracket = result.rfind('[');
                if (bracket != std::string::npos)
                {
                    result = result.substr(0, bracket);
                    modified = true;
                }
            }
        }

        if (result.starts_with("array<") && result.ends_with(">"))
        {
            std::string inner = result.substr(6, result.size() - 7);
            return CleanBaseType(inner);
        }

        return result;
    }

    std::string CanonicalizeArrayType(std::string_view typeName, std::string_view arrayTypeName)
    {
        std::string s(typeName);

        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '@' || s.back() == '&')) s.pop_back();

        if (s.starts_with("const "))
        {
            s = s.substr(6);
        }

        if (arrayTypeName.empty())
        {
            return s;
        }

        // The element of `int[][]` is `int[]`, so the element is canonicalised before it is
        // wrapped: `array<array<int>>`. Wrapping first would give `array<int[]>`, which is the
        // same type spelled in a way nothing else here recognises.
        while (s.ends_with("[]"))
        {
            const std::string element = CanonicalizeArrayType(s.substr(0, s.size() - 2), arrayTypeName);
            s = std::string(arrayTypeName) + "<" + element + ">";
        }

        return s;
    }

    std::string MemberOwnerType(std::string_view typeName, std::string_view arrayTypeName)
    {
        const std::string canonical = CanonicalizeArrayType(typeName, arrayTypeName);

        // A template instantiation's members are declared on the template, so `array<int>` reaches
        // `array::length`. Split on the *first* `<`, which is also the outermost one.
        if (canonical.ends_with(">"))
        {
            const size_t open = canonical.find('<');
            if (open != std::string::npos && open > 0)
            {
                std::string container = canonical.substr(0, open);
                while (!container.empty() && (container.back() == ' ' || container.back() == '\t'))
                {
                    container.pop_back();
                }
                if (!container.empty())
                {
                    return container;
                }
            }
        }

        return CleanBaseType(canonical);
    }

    std::string CanonicalizeType(std::string_view typeName)
    {
        std::string clean = CleanExpressionType(typeName);
        if (clean == "int32")
        {
            return "int";
        }
        if (clean == "uint32")
        {
            return "uint";
        }
        if (clean == "short")
        {
            return "int16";
        }
        if (clean == "ushort")
        {
            return "uint16";
        }
        return clean;
    }

    bool ResolvesToEnum(std::string_view typeName, const SymbolTable &table)
    {
        if (typeName.empty())
        {
            return false;
        }
        std::string bare = LastScopeSegment(std::string(typeName));
        for (const auto &candidate : { std::string(typeName), bare })
        {
            const auto bucket = table.FindSymbolsPtr(candidate);
            if (bucket)
            {
                for (const auto &sym : *bucket)
                {
                    if (sym.type == SymbolType::Enum)
                    {
                        return true;
                    }
                }
            }
            if (bare == typeName)
            {
                break;
            }
        }
        return false;
    }

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
                bool leftBoundary = (i == 0) || (!isalnum(static_cast<unsigned char>(typeStr[i - 1])) && typeStr[i - 1] != '_');
                size_t nextIdx = i + paramName.size();
                bool rightBoundary = (nextIdx >= typeStr.size()) || (!isalnum(static_cast<unsigned char>(typeStr[nextIdx])) && typeStr[nextIdx] != '_');
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

    TemplateBinding BindTemplateArguments(const std::string &writtenType, const SymbolTable &table)
    {
        TemplateBinding binding;

        const size_t open = writtenType.find('<');
        const std::string name = (open == std::string::npos) ? writtenType : writtenType.substr(0, open);

        const auto declarations = table.FindSymbolsPtr(LastScopeSegment(name));
        if (!declarations)
        {
            return binding;
        }

        for (const auto &declaration : *declarations)
        {
            if (declaration.type != SymbolType::Class ||
                !std::holds_alternative<ClassSignature>(declaration.signature))
            {
                continue;
            }
            const auto &cls = std::get<ClassSignature>(declaration.signature);
            if (!cls.isTemplate || cls.templateParams.empty())
            {
                continue;
            }

            binding.isTemplate = true;
            binding.parameters = cls.templateParams;
            break;
        }

        if (!binding.isTemplate || open == std::string::npos || !writtenType.ends_with('>'))
        {
            return binding;
        }

        const std::string inner = writtenType.substr(open + 1, writtenType.size() - open - 2);
        binding.arguments = SplitTemplateArguments(inner);

        if (!binding.arguments.empty() && binding.arguments.back().empty())
        {
            binding.arguments.pop_back();
        }

        binding.usable = binding.arguments.size() == binding.parameters.size();
        return binding;
    }

    std::string SubstituteTemplateParameters(std::string_view typeStr, const TemplateBinding &binding)
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

    std::string ResolveIndexedType(
        std::string_view typeName,
        size_t indexCount,
        const SymbolTable &symbolTable,
        std::string_view arrayTypeName)
    {
        std::string current(typeName);
        for (size_t idx = 0; idx < indexCount && !current.empty(); ++idx)
        {
            std::string canonical = CanonicalizeArrayType(current, arrayTypeName);
            std::string owner = MemberOwnerType(canonical, arrayTypeName);
            if (owner.empty())
            {
                owner = CleanExpressionType(canonical);
            }

            std::string indexedType;
            auto hierarchy = GetInheritedTypeHierarchy(owner, symbolTable);
            for (const auto &typeInHierarchy : hierarchy)
            {
                auto found = symbolTable.FindSymbols(typeInHierarchy + "::opIndex");
                for (const auto &sym : found)
                {
                    if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
                    {
                        std::string ret = sym.GetFunction().returnType;
                        auto binding = BindTemplateArguments(canonical, symbolTable);
                        if (binding.usable)
                        {
                            ret = SubstituteTemplateParameters(ret, binding);
                        }
                        else
                        {
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
                        }
                        indexedType = CleanExpressionType(ret);
                        break;
                    }
                }
                if (!indexedType.empty())
                {
                    break;
                }
            }

            if (!indexedType.empty())
            {
                current = indexedType;
                continue;
            }

            // Fallback for stubless environments or native bracket types
            auto tmpl = ParseTemplateType(canonical);
            if (!tmpl.templateArgs.empty())
            {
                if (tmpl.templateArgs.size() >= 2)
                {
                    current = CleanExpressionType(tmpl.templateArgs[1]);
                }
                else
                {
                    current = CleanExpressionType(tmpl.templateArgs[0]);
                }
            }
            else if (canonical.ends_with("[]"))
            {
                current = CleanExpressionType(canonical.substr(0, canonical.size() - 2));
            }
            else
            {
                break;
            }
        }
        return current;
    }

    TemplateTypeInfo ParseTemplateType(std::string_view typeName)
    {
        TemplateTypeInfo info;
        while (!typeName.empty() && (typeName.front() == ' ' || typeName.front() == '\t'))
        {
            typeName.remove_prefix(1);
        }
        while (!typeName.empty() && (typeName.back() == ' ' || typeName.back() == '\t' || typeName.back() == '@' || typeName.back() == '&'))
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

        size_t openBracket = typeName.find('<');
        if (openBracket == std::string_view::npos || !typeName.ends_with('>'))
        {
            info.containerName = std::string(typeName);
            return info;
        }

        info.containerName = std::string(typeName.substr(0, openBracket));
        std::string_view inner = typeName.substr(openBracket + 1, typeName.size() - openBracket - 2);

        // Empty pieces are dropped here: `array<int,>` names one argument, and the trailing
        // nothing is a typo rather than a second one.
        for (std::string &argument : SplitTemplateArguments(inner))
        {
            if (!argument.empty())
                info.templateArgs.push_back(std::move(argument));
        }

        return info;
    }

    std::string PropertyNameFromAccessor(const Symbol &sym, bool keywordRequired)
    {
        if (!std::holds_alternative<FunctionSignature>(sym.signature))
            return {};

        if (keywordRequired && !sym.GetFunction().modifiers.isProperty)
            return {};

        for (const std::string_view prefix : { std::string_view("get_"), std::string_view("set_") })
        {
            if (sym.name.size() > prefix.size() && sym.name.compare(0, prefix.size(), prefix) == 0)
                return sym.name.substr(prefix.size());
        }

        return {};
    }

    std::vector<Symbol> FindPropertyAccessors(const std::string &typeName,
                                              const std::string &propertyName,
                                              const SymbolTable &symbolTable,
                                              bool keywordRequired)
    {
        std::vector<Symbol> accessors;
        if (typeName.empty() || propertyName.empty())
            return accessors;

        // Getter first, and the whole hierarchy before moving on to the setter, so the type this
        // reports comes from the getter whenever there is one.
        for (const std::string_view prefix : { std::string_view("get_"), std::string_view("set_") })
        {
            for (const auto &owner : GetInheritedTypeHierarchy(typeName, symbolTable))
            {
                for (const auto &sym : symbolTable.FindSymbols(owner + "::" + std::string(prefix) + propertyName))
                {
                    if (!std::holds_alternative<FunctionSignature>(sym.signature))
                        continue;
                    if (keywordRequired && !sym.GetFunction().modifiers.isProperty)
                        continue;

                    accessors.push_back(sym);
                }

                // A derived class overriding the accessor answers for it; the base's copy would only
                // be the same property said twice.
                if (!accessors.empty())
                    break;
            }
        }

        return accessors;
    }

    std::vector<Symbol> FindGlobalPropertyAccessors(const std::string &propertyName,
                                                    const SymbolTable &symbolTable,
                                                    bool keywordRequired)
    {
        std::vector<Symbol> accessors;
        if (propertyName.empty())
        {
            return accessors;
        }

        for (const std::string_view prefix : { std::string_view("get_"), std::string_view("set_") })
        {
            for (const auto &sym : symbolTable.FindSymbols(std::string(prefix) + propertyName))
            {
                if (sym.type != SymbolType::Function || !sym.containerName.empty())
                {
                    continue;
                }
                if (!std::holds_alternative<FunctionSignature>(sym.signature))
                {
                    continue;
                }
                if (keywordRequired && !sym.GetFunction().modifiers.isProperty)
                {
                    continue;
                }

                accessors.push_back(sym);
            }
        }

        return accessors;
    }

    std::string PropertyTypeFromAccessors(const std::vector<Symbol> &accessors)
    {
        for (const auto &sym : accessors)
        {
            if (!std::holds_alternative<FunctionSignature>(sym.signature))
                continue;

            const FunctionSignature &func = sym.GetFunction();
            if (sym.name.compare(0, 4, "get_") == 0)
                return func.returnType;
        }

        for (const auto &sym : accessors)
        {
            if (!std::holds_alternative<FunctionSignature>(sym.signature))
                continue;

            const FunctionSignature &func = sym.GetFunction();
            if (sym.name.compare(0, 4, "set_") == 0 && !func.parameters.empty())
                return func.parameters.front().typeName;
        }

        return {};
    }

    std::vector<std::string> GetInheritedTypeHierarchy(const std::string &className, const SymbolTable &symbolTable)
    {
        std::vector<std::string> hierarchy;
        ankerl::unordered_dense::set<std::string> visited;
        std::vector<std::string> queue;

        std::string rootType = CleanBaseType(className);
        if (rootType.empty())
        {
            return hierarchy;
        }

        visited.insert(rootType);
        queue.push_back(rootType);

        size_t head = 0;
        while (head < queue.size())
        {
            std::string curType = queue[head++];
            hierarchy.push_back(curType);

            // FindSymbolsPtr, not FindSymbols: the latter deep-copies the whole overload bucket,
            // and Symbol is a heavy value type. Nothing here mutates it.
            const auto symbols = symbolTable.FindSymbolsPtr(curType);
            std::vector<Symbol> fallbackSymbols;
            const std::vector<Symbol> *symbolsToIterate = symbols ? symbols.get() : nullptr;
            if (!symbolsToIterate || symbolsToIterate->empty())
            {
                if (curType.find("::") == std::string::npos)
                {
                    fallbackSymbols = symbolTable.FindTypeSymbolsByShortName(curType);
                    symbolsToIterate = &fallbackSymbols;
                }
            }

            for (const auto &sym : (symbolsToIterate ? *symbolsToIterate : std::vector<Symbol>{}))
            {
                if (sym.type == SymbolType::Class)
                {
                    const std::string qName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                    if (visited.insert(qName).second)
                    {
                        hierarchy.push_back(qName);
                    }

                    const auto &cls = sym.GetClass();

                    // Classified once, then partitioned. Mixins must still be enqueued ahead of
                    // ordinary bases - that ordering is what makes a mixin's method take precedence
                    // - but the two passes this used to take each re-cleaned every base name and
                    // re-probed the symbol table through IsMixinClass, so every base was resolved
                    // twice to answer the same question.
                    struct ClassifiedBase
                    {
                        std::string name;
                        bool isMixin = false;
                    };

                    std::vector<ClassifiedBase> bases;
                    bases.reserve(cls.bases.size());

                    for (const auto &base : cls.bases)
                    {
                        std::string cleanBase = CleanBaseType(base);
                        if (cleanBase.empty())
                        {
                            continue;
                        }
                        const bool isMixin = IsMixinClass(cleanBase, symbolTable);
                        bases.push_back(ClassifiedBase{ std::move(cleanBase), isMixin });
                    }

                    for (const bool wantMixins : { true, false })
                    {
                        for (const auto &base : bases)
                        {
                            if (base.isMixin != wantMixins)
                            {
                                continue;
                            }
                            if (visited.insert(base.name).second)
                            {
                                queue.push_back(base.name);
                            }
                        }
                    }
                }
                else if (sym.type == SymbolType::Interface)
                {
                    const auto &iface = sym.GetInterface();
                    for (const auto &base : iface.inheritedInterfaces)
                    {
                        std::string cleanBase = CleanBaseType(base);
                        if (!cleanBase.empty() && visited.insert(cleanBase).second)
                        {
                            queue.push_back(cleanBase);
                        }
                    }
                }
            }
        }

        return hierarchy;
    }

    std::string ResolveBaseClass(std::string_view className, const SymbolTable &symbolTable)
    {
        if (className.empty())
        {
            return "";
        }
        auto hier = GetInheritedTypeHierarchy(std::string(className), symbolTable);
        for (size_t i = 1; i < hier.size(); ++i)
        {
            if (!IsMixinClass(hier[i], symbolTable))
            {
                return hier[i];
            }
        }
        if (hier.size() > 1)
        {
            return hier[1];
        }
        return "";
    }

    bool HierarchyIsFullyVisible(const std::string &typeName, const SymbolTable &symbolTable)
    {
        for (const auto &ancestor : GetInheritedTypeHierarchy(typeName, symbolTable))
        {
            const auto symbols = symbolTable.FindSymbolsPtr(ancestor);
            if (!symbols)
            {
                return false;
            }

            // The hierarchy walk itself stops at a name it cannot resolve, so an unresolvable base
            // never appears in the list above and has to be looked for here, one level down.
            for (const auto &sym : *symbols)
            {
                if (sym.type != SymbolType::Class)
                {
                    continue;
                }
                for (const auto &base : sym.GetClass().bases)
                {
                    if (!symbolTable.FindSymbolsPtr(CleanBaseType(base)))
                    {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    std::vector<std::string> GetAllRelatedClasses(const std::string &className, const SymbolTable &symbolTable)
    {
        std::vector<std::string> related;
        ankerl::unordered_dense::set<std::string> visited;
        std::vector<std::string> queue;

        std::string rootType = CleanBaseType(className);
        if (rootType.empty())
        {
            return related;
        }

        visited.insert(rootType);
        queue.push_back(rootType);

        // 1. Traverse base classes and interfaces
        size_t head = 0;
        while (head < queue.size())
        {
            std::string curType = queue[head++];
            related.push_back(curType);

            auto symbols = symbolTable.FindSymbols(curType);
            for (const auto &sym : symbols)
            {
                if (sym.type == SymbolType::Class)
                {
                    const auto &cls = sym.GetClass();
                    for (const auto &base : cls.bases)
                    {
                        std::string cleanBase = CleanBaseType(base);
                        if (!cleanBase.empty() && visited.insert(cleanBase).second)
                        {
                            queue.push_back(cleanBase);
                        }
                    }
                }
                else if (sym.type == SymbolType::Interface)
                {
                    const auto &iface = sym.GetInterface();
                    for (const auto &base : iface.inheritedInterfaces)
                    {
                        std::string cleanBase = CleanBaseType(base);
                        if (!cleanBase.empty() && visited.insert(cleanBase).second)
                        {
                            queue.push_back(cleanBase);
                        }
                    }
                }
            }
        }

        // 2. Discover derived classes, breadth-first over the reverse inheritance edges.
        //
        // This used to be a fixpoint loop whose every iteration walked the entire symbol table,
        // repeating until no new derived class turned up - O(inheritance depth x whole workspace)
        // per call. Inheritance is written the wrong way round for this question (a class names its
        // bases, not its children), so the index reverses the edges once per table version and the
        // search becomes an ordinary traversal. See RuleIndex::derivedByBase.
        const auto ruleIndex = symbolTable.GetRuleIndex();

        std::vector<std::string> frontier(related.begin(), related.end());
        while (!frontier.empty())
        {
            std::vector<std::string> next;

            for (const auto &baseName : frontier)
            {
                const auto it = ruleIndex->derivedByBase.find(baseName);
                if (it == ruleIndex->derivedByBase.end())
                {
                    continue;
                }

                for (const auto &derived : it->second)
                {
                    if (!visited.insert(derived.qualifiedName).second)
                    {
                        continue;
                    }

                    related.push_back(derived.qualifiedName);
                    next.push_back(derived.qualifiedName);

                    // The unqualified spelling is recorded too - callers look types up by either.
                    if (!derived.name.empty() && derived.name != derived.qualifiedName &&
                        visited.insert(derived.name).second)
                    {
                        related.push_back(derived.name);
                        next.push_back(derived.name);
                    }
                }
            }

            frontier = std::move(next);
        }

        return related;
    }

    std::vector<std::string> GetDerivedClasses(const std::string &className, const SymbolTable &symbolTable)
    {
        std::vector<std::string> derivedClasses;
        std::string rootType = CleanBaseType(className);
        if (rootType.empty())
        {
            return derivedClasses;
        }

        ankerl::unordered_dense::set<std::string> visited;
        visited.insert(rootType);

        const auto ruleIndex = symbolTable.GetRuleIndex();
        if (!ruleIndex)
        {
            return derivedClasses;
        }

        std::vector<std::string> frontier;
        frontier.push_back(rootType);

        while (!frontier.empty())
        {
            std::vector<std::string> next;
            for (const auto &baseName : frontier)
            {
                const auto it = ruleIndex->derivedByBase.find(baseName);
                if (it == ruleIndex->derivedByBase.end())
                {
                    continue;
                }

                for (const auto &derived : it->second)
                {
                    if (visited.insert(derived.qualifiedName).second)
                    {
                        derivedClasses.push_back(derived.qualifiedName);
                        next.push_back(derived.qualifiedName);
                    }

                    if (!derived.name.empty() && derived.name != derived.qualifiedName &&
                        visited.insert(derived.name).second)
                    {
                        derivedClasses.push_back(derived.name);
                        next.push_back(derived.name);
                    }
                }
            }

            frontier = std::move(next);
        }

        return derivedClasses;
    }

    std::vector<std::string> GetCompatibleMemberClasses(
        const std::string &className,
        const std::string &memberName,
        AccessModifier access,
        const SymbolTable &symbolTable)
    {
        std::vector<std::string> result;
        ankerl::unordered_dense::set<std::string> visited;

        auto addClass = [&](const std::string &c)
        {
            if (!c.empty() && visited.insert(c).second)
            {
                result.push_back(c);
                auto colon = c.rfind("::");
                if (colon != std::string::npos)
                {
                    std::string shortName = c.substr(colon + 2);
                    if (visited.insert(shortName).second)
                    {
                        result.push_back(shortName);
                    }
                }
            }
        };

        std::string cleanClass = CleanBaseType(className);
        if (cleanClass.empty())
        {
            return result;
        }

        if (access == AccessModifier::Private)
        {
            addClass(cleanClass);
            if (cleanClass.find("::") == std::string::npos)
            {
                auto typeSyms = symbolTable.FindTypeSymbolsByShortName(cleanClass);
                for (const auto &ts : typeSyms)
                {
                    if ((ts.type == SymbolType::Class || ts.type == SymbolType::Interface) && !ts.qualifiedName.empty())
                    {
                        addClass(ts.qualifiedName);
                    }
                }
            }
            return result;
        }

        // For Protected and Public: check if memberName is declared on an ancestor class or interface.
        std::string rootClass = cleanClass;
        auto hierarchy = GetInheritedTypeHierarchy(cleanClass, symbolTable);
        for (size_t i = 1; i < hierarchy.size(); ++i)
        {
            const auto &ancestor = hierarchy[i];
            auto syms = symbolTable.FindSymbols(ancestor + "::" + memberName);
            bool hasNonPrivate = false;
            for (const auto &s : syms)
            {
                AccessModifier a = AccessModifier::Public;
                if (std::holds_alternative<FunctionSignature>(s.signature))
                {
                    a = s.GetFunction().modifiers.access;
                }
                else if (std::holds_alternative<VariableSignature>(s.signature))
                {
                    a = s.GetVariable().modifiers.access;
                }

                if (a != AccessModifier::Private)
                {
                    hasNonPrivate = true;
                    break;
                }
            }

            if (hasNonPrivate)
            {
                rootClass = ancestor;
            }
        }

        addClass(rootClass);

        auto derived = GetDerivedClasses(rootClass, symbolTable);
        for (const auto &d : derived)
        {
            addClass(d);
        }

        if (access == AccessModifier::Public && rootClass != cleanClass)
        {
            auto rootHierarchy = GetInheritedTypeHierarchy(rootClass, symbolTable);
            for (const auto &base : rootHierarchy)
            {
                addClass(base);
            }
        }

        return result;
    }

    bool IsBaseConstructorCall(TSNode node, std::string_view sourceCode)
    {
        if (ts_node_is_null(node))
        {
            return false;
        }

        // Walk out to the function this sits in. The cap is the same one the rest of the analyzer
        // uses for ancestor walks; a tree deeper than that is malformed, not merely nested.
        TSNode function = ts_node_parent(node);
        int depth = 0;
        while (!ts_node_is_null(function) && ts_node_type(function) != std::string_view("func_declaration"))
        {
            if (++depth > k_maxAstDepth)
            {
                return false;
            }
            function = ts_node_parent(function);
        }

        if (ts_node_is_null(function))
        {
            return false;
        }

        // A constructor is a func_declaration with no return type whose name is the class's. The
        // absence of the field is what distinguishes it - `void Derived()` is a method that happens
        // to share the name and cannot call super.
        if (!ts_node_is_null(parser::GetChildByField(function, parser::fields::ReturnType)))
        {
            return false;
        }

        TSNode functionName = parser::GetChildByField(function, parser::fields::Name);
        if (ts_node_is_null(functionName))
        {
            return false;
        }

        TSNode owner = ts_node_parent(function);
        depth = 0;
        while (!ts_node_is_null(owner) && ts_node_type(owner) != std::string_view("class_declaration"))
        {
            if (++depth > k_maxAstDepth)
            {
                return false;
            }
            owner = ts_node_parent(owner);
        }

        if (ts_node_is_null(owner))
        {
            return false;
        }

        TSNode className = parser::GetChildByField(owner, parser::fields::Name);
        if (ts_node_is_null(className) ||
            GetNodeText(className, sourceCode) != GetNodeText(functionName, sourceCode))
        {
            return false;
        }

        // And the class has to have a base to call up into. `super(...)` in a class that names none
        // is an error the compiler does report, so this stops short of excusing it.
        const uint32_t childCount = ts_node_named_child_count(owner);
        for (uint32_t i = 0; i < childCount; ++i)
        {
            if (ts_node_type(ts_node_named_child(owner, i)) == std::string_view("base_class_list"))
            {
                return true;
            }
        }

        return false;
    }

    std::vector<ContainerInfo> GetEnclosingContainers(TSNode node, std::string_view sourceCode)
    {
        std::vector<ContainerInfo> rawContainers;
        if (ts_node_is_null(node))
        {
            return rawContainers;
        }

        TSNode current = node;

        // If the node itself is the name of a class/interface/namespace declaration (e.g. 'class Player'),
        // we want its enclosing container to be the outer scope, not itself.
        TSNode parent = ts_node_parent(current);
        if (!ts_node_is_null(parent))
        {
            std::string_view parentType = ts_node_type(parent);
            if (parentType == "class_declaration" || parentType == "interface_declaration" ||
                parentType == "namespace_declaration" || parentType == "mixin_declaration")
            {
                TSNode nameChild = parser::GetChildByField(parent, parser::fields::Name);
                if (!ts_node_is_null(nameChild) &&
                    ts_node_start_byte(nameChild) == ts_node_start_byte(current) &&
                    ts_node_end_byte(nameChild) == ts_node_end_byte(current))
                {
                    current = parent;
                }
            }
        }

        current = ts_node_parent(current);

        while (!ts_node_is_null(current))
        {
            std::string_view type = ts_node_type(current);
            ContainerKind kind = ContainerKind::Class;
            bool isContainer = false;

            // A mixin body is a class body for every question these helpers answer - what type
            // encloses this node, what `this` is, which methods an unqualified name can reach.
            // Leaving it out meant a mixin's whole body was treated as though it sat at file scope,
            // which the call-argument audit found: a method calling its own sibling matched an
            // unrelated global of the same name instead.
            if (type == "class_declaration" || type == "mixin_declaration")
            {
                kind = ContainerKind::Class;
                isContainer = true;
            }
            else if (type == "interface_declaration")
            {
                kind = ContainerKind::Interface;
                isContainer = true;
            }
            else if (type == "namespace_declaration")
            {
                kind = ContainerKind::Namespace;
                isContainer = true;
            }

            if (isContainer)
            {
                TSNode nameNode = parser::GetChildByField(current, parser::fields::Name);
                if (!ts_node_is_null(nameNode))
                {
                    uint32_t start = ts_node_start_byte(nameNode);
                    uint32_t end = ts_node_end_byte(nameNode);
                    if (start < sourceCode.size() && end <= sourceCode.size() && start < end)
                    {
                        std::string name(sourceCode.substr(start, end - start));
                        ContainerInfo info;
                        info.name = name;
                        info.kind = kind;
                        rawContainers.push_back(std::move(info));
                    }
                }
            }

            current = ts_node_parent(current);
        }

        // Compute qualified names from outermost to innermost
        for (size_t i = 0; i < rawContainers.size(); ++i)
        {
            std::string qName;
            for (size_t j = rawContainers.size(); j > i; --j)
            {
                if (!qName.empty())
                {
                    qName += "::";
                }
                qName += rawContainers[j - 1].name;
            }
            rawContainers[i].qualifiedName = qName;
        }

        return rawContainers;
    }

    std::vector<std::string> CollectUsingNamespaces(TSNode root, std::string_view sourceCode)
    {
        std::vector<std::string> usings;
        std::vector<TSNode> stack = { root };
        while (!stack.empty())
        {
            TSNode cur = stack.back();
            stack.pop_back();

            std::string_view type = ts_node_type(cur);
            if (type == "using_declaration")
            {
                TSNode nameNode = parser::GetChildByField(cur, parser::fields::Name);
                if (!ts_node_is_null(nameNode))
                {
                    std::string uName = GetNodeText(nameNode, sourceCode);
                    while (!uName.empty() && isspace(static_cast<unsigned char>(uName.front()))) uName.erase(uName.begin());
                    while (!uName.empty() && isspace(static_cast<unsigned char>(uName.back()))) uName.pop_back();
                    if (!uName.empty())
                    {
                        usings.push_back(uName);
                    }
                }
            }

            uint32_t count = ts_node_child_count(cur);
            for (uint32_t i = 0; i < count; ++i)
            {
                stack.push_back(ts_node_child(cur, i));
            }
        }
        return usings;
    }

    bool IsKnownScope(const std::string &prefix, TSNode node, std::string_view sourceCode, const SymbolTable &table)
    {
        if (prefix.empty())
        {
            return true;
        }

        // 1. Check if prefix is directly in table as symbol or qualified name
        if (table.HasSymbol(prefix) || table.HasSymbolAnywhere(prefix))
        {
            return true;
        }

        // 2. Check if any symbol in table has containerName equal to or prefixed with prefix
        bool hasContainer = false;
        table.ForEachSymbol([&](const std::string &qName, const std::vector<Symbol> &syms) {
            if (hasContainer) return;
            for (const auto &s : syms)
            {
                if (s.containerName == prefix || s.containerName.rfind(prefix + "::", 0) == 0 ||
                    s.name == prefix || qName == prefix || qName.rfind(prefix + "::", 0) == 0)
                {
                    hasContainer = true;
                    return;
                }
            }
        });
        if (hasContainer)
        {
            return true;
        }

        // 3. Check relative to enclosing containers
        auto containers = GetEnclosingContainers(node, sourceCode);
        for (const auto &c : containers)
        {
            std::string q = c.qualifiedName + "::" + prefix;
            if (table.HasSymbol(q) || table.HasSymbolAnywhere(q))
            {
                return true;
            }
        }

        // 4. Check relative to using namespaces
        TSNode root = node;
        while (!ts_node_is_null(ts_node_parent(root)))
        {
            root = ts_node_parent(root);
        }
        auto usings = CollectUsingNamespaces(root, sourceCode);
        for (const auto &ns : usings)
        {
            std::string q = ns + "::" + prefix;
            if (table.HasSymbol(q) || table.HasSymbolAnywhere(q))
            {
                return true;
            }
        }

        return false;
    }

    std::vector<Symbol> FindSymbolsInScope(
        const SymbolTable &symbolTable,
        const std::vector<ContainerInfo> &containers,
        const std::string &name,
        const std::vector<std::string> &usingNamespaces)
    {
        if (name.empty())
        {
            return {};
        }

        // Handle explicit global scope operator "::name"
        if (name.rfind("::", 0) == 0)
        {
            std::string globalName = name.substr(2);
            return symbolTable.FindSymbols(globalName);
        }

        // 1. Container hierarchy lookup (from innermost to outermost)
        for (const auto &container : containers)
        {
            if (container.kind == ContainerKind::Class || container.kind == ContainerKind::Interface)
            {
                auto hierarchy = GetInheritedTypeHierarchy(container.qualifiedName, symbolTable);
                for (const auto &cls : hierarchy)
                {
                    std::string qName = cls + "::" + name;
                    auto found = symbolTable.FindSymbols(qName);
                    if (!found.empty())
                    {
                        return found;
                    }
                }

                if (container.name != container.qualifiedName)
                {
                    auto bareHierarchy = GetInheritedTypeHierarchy(container.name, symbolTable);
                    for (const auto &cls : bareHierarchy)
                    {
                        std::string qName = cls + "::" + name;
                        auto found = symbolTable.FindSymbols(qName);
                        if (!found.empty())
                        {
                            return found;
                        }
                    }
                }
            }
            else if (container.kind == ContainerKind::Namespace)
            {
                std::string qName = container.qualifiedName + "::" + name;
                auto found = symbolTable.FindSymbols(qName);
                if (!found.empty())
                {
                    return found;
                }
            }
        }

        // 2. Global lookup
        auto globalFound = symbolTable.FindSymbols(name);
        if (!globalFound.empty())
        {
            return globalFound;
        }

        // 3. using namespace lookup
        for (const auto &ns : usingNamespaces)
        {
            std::string qName = ns + "::" + name;
            auto found = symbolTable.FindSymbols(qName);
            if (!found.empty())
            {
                return found;
            }
        }

        // 4. Enum member fallback via cached RuleIndex (O(1) lookup instead of full table scan)
        if (const auto ruleIndex = symbolTable.GetRuleIndex())
        {
            auto it = ruleIndex->enumSymbolsByMemberName.find(name);
            if (it != ruleIndex->enumSymbolsByMemberName.end())
            {
                return it->second;
            }
        }

        // 5. Short-name type fallback (e.g. CIns2Prop -> INS2PROP::CIns2Prop)
        auto typeMatches = symbolTable.FindTypeSymbolsByShortName(name);
        if (!typeMatches.empty())
        {
            return typeMatches;
        }

        return {};
    }

    std::vector<Symbol> FindSymbolsInScope(
        const std::string &name,
        TSNode node,
        std::string_view sourceCode,
        const SymbolTable &symbolTable)
    {
        std::vector<std::string> usings;
        TSNode p = node;
        while (!ts_node_is_null(p))
        {
            uint32_t count = ts_node_child_count(p);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode ch = ts_node_child(p, i);
                if (std::string_view(ts_node_type(ch)) == "using_declaration")
                {
                    TSNode nameNode = parser::GetChildByField(ch, parser::fields::Name);
                    if (!ts_node_is_null(nameNode))
                    {
                        std::string uName = GetNodeText(nameNode, sourceCode);
                        while (!uName.empty() && isspace(static_cast<unsigned char>(uName.front()))) uName.erase(uName.begin());
                        while (!uName.empty() && isspace(static_cast<unsigned char>(uName.back()))) uName.pop_back();
                        if (!uName.empty())
                        {
                            usings.push_back(uName);
                        }
                    }
                }
            }
            p = ts_node_parent(p);
        }

        std::vector<ContainerInfo> containers = GetEnclosingContainers(node, sourceCode);
        return FindSymbolsInScope(symbolTable, containers, name, usings);
    }

    std::string ResolveExpressionType(
        TSNode exprNode,
        const Scope *scope,
        const SymbolTable &symbolTable,
        std::string_view sourceCode,
        std::string_view uri,
        int depth)
    {
        // This resolver recurses on operands and member chains with nothing else bounding it, so a
        // deeply nested expression walked the stack down until it ran out. Returning empty is the
        // established "cannot see enough to judge" answer everywhere in this file, and every caller
        // already treats it as "stay silent" - so a truncated resolution costs a diagnostic, never
        // a wrong one. See k_maxAstDepth in ASTUtils.h.
        if (depth > k_maxAstDepth)
        {
            return "";
        }

        if (ts_node_is_null(exprNode) || sourceCode.empty())
        {
            return "";
        }

        std::string rawText = GetNodeText(exprNode, sourceCode);
        while (!rawText.empty() && isspace(static_cast<unsigned char>(rawText.front()))) rawText.erase(rawText.begin());
        while (!rawText.empty() && isspace(static_cast<unsigned char>(rawText.back()))) rawText.pop_back();

        if (rawText == "void")
        {
            return "void";
        }

        if (!rawText.empty() && rawText.front() == '{' && rawText.back() == '}')
        {
            return "init_list";
        }

        std::string_view nodeType = ts_node_type(exprNode);

        // Parenthesized expression (e.g. (expr))
        if (nodeType == "parenthesized_expression")
        {
            const uint32_t namedCount = ts_node_named_child_count(exprNode);
            for (uint32_t i = 0; i < namedCount; ++i)
            {
                TSNode child = ts_node_named_child(exprNode, i);
                std::string res = ResolveExpressionType(child, scope, symbolTable, sourceCode, uri, depth + 1);
                if (!res.empty())
                {
                    return res;
                }
            }
            const uint32_t count = ts_node_child_count(exprNode);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(exprNode, i);
                std::string_view cType = ts_node_type(child);
                if (cType != "(" && cType != ")")
                {
                    std::string res = ResolveExpressionType(child, scope, symbolTable, sourceCode, uri, depth + 1);
                    if (!res.empty())
                    {
                        return res;
                    }
                }
            }
            return "";
        }

        // `this` expression
        if (nodeType == "this_expression")
        {
            // The class's QUALIFIED name, not its simple one. A class declared inside a namespace
            // is collected as `NS::Class` and its methods as `NS::Class::member`, so answering
            // `Class` here sent every member lookup to `Class::member` - a name nothing declares.
            //
            // Containers arrive innermost first, so the class is found first and each namespace
            // outside it is prepended in turn.
            std::string qualified;
            for (const auto &container : GetEnclosingContainers(exprNode, sourceCode))
            {
                if (qualified.empty())
                {
                    if (container.kind == ContainerKind::Class)
                    {
                        qualified = container.name;
                    }
                    continue;
                }

                if (container.kind == ContainerKind::Namespace)
                {
                    qualified = container.name + "::" + qualified;
                }
            }
            return qualified;
        }

        // String literal
        if (nodeType == "string_literal")
        {
            return "string";
        }

        // Boolean literal
        if (nodeType == "boolean_literal")
        {
            return "bool";
        }

        // Null literal
        if (nodeType == "null_literal")
        {
            return "null";
        }

        // Character literal
        if (nodeType == "character_literal")
        {
            return "uint8";
        }

        // Number literal
        if (nodeType == "number_literal")
        {
            std::string text = GetNodeText(exprNode, sourceCode);

            // `d`, `e` and `f` are hex digits, so a hex literal must never be scanned for the
            // float suffix and the exponent marker: `0xefc60000` is a uint, and reading it as a
            // float made every rule downstream believe a bitwise expression was floating point.
            // Found by the numeric-warning corpus audit, which reported a truncation on
            // `y ^= (y << 15) & 0xefc60000;` in a Mersenne twister. `u` and `l` are not hex
            // digits, so the width suffixes below stay meaningful either way.
            const bool isRadixPrefixed = text.size() > 1 && text[0] == '0' &&
                                         (text[1] == 'x' || text[1] == 'X' ||
                                          text[1] == 'b' || text[1] == 'B');

            if (!isRadixPrefixed)
            {
                if (text.find('f') != std::string::npos || text.find('F') != std::string::npos)
                {
                    return "float";
                }
                if (text.find('.') != std::string::npos || text.find('e') != std::string::npos || text.find('E') != std::string::npos)
                {
                    return "double";
                }
            }
            if ((text.find('u') != std::string::npos || text.find('U') != std::string::npos) &&
                (text.find('l') != std::string::npos || text.find('L') != std::string::npos))
            {
                return "uint64";
            }
            if (text.find('l') != std::string::npos || text.find('L') != std::string::npos || text.ends_with("i64"))
            {
                return "int64";
            }
            if (text.find('u') != std::string::npos || text.find('U') != std::string::npos)
            {
                return "uint";
            }
            return "int";
        }

        // Scoped identifier (e.g. Game::Player or bare identifier wrapped in scoped_identifier)
        if (nodeType == "scoped_identifier")
        {
            TSNode lastIdentifier = {};
            bool qualified = false;
            const uint32_t childCount = ts_node_named_child_count(exprNode);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                TSNode child = ts_node_named_child(exprNode, i);
                if (std::string_view(ts_node_type(child)) == "identifier")
                {
                    qualified = !ts_node_is_null(lastIdentifier);
                    lastIdentifier = child;
                }
            }

            if (qualified)
            {
                const std::string whole = GetNodeText(exprNode, sourceCode);
                const auto wholeSyms = symbolTable.FindSymbols(whole);
                for (const auto &sym : wholeSyms)
                {
                    if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                        !sym.GetVariable().typeName.empty())
                    {
                        return CleanExpressionType(sym.GetVariable().typeName);
                    }
                    if (sym.type == SymbolType::Function && !sym.GetFunction().returnType.empty())
                    {
                        return CleanExpressionType(sym.GetFunction().returnType);
                    }
                }
            }

            return ts_node_is_null(lastIdentifier)
                       ? std::string()
                       : ResolveExpressionType(lastIdentifier, scope, symbolTable, sourceCode, uri, depth + 1);
        }

        // Bare identifier
        if (nodeType == "identifier")
        {
            std::string name = GetNodeText(exprNode, sourceCode);
            if (name == "this")
            {
                if (uri.starts_with("angelscript-virtual:") || uri.starts_with("angelscript-virtual://"))
                {
                    std::string hostClass = SymbolTable::ExtractVirtualHostClass(uri);
                    if (!hostClass.empty())
                    {
                        return CleanExpressionType(hostClass);
                    }
                }
                auto containers = GetEnclosingContainers(exprNode, sourceCode);
                for (const auto &c : containers)
                {
                    if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
                    {
                        return CleanExpressionType(c.qualifiedName.empty() ? c.name : c.qualifiedName);
                    }
                }
            }
            if (name == "BaseClass")
            {
                if (uri.starts_with("angelscript-virtual:") || uri.starts_with("angelscript-virtual://"))
                {
                    std::string hostClass = SymbolTable::ExtractVirtualHostClass(uri);
                    if (!hostClass.empty())
                    {
                        std::string base = ResolveBaseClass(hostClass, symbolTable);
                        if (!base.empty())
                        {
                            return CleanExpressionType(base);
                        }
                    }
                }
                auto containers = GetEnclosingContainers(exprNode, sourceCode);
                for (const auto &c : containers)
                {
                    if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
                    {
                        std::string base = ResolveBaseClass(c.qualifiedName.empty() ? c.name : c.qualifiedName, symbolTable);
                        if (!base.empty())
                        {
                            return CleanExpressionType(base);
                        }
                        break;
                    }
                }
            }
            if (scope)
            {
                const LocalDefinition *def = ResolveInScope(scope, name);
                if (def && !def->typeName.empty())
                {
                    return CleanExpressionType(def->typeName);
                }
            }

            auto syms = symbolTable.FindSymbols(name);
            if (syms.empty())
            {
                syms = FindSymbolsInScope(name, exprNode, sourceCode, symbolTable);
            }
            if (syms.empty())
            {
                auto containers = GetEnclosingContainers(exprNode, sourceCode);
                for (const auto &c : containers)
                {
                    if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
                    {
                        auto hierarchy = GetInheritedTypeHierarchy(c.qualifiedName.empty() ? c.name : c.qualifiedName, symbolTable);
                        for (const auto &cls : hierarchy)
                        {
                            auto found = symbolTable.FindSymbols(cls + "::" + name);
                            if (!found.empty())
                            {
                                syms = std::move(found);
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            if (syms.empty() && (uri.starts_with("angelscript-virtual:") || uri.starts_with("angelscript-virtual://")))
            {
                std::string hostClass = SymbolTable::ExtractVirtualHostClass(uri);
                if (!hostClass.empty())
                {
                    auto hierarchy = GetInheritedTypeHierarchy(hostClass, symbolTable);
                    for (const auto &cls : hierarchy)
                    {
                        auto found = symbolTable.FindSymbols(cls + "::" + name);
                        if (!found.empty())
                        {
                            syms = std::move(found);
                            break;
                        }
                    }
                    if (syms.empty())
                    {
                        for (const auto &cls : hierarchy)
                        {
                            auto accessors = FindPropertyAccessors(cls, name, symbolTable, false);
                            if (!accessors.empty())
                            {
                                std::string pType = PropertyTypeFromAccessors(accessors);
                                if (!pType.empty())
                                {
                                    return CleanExpressionType(pType);
                                }
                            }
                        }
                    }
                }
            }
            for (const auto &sym : syms)
            {
                if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) && !sym.GetVariable().typeName.empty())
                {
                    return CleanExpressionType(sym.GetVariable().typeName);
                }
                else if (sym.type == SymbolType::Function && !sym.GetFunction().returnType.empty())
                {
                    return CleanExpressionType(sym.GetFunction().returnType);
                }
            }

            for (const auto &prefix : { "get_", "set_" })
            {
                auto accessorSyms = symbolTable.FindSymbols(std::string(prefix) + name);
                for (const auto &sym : accessorSyms)
                {
                    if (sym.type == SymbolType::Function && sym.containerName.empty())
                    {
                        if (!sym.GetFunction().returnType.empty() && sym.GetFunction().returnType != "void")
                        {
                            return CleanExpressionType(sym.GetFunction().returnType);
                        }
                        else if (!sym.GetFunction().parameters.empty())
                        {
                            return CleanExpressionType(sym.GetFunction().parameters.front().typeName);
                        }
                    }
                }
            }

            return "";
        }

        // Assignment expression (e.g. g_var = 0, x += 1)
        if (nodeType == "assignment_expression")
        {
            TSNode left = parser::GetChildByField(exprNode, parser::fields::Left);
            if (!ts_node_is_null(left))
            {
                return ResolveExpressionType(left, scope, symbolTable, sourceCode, uri, depth + 1);
            }
            return "";
        }

        // Binary expression (e.g. a + b, x == y, etc.)
        if (nodeType == "binary_expression")
        {
            TSNode left = parser::GetChildByField(exprNode, parser::fields::Left);
            TSNode opNode = parser::GetChildByField(exprNode, parser::fields::Operator);
            TSNode right = parser::GetChildByField(exprNode, parser::fields::Right);
            if (ts_node_is_null(left) || ts_node_is_null(right))
            {
                return "";
            }

            std::string op = GetNodeText(opNode, sourceCode);
            std::string leftType = ResolveExpressionType(left, scope, symbolTable, sourceCode, uri, depth + 1);
            std::string rightType = ResolveExpressionType(right, scope, symbolTable, sourceCode, uri, depth + 1);

            // Relational and equality operators always evaluate to bool
            if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=" ||
                op == "is" || op == "!is")
            {
                return "bool";
            }

            // Logical operators always evaluate to bool
            if (op == "&&" || op == "||" || op == "and" || op == "or" || op == "^^" || op == "xor")
            {
                return "bool";
            }

            // String concatenation
            if (op == "+" && (CleanBaseType(leftType) == "string" || CleanBaseType(rightType) == "string"))
            {
                return "string";
            }

            std::string cleanLeft = CleanBaseType(leftType);
            std::string cleanRight = CleanBaseType(rightType);

            // Map binary operator to operator overload method name
            std::string opMethod;
            std::string revOpMethod;
            if (op == "+") { opMethod = "opAdd"; revOpMethod = "opAdd_r"; }
            else if (op == "-") { opMethod = "opSub"; revOpMethod = "opSub_r"; }
            else if (op == "*") { opMethod = "opMul"; revOpMethod = "opMul_r"; }
            else if (op == "/") { opMethod = "opDiv"; revOpMethod = "opDiv_r"; }
            else if (op == "%") { opMethod = "opMod"; revOpMethod = "opMod_r"; }
            else if (op == "**") { opMethod = "opPow"; revOpMethod = "opPow_r"; }
            else if (op == "&") { opMethod = "opAnd"; revOpMethod = "opAnd_r"; }
            else if (op == "|") { opMethod = "opOr"; revOpMethod = "opOr_r"; }
            else if (op == "^") { opMethod = "opXor"; revOpMethod = "opXor_r"; }
            else if (op == "<<") { opMethod = "opShl"; revOpMethod = "opShl_r"; }
            else if (op == ">>") { opMethod = "opShr"; revOpMethod = "opShr_r"; }
            else if (op == ">>>") { opMethod = "opUShr"; revOpMethod = "opUShr_r"; }

            // 1. Check member operator overloads on left operand
            if (!opMethod.empty() && !cleanLeft.empty())
            {
                std::vector<Symbol> candidates;
                auto hierarchy = GetInheritedTypeHierarchy(cleanLeft, symbolTable);
                for (const auto &typeName : hierarchy)
                {
                    auto found = symbolTable.FindSymbols(typeName + "::" + opMethod);
                    for (const auto &sym : found)
                    {
                        if (sym.type == SymbolType::Function)
                        {
                            candidates.push_back(sym);
                        }
                    }
                }
                if (!candidates.empty())
                {
                    auto match = ResolveBestOverload(candidates, { rightType }, symbolTable);
                    if (match.bestCandidate && std::holds_alternative<FunctionSignature>(match.bestCandidate->signature))
                    {
                        return CleanExpressionType(match.bestCandidate->GetFunction().returnType);
                    }
                }
            }

            // 2. Check reversed operator overloads on right operand
            if (!revOpMethod.empty() && !cleanRight.empty())
            {
                std::vector<Symbol> candidates;
                auto hierarchy = GetInheritedTypeHierarchy(cleanRight, symbolTable);
                for (const auto &typeName : hierarchy)
                {
                    auto found = symbolTable.FindSymbols(typeName + "::" + revOpMethod);
                    for (const auto &sym : found)
                    {
                        if (sym.type == SymbolType::Function)
                        {
                            candidates.push_back(sym);
                        }
                    }
                }
                if (!candidates.empty())
                {
                    auto match = ResolveBestOverload(candidates, { leftType }, symbolTable);
                    if (match.bestCandidate && std::holds_alternative<FunctionSignature>(match.bestCandidate->signature))
                    {
                        return CleanExpressionType(match.bestCandidate->GetFunction().returnType);
                    }
                }
            }

            // 3. Numeric promotions for primitives
            if (!cleanLeft.empty() && !cleanRight.empty())
            {
                if (op == "/" &&
                    (cleanLeft == "float" || cleanLeft == "double" || cleanLeft == "int" || cleanLeft == "uint") &&
                    (cleanRight == "float" || cleanRight == "double" || cleanRight == "int" || cleanRight == "uint"))
                {
                    return "float";
                }

                if (cleanLeft == "double" || cleanRight == "double")
                {
                    return "double";
                }
                if (cleanLeft == "float" || cleanRight == "float")
                {
                    return "float";
                }
                if (cleanLeft == "uint64" || cleanRight == "uint64")
                {
                    return "uint64";
                }
                if (cleanLeft == "int64" || cleanRight == "int64")
                {
                    return "int64";
                }
                if (op == "<<" || op == ">>" || op == ">>>")
                {
                    return cleanLeft == "uint" || cleanLeft == "uint32" || cleanLeft == "uint64" || cleanLeft == "uint16" || cleanLeft == "uint8" ? "uint" : "int";
                }
                if ((op == "|" || op == "&" || op == "^") && cleanLeft == cleanRight)
                {
                    return cleanLeft;
                }
                if (cleanLeft == "uint" || cleanRight == "uint" || cleanLeft == "uint32" || cleanRight == "uint32")
                {
                    return "uint";
                }
                if (IsPrimitiveTypeName(cleanLeft) && IsPrimitiveTypeName(cleanRight))
                {
                    return "int";
                }
            }

            return "";
        }

        // Ternary expression (e.g. cond ? expr1 : expr2)
        if (nodeType == "ternary_expression")
        {
            TSNode consequence = parser::GetChildByField(exprNode, parser::fields::Consequence);
            TSNode alternative = parser::GetChildByField(exprNode, parser::fields::Alternative);
            if (ts_node_is_null(consequence) || ts_node_is_null(alternative))
            {
                return "";
            }
            std::string t1 = ResolveExpressionType(consequence, scope, symbolTable, sourceCode, uri, depth + 1);
            std::string t2 = ResolveExpressionType(alternative, scope, symbolTable, sourceCode, uri, depth + 1);
            if (t1.empty())
            {
                return t2;
            }
            if (t2.empty())
            {
                return t1;
            }

            std::string c1 = CanonicalizeType(CleanBaseType(t1));
            std::string c2 = CanonicalizeType(CleanBaseType(t2));

            // Identity rule: identical types (including complex classes like Vector) resolve to c1
            if (!c1.empty() && c1 == c2)
            {
                if (t1.ends_with("@") && t2.ends_with("@"))
                {
                    return c1 + "@";
                }
                return c1;
            }

            // Numeric promotion
            if (IsNumericPrimitive(c1) && IsNumericPrimitive(c2))
            {
                if (c1 == "double" || c2 == "double")
                {
                    return "double";
                }
                if (c1 == "float" || c2 == "float")
                {
                    return "float";
                }
                if (c1 == "int64" || c2 == "int64")
                {
                    return "int64";
                }
                if (c1 == "uint64" || c2 == "uint64")
                {
                    return "uint64";
                }
                if (c1 == "uint" || c2 == "uint")
                {
                    return "uint";
                }
                return "int";
            }

            // Enum to integer promotion in ternary
            if (ResolvesToEnum(c1, symbolTable) && IsIntegerPrimitive(c2))
            {
                return c2;
            }
            if (ResolvesToEnum(c2, symbolTable) && IsIntegerPrimitive(c1))
            {
                return c1;
            }

            // Inheritance check (derived vs base)
            auto h1 = GetInheritedTypeHierarchy(c1, symbolTable);
            for (const auto &b : h1)
            {
                if (CanonicalizeType(CleanBaseType(b)) == c2)
                {
                    return t2;
                }
            }
            auto h2 = GetInheritedTypeHierarchy(c2, symbolTable);
            for (const auto &b : h2)
            {
                if (CanonicalizeType(CleanBaseType(b)) == c1)
                {
                    return t1;
                }
            }

            return "";
        }

        // Member expression (e.g. obj.member)
        if (nodeType == "member_expression")
        {
            TSNode objNode = parser::GetChildByField(exprNode, parser::fields::Object);
            TSNode memNode = parser::GetChildByField(exprNode, parser::fields::Member);
            if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
            {
                return "";
            }

            std::string objType = ResolveExpressionType(objNode, scope, symbolTable, sourceCode, uri, depth + 1);
            std::string cleanObj = CleanBaseType(objType);
            if (cleanObj.empty())
            {
                std::string rawObj = GetNodeText(objNode, sourceCode);
                while (!rawObj.empty() && isspace(static_cast<unsigned char>(rawObj.front())))
                {
                    rawObj.erase(rawObj.begin());
                }
                while (!rawObj.empty() && isspace(static_cast<unsigned char>(rawObj.back())))
                {
                    rawObj.pop_back();
                }
                cleanObj = rawObj;
            }
            if (cleanObj.empty())
            {
                return "";
            }

            std::string memName = GetNodeText(memNode, sourceCode);
            while (!memName.empty() && isspace(static_cast<unsigned char>(memName.front()))) memName.erase(memName.begin());
            while (!memName.empty() && isspace(static_cast<unsigned char>(memName.back()))) memName.pop_back();
            auto hierarchy = GetInheritedTypeHierarchy(cleanObj, symbolTable);
            if (hierarchy.empty() && !cleanObj.empty())
            {
                hierarchy.push_back(cleanObj);
            }
            std::vector<std::string> propSearchOrder;
            if (!hierarchy.empty())
            {
                propSearchOrder.push_back(hierarchy[0]);
                for (size_t i = 1; i < hierarchy.size(); ++i)
                {
                    if (!IsMixinClass(hierarchy[i], symbolTable))
                    {
                        propSearchOrder.push_back(hierarchy[i]);
                    }
                }
                for (size_t i = 1; i < hierarchy.size(); ++i)
                {
                    if (IsMixinClass(hierarchy[i], symbolTable))
                    {
                        propSearchOrder.push_back(hierarchy[i]);
                    }
                }
            }

            for (const auto &typeName : propSearchOrder)
            {
                std::string qName = typeName + "::" + memName;
                auto found = symbolTable.FindSymbols(qName);
                if (found.empty())
                {
                    found = symbolTable.FindSymbols(typeName + "::get_" + memName);
                }
                if (found.empty())
                {
                    found = symbolTable.FindSymbols(typeName + "::set_" + memName);
                }
                if (found.empty())
                {
                    auto allSymbols = symbolTable.FindSymbols(memName);
                    for (const auto &sSym : allSymbols)
                    {
                        if (sSym.containerName == typeName)
                        {
                            found.push_back(sSym);
                        }
                    }
                }
                for (const auto &sym : found)
                {
                    if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) && !sym.GetVariable().typeName.empty())
                    {
                        return CleanExpressionType(sym.GetVariable().typeName);
                    }
                    else if (sym.type == SymbolType::Function)
                    {
                        if (!sym.GetFunction().returnType.empty() && sym.GetFunction().returnType != "void")
                        {
                            return CleanExpressionType(sym.GetFunction().returnType);
                        }
                        else if (!sym.GetFunction().parameters.empty())
                        {
                            return CleanExpressionType(sym.GetFunction().parameters.back().typeName);
                        }
                    }
                }
            }
            return "";
        }

        // Call expression (e.g. func(arg1, arg2) or obj.method(arg1, arg2))
        if (nodeType == "call_expression")
        {
            TSNode funcNode = parser::GetChildByField(exprNode, parser::fields::Function);
            if (ts_node_is_null(funcNode) && ts_node_child_count(exprNode) > 0)
            {
                funcNode = ts_node_child(exprNode, 0);
            }
            if (ts_node_is_null(funcNode))
            {
                return "";
            }

            std::vector<std::string> argTypes;
            TSNode argsNode = parser::GetChildByField(exprNode, parser::fields::Arguments);
            if (!ts_node_is_null(argsNode))
            {
                uint32_t count = ts_node_named_child_count(argsNode);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode argChild = ts_node_named_child(argsNode, i);
                    argTypes.push_back(ResolveExpressionType(argChild, scope, symbolTable, sourceCode, uri, depth + 1));
                }
            }

            std::string_view funcNodeType = ts_node_type(funcNode);
            if (funcNodeType == "member_expression")
            {
                TSNode objNode = parser::GetChildByField(funcNode, parser::fields::Object);
                TSNode memNode = parser::GetChildByField(funcNode, parser::fields::Member);
                if (!ts_node_is_null(objNode) && !ts_node_is_null(memNode))
                {
                    // Canonicalised before it is parsed as a template: `int[]` and `array<int>` are
                    // the same type, and only the second was ever recognised here, so `a.length()`
                    // on a bracket-declared array resolved to nothing at all.
                    std::string objType = CanonicalizeArrayType(
                        ResolveExpressionType(objNode, scope, symbolTable, sourceCode, uri, depth + 1));
                    auto templateInfo = ParseTemplateType(objType);
                    std::string memName = GetNodeText(memNode, sourceCode);
                    while (!memName.empty() && isspace(static_cast<unsigned char>(memName.front()))) memName.erase(memName.begin());
                    while (!memName.empty() && isspace(static_cast<unsigned char>(memName.back()))) memName.pop_back();

                    std::string ownerType = MemberOwnerType(objType);
                    if (ownerType.empty())
                    {
                        ownerType = CleanExpressionType(objType);
                    }
                    if (ownerType.empty())
                    {
                        std::string rawObj = GetNodeText(objNode, sourceCode);
                        while (!rawObj.empty() && isspace(static_cast<unsigned char>(rawObj.front())))
                        {
                            rawObj.erase(rawObj.begin());
                        }
                        while (!rawObj.empty() && isspace(static_cast<unsigned char>(rawObj.back())))
                        {
                            rawObj.pop_back();
                        }
                        ownerType = rawObj;
                    }

                    std::vector<Symbol> candidates;
                    auto hierarchy = GetInheritedTypeHierarchy(ownerType, symbolTable);
                    if (hierarchy.empty() && !ownerType.empty())
                    {
                        hierarchy.push_back(ownerType);
                    }
                    for (const auto &typeName : hierarchy)
                    {
                        auto found = symbolTable.FindSymbols(typeName + "::" + memName);
                        for (const auto &sym : found)
                        {
                            if (sym.type == SymbolType::Function)
                            {
                                candidates.push_back(sym);
                            }
                        }
                    }

                    if (!candidates.empty())
                    {
                        auto match = ResolveBestOverload(candidates, argTypes, symbolTable);
                        const Symbol *chosen = match.bestCandidate ? match.bestCandidate : &candidates[0];
                        if (chosen && std::holds_alternative<FunctionSignature>(chosen->signature))
                        {
                            std::string ret = chosen->GetFunction().returnType;
                            auto binding = BindTemplateArguments(objType, symbolTable);
                            if (binding.usable)
                            {
                                ret = SubstituteTemplateParameters(ret, binding);
                            }
                            else if (!templateInfo.templateArgs.empty())
                            {
                                ret = SubstituteTypeParam(ret, "T", templateInfo.templateArgs[0]);
                            }
                            return CleanExpressionType(ret);
                        }
                        return CleanExpressionType(candidates[0].GetFunction().returnType);
                    }

                    // Fallback when symbol table has no stubs or declarations for this container method
                    if (!templateInfo.templateArgs.empty() || objType.ends_with("[]"))
                    {
                        if (memName == "length" || memName == "size")
                        {
                            return "uint";
                        }
                        if (memName == "isEmpty")
                        {
                            return "bool";
                        }
                    }
                }
            }
            else
            {
                std::string funcName = GetNodeText(funcNode, sourceCode);
                std::vector<Symbol> candidates;

                auto containers = GetEnclosingContainers(exprNode, sourceCode);
                for (const auto &c : containers)
                {
                    if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
                    {
                        auto hierarchy = GetInheritedTypeHierarchy(c.qualifiedName.empty() ? c.name : c.qualifiedName, symbolTable);
                        for (const auto &cls : hierarchy)
                        {
                            auto found = symbolTable.FindSymbols(cls + "::" + funcName);
                            for (const auto &sym : found)
                            {
                                if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
                                {
                                    bool overriddenLower = std::any_of(candidates.begin(), candidates.end(),
                                        [&](const Symbol &kept) {
                                            return HasSameParameterList(kept, sym);
                                        });
                                    if (!overriddenLower)
                                    {
                                        candidates.push_back(sym);
                                    }
                                }
                            }
                        }

                        // Also check directly included mixins if not surfaced in hierarchy
                        auto classSyms = symbolTable.FindSymbols(c.qualifiedName.empty() ? c.name : c.qualifiedName);
                        for (const auto &cs : classSyms)
                        {
                            if (cs.type == SymbolType::Class && std::holds_alternative<ClassSignature>(cs.signature))
                            {
                                for (const auto &mixin : cs.GetClass().includedMixins)
                                {
                                    auto mixinFound = symbolTable.FindSymbols(mixin + "::" + funcName);
                                    for (const auto &sym : mixinFound)
                                    {
                                        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
                                        {
                                            bool overriddenLower = std::any_of(candidates.begin(), candidates.end(),
                                                [&](const Symbol &kept) {
                                                    return HasSameParameterList(kept, sym);
                                                });
                                            if (!overriddenLower)
                                            {
                                                candidates.push_back(sym);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        break;
                    }
                }

                if (candidates.empty())
                {
                    std::vector<Symbol> inScope = FindSymbolsInScope(funcName, exprNode, sourceCode, symbolTable);
                    for (const auto &sym : inScope)
                    {
                        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
                        {
                            candidates.push_back(sym);
                        }
                    }
                }

                if (candidates.empty())
                {
                    auto globalFound = symbolTable.FindSymbols(funcName);
                    for (const auto &sym : globalFound)
                    {
                        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
                        {
                            candidates.push_back(sym);
                        }
                    }
                }

                if (!candidates.empty())
                {
                    auto match = ResolveBestOverload(candidates, argTypes, symbolTable);
                    if (match.bestCandidate && std::holds_alternative<FunctionSignature>(match.bestCandidate->signature))
                    {
                        std::string ret = CleanExpressionType(match.bestCandidate->GetFunction().returnType);
                        if (ret.empty() && match.bestCandidate->name == match.bestCandidate->containerName)
                        {
                            return match.bestCandidate->name;
                        }
                        return ret;
                    }
                    std::string ret = CleanExpressionType(candidates[0].GetFunction().returnType);
                    if (ret.empty() && candidates[0].name == candidates[0].containerName)
                    {
                        return candidates[0].name;
                    }
                    return ret;
                }

                auto globalFound = symbolTable.FindSymbols(funcName);
                for (const auto &sym : globalFound)
                {
                    if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface)
                    {
                        return sym.name;
                    }
                }
            }

            std::string calleeType = ResolveExpressionType(funcNode, scope, symbolTable, sourceCode, uri, depth + 1);
            std::string cleanCallee = CleanBaseType(calleeType);
            if (!cleanCallee.empty())
            {
                auto funcdefSymbols = symbolTable.FindSymbols(cleanCallee);
                for (const auto &s : funcdefSymbols)
                {
                    if (s.type == SymbolType::Funcdef)
                    {
                        return CleanExpressionType(s.GetFuncdef().returnType);
                    }
                }
            }

            return calleeType;
        }

        // Cast expression (e.g. cast<Player@>(ent))
        if (nodeType == "cast_expression" || nodeType == "functional_cast_expression")
        {
            TSNode typeNode = parser::GetChildByField(exprNode, parser::fields::Type);
            return ts_node_is_null(typeNode) ? std::string()
                                             : CleanExpressionType(GetNodeText(typeNode, sourceCode));
        }

        // Construct call expression (e.g. array<int>(5))
        if (nodeType == "construct_call_expression")
        {
            TSNode typeNode = parser::GetChildByField(exprNode, parser::fields::Type);
            if (ts_node_is_null(typeNode))
            {
                return "";
            }

            std::string written = GetNodeText(typeNode, sourceCode);
            const uint32_t childCount = ts_node_named_child_count(exprNode);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                TSNode child = ts_node_named_child(exprNode, i);
                if (std::string_view(ts_node_type(child)) == "template_type_list")
                {
                    written += GetNodeText(child, sourceCode);
                    break;
                }
            }
            return CleanExpressionType(written);
        }

        // Index expression (e.g. arr[i] or dict["key"])
        if (nodeType == "index_expression")
        {
            TSNode objNode = parser::GetChildByField(exprNode, parser::fields::Object);
            if (ts_node_is_null(objNode))
            {
                return "";
            }

            std::string objType = ResolveExpressionType(objNode, scope, symbolTable, sourceCode, uri, depth + 1);
            if (objType.empty())
            {
                return "";
            }

            return ResolveIndexedType(objType, 1, symbolTable);
        }

        // Unary expression (e.g. !x, -x, ++i)
        if (nodeType == "unary_expression")
        {
            TSNode operatorNode = parser::GetChildByField(exprNode, parser::fields::Operator);
            if (!ts_node_is_null(operatorNode))
            {
                const std::string op = GetNodeText(operatorNode, sourceCode);
                if (op == "!" || op == "not")
                {
                    return "bool";
                }

                // `@f` where `f` names a function is a *function handle*, and which funcdef it
                // becomes is decided by what it is being passed to - AngelScript picks the one the
                // parameter asks for. There is no single answer to give here, so the honest one is
                // none: resolving through to the operand returned `f`'s return type instead, and
                // `createCoRoutine(@worker, args)` was reported as "Cannot implicitly convert
                // 'void' to 'coroutine@'" on correct code.
                if (op == "@")
                {
                    TSNode target = parser::GetChildByField(exprNode, parser::fields::Operand);
                    if (!ts_node_is_null(target))
                    {
                        std::string targetName = GetNodeText(target, sourceCode);
                        while (!targetName.empty() && isspace(static_cast<unsigned char>(targetName.front()))) targetName.erase(targetName.begin());
                        while (!targetName.empty() && isspace(static_cast<unsigned char>(targetName.back()))) targetName.pop_back();

                        if (!targetName.empty() && targetName.find('(') == std::string::npos)
                        {
                            bool namesFunction = false;
                            if (const auto bucket = symbolTable.FindSymbolsPtr(targetName))
                            {
                                for (const auto &candidate : *bucket)
                                {
                                    if (candidate.type == SymbolType::Function)
                                    {
                                        namesFunction = true;
                                        break;
                                    }
                                }
                            }
                            if (!namesFunction)
                            {
                                auto syms = FindSymbolsInScope(targetName, exprNode, sourceCode, symbolTable);
                                if (syms.empty())
                                {
                                    syms = symbolTable.FindSymbols(targetName);
                                }
                                for (const auto &candidate : syms)
                                {
                                    if (candidate.type == SymbolType::Function)
                                    {
                                        namesFunction = true;
                                        break;
                                    }
                                }
                            }
                            if (namesFunction)
                            {
                                return {};
                            }
                        }
                    }
                }
            }

            TSNode operandNode = parser::GetChildByField(exprNode, parser::fields::Operand);
            return ts_node_is_null(operandNode)
                       ? std::string()
                       : ResolveExpressionType(operandNode, scope, symbolTable, sourceCode, uri, depth + 1);
        }

        // Postfix expression (e.g. i++)
        if (nodeType == "postfix_expression")
        {
            TSNode operandNode = parser::GetChildByField(exprNode, parser::fields::Operand);
            return ts_node_is_null(operandNode)
                       ? std::string()
                       : ResolveExpressionType(operandNode, scope, symbolTable, sourceCode, uri, depth + 1);
        }

        // Parenthesized expression (e.g. (expr))
        if (nodeType == "parenthesized_expression")
        {
            if (ts_node_named_child_count(exprNode) > 0)
            {
                return ResolveExpressionType(ts_node_named_child(exprNode, 0), scope, symbolTable, sourceCode, uri, depth + 1);
            }
            uint32_t count = ts_node_child_count(exprNode);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(exprNode, i);
                std::string_view cType = ts_node_type(child);
                if (cType != "(" && cType != ")")
                {
                    return ResolveExpressionType(child, scope, symbolTable, sourceCode, uri, depth + 1);
                }
            }
            return "";
        }

        // Initializer list (e.g. {123}, {"s"})
        if (nodeType == "initializer_list")
        {
            uint32_t count = ts_node_named_child_count(exprNode);
            if (count > 0)
            {
                std::string elemType = ResolveExpressionType(ts_node_named_child(exprNode, 0), scope, symbolTable, sourceCode, uri, depth + 1);
                return "{" + elemType + "}";
            }
            return "{}";
        }

        return "";
    }

    std::string ResolveReceiverType(
        TSNode objNode,
        std::string_view sourceCode,
        const SymbolTable &symbolTable,
        const Scope *scope,
        std::string_view virtualHostClass,
        std::string_view fileUri)
    {
        if (ts_node_is_null(objNode))
        {
            return "";
        }

        std::string objText = GetNodeText(objNode, sourceCode);
        if (objText.empty())
        {
            return "";
        }

        std::string effectiveHostClass = std::string(virtualHostClass);
        if (effectiveHostClass.empty() && (fileUri.starts_with("angelscript-virtual:") || fileUri.starts_with("angelscript-virtual://")))
        {
            effectiveHostClass = SymbolTable::ExtractVirtualHostClass(fileUri);
        }

        if (objText == "this")
        {
            if (!effectiveHostClass.empty())
            {
                return effectiveHostClass;
            }
            auto containers = GetEnclosingContainers(objNode, sourceCode);
            for (const auto &c : containers)
            {
                if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
                {
                    return c.qualifiedName.empty() ? c.name : c.qualifiedName;
                }
            }
            return "";
        }

        if (objText == "BaseClass")
        {
            if (!effectiveHostClass.empty())
            {
                return ResolveBaseClass(effectiveHostClass, symbolTable);
            }
            auto containers = GetEnclosingContainers(objNode, sourceCode);
            for (const auto &c : containers)
            {
                if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
                {
                    return ResolveBaseClass(c.qualifiedName.empty() ? c.name : c.qualifiedName, symbolTable);
                }
            }
            return "";
        }

        if (scope)
        {
            const LocalDefinition *objDef = ResolveInScope(scope, objText);
            if (objDef && !objDef->typeName.empty())
            {
                return MemberOwnerType(objDef->typeName);
            }
        }

        if (!effectiveHostClass.empty())
        {
            auto hier = GetInheritedTypeHierarchy(effectiveHostClass, symbolTable);
            for (const auto &cls : hier)
            {
                auto memberSyms = symbolTable.FindSymbols(cls + "::" + objText);
                for (const auto &sym : memberSyms)
                {
                    if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                        std::holds_alternative<VariableSignature>(sym.signature))
                    {
                        const auto &var = sym.GetVariable();
                        if (!var.typeName.empty())
                        {
                            return MemberOwnerType(var.typeName);
                        }
                    }
                    else if (sym.type == SymbolType::Function &&
                             std::holds_alternative<FunctionSignature>(sym.signature))
                    {
                        const auto &fn = sym.GetFunction();
                        if (!fn.returnType.empty() && fn.returnType != "void")
                        {
                            return MemberOwnerType(fn.returnType);
                        }
                    }
                }
                auto accessors = FindPropertyAccessors(cls, objText, symbolTable, false);
                if (!accessors.empty())
                {
                    std::string pType = PropertyTypeFromAccessors(accessors);
                    if (!pType.empty())
                    {
                        return MemberOwnerType(pType);
                    }
                }
            }
        }

        auto containers = GetEnclosingContainers(objNode, sourceCode);
        for (const auto &c : containers)
        {
            if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
            {
                std::string enclosing = c.qualifiedName.empty() ? c.name : c.qualifiedName;
                auto hier = GetInheritedTypeHierarchy(enclosing, symbolTable);
                for (const auto &cls : hier)
                {
                    auto memberSyms = symbolTable.FindSymbols(cls + "::" + objText);
                    for (const auto &sym : memberSyms)
                    {
                        if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                            std::holds_alternative<VariableSignature>(sym.signature))
                        {
                            const auto &var = sym.GetVariable();
                            if (!var.typeName.empty())
                            {
                                return MemberOwnerType(var.typeName);
                            }
                        }
                        else if (sym.type == SymbolType::Function &&
                                 std::holds_alternative<FunctionSignature>(sym.signature))
                        {
                            const auto &fn = sym.GetFunction();
                            if (!fn.returnType.empty() && fn.returnType != "void")
                            {
                                return MemberOwnerType(fn.returnType);
                            }
                        }
                    }
                    auto accessors = FindPropertyAccessors(cls, objText, symbolTable, false);
                    if (!accessors.empty())
                    {
                        std::string pType = PropertyTypeFromAccessors(accessors);
                        if (!pType.empty())
                        {
                            return MemberOwnerType(pType);
                        }
                    }
                }
                break;
            }
        }

        std::string exprType = ResolveExpressionType(objNode, scope, symbolTable, sourceCode, fileUri);
        if (!exprType.empty() && exprType != "void" && exprType != "unknown")
        {
            return MemberOwnerType(exprType);
        }

        auto globSyms = symbolTable.FindSymbols(objText);
        for (const auto &sym : globSyms)
        {
            if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                std::holds_alternative<VariableSignature>(sym.signature))
            {
                const auto &var = sym.GetVariable();
                if (!var.typeName.empty())
                {
                    return MemberOwnerType(var.typeName);
                }
            }
            else if (sym.type == SymbolType::Class || sym.type == SymbolType::Namespace)
            {
                return sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            }
        }

        auto shortMatches = symbolTable.FindTypeSymbolsByShortName(objText);
        for (const auto &sym : shortMatches)
        {
            if (sym.type == SymbolType::Class || sym.type == SymbolType::Namespace)
            {
                return sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            }
        }

        return "";
    }

    bool IsVariableType(std::string_view typeName)
    {
        // Strip whatever decoration the declaration carried: const, &, @, in/out/inout, spaces.
        // What has to remain is a bare '?' and nothing else.
        std::string cleaned;
        cleaned.reserve(typeName.size());
        for (const char c : typeName)
        {
            if (c == '?')
                cleaned.push_back(c);
            else if (c != ' ' && c != '\t' && c != '&' && c != '@')
            {
                // Any other identifier character means this is a real type, not the wildcard.
                // "const" / "in" / "out" / "inout" are the only words legally adjacent to it.
                cleaned.push_back(c);
            }
        }

        static constexpr std::string_view k_decorations[] = { "const", "inout", "out", "in" };
        for (const auto decoration : k_decorations)
        {
            for (size_t at = cleaned.find(decoration); at != std::string::npos; at = cleaned.find(decoration, at))
                cleaned.erase(at, decoration.size());
        }

        return cleaned == "?";
    }

    // --- A lambda against the funcdef it is being handed to -------------------------------

    namespace
    {

        std::string LastSegmentOf(const std::string &name)
        {
            const size_t at = name.rfind("::");
            return at == std::string::npos ? name : name.substr(at + 2);
        }

        /** @brief True for a name that denotes a typedef, whose alias no spelling can see through. */
        bool NamesATypedef(const std::string &name, const SymbolTable &table)
        {
            const auto bucket = table.FindSymbolsPtr(name);
            if (!bucket)
            {
                return false;
            }
            for (const auto &sym : *bucket)
            {
                if (sym.type == SymbolType::Typedef)
                {
                    return true;
                }
            }
            return false;
        }
    }

    bool IsLambdaExpression(TSNode node) noexcept
    {
        if (ts_node_is_null(node))
        {
            return false;
        }
        const std::string_view type(ts_node_type(node));
        return type == node_types::LambdaExpression;
    }

    std::vector<LambdaParameter> ReadLambdaParameters(TSNode listNode, std::string_view sourceCode)
    {
        std::vector<LambdaParameter> parameters;
        if (ts_node_is_null(listNode))
        {
            return parameters;
        }

        LambdaParameter current;
        bool groupHasContent = false;

        const uint32_t childCount = ts_node_child_count(listNode);
        for (uint32_t i = 0; i < childCount; ++i)
        {
            TSNode child = ts_node_child(listNode, i);
            const std::string text = GetNodeText(child, sourceCode);

            if (text == "(")
            {
                continue;
            }
            if (text == ")")
            {
                break;
            }
            if (text == ",")
            {
                parameters.push_back(current);
                current = LambdaParameter{};
                groupHasContent = false;
                continue;
            }

            const char *field = ts_node_field_name_for_child(listNode, i);
            if (field && std::string_view(field) == "param_type")
            {
                current.hasWrittenType = true;
                current.isConst = text.starts_with("const ") || text == "const";
                current.isHandle = text.find('@') != std::string::npos;
                current.typeName = CleanBaseType(text);
            }
            else if (text == "&")
            {
                current.isReference = true;
            }
            else if (text == "in" || text == "out" || text == "inout")
            {
                current.modifier = text == "in"  ? ParameterModifier::In
                                 : text == "out" ? ParameterModifier::Out
                                                 : ParameterModifier::InOut;
            }

            groupHasContent = true;
        }

        if (groupHasContent)
        {
            parameters.push_back(current);
        }
        return parameters;
    }

    bool LambdaContradictsFuncdef(const std::vector<LambdaParameter> &lambdaParameters,
                                  const FuncdefSignature &funcdefSig,
                                  const SymbolTable &table)
    {
        if (lambdaParameters.size() != funcdefSig.parameters.size())
        {
            return true;
        }

        for (size_t i = 0; i < lambdaParameters.size(); ++i)
        {
            const LambdaParameter &written = lambdaParameters[i];
            if (!written.hasWrittenType)
            {
                continue;
            }

            const ParameterInformation &expected = funcdefSig.parameters[i];
            if (written.isHandle != expected.isHandle ||
                written.isReference != expected.isReference ||
                written.modifier != expected.modifier)
            {
                return true;
            }

            // Compared by last `::` segment, so a name the funcdef writes bare and the lambda
            // writes qualified - or the other way round - is the same name. That costs the
            // `A::Foo` against `B::Foo` case, which is a rejection this misses rather than a legal
            // program it reports.
            const std::string writtenBase(CanonicalizeType(LastSegmentOf(written.typeName)));
            const std::string expectedBase(
                CanonicalizeType(LastSegmentOf(CleanBaseType(expected.typeName))));

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

    bool LambdaContradictsFuncdef(TSNode lambdaNode,
                                  const FuncdefSignature &funcdefSig,
                                  const SymbolTable &table,
                                  std::string_view sourceCode)
    {
        TSNode listNode = parser::GetChildByField(lambdaNode, parser::fields::Parameters);
        if (ts_node_is_null(listNode))
        {
            // No parameter list to read is not the same as an empty one, and guessing which it is
            // would be guessing about the whole signature.
            return false;
        }
        return LambdaContradictsFuncdef(ReadLambdaParameters(listNode, sourceCode), funcdefSig, table);
    }

    std::optional<Symbol> FindFuncdefSymbol(const std::string &typeName, const SymbolTable &table)
    {
        if (typeName.empty())
        {
            return std::nullopt;
        }

        if (const auto bucket = table.FindSymbolsPtr(typeName))
        {
            for (const auto &sym : *bucket)
            {
                if (sym.type == SymbolType::Funcdef)
                {
                    return sym;
                }
            }
        }

        const std::string bare = LastSegmentOf(typeName);
        const auto matches = table.FindTypeSymbolsByShortName(bare);
        for (const auto &sym : matches)
        {
            if (sym.type == SymbolType::Funcdef)
            {
                return sym;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief The funcdef a lambda is being handed to, read from where it is written.
     *
     * A lambda has no return type of its own - the grammar gives `lambda_expression` a parameter
     * list and a body and nothing else - so every question about what its body must return has to
     * come from the target. That is what this answers, for the three shapes a lambda reaches a
     * funcdef through:
     *
     *     CB@ cb = function(...) { };      a declaration whose written type is a funcdef
     *     Register(CB(function(...)));     a funcdef used as a conversion
     *     Take(function(...));             an argument landing on a funcdef parameter
     *
     * Anything else answers nullopt, and so does a call with more than one candidate offering a
     * different funcdef at that position: which one the lambda was written against is precisely
     * what is undecided there, and a return-type verdict drawn from a guess would be worse than
     * the silence it replaced.
     */
    std::optional<Symbol> FuncdefTargetOfLambda(TSNode lambdaNode,
                                                const SymbolTable &table,
                                                std::string_view sourceCode)
    {
        if (!IsLambdaExpression(lambdaNode))
        {
            return std::nullopt;
        }

        TSNode parent = ts_node_parent(lambdaNode);
        if (ts_node_is_null(parent))
        {
            return std::nullopt;
        }

        const std::string_view parentType(ts_node_type(parent));

        // `CB@ cb = function(...) { };` - the declaration writes the type down.
        if (parentType == "variable_declarator")
        {
            TSNode declaration = ts_node_parent(parent);
            if (!ts_node_is_null(declaration))
            {
                TSNode typeNode = parser::GetChildByField(declaration, parser::fields::VarType);
                if (ts_node_is_null(typeNode))
                {
                    typeNode = parser::GetChildByField(declaration, parser::fields::Type);
                }
                if (!ts_node_is_null(typeNode))
                {
                    return FindFuncdefSymbol(CleanBaseType(GetNodeText(typeNode, sourceCode)), table);
                }
            }
            return std::nullopt;
        }

        if (parentType != "argument_list")
        {
            return std::nullopt;
        }

        // Which argument this lambda is, and what is being called.
        uint32_t position = 0;
        bool found = false;
        for (uint32_t i = 0; i < ts_node_named_child_count(parent); ++i)
        {
            if (ts_node_eq(ts_node_named_child(parent, i), lambdaNode))
            {
                position = i;
                found = true;
                break;
            }
        }
        if (!found)
        {
            return std::nullopt;
        }

        TSNode call = ts_node_parent(parent);
        if (ts_node_is_null(call))
        {
            return std::nullopt;
        }

        TSNode callee = parser::GetChildByField(call, parser::fields::Type);
        if (ts_node_is_null(callee))
        {
            callee = parser::GetChildByField(call, parser::fields::Function);
        }
        if (ts_node_is_null(callee) && ts_node_child_count(call) > 0)
        {
            callee = ts_node_child(call, 0);
        }
        if (ts_node_is_null(callee))
        {
            return std::nullopt;
        }

        const std::string calleeName = CleanBaseType(GetNodeText(callee, sourceCode));

        // `Register(CB(function(...)))` - the callee IS the funcdef, used as a conversion.
        if (position == 0 && ts_node_named_child_count(parent) == 1)
        {
            if (auto asConversion = FindFuncdefSymbol(calleeName, table))
            {
                return asConversion;
            }
        }

        // `Take(function(...))` - the parameter at this position names the funcdef. Every
        // candidate has to agree on which one, or there is nothing to be sure of.
        std::optional<Symbol> agreed;
        bool sawCandidate = false;
        const auto lookUp = [&](const std::string &name) -> bool
        {
            const auto bucket = table.FindSymbolsPtr(name);
            if (!bucket)
            {
                return false;
            }
            for (const auto &sym : *bucket)
            {
                if (sym.type != SymbolType::Function ||
                    !std::holds_alternative<FunctionSignature>(sym.signature))
                {
                    continue;
                }
                const auto &parameters = sym.GetFunction().parameters;
                if (position >= parameters.size())
                {
                    return false;
                }
                auto funcdef = FindFuncdefSymbol(CleanBaseType(parameters[position].typeName), table);
                if (!funcdef)
                {
                    return false;
                }
                if (sawCandidate && agreed && agreed->name != funcdef->name)
                {
                    return false;
                }
                agreed = std::move(funcdef);
                sawCandidate = true;
            }
            return sawCandidate;
        };

        const size_t lastSeparator = calleeName.rfind("::");
        const std::string memberName = calleeName.rfind('.') != std::string::npos
                                           ? calleeName.substr(calleeName.rfind('.') + 1)
                                           : calleeName;
        if (!lookUp(calleeName) && !lookUp(memberName) &&
            !(lastSeparator != std::string::npos && lookUp(calleeName.substr(lastSeparator + 2))))
        {
            return std::nullopt;
        }
        return agreed;
    }
}
