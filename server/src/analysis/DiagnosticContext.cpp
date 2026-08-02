#include "analysis/DiagnosticContext.h"
#include <spdlog/fmt/fmt.h>

namespace angel_lsp::analysis
{
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
        diagnostics.push_back(CreateDiagnostic(sym, code, severity));
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

        diagnostics.push_back(std::move(diag));
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

        diagnostics.push_back(std::move(diag));
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

        diagnostics.push_back(std::move(diag));
    }

    void DiagnosticContext::Emit(const ParameterInformation &param, const Symbol &parentSym, std::string_view code, DiagnosticSeverity severity) const
    {
        diagnostics.push_back(CreateDiagnostic(param, parentSym, code, severity));
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

        diagnostics.push_back(std::move(diag));
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

        diagnostics.push_back(std::move(diag));
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
