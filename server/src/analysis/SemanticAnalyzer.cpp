#include "analysis/SemanticAnalyzer.h"
#include "spdlog/fmt/fmt.h"

#include <unordered_set>

namespace angel_lsp::analysis
{
    SemanticAnalyzer::SemanticAnalyzer(angel_lsp::utils::LspLogger *logger)
        : m_logger(logger)
    {
    }

    std::vector<Diagnostic> SemanticAnalyzer::Analyze(const SemanticAnalysisRequest &request) const
    {
        std::vector<Diagnostic> diagnostics;

        request.symbolTable.ForEachSymbol([&](const std::string &qualifiedName, const std::vector<Symbol> &symbols)
        {
            // 1. Check for duplicate symbol redeclarations within the same scope
            ValidateDuplicateSymbols(qualifiedName, symbols, diagnostics);

            for (const auto &sym : symbols)
            {
                // URI filter check (if fileUri is non-empty, process only symbols from that document)
                if (!request.fileUri.empty() && sym.fileUri != request.fileUri)
                {
                    continue;
                }

                // 2. Dispatch to specific category validator based on SymbolType
                switch (sym.type)
                {
                case SymbolType::Class:
                {
                    if (sym.classSignature.modifiers.isMixin)
                    {
                        ValidateMixinClass(sym, request.symbolTable, diagnostics);
                    }
                    else
                    {
                        ValidateClass(sym, request.symbolTable, diagnostics);
                    }
                    break;
                }
                case SymbolType::Interface:
                {
                    ValidateInterface(sym, request.symbolTable, diagnostics);
                    break;
                }
                case SymbolType::Function:
                {
                    ValidateFunction(sym, request.symbolTable, diagnostics);
                    ValidateFunctionParameters(sym, sym.functionSignature, request.symbolTable, diagnostics);
                    break;
                }
                case SymbolType::Variable:
                {
                    ValidateVariable(sym, request.symbolTable, diagnostics);
                    break;
                }
                case SymbolType::Property:
                {
                    ValidateVirtualProperty(sym, request.symbolTable, diagnostics);
                    break;
                }
                case SymbolType::Enum:
                {
                    ValidateEnum(sym, request.symbolTable, diagnostics);
                    break;
                }
                case SymbolType::Typedef:
                {
                    ValidateTypedef(sym, request.symbolTable, diagnostics);
                    break;
                }
                case SymbolType::Funcdef:
                {
                    ValidateFuncdef(sym, request.symbolTable, diagnostics);
                    break;
                }
                case SymbolType::Namespace:
                {
                    ValidateNamespace(sym, request.symbolTable, diagnostics);
                    break;
                }
                default:
                    break;
                }
            }
        });

        if (m_logger && !diagnostics.empty())
        {
            m_logger->LogInfo(fmt::format("[SemanticAnalyzer] Se encontraron {} diagnósticos.", diagnostics.size()));
        }

        return diagnostics;
    }

    // ─── 1. Class Validation ───────────────────────────────────────────────────

