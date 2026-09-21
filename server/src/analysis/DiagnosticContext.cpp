#include "analysis/DiagnosticContext.h"
#include "utils/LspLogger.h"
#include <cstdint>
#include <span>
#include <spdlog/fmt/fmt.h>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
void ApplyMessageFormatting(Diagnostic& diag, const i18n::I18n* i18n, std::string_view code,
                            std::span<const std::string_view> args)
{
    if (args.empty())
    {
        return;
    }

    const std::string codeStr(code);
    std::string pattern;
    if (i18n)
    {
        pattern = i18n->GetMessage(codeStr);
    }

    std::vector<std::string> argStrs;
    argStrs.reserve(args.size());
    for (const auto a : args)
    {
        argStrs.emplace_back(a);
    }

    if (!pattern.empty())
    {
        if (argStrs.size() == 1)
        {
            diag.message = fmt::format(fmt::runtime(pattern), argStrs[0]);
        }
        else if (argStrs.size() == 2)
        {
            diag.message = fmt::format(fmt::runtime(pattern), argStrs[0], argStrs[1]);
        }
        else if (argStrs.size() == 3)
        {
            diag.message = fmt::format(fmt::runtime(pattern), argStrs[0], argStrs[1], argStrs[2]);
        }
        else if (argStrs.size() >= 4)
        {
            diag.message = fmt::format(fmt::runtime(pattern), argStrs[0], argStrs[1], argStrs[2], argStrs[3]);
        }
    }

    if (diag.message.empty() || diag.message.starts_with("["))
    {
        std::string fallback = "[" + codeStr + "]";
        bool first = true;
        for (const auto& a : argStrs)
        {
            fallback += (first ? " " : ", ");
            fallback += a;
            first = false;
        }
        diag.message = fallback;
    }
}

void ApplyMessageFormatting(Diagnostic& diag, const i18n::I18n* i18n, std::string_view code,
                            std::initializer_list<std::string_view> args)
{
    ApplyMessageFormatting(diag, i18n, code, std::span<const std::string_view>(args.begin(), args.size()));
}

void ApplyMessageFormatting(Diagnostic& diag, const i18n::I18n* i18n, std::string_view code,
                            const std::vector<std::string>& args)
{
    std::vector<std::string_view> views;
    views.reserve(args.size());
    for (const auto& a : args)
    {
        views.emplace_back(a);
    }
    ApplyMessageFormatting(diag, i18n, code, views);
}

/**
 * @brief The column of `typeName` on `line`, searching from `fromColumn`.
 *
 * Returns the source line's own npos when the line is past the end of the document or the
 * name is not on it - a declaration split across lines, or a type spelled differently from
 * the base name the rule is complaining about.
 */
size_t ColumnOfTypeName(std::string_view sourceCode, uint32_t line, uint32_t fromColumn, std::string_view typeName)
{
    if (typeName.empty())
        return std::string_view::npos;

    size_t at = 0;
    for (uint32_t skipped = 0; skipped < line; ++skipped)
    {
        at = sourceCode.find('\n', at);
        if (at == std::string_view::npos)
            return std::string_view::npos;
        ++at;
    }

    size_t lineEnd = sourceCode.find('\n', at);
    if (lineEnd == std::string_view::npos)
        lineEnd = sourceCode.size();

    const std::string_view text = sourceCode.substr(at, lineEnd - at);
    if (fromColumn >= text.size())
        return std::string_view::npos;

    const size_t found = text.find(typeName, fromColumn);
    if (found == std::string_view::npos)
        return std::string_view::npos;

    // A name has to stand alone: `Foo` inside `FooBar` is a different type, and underlining
    // the first three characters of it would be worse than underlining the declaration.
    const bool leftClear =
        found == 0 || (!std::isalnum(static_cast<unsigned char>(text[found - 1])) && text[found - 1] != '_');
    const size_t after = found + typeName.size();
    const bool rightClear =
        after >= text.size() || (!std::isalnum(static_cast<unsigned char>(text[after])) && text[after] != '_');

    return (leftClear && rightClear) ? found : std::string_view::npos;
}
} // namespace

