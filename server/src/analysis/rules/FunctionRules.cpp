#include "analysis/rules/FunctionRules.h"
#include "analysis/SemanticHelpers.h"

#include <algorithm>
#include <string>
#include <vector>

namespace angel_lsp::analysis::rules
{
    namespace
    {
        /** @brief Everything about the declaration's surroundings that more than one rule needs. */
        struct FunctionContext
        {
            bool isMember = false;      ///< Declared inside a class or an interface.
            bool isInterface = false;   ///< Declared inside an interface.
            bool isMixin = false;       ///< Declared inside a mixin class.
            bool isConstructor = false;
            bool isDestructor = false;
        };

        /** @brief True when the symbol was declared in a predefined stub, which the rules exempt. */
        bool IsFromPredefinedStub(const Symbol &sym, const DiagnosticContext &ctx)
        {
            const std::string &extension = ctx.request.predefinedFileExtension;
            return !extension.empty() && sym.fileUri.ends_with(extension);
        }

        FunctionContext BuildFunctionContext(const Symbol &sym, const DiagnosticContext &ctx)
        {
            FunctionContext fctx;
            if (sym.containerName.empty())
            {
                return fctx;
            }

            const auto container = ctx.request.symbolTable.FindSymbolsPtr(sym.containerName);
            if (!container)
            {
                // The container did not resolve, so nothing is known about it - not even whether it
                // is a class. Treated as a non-member so no rule that presumes a class body fires.
                return fctx;
            }

            for (const auto &owner : *container)
            {
                if (owner.type == SymbolType::Interface)
                {
                    fctx.isMember = true;
                    fctx.isInterface = true;
                }
                else if (owner.type == SymbolType::Class)
                {
                    fctx.isMember = true;
                    fctx.isMixin = fctx.isMixin || owner.GetClass().modifiers.isMixin;
                }
            }

            if (fctx.isMember)
            {
                fctx.isDestructor = IsDestructorDeclaration(sym, ctx);
                // A constructor is spelled with the class's own name. The container's qualified name
                // is what the collector stores, so compare against its last segment.
                std::string ownerName = sym.containerName;
                const size_t scope = ownerName.rfind("::");
                if (scope != std::string::npos)
                {
                    ownerName = ownerName.substr(scope + 2);
                }
                fctx.isConstructor = !fctx.isDestructor && sym.name == ownerName;
            }

            return fctx;
        }

        /** @brief True for the kinds that can never be passed or returned by reference. */
        bool IsValueOnlyKind(TypeKind kind)
        {
            switch (kind)
            {
                case TypeKind::Int8: case TypeKind::Int16: case TypeKind::Int32: case TypeKind::Int64:
                case TypeKind::UInt8: case TypeKind::UInt16: case TypeKind::UInt32: case TypeKind::UInt64:
                case TypeKind::Float: case TypeKind::Double: case TypeKind::Bool:
                    return true;
                default:
                    return false;
            }
        }

        // =============================================================================
        // Body
        // =============================================================================

        /**
         * @brief True when a bodiless declaration cannot be read as a variable with a constructor
         *        initializer, and so really is a function prototype.
         *
         * `array<string> g_names(count);` and `void Think();` are the same shape to the grammar,
         * which resolves the ambiguity towards a function - so every declaration with a constructor
         * initializer arrives here looking like a body-less function. AngelScript has no prototypes
         * at all, which means the only safe reading is the variable one unless the declaration
         * carries something no variable could:
         *
         * - a `void` return type, which no variable may have;
         * - a named parameter, since `int radius` is not an expression;
         * - a parameter typed `void`, likewise. Note the collector erases a lone unnamed `void`,
         *   the C spelling of "no parameters", before the list ever reaches here.
         *
         * Without one of those, staying silent is the only correct answer: the corpus declares
         * globals and fields this way throughout, and every one of them is ordinary AngelScript.
         */
        bool IsUnmistakablyAPrototype(const FunctionSignature &sig)
        {
            if (sig.returnTypeKind == TypeKind::Void)
            {
                return true;
            }
            return std::any_of(sig.parameters.begin(), sig.parameters.end(),
                               [](const ParameterInformation &param)
                               {
                                   return !param.name.empty() || param.typeKind == TypeKind::Void;
                               });
        }