    void SemanticAnalyzer::ValidateClass(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const
    {
        // TODO: Add custom class validation rules (e.g. unresolved base, inheriting from final class, circular inheritance)
        for (const auto &baseName : sym.classSignature.bases)
        {
            const auto *baseSymbols = table.FindSymbolsPtr(baseName);
            if (!baseSymbols || baseSymbols->empty())
            {
                // Base class or interface not found in table
                // e.g.: diagnostics.push_back(CreateDiagnostic(sym, fmt::format("La clase base '{}' no fue encontrada.", baseName), "as-err-unresolved-base"));
            }
            else
            {
                for (const auto &baseSym : *baseSymbols)
                {
                    if (baseSym.type == SymbolType::Class && baseSym.classSignature.modifiers.isFinal)
                    {
                        diagnostics.push_back(CreateDiagnostic(sym,
                            fmt::format("No se puede heredar de la clase final '{}'.", baseName),
                            "as-err-inherit-final"));
                    }
                }
            }
        }
    }

    // ─── 2. Mixin Class Validation ─────────────────────────────────────────────

    void SemanticAnalyzer::ValidateMixinClass(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const
    {
        // Rule: A mixin class cannot be final or abstract
        if (sym.classSignature.modifiers.isFinal)
        {
            diagnostics.push_back(CreateDiagnostic(sym,
                fmt::format("Un mixin ('{}') no puede ser declarado como 'final'.", sym.name),
                "as-err-mixin-final"));
        }

        if (sym.classSignature.modifiers.isAbstract)
        {
            diagnostics.push_back(CreateDiagnostic(sym,
                fmt::format("Un mixin ('{}') no puede ser declarado como 'abstract'.", sym.name),
                "as-err-mixin-abstract"));
        }
    }

    // ─── 3. Interface Validation ───────────────────────────────────────────────

    void SemanticAnalyzer::ValidateInterface(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const
    {
        // TODO: Add custom interface validation rules (e.g. inherited interface resolution)
    }

    // ─── 4. Function Validation ────────────────────────────────────────────────

    void SemanticAnalyzer::ValidateFunction(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const
    {
        // TODO: Add custom function validation rules (e.g. return type validation, return reference check)
        if (sym.functionSignature.modifiers.isReturnReference)
        {
            // Function returns a reference (e.g. int& GetRef())
        }
    }

    // ─── 5. Function Parameter Validation ──────────────────────────────────────

    void SemanticAnalyzer::ValidateFunctionParameters(const Symbol &sym, const FunctionSignature &sig, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const
    {
        for (const auto &param : sig.parameters)
        {
            // Validate reference direction modifiers (&in, &out, &inout) and handle types (@)
            if (param.modifier == ParameterModifier::Out && param.defaultValue.size() > 0)
            {
                diagnostics.push_back(CreateDiagnostic(sym,
                    fmt::format("El parámetro '&out' '{}' no puede tener un valor por defecto.", param.name),
                    "as-err-out-param-default"));
            }
        }
    }

    // ─── 6. Variable Validation ────────────────────────────────────────────────

    void SemanticAnalyzer::ValidateVariable(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const
    {
        // TODO: Add custom variable validation rules
    }

    // ─── 7. Virtual Property Validation ────────────────────────────────────────

    void SemanticAnalyzer::ValidateVirtualProperty(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const
    {
        // TODO: Add custom virtual property validation rules (e.g. getter/setter parameter matching)
    }

    // ─── 8. Enum Validation ────────────────────────────────────────────────────

    void SemanticAnalyzer::ValidateEnum(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const
    {
        // Pass 2.1: Only top-level name validation (handled by ValidateDuplicateSymbols)
    }

    // ─── 9. Typedef Validation ─────────────────────────────────────────────────

    void SemanticAnalyzer::ValidateTypedef(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const
    {
        // TODO: Add custom typedef alias validation rules
    }

    // ─── 10. Funcdef Validation ────────────────────────────────────────────────

    void SemanticAnalyzer::ValidateFuncdef(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const
    {
        // TODO: Add custom funcdef delegate validation rules
    }

    // ─── 11. Namespace Validation ──────────────────────────────────────────────

    void SemanticAnalyzer::ValidateNamespace(const Symbol &sym, const SymbolTable &table, std::vector<Diagnostic> &diagnostics) const
    {
        // Namespaces can be declared multiple times (re-opened) across files; no duplicate error
    }

    // ─── 12. Duplicate Symbol Validation ───────────────────────────────────────

    void SemanticAnalyzer::ValidateDuplicateSymbols(const std::string &qualifiedName, const std::vector<Symbol> &symbols, std::vector<Diagnostic> &diagnostics) const
    {
        if (symbols.size() <= 1)
            return;

        // Namespaces are allowed to be re-opened across multiple declarations
        if (symbols.front().type == SymbolType::Namespace)
            return;

        // Functions are allowed to be overloaded if parameter signatures differ
        if (symbols.front().type == SymbolType::Function)
        {
            // TODO: Implement overload resolution check if needed
            return;
        }

        // For non-function symbols (Class, Enum, Variable, Typedef), multiple declarations in the same scope are errors
        for (size_t i = 1; i < symbols.size(); ++i)
        {
            diagnostics.push_back(CreateDiagnostic(symbols[i],
                fmt::format("Redeclaración de símbolo '{}' en el mismo ámbito.", qualifiedName),
                "as-err-duplicate-symbol"));
        }
    }

    // ─── Diagnostic Builder Helper ─────────────────────────────────────────────

    Diagnostic SemanticAnalyzer::CreateDiagnostic(const Symbol &sym, const std::string &message, const std::string &code, DiagnosticSeverity severity) const
    {
        Diagnostic diag;
        diag.range.start.line      = sym.startLine;
        diag.range.start.character = sym.startCharacter;
        diag.range.end.line        = sym.endLine;
        diag.range.end.character  = sym.endCharacter;
        diag.severity              = severity;
        diag.code                  = code;
        diag.source                = "AngelScript";
        diag.message               = message;
        diag.fileUri               = sym.fileUri;
        return diag;
    }
}
