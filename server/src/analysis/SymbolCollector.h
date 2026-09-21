#pragma once

#include "analysis/Diagnostics.h"
#include "analysis/SymbolTable.h"
#include "analysis/TypeExtraction.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include "config/ServerConfig.h"
#include "parser/GrammarNames.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

// Forward-declared to ensure AST symbol extraction remains in Layer 2 without pulling protocol definitions.
namespace angel_lsp::utils
{
class LspLogger;
}

namespace angel_lsp::analysis
{
/**
 * @brief Input parameters for symbol collection.
 */
struct SymbolCollectRequest
{
    /** @brief Document URI used to tag every collected symbol and diagnostic. */
    const std::string& fileUri;
    /** @brief Full text of the document. */
    const std::string& sourceCode;
    /** @brief Optional localizer for diagnostic messages; English is used when null. */
    const angel_lsp::i18n::I18n* i18n = nullptr;

    /**
     * @brief Constructs a SymbolCollectRequest bundle.
     * @param[in] uri Document URI string.
     * @param[in] code Full text of the document.
     * @param[in] loc Optional localizer pointer.
     */
    SymbolCollectRequest(const std::string& uri, const std::string& code, const angel_lsp::i18n::I18n* loc = nullptr)
        : fileUri(uri), sourceCode(code), i18n(loc)
    {
    }
};

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
    explicit SymbolCollector(angel_lsp::utils::LspLogger* logger);

    /** @brief Releases the compiled TAGS_QUERY. */
    ~SymbolCollector();

    /**
     * @brief Parses sourceCode and collects its symbols into symbolTable.
     * @param[in] request Request bundle containing file URI, source text, and optional localizer.
     * @param[in,out] parser Tree-sitter parser used to produce the AST.
     * @param[in,out] symbolTable Table that receives the collected symbols.
     * @return Diagnostics produced while parsing and validating the document.
     */
    std::vector<Diagnostic> CollectSymbols(const SymbolCollectRequest& request,
                                           angel_lsp::parser::AngelScriptParser& parser, SymbolTable& symbolTable);

    /**
     * @brief Convenience overload collecting symbols from URI and sourceCode with default localization.
     * @param[in] fileUri Document URI used to tag every collected symbol and diagnostic.
     * @param[in] sourceCode Full text of the document.
     * @param[in,out] parser Tree-sitter parser used to produce the AST.
     * @param[in,out] symbolTable Table that receives the collected symbols.
     * @return Diagnostics produced while parsing and validating the document.
     */
    std::vector<Diagnostic> CollectSymbols(const std::string& fileUri, const std::string& sourceCode,
                                           angel_lsp::parser::AngelScriptParser& parser, SymbolTable& symbolTable);

    /**
     * @brief Collects symbols from an already-parsed tree, without owning or freeing it.
     * @param[in] request Request bundle containing file URI, source text, and optional localizer.
     * @param[in] tree Pre-parsed tree-sitter tree; the caller retains ownership.
     * @param[in,out] symbolTable Table that receives the collected symbols.
     * @return Diagnostics produced while validating the document.
     */
    std::vector<Diagnostic> CollectSymbolsWithTree(const SymbolCollectRequest& request, TSTree* tree,
                                                   SymbolTable& symbolTable);

    /**
     * @brief Convenience overload collecting symbols from an already-parsed tree with default localization.
     * @param[in] fileUri Document URI used to tag every collected symbol and diagnostic.
     * @param[in] sourceCode Full text of the document.
     * @param[in] tree Pre-parsed tree-sitter tree; the caller retains ownership.
     * @param[in,out] symbolTable Table that receives the collected symbols.
     * @return Diagnostics produced while validating the document.
     */
    std::vector<Diagnostic> CollectSymbolsWithTree(const std::string& fileUri, const std::string& sourceCode,
                                                   TSTree* tree, SymbolTable& symbolTable);

    // =====================================================================================
    // AST Helpers
    // =====================================================================================