void DiagnosticContext::Append(Diagnostic&& diag) const
{
    // Only warnings move. An error stays an error at every mode - asEP_COMPILER_WARNINGS
    // decides what the compiler does with a warning, not whether it still refuses the file -
    // and a hint is this analyzer's own idea rather than anything the engine emits, so neither
    // is the engine's to suppress or promote.
    if (diag.severity == DiagnosticSeverity::Warning)
    {
        const int mode = request.CompilerWarningMode();
        if (mode == 0)
        {
            return;
        }
        if (mode == 2)
        {
            diag.severity = DiagnosticSeverity::Error;
        }
    }

    diagnostics.push_back(std::move(diag));
}

Diagnostic DiagnosticContext::CreateDiagnostic(SourceRange range, std::string_view fileUri, std::string_view code,
                                               DiagnosticSeverity severity) const
{
    Diagnostic diag;
    diag.range.start.line = range.startLine;
    diag.range.start.character = range.startCharacter;
    diag.range.end.line = range.endLine;
    diag.range.end.character = range.endCharacter;
    diag.severity = severity;

    std::string codeStr(code);
    if (request.severityOverrides)
    {
        auto it = request.severityOverrides->find(codeStr);
        if (it != request.severityOverrides->end())
        {
            diag.severity = it->second;
        }
    }
    diag.code = codeStr;
    diag.source = "AngelScript";
    diag.fileUri = std::string(fileUri);

    if (request.i18n)
    {
        diag.message = request.i18n->GetMessage(codeStr);
    }
    if (diag.message.empty())
    {
        diag.message = "[" + codeStr + "] Diagnostic code: " + codeStr;
    }

    return diag;
}

// --- Diagnostic Emission for Symbol ---

void DiagnosticContext::Emit(const Symbol& sym, std::string_view code, DiagnosticSeverity severity) const
{
    SourceRange range{sym.startLine, sym.startCharacter, sym.endLine, sym.endCharacter};
    Append(CreateDiagnostic(range, sym.fileUri, code, severity));
}

void DiagnosticContext::Emit(const Symbol& sym, std::string_view code, std::string_view arg1,
                             DiagnosticSeverity severity) const
{
    SourceRange range{sym.startLine, sym.startCharacter, sym.endLine, sym.endCharacter};
    Diagnostic diag = CreateDiagnostic(range, sym.fileUri, code, severity);
    ApplyMessageFormatting(diag, request.i18n, code, {arg1});
    Append(std::move(diag));
}

void DiagnosticContext::Emit(const Symbol& sym, std::string_view code, std::string_view arg1,
                             std::string_view arg2) const
{
    SourceRange range{sym.startLine, sym.startCharacter, sym.endLine, sym.endCharacter};
    Diagnostic diag = CreateDiagnostic(range, sym.fileUri, code, DiagnosticSeverity::Error);
    ApplyMessageFormatting(diag, request.i18n, code, {arg1, arg2});
    Append(std::move(diag));
}

void DiagnosticContext::Emit(const Symbol& sym, std::string_view code, std::initializer_list<std::string_view> args,
                             DiagnosticSeverity severity) const
{
    SourceRange range{sym.startLine, sym.startCharacter, sym.endLine, sym.endCharacter};
    Diagnostic diag = CreateDiagnostic(range, sym.fileUri, code, severity);
    ApplyMessageFormatting(diag, request.i18n, code, args);
    Append(std::move(diag));
}

// --- Diagnostic Emission for ParameterInformation ---

void DiagnosticContext::Emit(const ParameterInformation& param, const Symbol& parentSym, std::string_view code,
                             DiagnosticSeverity severity) const
{
    SourceRange range{param.startLine, param.startCharacter, param.endLine, param.endCharacter};
    Append(CreateDiagnostic(range, parentSym.fileUri, code, severity));
}

void DiagnosticContext::Emit(const ParameterInformation& param, const Symbol& parentSym, std::string_view code,
                             std::string_view arg1) const
{
    SourceRange range{param.startLine, param.startCharacter, param.endLine, param.endCharacter};
    Diagnostic diag = CreateDiagnostic(range, parentSym.fileUri, code, DiagnosticSeverity::Error);
    ApplyMessageFormatting(diag, request.i18n, code, {arg1});
    Append(std::move(diag));
}

void DiagnosticContext::Emit(const ParameterInformation& param, const Symbol& parentSym, std::string_view code,
                             std::initializer_list<std::string_view> args) const
{
    SourceRange range{param.startLine, param.startCharacter, param.endLine, param.endCharacter};
    Diagnostic diag = CreateDiagnostic(range, parentSym.fileUri, code, DiagnosticSeverity::Error);
    ApplyMessageFormatting(diag, request.i18n, code, args);
    Append(std::move(diag));
}

