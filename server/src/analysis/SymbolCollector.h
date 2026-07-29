#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/Diagnostics.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"
#include "utils/LspLogger.h"

#include <tree_sitter/api.h>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{
    class SymbolCollector
    {
    public:
        SymbolCollector(angel_lsp::utils::LspLogger *logger);
        ~SymbolCollector();

        std::vector<Diagnostic> CollectSymbols(const std::string &fileUri, const std::string &sourceCode, angel_lsp::parser::AngelScriptParser &parser, SymbolTable &symbolTable, const angel_lsp::i18n::I18n *i18n = nullptr);

    private:
        utils::LspLogger *m_logger;
        TSQuery *m_tagsQuery;

        struct NodeContext
        {
            std::string containerPath;
            bool isInsideFunction = false;
        };

        struct TypeExtractionResult
        {
            std::string baseTypeName;
            TypeKind kind = TypeKind::Unknown;
            bool isArray = false;
            bool isHandle = false;
            uint32_t arrayDepth = 0;
        };

        TypeExtractionResult ExtractTypeInfo(TSNode typeNode, const std::string &sourceCode) const;

        void ProcessVariable(TSNode varDeclNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable);
        void ProcessFunction(TSNode funcNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable);
        void ProcessClass(TSNode classNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable);
        void ProcessNamespace(TSNode namespaceNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable);
        void ProcessTypedef(TSNode node, const std::string &tagName, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable);
        void ProcessFuncdef(TSNode node, const std::string &tagName, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable);
        void ProcessEnum(TSNode node, const std::string &tagName, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable);
        void ProcessProperty(TSNode node, const std::string &tagName, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable);
        void ProcessInterface(TSNode node, const std::string &tagName, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable);

        std::string GetNodeText(TSNode node, const std::string &sourceCode) const;
        void ReportParseErrors(TSNode node, const std::string &fileUri, const std::string &sourceCode, std::vector<Diagnostic> &diagnostics, const angel_lsp::i18n::I18n *i18n = nullptr) const;

        NodeContext GetNodeContext(TSNode node, const std::string &sourceCode) const;

        SymbolModifiers ExtractModifiers(TSNode node, const std::string &sourceCode) const;
        ParameterInformation ExtractParameterInfo(TSNode paramNode, const std::string &sourceCode) const;
        std::vector<ParameterInformation> ExtractParameters(TSNode paramsNode, const std::string &sourceCode) const;

        Symbol CreateSymbol(SymbolType type, TSNode node, TSNode nameNode, const std::string &sourceCode, const std::string &fileUri, const std::string &containerPath) const;
        std::vector<std::string> ExtractBases(TSNode classNode, const std::string &sourceCode) const;
        TypeKind ParseTypeKind(const std::string &typeName) const;
    };
}