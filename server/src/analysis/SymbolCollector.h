#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/TypeExtraction.h"
#include "analysis/Diagnostics.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"
#include "utils/LspLogger.h"

#include "config/ServerConfig.h"
#include <tree_sitter/api.h>
#include <cstring>
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
        std::vector<Diagnostic> CollectSymbolsWithTree(const std::string &fileUri, const std::string &sourceCode, TSTree *tree, SymbolTable &symbolTable, const angel_lsp::i18n::I18n *i18n = nullptr, const angel_lsp::config::TypeConfig *typeConfig = nullptr);

    private:
        utils::LspLogger *m_logger;
        TSQuery *m_tagsQuery;

        TSSymbol m_symDeclarationModifier = 0;
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

        TSSymbol m_symBreakStatement = 0;
        TSSymbol m_symContinueStatement = 0;
        TSSymbol m_symReturnStatement = 0;
        TSSymbol m_symForStatement = 0;
        TSSymbol m_symWhileStatement = 0;
        TSSymbol m_symDoWhileStatement = 0;
        TSSymbol m_symSwitchStatement = 0;
        TSSymbol m_symCaseClause = 0;
        TSSymbol m_symCastExpression = 0;
        TSSymbol m_symScopedIdentifier = 0;
        TSSymbol m_symVariableDeclaration = 0;
        TSSymbol m_symGotoStatement = 0;
        TSSymbol m_symMixinDeclaration = 0;
        TSSymbol m_symClassDeclaration = 0;
        TSSymbol m_symUsingDeclaration = 0;
        TSSymbol m_symInterfaceDeclaration = 0;
        TSSymbol m_symVirtualProperty = 0;
        TSSymbol m_symCompoundStatement = 0;
        TSSymbol m_symBlock = 0;
        TSSymbol m_symBaseClassList = 0;

        // Anonymous token symbols (resolved once, compared via ts_node_symbol)
        TSSymbol m_tokConst = 0;
        TSSymbol m_tokIn = 0;
        TSSymbol m_tokOut = 0;
        TSSymbol m_tokInout = 0;
        TSSymbol m_tokAmp = 0;
        TSSymbol m_tokAt = 0;
        TSSymbol m_tokPrivate = 0;
        TSSymbol m_tokProtected = 0;
        TSSymbol m_tokPublic = 0;
        TSSymbol m_tokShared = 0;
        TSSymbol m_tokMixin = 0;
        TSSymbol m_tokAbstract = 0;
        TSSymbol m_tokFinal = 0;
        TSSymbol m_tokOverride = 0;
        TSSymbol m_tokExplicit = 0;
        TSSymbol m_tokProperty = 0;
        TSSymbol m_tokDelete = 0;
        TSSymbol m_tokExternal = 0;
        TSSymbol m_tokImport = 0;
        TSSymbol m_tokOpenBrace = 0;

        struct CollectionContext
        {
            std::string containerPath;
            bool isInsideFunction = false;
            bool isInsideClass = false;
            bool isInsideNamespace = false;
        };

        using ProcessFn = void (SymbolCollector::*)(TSNode, const std::string &, const std::string &, SymbolTable &, const CollectionContext &);
        std::vector<ProcessFn> m_captureDispatch;

        void ProcessVariable(TSNode varDeclNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessFunction(TSNode funcNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessClass(TSNode classNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessNamespace(TSNode namespaceNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessTypedef(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessFuncdef(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessEnum(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessProperty(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);
        void ProcessInterface(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);

        void CollectFromTree(TSNode rootNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const angel_lsp::i18n::I18n *i18n, std::vector<Diagnostic> &diagnostics);
        std::string GetNodeText(TSNode node, const std::string &sourceCode) const;
        std::string_view GetNodeView(TSNode node, const std::string &sourceCode) const;
        void ReportParseErrors(TSNode node, const std::string &fileUri, const std::string &sourceCode, std::vector<Diagnostic> &diagnostics, const angel_lsp::i18n::I18n *i18n = nullptr, int depth = 0) const;
        void CheckUsingDeclarations(TSNode node, const std::string &sourceCode, const std::string &fileUri, const angel_lsp::i18n::I18n *i18n, std::vector<Diagnostic> &diagnostics, int depth = 0) const;
        void CheckDuplicateModifiers(TSNode node, const std::string &sourceCode, const std::string &fileUri, const angel_lsp::i18n::I18n *i18n, std::vector<Diagnostic> &diagnostics, int depth = 0) const;

        CollectionContext BuildContext(TSNode node, const std::string &sourceCode) const;

        SymbolModifiers ExtractModifiers(TSNode node, const std::string &sourceCode) const;
        void ApplyModifierToken(TSSymbol tokenSymbol, SymbolModifiers &modifiers) const;
        bool HasNullLiteral(TSNode node) const;
        ParameterInformation ExtractParameterInfo(TSNode paramNode, const std::string &sourceCode) const;
        std::vector<ParameterInformation> ExtractParameters(TSNode paramsNode, const std::string &sourceCode) const;

        Symbol CreateSymbol(SymbolType type, TSNode node, TSNode nameNode, const std::string &sourceCode, const std::string &fileUri, const std::string &containerPath) const;
        std::vector<std::string> ExtractBases(TSNode classNode, const std::string &sourceCode) const;
    };
}