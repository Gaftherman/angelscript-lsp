#include "analysis/SymbolCollector.h"
#include "analysis/SemanticHelpers.h"
#include "parser/QueryRegistry.h"
#include "parser/queries/BuiltQueries.h"
#include "spdlog/fmt/fmt.h"
#include "utils/LspLogger.h"

#include <ankerl/unordered_dense.h>
#include <cctype>
#include <cstring>
#include <string_view>

extern "C" const TSLanguage* tree_sitter_angelscript();

namespace angel_lsp::analysis
{
namespace
{
#define SYM_NAME(str) str, static_cast<uint32_t>(sizeof(str) - 1)

std::string_view TrimView(std::string_view sv)
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

std::string ExtractFirstToken(const std::string& rawErrText)
{
    std::string firstToken;
    for (char c : rawErrText)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || firstToken.size() >= 20)
        {
            if (!firstToken.empty())
                break;
            continue;
        }
        if (static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) != 127)
        {
            firstToken += c;
        }
    }
    return firstToken;
}

struct MemberNodes
{
    TSNode declNode;
    TSNode nameNode;
};
} // namespace

// =========================================================================================
// Public API & Lifecycle
// =========================================================================================

SymbolCollector::SymbolCollector(angel_lsp::utils::LspLogger* logger) : m_logger(logger)
{
    const TSLanguage* lang = tree_sitter_angelscript();
    ResolveGrammarSymbols(lang);
    InitQueryDispatch();
}

void SymbolCollector::ResolveGrammarSymbols(const TSLanguage* lang)
{
    m_symDeclarationModifier = ts_language_symbol_for_name(lang, SYM_NAME("declaration_modifier"), true);
    m_symClassBody = ts_language_symbol_for_name(lang, SYM_NAME("class_body"), true);
    m_symNamespaceBody = ts_language_symbol_for_name(lang, SYM_NAME("namespace_body"), true);
    m_symInterfaceBody = ts_language_symbol_for_name(lang, SYM_NAME("interface_body"), true);
    m_symEnumMember = ts_language_symbol_for_name(lang, SYM_NAME("enum_member"), true);
    m_symFuncDeclaration = ts_language_symbol_for_name(lang, SYM_NAME("func_declaration"), true);
    m_symStatementBlock = ts_language_symbol_for_name(lang, SYM_NAME("statement_block"), true);
    m_symInterfaceMethod = ts_language_symbol_for_name(lang, SYM_NAME("interface_method"), true);
    m_symFuncAttributes = ts_language_symbol_for_name(lang, SYM_NAME("func_attributes"), true);
    m_symGet = ts_language_symbol_for_name(lang, SYM_NAME("get"), false);
    m_symSet = ts_language_symbol_for_name(lang, SYM_NAME("set"), false);

    m_symVariableDeclarator = ts_language_symbol_for_name(lang, SYM_NAME("variable_declarator"), true);
    m_symAccessor = ts_language_symbol_for_name(lang, SYM_NAME("accessor"), true);
    m_symImportDeclaration = ts_language_symbol_for_name(lang, SYM_NAME("import_declaration"), true);

    m_symScopedIdentifier = ts_language_symbol_for_name(lang, SYM_NAME("scoped_identifier"), true);
    m_symMixinDeclaration = ts_language_symbol_for_name(lang, SYM_NAME("mixin_declaration"), true);
    m_symSharedExternalModifier = ts_language_symbol_for_name(lang, SYM_NAME("shared_external_modifier"), true);
    m_symVirtualProperty = ts_language_symbol_for_name(lang, SYM_NAME("virtual_property"), true);
    m_symBaseClassList = ts_language_symbol_for_name(lang, SYM_NAME("base_class_list"), true);
    m_symParameter = ts_language_symbol_for_name(lang, SYM_NAME("parameter"), true);
    m_symMemberExpression = ts_language_symbol_for_name(lang, SYM_NAME("member_expression"), true);

    m_tokConst = ts_language_symbol_for_name(lang, SYM_NAME("const"), false);
    m_tokIn = ts_language_symbol_for_name(lang, SYM_NAME("in"), false);
    m_tokOut = ts_language_symbol_for_name(lang, SYM_NAME("out"), false);
    m_tokInout = ts_language_symbol_for_name(lang, SYM_NAME("inout"), false);
    m_tokAmp = ts_language_symbol_for_name(lang, SYM_NAME("&"), false);
    m_tokAt = ts_language_symbol_for_name(lang, SYM_NAME("@"), false);
    m_tokPrivate = ts_language_symbol_for_name(lang, SYM_NAME("private"), false);
    m_tokProtected = ts_language_symbol_for_name(lang, SYM_NAME("protected"), false);
    m_tokShared = ts_language_symbol_for_name(lang, SYM_NAME("shared"), false);
    m_tokMixin = ts_language_symbol_for_name(lang, SYM_NAME("mixin"), false);
    m_tokAbstract = ts_language_symbol_for_name(lang, SYM_NAME("abstract"), false);
    m_tokFinal = ts_language_symbol_for_name(lang, SYM_NAME("final"), false);
    m_tokOverride = ts_language_symbol_for_name(lang, SYM_NAME("override"), false);
    m_tokExplicit = ts_language_symbol_for_name(lang, SYM_NAME("explicit"), false);
    m_tokProperty = ts_language_symbol_for_name(lang, SYM_NAME("property"), false);
    m_tokDelete = ts_language_symbol_for_name(lang, SYM_NAME("delete"), false);
    m_tokExternal = ts_language_symbol_for_name(lang, SYM_NAME("external"), false);
    m_tokImport = ts_language_symbol_for_name(lang, SYM_NAME("import"), false);
    m_tokOpenBrace = ts_language_symbol_for_name(lang, SYM_NAME("{"), false);
}

void SymbolCollector::InitQueryDispatch()
{
    m_tagsQuery = angel_lsp::parser::QueryRegistry::GetTagsQuery();
    if (!m_tagsQuery)
    {
        if (m_logger)
        {
            m_logger->LogError("Failed to get TAGS_QUERY from QueryRegistry");
        }
        return;
    }

    PopulateDispatchTables();
}

