#include "analysis/SemanticAnalyzer.h"
#include <spdlog/fmt/fmt.h>
#include <sstream>

namespace angel_lsp::analysis
{
    /** @brief Checks whether the given name is a reserved AngelScript keyword that
     *         cannot be used as a symbol name.
     *  Context-sensitive keywords (abstract, final, function, get, set, etc.) are intentionally
     *  excluded since they are valid identifiers per specification.
     *  @param name The symbol name to check.
     *  @return True if name is a reserved keyword. */
    static bool IsReservedKeyword(const std::string &name)
    {
        static const ankerl::unordered_dense::set<std::string> kReserved = {
            "and", "auto", "bool", "break", "case", "cast", "catch",
            "class", "const", "continue", "default", "do", "double",
            "else", "enum", "false", "float", "for", "foreach", "funcdef",
            "if", "import", "in", "inout", "int", "int8", "int16", "int32", "int64",
            "interface", "is", "mixin", "namespace", "not", "null",
            "or", "out", "private", "protected", "return", "switch",
            "true", "try", "typedef", "uint", "uint8", "uint16", "uint32", "uint64",
            "using", "void", "while", "xor",
        };
        return kReserved.contains(name);
    }

    static bool IsPrimitiveTypeName(const std::string &name)
    {
        static const ankerl::unordered_dense::set<std::string> kPrimitives = {
            "int", "int8", "int16", "int32", "int64",
            "uint", "uint8", "uint16", "uint32", "uint64",
            "float", "double", "bool", "void"
        };
        return kPrimitives.contains(name);
    }

    SemanticAnalyzer::SemanticAnalyzer(angel_lsp::utils::LspLogger *logger)
        : m_logger(logger)
    {
    }

    std::vector<Diagnostic> SemanticAnalyzer::Analyze(const SemanticAnalysisRequest &request) const
    {
        std::vector<Diagnostic> diagnostics;

        request.symbolTable.ForEachSymbol(
            [&](const std::string &qualifiedName, const std::vector<Symbol> &symbols)
            {
                ValidateDuplicates(qualifiedName, symbols, request, diagnostics);

                for (const Symbol &sym : symbols)
                {
                    if (sym.fileUri != request.fileUri)
                        continue;

                    switch (sym.type)
                    {
                    case SymbolType::Function:
                        ValidateFunction(sym, request, diagnostics);
                        break;
                    case SymbolType::Variable:
                        ValidateVariable(sym, request, diagnostics);
                        break;
                    case SymbolType::Property:
                        ValidateProperty(sym, request, diagnostics);
                        break;
                    case SymbolType::Class:
                        ValidateClass(sym, request, diagnostics);
                        break;
                    case SymbolType::Interface:
                        ValidateInterface(sym, request, diagnostics);
                        break;
                    case SymbolType::Typedef:
                        ValidateTypedef(sym, request, diagnostics);
                        break;
                    case SymbolType::Funcdef:
                        ValidateFuncdef(sym, request, diagnostics);
                        break;
                    case SymbolType::Enum:
                        ValidateEnum(sym, request, diagnostics);
                        break;
                    case SymbolType::Namespace:
                        ValidateNamespace(sym, request, diagnostics);
                        break;
                    default:
                        break;
                    }
                }
            });

        return diagnostics;
    }



    SemanticAnalyzer::FunctionContext SemanticAnalyzer::BuildFunctionContext(const Symbol &sym, const SemanticAnalysisRequest &req) const
    {
        FunctionContext ctx;
        ctx.isCtor = (!sym.containerName.empty() && sym.name == sym.containerName);
        ctx.isDtor = (!sym.name.empty() && sym.name[0] == '~');

        if (!sym.containerName.empty())
        {
            auto parentOpt = req.symbolTable.FindFirstSymbol(sym.containerName);
            if (parentOpt)
            {
                if (parentOpt->type == SymbolType::Class || parentOpt->type == SymbolType::Interface)
                {
                    ctx.isInsideClass = true;
                    if (parentOpt->type == SymbolType::Interface)
                    {
                        ctx.isInterface = true;
                    }
                    else if (parentOpt->type == SymbolType::Class && parentOpt->GetClass().modifiers.isMixin)
                    {
                        ctx.isInsideMixin = true;
                    }
                }
            }
        }
        return ctx;
    }