    /**
     * @brief Returns the child of node bound to fieldName, or a null TSNode if absent.
     * @param[in] node Parent tree-sitter node.
     * @param[in] fieldName Field name string.
     * @return Child tree-sitter node.
     */
    static TSNode GetChildByFieldName(TSNode node, const char* fieldName)
    {
        return parser::GetChildByField(node, fieldName);
    }

  private:
    utils::LspLogger* m_logger;
    TSQuery* m_tagsQuery = nullptr;

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
    // Internal Bundled Contexts
    // =====================================================================================

    /** @brief Tracks the lexical scope a TAGS_QUERY match was found in while it is dispatched. */
    struct CollectionContext
    {
        std::string containerPath;
        bool isInsideFunction = false;
        bool isInsideClass = false;
        bool isInsideNamespace = false;
    };

    /** @brief Bundles mutable collectors and immutable request state during AST walks. */
    struct SymbolCollectContext
    {
        const SymbolCollectRequest& request;
        SymbolTable& symbolTable;
        std::vector<Diagnostic>& diagnostics;
    };

    /** @brief Bundles URI and source context for symbol creation. */
    struct SymbolLocationContext
    {
        const std::string& sourceCode;
        const std::string& fileUri;
        const std::string& containerPath;
    };

    struct VariableHeaderInfo
    {
        std::string typeStr;
        TypeExtractionResult typeInfo;
        SymbolModifiers modifiers;
        bool hasSemicolon = false;
    };

    // =====================================================================================
    // Query Dispatch Types & Members
    // =====================================================================================

    /** @brief Member-function pointer type used to dispatch a TAGS_QUERY capture to its handler. */
    using ProcessFn = void (SymbolCollector::*)(TSNode, SymbolCollectContext&, const CollectionContext&);

    /** @brief Maps each TAGS_QUERY capture index to its handler, built once in the constructor. */
    std::vector<ProcessFn> m_captureDispatch;

    /** @brief Member-function pointer type used to dispatch a validation capture to its handler. */
    using ValidationFn = void (SymbolCollector::*)(TSNode, SymbolCollectContext&) const;

    /** @brief Maps each TAGS_QUERY capture index to its validation handler. */
    std::vector<ValidationFn> m_validationDispatch;

    // =====================================================================================
    // Initialization Helpers
    // =====================================================================================

    void ResolveGrammarSymbols(const TSLanguage* lang);
    void InitQueryDispatch(const TSLanguage* lang);
    void PopulateDispatchTables();
    ProcessFn ResolveCaptureHandler(std::string_view captureName) const;
    ValidationFn ResolveValidationHandler(std::string_view captureName) const;

    // =====================================================================================
    // AST Traversal & Scope Helpers
    // =====================================================================================

    /** @brief Runs diagnostics and the TAGS_QUERY dispatch loop over a parsed root node. */
    void CollectFromTree(TSNode rootNode, SymbolCollectContext& sCtx);

    /** @brief Walks node's ancestors to derive the enclosing container path and nesting flags. */
    CollectionContext BuildContext(TSNode node, const std::string& sourceCode) const;

    // =====================================================================================
    // Declaration Collectors (TAGS_QUERY @definition.* handlers)
    // =====================================================================================

    void ProcessVariable(TSNode varDeclNode, SymbolCollectContext& sCtx, const CollectionContext& ctx);
    void ProcessVirtualPropertyVariable(TSNode varDeclNode, SymbolCollectContext& sCtx, const CollectionContext& ctx);
    void ProcessRegularVariable(TSNode varDeclNode, SymbolCollectContext& sCtx, const CollectionContext& ctx);
    void CollectDeclaratorSymbol(TSNode declaratorNode, const VariableHeaderInfo& header, SymbolCollectContext& sCtx,
                                 const CollectionContext& ctx);
    void ParseAccessorNodes(TSNode node, VariableSignature& varSig) const;
    void ParseSingleAccessor(TSNode accNode, VariableSignature& varSig) const;
    void ApplyAccessorTokens(TSNode accNode, bool isGet, bool isSet, VariableSignature& varSig) const;

