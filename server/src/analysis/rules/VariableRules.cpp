#include "analysis/rules/VariableRules.h"
#include "analysis/SemanticHelpers.h"
#include "spdlog/fmt/fmt.h"

#include <algorithm>
#include <string>
#include <vector>

namespace angel_lsp::analysis::rules
{
    namespace
    {
        /** @brief Looks up what kind of declaration a type name refers to, if this analyzer sees one. */
        const Symbol *FindTypeDeclaration(const std::string &typeName,
                                          const SymbolTable &table,
                                          std::shared_ptr<const std::vector<Symbol>> &keepAlive)
        {
            if (typeName.empty())
            {
                return nullptr;
            }

            keepAlive = table.FindSymbolsPtr(typeName);
            if (!keepAlive)
            {
                return nullptr;
            }

            for (const auto &sym : *keepAlive)
            {
                if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface ||
                    sym.type == SymbolType::Funcdef || sym.type == SymbolType::Enum ||
                    sym.type == SymbolType::Typedef)
                {
                    return &sym;
                }
            }
            return nullptr;
        }

        /** @brief True when the symbol's container is an interface, where members carry no body. */
        bool IsInterfaceMember(const Symbol &sym, const SymbolTable &table)
        {
            if (sym.containerName.empty())
            {
                return false;
            }
            const auto container = table.FindSymbolsPtr(sym.containerName);
            return container && std::any_of(container->begin(), container->end(),
                                            [](const Symbol &owner) { return owner.type == SymbolType::Interface; });
        }

        /** @brief True when the symbol's container is a mixin class. */
        bool IsMixinMember(const Symbol &sym, const SymbolTable &table)
        {
            if (sym.containerName.empty())
            {
                return false;
            }
            const auto container = table.FindSymbolsPtr(sym.containerName);
            return container && std::any_of(container->begin(), container->end(),
                                            [](const Symbol &owner)
                                            {
                                                return owner.type == SymbolType::Class &&
                                                       owner.GetClass().modifiers.isMixin;
                                            });
        }

        /**
         * @brief Rules about the arguments a template type is instantiated with.
         *
         * Only one argument is invalid for every template, and it is invalid whatever the host
         * registered: `void`. The engine answers "Attempting to instantiate invalid template
         * 'array<void>'" - the message this code has always carried - and accepts everything else
         * this analyzer can see, including a class, a handle, and a nested template.
         *
         * A text comparison rather than a resolution, deliberately. A template argument that
         * resolves to no declaration is an engine-registered type, which is most of them here, and
         * judging one would mean reporting every `array<CBasePlayer@>` in the corpus. `void` is the
         * one answer that needs nothing looked up.
         */
        void CheckTemplateArguments(const Symbol &sym, const VariableSignature &sig, const DiagnosticContext &ctx)
        {
            if (sig.templateName.empty())
            {
                return;
            }

            for (const auto &argument : sig.templateArgumentTypes)
            {
                if (CleanBaseType(argument) != "void")
                {
                    continue;
                }

                ctx.LogRule("CheckTemplateArguments", "as-err-array-invalid-template", sym);
                ctx.Emit(sym, "as-err-array-invalid-template",
                         fmt::format("{}<{}>", sig.templateName, argument));
            }
        }

