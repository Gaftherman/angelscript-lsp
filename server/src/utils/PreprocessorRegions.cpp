#include "utils/PreprocessorRegions.h"

#include "utils/Utils.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

namespace angel_lsp::utils
{
/**
 * @brief Checks if character is a valid identifier character (alnum or underscore).
 * @param[in] c Character to test.
 * @return True if valid identifier char.
 */
static bool IsIdentifierChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/**
 * @brief Checks if character is a space or tab.
 * @param[in] c Character to test.
 * @return True if space or tab.
 */
static bool IsSpaceOrTab(char c)
{
    return c == ' ' || c == '\t';
}

/**
 * @brief Advances index past spaces and tabs.
 * @param[in] sourceCode Document text.
 * @param[in,out] state Line scanner state.
 */
static void SkipSpacesAndTabs(std::string_view sourceCode, LineScanState& state)
{
    const size_t n = sourceCode.size();
    while (state.index < n && IsSpaceOrTab(sourceCode[state.index]))
        ++state.index;
}

/**
 * @brief Advances index until newline or end of source.
 * @param[in] sourceCode Document text.
 * @param[in,out] state Line scanner state.
 */
static void SkipUntilNewline(std::string_view sourceCode, LineScanState& state)
{
    const size_t n = sourceCode.size();
    while (state.index < n && sourceCode[state.index] != '\n' && sourceCode[state.index] != '\r')
        ++state.index;
}

/**
 * @brief Advances scan state past a newline sequence.
 * @param[in] sourceCode Document text.
 * @param[in,out] state Line scanner state.
 * @return True if a newline was consumed.
 */
static bool HandleNewline(std::string_view sourceCode, LineScanState& state)
{
    const char c = sourceCode[state.index];
    if (c == '\r')
    {
        if (state.index + 1 < sourceCode.size() && sourceCode[state.index + 1] == '\n')
            ++state.index;
        ++state.currentLine;
        ++state.index;
        state.lineStart = state.index;
        state.atLineStart = true;
        return true;
    }
    if (c == '\n')
    {
        ++state.currentLine;
        ++state.index;
        state.lineStart = state.index;
        state.atLineStart = true;
        return true;
    }
    return false;
}

/**
 * @brief Advances scan state past a single-line comment.
 * @param[in] sourceCode Document text.
 * @param[in,out] state Line scanner state.
 */
static void SkipLineComment(std::string_view sourceCode, LineScanState& state)
{
    state.index += 2;
    SkipUntilNewline(sourceCode, state);
}

/**
 * @brief Advances scan state past a multi-line block comment.
 * @param[in] sourceCode Document text.
 * @param[in,out] state Line scanner state.
 */
static void SkipBlockComment(std::string_view sourceCode, LineScanState& state)
{
    state.index += 2;
    const size_t n = sourceCode.size();
    while (state.index < n)
    {
        if (sourceCode[state.index] == '\n')
        {
            ++state.currentLine;
            state.lineStart = state.index + 1;
            state.atLineStart = true;
        }
        else if (sourceCode[state.index] == '*' && state.index + 1 < n && sourceCode[state.index + 1] == '/')
        {
            state.index += 2;
            break;
        }
        ++state.index;
    }
}

/**
 * @brief Tries to skip a line comment or block comment.
 * @param[in] sourceCode Document text.
 * @param[in,out] state Line scanner state.
 * @return True if a comment was encountered and skipped.
 */
static bool TrySkipComment(std::string_view sourceCode, LineScanState& state)
{
    return TrySkipCommentDispatch(sourceCode, state, SkipLineComment, SkipBlockComment);
}

/**
 * @brief Advances scan state past a quoted string or character literal.
 * @param[in] sourceCode Document text.
 * @param[in,out] state Line scanner state.
 */
static void SkipHeredocLiteral(std::string_view sourceCode, LineScanState& state)
{
    state.index += 3;
    const size_t n = sourceCode.size();
    while (state.index < n)
    {
        if (sourceCode[state.index] == '\r')
        {
            if (state.index + 1 < n && sourceCode[state.index + 1] == '\n')
            {
                ++state.index;
            }
            ++state.currentLine;
            state.lineStart = state.index + 1;
            state.atLineStart = true;
        }
        else if (sourceCode[state.index] == '\n')
        {
            ++state.currentLine;
            state.lineStart = state.index + 1;
            state.atLineStart = true;
        }
        else if (sourceCode.substr(state.index).starts_with("\"\"\""))
        {
            state.index += 3;
            state.atLineStart = false;
            return;
        }
        ++state.index;
    }
}

static void SkipStringLiteral(std::string_view sourceCode, LineScanState& state)
{
    if (sourceCode.substr(state.index).starts_with("\"\"\""))
    {
        SkipHeredocLiteral(sourceCode, state);
        return;
    }
    const char quote = sourceCode[state.index];
    SkipEscapedStringLiteral(sourceCode, state.index, quote);
}

/**
 * @brief Parses a directive beginning with '#' at line start.
 * @param[in] sourceCode Document text.
 * @param[in,out] state Line scanner state.
 * @return DirectiveHit if a valid directive candidate was found.
 */
static std::optional<DirectiveHit> ParseDirectiveHit(std::string_view sourceCode, LineScanState& state)
{
    const size_t n = sourceCode.size();
    DirectiveHit hit;
    hit.line = state.currentLine;
    hit.startColumn = static_cast<uint32_t>(state.index - state.lineStart);
    ++state.index;

    if (state.index < n && sourceCode[state.index] == '!')
    {
        SkipUntilNewline(sourceCode, state);
        return std::nullopt;
    }

    const size_t afterHash = state.index;
    SkipSpacesAndTabs(sourceCode, state);
    hit.touchesHash = (state.index == afterHash);

    const size_t nameStart = state.index;
    while (state.index < n && IsIdentifierChar(sourceCode[state.index]))
        ++state.index;
    hit.name = sourceCode.substr(nameStart, state.index - nameStart);
    hit.endColumn = static_cast<uint32_t>(state.index - state.lineStart);

    SkipSpacesAndTabs(sourceCode, state);

    hit.firstArgChar = (state.index < n && sourceCode[state.index] != '\n' && sourceCode[state.index] != '\r')
                           ? sourceCode[state.index]
                           : char{0};

    const size_t argStart = state.index;
    while (state.index < n && IsIdentifierChar(sourceCode[state.index]))
        ++state.index;
    hit.argument = sourceCode.substr(argStart, state.index - argStart);

    SkipUntilNewline(sourceCode, state);
    return hit;
}

/**
 * @brief Walks a document and reports every directive that starts a line.
 * @param[in] sourceCode Document text.
 * @param[in] fn Callback invoked for each found directive.
 * @return The 0-based number of the last line scanned.
 */
template <typename Fn> static uint32_t ForEachDirective(std::string_view sourceCode, Fn&& fn)
{
    const size_t n = sourceCode.size();
    LineScanState state;

    while (state.index < n)
    {
        const char c = sourceCode[state.index];
        if (HandleNewline(sourceCode, state))
            continue;
        if (IsSpaceOrTab(c))
        {
            ++state.index;
            continue;
        }
        if (TrySkipComment(sourceCode, state))
            continue;
        if (c == '"' || c == '\'')
        {
            SkipStringLiteral(sourceCode, state);
            continue;
        }
        if (c == '#' && state.atLineStart)
        {
            if (auto hit = ParseDirectiveHit(sourceCode, state))
            {
                fn(*hit);
            }
            continue;
        }
        state.atLineStart = false;
        ++state.index;
    }

    return state.currentLine;
}

/**
 * @brief Checks if a directive name is one of the recognized preprocessor directives.
 * @param[in] candidate Name to test.
 * @return True if recognized.
 */
static bool IsKnownDirectiveName(std::string_view candidate)
{
    return candidate == "include" || candidate == "if" || candidate == "endif" || candidate == "pragma" ||
           candidate == "else" || candidate == "elif" || candidate == "ifdef" || candidate == "ifndef" ||
           candidate == "define";
}

/**
 * @brief Validates directive syntax and argument shape.
 * @param[in,out] ctx Preprocessor scan context.
 * @param[in] hit Found directive.
 * @param[in] reaches True if the directive is in a live code block.
 * @return True if syntax is valid, false otherwise.
 */
static bool ValidateDirectiveSyntax(PreprocessorScanContext& ctx, const DirectiveHit& hit, bool reaches)
{
    if (!hit.touchesHash || !IsKnownDirectiveName(hit.name))
    {
        if (reaches)
            ctx.Report(hit, hit.touchesHash ? DirectiveProblem::Unrecognised : DirectiveProblem::SpaceAfterHash);
        return false;
    }

    if (hit.name == "include" && hit.firstArgChar != '"' && hit.firstArgChar != '\'')
    {
        if (reaches)
            ctx.Report(hit, DirectiveProblem::IncludeNotQuoted);
        return false;
    }

    return true;
}

/**
 * @brief Checks if directive name opens a conditional region.
 * @param[in] name Directive name.
 * @param[in] ifdefSupport Whether host supports #ifdef/#ifndef.
 * @return True if opens region.
 */
static bool IsRegionOpener(std::string_view name, bool ifdefSupport)
{
    if (name == "if")
        return true;
    return ifdefSupport && (name == "ifdef" || name == "ifndef");
}

/**
 * @brief Checks if directive name is a supported branch directive (#else/#elif).
 * @param[in] name Directive name.
 * @param[in] features Configured host features.
 * @return True if supported branch directive.
 */
static bool IsBranchDirective(std::string_view name, const PreprocessorFeatures& features)
{
    return (features.elseSupport && name == "else") || (features.elifSupport && name == "elif");
}

/**
 * @brief Processes an `#if`, `#ifdef`, or `#ifndef` directive.
 * @param[in,out] ctx Preprocessor scan context.
 * @param[in] hit Found directive.
 * @param[in] atExcludedTop True if top of stack is excluded.
 */
static void ProcessOpenRegion(PreprocessorScanContext& ctx, const DirectiveHit& hit, bool atExcludedTop)
{
    if (atExcludedTop)
    {
        ++ctx.excludedNesting;
        return;
    }

    if (hit.argument.empty())
    {
        ctx.Report(hit, DirectiveProblem::Unsupported);
        ctx.sawMalformedIf = true;
        return;
    }

    const bool defined = ctx.IsDefined(hit.argument);
    const bool live = hit.name == "ifndef" ? !defined : defined;

    OpenDirective open;
    open.branchStart = hit.line;
    open.excluded = !live;
    ctx.stack.push_back(open);
}

/**
 * @brief Records a script `#define` word into local definition set.
 * @param[in,out] ctx Preprocessor scan context.
 * @param[in] word Word to define.
 */
static void ProcessDefine(PreprocessorScanContext& ctx, std::string_view word)
{
    if (!ctx.usingLocalWords)
    {
        ctx.localWords = ctx.definedWords;
        ctx.usingLocalWords = true;
    }
    ctx.localWords.insert(std::string(word));
}

/**
 * @brief Checks if a directive is unsupported given configured host features.
 * @param[in,out] ctx Preprocessor scan context.
 * @param[in] hit Found directive.
 * @param[in] reaches True if the directive is in a live code block.
 * @return True if unsupported and reported.
 */
static bool CheckUnsupportedDirective(PreprocessorScanContext& ctx, const DirectiveHit& hit, bool reaches)
{
    if (!reaches)
        return false;

    const auto& d = hit.name;
    const auto& f = ctx.features;
    if ((!f.elseSupport && d == "else") || (!f.elifSupport && d == "elif") ||
        (!f.ifdefSupport && (d == "ifdef" || d == "ifndef")) || (!f.defineInScripts && d == "define") ||
        (ctx.reportPragma && d == "pragma"))
    {
        ctx.Report(hit, DirectiveProblem::Unsupported);
        return true;
    }
    return false;
}

/**
 * @brief Processes an `#else` or `#elif` branch transition.
 * @param[in,out] ctx Preprocessor scan context.
 * @param[in] hit Found directive.
 */
static void ProcessBranch(PreprocessorScanContext& ctx, const DirectiveHit& hit)
{
    if (ctx.excludedNesting > 0 || ctx.stack.empty())
        return;

    OpenDirective& open = ctx.stack.back();
    ctx.CloseBranch(open, hit.line);

    const bool conditionHolds = hit.name == "else" || (!hit.argument.empty() && ctx.IsDefined(hit.argument));
    open.branchStart = hit.line;
    open.excluded = open.takenAlready || !conditionHolds;
}

/**
 * @brief Processes an `#endif` directive.
 * @param[in,out] ctx Preprocessor scan context.
 * @param[in] hit Found directive.
 */
static void ProcessEndif(PreprocessorScanContext& ctx, const DirectiveHit& hit)
{
    if (ctx.excludedNesting > 0)
    {
        --ctx.excludedNesting;
    }
    else if (!ctx.stack.empty())
    {
        OpenDirective open = ctx.stack.back();
        ctx.stack.pop_back();
        ctx.CloseBranch(open, hit.line);
    }
    else if (ctx.sawMalformedIf)
    {
        ctx.sawMalformedIf = false;
    }
    else
    {
        ctx.Report(hit, DirectiveProblem::Unsupported);
    }
}

/**
 * @brief Dispatches a directive hit to appropriate preprocessor logic.
 * @param[in,out] ctx Preprocessor scan context.
 * @param[in] hit Found directive.
 */
static void ProcessDirectiveHit(PreprocessorScanContext& ctx, const DirectiveHit& hit)
{
    const bool atExcludedTop = !ctx.stack.empty() && ctx.stack.back().excluded;
    const bool reaches = !atExcludedTop && ctx.excludedNesting == 0;

    if (!ValidateDirectiveSyntax(ctx, hit, reaches))
        return;

    if (IsRegionOpener(hit.name, ctx.features.ifdefSupport))
    {
        ProcessOpenRegion(ctx, hit, atExcludedTop);
    }
    else if (ctx.features.defineInScripts && hit.name == "define" && !hit.argument.empty() && reaches)
    {
        ProcessDefine(ctx, hit.argument);
    }
    else if (CheckUnsupportedDirective(ctx, hit, reaches))
    {
        // Reported as unsupported directive
    }
    else if (IsBranchDirective(hit.name, ctx.features))
    {
        ProcessBranch(ctx, hit);
    }
    else if (hit.name == "endif")
    {
        ProcessEndif(ctx, hit);
    }
}

PreprocessorScan ScanPreprocessor(std::string_view sourceCode, const TransparentStringSet& definedWords,
                                  const PreprocessorFeatures& features, bool reportPragma)
{
    PreprocessorScan scan;
    PreprocessorScanContext ctx{scan, {}, 0, {}, definedWords, features, false, false, reportPragma};

    const uint32_t lastLine =
        ForEachDirective(sourceCode, [&](const DirectiveHit& hit) { ProcessDirectiveHit(ctx, hit); });

    for (const auto& open : ctx.stack)
    {
        if (open.excluded)
            scan.excluded.push_back(ExcludedLineRange{open.branchStart, lastLine});
    }

    return scan;
}

std::vector<ExcludedLineRange> FindExcludedLineRanges(std::string_view sourceCode,
                                                      const TransparentStringSet& definedWords,
                                                      const PreprocessorFeatures& features)
{
    return ScanPreprocessor(sourceCode, definedWords, features).excluded;
}

std::vector<std::string> ScanDefinedWords(std::string_view sourceCode)
{
    std::vector<std::string> words;
    ForEachDirective(sourceCode,
                     [&](const DirectiveHit& hit)
                     {
                         if (hit.name == "define" && !hit.argument.empty())
                             words.emplace_back(hit.argument);
                     });
    return words;
}

bool IsLineExcluded(std::span<const ExcludedLineRange> ranges, uint32_t line)
{
    for (const auto& r : ranges)
    {
        if (line >= r.startLine && line <= r.endLine)
            return true;
    }
    return false;
}

std::string BlankExcludedLines(std::string_view sourceCode, std::span<const ExcludedLineRange> ranges)
{
    if (ranges.empty() || sourceCode.empty())
    {
        return std::string(sourceCode);
    }

    std::vector<ExcludedLineRange> sortedRanges(ranges.begin(), ranges.end());
    std::sort(sortedRanges.begin(), sortedRanges.end(),
              [](const ExcludedLineRange& a, const ExcludedLineRange& b) { return a.startLine < b.startLine; });

    std::string result(sourceCode);
    uint32_t currentLine = 0;
    size_t rangeIdx = 0;
    const size_t numRanges = sortedRanges.size();
    const size_t len = result.size();

    for (size_t i = 0; i < len; ++i)
    {
        const char c = result[i];
        if (c == '\r')
        {
            if (i + 1 < len && result[i + 1] == '\n')
            {
                ++i;
            }
            ++currentLine;
            while (rangeIdx < numRanges && sortedRanges[rangeIdx].endLine < currentLine)
            {
                ++rangeIdx;
            }
            continue;
        }
        if (c == '\n')
        {
            ++currentLine;
            while (rangeIdx < numRanges && sortedRanges[rangeIdx].endLine < currentLine)
            {
                ++rangeIdx;
            }
            continue;
        }

        if (rangeIdx < numRanges && currentLine >= sortedRanges[rangeIdx].startLine &&
            currentLine <= sortedRanges[rangeIdx].endLine)
        {
            result[i] = ' ';
        }
    }
    return result;
}
} // namespace angel_lsp::utils
