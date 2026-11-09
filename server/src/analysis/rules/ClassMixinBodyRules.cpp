#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/ClassRuleFacts.h"
#include "analysis/index/MixinSourceIndex.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/InterfaceImplementation.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/ClassRules.h"
#include "analysis/types/TypeSanitization.h"
#include "document/Document.h"
#include "spdlog/fmt/fmt.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Emits a diagnostic when a mixin refers to a member not present in the host.
 * @param[in] mctx Mixin check context containing host and mixin symbols.
 * @param[in] memberName Name of the missing member identifier.
 * @param[in] span Source span of the referenced member in the mixin source.
 * @param[in] ctx Diagnostic context for error emission.
 */
static void EmitMixinMemberNotFound(const MixinCheckContext& mctx, std::string_view memberName,
                                    const IndexedSourceSpan& span, const DiagnosticContext& ctx)
{
    DiagnosticRelatedInformation rel;
    rel.fileUri = mctx.mixinSym->fileUri;
    rel.range = {{span.startRow, span.startColumn}, {span.endRow, span.endColumn}};
    rel.message = i18n::FormatMessage(ctx.request.i18n, "note-in-mixin-member", "In mixin '{}': Member '{}'",
                                      std::string_view(mctx.mixinSym->name), memberName);

    RelatedDiagnosticRequest req;
    req.range = mctx.hostRange;
    req.code = diagnostics::codes::MixinInstantiationMemberNotFound;
    req.args = {mctx.mixinSym->name, mctx.hostClassName, memberName, mctx.hostClassName};
    req.related = std::move(rel);
    req.severity = DiagnosticSeverity::Error;
    ctx.EmitWithRelated(req);
}

/**
 * @brief Checks if a member set contains a getter or setter accessor for a property name.
 * @param[in] set Set of known member names.
 * @param[in] prefix Accessor prefix ("get_" or "set_").
 * @param[in] name Property name to append.
 * @return True when the prefixed accessor name is found in set.
 */
static bool ContainsAccessor(const MemberNameSet& set, std::string_view prefix, std::string_view name)
{
    fmt::memory_buffer buf;
    fmt::format_to(std::back_inserter(buf), "{}{}", prefix, name);
    return set.contains(std::string_view(buf.data(), buf.size()));
}

/**
 * @brief Checks member expressions of form this->member.
 * @param[in] ref Indexed mixin member reference representing this-member access.
 * @param[in] mixinSource Source text buffer of the mixin file.
 * @param[in,out] mctx Mixin check context containing host and mixin symbols.
 * @param[in] ctx Diagnostic context for error reporting.
 */
static void CheckThisMember(const IndexedMixinMemberRef& ref, std::string_view mixinSource, MixinCheckContext& mctx,
                            const DiagnosticContext& ctx)
{
    const std::string_view propName =
        mixinSource.substr(ref.span.bytes.startByte, ref.span.bytes.endByte - ref.span.bytes.startByte);
    if (!propName.empty() && !mctx.hostMembers.contains(propName) && !mctx.mixinSelfMembers.contains(propName) &&
        !ContainsAccessor(mctx.hostMembers, "get_", propName) &&
        !ContainsAccessor(mctx.mixinSelfMembers, "get_", propName) &&
        !ContainsAccessor(mctx.hostMembers, "set_", propName) &&
        !ContainsAccessor(mctx.mixinSelfMembers, "set_", propName) && !mctx.reported.contains(propName))
    {
        mctx.reported.insert(propName);
        EmitMixinMemberNotFound(mctx, propName, ref.span, ctx);
    }
}

/**
 * @brief Checks bare function calls without qualifiers.
 * @param[in] ref Indexed mixin member reference representing a function call.
 * @param[in] mixinSource Source text buffer of the mixin file.
 * @param[in,out] state Function check state tracking local names and mixin context.
 * @param[in] ctx Diagnostic context for error reporting.
 */
static void CheckCallMember(const IndexedMixinMemberRef& ref, std::string_view mixinSource, FunctionCheckState& state,
                            const DiagnosticContext& ctx)
{
    const std::string_view fnNameStr =
        mixinSource.substr(ref.span.bytes.startByte, ref.span.bytes.endByte - ref.span.bytes.startByte);
    if (!fnNameStr.empty() && !IsValidMixinIdentifier(fnNameStr, state, ctx))
    {
        if (!state.mctx.reported.contains(fnNameStr))
        {
            state.mctx.reported.insert(fnNameStr);
            EmitMixinMemberNotFound(state.mctx, fnNameStr, ref.span, ctx);
        }
    }
}

/**
 * @brief Checks bare identifier references inside mixin function bodies.
 * @param[in] ref Indexed mixin member reference representing an identifier.
 * @param[in] mixinSource Source text buffer of the mixin file.
 * @param[in,out] state Function check state tracking local names and mixin context.
 * @param[in] ctx Diagnostic context for error reporting.
 */
static void CheckIdentifierMember(const IndexedMixinMemberRef& ref, std::string_view mixinSource,
                                  FunctionCheckState& state, const DiagnosticContext& ctx)
{
    const std::string_view nameStr =
        mixinSource.substr(ref.span.bytes.startByte, ref.span.bytes.endByte - ref.span.bytes.startByte);
    if (!IsValidMixinIdentifier(nameStr, state, ctx))
    {
        if (!state.mctx.reported.contains(nameStr))
        {
            state.mctx.reported.insert(nameStr);
            EmitMixinMemberNotFound(state.mctx, nameStr, ref.span, ctx);
        }
    }
}

/**
 * @brief Checks mixin member references within extracted function bodies against host and mixin symbols.
 * @param[in] facts Precalculated mixin body facts including function declarations and member references.
 * @param[in] mixinSource Source text buffer of the mixin file.
 * @param[in,out] mctx Mixin check context containing host and mixin symbols.
 * @param[in] ctx Diagnostic context for error reporting.
 */
void CheckMixinFunctions(const MixinBodyFacts& facts, std::string_view mixinSource, MixinCheckContext& mctx,
                         const DiagnosticContext& ctx)
{
    for (const auto& func : facts.functions)
    {
        ankerl::unordered_dense::set<std::string_view> localNames;
        localNames.reserve(func.localNames.size());
        for (const auto& range : func.localNames)
        {
            localNames.insert(mixinSource.substr(range.startByte, range.endByte - range.startByte));
        }
        FunctionCheckState state{localNames, mctx};
        for (const auto& ref : func.refs)
        {
            switch (ref.kind)
            {
            case MixinRefKind::ThisMember:
                CheckThisMember(ref, mixinSource, mctx, ctx);
                break;
            case MixinRefKind::Call:
                CheckCallMember(ref, mixinSource, state, ctx);
                break;
            case MixinRefKind::Identifier:
                CheckIdentifierMember(ref, mixinSource, state, ctx);
                break;
            }
        }
    }
}

} // namespace angel_lsp::analysis::rules
