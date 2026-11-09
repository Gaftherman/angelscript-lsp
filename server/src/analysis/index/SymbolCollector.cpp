#include "analysis/index/SymbolCollector.h"

#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/DeclarationRecovery.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "document/Document.h"
#include "parser/ASTUtils.h"
#include "parser/BraceBalance.h"
#include "parser/SyntaxTree.h"
#include "spdlog/fmt/fmt.h"
#include "utils/LspLogger.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <ankerl/unordered_dense.h>
#include <cctype>
#include <charconv>
#include <cstring>
#include <string_view>

extern "C" const TSLanguage* tree_sitter_angelscript();

namespace angel_lsp::analysis
{
#define SYM_NAME(str) str, static_cast<uint32_t>(sizeof(str) - 1)

/**
 * @brief Strips leading and trailing ASCII whitespace characters from a string view.
 * @param[in] sv Input string view to trim.
 * @return Trimmed string view without leading or trailing whitespace.
 */
static std::string_view TrimView(std::string_view sv)
{
    while (!sv.empty() && isspace(static_cast<unsigned char>(sv.front())))
    {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && isspace(static_cast<unsigned char>(sv.back())))
    {
        sv.remove_suffix(1);
    }
    return sv;
}

/**
 * @brief Advances index past whitespace and comments in text buffer.
 * @param[in] text Source text buffer.
 * @param[in,out] i Cursor index advanced past comments and whitespace.
 */
static void SkipWhitespaceAndComments(std::string_view text, size_t& i)
{
    while (i < text.size())
    {
        if (isspace(static_cast<unsigned char>(text[i])))
        {
            ++i;
            continue;
        }
        if (text.substr(i).starts_with("//"))
        {
            size_t nl = text.find('\n', i + 2);
            i = (nl == std::string_view::npos) ? text.size() : (nl + 1);
            continue;
        }
        if (text.substr(i).starts_with("/*"))
        {
            size_t end = text.find("*/", i + 2);
            i = (end == std::string_view::npos) ? text.size() : (end + 2);
            continue;
        }
        break;
    }
}

static std::string ExtractFirstToken(std::string_view rawErrText)
{
    size_t i = 0;
    SkipWhitespaceAndComments(rawErrText, i);
    std::string firstToken;
    while (i < rawErrText.size() && !isspace(static_cast<unsigned char>(rawErrText[i])) && firstToken.size() < 24)
    {
        char c = rawErrText[i++];
        if (static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) != 127)
        {
            firstToken += c;
        }
    }
    return firstToken;
}

using RecoveredClassContainer = SymbolCollector::RecoveredClassContainer;

static ScannedClassHeader FindClassHeaderInError(TSNode node, const parser::GrammarSymbols& gs)
{
    ScannedClassHeader result;
    TSTreeCursor inner = ts_tree_cursor_new(node);
    if (!ts_tree_cursor_goto_first_child(&inner))
    {
        ts_tree_cursor_delete(&inner);
        return result;
    }

    do
    {
        TSNode child = ts_tree_cursor_current_node(&inner);
        TSSymbol sym = ts_node_symbol(child);
        if (sym == gs.symComment)
        {
            continue;
        }

        if (sym == gs.tokClass)
        {
            result.isClass = true;
            if (ts_node_is_null(result.headerStart))
                result.headerStart = child;
        }
        else if (sym == gs.tokInterface)
        {
            result.isInterface = true;
            if (ts_node_is_null(result.headerStart))
                result.headerStart = child;
        }
        else if (sym == gs.symDeclarationModifier && ts_node_is_null(result.headerStart))
        {
            result.headerStart = child;
        }
        else if ((result.isClass || result.isInterface) && sym == gs.symIdentifier && ts_node_is_null(result.nameNode))
        {
            result.nameNode = child;
            break;
        }
    } while (ts_tree_cursor_goto_next_sibling(&inner));
    ts_tree_cursor_delete(&inner);
    return result;
}

static void TryRecoverClassFromErrorNode(TSNode node, RecoverClassContext& ctx)
{
    ScannedClassHeader header = FindClassHeaderInError(node, ctx.gs);
    if (ts_node_is_null(header.nameNode) || ts_node_is_null(header.headerStart))
    {
        return;
    }

    std::string className(parser::NodeText(header.nameNode, ctx.sourceCode));
    if (className.empty())
    {
        return;
    }

    TSPoint startPt = ts_node_start_point(header.headerStart);
    TSPoint endPt = ts_node_end_point(node);
    TSPoint nameStartPt = ts_node_start_point(header.nameNode);
    TSPoint nameEndPt = ts_node_end_point(header.nameNode);

    Symbol sym;
    sym.type = header.isInterface ? SymbolType::Interface : SymbolType::Class;
    sym.name = className;
    sym.qualifiedName = className;
    sym.fileUri = ctx.fileUri;
    sym.isPredefined = angel_lsp::utils::IsPredefinedFile(ctx.fileUri, ".as.predefined");
    sym.startLine = startPt.row;
    sym.startCharacter = startPt.column;
    sym.endLine = endPt.row;
    sym.endCharacter = endPt.column;
    sym.fullRange = {startPt.row, startPt.column, endPt.row, endPt.column};
    sym.selectionRange = {nameStartPt.row, nameStartPt.column, nameEndPt.row, nameEndPt.column};
    if (header.isInterface)
        sym.signature = InterfaceSignature{};
    else
        sym.signature = ClassSignature{};

    ctx.outSymbols.push_back(std::move(sym));
    ctx.outContainers.push_back({ts_node_start_byte(header.headerStart), ts_node_end_byte(node), std::move(className),
                                 header.isInterface ? ContainerKind::Interface : ContainerKind::Class});
}

static void ScanRecoverableClasses(TSNode rootNode, RecoverClassContext& ctx)
{
    TSTreeCursor cursor = ts_tree_cursor_new(rootNode);
    if (!ts_tree_cursor_goto_first_child(&cursor))
    {
        ts_tree_cursor_delete(&cursor);
        return;
    }

    do
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        if (ts_node_is_error(node))
        {
            TryRecoverClassFromErrorNode(node, ctx);
        }
    } while (ts_tree_cursor_goto_next_sibling(&cursor));

    ts_tree_cursor_delete(&cursor);
}

// Public API & Lifecycle

SymbolCollector::SymbolCollector(angel_lsp::utils::LspLogger* logger)
    : m_logger(logger), m_gs(parser::GetGrammarSymbols())
{
}

/**
 * @brief Maps an indexed declaration kind to the member function that collects it.
 * @param[in] kind Declaration kind recorded by the node index.
 * @return Handler for the kind, or nullptr for kinds collected elsewhere.
 */
SymbolCollector::ProcessFn SymbolCollector::DeclarationHandler(IndexedDeclarationKind kind)
{
    switch (kind)
    {
    case IndexedDeclarationKind::Function:
        return &SymbolCollector::ProcessFunction;
    case IndexedDeclarationKind::Class:
        return &SymbolCollector::ProcessClass;
    case IndexedDeclarationKind::Interface:
        return &SymbolCollector::ProcessInterface;
    case IndexedDeclarationKind::Namespace:
        return &SymbolCollector::ProcessNamespace;
    case IndexedDeclarationKind::Enum:
        return &SymbolCollector::ProcessEnum;
    case IndexedDeclarationKind::Typedef:
        return &SymbolCollector::ProcessTypedef;
    case IndexedDeclarationKind::Funcdef:
        return &SymbolCollector::ProcessFuncdef;
    case IndexedDeclarationKind::Property:
        return &SymbolCollector::ProcessProperty;
    case IndexedDeclarationKind::Variable:
        return &SymbolCollector::ProcessVariable;
    case IndexedDeclarationKind::Lambda:
        return &SymbolCollector::ProcessLambda;
    case IndexedDeclarationKind::Call:
        return &SymbolCollector::ProcessCallReference;
    default:
        return nullptr;
    }
}

SymbolCollector::~SymbolCollector() = default;

/**
 * @brief Parses source code and populates the symbol table with declarations and diagnostics.
 * @param[in] request Request containing document source code and file URI.
 * @param[in,out] parser Tree-sitter parser instance used for document parsing.
 * @param[in,out] symbolTable Destination symbol table to populate with discovered declarations.
 * @return Vector of parsing and declaration diagnostics discovered during symbol collection.
 */
std::vector<Diagnostic> SymbolCollector::CollectSymbols(const SymbolCollectRequest& request,
                                                        angel_lsp::parser::AngelScriptParser& parser,
                                                        SymbolTable& symbolTable)
{
    const auto tree = parser::MakeTreePtr(parser.Parse(request.sourceCode));
    return CollectSymbolsWithTree(request, tree.get(), symbolTable);
}

std::vector<Diagnostic> SymbolCollector::CollectSymbols(const std::string& fileUri, const std::string& sourceCode,
                                                        angel_lsp::parser::AngelScriptParser& parser,
                                                        SymbolTable& symbolTable)
{
    return CollectSymbols(SymbolCollectRequest(fileUri, sourceCode), parser, symbolTable);
}

std::vector<Diagnostic> SymbolCollector::CollectSymbolsWithIndex(const SymbolCollectRequest& request,
                                                                 const NodeIndex& index, SymbolTable& symbolTable)
{
    std::vector<Diagnostic> diagnostics;
    if (index.Empty())
    {
        return diagnostics;
    }
    if (request.sourceCode.size() > 16384)
    {
        symbolTable.Reserve(request.sourceCode.size() / 100);
    }

    SymbolCollectContext sCtx{request, symbolTable, diagnostics, index};
    sCtx.isPredefined = angel_lsp::utils::IsPredefinedFile(request.fileUri, ".as.predefined");
    CollectFromIndex(index, sCtx);
    symbolTable.ResolveIncludedMixins();
    return diagnostics;
}

std::vector<Diagnostic> SymbolCollector::CollectSymbolsWithTree(const SymbolCollectRequest& request, TSTree* tree,
                                                                SymbolTable& symbolTable)
{
    if (!tree)
    {
        return {};
    }
    const NodeIndex index(ts_tree_root_node(tree), nullptr, nullptr, NodeIndexContents::Declarations);
    return CollectSymbolsWithIndex(request, index, symbolTable);
}

std::vector<Diagnostic> SymbolCollector::CollectSymbolsWithTree(const std::string& fileUri,
                                                                const std::string& sourceCode, TSTree* tree,
                                                                SymbolTable& symbolTable)
{
    return CollectSymbolsWithTree(SymbolCollectRequest(fileUri, sourceCode), tree, symbolTable);
}

// AST Traversal & Scope Helpers

void SymbolCollector::PushClassContainer(TSNode node, const SymbolCollectContext& sCtx,
                                         std::vector<ActiveContainer>& stack) const
{
    TSNode bodyNode = GetChildByFieldName(node, "body");
    if (ts_node_is_null(bodyNode))
    {
        return;
    }
    TSNode nameNode = GetChildByFieldName(node, "name");
    std::string className(parser::NodeText(nameNode, sCtx.request.sourceCode));
    const size_t angle = className.find('<');
    if (angle != std::string_view::npos)
    {
        className.resize(angle);
    }
    if (className.empty())
    {
        return;
    }

    CollectionContext classCtx;
    classCtx.containerKind = ContainerKind::Class;
    classCtx.isInsideClass = true;
    if (!stack.empty())
    {
        classCtx.isInsideNamespace = stack.back().ctx.isInsideNamespace;
        classCtx.containerPath = utils::JoinScopedName(stack.back().ctx.containerPath, className);
    }
    else
    {
        classCtx.containerPath = className;
    }
    stack.push_back({ts_node_end_byte(bodyNode), std::move(classCtx)});
}

void SymbolCollector::PushInterfaceContainer(TSNode node, const SymbolCollectContext& sCtx,
                                             std::vector<ActiveContainer>& stack) const
{
    TSNode bodyNode = GetChildByFieldName(node, "body");
    if (ts_node_is_null(bodyNode))
    {
        return;
    }
    TSNode nameNode = GetChildByFieldName(node, "name");
    std::string ifaceName(parser::NodeText(nameNode, sCtx.request.sourceCode));
    if (ifaceName.empty())
    {
        return;
    }

    CollectionContext ifaceCtx;
    ifaceCtx.containerKind = ContainerKind::Interface;
    ifaceCtx.isInsideClass = true;
    if (!stack.empty())
    {
        ifaceCtx.isInsideNamespace = stack.back().ctx.isInsideNamespace;
        ifaceCtx.containerPath = utils::JoinScopedName(stack.back().ctx.containerPath, ifaceName);
    }
    else
    {
        ifaceCtx.containerPath = ifaceName;
    }
    stack.push_back({ts_node_end_byte(bodyNode), std::move(ifaceCtx)});
}

void SymbolCollector::PushNamespaceContainer(TSNode node, const SymbolCollectContext& sCtx,
                                             std::vector<ActiveContainer>& stack) const
{
    TSNode bodyNode = GetChildByFieldName(node, "body");
    if (ts_node_is_null(bodyNode))
    {
        return;
    }
    TSNode nameNode = GetChildByFieldName(node, "name");
    std::string nsName(parser::NodeText(nameNode, sCtx.request.sourceCode));
    if (nsName.empty())
    {
        return;
    }

    CollectionContext nsCtx;
    nsCtx.containerKind = ContainerKind::Namespace;
    nsCtx.isInsideNamespace = true;
    if (!stack.empty() && !stack.back().ctx.containerPath.empty())
    {
        nsCtx.containerPath = utils::JoinScopedName(stack.back().ctx.containerPath, nsName);
    }
    else
    {
        nsCtx.containerPath = nsName;
    }
    stack.push_back({ts_node_end_byte(bodyNode), std::move(nsCtx)});
}