SymbolCollector::ProcessFn SymbolCollector::ResolveCaptureHandler(std::string_view captureName) const
{
    if (captureName == "definition.function" || captureName == "definition.import" ||
        captureName == "local.definition.import")
        return &SymbolCollector::ProcessFunction;
    if (captureName == "definition.class")
        return &SymbolCollector::ProcessClass;
    if (captureName == "definition.variable")
        return &SymbolCollector::ProcessVariable;
    if (captureName == "definition.namespace")
        return &SymbolCollector::ProcessNamespace;
    if (captureName == "definition.enum")
        return &SymbolCollector::ProcessEnum;
    if (captureName == "definition.typedef")
        return &SymbolCollector::ProcessTypedef;
    if (captureName == "definition.funcdef")
        return &SymbolCollector::ProcessFuncdef;
    if (captureName == "definition.interface")
        return &SymbolCollector::ProcessInterface;
    if (captureName == "definition.property")
        return &SymbolCollector::ProcessProperty;
    if (captureName == "reference.call")
        return &SymbolCollector::ProcessCallReference;
    return nullptr;
}

SymbolCollector::ValidationFn SymbolCollector::ResolveValidationHandler(std::string_view captureName) const
{
    if (captureName == "validation.using")
        return &SymbolCollector::CheckUsingDeclarationCapture;
    if (captureName == "validation.modifiers")
        return &SymbolCollector::CheckDuplicateModifierGroup;
    return nullptr;
}

void SymbolCollector::PopulateDispatchTables()
{
    uint32_t captureCount = ts_query_capture_count(m_tagsQuery);
    m_captureDispatch.assign(captureCount, nullptr);
    m_validationDispatch.assign(captureCount, nullptr);

    for (uint32_t i = 0; i < captureCount; ++i)
    {
        uint32_t nameLen = 0;
        const char* name = ts_query_capture_name_for_id(m_tagsQuery, i, &nameLen);
        std::string_view captureName(name, nameLen);

        m_captureDispatch[i] = ResolveCaptureHandler(captureName);
        m_validationDispatch[i] = ResolveValidationHandler(captureName);
    }
}

SymbolCollector::~SymbolCollector() = default;

std::vector<Diagnostic> SymbolCollector::CollectSymbols(const SymbolCollectRequest& request,
                                                        angel_lsp::parser::AngelScriptParser& parser,
                                                        SymbolTable& symbolTable)
{
    std::vector<Diagnostic> diagnostics;
    TSTree* tree = parser.Parse(request.sourceCode);

    if (!tree)
        return diagnostics;

    TSNode rootNode = ts_tree_root_node(tree);
    SymbolCollectContext sCtx{request, symbolTable, diagnostics};
    CollectFromTree(rootNode, sCtx);
    symbolTable.ResolveIncludedMixins();

    ts_tree_delete(tree);
    return diagnostics;
}

std::vector<Diagnostic> SymbolCollector::CollectSymbols(const std::string& fileUri, const std::string& sourceCode,
                                                        angel_lsp::parser::AngelScriptParser& parser,
                                                        SymbolTable& symbolTable)
{
    return CollectSymbols(SymbolCollectRequest(fileUri, sourceCode), parser, symbolTable);
}

std::vector<Diagnostic> SymbolCollector::CollectSymbolsWithTree(const SymbolCollectRequest& request, TSTree* tree,
                                                                SymbolTable& symbolTable)
{
    std::vector<Diagnostic> diagnostics;
    if (!tree)
        return diagnostics;

    TSNode rootNode = ts_tree_root_node(tree);
    SymbolCollectContext sCtx{request, symbolTable, diagnostics};
    CollectFromTree(rootNode, sCtx);
    symbolTable.ResolveIncludedMixins();

    return diagnostics;
}

std::vector<Diagnostic> SymbolCollector::CollectSymbolsWithTree(const std::string& fileUri,
                                                                const std::string& sourceCode, TSTree* tree,
                                                                SymbolTable& symbolTable)
{
    return CollectSymbolsWithTree(SymbolCollectRequest(fileUri, sourceCode), tree, symbolTable);
}

// =========================================================================================
// AST Traversal & Scope Helpers
// =========================================================================================

void SymbolCollector::CollectFromTree(TSNode rootNode, SymbolCollectContext& sCtx)
{
    ReportParseErrors(rootNode, sCtx);

    TSQueryCursor* cursor = angel_lsp::parser::QueryRegistry::GetThreadLocalCursor();
    ts_query_cursor_exec(cursor, m_tagsQuery, rootNode);

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match))
    {
        for (uint32_t i = 0; i < match.capture_count; ++i)
        {
            uint32_t captureIdx = match.captures[i].index;
            if (captureIdx < m_captureDispatch.size() && m_captureDispatch[captureIdx])
            {
                CollectionContext ctx = BuildContext(match.captures[i].node, sCtx.request.sourceCode);
                (this->*m_captureDispatch[captureIdx])(match.captures[i].node, sCtx, ctx);
            }
            else if (captureIdx < m_validationDispatch.size() && m_validationDispatch[captureIdx])
            {
                (this->*m_validationDispatch[captureIdx])(match.captures[i].node, sCtx);
            }
        }
    }
}

SymbolCollector::CollectionContext SymbolCollector::BuildContext(TSNode node, std::string_view sourceCode) const
{
    CollectionContext ctx;
    TSNode current = ts_node_parent(node);

    while (!ts_node_is_null(current))
    {
        TSSymbol sym = ts_node_symbol(current);

        if (!ctx.isInsideFunction && (sym == m_symFuncDeclaration || sym == m_symStatementBlock))
        {
            ctx.isInsideFunction = true;
        }

        if (sym == m_symClassBody || sym == m_symInterfaceBody)
        {
            ctx.isInsideClass = true;
            TSNode parentDecl = ts_node_parent(current);
            TSNode nameNode = GetChildByFieldName(parentDecl, "name");
            std::string name = GetNodeText(nameNode, sourceCode);
            if (ctx.containerPath.empty())
            {
                ctx.containerPath = std::move(name);
            }
            else
            {
                std::string tmp;
                appendQualifiedName(tmp, name, ctx.containerPath);
                ctx.containerPath = std::move(tmp);
            }
        }
        else if (sym == m_symNamespaceBody)
        {
            ctx.isInsideNamespace = true;
            TSNode parentDecl = ts_node_parent(current);
            TSNode nameNode = GetChildByFieldName(parentDecl, "name");
            std::string name = GetNodeText(nameNode, sourceCode);
            if (ctx.containerPath.empty())
            {
                ctx.containerPath = std::move(name);
            }
            else
            {
                std::string tmp;
                appendQualifiedName(tmp, name, ctx.containerPath);
                ctx.containerPath = std::move(tmp);
            }
        }

        current = ts_node_parent(current);
    }
    return ctx;
}

