#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"

#include <tree_sitter/api.h>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Validates a function, method, constructor or destructor declaration.
 *
 * Declaration level only: the shape of the signature, the modifiers it carries and where it
 * sits. Nothing here reads the body - what a body does is the control-flow checker's work, and
 * what an expression inside it means is the conversion checker's.
 *
 * @param sym Function symbol to validate.
 * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
 */
void ValidateFunction(const Symbol& sym, const DiagnosticContext& ctx);

/**
 * @brief Validates a parameter list on its own.
 *
 * Split out because a funcdef carries the same ParameterInformation as a function and the same
 * rules apply to it, minus everything that presumes a body or a container.
 *
 * @param sym Symbol owning the list, used for the diagnostic's message and file.
 * @param parameters The list to validate.
 * @param isFuncdef True when the owner is a funcdef, which admits a bodiless declaration.
 */
void ValidateParameters(const Symbol& sym, const std::vector<ParameterInformation>& parameters, bool isFuncdef,
                        const DiagnosticContext& ctx);

} // namespace angel_lsp::analysis::rules

namespace angel_lsp::analysis
{
class NodeIndex;
}

namespace angel_lsp::analysis::rules
{
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
    std::string ownerName;         ///< Container's last segment, i.e. the bare class name.
    bool containerIsKnown = false; ///< False means nothing may be concluded from the container.
};
} // namespace angel_lsp::analysis::rules