void SymbolCollector::PushFunctionContainer(TSNode node, const SymbolCollectContext& sCtx,
                                            std::vector<ActiveContainer>& stack) const
{
    if (sCtx.isPredefined)
    {
        return;
    }
    TSNode bodyNode = GetChildByFieldName(node, "body");
    if (ts_node_is_null(bodyNode))
    {
        std::string_view text = parser::NodeText(node, sCtx.request.sourceCode);
        if (text.find('{') == std::string_view::npos)
        {
            return;
        }
        bodyNode = FindFunctionBody(node);
    }
    if (ts_node_is_null(bodyNode))
    {
        return;
    }
    CollectionContext funcCtx = stack.empty() ? CollectionContext{} : stack.back().ctx;
    funcCtx.isInsideFunction = true;
    stack.push_back({ts_node_end_byte(bodyNode), std::move(funcCtx)});
}

void SymbolCollector::PushContainerIfApplicable(ProcessFn fn, TSNode node, const SymbolCollectContext& sCtx,
                                                std::vector<ActiveContainer>& stack) const
{
    if (fn == &SymbolCollector::ProcessClass)
    {
        PushClassContainer(node, sCtx, stack);
    }
    else if (fn == &SymbolCollector::ProcessInterface)
    {
        PushInterfaceContainer(node, sCtx, stack);
    }
    else if (fn == &SymbolCollector::ProcessNamespace)
    {
        PushNamespaceContainer(node, sCtx, stack);
    }
    else if (fn == &SymbolCollector::ProcessFunction)
    {
        PushFunctionContainer(node, sCtx, stack);
    }
    else if (fn == &SymbolCollector::ProcessLambda)
    {
        CollectionContext lambdaCtx = stack.empty() ? CollectionContext{} : stack.back().ctx;
        lambdaCtx.isInsideFunction = true;
        stack.push_back({ts_node_end_byte(node), std::move(lambdaCtx)});
    }
}

void SymbolCollector::SyncRecoveredContainers(uint32_t nodeStart,
                                              const std::vector<RecoveredClassContainer>& recoveredClasses,
                                              std::vector<ActiveContainer>& containerStack) const
{
    for (const auto& rec : recoveredClasses)
    {
        if (rec.startByte <= nodeStart && nodeStart < rec.endByte)
        {
            bool onStack = false;
            for (const auto& c : containerStack)
            {
                if (c.ctx.containerPath == rec.className)
                {
                    onStack = true;
                    break;
                }
            }
            if (!onStack)
            {
                CollectionContext recCtx;
                recCtx.containerKind = rec.kind;
                recCtx.isInsideClass = true;
                recCtx.containerPath = rec.className;
                containerStack.push_back({rec.endByte, std::move(recCtx)});
            }
        }
    }
}

void SymbolCollector::CollectFromIndex(const NodeIndex& index, SymbolCollectContext& sCtx)
{
    const TSNode rootNode = index.Root();
    sCtx.sourceHasBrackets = !sCtx.isPredefined && (sCtx.request.sourceCode.find('[') != std::string_view::npos);
    const size_t estimatedCapacity =
        sCtx.request.sourceCode.size() > 16384 ? (sCtx.request.sourceCode.size() / 128) : 256;
    sCtx.collectedSymbols.reserve(estimatedCapacity);
    ReportParseErrors(index, sCtx);

    std::vector<RecoveredClassContainer> recoveredClasses;
    if (ts_node_has_error(rootNode))
    {
        RecoverClassContext recCtx{m_gs, sCtx.request.sourceCode, sCtx.request.fileUri, recoveredClasses,
                                   sCtx.collectedSymbols};
        ScanRecoverableClasses(rootNode, recCtx);
    }

    std::vector<ActiveContainer> containerStack;
    containerStack.reserve(16);
    index.ForEachDeclaration([&](TSNode node, IndexedDeclarationKind kind)
                             { DispatchDeclaration(node, kind, sCtx, containerStack, recoveredClasses); });

    if (ts_node_has_error(rootNode) && !sCtx.isRecoveringSwallowed)
    {
        RecoverSwallowedDeclarations(sCtx);
    }

    if (sCtx.isRecoveringSwallowed)
    {
        return;
    }

    sCtx.symbolTable.AddSymbols(std::move(sCtx.collectedSymbols));
}

void SymbolCollector::DispatchDeclaration(TSNode node, IndexedDeclarationKind kind, SymbolCollectContext& sCtx,
                                          std::vector<ActiveContainer>& containerStack,
                                          const std::vector<RecoveredClassContainer>& recoveredClasses)
{
    if (kind == IndexedDeclarationKind::Using)
    {
        CheckUsingDeclarationCapture(node, sCtx);
        return;
    }

    const ProcessFn handler = DeclarationHandler(kind);
    if (!handler)
    {
        return;
    }

    const uint32_t nodeStart = ts_node_start_byte(node);
    while (!containerStack.empty() && containerStack.back().endByte <= nodeStart)
    {
        containerStack.pop_back();
    }
    if (!recoveredClasses.empty())
    {
        SyncRecoveredContainers(nodeStart, recoveredClasses, containerStack);
    }

    const CollectionContext ctx = containerStack.empty() ? CollectionContext{} : containerStack.back().ctx;
    (this->*handler)(node, sCtx, ctx);
    PushContainerIfApplicable(handler, node, sCtx, containerStack);
    if (kind == IndexedDeclarationKind::Class || kind == IndexedDeclarationKind::Interface)
    {
        CheckDuplicateModifierGroup(node, sCtx);
    }
}

void SymbolCollector::RecoverSwallowedDeclarations(SymbolCollectContext& sCtx)
{
    ankerl::unordered_dense::set<std::string> knownNames;
    knownNames.reserve(sCtx.collectedSymbols.size());
    for (const auto& sym : sCtx.collectedSymbols)
    {
        knownNames.insert(sym.name);
    }

    const auto candidates = FindSwallowedDeclarations(sCtx.request.sourceCode, knownNames);
    if (candidates.empty())
    {
        return;
    }

    angel_lsp::parser::AngelScriptParser parser;
    for (const auto& cand : candidates)
    {
        parser::TreePtr snippetTree = parser::MakeTreePtr(parser.Parse(cand.snippet));
        if (!snippetTree)
            continue;

        SymbolTable snippetTable;
        std::vector<Diagnostic> snippetDiags;
        SymbolCollectRequest snippetReq(sCtx.request.fileUri, cand.snippet);
        const NodeIndex snippetIndex(ts_tree_root_node(snippetTree.get()), nullptr, nullptr,
                                     NodeIndexContents::Declarations);
        SymbolCollectContext snippetCtx{snippetReq, snippetTable, snippetDiags, snippetIndex};
        snippetCtx.isPredefined = sCtx.isPredefined;
        snippetCtx.isRecoveringSwallowed = true;

        CollectFromIndex(snippetIndex, snippetCtx);

        for (auto& sym : snippetCtx.collectedSymbols)
        {
            AdjustRecoveredSymbolOffsets(sym, cand.startLine, cand.startCol);
            sCtx.collectedSymbols.push_back(std::move(sym));
        }
    }
}

void SymbolCollector::ProcessLambda([[maybe_unused]] TSNode node, [[maybe_unused]] SymbolCollectContext& sCtx,
                                    [[maybe_unused]] const CollectionContext& ctx)
{
}

// Declaration Collectors

static void ApplyOverrideAndFinal(TSSymbol cSym, const parser::GrammarSymbols& gs, bool isGet, bool isSet,
                                  VariableSignature& varSig)
{
    if (cSym == gs.tokOverride)
    {
        if (isGet)
        {
            varSig.isGetOverride = true;
        }
        else if (isSet)
        {
            varSig.isSetOverride = true;
        }
    }
    else if (cSym == gs.tokFinal)
    {
        if (isGet)
        {
            varSig.isGetFinal = true;
        }
        else if (isSet)
        {
            varSig.isSetFinal = true;
        }
    }
}

/**
 * @brief Applies accessor tokens to a variable signature.
 * @param[in] accNode AST node of the accessor declaration.
 * @param[in] isGet True if getter accessor was declared.
 * @param[in] isSet True if setter accessor was declared.
 * @param[in,out] varSig VariableSignature being updated with accessor traits.
 */
