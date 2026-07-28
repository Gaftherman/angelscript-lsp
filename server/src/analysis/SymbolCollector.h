#pragma once

#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"
#include "utils/LspLogger.h"

#include <tree_sitter/api.h>
#include <string>

namespace angel_lsp::analysis
{
    class SymbolCollector
    {
    public:
        SymbolCollector(angel_lsp::utils::LspLogger *logger);
        ~SymbolCollector();

        void CollectSymbols(const std::string &fileUri, const std::string &sourceCode, angel_lsp::parser::AngelScriptParser &parser, SymbolTable &symbolTable);

    private:
        utils::LspLogger *m_logger;
        TSQuery *m_tagsQuery;

        struct NodeContext
        {
            std::string containerPath;
            bool isInsideFunction = false;
        };

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
        void ReportParseErrors(TSNode node, const std::string &fileUri, const std::string &sourceCode) const;

        NodeContext GetNodeContext(TSNode node, const std::string &sourceCode) const;

        SymbolModifiers ExtractModifiers(TSNode node, const std::string &sourceCode) const;
        ParameterInformation ExtractParameterInfo(TSNode paramNode, const std::string &sourceCode) const;
        std::vector<ParameterInformation> ExtractParameters(TSNode paramsNode, const std::string &sourceCode) const;

        Symbol CreateSymbol(SymbolType type, TSNode node, TSNode nameNode, const std::string &sourceCode, const std::string &fileUri, const std::string &containerPath) const;
        std::vector<std::string> ExtractBases(TSNode classNode, const std::string &sourceCode) const;
    };
}