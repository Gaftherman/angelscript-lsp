#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/TypeExtraction.h"
#include "analysis/Diagnostics.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"
#include "utils/LspLogger.h"

#include "config/ServerConfig.h"
#include <tree_sitter/api.h>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace angel_lsp::analysis
{
    /**
     * @brief Walks a parsed AngelScript tree-sitter AST and populates a SymbolTable with
     *        declarations (functions, classes, variables, ...) and out-of-body call references.
     *
     * SymbolCollector is a Layer 2 (Analysis) component: it depends only on Layer 1
     * (parser/, document/, utils/, config/) and the C++ standard library, and must never
     * include anything from features/ (Layer 3) or lsp/ (Layer 4). It holds no static or
     * global state - all mutable state lives in the instance (cached grammar symbol IDs and
     * the compiled TAGS_QUERY), and every collection entry point takes its inputs by const
     * reference and never throws; parse and validation failures are reported as Diagnostic
     * values instead.
     */
    class SymbolCollector
    {
    public:
        // =====================================================================================
        // Public API
        // =====================================================================================

        /** @brief Constructs the collector and pre-resolves all tree-sitter grammar symbols and the TAGS_QUERY. */
        explicit SymbolCollector(angel_lsp::utils::LspLogger *logger);

        /** @brief Releases the compiled TAGS_QUERY. */
        ~SymbolCollector();

        /**
         * @brief Parses sourceCode and collects its symbols into symbolTable.
         * @param fileUri Document URI used to tag every collected symbol and diagnostic.
         * @param sourceCode Full text of the document.
         * @param parser Tree-sitter parser used to produce the AST (and immediately discarded).
         * @param symbolTable Table that receives the collected symbols.
         * @param i18n Optional localizer for diagnostic messages; English is used when null.
         * @param typeConfig Optional type configuration (reserved for future type resolution).
         * @return Diagnostics produced while parsing and validating the document.
         */
        std::vector<Diagnostic> CollectSymbols(const std::string &fileUri, const std::string &sourceCode, angel_lsp::parser::AngelScriptParser &parser, SymbolTable &symbolTable, const angel_lsp::i18n::I18n *i18n = nullptr, const angel_lsp::config::TypeConfig *typeConfig = nullptr);

        /**
         * @brief Collects symbols from an already-parsed tree, without owning or freeing it.
         * @param tree Pre-parsed tree-sitter tree; the caller retains ownership.
         * @see CollectSymbols for the remaining parameters.
         */
        std::vector<Diagnostic> CollectSymbolsWithTree(const std::string &fileUri, const std::string &sourceCode, TSTree *tree, SymbolTable &symbolTable, const angel_lsp::i18n::I18n *i18n = nullptr, const angel_lsp::config::TypeConfig *typeConfig = nullptr);

        // =====================================================================================
        // AST Helpers
        // =====================================================================================

        /** @brief Returns the child of node bound to fieldName, or a null TSNode if absent. */
        static TSNode GetChildByFieldName(TSNode node, const char *fieldName)
        {
            return ts_node_child_by_field_name(node, fieldName, static_cast<uint32_t>(std::strlen(fieldName)));
        }

    private:
        utils::LspLogger *m_logger;
        TSQuery *m_tagsQuery;

        // =====================================================================================
        // Cached grammar symbols (tree-sitter TSSymbol IDs, resolved once per instance)
        // =====================================================================================

        TSSymbol m_symDeclarationModifier = 0;
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
        TSSymbol m_symVariableDeclarator = 0;
        TSSymbol m_symAccessor = 0;
        TSSymbol m_symImportDeclaration = 0;

        TSSymbol m_symScopedIdentifier = 0;
        TSSymbol m_symMixinDeclaration = 0;
        TSSymbol m_symSharedExternalModifier = 0;
        TSSymbol m_symVirtualProperty = 0;
        TSSymbol m_symCompoundStatement = 0;
        TSSymbol m_symBlock = 0;
        TSSymbol m_symBaseClassList = 0;
        TSSymbol m_symParameter = 0;
        TSSymbol m_symMemberExpression = 0;

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

        // =====================================================================================
        // AST Traversal & Scope Helpers
        // =====================================================================================

        /** @brief Tracks the lexical scope a TAGS_QUERY match was found in while it is dispatched. */
        struct CollectionContext
        {
            std::string containerPath;
            bool isInsideFunction = false;
            bool isInsideClass = false;
            bool isInsideNamespace = false;
        };

        /** @brief Member-function pointer type used to dispatch a TAGS_QUERY capture to its handler. */
        using ProcessFn = void (SymbolCollector::*)(TSNode, const std::string &, const std::string &, SymbolTable &, const CollectionContext &);

        /** @brief Maps each TAGS_QUERY capture index (by definition order) to its handler, built once in the constructor. */
        std::vector<ProcessFn> m_captureDispatch;

        /** @brief Member-function pointer type used to dispatch a TAGS_QUERY capture to a diagnostics-only validation handler. */
        using ValidationFn = void (SymbolCollector::*)(TSNode, const std::string &, const std::string &, std::vector<Diagnostic> &, const angel_lsp::i18n::I18n *) const;

        /**
         * @brief Maps each TAGS_QUERY capture index to its validation handler, built once in the
         *        constructor. Kept separate from m_captureDispatch (rather than widening ProcessFn)
         *        because validation captures need no SymbolTable/CollectionContext - they only
         *        append Diagnostics, so BuildContext's O(depth) ancestor walk is skipped for them.
         *        A given capture index is only ever present in one of the two tables.
         */
        std::vector<ValidationFn> m_validationDispatch;

        /** @brief Runs diagnostics and the TAGS_QUERY dispatch loop over a parsed root node. */
        void CollectFromTree(TSNode rootNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const angel_lsp::i18n::I18n *i18n, std::vector<Diagnostic> &diagnostics);

        /** @brief Walks node's ancestors to derive the enclosing container path and function/class/namespace nesting flags. */
        CollectionContext BuildContext(TSNode node, const std::string &sourceCode) const;

        // =====================================================================================
        // Declaration Collectors (TAGS_QUERY @definition.* handlers)
        // =====================================================================================

        /** @brief Collects a global/class-field/namespace variable declaration or virtual property; local variables are skipped. */
        void ProcessVariable(TSNode varDeclNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);

        /** @brief Collects a function, method, interface method, or imported function declaration. */
        void ProcessFunction(TSNode funcNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);

        /** @brief Collects a class or mixin declaration, including its base list and template parameters. */
        void ProcessClass(TSNode classNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);

        /** @brief Collects a namespace declaration. */
        void ProcessNamespace(TSNode namespaceNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);

        /** @brief Collects a typedef declaration. */
        void ProcessTypedef(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);

        /** @brief Collects a funcdef (function-pointer type alias) declaration. */
        void ProcessFuncdef(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);

        /** @brief Collects an enum declaration and indexes each of its members as Variable symbols. */
        void ProcessEnum(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);

        /** @brief Collects a standalone virtual property (get/set) declaration. */
        void ProcessProperty(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);

        /** @brief Collects an interface declaration, including its inherited interface list. */
        void ProcessInterface(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);

        // =====================================================================================
        // Reference & Out-of-Body Call Collectors (TAGS_QUERY @reference.* handlers)
        // =====================================================================================

        /**
         * @brief Records a function or method call that occurs outside any function body
         *        (e.g. a global/class-field initializer or an enum member value) as a
         *        CallReference symbol. Calls made from inside a function body are ignored here,
         *        since they belong to that function's own body analysis instead.
         */
        void ProcessCallReference(TSNode callNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx);

        // =====================================================================================
        // Validation Collectors (TAGS_QUERY @validation.* handlers)
        // =====================================================================================

        /** @brief Flags `using namespace <reserved-keyword>;` declarations as invalid identifiers. */
        void CheckUsingDeclarationCapture(TSNode usingNode, const std::string &sourceCode, const std::string &fileUri, std::vector<Diagnostic> &diagnostics, const angel_lsp::i18n::I18n *i18n) const;

        /**
         * @brief Flags repeated declaration modifiers (e.g. `final final class Foo {}`) as warnings.
         *        Handles both declaration_modifier (class/mixin) and shared_external_modifier
         *        (interface) child nodes, since the query matches all three declaration kinds.
         */
        void CheckDuplicateModifierGroup(TSNode declNode, const std::string &sourceCode, const std::string &fileUri, std::vector<Diagnostic> &diagnostics, const angel_lsp::i18n::I18n *i18n) const;

        // =====================================================================================
        // Diagnostics & Error Recovery
        // =====================================================================================

        /** @brief Recursively reports tree-sitter ERROR/MISSING nodes as syntax-error diagnostics. */
        void ReportParseErrors(TSNode node, const std::string &fileUri, const std::string &sourceCode, std::vector<Diagnostic> &diagnostics, const angel_lsp::i18n::I18n *i18n = nullptr, int depth = 0) const;

        // =====================================================================================
        // AST/Text Extraction Helpers
        // =====================================================================================

        /** @brief Returns the source text spanned by node, or an empty string for a null/degenerate node. */
        std::string GetNodeText(TSNode node, const std::string &sourceCode) const;

        /** @brief Non-owning view equivalent of GetNodeText; valid only as long as sourceCode is alive. */
        std::string_view GetNodeView(TSNode node, const std::string &sourceCode) const;

        /** @brief Extracts access/const/handle/shared/... modifiers from a declaration_modifier or func_attributes child list. */
        SymbolModifiers ExtractModifiers(TSNode node, const std::string &sourceCode) const;

        /** @brief Applies a single anonymous modifier token (e.g. "const", "shared") onto modifiers. */
        void ApplyModifierToken(TSSymbol tokenSymbol, SymbolModifiers &modifiers) const;

        /** @brief Returns true if node or any of its descendants is a null_literal. */

        /** @brief Extracts name, type, and modifier information from a single function/method parameter node. */
        ParameterInformation ExtractParameterInfo(TSNode paramNode, const std::string &sourceCode) const;

        /** @brief Extracts all parameters from a parameter_list node, collapsing a lone `(void)` to an empty list. */
        std::vector<ParameterInformation> ExtractParameters(TSNode paramsNode, const std::string &sourceCode) const;

        /** @brief Builds a Symbol with its type, name, container, qualified name, file URI, and source range populated. */
        Symbol CreateSymbol(SymbolType type, TSNode node, TSNode nameNode, const std::string &sourceCode, const std::string &fileUri, const std::string &containerPath) const;

        /** @brief Extracts the base class/interface name list from a class or interface declaration's base_class_list. */
        std::vector<std::string> ExtractBases(TSNode classNode, const std::string &sourceCode) const;
    };
}
