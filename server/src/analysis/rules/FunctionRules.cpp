#include "analysis/rules/FunctionRules.h"
#include "analysis/SemanticHelpers.h"

#include <algorithm>
#include <string>
#include <string_view>
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
            std::string ownerName;      ///< Container's last segment, i.e. the bare class name.
            bool containerIsKnown = false; ///< False means nothing may be concluded from the container.
        };

        FunctionContext BuildFunctionContext(const Symbol &sym, const DiagnosticContext &ctx)
        {
            FunctionContext fctx;
            if (sym.containerName.empty())
            {
                // No container at all is itself knowledge: this is a global function.
                fctx.containerIsKnown = true;
                return fctx;
            }

            const auto container = ctx.request.symbolTable.FindSymbolsPtr(sym.containerName);
            if (!container)
            {
                // The container did not resolve, so nothing is known about it - not even whether it
                // is a class. Treated as a non-member so no rule that presumes a class body fires.
                return fctx;
            }
            fctx.containerIsKnown = true;

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
                fctx.ownerName = sym.containerName;
                const size_t scope = fctx.ownerName.rfind("::");
                if (scope != std::string::npos)
                {
                    fctx.ownerName = fctx.ownerName.substr(scope + 2);
                }
                fctx.isConstructor = !fctx.isDestructor && sym.name == fctx.ownerName;
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
            if (sig.isImported && sig.hasBody)
            {
                ctx.LogRule("CheckBody", "as-err-import-has-body", sym);
                ctx.Emit(sym, "as-err-import-has-body", sym.name);
                return;
            }

            const bool mayOmitBody = sig.isInterfaceMethod || fctx.isInterface ||
                                     sig.modifiers.isExternal || sig.modifiers.isDelete ||
                                     fctx.isConstructor || fctx.isDestructor || sig.isImported;

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

            // Returning an abstract class or an interface by value means constructing one to
            // return, so the engine answers "Return type can't be 'Shape'" at the declaration.
            // Only a plain by-value return is judged: `Shape@` is the ordinary and correct way to
            // write this, and a reference return is left alone because whether one is legal at all
            // depends on asEP_ALLOW_UNSAFE_REFERENCES rather than on the type.
            const bool returnsByValue = sig.returnType.find('@') == std::string::npos &&
                                        sig.returnType.find('&') == std::string::npos &&
                                        !sig.modifiers.isReturnReference;
            if (returnsByValue && !sig.returnIsArray && sig.returnTemplateName.empty() &&
                ClassifyNonInstantiable(sig.returnBaseTypeName, ctx.request.symbolTable) != NonInstantiableKind::None)
            {
                ctx.LogRule("CheckReturnType", "as-err-return-not-instantiable", sym);
                ctx.Emit(sym, "as-err-return-not-instantiable", sig.returnBaseTypeName);
            }

            if (sig.isImported && !sig.returnBaseTypeName.empty() && sig.returnBaseTypeName != "void" &&
                !IsKnownType(sig.returnBaseTypeName, ctx))
            {
                ctx.LogRule("CheckReturnType", "as-err-unresolved-type", sym);
                ctx.Emit(sym, "as-err-unresolved-type", sig.returnBaseTypeName);
            }

            // NOT IMPLEMENTED: as-err-invalid-reference-return.
            //
            // Not for want of an engine option, as this comment used to claim. Compiled against a
            // real engine at its defaults, `int& GetRef() { return g_value; }` builds clean and
            // `int& Bad() { int local = 1; return local; }` answers "Not a valid reference" - so
            // the declaration is never what is wrong. What the engine rejects is the returned
            // expression, and judging that needs the return statement's type, not the signature's.
            // It belongs with the use-site rules, and waits on the expression resolver reaching
            // far enough to answer what a return statement yields.
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
            // Only judged when the container is known not to be a class: an unresolved container
            // means the analyzer cannot tell what this declaration is.
            if (!fctx.containerIsKnown || fctx.isMember)
            {
                return;
            }

            // `const` on a global function is a hard error - the engine's parser refuses the token
            // outright, answering "Instead found reserved keyword 'const'".
            if (sig.modifiers.isConst)
            {
                ctx.LogRule("CheckModifiers", "as-err-global-function-qualifiers", sym);
                ctx.Emit(sym, "as-err-global-function-qualifiers", sym.name);
            }

            // `override` and `final` are not. The engine accepts both on a global function and
            // silently ignores them, so calling it an error would be this analyzer inventing a rule
            // AngelScript does not have. Saying nothing would be worse - a global marked `override`
            // is almost always a method that lost its class - so it is a warning, which is what a
            // qualifier with no effect is worth.
            if (sig.modifiers.isOverride)
            {
                ctx.LogRule("CheckModifiers", "as-warn-global-function-attribute", sym);
                ctx.Emit(sym, "as-warn-global-function-attribute", "override", sym.name,
                         DiagnosticSeverity::Warning);
            }
            if (sig.modifiers.isFinal)
            {
                ctx.LogRule("CheckModifiers", "as-warn-global-function-attribute", sym);
                ctx.Emit(sym, "as-warn-global-function-attribute", "final", sym.name,
                         DiagnosticSeverity::Warning);
            }
        }

        // =============================================================================
        // Function attributes: explicit, property, delete
        //
        // The grammar accepts `override | final | explicit | property | delete` after any parameter
        // list, so where each one is actually allowed is a semantic question. The three rules below
        // were each read off the engine rather than off the manual: every case in them was compiled
        // with a real AngelScript build and the verdict recorded, which is how the arity limits and
        // the exemptions further down came to be what they are rather than what they look like.
        // =============================================================================

        /**
         * @brief True when the deleted declaration names a function the engine auto generates.
         *
         * Only three exist for a script class - the default constructor, the copy constructor, and
         * opAssign - and `delete` says "do not generate this one", so it is meaningless on anything
         * else; the engine answers "Cannot flag function that will not be auto generated as
         * deleted". Neither the reference modifier nor const nor the return type takes part in the
         * match: `A(A a)`, `A(A &inout)` and `A(const A &in)` are all accepted as the copy
         * constructor, and `void opAssign(const A &inout)` is accepted despite returning void. What
         * decides it is the arity and the parameter's type.
         */
        bool IsAutoGeneratedFunction(const Symbol &sym, const FunctionSignature &sig,
                                     const FunctionContext &fctx)
        {
            const bool takesOwnTypeOnce = sig.parameters.size() == 1 &&
                                          CleanBaseType(sig.parameters.front().baseTypeName) == fctx.ownerName;

            if (fctx.isConstructor)
            {
                return sig.parameters.empty() || takesOwnTypeOnce;
            }
            return sym.name == "opAssign" && takesOwnTypeOnce;
        }

        /** @brief True when a virtual property accessor's signature is one the engine accepts. */
        bool IsValidAccessorSignature(std::string_view name, const FunctionSignature &sig)
        {
            // An accessor may carry a leading index parameter, which is how `a[i].prop` and the
            // corpus's own `get_stringArray(int idx)` are written. That is the only reason a getter
            // has any parameters at all and the only reason a setter has two.
            constexpr size_t k_maxIndexParams = 1;

            if (name.rfind("get_", 0) == 0)
            {
                return sig.returnTypeKind != TypeKind::Void && sig.parameters.size() <= k_maxIndexParams;
            }
            if (name.rfind("set_", 0) == 0)
            {
                return sig.returnTypeKind == TypeKind::Void && !sig.parameters.empty() &&
                       sig.parameters.size() <= k_maxIndexParams + 1;
            }
            // Anything else is not an accessor at all, whatever it claims.
            return false;
        }

        void CheckAttributes(const Symbol &sym, const FunctionSignature &sig, const FunctionContext &fctx,
                             const DiagnosticContext &ctx)
        {
            // An interface declares a contract, not an implementation, and the engine rejects every
            // one of the five attributes on a method of one. The grammar parses them so this can
            // name the offender instead of leaving a syntax error on the token.
            if (sig.isInterfaceMethod)
            {
                const std::string_view attribute = FirstAttributeName(sig.modifiers);
                if (!attribute.empty())
                {
                    ctx.LogRule("CheckAttributes", "as-err-interface-method-attribute", sym);
                    ctx.Emit(sym, "as-err-interface-method-attribute", attribute, sym.name);
                }
                return;
            }

            // `explicit` suppresses an implicit conversion through a constructor, so it only means
            // anything inside a class body; the engine's parser refuses the token anywhere else.
            if (sig.modifiers.isExplicit && fctx.containerIsKnown && !fctx.isMember)
            {
                ctx.LogRule("CheckAttributes", "as-err-explicit-not-member", sym);
                ctx.Emit(sym, "as-err-explicit-not-member", sym.name);
            }

            if (sig.modifiers.isProperty && !IsValidAccessorSignature(sym.name, sig))
            {
                ctx.LogRule("CheckAttributes", "as-err-virtual-property-signature", sym);
                ctx.Emit(sym, "as-err-virtual-property-signature", sym.name);
            }

            // A destructor carrying `delete` is already reported, with a message that says so; this
            // one would only repeat it less precisely. The container has to be known too, since an
            // unresolved one leaves isConstructor false and would turn every deleted constructor in
            // an unreadable hierarchy into a finding.
            if (sig.modifiers.isDelete && fctx.containerIsKnown && !fctx.isDestructor &&
                !IsAutoGeneratedFunction(sym, sig, fctx))
            {
                ctx.LogRule("CheckAttributes", "as-err-delete-not-auto-generated", sym);
                ctx.Emit(sym, "as-err-delete-not-auto-generated", sym.name);
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
            const RuleIndex &index = ctx.request.GetRuleIndex();
            for (const auto &ancestor : hierarchy)
            {
                if (ancestor != sym.containerName)
                {
                    if (index.Members(ancestor).finalMethodNames.contains(sym.name))
                    {
                        // Handled by CheckFinalOverrides in ClassRules
                        return;
                    }
                    if (index.Members(ancestor).methodNames.contains(sym.name))
                    {
                        found = true;
                        break;
                    }
                }
            }

            if (!found)
            {
                ctx.LogRule("CheckOverride", "as-err-override-no-base", sym);
                ctx.Emit(sym, "as-err-override-no-base", sym.name, sym.containerName);
            }
        }

        void CheckExternal(const Symbol &sym, const FunctionSignature &sig, const DiagnosticContext &ctx)
        {
            if (sig.isImported)
            {
                return;
            }

            if (sig.modifiers.isExternal)
            {
                if (!sig.modifiers.isShared)
                {
                    ctx.LogRule("CheckExternal", "as-err-external-not-shared", sym);
                    ctx.Emit(sym, "as-err-external-not-shared", sym.name);
                }
                else
                {
                    bool hasFullSharedDefinition = false;
                    if (auto symsPtr = ctx.request.symbolTable.FindSymbolsPtr(sym.name))
                    {
                        for (const auto &s : *symsPtr)
                        {
                            if (s.type == SymbolType::Function && s.GetFunction().hasBody &&
                                s.GetFunction().modifiers.isShared && !s.GetFunction().modifiers.isExternal)
                            {
                                hasFullSharedDefinition = true;
                                break;
                            }
                        }
                    }
                    if (!hasFullSharedDefinition)
                    {
                        ctx.LogRule("CheckExternal", "as-err-external-not-found", sym);
                        ctx.Emit(sym, "as-err-external-not-found", sym.name);
                    }
                }
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

            // A bare `&` is `&inout` spelled shorter - the engine answers both with the very same
            // sentence, "Only object types that support object handles can use &inout", so both
            // arrive here. Verified against a real engine: `void f(int &x)` and `void f(int &inout
            // x)` are refused identically, while `void f(Foo &x)` on a script class is accepted.
            //
            // Unless the host turned asEP_ALLOW_UNSAFE_REFERENCES on, in which case a primitive by
            // reference is exactly what that option exists to permit and this rule would be
            // reporting a legal program. Only primitives are judged either way: whether a
            // registered type supports handles is not something script text says, and `string &x`
            // is an error for that reason while `Foo &x` is not.
            const bool isInOutReference = param.modifier == ParameterModifier::InOut || param.isStandaloneRef;
            if (isInOutReference && IsValueOnlyKind(param.typeKind) && !param.isArray &&
                !ctx.request.AllowsUnsafeReferences())
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

            // A by-value parameter of an abstract class or an interface is an instance the caller
            // has to make, so the engine refuses the signature itself rather than any call to it -
            // and refuses `const Shape &in` too, since a reference is still not a handle. It has a
            // message of its own here, naming the parameter's whole written type the way the
            // engine's does.
            // A template argument is left alone here for the same reason it is in VariableRules:
            // baseTypeName is the element, and the engine decides a subtype by its registered
            // factory rather than by abstractness.
            if (!param.isHandle && param.templateName.empty() && !param.isArray &&
                ClassifyNonInstantiable(param.baseTypeName, ctx.request.symbolTable) != NonInstantiableKind::None)
            {
                ctx.LogParam("ValidateParameters", "as-err-parameter-not-instantiable", param, sym);
                ctx.Emit(param, sym, "as-err-parameter-not-instantiable",
                         param.typeName.empty() ? param.baseTypeName : param.typeName);
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

            if (sym.type == SymbolType::Function && sym.GetFunction().isImported &&
                !param.baseTypeName.empty() && !IsKnownType(param.baseTypeName, ctx))
            {
                ctx.LogParam("ValidateParameters", "as-err-unresolved-type", param, sym);
                ctx.Emit(param, sym, "as-err-unresolved-type", param.baseTypeName);
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

        // NOT IMPLEMENTED: as-warn-shadow-global.
        //
        // Shadowing a global is legal AngelScript and extremely common in the corpus, where the
        // warning fires on ordinary parameter naming rather than on anything a user would want to
        // change. Unlike the reference rules above, no engine option decides it - it is a matter
        // of taste, and would belong behind a lint preference rather than a dialect one.
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
        CheckExternal(sym, sig, ctx);
        CheckReturnType(sym, sig, fctx, ctx);
        CheckModifiers(sym, sig, fctx, ctx);
        CheckAttributes(sym, sig, fctx, ctx);
        CheckConstructorDestructor(sym, sig, fctx, ctx);
        CheckOverride(sym, sig, fctx, ctx);
        ValidateParameters(sym, sig.parameters, false, ctx);
    }
}
