#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/Diagnostics.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"
#include "utils/LspLogger.h"

#include "config/ServerConfig.h"
#include <tree_sitter/api.h>
#include <string_view>

namespace angel_lsp::analysis
{
    class SymbolCollector
    {
    public:
        explicit SymbolCollector(angel_lsp::utils::LspLogger *logger);
        ~SymbolCollector();

        static TSNode GetChildByFieldName(TSNode node, const char *fieldName)
        {
            return ts_node_child_by_field_name(node, fieldName, static_cast<uint32_t>(std::strlen(fieldName)));
        }

        std::vector<Diagnostic> CollectSymbols(const std::string &fileUri, const std::string &sourceCode, angel_lsp::parser::AngelScriptParser &parser, SymbolTable &symbolTable, const angel_lsp::i18n::I18n *i18n = nullptr, const angel_lsp::config::TypeConfig *typeConfig = nullptr);

    private:
        utils::LspLogger *m_logger;
        TSQuery *m_tagsQuery;

        TSSymbol m_symPrimitiveType = 0;
        TSSymbol m_symDatatype = 0;
        TSSymbol m_symTemplateTypeList = 0;
        TSSymbol m_symIdentifier = 0;
        TSSymbol m_symDeclarationModifier = 0;
        TSSymbol m_symType = 0;
        TSSymbol m_symParameter = 0;
        TSSymbol m_symClassBody = 0;
        TSSymbol m_symNamespaceBody = 0;
        TSSymbol m_symInterfaceBody = 0;
        TSSymbol m_symEnumMember = 0;
        TSSymbol m_symFuncDeclaration = 0;
        TSSymbol m_symStatementBlock = 0;
        TSSymbol m_symInterfaceMethod = 0;
        TSSymbol m_symFuncAttributes = 0;
        TSSymbol m_symGet = 0;
        TSSymbol m_symSet = 0;
        TSSymbol m_symNullLiteral = 0;
        TSSymbol m_symCallExpression = 0;
        TSSymbol m_symVariableDeclarator = 0;
        TSSymbol m_symAccessor = 0;
        TSSymbol m_symLambdaExpression = 0;
        TSSymbol m_symBooleanLiteral = 0;
        TSSymbol m_symImportDeclaration = 0;

        ankerl::unordered_dense::map<TSSymbol, TypeKind> m_primitiveKindMap;

        struct CollectionContext
        {
            std::string containerPath;
            bool isInsideFunction = false;
            bool isInsideClass = false;
            bool isInsideNamespace = false;
        };

        using ProcessFn = void (SymbolCollector::*)(TSNode, const std::string &, const std::string &, SymbolTable &, const CollectionContext &);
        std::vector<ProcessFn> m_captureDispatch;

        struct TypeExtractionResult
        {
            std::string baseTypeName;
            std::string templateName;
            TypeKind kind = TypeKind::Unknown;
            bool isArray = false;
            bool isHandle = false;
            bool isReference = false;
            bool isConst = false;
            bool hasPrimitiveHandle = false;
            uint32_t arrayDepth = 0;
            std::vector<TypeExtractionResult> templateArguments;
        };

        TypeExtractionResult ExtractTypeInfo(TSNode typeNode, const std::string &sourceCode) const;

        void ProcessVariable(TSNode varDeclNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessFunction(TSNode funcNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessClass(TSNode classNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessNamespace(TSNode namespaceNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessTypedef(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessFuncdef(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessEnum(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessProperty(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessInterface(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);

        std::string GetNodeText(TSNode node, const std::string &sourceCode) const;
        std::string_view GetNodeView(TSNode node, const std::string &sourceCode) const;
        void ReportParseErrors(TSNode node, const std::string &fileUri, const std::string &sourceCode, std::vector<Diagnostic> &diagnostics, const angel_lsp::i18n::I18n *i18n = nullptr) const;
        void CheckUsingDeclarations(TSNode node, const std::string &sourceCode, const std::string &fileUri, const angel_lsp::i18n::I18n *i18n, std::vector<Diagnostic> &diagnostics) const;
        void CheckDuplicateModifiers(TSNode node, const std::string &sourceCode, const std::string &fileUri, const angel_lsp::i18n::I18n *i18n, std::vector<Diagnostic> &diagnostics) const;
        void CheckMixinDeclarations(TSNode node, const std::string &sourceCode, const std::string &fileUri, const angel_lsp::i18n::I18n *i18n, std::vector<Diagnostic> &diagnostics) const;

        CollectionContext BuildContext(TSNode node, const std::string &sourceCode) const;

        SymbolModifiers ExtractModifiers(TSNode node, const std::string &sourceCode) const;
        ParameterInformation ExtractParameterInfo(TSNode paramNode, const std::string &sourceCode) const;
        std::vector<ParameterInformation> ExtractParameters(TSNode paramsNode, const std::string &sourceCode) const;

        Symbol CreateSymbol(SymbolType type, TSNode node, TSNode nameNode, const std::string &sourceCode, const std::string &fileUri, const std::string &containerPath) const;
        std::vector<std::string> ExtractBases(TSNode classNode, const std::string &sourceCode) const;
        TypeKind LookupPrimitiveKind(TSSymbol symbol) const;
    };
}