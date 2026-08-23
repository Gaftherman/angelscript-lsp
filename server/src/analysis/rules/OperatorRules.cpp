#include "analysis/rules/OperatorRules.h"
#include "analysis/SemanticHelpers.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace angel_lsp::analysis::rules
{
    namespace
    {
        /**
         * @brief The binary operator methods, each of which takes exactly one argument.
         *
         * The `_r` forms are the reversed-operand overloads the engine looks for when the left-hand
         * operand is not the object; they take one argument like the others.
         */
        constexpr std::array<std::string_view, 34> k_binaryOperators = {
            "opAdd", "opSub", "opMul", "opDiv", "opMod", "opPow",
            "opAnd", "opOr", "opXor", "opShl", "opShr", "opUShr",
            "opAdd_r", "opSub_r", "opMul_r", "opDiv_r", "opMod_r", "opPow_r",
            "opAnd_r", "opOr_r", "opXor_r", "opShl_r", "opShr_r", "opUShr_r",
            "opAddAssign", "opSubAssign", "opMulAssign", "opDivAssign", "opModAssign",
            "opPowAssign", "opAndAssign", "opOrAssign", "opXorAssign", "opAssign"
        };

        /** @brief The unary operator methods, which take no arguments. */
        constexpr std::array<std::string_view, 6> k_unaryOperators = {
            "opNeg", "opCom", "opPreInc", "opPreDec", "opPostInc", "opPostDec"
        };

        /** @brief The conversion operators, whose shape TypeConversionChecker already knows about. */
        constexpr std::array<std::string_view, 4> k_conversionOperators = {
            "opConv", "opImplConv", "opCast", "opImplCast"
        };

        bool Contains(const auto &names, const std::string &name)
        {
            return std::find(names.begin(), names.end(), name) != names.end();
        }

        /** @brief True when the name is one AngelScript gives a fixed meaning to. */
        bool IsOperatorName(const std::string &name)
        {
            return Contains(k_binaryOperators, name) || Contains(k_unaryOperators, name) ||
                   Contains(k_conversionOperators, name) ||
                   name == "opCmp" || name == "opEquals" || name == "opIndex" ||
                   name == "opForBegin" || name == "opForEnd" || name == "opForNext" ||
                   name == "opForValue" || name == "opCall" || name == "opHndlAssign";
        }

        /** @brief True when the symbol was declared in a predefined stub, which the rules exempt. */
        bool IsFromPredefinedStub(const Symbol &sym, const DiagnosticContext &ctx)
        {
            const std::string &extension = ctx.request.predefinedFileExtension;
            return !extension.empty() && sym.fileUri.ends_with(extension);
        }

        /** @brief True when the container resolves to a class or an interface this analyzer can read. */
        bool IsDeclaredInAClass(const Symbol &sym, const DiagnosticContext &ctx)
        {
            if (sym.containerName.empty())
            {
                return false;
            }
            const auto container = ctx.request.symbolTable.FindSymbolsPtr(sym.containerName);
            return container && std::any_of(container->begin(), container->end(),
                                            [](const Symbol &owner)
                                            {
                                                return owner.type == SymbolType::Class ||
                                                       owner.type == SymbolType::Interface;
                                            });
        }
    }

    void ValidateOperator(const Symbol &sym, const DiagnosticContext &ctx)
    {
        if (sym.type != SymbolType::Function || !IsOperatorName(sym.name))
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

        // An operator method only means anything as a member. Reported only when the container is
        // known: an unresolved container is a class this analyzer cannot see, not a global scope.
        const bool containerIsKnown = sym.containerName.empty() ||
                                      ctx.request.symbolTable.FindSymbolsPtr(sym.containerName) != nullptr;
        if (containerIsKnown && !IsDeclaredInAClass(sym, ctx))
        {
            ctx.LogRule("ValidateOperator", "as-err-op-overload-global", sym);
            ctx.Emit(sym, "as-err-op-overload-global", sym.name);
            return;
        }

        const size_t parameterCount = sig.parameters.size();

        if (sym.name == "opCmp")
        {
            if (sig.returnTypeKind != TypeKind::Int32 || sig.returnIsArray || sig.modifiers.isHandle)
            {
                ctx.LogRule("ValidateOperator", "as-err-opcmp-return-int", sym);
                ctx.Emit(sym, "as-err-opcmp-return-int");
            }
            if (parameterCount != 1)
            {
                ctx.LogRule("ValidateOperator", "as-err-binary-operator-arity", sym);
                ctx.Emit(sym, "as-err-binary-operator-arity", sym.name);
            }
            return;
        }

        if (sym.name == "opEquals")
        {
            if (sig.returnTypeKind != TypeKind::Bool || sig.returnIsArray || sig.modifiers.isHandle)
            {
                ctx.LogRule("ValidateOperator", "as-err-opequals-return-bool", sym);
                ctx.Emit(sym, "as-err-opequals-return-bool");
            }
            if (parameterCount != 1)
            {
                ctx.LogRule("ValidateOperator", "as-err-binary-operator-arity", sym);
                ctx.Emit(sym, "as-err-binary-operator-arity", sym.name);
            }
            return;
        }

        if (sym.name == "opIndex")
        {
            if (parameterCount == 0)
            {
                ctx.LogRule("ValidateOperator", "as-err-opindex-no-params", sym);
                ctx.Emit(sym, "as-err-opindex-no-params");
            }
            return;
        }

        if (Contains(k_binaryOperators, sym.name) && parameterCount != 1)
        {
            ctx.LogRule("ValidateOperator", "as-err-binary-operator-arity", sym);
            ctx.Emit(sym, "as-err-binary-operator-arity", sym.name);
        }
    }

    // NOT IMPLEMENTED: as-err-opindex-arity.
    //
    // Its message is word for word as-err-opindex-no-params', in both languages - the two codes say
    // the same sentence about the same condition. Emitting both would put the same finding on the
    // line twice, so only one is used and this one is left as the historical spelling.
    //
    // NOT IMPLEMENTED: an arity rule for the unary operators. opNeg and its neighbours take no
    // arguments, but nothing distinguishes "opPreInc declared wrong" from "a method that happens to
    // be called opPreInc and is never used as an operator", and the engine simply ignores the
    // latter rather than rejecting the script.
}