    void ProcessFunction(TSNode funcNode, SymbolCollectContext& sCtx, const CollectionContext& ctx);
    TSNode FindFunctionBody(TSNode funcNode) const;
    bool IsExternalFunction(TSNode funcNode) const;
    std::string ExtractOriginModule(TSNode funcNode, const std::string& sourceCode) const;

    void ProcessClass(TSNode classNode, SymbolCollectContext& sCtx, const CollectionContext& ctx);
    void ProcessNamespace(TSNode namespaceNode, SymbolCollectContext& sCtx, const CollectionContext& ctx);
    void ProcessTypedef(TSNode node, SymbolCollectContext& sCtx, const CollectionContext& ctx);
    void ProcessFuncdef(TSNode node, SymbolCollectContext& sCtx, const CollectionContext& ctx);

    void ProcessEnum(TSNode node, SymbolCollectContext& sCtx, const CollectionContext& ctx);
    bool EnumHasBraces(TSNode node) const;
    void CollectEnumMembers(TSNode node, const std::string& sourceCode, EnumSignature& enumSig) const;
    void PublishEnumMembers(TSNode node, const EnumSignature& enumSig, SymbolCollectContext& sCtx,
                            const CollectionContext& ctx);

    void ProcessProperty(TSNode node, SymbolCollectContext& sCtx, const CollectionContext& ctx);
    void ProcessInterface(TSNode node, SymbolCollectContext& sCtx, const CollectionContext& ctx);

    // =====================================================================================
    // Reference & Out-of-Body Call Collectors
    // =====================================================================================

    void ProcessCallReference(TSNode callNode, SymbolCollectContext& sCtx, const CollectionContext& ctx);

    // =====================================================================================
    // Validation Collectors
    // =====================================================================================

    void CheckUsingDeclarationCapture(TSNode usingNode, SymbolCollectContext& sCtx) const;
    void CheckDuplicateModifierGroup(TSNode declNode, SymbolCollectContext& sCtx) const;

    // =====================================================================================
    // Diagnostics & Error Recovery
    // =====================================================================================

    void ReportParseErrors(TSNode rootNode, SymbolCollectContext& sCtx) const;
    void EmitParseErrorDiagnostic(TSNode node, SymbolCollectContext& sCtx) const;
    std::string FormatSyntaxErrorMessage(const std::string& rawErrText, std::string& outCode,
                                         const angel_lsp::i18n::I18n* i18n) const;

    // =====================================================================================
    // AST/Text Extraction Helpers
    // =====================================================================================

    std::string GetNodeText(TSNode node, const std::string& sourceCode) const;
    std::string_view GetNodeView(TSNode node, const std::string& sourceCode) const;

    SymbolModifiers ExtractModifiers(TSNode node, const std::string& sourceCode) const;
    void ProcessModifierChild(TSNode child, const std::string& sourceCode, SymbolModifiers& modifiers) const;
    void ApplyModifierString(std::string_view text, SymbolModifiers& modifiers, bool isFuncAttr) const;
    bool ApplyAccessOrStorageString(std::string_view text, SymbolModifiers& modifiers) const;
    void ApplyModifierToken(TSSymbol tokenSymbol, SymbolModifiers& modifiers) const;
    bool ApplyParamModifierToken(TSSymbol tokenSymbol, SymbolModifiers& modifiers) const;

    ParameterInformation ExtractParameterInfo(TSNode paramNode, const std::string& sourceCode) const;
    void ExtractParamTypeRefAndConst(TSNode pTypeNode, const std::string& sourceCode, ParameterInformation& paramInfo,
                                     uint32_t& refCount) const;
    void ExtractParamModifierTokens(TSNode paramNode, const std::string& sourceCode, ParameterInformation& paramInfo,
                                    uint32_t& refCount) const;

    std::vector<ParameterInformation> ExtractParameters(TSNode paramsNode, const std::string& sourceCode) const;

    Symbol CreateSymbol(SymbolType type, TSNode node, TSNode nameNode, const SymbolLocationContext& loc) const;

    std::vector<std::string> ExtractBases(TSNode classNode, const std::string& sourceCode) const;
};
} // namespace angel_lsp::analysis