        /** @brief Rules about what the declared type may be. */
        void CheckDeclaredType(const Symbol &sym, const VariableSignature &sig, const DiagnosticContext &ctx)
        {
            if (sig.typeKind == TypeKind::Void && !sig.modifiers.isHandle)
            {
                ctx.LogRule("CheckDeclaredType", "as-err-void-variable", sym);
                ctx.Emit(sym, "as-err-void-variable", sym.name);
            }

            if (sig.hasPrimitiveHandle)
            {
                ctx.LogRule("CheckDeclaredType", "as-err-handle-on-primitive", sym);
                ctx.Emit(sym, "as-err-handle-on-primitive", sig.baseTypeName);
            }

            const std::string baseType = CleanBaseType(sig.baseTypeName.empty() ? sig.typeName : sig.baseTypeName);
            std::shared_ptr<const std::vector<Symbol>> keepAlive;
            const Symbol *declaration = FindTypeDeclaration(baseType, ctx.request.symbolTable, keepAlive);
            if (!declaration)
            {
                // Unresolved is what an engine-registered type looks like, so nothing more is said
                // about this declaration's type.
                return;
            }

            if (declaration->type == SymbolType::Funcdef && !sig.modifiers.isHandle)
            {
                ctx.LogRule("CheckDeclaredType", "as-err-funcdef-not-handle", sym);
                ctx.Emit(sym, "as-err-funcdef-not-handle", baseType, baseType);
            }

            if (declaration->type == SymbolType::Class && declaration->GetClass().modifiers.isMixin)
            {
                ctx.LogRule("CheckDeclaredType", "as-err-mixin-not-a-type", sym);
                ctx.Emit(sym, "as-err-mixin-not-a-type", baseType);
            }

            // Declaring one by value makes an instance of it, which is the one thing an abstract
            // class and an interface exist not to allow. A handle is not an instance and is the
            // whole point of both, so `Shape@ s;` is legal - and a reference does not save it,
            // since even `const Shape &in` is refused as a parameter.
            //
            // A template instantiation is not judged at all. baseType is the *element* there,
            // because CleanBaseType unwraps `array<T>` to T, and the engine decides a subtype by
            // the factory registered for it rather than by whether it is abstract: `array<Shape>`
            // by value compiles, while `array<IThing>` fails with a message about a missing
            // default factory rather than about instantiating an interface. Found by the corpus
            // audit, which flagged `array<IContext@>` - ordinary, correct code.
            if (IsMixinClass(baseType, ctx.request.symbolTable))
            {
                ctx.LogRule("CheckDeclaredType", "as-err-mixin-not-a-type", sym);
                ctx.Emit(sym, "as-err-mixin-not-a-type", baseType);
                return;
            }

            if (!sig.modifiers.isHandle && sig.templateName.empty())
            {
                switch (ClassifyNonInstantiable(baseType, ctx.request.symbolTable))
                {
                    case NonInstantiableKind::Abstract:
                        ctx.LogRule("CheckDeclaredType", "as-err-abstract-instantiated", sym);
                        ctx.Emit(sym, "as-err-abstract-instantiated", baseType, baseType);
                        break;
                    case NonInstantiableKind::Interface:
                        ctx.LogRule("CheckDeclaredType", "as-err-interface-instantiated", sym);
                        ctx.Emit(sym, "as-err-interface-instantiated", baseType, baseType);
                        break;
                    case NonInstantiableKind::None:
                        break;
                }
            }

        }

        /** @brief Rules about the modifiers a declaration carries where it sits. */
        void CheckPlacement(const Symbol &sym, const VariableSignature &sig, const DiagnosticContext &ctx)
        {
            const bool isMember = !sym.containerName.empty();

            // private/protected mean nothing outside a class body.
            if (!isMember && sig.modifiers.access != AccessModifier::Public)
            {
                ctx.LogRule("CheckPlacement", "as-err-global-variable-access-modifier", sym);
                ctx.Emit(sym, "as-err-global-variable-access-modifier", sym.name);
            }

            // A host that gives each script its own isolated state builds its engine with
            // asEP_DISALLOW_GLOBAL_VARS, and then a global is a compile error rather than a style
            // question. Off by default, because the engine's default is off; a namespace member is
            // still a global as far as the engine is concerned, and containerName cannot tell a
            // namespace from a class, so only an unambiguous non-member is judged.
            if (!isMember && ctx.request.DisallowsGlobalVars())
            {
                ctx.LogRule("CheckPlacement", "as-err-global-vars-disallowed", sym);
                ctx.Emit(sym, "as-err-global-vars-disallowed", sym.name);
            }

            // A class property has no initialization phase in which a const could be given its
            // value, so AngelScript does not allow one. Two things it is not:
            //
            // - a namespace-scope const, which is ordinary and which the corpus is full of. A
            //   namespace and a class may carry the same name in one file, and containerName cannot
            //   tell them apart, so an ambiguous container is left alone;
            // - `const T@ m`, where the const qualifies what the handle points at rather than the
            //   member. A read-only handle to an object is legal and idiomatic.
            if (isMember && sig.modifiers.isConst && !sig.isVirtualProperty && !sig.modifiers.isHandle)
            {
                const SymbolTable &table = ctx.request.symbolTable;
                const auto container = table.FindSymbolsPtr(sym.containerName);
                bool inClass = false;
                bool ambiguous = false;
                if (container)
                {
                    for (const auto &owner : *container)
                    {
                        inClass = inClass || owner.type == SymbolType::Class;
                        ambiguous = ambiguous || owner.type == SymbolType::Namespace;
                    }
                }

                if (inClass && !ambiguous)
                {
                    ctx.LogRule("CheckPlacement", "as-err-class-member-const", sym);
                    ctx.Emit(sym, "as-err-class-member-const", sym.name);
                }
            }
        }

