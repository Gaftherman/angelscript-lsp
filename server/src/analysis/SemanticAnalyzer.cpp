#include "analysis/SemanticAnalyzer.h"
#include "analysis/DiagnosticContext.h"
#include "analysis/rules/FunctionRules.h"
#include "analysis/rules/ClassRules.h"
#include "analysis/rules/VariableRules.h"
#include "analysis/rules/PropertyRules.h"
#include "analysis/rules/TypeRules.h"

namespace angel_lsp::analysis
{
    SemanticAnalyzer::SemanticAnalyzer(angel_lsp::utils::LspLogger *logger)
        : m_logger(logger)
    {
    }

    std::vector<Diagnostic> SemanticAnalyzer::Analyze(const SemanticAnalysisRequest &request) const
    {
        std::vector<Diagnostic> diagnostics;
        DiagnosticContext ctx{request, diagnostics, m_logger};

        request.symbolTable.ForEachSymbol(
            [&](const std::string &qualifiedName, const std::vector<Symbol> &symbols)
            {
                rules::ValidateDuplicates(qualifiedName, symbols, ctx);

                for (const Symbol &sym : symbols)
                {
                    if (sym.fileUri != request.fileUri)
                    {
                        continue;
                    }

                    switch (sym.type)
                    {
                    case SymbolType::Function:
                        rules::ValidateFunction(sym, ctx);
                        break;
                    case SymbolType::Variable:
                        rules::ValidateVariable(sym, ctx);
                        break;
                    case SymbolType::Property:
                        rules::ValidateProperty(sym, ctx);
                        break;
                    case SymbolType::Class:
                        rules::ValidateClass(sym, ctx);
                        break;
                    case SymbolType::Interface:
                        rules::ValidateInterface(sym, ctx);
                        break;
                    case SymbolType::Typedef:
                        rules::ValidateTypedef(sym, ctx);
                        break;
                    case SymbolType::Funcdef:
                        rules::ValidateFuncdef(sym, ctx);
                        break;
                    case SymbolType::Enum:
                        rules::ValidateEnum(sym, ctx);
                        break;
                    case SymbolType::Namespace:
                        rules::ValidateNamespace(sym, ctx);
                        break;
                    default:
                        break;
                    }
                }
            });

        return diagnostics;
    }
}
