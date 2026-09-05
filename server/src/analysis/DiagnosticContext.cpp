#include "analysis/DiagnosticContext.h"
#include "utils/LspLogger.h"
#include <spdlog/fmt/fmt.h>

namespace angel_lsp::analysis
{
    void DiagnosticContext::Append(Diagnostic &&diag) const
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

    Diagnostic DiagnosticContext::CreateDiagnostic(const Symbol &sym, std::string_view code, DiagnosticSeverity severity) const
    {
        Diagnostic diag;
        diag.range.start.line = sym.startLine;
        diag.range.start.character = sym.startCharacter;
        diag.range.end.line = sym.endLine;
        diag.range.end.character = sym.endCharacter;
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
        diag.fileUri = sym.fileUri;

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

    Diagnostic DiagnosticContext::CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, std::string_view code, DiagnosticSeverity severity) const
    {
        Diagnostic diag;
        diag.range.start.line = param.startLine;
        diag.range.start.character = param.startCharacter;
        diag.range.end.line = param.endLine;
        diag.range.end.character = param.endCharacter;
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
        diag.fileUri = parentSym.fileUri;

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

    void DiagnosticContext::Emit(const Symbol &sym, std::string_view code, DiagnosticSeverity severity) const
    {
        Append(CreateDiagnostic(sym, code, severity));
    }

    void DiagnosticContext::Emit(const Symbol &sym, std::string_view code, std::string_view arg1, DiagnosticSeverity severity) const
    {
        Diagnostic diag = CreateDiagnostic(sym, code, severity);
        std::string codeStr(code);
        std::string a1(arg1);

        if (request.i18n)
        {
            std::string pattern = request.i18n->GetMessage(codeStr);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), a1);
            }
        }
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + codeStr + "] " + a1;
        }

        Append(std::move(diag));
    }

    void DiagnosticContext::Emit(const Symbol &sym, std::string_view code, std::string_view arg1, std::string_view arg2, DiagnosticSeverity severity) const
    {
        Diagnostic diag = CreateDiagnostic(sym, code, severity);
        std::string codeStr(code);
        std::string a1(arg1);
        std::string a2(arg2);

        if (request.i18n)
        {
            std::string pattern = request.i18n->GetMessage(codeStr);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), a1, a2);
            }
        }
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + codeStr + "] " + a1 + ", " + a2;
        }

        Append(std::move(diag));
    }

    void DiagnosticContext::Emit(const Symbol &sym, std::string_view code, std::string_view arg1, std::string_view arg2, std::string_view arg3, DiagnosticSeverity severity) const
    {
        Diagnostic diag = CreateDiagnostic(sym, code, severity);
        std::string codeStr(code);
        std::string a1(arg1);
        std::string a2(arg2);
        std::string a3(arg3);

        if (request.i18n)
        {
            std::string pattern = request.i18n->GetMessage(codeStr);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), a1, a2, a3);
            }
        }
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + codeStr + "] " + a1 + ", " + a2 + ", " + a3;
        }

        Append(std::move(diag));
    }

    void DiagnosticContext::Emit(const ParameterInformation &param, const Symbol &parentSym, std::string_view code, DiagnosticSeverity severity) const
    {
        Append(CreateDiagnostic(param, parentSym, code, severity));
    }

    void DiagnosticContext::Emit(const ParameterInformation &param, const Symbol &parentSym, std::string_view code, std::string_view arg1, DiagnosticSeverity severity) const
    {
        Diagnostic diag = CreateDiagnostic(param, parentSym, code, severity);
        std::string codeStr(code);
        std::string a1(arg1);

        if (request.i18n)
        {
            std::string pattern = request.i18n->GetMessage(codeStr);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), a1);
            }
        }
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + codeStr + "] " + a1;
        }

        Append(std::move(diag));
    }

    void DiagnosticContext::Emit(const ParameterInformation &param, const Symbol &parentSym, std::string_view code, std::string_view arg1, std::string_view arg2, DiagnosticSeverity severity) const
    {
        Diagnostic diag = CreateDiagnostic(param, parentSym, code, severity);
        std::string codeStr(code);
        std::string a1(arg1);
        std::string a2(arg2);

        if (request.i18n)
        {
            std::string pattern = request.i18n->GetMessage(codeStr);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), a1, a2);
            }
        }
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + codeStr + "] " + a1 + ", " + a2;
        }

        Append(std::move(diag));
    }

    void DiagnosticContext::EmitAtRange(const Symbol &parentSym, const SourceRange &range, std::string_view code, DiagnosticSeverity severity) const
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
        diag.fileUri = parentSym.fileUri;

        if (request.i18n)
        {
            diag.message = request.i18n->GetMessage(codeStr);
        }
        if (diag.message.empty())
        {
            diag.message = "[" + codeStr + "] Diagnostic code: " + codeStr;
        }

        Append(std::move(diag));
    }

    void DiagnosticContext::EmitAtRange(const Symbol &parentSym, const SourceRange &range, std::string_view code, std::string_view arg1, DiagnosticSeverity severity) const
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
        diag.fileUri = parentSym.fileUri;
        std::string a1(arg1);

        if (request.i18n)
        {
            std::string pattern = request.i18n->GetMessage(codeStr);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), a1);
            }
        }
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + codeStr + "] " + a1;
        }

        Append(std::move(diag));
    }

    namespace
    {
        /**
         * @brief The column of `typeName` on `line`, searching from `fromColumn`.
         *
         * Returns the source line's own npos when the line is past the end of the document or the
         * name is not on it - a declaration split across lines, or a type spelled differently from
         * the base name the rule is complaining about.
         */
        size_t ColumnOfTypeName(std::string_view sourceCode, uint32_t line, uint32_t fromColumn,
                                std::string_view typeName)
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
            const bool leftClear = found == 0 ||
                                   (!std::isalnum(static_cast<unsigned char>(text[found - 1])) &&
                                    text[found - 1] != '_');
            const size_t after = found + typeName.size();
            const bool rightClear = after >= text.size() ||
                                    (!std::isalnum(static_cast<unsigned char>(text[after])) &&
                                     text[after] != '_');

            return (leftClear && rightClear) ? found : std::string_view::npos;
        }
    }

    void DiagnosticContext::EmitAtTypeName(const Symbol &sym, std::string_view code,
                                           std::string_view typeName, DiagnosticSeverity severity) const
    {
        const size_t column = ColumnOfTypeName(request.sourceCode, sym.fullRange.startLine,
                                               sym.fullRange.startCharacter, typeName);

        if (column == std::string_view::npos)
        {
            Emit(sym, code, typeName, severity);
            return;
        }

        EmitAtRange(sym.fullRange.startLine, static_cast<uint32_t>(column),
                    sym.fullRange.startLine, static_cast<uint32_t>(column + typeName.size()),
                    code, typeName, severity);
    }

    void DiagnosticContext::EmitAtTypeName(const ParameterInformation &param, const Symbol &parentSym,
                                           std::string_view code, std::string_view typeName,
                                           DiagnosticSeverity severity) const
    {
        const size_t column = ColumnOfTypeName(request.sourceCode, param.startLine,
                                               param.startCharacter, typeName);

        if (column == std::string_view::npos)
        {
            Emit(param, parentSym, code, typeName, severity);
            return;
        }

        EmitAtRange(param.startLine, static_cast<uint32_t>(column),
                    param.startLine, static_cast<uint32_t>(column + typeName.size()),
                    code, typeName, severity);
    }

    void DiagnosticContext::EmitAtRange(uint32_t startLine, uint32_t startCharacter, uint32_t endLine, uint32_t endCharacter, std::string_view code, DiagnosticSeverity severity) const
    {
        Diagnostic diag;
        diag.range.start.line = startLine;
        diag.range.start.character = startCharacter;
        diag.range.end.line = endLine;
        diag.range.end.character = endCharacter;
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
        diag.fileUri = request.fileUri;

        if (request.i18n)
        {
            diag.message = request.i18n->GetMessage(codeStr);
        }
        if (diag.message.empty())
        {
            diag.message = "[" + codeStr + "] Diagnostic code: " + codeStr;
        }

        Append(std::move(diag));
    }

    void DiagnosticContext::EmitAtRange(uint32_t startLine, uint32_t startCharacter, uint32_t endLine, uint32_t endCharacter, std::string_view code, std::string_view arg1, DiagnosticSeverity severity) const
    {
        Diagnostic diag;
        diag.range.start.line = startLine;
        diag.range.start.character = startCharacter;
        diag.range.end.line = endLine;
        diag.range.end.character = endCharacter;
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
        diag.fileUri = request.fileUri;
        std::string a1(arg1);

        if (request.i18n)
        {
            std::string pattern = request.i18n->GetMessage(codeStr);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), a1);
            }
        }
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + codeStr + "] " + a1;
        }

        Append(std::move(diag));
    }

    void DiagnosticContext::EmitAtRange(uint32_t startLine, uint32_t startCharacter, uint32_t endLine, uint32_t endCharacter, std::string_view code, std::string_view arg1, std::string_view arg2, DiagnosticSeverity severity) const
    {
        Diagnostic diag;
        diag.range.start.line = startLine;
        diag.range.start.character = startCharacter;
        diag.range.end.line = endLine;
        diag.range.end.character = endCharacter;
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
        diag.fileUri = request.fileUri;
        std::string a1(arg1);
        std::string a2(arg2);

        if (request.i18n)
        {
            std::string pattern = request.i18n->GetMessage(codeStr);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), a1, a2);
            }
        }
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + codeStr + "] " + a1 + " -> " + a2;
        }

        Append(std::move(diag));
    }

    void DiagnosticContext::LogRule(std::string_view ruleName, std::string_view code, const Symbol &sym) const
    {
        if (!logger)
        {
            return;
        }

        logger->LogDebug(fmt::format("[SA-DEBUG] rule={:<35} code={:<35} sym={} container={}",
                                     ruleName, code, sym.name, sym.containerName));
    }

    void DiagnosticContext::LogParam(std::string_view ruleName, std::string_view code, const ParameterInformation &param, const Symbol &parentSym) const
    {
        if (!logger)
        {
            return;
        }

        logger->LogDebug(fmt::format("[SA-DEBUG] rule={:<35} code={:<35} param={} parent={}",
                                     ruleName, code, param.name, parentSym.name));
    }
}
