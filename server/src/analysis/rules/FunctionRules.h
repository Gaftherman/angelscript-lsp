#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"

#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Validates a function, method, constructor or destructor declaration.
 *        Declaration level only: the shape of the signature, the modifiers it carries and where it
 * @param sym Function symbol to validate.
 * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
 */
void ValidateFunction(const Symbol& sym, const DiagnosticContext& ctx);

/**
 * @brief Validates a parameter list on its own.
 *        Split out because a funcdef carries the same ParameterInformation as a function and the same
 * @param sym Symbol owning the list, used for the diagnostic's message and file.
 * @param parameters The list to validate.
 * @param isFuncdef True when the owner is a funcdef, which admits a bodiless declaration.
 */
void ValidateParameters(const Symbol& sym, const std::vector<ParameterInformation>& parameters, bool isFuncdef,
                        const DiagnosticContext& ctx);

/**
 * @brief Reports an anonymous function (lambda expression) using pre-indexed AST nodes.
 * @param nodeIndex Pre-indexed syntax tree nodes.
 * @param ctx Diagnostic sink.
 */
void ValidateStandaloneLambda(const analysis::NodeIndex& nodeIndex, const DiagnosticContext& ctx);

/**
 * @brief Validation context for function declarations, return types, and parameter modifiers.
 */
struct FunctionContext
{
    bool isMember = false;    ///< Declared inside a class or an interface.
    bool isInterface = false; ///< Declared inside an interface.
    bool isMixin = false;     ///< Declared inside a mixin class.
    bool isConstructor = false;
    bool isDestructor = false;
    std::string_view ownerName;    ///< Views the container name storage of the symbol.
    bool containerIsKnown = false; ///< False means nothing may be concluded from the container.
};
FunctionContext BuildFunctionContext(const Symbol& sym, const DiagnosticContext& ctx);

/**
 * @brief True when the type cannot be passed as &inout without unsafe references.
 */
bool IsDisallowedInOutType(const ParameterInformation& param, const SymbolTable& table);

void CheckBody(const Symbol& sym, const FunctionSignature& sig, const FunctionContext& fctx,
               const DiagnosticContext& ctx);

void CheckReturnType(const Symbol& sym, const FunctionSignature& sig, const FunctionContext& fctx,
                     const DiagnosticContext& ctx);

void CheckModifiers(const Symbol& sym, const FunctionSignature& sig, const FunctionContext& fctx,
                    const DiagnosticContext& ctx);

void CheckAttributes(const Symbol& sym, const FunctionSignature& sig, const FunctionContext& fctx,
                     const DiagnosticContext& ctx);

void CheckConstructorDestructor(const Symbol& sym, const FunctionSignature& sig, const FunctionContext& fctx,
                                const DiagnosticContext& ctx);

void CheckOverride(const Symbol& sym, const FunctionSignature& sig, const FunctionContext& fctx,
                   const DiagnosticContext& ctx);

void CheckExternal(const Symbol& sym, const FunctionSignature& sig, const DiagnosticContext& ctx);

} // namespace angel_lsp::analysis::rules
