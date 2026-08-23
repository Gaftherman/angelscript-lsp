#include "analysis/rules/ClassRules.h"
#include "analysis/SemanticHelpers.h"

#include <algorithm>
#include <vector>

namespace angel_lsp::analysis::rules
{
    namespace
    {
        /** @brief True when the symbol was declared in a predefined stub.
         *  @note Stubs describe a host application's already-registered API, so the rules about how
         *        a declaration may be written do not apply to them: the engine accepted it already. */
        bool IsFromPredefinedStub(const Symbol &sym, const DiagnosticContext &ctx)
        {
            const std::string &extension = ctx.request.predefinedFileExtension;
            return !extension.empty() && sym.fileUri.ends_with(extension);
        }

        /** @brief Modifier and shape rules a class declaration must satisfy on its own. */
        void CheckClassModifiers(const Symbol &sym, const ClassSignature &sig, const DiagnosticContext &ctx)
        {
            // 'external shared class X;' is the one form allowed to have no body.
            if (!sig.hasBraces && !sig.modifiers.isExternal)
            {
                ctx.LogRule("CheckClassModifiers", "as-syntax-error", sym);
                ctx.Emit(sym, "as-syntax-error", sym.name);
            }

            if (sig.modifiers.isExternal && !sig.modifiers.isShared)
            {
                ctx.LogRule("CheckClassModifiers", "as-syntax-error", sym);
                ctx.Emit(sym, "as-syntax-error", sym.name);
            }

            if (sig.modifiers.isOverride || sig.modifiers.isExplicit)
            {
                ctx.LogRule("CheckClassModifiers", "as-syntax-error", sym);
                ctx.Emit(sym, "as-syntax-error", sym.name);
            }

            if (!sig.modifiers.isMixin)
            {
                return;
            }

            if (sig.modifiers.isFinal)
            {
                ctx.LogRule("CheckClassModifiers", "as-err-mixin-final", sym);
                ctx.Emit(sym, "as-err-mixin-final", sym.name);
            }

            if (sig.modifiers.isAbstract)
            {
                ctx.LogRule("CheckClassModifiers", "as-err-mixin-abstract", sym);
                ctx.Emit(sym, "as-err-mixin-abstract", sym.name);
            }

            // A mixin is textually merged into whatever includes it, so it can declare neither a
            // base nor a nested type of its own.
            bool reportedChildType = false;
            ctx.request.symbolTable.ForEachSymbol(
                [&](const std::string &, const std::vector<Symbol> &symbols)
                {
                    if (reportedChildType)
                    {
                        return;
                    }
                    for (const auto &member : symbols)
                    {
                        const bool isNestedType = member.type == SymbolType::Funcdef ||
                                                  member.type == SymbolType::Class ||
                                                  member.type == SymbolType::Enum ||
                                                  member.type == SymbolType::Typedef ||
                                                  member.type == SymbolType::Interface;
                        if (member.containerName == sym.qualifiedName && isNestedType)
                        {
                            ctx.LogRule("CheckClassModifiers", "as-err-mixin-child-type", sym);
                            ctx.Emit(sym, "as-err-mixin-child-type", sym.name);
                            reportedChildType = true;
                            return;
                        }
                    }
                });
        }

        /**
         * @brief Collects the method names a type actually provides, base *classes* included.
         * @note Interfaces in the hierarchy are skipped on purpose. They declare what must be
         *       implemented, not what is - counting their own declarations as implementations makes
         *       every class trivially satisfy every interface it names.
         */
        ankerl::unordered_dense::set<std::string> CollectImplementedMethodNames(const std::string &typeName,
                                                                               const SymbolTable &table)
        {
            ankerl::unordered_dense::set<std::string> names;

            for (const auto &ancestor : GetInheritedTypeHierarchy(typeName, table))
            {
                const auto ancestorSymbols = table.FindSymbolsPtr(ancestor);
                const bool isInterface = ancestorSymbols &&
                    std::any_of(ancestorSymbols->begin(), ancestorSymbols->end(),
                                [](const Symbol &sym) { return sym.type == SymbolType::Interface; });
                if (isInterface)
                {
                    continue;
                }

                table.ForEachSymbol(
                    [&](const std::string &, const std::vector<Symbol> &symbols)
                    {
                        for (const auto &member : symbols)
                        {
                            if (member.type == SymbolType::Function && member.containerName == ancestor)
                            {
                                names.insert(member.name);
                            }
                        }
                    });
            }
            return names;
        }