        void CheckBody(const Symbol &sym, const FunctionSignature &sig, const FunctionContext &fctx,
                       const DiagnosticContext &ctx)
        {
            const bool mayOmitBody = sig.isInterfaceMethod || fctx.isInterface ||
                                     sig.modifiers.isExternal || sig.modifiers.isDelete ||
                                     fctx.isConstructor || fctx.isDestructor;

            if (!sig.hasBody && !mayOmitBody && IsUnmistakablyAPrototype(sig))
            {
                ctx.LogRule("CheckBody", "as-err-missing-body", sym);
                ctx.Emit(sym, "as-err-missing-body", sym.name);
            }

            if (sig.hasBody && sig.modifiers.isDelete)
            {
                ctx.LogRule("CheckBody", "as-err-delete-with-body", sym);
                ctx.Emit(sym, "as-err-delete-with-body", sym.name);
            }

            if (sig.modifiers.isDelete &&
                (sig.modifiers.isConst || sig.modifiers.isFinal || sig.modifiers.isOverride))
            {
                ctx.LogRule("CheckBody", "as-err-delete-with-other-qualifier", sym);
                ctx.Emit(sym, "as-err-delete-with-other-qualifier", sym.name);
            }
        }

        // =============================================================================
        // Return type
        // =============================================================================

        void CheckReturnType(const Symbol &sym, const FunctionSignature &sig, const FunctionContext &fctx,
                             const DiagnosticContext &ctx)
        {
            // A constructor and a destructor have no return type to judge, and what the collector
            // put in returnType for them is whatever preceded the name.
            if (fctx.isConstructor || fctx.isDestructor)
            {
                return;
            }

            if (sig.returnTypeKind == TypeKind::Void && sig.returnIsConst)
            {
                ctx.LogRule("CheckReturnType", "as-err-const-void-return", sym);
                ctx.Emit(sym, "as-err-const-void-return");
            }

            if (sig.returnTypeKind == TypeKind::Void && sig.modifiers.isReturnReference)
            {
                ctx.LogRule("CheckReturnType", "as-err-void-reference", sym);
                ctx.Emit(sym, "as-err-void-reference");
            }

            if (sig.returnHasPrimitiveHandle && !sig.returnIsArray)
            {
                ctx.LogRule("CheckReturnType", "as-err-handle-on-primitive", sym);
                ctx.Emit(sym, "as-err-handle-on-primitive", sig.returnBaseTypeName);
            }

            if (IsMixinClass(sig.returnBaseTypeName, ctx.request.symbolTable))
            {
                ctx.LogRule("CheckReturnType", "as-err-mixin-not-a-type", sym);
                ctx.Emit(sym, "as-err-mixin-not-a-type", sig.returnBaseTypeName);
            }

            // NOT IMPLEMENTED: as-err-invalid-reference-return.
            //
            // Returning `int&` is legal AngelScript whenever the host built the engine with
            // asEP_ALLOW_UNSAFE_REFERENCES, which is a host build option no analyzer reading only
            // script text can observe. The engine's own documentation uses `int &Function()` as a
            // worked example, and that example is in this corpus - so the rule would report the
            // language's own manual. Same reasoning retires as-err-standalone-reference in
            // ValidateParameters.
            //
            // NOT IMPLEMENTED: as-err-unresolved-type on the return type.
            //
            // An engine-registered type and a typo look identical from here - neither resolves to a
            // declaration this analyzer can read. The corpus is nothing but engine types, so the
            // rule would report a finding on virtually every function in it. Reporting an unknown
            // type is only decidable once the workspace's stubs are known to be complete, which is
            // not something a single document's analysis can establish.
        }

        // =============================================================================
        // Modifiers and placement
        // =============================================================================

        void CheckModifiers(const Symbol &sym, const FunctionSignature &sig, const FunctionContext &fctx,
                            const DiagnosticContext &ctx)
        {
            // const/override/final describe a method's relationship to a class. Outside one they
            // describe nothing. Only reported when the container is known not to be a class: an
            // unresolved container means the analyzer cannot tell.
            const bool containerIsKnown = sym.containerName.empty() ||
                                          ctx.request.symbolTable.FindSymbolsPtr(sym.containerName) != nullptr;
            if (containerIsKnown && !fctx.isMember &&
                (sig.modifiers.isConst || sig.modifiers.isOverride || sig.modifiers.isFinal))
            {
                ctx.LogRule("CheckModifiers", "as-err-global-function-qualifiers", sym);
                ctx.Emit(sym, "as-err-global-function-qualifiers", sym.name);
            }
        }

        // =============================================================================
        // Constructors and destructors
        // =============================================================================

