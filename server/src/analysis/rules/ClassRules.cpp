#include "analysis/rules/ClassRules.h"
#include "analysis/SemanticHelpers.h"

#include <algorithm>
#include <vector>

namespace angel_lsp::analysis::rules
{
    namespace
    {
        /** @brief Modifier and shape rules a class declaration must satisfy on its own. */
        void CheckClassModifiers(const Symbol &sym, const ClassSignature &sig, const DiagnosticContext &ctx)
        {
            // These three used to borrow as-syntax-error, whose message reads `Syntax error: "Foo"`.
            // None of them is one: the parser accepted the declaration and handed it over intact.
            // What the user needs told is which rule the declaration broke.

            // 'external shared class X;' is allowed to have no body.
            // A forward declaration 'class X;' is allowed if a full definition with a body exists.
            if (!sig.hasBraces && !sig.modifiers.isExternal)
            {
                bool hasFullDefinition = false;
                if (auto symsPtr = ctx.request.symbolTable.FindSymbolsPtr(sym.name))
                {
                    for (const auto &s : *symsPtr)
                    {
                        if (s.type == SymbolType::Class && s.GetClass().hasBraces)
                        {
                            hasFullDefinition = true;
                            break;
                        }
                    }
                }
                if (!hasFullDefinition)
                {
                    ctx.LogRule("CheckClassModifiers", "as-err-declaration-missing-body", sym);
                    ctx.Emit(sym, "as-err-declaration-missing-body", sym.name);
                }
            }

            if (sig.modifiers.isExternal && !sig.modifiers.isShared)
            {
                ctx.LogRule("CheckClassModifiers", "as-err-external-not-shared", sym);
                ctx.Emit(sym, "as-err-external-not-shared", sym.name);
            }

            if (sig.modifiers.isExternal && sig.modifiers.isShared)
            {
                bool hasFullSharedDefinition = false;
                if (auto symsPtr = ctx.request.symbolTable.FindSymbolsPtr(sym.name))
                {
                    for (const auto &s : *symsPtr)
                    {
                        if (s.type == SymbolType::Class && s.GetClass().hasBraces &&
                            s.GetClass().modifiers.isShared && !s.GetClass().modifiers.isExternal)
                        {
                            hasFullSharedDefinition = true;
                            break;
                        }
                    }
                }
                if (!hasFullSharedDefinition)
                {
                    ctx.LogRule("CheckClassModifiers", "as-err-external-not-found", sym);
                    ctx.Emit(sym, "as-err-external-not-found", sym.name);
                }
            }

            // REMOVED: a check for 'override'/'explicit' on a class. It could never fire. The
            // grammar's declaration_modifier is choice("shared", "external", "abstract", "final"),
            // so `override class Foo {}` does not parse as a class declaration at all and the
            // parser pass reports it. ClassSignature::modifiers.isOverride is unreachable for the
            // same reason ClassSignature::isTemplate is - see the note further down.

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
            if (ctx.request.GetRuleIndex().Members(sym.qualifiedName).hasNestedType)
            {
                ctx.LogRule("CheckClassModifiers", "as-err-mixin-child-type", sym);
                ctx.Emit(sym, "as-err-mixin-child-type", sym.name);
            }
        }

        /**
         * @brief Collects the method names a type actually provides, base *classes* included.
         * @note Interfaces in the hierarchy are skipped on purpose. They declare what must be
         *       implemented, not what is - counting their own declarations as implementations makes
         *       every class trivially satisfy every interface it names.
         */
        ankerl::unordered_dense::set<std::string> CollectImplementedMethodNames(const std::string &typeName,
                                                                               const SymbolTable &table,
                                                                               const RuleIndex &index)
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

                for (const auto &name : index.Members(ancestor).methodNames)
                {
                    names.insert(name);
                }
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
            // An abstract class or mixin is allowed to leave the interface to its own subclasses.
            if (sig.modifiers.isAbstract || sig.modifiers.isMixin)
            {
                return;
            }

            const SymbolTable &table = ctx.request.symbolTable;
            const std::string container = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            const auto implemented = CollectImplementedMethodNames(container, table, ctx.request.GetRuleIndex());

            ankerl::unordered_dense::set<std::string> interfacesToCheck;

            for (const auto &baseName : sig.bases)
            {
                const std::string cleanBase = CleanBaseType(baseName);
                const auto baseSymbols = table.FindSymbolsPtr(cleanBase);
                if (!baseSymbols)
                {
                    continue;
                }

                for (const auto &base : *baseSymbols)
                {
                    if (base.type == SymbolType::Interface)
                    {
                        interfacesToCheck.insert(cleanBase);
                    }
                    else if (base.type == SymbolType::Class && base.GetClass().modifiers.isMixin)
                    {
                        for (const auto &mixinAncestor : GetInheritedTypeHierarchy(cleanBase, table))
                        {
                            const auto mixinAncestorSyms = table.FindSymbolsPtr(mixinAncestor);
                            if (mixinAncestorSyms && std::any_of(mixinAncestorSyms->begin(), mixinAncestorSyms->end(),
                                [](const Symbol &s) { return s.type == SymbolType::Interface; }))
                            {
                                interfacesToCheck.insert(mixinAncestor);
                            }
                        }
                    }
                }
            }