// --- Diagnostic Emission for SourceRange ---

void DiagnosticContext::EmitAtRange(const Symbol& parentSym, const SourceRange& range, std::string_view code,
                                    DiagnosticSeverity severity) const
{
    Append(CreateDiagnostic(range, parentSym.fileUri, code, severity));
}

void DiagnosticContext::EmitAtRange(SourceRange range, std::string_view code, DiagnosticSeverity severity) const
{
    Append(CreateDiagnostic(range, request.fileUri, code, severity));
}

void DiagnosticContext::EmitAtRange(SourceRange range, std::string_view code, std::string_view arg1,
                                    DiagnosticSeverity severity) const
{
    Diagnostic diag = CreateDiagnostic(range, request.fileUri, code, severity);
    ApplyMessageFormatting(diag, request.i18n, code, {arg1});
    Append(std::move(diag));
}

void DiagnosticContext::EmitAtRange(SourceRange range, std::string_view code, std::string_view arg1,
                                    std::string_view arg2) const
{
    Diagnostic diag = CreateDiagnostic(range, request.fileUri, code, DiagnosticSeverity::Error);
    ApplyMessageFormatting(diag, request.i18n, code, {arg1, arg2});
    Append(std::move(diag));
}

void DiagnosticContext::EmitAtRange(SourceRange range, std::string_view code,
                                    std::initializer_list<std::string_view> args, DiagnosticSeverity severity) const
{
    Diagnostic diag = CreateDiagnostic(range, request.fileUri, code, severity);
    ApplyMessageFormatting(diag, request.i18n, code, args);
    Append(std::move(diag));
}

// --- Diagnostic Emission with Related Information ---

void DiagnosticContext::EmitWithRelated(const RelatedDiagnosticRequest& req) const
{
    Diagnostic diag = CreateDiagnostic(req.range, request.fileUri, req.code, req.severity);
    ApplyMessageFormatting(diag, request.i18n, req.code, req.args);
    diag.relatedInformation.push_back(req.related);
    Append(std::move(diag));
}

// --- Diagnostic Emission at Type Name ---

void DiagnosticContext::EmitAtTypeName(const Symbol& sym, std::string_view code, std::string_view typeName,
                                       DiagnosticSeverity severity) const
{
    const size_t column =
        ColumnOfTypeName(request.sourceCode, sym.fullRange.startLine, sym.fullRange.startCharacter, typeName);

    if (column == std::string_view::npos)
    {
        Emit(sym, code, typeName, severity);
        return;
    }

    SourceRange range{sym.fullRange.startLine, static_cast<uint32_t>(column), sym.fullRange.startLine,
                      static_cast<uint32_t>(column + typeName.size())};
    EmitAtRange(range, code, typeName, severity);
}

void DiagnosticContext::EmitAtTypeName(const ParameterInformation& param, const Symbol& parentSym,
                                       std::string_view code, std::string_view typeName) const
{
    const size_t column = ColumnOfTypeName(request.sourceCode, param.startLine, param.startCharacter, typeName);

    if (column == std::string_view::npos)
    {
        Emit(param, parentSym, code, typeName);
        return;
    }

    SourceRange range{param.startLine, static_cast<uint32_t>(column), param.startLine,
                      static_cast<uint32_t>(column + typeName.size())};
    EmitAtRange(range, code, typeName, DiagnosticSeverity::Error);
}

// --- Debug Logging ---

void DiagnosticContext::LogRule(std::string_view ruleName, std::string_view code, const Symbol& sym) const
{
    if (!logger || !logger->IsDebugEnabled())
    {
        return;
    }

    logger->LogDebug(fmt::format("[SA-DEBUG] rule={:<35} code={:<35} sym={} container={}", ruleName, code, sym.name,
                                 sym.containerName));
}

void DiagnosticContext::LogParam(std::string_view ruleName, std::string_view code, const ParameterInformation& param,
                                 const Symbol& parentSym) const
{
    if (!logger || !logger->IsDebugEnabled())
    {
        return;
    }

    logger->LogDebug(fmt::format("[SA-DEBUG] rule={:<35} code={:<35} param={} parent={}", ruleName, code, param.name,
                                 parentSym.name));
}
} // namespace angel_lsp::analysis