        void CheckConstructorDestructor(const Symbol &sym, const FunctionSignature &sig,
                                        const FunctionContext &fctx, const DiagnosticContext &ctx)
        {
            if (fctx.isMixin && fctx.isConstructor)
            {
                ctx.LogRule("CheckConstructorDestructor", "as-err-mixin-constructor", sym);
                ctx.Emit(sym, "as-err-mixin-constructor", sym.containerName);
            }

            if (!fctx.isDestructor)
            {
                return;
            }

            if (fctx.isMixin)
            {
                ctx.LogRule("CheckConstructorDestructor", "as-err-mixin-destructor", sym);
                ctx.Emit(sym, "as-err-mixin-destructor", sym.containerName);
            }

            if (!sig.parameters.empty())
            {
                ctx.LogRule("CheckConstructorDestructor", "as-err-destructor-param", sym);
                ctx.Emit(sym, "as-err-destructor-param", sym.name);
            }

            if (!sig.returnType.empty())
            {
                ctx.LogRule("CheckConstructorDestructor", "as-err-destructor-return-type", sym);
                ctx.Emit(sym, "as-err-destructor-return-type", sym.name);
            }

            if (sig.modifiers.isDelete)
            {
                ctx.LogRule("CheckConstructorDestructor", "as-err-destructor-delete", sym);
                ctx.Emit(sym, "as-err-destructor-delete", sym.name);
            }
        }

        // =============================================================================
        // Override
        // =============================================================================

        /** @brief True when every type in the container's inheritance chain resolves to a declaration. */
        bool HierarchyIsFullyVisible(const std::string &typeName, const SymbolTable &table)
        {
            for (const auto &ancestor : GetInheritedTypeHierarchy(typeName, table))
            {
                const auto symbols = table.FindSymbolsPtr(ancestor);
                if (!symbols)
                {
                    return false;
                }
                for (const auto &sym : *symbols)
                {
                    if (sym.type == SymbolType::Class)
                    {
                        for (const auto &base : sym.GetClass().bases)
                        {
                            if (!table.FindSymbolsPtr(CleanBaseType(base)))
                            {
                                return false;
                            }
                        }
                    }
                }
            }
            return true;
        }

        void CheckOverride(const Symbol &sym, const FunctionSignature &sig, const FunctionContext &fctx,
                           const DiagnosticContext &ctx)
        {
            if (!sig.modifiers.isOverride || !fctx.isMember || fctx.isDestructor)
            {
                return;
            }

            const SymbolTable &table = ctx.request.symbolTable;

            // The whole point of the rule is that no base declares the method, so it may only speak
            // when every base is visible. One unresolved link - a class from a file outside the
            // workspace, or a host type - and the answer is unknowable rather than "no".
            if (!HierarchyIsFullyVisible(sym.containerName, table))
            {
                return;
            }

            const auto hierarchy = GetInheritedTypeHierarchy(sym.containerName, table);
            if (hierarchy.size() <= 1)
            {
                // No bases at all. `override` on such a method is meaningless whatever it names.
                ctx.LogRule("CheckOverride", "as-err-override-no-base", sym);
                ctx.Emit(sym, "as-err-override-no-base", sym.name, sym.containerName);
                return;
            }

            bool found = false;
            table.ForEachSymbol(
                [&](const std::string &, const std::vector<Symbol> &symbols)
                {
                    if (found)
                    {
                        return;
                    }
                    for (const auto &member : symbols)
                    {
                        if (member.type != SymbolType::Function || member.name != sym.name)
                        {
                            continue;
                        }
                        if (member.containerName == sym.containerName)
                        {
                            continue;
                        }
                        if (std::find(hierarchy.begin(), hierarchy.end(), member.containerName) != hierarchy.end())
                        {
                            found = true;
                            return;
                        }
                    }
                });

            if (!found)
            {
                ctx.LogRule("CheckOverride", "as-err-override-no-base", sym);
                ctx.Emit(sym, "as-err-override-no-base", sym.name, sym.containerName);
            }
        }
    }

    // =============================================================================
    // Parameters
    // =============================================================================

