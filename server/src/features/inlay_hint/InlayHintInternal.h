#pragma once

#include "analysis/documentation/InlayHintFormatter.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/InlayHintFacts.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/InlayCalleeResolution.h"
#include "features/common/FeatureSharedUtils.h"
#include "features/inlay_hint/InlayHintHandler.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Parameter binding and 0-based coordinates of a call argument for omitted default hints.
 */
struct CallArgPosition
{
    uint32_t startLine = 0;      ///< 0-based line coordinate of start of argument.
    uint32_t startCharacter = 0; ///< 0-based character coordinate of start of argument.
    uint32_t endLine = 0;        ///< 0-based line coordinate of end of argument.
    uint32_t endCharacter = 0;   ///< 0-based character coordinate of end of argument.
    int boundParamIndex = -1;    ///< Index of resolved callee parameter, or -1.
};

/**
 * @brief Metadata for omitted optional parameter hints.
 */
struct OmittedParamHint
{
    std::string_view fileUri{};                            ///< Document URI of callee definition.
    const analysis::ParameterInformation* param = nullptr; ///< Callee parameter metadata.
};

/**
 * @brief Context for generating parameter name inlay hints at a call site.
 */
struct ParamHintContext
{
    const analysis::ResolvedCallee& callee; ///< Resolved callee metadata and parameters.
    const InlayHintRequest& request;        ///< Top-level inlay hint request.
};

/**
 * @brief Resolves parameter label name, falling back to declared type for nameless parameters.
 * @param[in] param Function parameter information.
 * @return Resolved parameter label view.
 */
[[nodiscard]] std::string_view ResolveParameterLabel(const analysis::ParameterInformation& param) noexcept;

/**
 * @brief Checks whether the argument expression matches the parameter name.
 * @param[in] argText Argument source expression text.
 * @param[in] paramName Parameter label text.
 * @return True if argument expression trivially mirrors the parameter name.
 */
[[nodiscard]] bool ArgumentMatchesParameterName(std::string_view argText, std::string_view paramName) noexcept;

/**
 * @brief Resolves target location for an inlay hint parameter definition.
 * @param[in] fallbackUri Fallback document URI.
 * @param[in] calleeUri Declared URI of the callee.
 * @param[in] param Callee parameter information.
 * @param[in] fallbackRange Optional fallback location range if parameter has no valid range.
 * @return Resolved Location if parameter has valid range, std::nullopt otherwise.
 */
std::optional<lsp::Location>
ResolveParameterLocation(std::string_view fallbackUri, std::string_view calleeUri,
                         const analysis::ParameterInformation& param,
                         const std::optional<analysis::SourceRange>& fallbackRange = std::nullopt);

/**
 * @brief Constructs an ExpressionTypeContext for expression evaluation from InlayHintRequest.
 * @param[in] request Top-level inlay hint request.
 * @param[in] scope Optional resolved lexical scope.
 * @return Populated ExpressionTypeContext.
 */
[[nodiscard]] analysis::ExpressionTypeContext
MakeExpressionTypeContext(const InlayHintRequest& request, const analysis::Scope* scope = nullptr) noexcept;

/**
 * @brief Processes a function/method call site to produce parameter name and omitted default hints.
 * @param[in] call Extracted call site fact record.
 * @param[in] args Arguments slice for this call site.
 * @param[in] request Top-level inlay hint request.
 * @param[in,out] hints Hint collection receiving generated hints.
 */
void ProcessCallSite(const analysis::IndexedCallSite& call, std::span<const analysis::IndexedCallArgument> args,
                     const InlayHintRequest& request, std::vector<lsp::InlayHint>& hints);

/**
 * @brief Adds parameter hints and omitted default hints for constructor direct-initialization.
 * @param[in] callee Resolved constructor callee metadata.
 * @param[in] args Arguments passed to constructor.
 * @param[in] emptyArgsLine 0-based line coordinate for empty argument list hint.
 * @param[in] emptyArgsCharacter 0-based character coordinate for empty argument list hint.
 * @param[in] hasArgumentList True if argument list was syntactically present.
 * @param[in] request Top-level inlay hint request.
 * @param[in,out] hints Hint collection receiving generated hints.
 */
void ProcessConstructorInitParameters(const analysis::ResolvedCallee& callee,
                                      std::span<const analysis::IndexedCallArgument> args, uint32_t emptyArgsLine,
                                      uint32_t emptyArgsCharacter, bool hasArgumentList,
                                      const InlayHintRequest& request, std::vector<lsp::InlayHint>& hints);

/**
 * @brief Processes an auto variable declarator to produce deduced type hints.
 * @param[in] autoVar Extracted auto declarator fact record.
 * @param[in] request Top-level inlay hint request.
 * @param[in,out] hints Hint collection receiving generated hints.
 */
void ProcessAutoVariable(const analysis::IndexedAutoDeclarator& autoVar, const InlayHintRequest& request,
                         std::vector<lsp::InlayHint>& hints);

/**
 * @brief Processes a constructor direct-initialization declarator to resolve callee and parameter hints.
 * @param[in] ctorInit Extracted constructor init declarator fact record.
 * @param[in] args Arguments passed to constructor.
 * @param[in] request Top-level inlay hint request.
 * @param[in,out] hints Hint collection receiving generated hints.
 */
void ProcessConstructorInit(const analysis::IndexedConstructorInitDeclarator& ctorInit,
                            std::span<const analysis::IndexedCallArgument> args, const InlayHintRequest& request,
                            std::vector<lsp::InlayHint>& hints);

/**
 * @brief Processes a lambda expression to produce return type inlay hints.
 * @param[in] lambda Extracted lambda site fact record.
 * @param[in] request Top-level inlay hint request.
 * @param[in,out] hints Hint collection receiving generated hints.
 */
void ProcessLambda(const analysis::IndexedLambdaSite& lambda, const InlayHintRequest& request,
                   std::vector<lsp::InlayHint>& hints);

/**
 * @brief Processes enum member numbering facts to produce implicit value inlay hints.
 * @param[in] enumValues Slice of enum member facts belonging to one declaration.
 * @param[in] request Top-level inlay hint request.
 * @param[in,out] hints Hint collection receiving generated hints.
 */
void ProcessEnum(std::span<const analysis::IndexedEnumValue> enumValues, const InlayHintRequest& request,
                 std::vector<lsp::InlayHint>& hints);

} // namespace angel_lsp::features