    void SemanticAnalyzer::DebugDiag(const std::string &ruleName, const std::string &code, const Symbol &sym) const
    {
        if (!m_logger)
        {
            return;
        }

        m_logger->LogDebug(fmt::format("[SA-DEBUG] rule={:<35} code={:<35} sym={} container={}",
                                       ruleName, code, sym.name, sym.containerName));
    }

    void SemanticAnalyzer::DebugParamDiag(const std::string &ruleName, const std::string &code, const ParameterInformation &param, const Symbol &parentSym) const
    {
        if (!m_logger)
        {
            return;
        }

        m_logger->LogDebug(fmt::format("[SA-DEBUG] rule={:<35} code={:<35} param={} parent={}",
                                       ruleName, code, param.name, parentSym.name));
    }



    Diagnostic SemanticAnalyzer::CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code, DiagnosticSeverity severity) const
    {
        Diagnostic diag;
        diag.range.start.line = sym.startLine;
        diag.range.start.character = sym.startCharacter;
        diag.range.end.line = sym.endLine;
        diag.range.end.character = sym.endCharacter;
        diag.severity = severity;
        if (req.severityOverrides)
        {
            auto it = req.severityOverrides->find(code);
            if (it != req.severityOverrides->end())
            {
                diag.severity = it->second;
            }
        }
        diag.code = code;
        diag.source = "AngelScript";
        diag.fileUri = sym.fileUri;

        if (req.i18n)
        {
            diag.message = req.i18n->GetMessage(code);
        }
        if (diag.message.empty())
        {
            diag.message = "[" + code + "] Diagnostic code: " + code;
        }

        return diag;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1, DiagnosticSeverity severity) const
    {
        Diagnostic diag = CreateDiagnostic(sym, req, code, severity);

        if (req.i18n)
        {
            std::string pattern = req.i18n->GetMessage(code);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), arg1);
            }
        }
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + code + "] " + arg1;
        }

        return diag;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1, const std::string &arg2, DiagnosticSeverity severity) const
    {
        Diagnostic diag = CreateDiagnostic(sym, req, code, severity);

        if (req.i18n)
        {
            std::string pattern = req.i18n->GetMessage(code);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), arg1, arg2);
            }
        }
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + code + "] " + arg1 + ", " + arg2;
        }

        return diag;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1, const std::string &arg2, const std::string &arg3, DiagnosticSeverity severity) const
    {
        Diagnostic diag = CreateDiagnostic(sym, req, code, severity);

        if (req.i18n)
        {
            std::string pattern = req.i18n->GetMessage(code);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), arg1, arg2, arg3);
            }
        }
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + code + "] " + arg1 + ", " + arg2 + ", " + arg3;
        }

        return diag;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, const SemanticAnalysisRequest &req, const std::string &code, DiagnosticSeverity severity) const
    {
        Diagnostic diag;
        diag.range.start.line = param.startLine;
        diag.range.start.character = param.startCharacter;
        diag.range.end.line = param.endLine;
        diag.range.end.character = param.endCharacter;
        diag.severity = severity;
        if (req.severityOverrides)
        {
            auto it = req.severityOverrides->find(code);
            if (it != req.severityOverrides->end())
            {
                diag.severity = it->second;
            }
        }
        diag.code = code;
        diag.source = "AngelScript";
        diag.fileUri = parentSym.fileUri;

        if (req.i18n)
        {
            diag.message = req.i18n->GetMessage(code);
        }
        if (diag.message.empty())
        {
            diag.message = "[" + code + "] Diagnostic code: " + code;
        }

        return diag;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1, DiagnosticSeverity severity) const
    {
        Diagnostic diag = CreateDiagnostic(param, parentSym, req, code, severity);

        if (req.i18n)
        {
            std::string pattern = req.i18n->GetMessage(code);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), arg1);
            }
        }
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + code + "] " + arg1;
        }

        return diag;
    }

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1, const std::string &arg2, DiagnosticSeverity severity) const
    {
        Diagnostic diag = CreateDiagnostic(param, parentSym, req, code, severity);

        if (req.i18n)
        {
            std::string pattern = req.i18n->GetMessage(code);
            if (!pattern.empty())
            {
                diag.message = fmt::format(fmt::runtime(pattern), arg1, arg2);
            }
        }
        if (diag.message.empty() || diag.message.starts_with("["))
        {
            diag.message = "[" + code + "] " + arg1 + ", " + arg2;
        }

        return diag;
    }
}