        /** @brief Rules for a virtual property's accessors. */
        void CheckVirtualProperty(const Symbol &sym, const VariableSignature &sig, const DiagnosticContext &ctx)
        {
            if (!sig.isVirtualProperty)
            {
                return;
            }

            // Two `get` blocks in one property is something the engine rejects outright, and the
            // collector has always worked it out - nothing read the answer until now. Independent
            // of the container, so it is checked before the mixin and interface exemptions.
            if (sig.hasDuplicateGet)
            {
                ctx.LogRule("CheckVirtualProperty", "as-err-property-duplicate-accessor", sym);
                ctx.Emit(sym, "as-err-property-duplicate-accessor", sym.name, "get");
            }
            if (sig.hasDuplicateSet)
            {
                ctx.LogRule("CheckVirtualProperty", "as-err-property-duplicate-accessor", sym);
                ctx.Emit(sym, "as-err-property-duplicate-accessor", sym.name, "set");
            }

            const SymbolTable &table = ctx.request.symbolTable;

            if (IsMixinMember(sym, table))
            {
                ctx.LogRule("CheckVirtualProperty", "as-err-mixin-virtual-property", sym);
                ctx.Emit(sym, "as-err-mixin-virtual-property", sym.name);
            }

            // An interface declares accessors without bodies by definition, and so does a stub.
            if (IsInterfaceMember(sym, table))
            {
                return;
            }

            if ((sig.hasGet && !sig.hasBodyGet) || (sig.hasSet && !sig.hasBodySet))
            {
                ctx.LogRule("CheckVirtualProperty", "as-err-property-accessor-missing-body", sym);
                ctx.Emit(sym, "as-err-property-accessor-missing-body", sym.name);
            }
        }

        void CheckModifiers(const Symbol &sym, const VariableSignature &sig, const DiagnosticContext &ctx)
        {
            if (sig.modifiers.isShared)
            {
                ctx.LogRule("CheckModifiers", "as-err-shared-not-allowed-on-entity", sym);
                ctx.Emit(sym, "as-err-shared-not-allowed-on-entity", sym.name);
            }
        }
    }

    void ValidateVariable(const Symbol &sym, const DiagnosticContext &ctx)
    {
        if (sym.type != SymbolType::Variable && sym.type != SymbolType::Property)
        {
            return;
        }
        if (IsFromPredefinedStub(sym, ctx))
        {
            return;
        }
        if (!std::holds_alternative<VariableSignature>(sym.signature))
        {
            return;
        }

        const auto &sig = sym.GetVariable();

        // Function-body locals are not reached through the symbol table at all; anything marked
        // local here came in some other way and is not this module's business.
        if (sig.isLocal)
        {
            return;
        }

        // NOT IMPLEMENTED: as-err-standalone-reference.
        //
        // A standalone reference is only detectable through ParameterInformation::isStandaloneRef,
        // which exists for parameters and has no VariableSignature counterpart - a variable
        // declared `int &x;` reaches the collector with nothing recording the ampersand. Whether it
        // is even an error depends on asEP_ALLOW_UNSAFE_REFERENCES, a host build option no reader
        // of script text can observe.

        CheckModifiers(sym, sig, ctx);
        CheckTemplateArguments(sym, sig, ctx);
        CheckDeclaredType(sym, sig, ctx);
        CheckPlacement(sym, sig, ctx);
        CheckVirtualProperty(sym, sig, ctx);
    }
}