void SymbolCollector::ApplyAccessorTokens(TSNode accNode, bool isGet, bool isSet, VariableSignature& varSig) const
{
    TSTreeCursor cursor = ts_tree_cursor_new(accNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            const TSSymbol cSym = ts_node_symbol(child);
            if (cSym == m_gs.tokConst && isGet)
            {
                varSig.isGetConst = true;
            }
            else if (cSym == m_gs.symFuncAttributes)
            {
                ApplyFuncAttributeTokens(child, isGet, isSet, varSig);
            }
            else
            {
                ApplyOverrideAndFinal(cSym, m_gs, isGet, isSet, varSig);
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

/**
 * @brief Evaluates function attributes applied to an accessor node.
 * @param[in] attrNode AST node of the attribute list.
 * @param[in] isGet True if getter accessor was declared.
 * @param[in] isSet True if setter accessor was declared.
 * @param[in,out] varSig VariableSignature being updated with accessor traits.
 */
void SymbolCollector::ApplyFuncAttributeTokens(TSNode attrNode, bool isGet, bool isSet, VariableSignature& varSig) const
{
    TSTreeCursor cursor = ts_tree_cursor_new(attrNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            ApplyOverrideAndFinal(ts_node_symbol(child), m_gs, isGet, isSet, varSig);
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

void SymbolCollector::ParseSingleAccessor(TSNode accNode, VariableSignature& varSig) const
{
    TSNode kindNode = GetChildByFieldName(accNode, "kind");
    TSNode bodyNode = GetChildByFieldName(accNode, "body");
    TSSymbol kindSym = !ts_node_is_null(kindNode) ? ts_node_symbol(kindNode) : 0;
    bool isGet = (kindSym == m_gs.tokGet);
    bool isSet = (kindSym == m_gs.tokSet);
    bool hasBody = !ts_node_is_null(bodyNode);

    if (isGet)
    {
        if (varSig.hasGet)
            varSig.hasDuplicateGet = true;
        varSig.hasGet = true;
        if (hasBody)
            varSig.hasBodyGet = true;
    }
    else if (isSet)
    {
        if (varSig.hasSet)
            varSig.hasDuplicateSet = true;
        varSig.hasSet = true;
        if (hasBody)
            varSig.hasBodySet = true;
    }

    ApplyAccessorTokens(accNode, isGet, isSet, varSig);
}

/**
 * @brief Symbol collector parse accessor nodes.
 * @param[in] node AST node under evaluation.
 * @param[in,out] varSig VariableSignature being updated with accessor traits.
 */
void SymbolCollector::ParseAccessorNodes(TSNode node, VariableSignature& varSig) const
{
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode accNode = ts_tree_cursor_current_node(&cursor);
            if (ts_node_is_named(accNode) && ts_node_symbol(accNode) == m_gs.symAccessor)
            {
                ParseSingleAccessor(accNode, varSig);
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

/**
 * @brief Symbol collector process virtual property variable.
 * @param[in] varDeclNode AST node under evaluation.
 * @param[in,out] sCtx Analysis or feature execution context.
 * @param[in] ctx Diagnostic collection and analysis context.
 */
void SymbolCollector::ProcessVirtualPropertyVariable(TSNode varDeclNode, SymbolCollectContext& sCtx,
                                                     const CollectionContext& ctx)
{
    TSNode typeNode = GetChildByFieldName(varDeclNode, "prop_type");
    TSNode nameNode = GetChildByFieldName(varDeclNode, "name");
    std::string typeStr(parser::NodeText(typeNode, sCtx.request.sourceCode));
    TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, sCtx.request.sourceCode, &sCtx.index);
    SymbolModifiers modifiers = ExtractModifiers(varDeclNode, sCtx.request.sourceCode);
    modifiers.isHandle = typeInfo.isHandle || modifiers.isHandle;

    SymbolLocationContext loc{sCtx, ctx.containerPath, ctx.containerKind};
    Symbol sym = CreateSymbol(SymbolType::Property, varDeclNode, nameNode, loc);

    VariableSignature varSig;
    varSig.typeName = typeStr;
    varSig.baseTypeName = typeInfo.baseTypeName;
    varSig.templateName = typeInfo.templateName;
    varSig.typeRange = ToSourceRange(typeNode);
    varSig.typeKind = typeInfo.kind;
    varSig.isArray = typeInfo.isArray;
    varSig.hasPrimitiveHandle = typeInfo.hasPrimitiveHandle;
    varSig.arrayDepth = typeInfo.arrayDepth;
    varSig.defaultValue = std::string(parser::NodeText(varDeclNode, sCtx.request.sourceCode));
    varSig.modifiers = modifiers;
    varSig.isVirtualProperty = true;

    ParseAccessorNodes(varDeclNode, varSig);

    sym.signature = varSig;
    sCtx.collectedSymbols.push_back(std::move(sym));
}

void SymbolCollector::CollectDeclaratorSymbol(TSNode declaratorNode, const VariableHeaderInfo& header,
                                              SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode varDeclNode = header.varDeclNode;
    TSNode nameNode = GetChildByFieldName(declaratorNode, "name");
    TSNode valueNode = parser::FindDeclaratorValueNode(declaratorNode, sCtx.request.sourceCode);

    SymbolLocationContext loc{sCtx, ctx.containerPath, ctx.containerKind};
    Symbol sym = CreateSymbol(SymbolType::Variable, varDeclNode, nameNode, loc);

    VariableSignature varSig;
    varSig.typeName = header.typeStr;
    varSig.baseTypeName = header.typeInfo.baseTypeName;
    varSig.templateName = header.typeInfo.templateName;
    varSig.typeRange = header.typeRange;
    varSig.templateArgumentTypes = header.typeInfo.templateArgumentTypes;
    varSig.typeKind = header.typeInfo.kind;
    varSig.isArray = header.typeInfo.isArray;
    varSig.hasPrimitiveHandle = header.typeInfo.hasPrimitiveHandle;
    varSig.arrayDepth = header.typeInfo.arrayDepth;
    varSig.defaultValue = std::string(parser::NodeText(valueNode, sCtx.request.sourceCode));
    varSig.hasNullInitializer = IsNullInitializer(valueNode);
    varSig.hasSemicolon = header.hasSemicolon;
    varSig.modifiers = header.modifiers;
    if (header.typeInfo.isConst)
    {
        varSig.modifiers.isConst = true;
    }

    TSNode argsNode = GetChildByFieldName(declaratorNode, "arguments");
    bool hasCallInit = !ts_node_is_null(argsNode);
    if (!hasCallInit && !ts_node_is_null(valueNode))
    {
        TSSymbol vSym = ts_node_symbol(valueNode);
        if (vSym == m_gs.symCallExpression || vSym == m_gs.symConstructCallExpression)
        {
            hasCallInit = true;
        }
    }
    varSig.hasCallInitializer = hasCallInit;

    sym.signature = varSig;
    sCtx.collectedSymbols.push_back(std::move(sym));

    if (!ts_node_is_null(argsNode) && !header.typeStr.empty())
    {
        CallReferenceSignature callSig;
        callSig.calleeName = header.typeInfo.baseTypeName.empty() ? header.typeStr : header.typeInfo.baseTypeName;
        SymbolLocationContext callLoc{sCtx, ctx.containerPath, ctx.containerKind};
        TSNode varTypeNode = GetChildByFieldName(varDeclNode, "var_type");
        TSNode refNode = ts_node_is_null(varTypeNode) ? declaratorNode : varTypeNode;
        Symbol callSym = CreateSymbol(SymbolType::CallReference, declaratorNode, refNode, callLoc);
        callSym.name = callSig.calleeName;
        callSym.signature = callSig;
        sCtx.collectedSymbols.push_back(std::move(callSym));
    }
}

void SymbolCollector::ProcessRegularVariable(TSNode varDeclNode, SymbolCollectContext& sCtx,
                                             const CollectionContext& ctx)
{
    TSNode typeNode = GetChildByFieldName(varDeclNode, "var_type");
    std::string typeStr(parser::NodeText(typeNode, sCtx.request.sourceCode));
    TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, sCtx.request.sourceCode, &sCtx.index);
    if (IsReservedKeyword(typeInfo.baseTypeName) && !IsPrimitiveTypeName(typeInfo.baseTypeName) &&
        typeInfo.baseTypeName != "auto")
    {
        return;
    }
    SymbolModifiers modifiers = ExtractModifiers(varDeclNode, sCtx.request.sourceCode);
    modifiers.isHandle = typeInfo.isHandle || modifiers.isHandle;
    modifiers.isReturnReference = typeInfo.isReference || modifiers.isReturnReference;

    std::string_view rawVarText = TrimView(parser::NodeText(varDeclNode, sCtx.request.sourceCode));
    bool hasSemicolon = (!rawVarText.empty() && rawVarText.back() == ';');

    VariableHeaderInfo header{varDeclNode, std::move(typeStr), std::move(typeInfo), ToSourceRange(typeNode),
                              modifiers,   hasSemicolon};

    TSTreeCursor cursor = ts_tree_cursor_new(varDeclNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode declaratorNode = ts_tree_cursor_current_node(&cursor);
            if (ts_node_is_named(declaratorNode) && ts_node_symbol(declaratorNode) == m_gs.symVariableDeclarator)
            {
                CollectDeclaratorSymbol(declaratorNode, header, sCtx, ctx);
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

void SymbolCollector::ProcessVariable(TSNode varDeclNode, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    if (ctx.isInsideFunction)
        return;

    if (ts_node_symbol(varDeclNode) == m_gs.symVirtualProperty)
    {
        ProcessVirtualPropertyVariable(varDeclNode, sCtx, ctx);
    }
    else
    {
        ProcessRegularVariable(varDeclNode, sCtx, ctx);
    }
}

/**
 * @brief Finds the statement block of a function, falling back to a child scan when the body field is absent.
 * @param[in] funcNode Function declaration node.
 * @return Body node, or a null node for declarations without a body.
 */
TSNode SymbolCollector::FindFunctionBody(TSNode funcNode) const
{
    TSNode bodyNode = GetChildByFieldId(funcNode, m_gs.fieldBody);
    if (!ts_node_is_null(bodyNode))
        return bodyNode;

    TSTreeCursor cursor = ts_tree_cursor_new(funcNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode ch = ts_tree_cursor_current_node(&cursor);
            if (ts_node_symbol(ch) == m_gs.symStatementBlock)
            {
                ts_tree_cursor_delete(&cursor);
                return ch;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return bodyNode;
}

bool SymbolCollector::IsExternalFunction(TSNode funcNode, [[maybe_unused]] std::string_view sourceCode) const
{
    TSSymbol funcNodeSym = ts_node_symbol(funcNode);
    return (funcNodeSym == m_gs.symImportDeclaration || funcNodeSym == m_gs.tokImport);
}

std::string SymbolCollector::ExtractOriginModule(TSNode funcNode, std::string_view sourceCode) const
{
    TSNode srcNode = GetChildByFieldId(funcNode, m_gs.fieldSource);
    if (!ts_node_is_null(srcNode))
    {
        std::string_view modStr = parser::NodeText(srcNode, sourceCode);
        return std::string(parser::UnquoteStringView(modStr));
    }
    return "";
}

void SymbolCollector::ProcessEmptyParenVariable(TSNode funcNode, SymbolCollectContext& sCtx,
                                                const CollectionContext& ctx)
{
    TSNode nameNode = GetChildByFieldId(funcNode, m_gs.fieldName);
    TSNode typeNode = GetChildByFieldId(funcNode, m_gs.fieldReturnType);
    TypeExtractionResult retInfo = ExtractTypeInfoFromAST(typeNode, sCtx.request.sourceCode, &sCtx.index);
    SymbolModifiers modifiers = ExtractModifiers(funcNode, sCtx.request.sourceCode);

    SymbolLocationContext loc{sCtx, ctx.containerPath, ctx.containerKind};
    Symbol sym = CreateSymbol(SymbolType::Variable, funcNode, nameNode, loc);

    VariableSignature varSig;
    varSig.typeName = std::string(parser::NodeText(typeNode, sCtx.request.sourceCode));
    varSig.baseTypeName = retInfo.baseTypeName;
    varSig.templateName = retInfo.templateName;
    varSig.typeRange = ToSourceRange(typeNode);
    varSig.templateArgumentTypes = retInfo.templateArgumentTypes;
    varSig.typeKind = retInfo.kind;
    varSig.isArray = retInfo.isArray;
    varSig.hasPrimitiveHandle = retInfo.hasPrimitiveHandle;
    varSig.arrayDepth = retInfo.arrayDepth;
    varSig.modifiers = modifiers;
    if (retInfo.isConst)
    {
        varSig.modifiers.isConst = true;
    }
    varSig.hasCallInitializer = true;
    varSig.hasSemicolon = true;

    sym.signature = std::move(varSig);
    sCtx.collectedSymbols.push_back(std::move(sym));

    if (!ts_node_is_null(typeNode))
    {
        CallReferenceSignature callSig;
        callSig.calleeName = std::string(parser::NodeText(typeNode, sCtx.request.sourceCode));
        Symbol callSym = CreateSymbol(SymbolType::CallReference, typeNode, typeNode, loc);
        callSym.signature = std::move(callSig);
        sCtx.collectedSymbols.push_back(std::move(callSym));
    }
}

static bool HasDisqualifyingModifiers(const SymbolModifiers& modifiers)
{
    return modifiers.isExternal || modifiers.isAbstract || modifiers.isOverride || modifiers.isFinal ||
           modifiers.isDelete || modifiers.isConst || modifiers.isReturnReference || modifiers.isHandle;
}

static bool IsDisqualifyingReturnType(const TypeExtractionResult& retInfo, std::string_view typeText)
{
    return retInfo.baseTypeName == "void" || typeText == "void" || retInfo.isReference || retInfo.isHandle;
}

bool SymbolCollector::TryProcessEmptyParenVariable(TSNode funcNode, const SymbolModifiers& modifiers,
                                                   SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    if (ctx.containerKind == ContainerKind::Interface || ctx.containerKind == ContainerKind::Enum || sCtx.isPredefined)
    {
        return false;
    }
    TSNode bodyNode = FindFunctionBody(funcNode);
    if (!ts_node_is_null(bodyNode))
    {
        return false;
    }
    const TSSymbol sym = ts_node_symbol(funcNode);
    if (sym == m_gs.symInterfaceMethod || sym == m_gs.symImportDeclaration || HasDisqualifyingModifiers(modifiers))
    {
        return false;
    }
    TSNode paramsNode = GetChildByFieldId(funcNode, m_gs.fieldParameters);
    if (!ts_node_is_null(paramsNode) && ts_node_named_child_count(paramsNode) > 0)
    {
        return false;
    }
    TSNode nameNode = GetChildByFieldId(funcNode, m_gs.fieldName);
    const std::string nameText(parser::NodeText(nameNode, sCtx.request.sourceCode));
    if (nameText == ctx.containerPath || nameText.starts_with('~'))
    {
        return false;
    }
    TSNode typeNode = GetChildByFieldId(funcNode, m_gs.fieldReturnType);
    TypeExtractionResult retInfo = ExtractTypeInfoFromAST(typeNode, sCtx.request.sourceCode, &sCtx.index);
    if (IsDisqualifyingReturnType(retInfo, std::string(parser::NodeText(typeNode, sCtx.request.sourceCode))))
    {
        return false;
    }
    ProcessEmptyParenVariable(funcNode, sCtx, ctx);
    return true;
}

/**
 * @brief Records a function or method symbol with its signature, modifiers and body range.
 * @param[in] funcNode Function declaration node.
 * @param[in,out] sCtx Collection state receiving the symbol.
 * @param[in] ctx Enclosing container and namespace.
 */
void SymbolCollector::ProcessFunction(TSNode funcNode, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode nameNode = GetChildByFieldId(funcNode, m_gs.fieldName);
    TSNode typeNode = GetChildByFieldId(funcNode, m_gs.fieldReturnType);
    TSNode paramsNode = GetChildByFieldId(funcNode, m_gs.fieldParameters);
    TSNode bodyNode = sCtx.isPredefined ? TSNode{} : FindFunctionBody(funcNode);

    TypeExtractionResult retInfo = ExtractTypeInfoFromAST(typeNode, sCtx.request.sourceCode, &sCtx.index);
    SymbolModifiers modifiers = ExtractModifiers(funcNode, sCtx.request.sourceCode);

    if (IsExternalFunction(funcNode, sCtx.request.sourceCode))
    {
        modifiers.isExternal = true;
    }

    if (TryProcessEmptyParenVariable(funcNode, modifiers, sCtx, ctx))
    {
        return;
    }

    modifiers.isHandle = retInfo.isHandle || modifiers.isHandle;
    modifiers.isReturnReference = retInfo.isReference || modifiers.isReturnReference;

    SymbolLocationContext loc{sCtx, ctx.containerPath, ctx.containerKind};
    Symbol sym = CreateSymbol(SymbolType::Function, funcNode, nameNode, loc);

    FunctionSignature funcSig;
    funcSig.returnType = std::string(parser::NodeText(typeNode, sCtx.request.sourceCode));
    funcSig.returnBaseTypeName = retInfo.baseTypeName;
    funcSig.returnTemplateName = retInfo.templateName;
    funcSig.returnTypeKind = retInfo.kind;
    funcSig.returnIsArray = retInfo.isArray;
    funcSig.returnIsConst = retInfo.isConst;
    funcSig.returnHasPrimitiveHandle = retInfo.hasPrimitiveHandle;
    funcSig.returnArrayDepth = retInfo.arrayDepth;
    funcSig.modifiers = modifiers;
    funcSig.parameters = ExtractParameters(paramsNode, sCtx);

    funcSig.hasBody = !ts_node_is_null(bodyNode);
    funcSig.isInterfaceMethod = (ts_node_symbol(funcNode) == m_gs.symInterfaceMethod);
    funcSig.isImported = (ts_node_symbol(funcNode) == m_gs.symImportDeclaration);
    if (funcSig.isImported)
    {
        funcSig.originModule = ExtractOriginModule(funcNode, sCtx.request.sourceCode);
    }

    sym.signature = std::move(funcSig);
    sCtx.collectedSymbols.push_back(std::move(sym));
}

/**
 * @brief Checks if a class body declares any explicit constructor.
 * @param[in] bodyNode Class body AST node.
 * @param[in] className Name of the class being inspected.
 * @param[in] sourceCode Document source string.
 * @param[in] symFuncDeclaration Pre-resolved symbol for func_declaration.
 * @return True if at least one constructor declaration is present.
 */
static bool ClassHasExplicitConstructor(TSNode bodyNode, std::string_view className, std::string_view sourceCode,
                                        TSSymbol symFuncDeclaration)
{
    if (ts_node_is_null(bodyNode) || className.empty())
    {
        return false;
    }

    bool found = false;
    TSTreeCursor cursor = ts_tree_cursor_new(bodyNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            if (ts_node_symbol(child) == symFuncDeclaration)
            {
                TSNode nameNode = parser::GetChildByField(child, parser::GetGrammarSymbols().fieldName);
                if (parser::NodeText(nameNode, sourceCode) == className)
                {
                    found = true;
                    break;
                }
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return found;
}

/**
 * @brief Synthesizes an implicit default constructor when a non-abstract class declares none.
 * @param[in] sym Class symbol.
 * @param[in,out] sCtx Symbol collection context receiving the synthesized constructor.
 */
void SymbolCollector::SynthesizeDefaultConstructor(const Symbol& sym, SymbolCollectContext& sCtx) const
{
    Symbol ctorSym;
    ctorSym.type = SymbolType::Function;
    ctorSym.name = sym.name;
    ctorSym.containerName = sym.qualifiedName;
    ctorSym.containerKind = ContainerKind::Class;
    SymbolCollector::appendQualifiedName(ctorSym.qualifiedName, sym.qualifiedName, sym.name);
    ctorSym.fileUri = sym.fileUri;
    ctorSym.isPredefined = sym.isPredefined;
    ctorSym.fullRange = sym.fullRange;
    ctorSym.selectionRange = sym.selectionRange;
    ctorSym.startLine = sym.startLine;
    ctorSym.startCharacter = sym.startCharacter;
    ctorSym.endLine = sym.endLine;
    ctorSym.endCharacter = sym.endCharacter;
    ctorSym.isSynthesized = true;

    FunctionSignature ctorSig;
    ctorSig.returnType = sym.name;
    ctorSig.returnBaseTypeName = sym.name;
    ctorSig.returnTypeKind = TypeKind::Object;
    ctorSig.hasBody = true;
    ctorSym.signature = std::move(ctorSig);
    ctorSym.GetFunction().displaySignature = FormatFunctionDeclaration(ctorSym, true);
    sCtx.collectedSymbols.push_back(std::move(ctorSym));
}

void SymbolCollector::ProcessClass(TSNode classNode, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode nameNode = GetChildByFieldName(classNode, "name");
    SymbolLocationContext loc{sCtx, ctx.containerPath, ctx.containerKind};
    Symbol sym = CreateSymbol(SymbolType::Class, classNode, nameNode, loc);

    ClassSignature classSig;
    classSig.modifiers = ExtractModifiers(classNode, sCtx.request.sourceCode);
    if (ts_node_symbol(classNode) == m_gs.symMixinDeclaration)
    {
        classSig.modifiers.isMixin = true;
    }
    classSig.bases = ExtractBases(classNode, sCtx.request.sourceCode);

    TSNode templateParams = GetChildByFieldName(classNode, "template_params");
    if (!ts_node_is_null(templateParams))
    {
        TSTreeCursor cursor = ts_tree_cursor_new(templateParams);
        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            do
            {
                TSNode child = ts_tree_cursor_current_node(&cursor);
                if (ts_node_is_named(child))
                {
                    std::string paramName(parser::NodeText(child, sCtx.request.sourceCode));
                    if (!paramName.empty())
                    {
                        classSig.isTemplate = true;
                        classSig.templateParams.push_back(std::move(paramName));
                    }
                }
            } while (ts_tree_cursor_goto_next_sibling(&cursor));
        }
        ts_tree_cursor_delete(&cursor);
    }

    size_t angleInName = sym.name.find('<');
    if (angleInName != std::string::npos)
    {
        sym.name = sym.name.substr(0, angleInName);
        sym.qualifiedName = utils::JoinScopedName(ctx.containerPath, sym.name);
    }

    TSNode bodyNode = GetChildByFieldName(classNode, "body");
    classSig.hasBraces = !ts_node_is_null(bodyNode);
    classSig.hasListPattern =
        HasPrecedingDocTagAtByte(sCtx.request.sourceCode, ts_node_start_byte(classNode), "@listpattern");

    sym.signature = classSig;
    const bool needsDefaultCtor =
        !sCtx.isPredefined && classSig.hasBraces && !classSig.modifiers.isAbstract && !classSig.modifiers.isMixin &&
        !ClassHasExplicitConstructor(bodyNode, sym.name, sCtx.request.sourceCode, m_gs.symFuncDeclaration);

    sCtx.collectedSymbols.push_back(std::move(sym));
    if (needsDefaultCtor)
    {
        SynthesizeDefaultConstructor(sCtx.collectedSymbols.back(), sCtx);
    }
}

void SymbolCollector::ProcessNamespace(TSNode namespaceNode, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode nameNode = GetChildByFieldName(namespaceNode, "name");
    if (ts_node_is_null(nameNode))
    {
        return;
    }

    if (ts_node_symbol(nameNode) == m_gs.symScopedIdentifier)
    {
        std::string currentPath = ctx.containerPath;
        const uint32_t count = ts_node_named_child_count(nameNode);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode segNode = ts_node_named_child(nameNode, i);
            if (ts_node_symbol(segNode) != m_gs.symIdentifier)
            {
                continue;
            }
            SymbolLocationContext loc{sCtx, currentPath, ContainerKind::Namespace};
            Symbol subSym = CreateSymbol(SymbolType::Namespace, namespaceNode, segNode, loc);
            sCtx.collectedSymbols.push_back(subSym);
            currentPath = subSym.qualifiedName;
        }
    }
    else
    {
        SymbolLocationContext loc{sCtx, ctx.containerPath, ctx.containerKind};
        Symbol sym = CreateSymbol(SymbolType::Namespace, namespaceNode, nameNode, loc);
        sCtx.collectedSymbols.push_back(std::move(sym));
    }
}

void SymbolCollector::ProcessTypedef(TSNode node, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode nameNode = GetChildByFieldName(node, "name");
    TSNode baseTypeNode = GetChildByFieldName(node, "base_type");
    if (ts_node_is_null(baseTypeNode))
    {
        baseTypeNode = GetChildByFieldName(node, "type");
    }

    SymbolLocationContext loc{sCtx, ctx.containerPath, ctx.containerKind};
    Symbol sym = CreateSymbol(SymbolType::Typedef, node, nameNode, loc);
    TypedefSignature typedefSig;

    if (!ts_node_is_null(baseTypeNode))
    {
        TypeExtractionResult info = ExtractTypeInfoFromAST(baseTypeNode, sCtx.request.sourceCode, &sCtx.index);
        typedefSig.baseType = info.baseTypeName;
        typedefSig.typeKind = info.kind;

        TSPoint startPoint = ts_node_start_point(baseTypeNode);
        TSPoint endPoint = ts_node_end_point(baseTypeNode);
        typedefSig.baseTypeStartLine = startPoint.row;
        typedefSig.baseTypeStartCharacter = startPoint.column;
        typedefSig.baseTypeEndLine = endPoint.row;
        typedefSig.baseTypeEndCharacter = endPoint.column;
    }

    std::string_view rawNodeText = TrimView(parser::NodeText(node, sCtx.request.sourceCode));
    typedefSig.hasSemicolon = (!rawNodeText.empty() && rawNodeText.back() == ';');

    sym.signature = typedefSig;
    sCtx.collectedSymbols.push_back(std::move(sym));
}

/**
 * @brief Records a funcdef type symbol with its return type and parameters.
 * @param[in] node Funcdef declaration node.
 * @param[in,out] sCtx Collection state receiving the symbol.
 * @param[in] ctx Enclosing container and namespace.
 */
void SymbolCollector::ProcessFuncdef(TSNode node, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode nameNode = GetChildByFieldName(node, "name");
    TSNode typeNode = GetChildByFieldName(node, "return_type");
    TSNode paramsNode = GetChildByFieldName(node, "parameters");

    TypeExtractionResult retInfo = ExtractTypeInfoFromAST(typeNode, sCtx.request.sourceCode, &sCtx.index);
    SymbolModifiers modifiers = ExtractModifiers(node, sCtx.request.sourceCode);
    modifiers.isHandle = retInfo.isHandle || modifiers.isHandle;
    modifiers.isReturnReference = retInfo.isReference || modifiers.isReturnReference;
    modifiers.isConst = retInfo.isConst || modifiers.isConst;

    SymbolLocationContext loc{sCtx, ctx.containerPath, ctx.containerKind};
    Symbol sym = CreateSymbol(SymbolType::Funcdef, node, nameNode, loc);

    FuncdefSignature funcdefSig;
    funcdefSig.returnType = std::string(parser::NodeText(typeNode, sCtx.request.sourceCode));
    funcdefSig.returnBaseTypeName = retInfo.baseTypeName;
    funcdefSig.returnTemplateName = retInfo.templateName;
    funcdefSig.returnTypeKind = retInfo.kind;
    funcdefSig.returnIsArray = retInfo.isArray;
    funcdefSig.returnArrayDepth = retInfo.arrayDepth;
    funcdefSig.returnHasPrimitiveHandle = retInfo.hasPrimitiveHandle;
    funcdefSig.modifiers = modifiers;
    funcdefSig.parameters = ExtractParameters(paramsNode, sCtx);
    if (!ts_node_is_null(typeNode))
    {
        TSPoint retStart = ts_node_start_point(typeNode);
        TSPoint retEnd = ts_node_end_point(typeNode);
        funcdefSig.returnTypeStartLine = retStart.row;
        funcdefSig.returnTypeStartCharacter = retStart.column;
        funcdefSig.returnTypeEndLine = retEnd.row;
        funcdefSig.returnTypeEndCharacter = retEnd.column;
    }

    sym.signature = funcdefSig;
    sCtx.collectedSymbols.push_back(std::move(sym));
}

bool SymbolCollector::EnumHasBraces(TSNode node) const
{
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            if (ts_node_symbol(ts_tree_cursor_current_node(&cursor)) == m_gs.tokOpenBrace)
            {
                ts_tree_cursor_delete(&cursor);
                return true;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return false;
}

void SymbolCollector::CollectEnumMembers(TSNode node, std::string_view sourceCode, EnumSignature& enumSig) const
{
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            if (ts_node_is_named(child) && ts_node_symbol(child) == m_gs.symEnumMember)
            {
                TSNode memberName = parser::GetChildByField(child, m_gs.fieldName);
                TSNode memberValue = parser::GetChildByField(child, m_gs.fieldValue);

                EnumMemberInformation member;
                member.name = std::string(parser::NodeText(memberName, sourceCode));
                member.value = std::string(parser::NodeText(memberValue, sourceCode));
                if (!ts_node_is_null(memberValue))
                {
                    member.valueNodeType = std::string(parser::GetNodeType(memberValue));
                }
                enumSig.members.push_back(std::move(member));
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

static ankerl::unordered_dense::map<std::string, MemberNodes>
BuildEnumMemberNodeMap(TSNode node, TSSymbol symEnumMember, TSFieldId fieldName, std::string_view sourceCode)
{
    ankerl::unordered_dense::map<std::string, MemberNodes> map;
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            if (ts_node_is_named(child) && ts_node_symbol(child) == symEnumMember)
            {
                TSNode mNameNode = ts_node_child_by_field_id(child, fieldName);
                std::string mName(parser::NodeText(mNameNode, sourceCode));
                if (!mName.empty())
                {
                    map[mName] = MemberNodes{child, mNameNode};
                }
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return map;
}

static std::string ComputeEnumDefaultValue(std::string_view explicitVal, int64_t& nextVal)
{
    if (!explicitVal.empty())
    {
        std::string_view trimmed = explicitVal;
        while (!trimmed.empty() && isspace(static_cast<unsigned char>(trimmed.front())))
        {
            trimmed.remove_prefix(1);
        }
        while (!trimmed.empty() && isspace(static_cast<unsigned char>(trimmed.back())))
        {
            trimmed.remove_suffix(1);
        }

        int64_t parsed = 0;
        int base = 10;
        bool isNegative = false;
        if (trimmed.starts_with("-"))
        {
            isNegative = true;
            trimmed.remove_prefix(1);
        }
        else if (trimmed.starts_with("+"))
        {
            trimmed.remove_prefix(1);
        }

        if (trimmed.starts_with("0x") || trimmed.starts_with("0X"))
        {
            trimmed.remove_prefix(2);
            base = 16;
        }

        auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed, base);
        if (ec == std::errc{})
        {
            if (isNegative)
            {
                parsed = -parsed;
            }
            nextVal = parsed + 1;
        }
        else
        {
            nextVal++;
        }
        return std::string(explicitVal);
    }
    return std::to_string(nextVal++);
}

void SymbolCollector::PublishEnumMembers(TSNode node, const EnumSignature& enumSig, SymbolCollectContext& sCtx,
                                         const CollectionContext& ctx)
{
    TSNode nameNode = parser::GetChildByField(node, m_gs.fieldName);
    std::string enumName(parser::NodeText(nameNode, sCtx.request.sourceCode));
    const auto memberNodeMap =
        BuildEnumMemberNodeMap(node, m_gs.symEnumMember, m_gs.fieldName, sCtx.request.sourceCode);

    std::string enumContainer;
    appendQualifiedName(enumContainer, ctx.containerPath, enumName);
    int64_t nextImplicitValue = 0;
    for (const auto& m : enumSig.members)
    {
        if (m.name.empty())
            continue;

        VariableSignature varSig;
        varSig.typeName = enumContainer;
        varSig.defaultValue = ComputeEnumDefaultValue(m.value, nextImplicitValue);
        varSig.modifiers.isConst = true;
        varSig.isEnumConstant = true;

        TSNode memberDeclNode = node;
        TSNode memberNameNode = nameNode;
        auto it = memberNodeMap.find(m.name);
        if (it != memberNodeMap.end())
        {
            memberDeclNode = it->second.declNode;
            memberNameNode = it->second.nameNode;
        }

        SymbolLocationContext contLoc{sCtx, ctx.containerPath, ContainerKind::Enum};
        Symbol mSym = CreateSymbol(SymbolType::Variable, memberDeclNode, memberNameNode, contLoc);
        mSym.name = m.name;
        appendQualifiedName(mSym.qualifiedName, ctx.containerPath, m.name);
        mSym.containerName = ctx.containerPath;
        mSym.containerKind = ContainerKind::Enum;
        mSym.signature = varSig;
        sCtx.collectedSymbols.push_back(std::move(mSym));

        SymbolLocationContext enumLoc{sCtx, enumContainer, ContainerKind::Enum};
        Symbol mSymScoped = CreateSymbol(SymbolType::Variable, memberDeclNode, memberNameNode, enumLoc);
        mSymScoped.name = m.name;
        appendQualifiedName(mSymScoped.qualifiedName, enumContainer, m.name);
        mSymScoped.containerName = enumContainer;
        mSymScoped.containerKind = ContainerKind::Enum;
        mSymScoped.signature = varSig;
        sCtx.collectedSymbols.push_back(std::move(mSymScoped));
    }
}

Symbol SymbolCollector::CreateNamedContainerSymbol(SymbolType type, TSNode node, SymbolCollectContext& sCtx,
                                                   const CollectionContext& ctx) const
{
    TSNode nameNode = GetChildByFieldName(node, "name");
    SymbolLocationContext loc{sCtx, ctx.containerPath, ctx.containerKind};
    return CreateSymbol(type, node, nameNode, loc);
}

/**
 * @brief Collects enum symbol declarations and publishes individual enumerator members.
 * @param[in] node AST node representing the enum declaration.
 * @param[in,out] sCtx Symbol collection state accumulating discovered declarations.
 * @param[in] ctx Lexical container context specifying enclosing paths and scopes.
 */
void SymbolCollector::ProcessEnum(TSNode node, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    Symbol sym = CreateNamedContainerSymbol(SymbolType::Enum, node, sCtx, ctx);

    EnumSignature enumSig;
    enumSig.modifiers = ExtractModifiers(node, sCtx.request.sourceCode);
    enumSig.hasBraces = EnumHasBraces(node);

    CollectEnumMembers(node, sCtx.request.sourceCode, enumSig);

    sym.signature = enumSig;
    sCtx.collectedSymbols.push_back(std::move(sym));

    PublishEnumMembers(node, enumSig, sCtx, ctx);
}

/**
 * @brief Collects virtual property declarations and extracts type, modifier, and accessor information.
 * @param[in] node AST node representing the property declaration.
 * @param[in,out] sCtx Symbol collection state accumulating discovered declarations.
 * @param[in] ctx Lexical container context specifying enclosing paths and scopes.
 */
void SymbolCollector::ProcessProperty(TSNode node, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode typeNode = GetChildByFieldName(node, "prop_type");
    TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, sCtx.request.sourceCode, &sCtx.index);
    Symbol sym = CreateNamedContainerSymbol(SymbolType::Property, node, sCtx, ctx);

    VariableSignature varSig;
    varSig.typeName = std::string(parser::NodeText(typeNode, sCtx.request.sourceCode));
    varSig.baseTypeName = typeInfo.baseTypeName;
    varSig.templateName = typeInfo.templateName;
    varSig.typeRange = ToSourceRange(typeNode);
    varSig.typeKind = typeInfo.kind;
    varSig.isArray = typeInfo.isArray;
    varSig.hasPrimitiveHandle = typeInfo.hasPrimitiveHandle;
    varSig.arrayDepth = typeInfo.arrayDepth;
    varSig.modifiers = ExtractModifiers(node, sCtx.request.sourceCode);
    varSig.defaultValue = std::string(parser::NodeText(node, sCtx.request.sourceCode));
    varSig.isVirtualProperty = true;

    ParseAccessorNodes(node, varSig);

    sym.signature = varSig;
    sCtx.collectedSymbols.push_back(std::move(sym));
}

/**
 * @brief Collects interface type definitions and extracts inheritance declarations.
 * @param[in] node AST node representing the interface declaration.
 * @param[in,out] sCtx Symbol collection state accumulating discovered declarations.
 * @param[in] ctx Lexical container context specifying enclosing paths and scopes.
 */
void SymbolCollector::ProcessInterface(TSNode node, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    Symbol sym = CreateNamedContainerSymbol(SymbolType::Interface, node, sCtx, ctx);

    InterfaceSignature ifaceSig;
    ifaceSig.modifiers = ExtractModifiers(node, sCtx.request.sourceCode);
    ifaceSig.inheritedInterfaces = ExtractBases(node, sCtx.request.sourceCode);

    sym.signature = ifaceSig;
    sCtx.collectedSymbols.push_back(std::move(sym));
}

// Reference & Out-of-Body Call Collectors

void SymbolCollector::ProcessCallReference(TSNode callNode, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    if (ctx.isInsideFunction)
        return;

    TSNode functionNode = GetChildByFieldName(callNode, "function");
    if (ts_node_is_null(functionNode))
        return;

    TSSymbol functionNodeSym = ts_node_symbol(functionNode);

    CallReferenceSignature callSig;
    TSNode nameNode = functionNode;

    if (functionNodeSym == m_gs.symMemberExpression)
    {
        TSNode memberNode = GetChildByFieldName(functionNode, "member");
        if (ts_node_is_null(memberNode) || ts_node_symbol(memberNode) != m_gs.symIdentifier)
        {
            return;
        }

        callSig.isMethodCall = true;
        callSig.objectExpression =
            std::string(parser::NodeText(GetChildByFieldName(functionNode, "object"), sCtx.request.sourceCode));
        nameNode = memberNode;
    }
    else if (functionNodeSym != m_gs.symScopedIdentifier)
    {
        return;
    }

    callSig.calleeName = std::string(parser::NodeText(nameNode, sCtx.request.sourceCode));
    if (callSig.calleeName.empty())
        return;

    SymbolLocationContext loc{sCtx, ctx.containerPath, ctx.containerKind};
    Symbol sym = CreateSymbol(SymbolType::CallReference, callNode, nameNode, loc);
    sym.signature = callSig;
    sCtx.collectedSymbols.push_back(std::move(sym));
}

// Validation Collectors

/**
 * @brief Symbol collector check using declaration capture.
 * @param[in] usingNode AST node under evaluation.
 * @param[in,out] sCtx Analysis or feature execution context.
 */
void SymbolCollector::CheckUsingDeclarationCapture(TSNode usingNode, SymbolCollectContext& sCtx) const
{
    TSNode nameNode = GetChildByFieldName(usingNode, "name");
    std::string nameText(parser::NodeText(nameNode, sCtx.request.sourceCode));
    if (nameText.empty() || !IsReservedKeyword(nameText))
        return;

    TSPoint startPt = ts_node_start_point(nameNode);
    TSPoint endPt = ts_node_end_point(nameNode);

    Diagnostic diag;
    diag.range.start.line = startPt.row;
    diag.range.start.character = startPt.column;
    diag.range.end.line = endPt.row;
    diag.range.end.character = endPt.column;
    diag.severity = DiagnosticSeverity::Error;
    diag.code = "as-err-reserved-keyword-name";
    diag.source = "AngelScript";
    diag.fileUri = sCtx.request.fileUri;
    const std::string_view pattern = sCtx.request.i18n
                                         ? sCtx.request.i18n->GetMessageView("as-err-reserved-keyword-name")
                                         : std::string_view("Instead found reserved keyword '{}'.");
    diag.message = fmt::format(fmt::runtime(pattern), nameText);
    sCtx.diagnostics.push_back(std::move(diag));
}

/**
 * @brief Symbol collector check duplicate modifier group.
 * @param[in] declNode AST node under evaluation.
 * @param[in,out] sCtx Analysis or feature execution context.
 */
void SymbolCollector::CheckDuplicateModifierGroup(TSNode declNode, SymbolCollectContext& sCtx) const
{
    ankerl::unordered_dense::set<std::string> seenModifiers;
    TSTreeCursor cursor = ts_tree_cursor_new(declNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            TSSymbol childSym = ts_node_symbol(child);
            if (childSym != m_gs.symDeclarationModifier && childSym != m_gs.symSharedExternalModifier)
                continue;

            TSTreeCursor modCursor = ts_tree_cursor_new(child);
            if (ts_tree_cursor_goto_first_child(&modCursor))
            {
                do
                {
                    TSNode modTokNode = ts_tree_cursor_current_node(&modCursor);
                    std::string modText(std::string(parser::NodeText(modTokNode, sCtx.request.sourceCode)));
                    if (seenModifiers.contains(modText))
                    {
                        TSPoint startPt = ts_node_start_point(modTokNode);
                        TSPoint endPt = ts_node_end_point(modTokNode);
                        Diagnostic diag;
                        diag.range.start.line = startPt.row;
                        diag.range.start.character = startPt.column;
                        diag.range.end.line = endPt.row;
                        diag.range.end.character = endPt.column;
                        diag.severity = DiagnosticSeverity::Warning;
                        diag.code = "as-err-attribute-repeated";
                        diag.source = "AngelScript";
                        diag.fileUri = sCtx.request.fileUri;
                        const std::string_view pattern =
                            sCtx.request.i18n ? sCtx.request.i18n->GetMessageView("as-err-attribute-repeated")
                                              : std::string_view("Attribute '{}' is informed multiple times.");
                        diag.message = fmt::format(fmt::runtime(pattern), modText);
                        sCtx.diagnostics.push_back(std::move(diag));
                    }
                    else
                    {
                        seenModifiers.insert(modText);
                    }
                } while (ts_tree_cursor_goto_next_sibling(&modCursor));
            }
            ts_tree_cursor_delete(&modCursor);
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

// Diagnostics & Error Recovery

/**
 * @brief Checks whether an AST node contains nested ERROR or MISSING child nodes.
 * @param[in] node Node to inspect.
 * @return True if any child is missing, error, or has error.
 */
static bool HasNestedErrorOrMissing(TSNode node)
{
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (!ts_tree_cursor_goto_first_child(&cursor))
    {
        ts_tree_cursor_delete(&cursor);
        return false;
    }
    bool found = false;
    do
    {
        TSNode child = ts_tree_cursor_current_node(&cursor);
        if (ts_node_is_missing(child) || ts_node_is_error(child) || ts_node_has_error(child))
        {
            found = true;
            break;
        }
    } while (ts_tree_cursor_goto_next_sibling(&cursor));
    ts_tree_cursor_delete(&cursor);
    return found;
}

/**
 * @brief Is declaration symbol.
 * @param[in] sym Tree-sitter grammar symbol.
 * @param[in] gs Precompiled grammar symbols.
 * @return True if the condition is satisfied; false otherwise.
 */
static bool IsDeclarationSymbol(TSSymbol sym, const parser::GrammarSymbols& gs)
{
    return sym == gs.symFuncDeclaration || sym == gs.symVariableDeclaration || sym == gs.symClassDeclaration ||
           sym == gs.symInterfaceDeclaration || sym == gs.symEnumDeclaration || sym == gs.symNamespaceDeclaration;
}

static TSNode ResolveOpeningBraceTarget(TSNode classOpeningBrace, const parser::GrammarSymbols& gs)
{
    TSNode next = ts_node_next_sibling(classOpeningBrace);
    while (!ts_node_is_null(next) && ts_node_symbol(next) == gs.symComment)
    {
        next = ts_node_next_sibling(next);
    }
    return !ts_node_is_null(next) ? next : classOpeningBrace;
}

static void ScanOffendingChild(TSNode child, const parser::GrammarSymbols& gs, OffendingScanState& state)
{
    TSSymbol sym = ts_node_symbol(child);
    if (sym == gs.symComment)
    {
        return;
    }
    if (ts_node_is_null(state.firstNonComment))
    {
        state.firstNonComment = child;
    }
    if (sym == gs.symClassDeclaration || sym == gs.symInterfaceDeclaration || sym == gs.tokClass ||
        sym == gs.tokInterface)
    {
        state.seenClassOrIface = true;
    }
    if (state.seenClassOrIface && ts_node_is_null(state.classOpeningBrace))
    {
        if (sym == gs.tokOpenBrace)
        {
            state.classOpeningBrace = child;
        }
    }
    if (IsDeclarationSymbol(sym, gs))
    {
        state.lastValidDecl = child;
        state.firstOffendingAfterDecl = TSNode{};
    }
    else if (!ts_node_is_null(state.lastValidDecl) && ts_node_is_null(state.firstOffendingAfterDecl))
    {
        state.firstOffendingAfterDecl = child;
    }
}

/**
 * @brief Pinpoints the most specific offending AST node inside an ERROR node.
 * @param[in] errorNode Outer error node.
 * @param[in] gs Pre-resolved grammar symbols.
 * @return Pinpointed node representing the error location.
 */
static TSNode PinpointOffendingNode(TSNode errorNode, const parser::GrammarSymbols& gs)
{
    TSNode parent = parser::GetParent(errorNode);
    if (!ts_node_is_null(parent) && ts_node_symbol(parent) == gs.symArgumentList)
    {
        return errorNode;
    }

    TSTreeCursor cursor = ts_tree_cursor_new(errorNode);
    if (!ts_tree_cursor_goto_first_child(&cursor))
    {
        ts_tree_cursor_delete(&cursor);
        return errorNode;
    }

    OffendingScanState state;
    do
    {
        ScanOffendingChild(ts_tree_cursor_current_node(&cursor), gs, state);
    } while (ts_tree_cursor_goto_next_sibling(&cursor));

    ts_tree_cursor_delete(&cursor);

    if (!ts_node_is_null(state.firstOffendingAfterDecl))
    {
        return state.firstOffendingAfterDecl;
    }
    if (!ts_node_is_null(state.classOpeningBrace))
    {
        return ResolveOpeningBraceTarget(state.classOpeningBrace, gs);
    }
    if (!ts_node_is_null(state.firstNonComment))
    {
        return state.firstNonComment;
    }
    return errorNode;
}

static TSPoint ComputeLocalPoint(std::string_view sourceCode, TSNode baseNode, size_t targetOffset)
{
    TSPoint pt = ts_node_start_point(baseNode);
    const size_t limit = std::min(targetOffset, sourceCode.size());
    for (size_t i = ts_node_start_byte(baseNode); i < limit; ++i)
    {
        if (sourceCode[i] == '\n')
        {
            ++pt.row;
            pt.column = 0;
        }
        else
        {
            ++pt.column;
        }
    }
    return pt;
}

static bool IsIdentChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static void SkipStringLiteral(std::string_view text, size_t& idx)
{
    if (idx >= text.size())
    {
        return;
    }
    if (text.substr(idx).starts_with("\"\"\""))
    {
        const size_t end = text.find("\"\"\"", idx + 3);
        idx = (end == std::string_view::npos) ? text.size() : (end + 3);
        return;
    }
    const char quote = text[idx++];
    while (idx < text.size())
    {
        if (text[idx] == '\\')
        {
            idx += 2;
            continue;
        }
        if (text[idx] == quote)
        {
            ++idx;
            break;
        }
        if (text[idx] == '\n')
        {
            break;
        }
        ++idx;
    }
}

/**
 * @brief Advances through text skipping comments and literals to locate the next clause boundary.
 * @param[in] text Source text buffer to scan.
 * @param[in] start Byte offset from which scanning begins.
 * @param[in] limit Maximum byte offset boundary for scanning.
 * @return Byte offset of the next clause delimiter or limit if not found.
 */
static size_t FindNextClauseBoundary(std::string_view text, size_t start, size_t limit)
{
    size_t i = start;
    while (i < limit)
    {
        SkipWhitespaceAndComments(text, i);
        if (i >= limit)
        {
            break;
        }
        if (text[i] == '"' || text[i] == '\'')
        {
            SkipStringLiteral(text, i);
            continue;
        }
        std::string_view rem = text.substr(i, limit - i);
        if (rem.starts_with("case") && (rem.size() == 4 || !IsIdentChar(rem[4])))
        {
            return i;
        }
        if (rem.starts_with("default") && (rem.size() == 7 || !IsIdentChar(rem[7])))
        {
            return i;
        }
        if (text[i] == '}')
        {
            return i;
        }
        ++i;
    }
    return limit;
}

static bool IsStatementStart(std::string_view word)
{
    static constexpr std::string_view k_keywords[] = {
        "bool",  "break", "case",   "continue", "default", "double", "float",  "for",    "if",    "int",  "int16",
        "int64", "int8",  "return", "string",   "switch",  "uint",   "uint16", "uint64", "uint8", "void", "while"};
    return std::binary_search(std::begin(k_keywords), std::end(k_keywords), word);
}

static bool IsContinuationChar(char ch) noexcept
{
    constexpr std::string_view k_ops = "+-*/%&|^?:,=";
    return k_ops.find(ch) != std::string_view::npos;
}

static bool HandleCaseContinuation(std::string_view text, size_t idx, CaseExpressionState& state)
{
    if (state.parenDepth != 0 || state.ternaryDepth != 0)
    {
        return false;
    }

    std::string_view prefix = text.substr(0, idx);
    while (!prefix.empty() && (prefix.back() == ' ' || prefix.back() == '\t' || prefix.back() == '\r'))
    {
        prefix.remove_suffix(1);
    }

    if (!prefix.empty() && !IsContinuationChar(prefix.back()))
    {
        state.exprEnd = prefix.size();
        return true;
    }
    return false;
}

static void HandleParenToken(char ch, CaseExpressionState& state, size_t& idx)
{
    if (ch == '(' || ch == '[')
    {
        ++state.parenDepth;
    }
    else if ((ch == ')' || ch == ']') && state.parenDepth > 0)
    {
        --state.parenDepth;
    }
    ++idx;
    state.exprEnd = idx;
}

static ColonResult HandleColonToken(std::string_view text, size_t limit, CaseExpressionState& state, size_t& idx)
{
    if (idx + 1 < limit && text[idx + 1] == ':')
    {
        idx += 2;
        state.exprEnd = idx;
        return ColonResult::ScopeResolution;
    }

    if (state.parenDepth == 0 && state.ternaryDepth > 0)
    {
        --state.ternaryDepth;
        ++idx;
        state.exprEnd = idx;
        return ColonResult::TernaryMatched;
    }

    if (state.parenDepth == 0)
    {
        ++idx;
        return ColonResult::CaseColonFound;
    }

    ++idx;
    state.exprEnd = idx;
    return ColonResult::TernaryMatched;
}

static bool HandleIdentOrAssignment(std::string_view text, size_t limit, CaseExpressionState& state, size_t& idx)
{
    const char ch = text[idx];
    if (IsIdentChar(ch))
    {
        size_t wEnd = idx;
        while (wEnd < limit && IsIdentChar(text[wEnd]))
        {
            ++wEnd;
        }
        if (IsStatementStart(text.substr(idx, wEnd - idx)))
        {
            return false;
        }
    }

    if (ch == '=' && idx + 1 < limit && text[idx + 1] != '=' && text[idx + 1] != '>')
    {
        if (idx == 0 || (text[idx - 1] != '=' && text[idx - 1] != '!' && text[idx - 1] != '<' && text[idx - 1] != '>'))
        {
            return false;
        }
    }

    ++idx;
    state.exprEnd = idx;
    return true;
}

static void SkipCaseWhitespace(std::string_view text, size_t& idx, size_t limit)
{
    while (idx < limit && (text[idx] == ' ' || text[idx] == '\t' || text[idx] == '\r'))
    {
        ++idx;
    }
}

static CaseTokenAction HandleNewlineToken(std::string_view text, size_t& idx, CaseExpressionState& state)
{
    if (HandleCaseContinuation(text, idx, state))
    {
        return CaseTokenAction::Break;
    }
    ++idx;
    return CaseTokenAction::Continue;
}

static void HandleStringToken(std::string_view text, size_t& idx, CaseExpressionState& state)
{
    SkipStringLiteral(text, idx);
    state.exprEnd = idx;
}

static bool IsParenOrBracket(char ch) noexcept
{
    return ch == '(' || ch == '[' || ch == ')' || ch == ']';
}

static bool IsCaseTerminator(char ch, uint32_t parenDepth) noexcept
{
    return parenDepth == 0 && (ch == ';' || ch == '}');
}

static CaseTokenAction HandleColonOrQuestion(std::string_view text, size_t limit, CaseExpressionState& state,
                                             size_t& idx)
{
    const char ch = text[idx];
    if (ch == '?')
    {
        if (state.parenDepth == 0)
        {
            ++state.ternaryDepth;
        }
        ++idx;
        state.exprEnd = idx;
        return CaseTokenAction::Continue;
    }
    return (HandleColonToken(text, limit, state, idx) == ColonResult::CaseColonFound) ? CaseTokenAction::CaseColonFound
                                                                                      : CaseTokenAction::Continue;
}

static CaseTokenAction ProcessCaseExpressionChar(std::string_view text, size_t limit, CaseExpressionState& state,
                                                 size_t& idx)
{
    const char ch = text[idx];
    if (ch == '\n')
    {
        return HandleNewlineToken(text, idx, state);
    }
    if (ch == '"' || ch == '\'')
    {
        HandleStringToken(text, idx, state);
        return CaseTokenAction::Continue;
    }
    if (IsParenOrBracket(ch))
    {
        HandleParenToken(ch, state, idx);
        return CaseTokenAction::Continue;
    }
    if (ch == '?' || ch == ':')
    {
        return HandleColonOrQuestion(text, limit, state, idx);
    }
    if (IsCaseTerminator(ch, state.parenDepth))
    {
        return CaseTokenAction::Break;
    }
    return HandleIdentOrAssignment(text, limit, state, idx) ? CaseTokenAction::Continue : CaseTokenAction::Break;
}

static bool ScanCaseExpression(std::string_view text, size_t& idx, size_t limit, size_t& outExprEnd)
{
    CaseExpressionState state{0, 0, idx};

    while (idx < limit)
    {
        SkipCaseWhitespace(text, idx, limit);
        if (idx >= limit)
        {
            break;
        }

        const auto action = ProcessCaseExpressionChar(text, limit, state, idx);
        if (action == CaseTokenAction::Break)
        {
            break;
        }
        if (action == CaseTokenAction::CaseColonFound)
        {
            outExprEnd = state.exprEnd;
            return true;
        }
    }

    outExprEnd = state.exprEnd;
    return false;
}

/**
 * @brief Records a missing case or default colon diagnostic and returns the next clause boundary byte offset.
 * @param[in] sCtx Context bundle containing switch AST node and source text.
 * @param[in] startByte Clause keyword beginning byte offset.
 * @param[in] ptOffset Position offset for computing line and column coordinates.
 * @param[in] isDefault True if this clause is a 'default:' statement; false for 'case'.
 * @param[in,out] outColons Container accumulating discovered missing colon entries.
 * @return Next clause boundary byte offset.
 */
static size_t AppendMissingCaseColon(const SwitchScanContext& sCtx, size_t startByte, size_t ptOffset, bool isDefault,
                                     std::vector<MissingCaseColon>& outColons)
{
    MissingCaseColon colon;
    colon.clauseStartByte = static_cast<uint32_t>(startByte);
    const TSPoint pt = ComputeLocalPoint(sCtx.sourceCode, sCtx.switchNode, ptOffset);
    colon.line = pt.row;
    colon.character = pt.column;
    colon.isDefault = isDefault;
    colon.clauseEndByte = static_cast<uint32_t>(FindNextClauseBoundary(sCtx.sourceCode, ptOffset, sCtx.closeBraceByte));
    outColons.push_back(colon);
    return colon.clauseEndByte;
}

/**
 * @brief Scans a default clause within a switch statement to verify presence of a terminating colon.
 * @param[in] sCtx Context bundle containing switch AST node and source text.
 * @param[in,out] idx Current scanner byte offset advanced past the default clause.
 * @param[in,out] outColons Container accumulating discovered missing colon entries.
 */
static void ScanDefaultClauseColon(const SwitchScanContext& sCtx, size_t& idx, std::vector<MissingCaseColon>& outColons)
{
    const size_t defStart = idx;
    idx += 7;
    while (idx < sCtx.closeBraceByte &&
           (sCtx.sourceCode[idx] == ' ' || sCtx.sourceCode[idx] == '\t' || sCtx.sourceCode[idx] == '\r'))
    {
        ++idx;
    }
    if (idx < sCtx.closeBraceByte && sCtx.sourceCode[idx] == ':')
    {
        ++idx;
        return;
    }

    idx = AppendMissingCaseColon(sCtx, defStart, defStart + 7, true, outColons);
}

/**
 * @brief Scans a case clause within a switch statement to verify presence of a terminating colon.
 * @param[in] sCtx Context bundle containing switch AST node and source text.
 * @param[in,out] idx Current scanner byte offset advanced past the case clause.
 * @param[in,out] outColons Container accumulating discovered missing colon entries.
 */
static void ScanCaseClauseColon(const SwitchScanContext& sCtx, size_t& idx, std::vector<MissingCaseColon>& outColons)
{
    const size_t caseStart = idx;
    idx += 4;
    while (idx < sCtx.closeBraceByte &&
           (sCtx.sourceCode[idx] == ' ' || sCtx.sourceCode[idx] == '\t' || sCtx.sourceCode[idx] == '\r'))
    {
        ++idx;
    }
    size_t exprEnd = idx;
    const bool hasColon = ScanCaseExpression(sCtx.sourceCode, idx, sCtx.closeBraceByte, exprEnd);
    if (!hasColon)
    {
        idx = AppendMissingCaseColon(sCtx, caseStart, exprEnd, false, outColons);
    }
}

static bool FindSwitchBraceOffsets(TSNode switchNode, uint32_t& openByte, uint32_t& closeByte,
                                   const parser::GrammarSymbols& gs)
{
    openByte = UINT32_MAX;
    closeByte = ts_node_end_byte(switchNode);
    TSTreeCursor cur = ts_tree_cursor_new(switchNode);
    if (ts_tree_cursor_goto_first_child(&cur))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cur);
            const TSSymbol sym = ts_node_symbol(child);
            if (sym == gs.tokOpenBrace)
            {
                openByte = ts_node_end_byte(child);
            }
            else if (sym == gs.tokCloseBrace)
            {
                closeByte = ts_node_start_byte(child);
            }
        } while (ts_tree_cursor_goto_next_sibling(&cur));
    }
    ts_tree_cursor_delete(&cur);
    return openByte != UINT32_MAX && openByte < closeByte;
}

/**
 * @brief Scan switch statement case colons.
 * @param[in] switchNode AST node under evaluation.
 * @param[in] sourceCode Document source text.
 * @param[in] gs Precompiled grammar symbols.
 * @param[in,out] outColons Vector collecting byte offsets of double-colon scope separators.
 */
static void ScanSwitchStatementCaseColons(TSNode switchNode, std::string_view sourceCode,
                                          const parser::GrammarSymbols& gs, std::vector<MissingCaseColon>& outColons)
{
    uint32_t openByte = 0;
    uint32_t closeByte = 0;
    if (!FindSwitchBraceOffsets(switchNode, openByte, closeByte, gs))
    {
        return;
    }

    SwitchScanContext scanCtx{switchNode, sourceCode, closeByte};

    size_t idx = openByte;
    while (idx < closeByte)
    {
        SkipWhitespaceAndComments(sourceCode, idx);
        if (idx >= closeByte)
        {
            break;
        }

        if (sourceCode[idx] == '"' || sourceCode[idx] == '\'')
        {
            SkipStringLiteral(sourceCode, idx);
            continue;
        }

        std::string_view rem = sourceCode.substr(idx, closeByte - idx);
        if (rem.starts_with("default") && (rem.size() == 7 || !IsIdentChar(rem[7])))
        {
            ScanDefaultClauseColon(scanCtx, idx, outColons);
        }
        else if (rem.starts_with("case") && (rem.size() == 4 || !IsIdentChar(rem[4])))
        {
            ScanCaseClauseColon(scanCtx, idx, outColons);
        }
        else
        {
            ++idx;
        }
    }
}

void SymbolCollector::EmitMissingColonDiagnostics(std::span<const MissingCaseColon> missingColons,
                                                  SymbolCollectContext& sCtx) const
{
    for (const auto& mc : missingColons)
    {
        Diagnostic diag;
        diag.range.start.line = mc.line;
        diag.range.start.character = mc.character;
        diag.range.end.line = mc.line;
        diag.range.end.character = mc.character + 1;
        diag.severity = DiagnosticSeverity::Error;
        diag.code = "as-syntax-error";
        diag.source = "AngelScript";
        diag.fileUri = sCtx.request.fileUri;
        const char* key = mc.isDefault ? "note-expected-colon-after-default" : "note-expected-colon-after-case";
        const char* fallback = mc.isDefault ? "Expected ':' after 'default'" : "Expected ':' after case expression";
        diag.message = i18n::FormatMessage(sCtx.request.i18n, key, fallback);
        sCtx.diagnostics.push_back(std::move(diag));
    }
}

void SymbolCollector::HandleParseErrorNode(TSNode node, ParseErrorReportContext& errCtx) const
{
    const uint32_t startByte = ts_node_start_byte(node);
    for (const auto& mc : errCtx.missingColons)
    {
        if (startByte >= mc.clauseStartByte && startByte < mc.clauseEndByte)
        {
            return;
        }
    }

    TSNode target = ts_node_is_missing(node) ? node : PinpointOffendingNode(node, m_gs);
    if (ts_node_symbol(target) == m_gs.symComment || ts_node_symbol(node) == m_gs.symComment)
    {
        return;
    }
    const uint32_t line = ts_node_start_point(target).row;
    if (errCtx.missingOpenBrace && errCtx.firstErrorLine != UINT32_MAX && line > errCtx.firstErrorLine)
    {
        return;
    }
    EmitParseErrorDiagnostic(target, errCtx.sCtx);
    if (errCtx.firstErrorLine == UINT32_MAX)
    {
        errCtx.firstErrorLine = line;
    }
}

void SymbolCollector::ReportParseErrors(const NodeIndex& index, SymbolCollectContext& sCtx) const
{
    if (!ts_node_has_error(index.Root()))
    {
        return;
    }

    const auto braceBalance = parser::ComputeBraceBalance(sCtx.request.sourceCode);
    ParseErrorReportContext errCtx{sCtx, braceBalance.HasMissingOpenBrace(), UINT32_MAX, {}};
    const TSSymbol candidates[] = {m_gs.symSwitchStatement, m_gs.symError};
    index.ForEachNodeOrdered(
        candidates,
        [&](TSNode node)
        {
            if (ts_node_is_missing(node) || (ts_node_is_error(node) && !HasNestedErrorOrMissing(node)))
            {
                HandleParseErrorNode(node, errCtx);
            }
            else if (ts_node_symbol(node) == m_gs.symSwitchStatement && ts_node_has_error(node))
            {
                const size_t prevCount = errCtx.missingColons.size();
                ScanSwitchStatementCaseColons(node, sCtx.request.sourceCode, m_gs, errCtx.missingColons);
                EmitMissingColonDiagnostics(std::span<const MissingCaseColon>(errCtx.missingColons).subspan(prevCount),
                                            sCtx);
            }
        });
}

/**
 * @brief Formats an argument list syntax error when two arguments appear without a comma.
 * @param[in] node AST error node.
 * @param[in] sCtx Symbol collection context.
 * @param[in,out] diag Diagnostic to populate.
 * @return True if formatted as an argument list syntax error.
 */
bool SymbolCollector::TryFormatArgumentListSyntaxError(TSNode node, const SymbolCollectContext& sCtx,
                                                       Diagnostic& diag) const
{
    TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent) || ts_node_symbol(parent) != m_gs.symArgumentList)
    {
        return false;
    }

    TSNode nextSibling = ts_node_next_sibling(node);
    if (ts_node_is_null(nextSibling))
    {
        return false;
    }

    TSSymbol nextSym = ts_node_symbol(nextSibling);
    if (nextSym == m_gs.tokComma || nextSym == m_gs.tokCloseParen)
    {
        return false;
    }

    std::string_view nextView = parser::NodeText(nextSibling, sCtx.request.sourceCode);
    while (!nextView.empty() && (nextView.front() == ' ' || nextView.front() == '\t'))
    {
        nextView.remove_prefix(1);
    }
    while (!nextView.empty() && (nextView.back() == ' ' || nextView.back() == '\t'))
    {
        nextView.remove_suffix(1);
    }
    std::string nextText;
    if (nextView.size() > 24)
    {
        nextText = fmt::format("{}...", nextView.substr(0, 24));
    }
    else
    {
        nextText = std::string(nextView);
    }

    diag.range.end.line = ts_node_end_point(nextSibling).row;
    diag.range.end.character = ts_node_end_point(nextSibling).column;
    diag.message = i18n::FormatMessage(sCtx.request.i18n, "note-expected-comma-or-paren",
                                       "Expected ',' or ')' before '{}'", std::string_view(nextText));
    return true;
}

/**
 * @brief Retrieves the effective previous sibling for an error node, inspecting its parent if necessary.
 * @param[in] node AST error node.
 * @return Previous sibling node, or null node if none exists.
 */
static TSNode FindEffectivePreviousSibling(TSNode node)
{
    TSNode prev = ts_node_prev_sibling(node);
    if (ts_node_is_null(prev))
    {
        TSNode parent = parser::GetParent(node);
        if (!ts_node_is_null(parent))
        {
            prev = ts_node_prev_sibling(parent);
        }
    }
    return prev;
}

/**
 * @brief Checks if a preceding node represents a statement-ending expression symbol.
 * @param[in] prev Preceding node to test.
 * @param[in] gs Grammar symbol table.
 * @return True if the node is an expression candidate preceding a missing semicolon.
 */
static bool IsPrecedingExpression(TSNode prev, const parser::GrammarSymbols& gs)
{
    if (ts_node_is_null(prev))
    {
        return false;
    }
    const TSSymbol prevSym = ts_node_symbol(prev);
    return (prevSym == gs.symMemberExpression || prevSym == gs.symScopedIdentifier || prevSym == gs.symCallExpression ||
            prevSym == gs.symIdentifier);
}

/**
 * @brief Extracts the first non-whitespace lexical token from error text up to a semicolon or delimiter.
 * @param[in] text Raw text snippet containing error.
 * @return Substring view of the first token, or empty view if none.
 */
static std::string_view ExtractMissingSemicolonToken(std::string_view text)
{
    std::string_view trimmed = TrimView(text);
    if (trimmed.empty())
    {
        return {};
    }
    const size_t tokenEnd = trimmed.find_first_of(" \t\r\n;");
    return (tokenEnd != std::string_view::npos) ? trimmed.substr(0, tokenEnd) : trimmed;
}

bool SymbolCollector::TryFormatMissingSemicolonError(TSNode node, const SymbolCollectContext& sCtx,
                                                     Diagnostic& diag) const
{
    TSNode prev = FindEffectivePreviousSibling(node);
    if (!IsPrecedingExpression(prev, m_gs))
    {
        return false;
    }

    std::string_view token = ExtractMissingSemicolonToken(parser::NodeText(node, sCtx.request.sourceCode));
    if (token.empty())
    {
        return false;
    }

    diag.message =
        i18n::FormatMessage(sCtx.request.i18n, "note-expected-semicolon-before", "Expected ';' before '{}'", token);
    return true;
}

void SymbolCollector::EmitParseErrorDiagnostic(TSNode node, SymbolCollectContext& sCtx) const
{
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt = ts_node_end_point(node);

    Diagnostic diag;
    diag.range.start.line = startPt.row;
    diag.range.start.character = startPt.column;
    diag.range.end.line = endPt.row;
    diag.range.end.character = endPt.column;
    diag.severity = DiagnosticSeverity::Error;
    diag.code = "as-syntax-error";
    diag.source = "AngelScript";
    diag.fileUri = sCtx.request.fileUri;

    std::string_view rawErrText = parser::NodeText(node, sCtx.request.sourceCode);
    std::string logMsg;

    if (ts_node_is_missing(node))
    {
        std::string_view missingToken = parser::GetNodeType(node);
        const std::string_view pattern = sCtx.request.i18n
                                             ? sCtx.request.i18n->GetMessageView("as-syntax-error-missing")
                                             : std::string_view("Syntax error: missing '{}'");
        diag.message = fmt::format(fmt::runtime(pattern), missingToken);
        logMsg = diag.message;
    }
    else if (TryFormatArgumentListSyntaxError(node, sCtx, diag))
    {
        logMsg = diag.message;
    }
    else if (TryFormatMissingSemicolonError(node, sCtx, diag))
    {
        logMsg = diag.message;
    }
    else
    {
        diag.message = FormatSyntaxErrorMessage(rawErrText, diag.code, sCtx.request.i18n);
        logMsg = diag.message;
    }

    uint32_t startByte = ts_node_start_byte(node);
    while (startByte < sCtx.request.sourceCode.size() &&
           (sCtx.request.sourceCode[startByte] == ' ' || sCtx.request.sourceCode[startByte] == '\t'))
    {
        ++startByte;
        ++diag.range.start.character;
    }

    if (diag.range.end.line > diag.range.start.line)
    {
        diag.range.end.line = diag.range.start.line;
        std::string firstTok = ExtractFirstToken(rawErrText);
        uint32_t len = firstTok.empty() ? 1 : static_cast<uint32_t>(firstTok.size());
        diag.range.end.character = diag.range.start.character + len;
    }
    else if (diag.range.end.character <= diag.range.start.character)
    {
        diag.range.end.character = diag.range.start.character + 1;
    }

    sCtx.diagnostics.push_back(std::move(diag));

    if (m_logger)
    {
        m_logger->LogWarning(fmt::format("[Tree-sitter Error] {} en [L{}:C{}] (File: {})", logMsg, startPt.row + 1,
                                         startPt.column + 1, sCtx.request.fileUri));
    }
}

std::string SymbolCollector::FormatSyntaxErrorMessage(std::string_view rawErrText, std::string_view& outCode,
                                                      const angel_lsp::i18n::I18n* i18n) const
{
    std::string firstToken = ExtractFirstToken(rawErrText);

    if (firstToken == "shared")
    {
        outCode = "as-err-shared-not-allowed-on-entity";
        return i18n ? std::string(i18n->GetMessageView("as-err-shared-not-allowed-on-entity"))
                    : "The 'shared' modifier is not allowed on this entity";
    }
    if (firstToken.empty())
    {
        return i18n ? std::string(i18n->GetMessageView("as-syntax-error-generic")) : "Syntax error";
    }

    const std::string_view pattern =
        i18n ? i18n->GetMessageView("as-syntax-error") : std::string_view("Syntax error: \"{}\"");
    return fmt::format(fmt::runtime(pattern), firstToken);
}

// AST/Text Extraction Helpers

bool SymbolCollector::ApplyAccessOrStorageString(std::string_view text, SymbolModifiers& modifiers) const
{
    if (text == "private")
        modifiers.access = AccessModifier::Private;
    else if (text == "protected")
        modifiers.access = AccessModifier::Protected;
    else if (text == "public")
        modifiers.access = AccessModifier::Public;
    else if (text == "const")
        modifiers.isConst = true;
    else if (text == "shared")
        modifiers.isShared = true;
    else if (text == "external")
        modifiers.isExternal = true;
    else
        return false;
    return true;
}

void SymbolCollector::ApplyModifierString(std::string_view text, SymbolModifiers& modifiers, bool isFuncAttr) const
{
    if (ApplyAccessOrStorageString(text, modifiers))
        return;

    if (text == "final")
    {
        modifiers.isFinal = true;
        if (!isFuncAttr)
            modifiers.isDeclarationFinal = true;
    }
    else if (text == "abstract")
    {
        modifiers.isAbstract = true;
        if (!isFuncAttr)
            modifiers.isDeclarationAbstract = true;
    }
    else if (text == "override")
        modifiers.isOverride = true;
    else if (text == "explicit")
        modifiers.isExplicit = true;
    else if (text == "property")
        modifiers.isProperty = true;
    else if (text == "mixin")
        modifiers.isMixin = true;
    else if (text == "delete")
        modifiers.isDelete = true;
}

void SymbolCollector::ProcessModifierChild(TSNode child, std::string_view sourceCode, SymbolModifiers& modifiers) const
{
    std::string_view childText = TrimView(parser::NodeText(child, sourceCode));
    bool isDeclMod = (ts_node_symbol(child) == m_gs.symDeclarationModifier ||
                      ts_node_symbol(child) == m_gs.symSharedExternalModifier);
    bool isFuncAttr = (ts_node_symbol(child) == m_gs.symFuncAttributes);

    ApplyModifierString(childText, modifiers, isFuncAttr);

    if (isDeclMod || isFuncAttr)
    {
        TSTreeCursor cursor = ts_tree_cursor_new(child);
        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            do
            {
                TSNode grandChild = ts_tree_cursor_current_node(&cursor);
                std::string_view gcText = TrimView(parser::NodeText(grandChild, sourceCode));
                ApplyModifierString(gcText, modifiers, isFuncAttr);

                TSSymbol tokSym = ts_node_symbol(grandChild);
                ApplyModifierToken(tokSym, modifiers);
                if (tokSym == m_gs.tokFinal && !isFuncAttr)
                    modifiers.isDeclarationFinal = true;
                else if (tokSym == m_gs.tokAbstract && !isFuncAttr)
                    modifiers.isDeclarationAbstract = true;
            } while (ts_tree_cursor_goto_next_sibling(&cursor));
        }
        ts_tree_cursor_delete(&cursor);
    }
    else if (!ts_node_is_named(child))
    {
        ApplyModifierToken(ts_node_symbol(child), modifiers);
    }
}

SymbolModifiers SymbolCollector::ExtractModifiers(TSNode node, std::string_view sourceCode) const
{
    SymbolModifiers modifiers;
    if (ts_node_is_null(node))
        return modifiers;

    TSNode nameNode = GetChildByFieldId(node, m_gs.fieldName);

    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            if (!ts_node_is_null(nameNode) && ts_node_eq(child, nameNode))
            {
                continue;
            }
            ProcessModifierChild(child, sourceCode, modifiers);
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return modifiers;
}

bool SymbolCollector::ApplyParamModifierToken(TSSymbol tokenSymbol, SymbolModifiers& modifiers) const
{
    if (tokenSymbol == m_gs.tokIn)
        modifiers.paramModifier = ParameterModifier::In;
    else if (tokenSymbol == m_gs.tokOut)
        modifiers.paramModifier = ParameterModifier::Out;
    else if (tokenSymbol == m_gs.tokInout)
        modifiers.paramModifier = ParameterModifier::InOut;
    else if (tokenSymbol == m_gs.tokAmp)
        modifiers.isReturnReference = true;
    else if (tokenSymbol == m_gs.tokAt)
        modifiers.isHandle = true;
    else
        return false;
    return true;
}

void SymbolCollector::ApplyModifierToken(TSSymbol tokenSymbol, SymbolModifiers& modifiers) const
{
    if (ApplyParamModifierToken(tokenSymbol, modifiers))
        return;

    if (tokenSymbol == m_gs.tokConst)
        modifiers.isConst = true;
    else if (tokenSymbol == m_gs.tokPrivate)
        modifiers.access = AccessModifier::Private;
    else if (tokenSymbol == m_gs.tokProtected)
        modifiers.access = AccessModifier::Protected;
    else if (tokenSymbol == m_gs.tokShared)
        modifiers.isShared = true;
    else if (tokenSymbol == m_gs.tokMixin)
        modifiers.isMixin = true;
    else if (tokenSymbol == m_gs.tokAbstract)
        modifiers.isAbstract = true;
    else if (tokenSymbol == m_gs.tokFinal)
        modifiers.isFinal = true;
    else if (tokenSymbol == m_gs.tokOverride)
        modifiers.isOverride = true;
    else if (tokenSymbol == m_gs.tokExplicit)
        modifiers.isExplicit = true;
    else if (tokenSymbol == m_gs.tokProperty)
        modifiers.isProperty = true;
    else if (tokenSymbol == m_gs.tokDelete)
        modifiers.isDelete = true;
    else if (tokenSymbol == m_gs.tokExternal || tokenSymbol == m_gs.tokImport)
        modifiers.isExternal = true;
}

void SymbolCollector::ExtractParamTypeRefAndConst(TSNode pTypeNode, std::string_view sourceCode,
                                                  ParameterInformation& paramInfo, uint32_t& refCount) const
{
    if (ts_node_is_null(pTypeNode))
        return;

    TSTreeCursor cursor = ts_tree_cursor_new(pTypeNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode typeChild = ts_tree_cursor_current_node(&cursor);
            if (!ts_node_is_named(typeChild))
            {
                std::string_view tok = parser::NodeText(typeChild, sourceCode);
                if (tok == "const")
                    paramInfo.isConst = true;
                else if (tok == "&")
                {
                    paramInfo.isReference = true;
                    refCount++;
                }
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

void SymbolCollector::ExtractParamModifierTokens(TSNode paramNode, std::string_view sourceCode,
                                                 ParameterInformation& paramInfo, uint32_t& refCount) const
{
    TSNode dirNode = GetChildByFieldId(paramNode, m_gs.fieldDirection);
    if (!ts_node_is_null(dirNode))
    {
        std::string_view dirTok = parser::NodeText(dirNode, sourceCode);
        paramInfo.isReference = true;
        if (dirTok == "inout")
        {
            paramInfo.modifier = ParameterModifier::InOut;
        }
        else if (dirTok == "in")
        {
            paramInfo.modifier = ParameterModifier::In;
        }
        else if (dirTok == "out")
        {
            paramInfo.modifier = ParameterModifier::Out;
        }
    }

    TSTreeCursor cursor = ts_tree_cursor_new(paramNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            std::string_view tok = parser::NodeText(child, sourceCode);
            if (tok == "&")
            {
                paramInfo.isReference = true;
                refCount++;
            }
            else if (tok == "inout" || tok == "&inout")
            {
                paramInfo.modifier = ParameterModifier::InOut;
                paramInfo.isReference = true;
            }
            else if (tok == "in" || tok == "&in")
            {
                paramInfo.modifier = ParameterModifier::In;
                paramInfo.isReference = true;
            }
            else if (tok == "out" || tok == "&out")
            {
                paramInfo.modifier = ParameterModifier::Out;
                paramInfo.isReference = true;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

static std::string ExtractFallbackDefaultValue(std::string_view rawText)
{
    const size_t eqPos = rawText.find('=');
    if (eqPos == std::string_view::npos)
    {
        return {};
    }
    std::string_view defVal = rawText.substr(eqPos + 1);
    while (!defVal.empty() && (defVal.front() == ' ' || defVal.front() == '\t'))
    {
        defVal.remove_prefix(1);
    }
    while (!defVal.empty() && (defVal.back() == ' ' || defVal.back() == '\t'))
    {
        defVal.remove_suffix(1);
    }
    return std::string(defVal);
}

static void AssignParamLocations(ParameterInformation& paramInfo, TSNode paramNode, TSNode pNameNode)
{
    TSPoint startPt = ts_node_start_point(paramNode);
    TSPoint endPt = ts_node_end_point(paramNode);
    paramInfo.startLine = startPt.row;
    paramInfo.startCharacter = startPt.column;
    paramInfo.endLine = endPt.row;
    paramInfo.endCharacter = endPt.column;

    if (!ts_node_is_null(pNameNode))
    {
        TSPoint nameStart = ts_node_start_point(pNameNode);
        TSPoint nameEnd = ts_node_end_point(pNameNode);
        paramInfo.nameStartLine = nameStart.row;
        paramInfo.nameStartCharacter = nameStart.column;
        paramInfo.nameEndLine = nameEnd.row;
        paramInfo.nameEndCharacter = nameEnd.column;
    }
}

/**
 * @brief Extracts parameter information including type, modifiers, and default value from AST.
 * @param[in] paramNode AST parameter node being evaluated.
 * @param[in] ctx Document source and captured operands.
 * @return Populated ParameterInformation struct describing the parameter.
 */
ParameterInformation SymbolCollector::ExtractParameterInfo(TSNode paramNode, const SymbolCollectContext& ctx) const
{
    const auto sourceCode = ctx.request.sourceCode;
    TSNode pNameNode = GetChildByFieldId(paramNode, m_gs.fieldName);
    TSNode pTypeNode = GetChildByFieldId(paramNode, m_gs.fieldParamType);
    TSNode pDefaultNode = GetChildByFieldId(paramNode, m_gs.fieldDefaultValue);

    TypeExtractionResult pInfo = ExtractTypeInfoFromAST(pTypeNode, sourceCode, &ctx.index);

    ParameterInformation paramInfo;
    paramInfo.name = std::string(parser::NodeText(pNameNode, sourceCode));
    paramInfo.typeName = std::string(parser::NodeText(pTypeNode, sourceCode));
    const std::string_view paramText = parser::NodeText(paramNode, sourceCode);
    paramInfo.baseTypeName = pInfo.baseTypeName;
    paramInfo.templateName = pInfo.templateName;
    paramInfo.typeKind = pInfo.kind;
    paramInfo.isArray = pInfo.isArray;
    paramInfo.hasPrimitiveHandle = pInfo.hasPrimitiveHandle;
    paramInfo.arrayDepth = pInfo.arrayDepth;
    paramInfo.defaultValue = std::string(parser::NodeText(pDefaultNode, sourceCode));
    if (paramInfo.defaultValue.empty())
    {
        paramInfo.defaultValue = ExtractFallbackDefaultValue(paramText);
    }
    paramInfo.isHandle = pInfo.isHandle;
    AssignParamLocations(paramInfo, paramNode, pNameNode);

    uint32_t refCount = 0;
    if (pInfo.isConst || pInfo.isReference)
    {
        ExtractParamTypeRefAndConst(pTypeNode, sourceCode, paramInfo, refCount);
    }
    ExtractParamModifierTokens(paramNode, sourceCode, paramInfo, refCount);

    paramInfo.hasDoubleReference = (refCount > 1);
    paramInfo.isStandaloneRef = (paramInfo.isReference && paramInfo.modifier == ParameterModifier::None);
    paramInfo.isOutRef = (paramInfo.modifier == ParameterModifier::Out);
    paramInfo.isInOutRef = (paramInfo.modifier == ParameterModifier::InOut);
    return paramInfo;
}

std::vector<ParameterInformation> SymbolCollector::ExtractParameters(TSNode paramsNode,
                                                                     const SymbolCollectContext& ctx) const
{
    std::vector<ParameterInformation> parameters;
    SyntaxOperands(paramsNode, &ctx.index)
        .ForEachNamed(
            [&](TSNode parameter)
            {
                if (ts_node_symbol(parameter) == m_gs.symParameter)
                {
                    parameters.push_back(ExtractParameterInfo(parameter, ctx));
                }
                return true;
            });
    if (parameters.size() == 1 && parameters[0].typeName == "void" && parameters[0].name.empty())
    {
        parameters.clear();
    }
    return parameters;
}

void SymbolCollector::appendQualifiedName(std::string& outBuffer, std::string_view scope, std::string_view name)
{
    outBuffer.clear();
    outBuffer.reserve(scope.size() + (scope.empty() ? 0 : 2) + name.size());
    if (!scope.empty())
    {
        outBuffer.append(scope);
        outBuffer.append("::");
    }
    outBuffer.append(name);
}

static bool PrecedingByteCouldBeMetadata(std::string_view sourceCode, uint32_t startByte) noexcept
{
    if (startByte == 0 || startByte > sourceCode.size())
    {
        return false;
    }
    size_t pos = startByte;
    while (pos > 0)
    {
        --pos;
        char c = sourceCode[pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            continue;
        }
        return (c == ']' || c == '/');
    }
    return false;
}

Symbol SymbolCollector::CreateSymbol(SymbolType type, TSNode node, TSNode nameNode,
                                     const SymbolLocationContext& loc) const
{
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt = ts_node_end_point(node);
    TSPoint nameStartPt = ts_node_is_null(nameNode) ? startPt : ts_node_start_point(nameNode);
    TSPoint nameEndPt = ts_node_is_null(nameNode) ? endPt : ts_node_end_point(nameNode);

    Symbol sym;
    sym.type = type;
    sym.name = std::string(parser::NodeText(nameNode, loc.sourceCode));
    sym.containerName = loc.containerPath;
    sym.containerKind = loc.containerKind;
    appendQualifiedName(sym.qualifiedName, loc.containerPath, sym.name);
    sym.fileUri = loc.fileUri;
    sym.isPredefined = loc.isPredefined;
    sym.startLine = startPt.row;
    sym.startCharacter = startPt.column;
    sym.endLine = endPt.row;
    sym.endCharacter = endPt.column;

    sym.fullRange = {startPt.row, startPt.column, endPt.row, endPt.column};
    sym.selectionRange = {nameStartPt.row, nameStartPt.column, nameEndPt.row, nameEndPt.column};

    if (loc.sourceHasBrackets && type != SymbolType::CallReference && type != SymbolType::Namespace)
    {
        uint32_t startByte = ts_node_start_byte(node);
        if (PrecedingByteCouldBeMetadata(loc.sourceCode, startByte))
        {
            TSNode prev = ts_node_prev_named_sibling(node);
            std::vector<std::string> metaBlocks;
            while (!ts_node_is_null(prev) && ts_node_symbol(prev) == m_gs.symMetadata)
            {
                metaBlocks.push_back(std::string(parser::NodeText(prev, loc.sourceCode)));
                prev = ts_node_prev_named_sibling(prev);
            }
            if (!metaBlocks.empty())
            {
                std::reverse(metaBlocks.begin(), metaBlocks.end());
                sym.metadata = std::move(metaBlocks);
            }
        }
    }

    return sym;
}

std::vector<std::string> SymbolCollector::ExtractBases(TSNode classNode, std::string_view sourceCode) const
{
    std::vector<std::string> bases;

    TSTreeCursor cursor = ts_tree_cursor_new(classNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            if (ts_node_is_named(child) && ts_node_symbol(child) == m_gs.symBaseClassList)
            {
                TSTreeCursor baseCursor = ts_tree_cursor_new(child);
                if (ts_tree_cursor_goto_first_child(&baseCursor))
                {
                    do
                    {
                        TSNode baseChild = ts_tree_cursor_current_node(&baseCursor);
                        if (ts_node_is_named(baseChild))
                        {
                            std::string text(parser::NodeText(baseChild, sourceCode));
                            if (!text.empty())
                                bases.push_back(std::move(text));
                        }
                    } while (ts_tree_cursor_goto_next_sibling(&baseCursor));
                }
                ts_tree_cursor_delete(&baseCursor);
                break;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return bases;
}
} // namespace angel_lsp::analysis