            for (const auto &cleanBase : interfacesToCheck)
            {
                for (const auto &methodName : ctx.request.GetRuleIndex().Members(cleanBase).methodNames)
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

                for (const auto &methodName : ctx.request.GetRuleIndex().Members(cleanBase).finalMethodNames)
                {
                    const std::string derivedName = container.empty() ? methodName
                                                                      : container + "::" + methodName;
                    if (auto methodsPtr = table.FindSymbolsPtr(derivedName))
                    {
                        for (const auto &mSym : *methodsPtr)
                        {
                            if (mSym.type == SymbolType::Function)
                            {
                                ctx.LogRule("CheckFinalOverrides", "as-err-override-final-method", mSym);
                                ctx.Emit(mSym, "as-err-override-final-method", methodName, cleanBase);
                            }
                        }
                    }
                }
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
                            if (sig.modifiers.isMixin)
                            {
                                ctx.LogRule("CheckBases", "as-err-mixin-inherit-class", sym);
                                ctx.Emit(sym, "as-err-mixin-inherit-class", sym.name, cleanBase);
                            }
                        }
                        // A mixin among the bases is not an error and never was: that is how a
                        // mixin is included. `class weapon_p90 : ScriptBasePlayerWeaponEntity,
                        // CS16BASE::WeaponBase` is the idiom, hundreds of corpus files use it, and
                        // a real engine compiles it.
                        if (base.GetClass().modifiers.isFinal)
                        {
                            ctx.LogRule("CheckBases", "as-err-inherit-final", sym);
                            ctx.Emit(sym, "as-err-inherit-final", cleanBase);
                        }
                        break;
                    }
                }

                if (sawClassBase && !sig.modifiers.isMixin && ++classBaseCount > 1)
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

        /** @brief Reports property accessor get/set type mismatches within a class/interface. */
        void CheckPropertyAccessors(const Symbol &sym, const ClassSignature &sig, const DiagnosticContext &ctx)
        {
            const std::string container = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            if (container.empty())
            {
                return;
            }

            const SymbolTable &table = ctx.request.symbolTable;

            ankerl::unordered_dense::map<std::string, const Symbol *> getters;
            ankerl::unordered_dense::map<std::string, const Symbol *> setters;

            table.ForEachSymbolInFile(ctx.request.fileUri, [&](const std::string &qName, const std::vector<Symbol> &syms) {
                for (const auto &s : syms)
                {
                    if (s.containerName == container && s.type == SymbolType::Function &&
                        std::holds_alternative<FunctionSignature>(s.signature))
                    {
                        const auto &fn = s.GetFunction();
                        if (fn.modifiers.isProperty)
                        {
                            if (s.name.starts_with("get_") && s.name.size() > 4)
                            {
                                std::string propName = s.name.substr(4);
                                getters[propName] = &s;
                            }
                            else if (s.name.starts_with("set_") && s.name.size() > 4)
                            {
                                std::string propName = s.name.substr(4);
                                setters[propName] = &s;
                            }
                        }
                    }
                }
            });

            for (const auto &[propName, getSym] : getters)
            {
                auto it = setters.find(propName);
                if (it != setters.end())
                {
                    const Symbol *setSym = it->second;
                    std::string getRet = CleanBaseType(getSym->GetFunction().returnType);
                    std::string setParam = !setSym->GetFunction().parameters.empty()
                                               ? CleanBaseType(setSym->GetFunction().parameters.back().typeName)
                                               : "";
                    if (!getRet.empty() && !setParam.empty() && getRet != setParam)
                    {
                        ctx.LogRule("CheckPropertyAccessors", "as-err-property-type-mismatch", *setSym);
                        ctx.Emit(*setSym, "as-err-property-type-mismatch", propName, getRet, setParam);
                    }
                }
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
        const auto strType = ctx.request.GetStringTypeName();
        const auto arrType = ctx.request.GetArrayTypeName();
        const std::string_view effectiveStrType = strType.empty() ? std::string_view("string") : strType;
        const std::string_view effectiveArrType = arrType.empty() ? std::string_view("array") : arrType;
        if (IsReservedKeyword(sym.name) || IsPrimitiveTypeName(sym.name) ||
            sym.name == effectiveStrType || sym.name == effectiveArrType)
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

        // A script cannot declare a template class - the application registers template types, and a
        // predefined stub is where it writes them down. Stubs never reach here at all, thanks to the
        // early return above, which is precisely the distinction the message draws.
        //
        // Reachable only since the grammar gained the production. Before that `class Holder<T>`
        // parsed with an ERROR node over the `<T>`, so all the user got was a generic syntax error
        // pointing at the angle brackets rather than at the class.
        if (sig.isTemplate)
        {
            ctx.LogRule("ValidateClass", "as-err-template-class-not-supported", sym);
            ctx.Emit(sym, "as-err-template-class-not-supported", sym.name);
        }

        CheckClassModifiers(sym, sig, ctx);
        CheckBases(sym, sig, ctx);
        CheckFinalOverrides(sym, sig, ctx);
        CheckInterfaceImplementation(sym, sig, ctx);
        CheckPropertyAccessors(sym, sig, ctx);
    }
}