        /**
         * @brief Reports interface methods a class declares nowhere in its hierarchy.
         *
         * Matched by name only, deliberately. The deleted implementation compared return type,
         * constness and every parameter's type, direction, constness and reference-ness, and looked
         * only at direct bases - so an implementation inherited from a grandparent, or one whose
         * parameter spelling differed harmlessly, was reported as missing. What a user actually
         * wants caught is a method they forgot entirely, and that survives the weaker test.
         */
        void CheckInterfaceImplementation(const Symbol &sym, const ClassSignature &sig, const DiagnosticContext &ctx)
        {
            // An abstract class is allowed to leave the interface to its own subclasses.
            if (sig.modifiers.isAbstract || sig.modifiers.isMixin)
            {
                return;
            }

            const SymbolTable &table = ctx.request.symbolTable;
            const std::string container = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            const auto implemented = CollectImplementedMethodNames(container, table);

            for (const auto &baseName : sig.bases)
            {
                const std::string cleanBase = CleanBaseType(baseName);
                const auto baseSymbols = table.FindSymbolsPtr(cleanBase);
                if (!baseSymbols)
                {
                    continue;
                }

                const bool isInterface = std::any_of(baseSymbols->begin(), baseSymbols->end(),
                    [](const Symbol &base) { return base.type == SymbolType::Interface; });
                if (!isInterface)
                {
                    continue;
                }

                std::vector<std::string> required;
                table.ForEachSymbol(
                    [&](const std::string &, const std::vector<Symbol> &symbols)
                    {
                        for (const auto &member : symbols)
                        {
                            if (member.type == SymbolType::Function && member.containerName == cleanBase)
                            {
                                required.push_back(member.name);
                            }
                        }
                    });

                for (const auto &methodName : required)
                {
                    if (!implemented.contains(methodName))
                    {
                        ctx.LogRule("CheckInterfaceImplementation", "as-err-interface-impl-missing", sym);
                        ctx.Emit(sym, "as-err-interface-impl-missing", sym.name, methodName, cleanBase);
                    }
                }
            }
        }

        /** @brief Reports a method that replaces one a visible base class declared final. */
        void CheckFinalOverrides(const Symbol &sym, const ClassSignature &sig, const DiagnosticContext &ctx)
        {
            const SymbolTable &table = ctx.request.symbolTable;
            const std::string container = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;

            for (const auto &baseName : sig.bases)
            {
                const std::string cleanBase = CleanBaseType(baseName);
                if (cleanBase.empty() || !table.HasSymbolAnywhere(cleanBase))
                {
                    continue;
                }

                table.ForEachSymbol(
                    [&](const std::string &, const std::vector<Symbol> &symbols)
                    {
                        for (const auto &baseMethod : symbols)
                        {
                            if (baseMethod.type != SymbolType::Function ||
                                baseMethod.containerName != cleanBase ||
                                !baseMethod.GetFunction().modifiers.isFinal)
                            {
                                continue;
                            }

                            const std::string derivedName = container.empty()
                                                                ? baseMethod.name
                                                                : container + "::" + baseMethod.name;
                            if (table.HasSymbol(derivedName))
                            {
                                ctx.LogRule("CheckFinalOverrides", "as-err-override-final-method", sym);
                                ctx.Emit(sym, "as-err-override-final-method", baseMethod.name, cleanBase);
                            }
                        }
                    });
            }
        }

        /** @brief Rules about the base list: existence, kind, count and finality. */
        void CheckBases(const Symbol &sym, const ClassSignature &sig, const DiagnosticContext &ctx)
        {
            uint32_t classBaseCount = 0;

            for (const auto &baseName : sig.bases)
            {
                const std::string cleanBase = CleanBaseType(baseName);
                if (cleanBase.empty())
                {
                    continue;
                }

                const auto baseSymbols = ctx.request.symbolTable.FindSymbolsPtr(cleanBase);
                if (!baseSymbols || baseSymbols->empty())
                {
                    // Deliberately silent. A base this analyzer cannot resolve is almost always a
                    // host application type whose stub is not loaded, not a typo - and reporting
                    // those would bury the real findings. The unresolved-type rule owns that
                    // question, gated on stub visibility.
                    continue;
                }

                bool sawClassBase = false;
                for (const auto &base : *baseSymbols)
                {
                    if (base.type == SymbolType::Class)
                    {
                        if (!base.GetClass().modifiers.isMixin)
                        {
                            sawClassBase = true;
                        }
                        if (base.GetClass().modifiers.isFinal)
                        {
                            ctx.LogRule("CheckBases", "as-err-inherit-final", sym);
                            ctx.Emit(sym, "as-err-inherit-final", cleanBase);
                        }
                        break;
                    }
                }

                if (sawClassBase && ++classBaseCount > 1)
                {
                    ctx.LogRule("CheckBases", "as-err-multi-class-inherit", sym);
                    ctx.Emit(sym, "as-err-multi-class-inherit", sym.name);
                }
            }

            if (HasInheritanceCycle(sym.qualifiedName.empty() ? sym.name : sym.qualifiedName,
                                    ctx.request.symbolTable))
            {
                ctx.LogRule("CheckBases", "as-err-circular-inherit", sym);
                ctx.Emit(sym, "as-err-circular-inherit", sym.name);
            }
        }
    }