    void ValidateParameters(const Symbol &sym,
                            const std::vector<ParameterInformation> &parameters,
                            bool isFuncdef,
                            const DiagnosticContext &ctx)
    {
        ankerl::unordered_dense::set<std::string> seenNames;
        bool seenDefault = false;

        for (const auto &param : parameters)
        {
            if (!param.defaultValue.empty())
            {
                seenDefault = true;
            }
            else if (seenDefault && !isFuncdef)
            {
                ctx.LogParam("ValidateParameters", "as-err-default-param-order", param, sym);
                ctx.Emit(param, sym, "as-err-default-param-order", sym.name);
            }

            // `void` alone in the list is the C spelling of "no parameters" and the grammar accepts
            // it; anything else typed void is an error.
            if (param.typeKind == TypeKind::Void && !(parameters.size() == 1 && param.name.empty()))
            {
                ctx.LogParam("ValidateParameters", "as-err-void-parameter", param, sym);
                ctx.Emit(param, sym, "as-err-void-parameter", param.name, sym.name);
            }

            // An &out parameter is written by the callee, so the caller has to supply something to
            // write to - a default value has nothing to be written back into, and const forbids
            // the write outright.
            if (param.modifier == ParameterModifier::Out)
            {
                if (param.isConst)
                {
                    ctx.LogParam("ValidateParameters", "as-err-const-out-param", param, sym);
                    ctx.Emit(param, sym, "as-err-const-out-param", param.name);
                }
                // `= void` is the one default an &out parameter may carry: it is AngelScript's
                // spelling of "the caller may leave this argument out and discard the value", and
                // the engine's own documentation uses `void func(int &out output = void)`. Any
                // other default has nothing to write back into.
                if (!param.defaultValue.empty() && CleanBaseType(param.defaultValue) != "void")
                {
                    ctx.LogParam("ValidateParameters", "as-err-out-param-default", param, sym);
                    ctx.Emit(param, sym, "as-err-out-param-default", param.name);
                }
            }

            if (param.modifier == ParameterModifier::InOut && IsValueOnlyKind(param.typeKind) && !param.isArray)
            {
                ctx.LogParam("ValidateParameters", "as-err-inout-on-primitive", param, sym);
                ctx.Emit(param, sym, "as-err-inout-on-primitive", param.baseTypeName);
            }

            if (param.hasDoubleReference)
            {
                ctx.LogParam("ValidateParameters", "as-err-double-reference", param, sym);
                ctx.Emit(param, sym, "as-err-double-reference", param.baseTypeName);
            }

            if (param.hasPrimitiveHandle)
            {
                ctx.LogParam("ValidateParameters", "as-err-handle-on-primitive", param, sym);
                ctx.Emit(param, sym, "as-err-handle-on-primitive", param.baseTypeName);
            }

            if (IsMixinClass(param.baseTypeName, ctx.request.symbolTable))
            {
                ctx.LogParam("ValidateParameters", "as-err-mixin-not-a-type", param, sym);
                ctx.Emit(param, sym, "as-err-mixin-not-a-type", param.baseTypeName);
            }

            if (!param.isHandle && !param.baseTypeName.empty())
            {
                const auto typeSymbols = ctx.request.symbolTable.FindSymbolsPtr(param.baseTypeName);
                const bool isFuncdefType = typeSymbols &&
                    std::any_of(typeSymbols->begin(), typeSymbols->end(),
                                [](const Symbol &type) { return type.type == SymbolType::Funcdef; });
                if (isFuncdefType)
                {
                    ctx.LogParam("ValidateParameters", "as-err-funcdef-not-handle", param, sym);
                    ctx.Emit(param, sym, "as-err-funcdef-not-handle", param.baseTypeName, param.baseTypeName);
                }
            }

            if (param.name.empty())
            {
                continue;
            }

            if (!seenNames.insert(param.name).second)
            {
                ctx.LogParam("ValidateParameters", "as-err-duplicate-param", param, sym);
                ctx.Emit(param, sym, "as-err-duplicate-param", param.name, sym.name);
            }
        }

        // NOT IMPLEMENTED: as-err-standalone-reference and as-warn-shadow-global.
        //
        // A bare `&` on a parameter is only invalid when the engine was built without unsafe
        // references, which is a host build option this analyzer cannot observe - and the corpus
        // runs on an engine that permits it. Shadowing a global is legal AngelScript and extremely
        // common in the corpus, where the warning fires on ordinary parameter naming rather than on
        // anything a user would want to change.
    }

    void ValidateFunction(const Symbol &sym, const DiagnosticContext &ctx)
    {
        if (sym.type != SymbolType::Function)
        {
            return;
        }
        if (IsFromPredefinedStub(sym, ctx))
        {
            return;
        }
        if (!std::holds_alternative<FunctionSignature>(sym.signature))
        {
            return;
        }

        const auto &sig = sym.GetFunction();
        const FunctionContext fctx = BuildFunctionContext(sym, ctx);

        CheckBody(sym, sig, fctx, ctx);
        CheckReturnType(sym, sig, fctx, ctx);
        CheckModifiers(sym, sig, fctx, ctx);
        CheckConstructorDestructor(sym, sig, fctx, ctx);
        CheckOverride(sym, sig, fctx, ctx);
        ValidateParameters(sym, sig.parameters, false, ctx);
    }
}