// =========================================================================================
// Declaration Collectors
// =========================================================================================

void SymbolCollector::ApplyAccessorTokens(TSNode accNode, bool isGet, bool isSet, VariableSignature& varSig) const
{
    TSTreeCursor cursor = ts_tree_cursor_new(accNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            TSSymbol cSym = ts_node_symbol(child);
            if (cSym == m_tokConst && isGet)
            {
                varSig.isGetConst = true;
            }
            else if (cSym == m_tokOverride)
            {
                if (isGet)
                    varSig.isGetOverride = true;
                else if (isSet)
                    varSig.isSetOverride = true;
            }
            else if (cSym == m_tokFinal)
            {
                if (isGet)
                    varSig.isGetFinal = true;
                else if (isSet)
                    varSig.isSetFinal = true;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

void SymbolCollector::ParseSingleAccessor(TSNode accNode, VariableSignature& varSig) const
{
    TSNode kindNode = GetChildByFieldName(accNode, "kind");
    TSNode bodyNode = GetChildByFieldName(accNode, "body");
    TSSymbol kindSym = !ts_node_is_null(kindNode) ? ts_node_symbol(kindNode) : 0;
    bool isGet = (kindSym == m_symGet);
    bool isSet = (kindSym == m_symSet);
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

void SymbolCollector::ParseAccessorNodes(TSNode node, VariableSignature& varSig) const
{
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode accNode = ts_tree_cursor_current_node(&cursor);
            if (ts_node_is_named(accNode) && ts_node_symbol(accNode) == m_symAccessor)
            {
                ParseSingleAccessor(accNode, varSig);
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

void SymbolCollector::ProcessVirtualPropertyVariable(TSNode varDeclNode, SymbolCollectContext& sCtx,
                                                     const CollectionContext& ctx)
{
    TSNode typeNode = GetChildByFieldName(varDeclNode, "prop_type");
    TSNode nameNode = GetChildByFieldName(varDeclNode, "name");
    std::string typeStr = GetNodeText(typeNode, sCtx.request.sourceCode);
    TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, sCtx.request.sourceCode);
    SymbolModifiers modifiers = ExtractModifiers(varDeclNode, sCtx.request.sourceCode);
    modifiers.isHandle = typeInfo.isHandle || modifiers.isHandle;

    SymbolLocationContext loc{sCtx.request.sourceCode, sCtx.request.fileUri, ctx.containerPath};
    Symbol sym = CreateSymbol(SymbolType::Property, varDeclNode, nameNode, loc);

    VariableSignature varSig;
    varSig.typeName = typeStr;
    varSig.baseTypeName = typeInfo.baseTypeName;
    varSig.templateName = typeInfo.templateName;
    varSig.typeKind = typeInfo.kind;
    varSig.isArray = typeInfo.isArray;
    varSig.hasPrimitiveHandle = typeInfo.hasPrimitiveHandle;
    varSig.arrayDepth = typeInfo.arrayDepth;
    varSig.defaultValue = GetNodeText(varDeclNode, sCtx.request.sourceCode);
    varSig.modifiers = modifiers;
    varSig.isVirtualProperty = true;

    ParseAccessorNodes(varDeclNode, varSig);

    sym.signature = varSig;
    sCtx.symbolTable.AddSymbol(sym);
}

static TSNode FindDeclaratorValueNode(TSNode declaratorNode, std::string_view sourceCode)
{
    TSNode valueNode = parser::GetChildByField(declaratorNode, parser::fields::Value);
    if (!ts_node_is_null(valueNode))
    {
        return valueNode;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(declaratorNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        bool foundEq = false;
        do
        {
            TSNode ch = ts_tree_cursor_current_node(&cursor);
            const uint32_t start = ts_node_start_byte(ch);
            const uint32_t end = ts_node_end_byte(ch);
            if (start < end && end <= sourceCode.size())
            {
                std::string_view text = sourceCode.substr(start, end - start);
                while (!text.empty() && isspace(static_cast<unsigned char>(text.front())))
                    text.remove_prefix(1);
                while (!text.empty() && isspace(static_cast<unsigned char>(text.back())))
                    text.remove_suffix(1);
                if (text == "=")
                {
                    foundEq = true;
                }
                else if (foundEq)
                {
                    valueNode = ch;
                    break;
                }
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return valueNode;
}

void SymbolCollector::CollectDeclaratorSymbol(TSNode declaratorNode, const VariableHeaderInfo& header,
                                              SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode varDeclNode = ts_node_parent(declaratorNode);
    TSNode nameNode = GetChildByFieldName(declaratorNode, "name");
    TSNode valueNode = FindDeclaratorValueNode(declaratorNode, sCtx.request.sourceCode);

    SymbolLocationContext loc{sCtx.request.sourceCode, sCtx.request.fileUri, ctx.containerPath};
    Symbol sym = CreateSymbol(SymbolType::Variable, varDeclNode, nameNode, loc);

    VariableSignature varSig;
    varSig.typeName = header.typeStr;
    varSig.baseTypeName = header.typeInfo.baseTypeName;
    varSig.templateName = header.typeInfo.templateName;
    for (const auto& tArg : header.typeInfo.templateArguments)
    {
        varSig.templateArgumentTypes.push_back(tArg.baseTypeName);
    }
    varSig.typeKind = header.typeInfo.kind;
    varSig.isArray = header.typeInfo.isArray;
    varSig.hasPrimitiveHandle = header.typeInfo.hasPrimitiveHandle;
    varSig.arrayDepth = header.typeInfo.arrayDepth;
    varSig.defaultValue = GetNodeText(valueNode, sCtx.request.sourceCode);
    varSig.hasNullInitializer = IsNullInitializer(valueNode);
    varSig.hasSemicolon = header.hasSemicolon;
    varSig.modifiers = header.modifiers;
    if (header.typeInfo.isConst)
    {
        varSig.modifiers.isConst = true;
    }

    sym.signature = varSig;
    sCtx.symbolTable.AddSymbol(sym);

    if (!ctx.isInsideFunction)
    {
        TSNode argsNode = GetChildByFieldName(declaratorNode, "arguments");
        if (!ts_node_is_null(argsNode) && !header.typeStr.empty())
        {
            CallReferenceSignature callSig;
            callSig.calleeName = header.typeStr;
            SymbolLocationContext callLoc{sCtx.request.sourceCode, sCtx.request.fileUri, ctx.containerPath};
            Symbol callSym = CreateSymbol(SymbolType::CallReference, declaratorNode, declaratorNode, callLoc);
            callSym.signature = callSig;
            sCtx.symbolTable.AddSymbol(callSym);
        }
    }
}

void SymbolCollector::ProcessRegularVariable(TSNode varDeclNode, SymbolCollectContext& sCtx,
                                             const CollectionContext& ctx)
{
    TSNode typeNode = GetChildByFieldName(varDeclNode, "var_type");
    std::string typeStr = GetNodeText(typeNode, sCtx.request.sourceCode);
    TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, sCtx.request.sourceCode);
    SymbolModifiers modifiers = ExtractModifiers(varDeclNode, sCtx.request.sourceCode);
    modifiers.isHandle = typeInfo.isHandle || modifiers.isHandle;
    modifiers.isReturnReference = typeInfo.isReference || modifiers.isReturnReference;

    std::string_view rawVarText = TrimView(GetNodeView(varDeclNode, sCtx.request.sourceCode));
    bool hasSemicolon = (!rawVarText.empty() && rawVarText.back() == ';');

    VariableHeaderInfo header{std::move(typeStr), std::move(typeInfo), modifiers, hasSemicolon};

    TSTreeCursor cursor = ts_tree_cursor_new(varDeclNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode declaratorNode = ts_tree_cursor_current_node(&cursor);
            if (ts_node_is_named(declaratorNode) && ts_node_symbol(declaratorNode) == m_symVariableDeclarator)
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

    if (ts_node_symbol(varDeclNode) == m_symVirtualProperty)
    {
        ProcessVirtualPropertyVariable(varDeclNode, sCtx, ctx);
    }
    else
    {
        ProcessRegularVariable(varDeclNode, sCtx, ctx);
    }
}

TSNode SymbolCollector::FindFunctionBody(TSNode funcNode) const
{
    TSNode bodyNode = GetChildByFieldName(funcNode, "body");
    if (!ts_node_is_null(bodyNode))
        return bodyNode;

    TSTreeCursor cursor = ts_tree_cursor_new(funcNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode ch = ts_tree_cursor_current_node(&cursor);
            if (ts_node_symbol(ch) == m_symStatementBlock)
            {
                ts_tree_cursor_delete(&cursor);
                return ch;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return bodyNode;
}

bool SymbolCollector::IsExternalFunction(TSNode funcNode) const
{
    TSSymbol funcNodeSym = ts_node_symbol(funcNode);
    if (funcNodeSym == m_symImportDeclaration || funcNodeSym == m_tokImport)
        return true;

    TSNode p2 = ts_node_parent(funcNode);
    TSSymbol p2Sym = !ts_node_is_null(p2) ? ts_node_symbol(p2) : 0;
    return (p2Sym == m_symImportDeclaration || p2Sym == m_tokImport);
}

std::string SymbolCollector::ExtractOriginModule(TSNode funcNode, std::string_view sourceCode) const
{
    TSTreeCursor cursor = ts_tree_cursor_new(funcNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode ch = ts_tree_cursor_current_node(&cursor);
            if (std::string_view(ts_node_type(ch)) == "string_literal")
            {
                std::string modStr = GetNodeText(ch, sourceCode);
                ts_tree_cursor_delete(&cursor);
                if (modStr.size() >= 2 && modStr.front() == '"' && modStr.back() == '"')
                {
                    return modStr.substr(1, modStr.size() - 2);
                }
                return modStr;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return "";
}

void SymbolCollector::ProcessFunction(TSNode funcNode, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode nameNode = GetChildByFieldName(funcNode, "name");
    TSNode typeNode = GetChildByFieldName(funcNode, "return_type");
    TSNode paramsNode = GetChildByFieldName(funcNode, "parameters");
    TSNode bodyNode = FindFunctionBody(funcNode);

    TypeExtractionResult retInfo = ExtractTypeInfoFromAST(typeNode, sCtx.request.sourceCode);
    SymbolModifiers modifiers = ExtractModifiers(funcNode, sCtx.request.sourceCode);

    if (IsExternalFunction(funcNode))
        modifiers.isExternal = true;

    modifiers.isHandle = retInfo.isHandle || modifiers.isHandle;
    modifiers.isReturnReference = retInfo.isReference || modifiers.isReturnReference;

    TSTreeCursor delCursor = ts_tree_cursor_new(funcNode);
    if (ts_tree_cursor_goto_first_child(&delCursor))
    {
        do
        {
            if (ts_node_symbol(ts_tree_cursor_current_node(&delCursor)) == m_tokDelete)
            {
                modifiers.isDelete = true;
                break;
            }
        } while (ts_tree_cursor_goto_next_sibling(&delCursor));
    }
    ts_tree_cursor_delete(&delCursor);

    SymbolLocationContext loc{sCtx.request.sourceCode, sCtx.request.fileUri, ctx.containerPath};
    Symbol sym = CreateSymbol(SymbolType::Function, funcNode, nameNode, loc);

    FunctionSignature funcSig;
    funcSig.returnType = GetNodeText(typeNode, sCtx.request.sourceCode);
    funcSig.returnBaseTypeName = retInfo.baseTypeName;
    funcSig.returnTemplateName = retInfo.templateName;
    funcSig.returnTypeKind = retInfo.kind;
    funcSig.returnIsArray = retInfo.isArray;
    funcSig.returnIsConst = retInfo.isConst;
    funcSig.returnHasPrimitiveHandle = retInfo.hasPrimitiveHandle;
    funcSig.returnArrayDepth = retInfo.arrayDepth;
    funcSig.modifiers = modifiers;
    funcSig.parameters = ExtractParameters(paramsNode, sCtx.request.sourceCode);
    funcSig.hasBody = !ts_node_is_null(bodyNode);
    funcSig.isInterfaceMethod = (ts_node_symbol(funcNode) == m_symInterfaceMethod);
    funcSig.isImported = (ts_node_symbol(funcNode) == m_symImportDeclaration ||
                          std::string_view(ts_node_type(funcNode)) == "import_declaration");
    if (funcSig.isImported)
    {
        funcSig.originModule = ExtractOriginModule(funcNode, sCtx.request.sourceCode);
    }

    sym.signature = funcSig;
    sCtx.symbolTable.AddSymbol(sym);
}

void SymbolCollector::ProcessClass(TSNode classNode, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode nameNode = GetChildByFieldName(classNode, "name");
    SymbolLocationContext loc{sCtx.request.sourceCode, sCtx.request.fileUri, ctx.containerPath};
    Symbol sym = CreateSymbol(SymbolType::Class, classNode, nameNode, loc);

    ClassSignature classSig;
    classSig.modifiers = ExtractModifiers(classNode, sCtx.request.sourceCode);
    if (ts_node_symbol(classNode) == m_symMixinDeclaration)
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
                    std::string paramName = GetNodeText(child, sCtx.request.sourceCode);
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
        sym.qualifiedName = ctx.containerPath.empty() ? sym.name : ctx.containerPath + "::" + sym.name;
    }

    classSig.hasBraces = !ts_node_is_null(GetChildByFieldName(classNode, "body"));

    sym.signature = classSig;
    sCtx.symbolTable.AddSymbol(sym);
}

void SymbolCollector::ProcessNamespace(TSNode namespaceNode, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode nameNode = GetChildByFieldName(namespaceNode, "name");
    SymbolLocationContext loc{sCtx.request.sourceCode, sCtx.request.fileUri, ctx.containerPath};
    Symbol sym = CreateSymbol(SymbolType::Namespace, namespaceNode, nameNode, loc);
    sCtx.symbolTable.AddSymbol(sym);
}

void SymbolCollector::ProcessTypedef(TSNode node, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode nameNode = GetChildByFieldName(node, "name");
    TSNode baseTypeNode = GetChildByFieldName(node, "base_type");
    if (ts_node_is_null(baseTypeNode))
    {
        baseTypeNode = GetChildByFieldName(node, "type");
    }

    SymbolLocationContext loc{sCtx.request.sourceCode, sCtx.request.fileUri, ctx.containerPath};
    Symbol sym = CreateSymbol(SymbolType::Typedef, node, nameNode, loc);
    TypedefSignature typedefSig;

    if (!ts_node_is_null(baseTypeNode))
    {
        TypeExtractionResult info = ExtractTypeInfoFromAST(baseTypeNode, sCtx.request.sourceCode);
        typedefSig.baseType = info.baseTypeName;
        typedefSig.typeKind = info.kind;

        TSPoint startPoint = ts_node_start_point(baseTypeNode);
        TSPoint endPoint = ts_node_end_point(baseTypeNode);
        typedefSig.baseTypeStartLine = startPoint.row;
        typedefSig.baseTypeStartCharacter = startPoint.column;
        typedefSig.baseTypeEndLine = endPoint.row;
        typedefSig.baseTypeEndCharacter = endPoint.column;
    }

    std::string_view rawNodeText = TrimView(GetNodeView(node, sCtx.request.sourceCode));
    typedefSig.hasSemicolon = (!rawNodeText.empty() && rawNodeText.back() == ';');

    sym.signature = typedefSig;
    sCtx.symbolTable.AddSymbol(sym);
}

void SymbolCollector::ProcessFuncdef(TSNode node, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode nameNode = GetChildByFieldName(node, "name");
    TSNode typeNode = GetChildByFieldName(node, "return_type");
    TSNode paramsNode = GetChildByFieldName(node, "parameters");

    TypeExtractionResult retInfo = ExtractTypeInfoFromAST(typeNode, sCtx.request.sourceCode);
    SymbolModifiers modifiers = ExtractModifiers(node, sCtx.request.sourceCode);
    modifiers.isHandle = retInfo.isHandle || modifiers.isHandle;
    modifiers.isReturnReference = retInfo.isReference || modifiers.isReturnReference;
    modifiers.isConst = retInfo.isConst || modifiers.isConst;

    SymbolLocationContext loc{sCtx.request.sourceCode, sCtx.request.fileUri, ctx.containerPath};
    Symbol sym = CreateSymbol(SymbolType::Funcdef, node, nameNode, loc);

    FuncdefSignature funcdefSig;
    funcdefSig.returnType = GetNodeText(typeNode, sCtx.request.sourceCode);
    funcdefSig.returnBaseTypeName = retInfo.baseTypeName;
    funcdefSig.returnTemplateName = retInfo.templateName;
    funcdefSig.returnTypeKind = retInfo.kind;
    funcdefSig.returnIsArray = retInfo.isArray;
    funcdefSig.returnArrayDepth = retInfo.arrayDepth;
    funcdefSig.returnHasPrimitiveHandle = retInfo.hasPrimitiveHandle;
    funcdefSig.modifiers = modifiers;
    funcdefSig.parameters = ExtractParameters(paramsNode, sCtx.request.sourceCode);
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
    sCtx.symbolTable.AddSymbol(sym);
}

bool SymbolCollector::EnumHasBraces(TSNode node) const
{
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            if (ts_node_symbol(ts_tree_cursor_current_node(&cursor)) == m_tokOpenBrace)
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
            if (ts_node_is_named(child) && ts_node_symbol(child) == m_symEnumMember)
            {
                TSNode memberName = GetChildByFieldName(child, "name");
                TSNode memberValue = GetChildByFieldName(child, "value");

                EnumMemberInformation member;
                member.name = GetNodeText(memberName, sourceCode);
                member.value = GetNodeText(memberValue, sourceCode);
                if (!ts_node_is_null(memberValue))
                {
                    member.valueNodeType = ts_node_type(memberValue);
                }
                enumSig.members.push_back(std::move(member));
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

void SymbolCollector::PublishEnumMembers(TSNode node, const EnumSignature& enumSig, SymbolCollectContext& sCtx,
                                         const CollectionContext& ctx)
{
    TSNode nameNode = GetChildByFieldName(node, "name");
    std::string enumName = GetNodeText(nameNode, sCtx.request.sourceCode);

    ankerl::unordered_dense::map<std::string, MemberNodes> memberNodeMap;
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            if (ts_node_is_named(child) && ts_node_symbol(child) == m_symEnumMember)
            {
                TSNode mNameNode = GetChildByFieldName(child, "name");
                std::string mName = GetNodeText(mNameNode, sCtx.request.sourceCode);
                if (!mName.empty())
                {
                    memberNodeMap[mName] = MemberNodes{child, mNameNode};
                }
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);

    std::string enumContainer;
    appendQualifiedName(enumContainer, ctx.containerPath, enumName);
    for (const auto& m : enumSig.members)
    {
        if (m.name.empty())
            continue;

        VariableSignature varSig;
        varSig.typeName = enumContainer;
        varSig.defaultValue = m.value;
        varSig.modifiers.isConst = true;

        TSNode memberDeclNode = node;
        TSNode memberNameNode = nameNode;
        auto it = memberNodeMap.find(m.name);
        if (it != memberNodeMap.end())
        {
            memberDeclNode = it->second.declNode;
            memberNameNode = it->second.nameNode;
        }

        SymbolLocationContext contLoc{sCtx.request.sourceCode, sCtx.request.fileUri, ctx.containerPath};
        Symbol mSym = CreateSymbol(SymbolType::Variable, memberDeclNode, memberNameNode, contLoc);
        mSym.name = m.name;
        appendQualifiedName(mSym.qualifiedName, ctx.containerPath, m.name);
        mSym.containerName = ctx.containerPath;
        mSym.signature = varSig;
        sCtx.symbolTable.AddSymbol(mSym);

        SymbolLocationContext enumLoc{sCtx.request.sourceCode, sCtx.request.fileUri, enumContainer};
        Symbol mSymScoped = CreateSymbol(SymbolType::Variable, memberDeclNode, memberNameNode, enumLoc);
        mSymScoped.name = m.name;
        appendQualifiedName(mSymScoped.qualifiedName, enumContainer, m.name);
        mSymScoped.containerName = enumContainer;
        mSymScoped.signature = varSig;
        sCtx.symbolTable.AddSymbol(mSymScoped);
    }
}

void SymbolCollector::ProcessEnum(TSNode node, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode nameNode = GetChildByFieldName(node, "name");
    SymbolLocationContext loc{sCtx.request.sourceCode, sCtx.request.fileUri, ctx.containerPath};
    Symbol sym = CreateSymbol(SymbolType::Enum, node, nameNode, loc);

    EnumSignature enumSig;
    enumSig.modifiers = ExtractModifiers(node, sCtx.request.sourceCode);
    enumSig.hasBraces = EnumHasBraces(node);

    CollectEnumMembers(node, sCtx.request.sourceCode, enumSig);

    sym.signature = enumSig;
    sCtx.symbolTable.AddSymbol(sym);

    PublishEnumMembers(node, enumSig, sCtx, ctx);
}

void SymbolCollector::ProcessProperty(TSNode node, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode nameNode = GetChildByFieldName(node, "name");
    TSNode typeNode = GetChildByFieldName(node, "prop_type");

    TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, sCtx.request.sourceCode);
    SymbolModifiers modifiers = ExtractModifiers(node, sCtx.request.sourceCode);

    SymbolLocationContext loc{sCtx.request.sourceCode, sCtx.request.fileUri, ctx.containerPath};
    Symbol sym = CreateSymbol(SymbolType::Property, node, nameNode, loc);

    VariableSignature varSig;
    varSig.typeName = GetNodeText(typeNode, sCtx.request.sourceCode);
    varSig.baseTypeName = typeInfo.baseTypeName;
    varSig.templateName = typeInfo.templateName;
    varSig.typeKind = typeInfo.kind;
    varSig.isArray = typeInfo.isArray;
    varSig.hasPrimitiveHandle = typeInfo.hasPrimitiveHandle;
    varSig.arrayDepth = typeInfo.arrayDepth;
    varSig.modifiers = modifiers;
    varSig.defaultValue = GetNodeText(node, sCtx.request.sourceCode);
    varSig.isVirtualProperty = true;

    ParseAccessorNodes(node, varSig);

    sym.signature = varSig;
    sCtx.symbolTable.AddSymbol(sym);
}

void SymbolCollector::ProcessInterface(TSNode node, SymbolCollectContext& sCtx, const CollectionContext& ctx)
{
    TSNode nameNode = GetChildByFieldName(node, "name");
    SymbolLocationContext loc{sCtx.request.sourceCode, sCtx.request.fileUri, ctx.containerPath};
    Symbol sym = CreateSymbol(SymbolType::Interface, node, nameNode, loc);

    InterfaceSignature ifaceSig;
    ifaceSig.modifiers = ExtractModifiers(node, sCtx.request.sourceCode);
    ifaceSig.inheritedInterfaces = ExtractBases(node, sCtx.request.sourceCode);

    sym.signature = ifaceSig;
    sCtx.symbolTable.AddSymbol(sym);
}

// =========================================================================================
// Reference & Out-of-Body Call Collectors
// =========================================================================================

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

    if (functionNodeSym == m_symMemberExpression)
    {
        TSNode memberNode = GetChildByFieldName(functionNode, "member");
        if (ts_node_is_null(memberNode))
            return;

        callSig.isMethodCall = true;
        callSig.objectExpression = GetNodeText(GetChildByFieldName(functionNode, "object"), sCtx.request.sourceCode);
        nameNode = memberNode;
    }
    else if (functionNodeSym != m_symScopedIdentifier)
    {
        return;
    }

    callSig.calleeName = GetNodeText(nameNode, sCtx.request.sourceCode);
    if (callSig.calleeName.empty())
        return;

    SymbolLocationContext loc{sCtx.request.sourceCode, sCtx.request.fileUri, ctx.containerPath};
    Symbol sym = CreateSymbol(SymbolType::CallReference, callNode, nameNode, loc);
    sym.signature = callSig;
    sCtx.symbolTable.AddSymbol(sym);
}

// =========================================================================================
// Validation Collectors
// =========================================================================================

void SymbolCollector::CheckUsingDeclarationCapture(TSNode usingNode, SymbolCollectContext& sCtx) const
{
    TSNode nameNode = GetChildByFieldName(usingNode, "name");
    std::string nameText = GetNodeText(nameNode, sCtx.request.sourceCode);
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
    std::string pattern = sCtx.request.i18n ? sCtx.request.i18n->GetMessage("as-err-reserved-keyword-name")
                                            : "Instead found reserved keyword '{}'.";
    diag.message = fmt::format(fmt::runtime(pattern), nameText);
    sCtx.diagnostics.push_back(diag);
}

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
            if (childSym != m_symDeclarationModifier && childSym != m_symSharedExternalModifier)
                continue;

            TSTreeCursor modCursor = ts_tree_cursor_new(child);
            if (ts_tree_cursor_goto_first_child(&modCursor))
            {
                do
                {
                    TSNode modTokNode = ts_tree_cursor_current_node(&modCursor);
                    std::string modText(GetNodeText(modTokNode, sCtx.request.sourceCode));
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
                        std::string pattern = sCtx.request.i18n
                                                  ? sCtx.request.i18n->GetMessage("as-err-attribute-repeated")
                                                  : "Attribute '{}' is informed multiple times.";
                        diag.message = fmt::format(fmt::runtime(pattern), modText);
                        sCtx.diagnostics.push_back(diag);
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

// =========================================================================================
// Diagnostics & Error Recovery
// =========================================================================================

void SymbolCollector::ReportParseErrors(TSNode rootNode, SymbolCollectContext& sCtx) const
{
    if (!ts_node_has_error(rootNode))
        return;

    TSTreeCursor cursor = ts_tree_cursor_new(rootNode);
    bool hasChild = true;

    while (hasChild)
    {
        TSNode current = ts_tree_cursor_current_node(&cursor);

        if (ts_node_is_missing(current) || ts_node_is_error(current))
        {
            EmitParseErrorDiagnostic(current, sCtx);
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                continue;
            }
            bool ascended = false;
            while (ts_tree_cursor_goto_parent(&cursor))
            {
                if (ts_tree_cursor_goto_next_sibling(&cursor))
                {
                    ascended = true;
                    break;
                }
            }
            if (!ascended)
                break;
            continue;
        }

        if (ts_node_has_error(current) && ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }

        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }

        bool ascended = false;
        while (ts_tree_cursor_goto_parent(&cursor))
        {
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                ascended = true;
                break;
            }
        }
        if (!ascended)
            break;
    }

    ts_tree_cursor_delete(&cursor);
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

    std::string logMsg;

    if (ts_node_is_missing(node))
    {
        std::string missingToken = ts_node_type(node);
        std::string pattern =
            sCtx.request.i18n ? sCtx.request.i18n->GetMessage("as-syntax-error-missing") : "Syntax error: missing '{}'";
        diag.message = fmt::format(fmt::runtime(pattern), missingToken);
        logMsg = diag.message;
    }
    else
    {
        std::string rawErrText = GetNodeText(node, sCtx.request.sourceCode);
        diag.message = FormatSyntaxErrorMessage(rawErrText, diag.code, sCtx.request.i18n);
        logMsg = diag.message;
    }

    sCtx.diagnostics.push_back(diag);

    if (m_logger)
    {
        m_logger->LogWarning(fmt::format("[Tree-sitter Error] {} en [L{}:C{}] (File: {})", logMsg, startPt.row + 1,
                                         startPt.column + 1, sCtx.request.fileUri));
    }
}

std::string SymbolCollector::FormatSyntaxErrorMessage(const std::string& rawErrText, std::string& outCode,
                                                      const angel_lsp::i18n::I18n* i18n) const
{
    std::string firstToken = ExtractFirstToken(rawErrText);

    if (firstToken == "shared")
    {
        outCode = "as-err-shared-not-allowed-on-entity";
        return i18n ? i18n->GetMessage("as-err-shared-not-allowed-on-entity")
                    : "The 'shared' modifier is not allowed on this entity";
    }
    if (firstToken.empty())
    {
        return i18n ? i18n->GetMessage("as-syntax-error-generic") : "Syntax error";
    }

    std::string pattern = i18n ? i18n->GetMessage("as-syntax-error") : "Syntax error: \"{}\"";
    return fmt::format(fmt::runtime(pattern), firstToken);
}

// =========================================================================================
// AST/Text Extraction Helpers
// =========================================================================================

std::string SymbolCollector::GetNodeText(TSNode node, std::string_view sourceCode) const
{
    if (ts_node_is_null(node))
        return "";

    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);

    if (start >= end || end > sourceCode.size())
        return "";

    return std::string(sourceCode.substr(start, end - start));
}

std::string_view SymbolCollector::GetNodeView(TSNode node, std::string_view sourceCode) const
{
    if (ts_node_is_null(node))
        return {};

    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);

    if (start >= end || end > sourceCode.size())
        return {};

    return std::string_view(sourceCode.data() + start, end - start);
}

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
    std::string_view childText = TrimView(GetNodeView(child, sourceCode));
    bool isDeclMod =
        (ts_node_symbol(child) == m_symDeclarationModifier || ts_node_symbol(child) == m_symSharedExternalModifier);
    bool isFuncAttr = (ts_node_symbol(child) == m_symFuncAttributes);

    ApplyModifierString(childText, modifiers, isFuncAttr);

    if (isDeclMod || isFuncAttr)
    {
        TSTreeCursor cursor = ts_tree_cursor_new(child);
        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            do
            {
                TSNode grandChild = ts_tree_cursor_current_node(&cursor);
                std::string_view gcText = TrimView(GetNodeView(grandChild, sourceCode));
                ApplyModifierString(gcText, modifiers, isFuncAttr);

                TSSymbol tokSym = ts_node_symbol(grandChild);
                ApplyModifierToken(tokSym, modifiers);
                if (tokSym == m_tokFinal && !isFuncAttr)
                    modifiers.isDeclarationFinal = true;
                else if (tokSym == m_tokAbstract && !isFuncAttr)
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

    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            ProcessModifierChild(ts_tree_cursor_current_node(&cursor), sourceCode, modifiers);
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return modifiers;
}

bool SymbolCollector::ApplyParamModifierToken(TSSymbol tokenSymbol, SymbolModifiers& modifiers) const
{
    if (tokenSymbol == m_tokIn)
        modifiers.paramModifier = ParameterModifier::In;
    else if (tokenSymbol == m_tokOut)
        modifiers.paramModifier = ParameterModifier::Out;
    else if (tokenSymbol == m_tokInout)
        modifiers.paramModifier = ParameterModifier::InOut;
    else if (tokenSymbol == m_tokAmp)
        modifiers.isReturnReference = true;
    else if (tokenSymbol == m_tokAt)
        modifiers.isHandle = true;
    else
        return false;
    return true;
}

void SymbolCollector::ApplyModifierToken(TSSymbol tokenSymbol, SymbolModifiers& modifiers) const
{
    if (ApplyParamModifierToken(tokenSymbol, modifiers))
        return;

    if (tokenSymbol == m_tokConst)
        modifiers.isConst = true;
    else if (tokenSymbol == m_tokPrivate)
        modifiers.access = AccessModifier::Private;
    else if (tokenSymbol == m_tokProtected)
        modifiers.access = AccessModifier::Protected;
    else if (tokenSymbol == m_tokShared)
        modifiers.isShared = true;
    else if (tokenSymbol == m_tokMixin)
        modifiers.isMixin = true;
    else if (tokenSymbol == m_tokAbstract)
        modifiers.isAbstract = true;
    else if (tokenSymbol == m_tokFinal)
        modifiers.isFinal = true;
    else if (tokenSymbol == m_tokOverride)
        modifiers.isOverride = true;
    else if (tokenSymbol == m_tokExplicit)
        modifiers.isExplicit = true;
    else if (tokenSymbol == m_tokProperty)
        modifiers.isProperty = true;
    else if (tokenSymbol == m_tokDelete)
        modifiers.isDelete = true;
    else if (tokenSymbol == m_tokExternal || tokenSymbol == m_tokImport)
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
                std::string_view tok = GetNodeView(typeChild, sourceCode);
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
    TSTreeCursor cursor = ts_tree_cursor_new(paramNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            std::string_view tok = GetNodeView(child, sourceCode);
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

ParameterInformation SymbolCollector::ExtractParameterInfo(TSNode paramNode, std::string_view sourceCode) const
{
    TSNode pNameNode = GetChildByFieldName(paramNode, "name");
    TSNode pTypeNode = GetChildByFieldName(paramNode, "param_type");
    TSNode pDefaultNode = GetChildByFieldName(paramNode, "default_value");

    TypeExtractionResult pInfo = ExtractTypeInfoFromAST(pTypeNode, sourceCode);

    TSPoint startPt = ts_node_start_point(paramNode);
    TSPoint endPt = ts_node_end_point(paramNode);

    ParameterInformation paramInfo;
    paramInfo.name = GetNodeText(pNameNode, sourceCode);
    paramInfo.typeName = GetNodeText(pTypeNode, sourceCode);
    paramInfo.rawText = GetNodeText(paramNode, sourceCode);
    paramInfo.baseTypeName = pInfo.baseTypeName;
    paramInfo.templateName = pInfo.templateName;
    paramInfo.typeKind = pInfo.kind;
    paramInfo.isArray = pInfo.isArray;
    paramInfo.hasPrimitiveHandle = pInfo.hasPrimitiveHandle;
    paramInfo.arrayDepth = pInfo.arrayDepth;
    paramInfo.defaultValue = GetNodeText(pDefaultNode, sourceCode);
    paramInfo.isHandle = pInfo.isHandle;
    paramInfo.startLine = startPt.row;
    paramInfo.startCharacter = startPt.column;
    paramInfo.endLine = endPt.row;
    paramInfo.endCharacter = endPt.column;

    uint32_t refCount = 0;
    ExtractParamTypeRefAndConst(pTypeNode, sourceCode, paramInfo, refCount);
    ExtractParamModifierTokens(paramNode, sourceCode, paramInfo, refCount);

    paramInfo.hasDoubleReference = (refCount > 1);
    paramInfo.isStandaloneRef = (paramInfo.isReference && paramInfo.modifier == ParameterModifier::None);
    return paramInfo;
}

std::vector<ParameterInformation> SymbolCollector::ExtractParameters(TSNode paramsNode,
                                                                     std::string_view sourceCode) const
{
    std::vector<ParameterInformation> parameters;
    if (ts_node_is_null(paramsNode))
        return parameters;

    TSTreeCursor cursor = ts_tree_cursor_new(paramsNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            if (ts_node_is_named(child) && ts_node_symbol(child) == m_symParameter)
            {
                parameters.push_back(ExtractParameterInfo(child, sourceCode));
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
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

Symbol SymbolCollector::CreateSymbol(SymbolType type, TSNode node, TSNode nameNode,
                                     const SymbolLocationContext& loc) const
{
    TSPoint startPt = ts_node_start_point(node);
    TSPoint endPt = ts_node_end_point(node);
    TSPoint nameStartPt = ts_node_is_null(nameNode) ? startPt : ts_node_start_point(nameNode);
    TSPoint nameEndPt = ts_node_is_null(nameNode) ? endPt : ts_node_end_point(nameNode);

    Symbol sym;
    sym.type = type;
    sym.name = GetNodeText(nameNode, loc.sourceCode);
    sym.containerName = loc.containerPath;
    appendQualifiedName(sym.qualifiedName, loc.containerPath, sym.name);
    sym.fileUri = loc.fileUri;
    sym.startLine = startPt.row;
    sym.startCharacter = startPt.column;
    sym.endLine = endPt.row;
    sym.endCharacter = endPt.column;

    sym.fullRange = {startPt.row, startPt.column, endPt.row, endPt.column};
    sym.selectionRange = {nameStartPt.row, nameStartPt.column, nameEndPt.row, nameEndPt.column};

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
            if (ts_node_is_named(child) && ts_node_symbol(child) == m_symBaseClassList)
            {
                TSTreeCursor baseCursor = ts_tree_cursor_new(child);
                if (ts_tree_cursor_goto_first_child(&baseCursor))
                {
                    do
                    {
                        TSNode baseChild = ts_tree_cursor_current_node(&baseCursor);
                        if (ts_node_is_named(baseChild))
                        {
                            std::string text = GetNodeText(baseChild, sourceCode);
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