    bool HasInheritanceCycle(const std::string &typeName, const SymbolTable &table)
    {
        // The path being walked, not every type seen: revisiting a type through a second branch is
        // diamond inheritance, which is ordinary, while revisiting one already on the path is a
        // genuine cycle. Conflating the two reports every interface diamond as circular.
        std::vector<std::string> path;

        const auto walk = [&](const std::string &current, const auto &self) -> bool
        {
            if (std::find(path.begin(), path.end(), current) != path.end())
            {
                return true;
            }
            if (path.size() > 64)
            {
                // Depth this large means a malformed hierarchy; stop rather than recurse forever.
                return false;
            }

            path.push_back(current);

            const auto symbols = table.FindSymbolsPtr(current);
            if (symbols)
            {
                for (const auto &sym : *symbols)
                {
                    std::vector<std::string> bases;
                    if (sym.type == SymbolType::Class)
                    {
                        bases = sym.GetClass().bases;
                    }
                    else if (sym.type == SymbolType::Interface)
                    {
                        bases = sym.GetInterface().inheritedInterfaces;
                    }

                    for (const auto &base : bases)
                    {
                        const std::string cleanBase = CleanBaseType(base);
                        if (!cleanBase.empty() && self(cleanBase, self))
                        {
                            path.pop_back();
                            return true;
                        }
                    }
                }
            }

            path.pop_back();
            return false;
        };

        const auto symbols = table.FindSymbolsPtr(typeName);
        if (!symbols)
        {
            return false;
        }

        // Started from the bases rather than from typeName itself, so the cycle reported is one
        // that actually comes back round to it.
        for (const auto &sym : *symbols)
        {
            std::vector<std::string> bases;
            if (sym.type == SymbolType::Class)
            {
                bases = sym.GetClass().bases;
            }
            else if (sym.type == SymbolType::Interface)
            {
                bases = sym.GetInterface().inheritedInterfaces;
            }

            path.assign(1, typeName);
            for (const auto &base : bases)
            {
                const std::string cleanBase = CleanBaseType(base);
                if (!cleanBase.empty() && walk(cleanBase, walk))
                {
                    return true;
                }
            }
        }

        return false;
    }

    void ValidateClass(const Symbol &sym, const DiagnosticContext &ctx)
    {
        if (sym.type != SymbolType::Class && sym.type != SymbolType::Interface)
        {
            return;
        }

        if (IsFromPredefinedStub(sym, ctx))
        {
            return;
        }

        // A type named after a keyword or a built-in is reported and nothing else is: every later
        // rule would be describing a declaration the parser never really understood.
        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) ||
            sym.name == ctx.request.GetStringTypeName() || sym.name == ctx.request.GetArrayTypeName())
        {
            ctx.LogRule("ValidateClass", "as-err-reserved-keyword-name", sym);
            ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
            return;
        }

        if (sym.type == SymbolType::Interface)
        {
            if (HasInheritanceCycle(sym.qualifiedName.empty() ? sym.name : sym.qualifiedName,
                                    ctx.request.symbolTable))
            {
                ctx.LogRule("ValidateClass", "as-err-circular-inherit", sym);
                ctx.Emit(sym, "as-err-circular-inherit", sym.name);
            }
            return;
        }

        const auto &sig = sym.GetClass();

        // NOT IMPLEMENTED: as-err-template-class-not-supported.
        //
        // It would key off ClassSignature::isTemplate, which is never true. SymbolCollector sets it
        // from a "template_param" field that the tree-sitter grammar does not define - there is no
        // production for a template class declaration at all, so `class Holder<T>` parses with an
        // ERROR node over the `<T>` and is already reported as a syntax error by the parser pass.
        // Implementing the rule here would only duplicate that. Re-enable if the grammar ever
        // gains the production; the collector's detection needs fixing at the same time.

        CheckClassModifiers(sym, sig, ctx);
        CheckBases(sym, sig, ctx);
        CheckFinalOverrides(sym, sig, ctx);
        CheckInterfaceImplementation(sym, sig, ctx);
    }
}
