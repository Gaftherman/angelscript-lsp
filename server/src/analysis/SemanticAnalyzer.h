#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/Diagnostics.h"
#include "i18n/i18n.h"
#include "utils/LspLogger.h"

#include <string>
#include <vector>
#include <ankerl/unordered_dense.h>

#include "config/ServerConfig.h"

namespace angel_lsp::analysis
{
    namespace node_types
    {
        constexpr std::string_view StringLiteral = "string_literal";
        constexpr std::string_view LambdaExpression = "lambda_expression";
        constexpr std::string_view BooleanLiteral = "boolean_literal";
        constexpr std::string_view NumberLiteral = "number_literal";
        constexpr std::string_view ImportDeclaration = "import_declaration";
        constexpr std::string_view NullLiteral = "null_literal";
        constexpr std::string_view CallExpression = "call_expression";
    }
    struct SemanticAnalysisRequest
    {
        const SymbolTable &symbolTable;
        std::string fileUri;
        std::string predefinedFileExtension;
        const i18n::I18n *i18n = nullptr;
        const config::TypeConfig *typeConfig = nullptr;
        const ankerl::unordered_dense::map<std::string, DiagnosticSeverity> *severityOverrides = nullptr;

        std::string_view GetStringTypeName() const
        {
            return (typeConfig && !typeConfig->stringTypeName.empty()) ? std::string_view(typeConfig->stringTypeName) : std::string_view("string");
        }

        std::string_view GetArrayTypeName() const
        {
            return (typeConfig && !typeConfig->arrayTypeName.empty()) ? std::string_view(typeConfig->arrayTypeName) : std::string_view("array");
        }
    };

    class SemanticAnalyzer
    {
    public:
        explicit SemanticAnalyzer(angel_lsp::utils::LspLogger *logger = nullptr);
        ~SemanticAnalyzer() = default;

        std::vector<Diagnostic> Analyze(const SemanticAnalysisRequest &request) const;

    private:
        struct FunctionContext
        {
            bool isInsideClass = false;
            bool isInsideMixin = false;
            bool isCtor = false;
            bool isDtor = false;
            bool isInterface = false;
            const Symbol *container = nullptr;
        };

        angel_lsp::utils::LspLogger *m_logger;

        FunctionContext BuildFunctionContext(const Symbol &sym, const SemanticAnalysisRequest &req) const;
        void DebugDiag(const std::string &ruleName, const std::string &code, const Symbol &sym) const;
        void DebugParamDiag(const std::string &ruleName, const std::string &code, const ParameterInformation &param, const Symbol &parentSym) const;

        // === Function Rule Methods ===
        bool Rule_FunctionName(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_FunctionBody(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_FunctionReturnType(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_FunctionModifiers(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_CtorDtor(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_FunctionBodyFlow(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_FunctionBodyCast(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_FunctionBodyScope(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_FunctionReturnExpr(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_FunctionOverride(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_OperatorOverload(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_MixinConstraints(const Symbol &sym, const FunctionContext &ctx, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;

        // === Duplicate Rule Methods ===
        bool Rule_DuplicateTypeConflict(const std::vector<Symbol> &symbols, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        bool Rule_DuplicateVarCallableCollision(const std::vector<Symbol> &symbols, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_DuplicateSignature(const std::vector<Symbol> &symbols, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;

        // === Class Rule Methods ===
        bool Rule_ClassName(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_ClassModifiers(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_ClassInheritance(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;

        // === Variable Rule Methods ===
        bool Rule_VariableName(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_VariableModifiers(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_VariableTypeResolution(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_VariableInitializer(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;

        // === Property Rule Methods ===
        bool Rule_PropertyModifiers(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_PropertyAccessors(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;

        // === Interface Rule Methods ===
        bool Rule_InterfaceName(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_InterfaceInheritance(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_InterfaceMethods(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;

        // === Typedef Rule Methods ===
        bool Rule_TypedefName(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_TypedefTypeResolution(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;

        // === Funcdef Rule Methods ===
        bool Rule_FuncdefName(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_FuncdefReturn(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;

        // === Enum Rule Methods ===
        bool Rule_EnumName(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void Rule_EnumMembers(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;

        // === Namespace Rule Methods ===
        bool Rule_NamespaceName(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;

        void ValidateDuplicates(const std::string &qualifiedName, const std::vector<Symbol> &symbols, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateFunction(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateFunctionParameters(const Symbol &sym, const FunctionSignature &sig, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateVariable(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateProperty(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateClass(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateInterface(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateTypedef(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateFuncdef(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateEnum(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;
        void ValidateNamespace(const Symbol &sym, const SemanticAnalysisRequest &req, std::vector<Diagnostic> &diagnostics) const;

        bool CheckCircularInheritance(const std::string &currentClass, const SymbolTable &table, ankerl::unordered_dense::set<std::string> &visited) const;

        Diagnostic CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
        Diagnostic CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
        Diagnostic CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1, const std::string &arg2, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
        Diagnostic CreateDiagnostic(const Symbol &sym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1, const std::string &arg2, const std::string &arg3, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
        Diagnostic CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, const SemanticAnalysisRequest &req, const std::string &code, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
        Diagnostic CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
        Diagnostic CreateDiagnostic(const ParameterInformation &param, const Symbol &parentSym, const SemanticAnalysisRequest &req, const std::string &code, const std::string &arg1, const std::string &arg2, DiagnosticSeverity severity = DiagnosticSeverity::Error) const;
    };
}
