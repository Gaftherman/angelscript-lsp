#include "analysis/SymbolCollector.h"
#include "analysis/SemanticHelpers.h"
#include "parser/queries/BuiltQueries.h"
#include "spdlog/fmt/fmt.h"

#include <cstring>
#include <cctype>
#include <unordered_set>

extern "C" const TSLanguage *tree_sitter_angelscript();

namespace angel_lsp::analysis
{
    // =========================================================================================
    // Public API
    // =========================================================================================

#define SYM_NAME(str) str, static_cast<uint32_t>(sizeof(str) - 1)

    SymbolCollector::SymbolCollector(angel_lsp::utils::LspLogger *logger)
        : m_logger(logger)
    {
        const TSLanguage *lang = tree_sitter_angelscript();

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
        m_symCompoundStatement = ts_language_symbol_for_name(lang, SYM_NAME("compound_statement"), true);
        m_symBlock = ts_language_symbol_for_name(lang, SYM_NAME("block"), true);
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
        m_tokPublic = ts_language_symbol_for_name(lang, SYM_NAME("public"), false);
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

        uint32_t errorOffset = 0;
        TSQueryError errorType = TSQueryErrorNone;
        m_tagsQuery = ts_query_new(lang, angel_lsp::parser::queries::TAGS_QUERY,
                                   static_cast<uint32_t>(strlen(angel_lsp::parser::queries::TAGS_QUERY)),
                                   &errorOffset, &errorType);

        if (!m_tagsQuery && m_logger)
        {
            m_logger->LogError(fmt::format("Error al compilar TAGS_QUERY en offset: {}", errorOffset));
        }

        if (m_tagsQuery)
        {
            uint32_t captureCount = ts_query_capture_count(m_tagsQuery);
            m_captureDispatch.assign(captureCount, nullptr);
            m_validationDispatch.assign(captureCount, nullptr);

            for (uint32_t i = 0; i < captureCount; ++i)
            {
                uint32_t nameLen = 0;
                const char *name = ts_query_capture_name_for_id(m_tagsQuery, i, &nameLen);
                std::string_view captureName(name, nameLen);

                if (captureName == "definition.function" || captureName == "definition.import" || captureName == "local.definition.import")
                    m_captureDispatch[i] = &SymbolCollector::ProcessFunction;
                else if (captureName == "definition.class")
                    m_captureDispatch[i] = &SymbolCollector::ProcessClass;
                else if (captureName == "definition.variable")
                    m_captureDispatch[i] = &SymbolCollector::ProcessVariable;
                else if (captureName == "definition.namespace")
                    m_captureDispatch[i] = &SymbolCollector::ProcessNamespace;
                else if (captureName == "definition.enum")
                    m_captureDispatch[i] = &SymbolCollector::ProcessEnum;
                else if (captureName == "definition.typedef")
                    m_captureDispatch[i] = &SymbolCollector::ProcessTypedef;
                else if (captureName == "definition.funcdef")
                    m_captureDispatch[i] = &SymbolCollector::ProcessFuncdef;
                else if (captureName == "definition.interface")
                    m_captureDispatch[i] = &SymbolCollector::ProcessInterface;
                else if (captureName == "definition.property")
                    m_captureDispatch[i] = &SymbolCollector::ProcessProperty;
                else if (captureName == "reference.call")
                    m_captureDispatch[i] = &SymbolCollector::ProcessCallReference;
                else if (captureName == "validation.using")
                    m_validationDispatch[i] = &SymbolCollector::CheckUsingDeclarationCapture;
                else if (captureName == "validation.modifiers")
                    m_validationDispatch[i] = &SymbolCollector::CheckDuplicateModifierGroup;
            }
        }
    }

    SymbolCollector::~SymbolCollector()
    {
        if (m_tagsQuery)
        {
            ts_query_delete(m_tagsQuery);
        }
    }

    std::vector<Diagnostic> SymbolCollector::CollectSymbols(const std::string &fileUri, const std::string &sourceCode, angel_lsp::parser::AngelScriptParser &parser, SymbolTable &symbolTable, const angel_lsp::i18n::I18n *i18n, const angel_lsp::config::TypeConfig *typeConfig)
    {
        std::vector<Diagnostic> diagnostics;
        TSTree *tree = parser.Parse(sourceCode);

        if (!tree)
            return diagnostics;

        TSNode rootNode = ts_tree_root_node(tree);
        CollectFromTree(rootNode, sourceCode, fileUri, symbolTable, i18n, diagnostics);

        ts_tree_delete(tree);
        return diagnostics;
    }

    std::vector<Diagnostic> SymbolCollector::CollectSymbolsWithTree(const std::string &fileUri, const std::string &sourceCode, TSTree *tree, SymbolTable &symbolTable, const angel_lsp::i18n::I18n *i18n, const angel_lsp::config::TypeConfig *typeConfig)
    {
        std::vector<Diagnostic> diagnostics;
        if (!tree)
            return diagnostics;

        TSNode rootNode = ts_tree_root_node(tree);
        CollectFromTree(rootNode, sourceCode, fileUri, symbolTable, i18n, diagnostics);

        return diagnostics;
    }

    // =========================================================================================
    // AST Traversal & Scope Helpers
    // =========================================================================================

    void SymbolCollector::CollectFromTree(TSNode rootNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const angel_lsp::i18n::I18n *i18n, std::vector<Diagnostic> &diagnostics)
    {
        // ReportParseErrors prunes via the O(1) ts_node_has_error() flag and only recurses into
        // subtrees that actually contain a syntax error, so it stays a manual walk. Using-declaration
        // and duplicate-modifier validation are driven by TAGS_QUERY captures below instead, so the
        // whole document is only ever walked once by the query engine for those checks.
        ReportParseErrors(rootNode, fileUri, sourceCode, diagnostics, i18n);

        TSQueryCursor *cursor = ts_query_cursor_new();
        ts_query_cursor_exec(cursor, m_tagsQuery, rootNode);

        TSQueryMatch match;
        while (ts_query_cursor_next_match(cursor, &match))
        {
            for (uint32_t i = 0; i < match.capture_count; ++i)
            {
                uint32_t captureIdx = match.captures[i].index;
                if (captureIdx < m_captureDispatch.size() && m_captureDispatch[captureIdx])
                {
                    CollectionContext ctx = BuildContext(match.captures[i].node, sourceCode);
                    (this->*m_captureDispatch[captureIdx])(match.captures[i].node, sourceCode, fileUri, symbolTable, ctx);
                }
                else if (captureIdx < m_validationDispatch.size() && m_validationDispatch[captureIdx])
                {
                    (this->*m_validationDispatch[captureIdx])(match.captures[i].node, sourceCode, fileUri, diagnostics, i18n);
                }
            }
        }

        ts_query_cursor_delete(cursor);
    }

    SymbolCollector::CollectionContext SymbolCollector::BuildContext(TSNode node, const std::string &sourceCode) const
    {
        CollectionContext ctx;
        TSNode current = ts_node_parent(node);

        while (!ts_node_is_null(current))
        {
            TSSymbol sym = ts_node_symbol(current);

            if (!ctx.isInsideFunction &&
                (sym == m_symFuncDeclaration || sym == m_symStatementBlock))
            {
                ctx.isInsideFunction = true;
            }

            if (sym == m_symClassBody || sym == m_symInterfaceBody)
            {
                ctx.isInsideClass = true;
                TSNode parentDecl = ts_node_parent(current);
                TSNode nameNode = GetChildByFieldName(parentDecl, "name");
                std::string name = GetNodeText(nameNode, sourceCode);
                ctx.containerPath = ctx.containerPath.empty() ? name : name + "::" + ctx.containerPath;
            }
            else if (sym == m_symNamespaceBody)
            {
                ctx.isInsideNamespace = true;
                TSNode parentDecl = ts_node_parent(current);
                TSNode nameNode = GetChildByFieldName(parentDecl, "name");
                std::string name = GetNodeText(nameNode, sourceCode);
                ctx.containerPath = ctx.containerPath.empty() ? name : name + "::" + ctx.containerPath;
            }

            current = ts_node_parent(current);
        }
        return ctx;
    }

    // =========================================================================================
    // Declaration Collectors
    // =========================================================================================

    void SymbolCollector::ProcessVariable(TSNode varDeclNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        if (ctx.isInsideFunction)
            return;

        if (ts_node_symbol(varDeclNode) == m_symVirtualProperty)
        {
            TSNode typeNode = GetChildByFieldName(varDeclNode, "prop_type");
            TSNode nameNode = GetChildByFieldName(varDeclNode, "name");
            std::string typeStr = GetNodeText(typeNode, sourceCode);
            TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, sourceCode);
            SymbolModifiers modifiers = ExtractModifiers(varDeclNode, sourceCode);
            modifiers.isHandle = typeInfo.isHandle || modifiers.isHandle;

            Symbol sym = CreateSymbol(SymbolType::Property, varDeclNode, nameNode, sourceCode, fileUri, ctx.containerPath);
            VariableSignature varSig;
            varSig.typeName = typeStr;
            varSig.baseTypeName = typeInfo.baseTypeName;
            varSig.templateName = typeInfo.templateName;
            varSig.typeKind = typeInfo.kind;
            varSig.isArray = typeInfo.isArray;
            varSig.hasPrimitiveHandle = typeInfo.hasPrimitiveHandle;
            varSig.arrayDepth = typeInfo.arrayDepth;
            varSig.defaultValue = GetNodeText(varDeclNode, sourceCode);
            varSig.modifiers = modifiers;
            varSig.isVirtualProperty = true;

            uint32_t count = ts_node_named_child_count(varDeclNode);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode accNode = ts_node_named_child(varDeclNode, i);
                if (ts_node_symbol(accNode) == m_symAccessor)
                {
                    TSNode kindNode = GetChildByFieldName(accNode, "kind");
                    TSNode bodyNode = GetChildByFieldName(accNode, "body");
                    TSSymbol kindSym = !ts_node_is_null(kindNode) ? ts_node_symbol(kindNode) : 0;
                    bool isGet = (kindSym == m_symGet);
                    bool isSet = (kindSym == m_symSet);
                    bool hasBody = !ts_node_is_null(bodyNode);
                    if (isGet)
                    {
                        if (varSig.hasGet) varSig.hasDuplicateGet = true;
                        varSig.hasGet = true;
                        if (hasBody) varSig.hasBodyGet = true;
                    }
                    else if (isSet)
                    {
                        if (varSig.hasSet) varSig.hasDuplicateSet = true;
                        varSig.hasSet = true;
                        if (hasBody) varSig.hasBodySet = true;
                    }

                    uint32_t accChildCount = ts_node_child_count(accNode);
                    for (uint32_t c = 0; c < accChildCount; ++c)
                    {
                        TSSymbol cSym = ts_node_symbol(ts_node_child(accNode, c));
                        if (cSym == m_tokConst && isGet) varSig.isGetConst = true;
                        else if (cSym == m_tokOverride) { if (isGet) varSig.isGetOverride = true; else if (isSet) varSig.isSetOverride = true; }
                        else if (cSym == m_tokFinal) { if (isGet) varSig.isGetFinal = true; else if (isSet) varSig.isSetFinal = true; }
                    }
                }
            }

            sym.signature = varSig;
            symbolTable.AddSymbol(sym);
            return;
        }

        TSNode typeNode = GetChildByFieldName(varDeclNode, "var_type");
        std::string typeStr = GetNodeText(typeNode, sourceCode);
        TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, sourceCode);
        SymbolModifiers modifiers = ExtractModifiers(varDeclNode, sourceCode);
        modifiers.isHandle = typeInfo.isHandle || modifiers.isHandle;
        modifiers.isReturnReference = typeInfo.isReference || modifiers.isReturnReference;

        std::string_view rawVarText = GetNodeView(varDeclNode, sourceCode);
        while (!rawVarText.empty() && isspace(static_cast<unsigned char>(rawVarText.back())))
        {
            rawVarText.remove_suffix(1);
        }
        bool hasSemicolon = (!rawVarText.empty() && rawVarText.back() == ';');

        uint32_t count = ts_node_named_child_count(varDeclNode);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode declaratorNode = ts_node_named_child(varDeclNode, i);
            if (ts_node_symbol(declaratorNode) != m_symVariableDeclarator)
                continue;

            TSNode nameNode = GetChildByFieldName(declaratorNode, "name");
            TSNode valueNode = GetChildByFieldName(declaratorNode, "value");
            if (ts_node_is_null(valueNode)) valueNode = GetChildByFieldName(declaratorNode, "initializer");
            if (ts_node_is_null(valueNode))
            {
                uint32_t cCnt = ts_node_child_count(declaratorNode);
                bool foundEq = false;
                for (uint32_t c = 0; c < cCnt; ++c)
                {
                    TSNode ch = ts_node_child(declaratorNode, c);
                    std::string chText = GetNodeText(ch, sourceCode);
                    if (chText == "=")
                    {
                        foundEq = true;
                    }
                    else if (foundEq)
                    {
                        valueNode = ch;
                        break;
                    }
                }
            }

            Symbol sym = CreateSymbol(SymbolType::Variable, varDeclNode, nameNode, sourceCode, fileUri, ctx.containerPath);
            VariableSignature varSig;
            varSig.typeName = typeStr;
            varSig.baseTypeName = typeInfo.baseTypeName;
            varSig.templateName = typeInfo.templateName;
            for (const auto &tArg : typeInfo.templateArguments)
            {
                varSig.templateArgumentTypes.push_back(tArg.baseTypeName);
            }
            varSig.typeKind = typeInfo.kind;
            varSig.isArray = typeInfo.isArray;
            varSig.hasPrimitiveHandle = typeInfo.hasPrimitiveHandle;
            varSig.arrayDepth = typeInfo.arrayDepth;
            varSig.defaultValue = GetNodeText(valueNode, sourceCode);
            varSig.hasNullInitializer = IsNullInitializer(valueNode);
            varSig.hasSemicolon = hasSemicolon;
            varSig.modifiers = modifiers;
            if (typeInfo.isConst)
            {
                varSig.modifiers.isConst = true;
            }

            sym.signature = varSig;
            symbolTable.AddSymbol(sym);
        }
    }

    void SymbolCollector::ProcessFunction(TSNode funcNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = GetChildByFieldName(funcNode, "name");
        TSNode typeNode = GetChildByFieldName(funcNode, "return_type");
        TSNode paramsNode = GetChildByFieldName(funcNode, "parameters");
        TSNode bodyNode = GetChildByFieldName(funcNode, "body");
        if (ts_node_is_null(bodyNode))
        {
            uint32_t cCount = ts_node_child_count(funcNode);
            for (uint32_t i = 0; i < cCount; ++i)
            {
                TSNode ch = ts_node_child(funcNode, i);
                TSSymbol chSym = ts_node_symbol(ch);
                if (chSym == m_symCompoundStatement || chSym == m_symStatementBlock || chSym == m_symBlock)
                {
                    bodyNode = ch;
                    break;
                }
            }
        }

        TypeExtractionResult retInfo = ExtractTypeInfoFromAST(typeNode, sourceCode);
        SymbolModifiers modifiers = ExtractModifiers(funcNode, sourceCode);

        TSSymbol funcNodeSym = ts_node_symbol(funcNode);
        if (funcNodeSym == m_symImportDeclaration || funcNodeSym == m_tokImport)
        {
            modifiers.isExternal = true;
        }
        else
        {
            TSNode p2 = ts_node_parent(funcNode);
            TSSymbol p2Sym = !ts_node_is_null(p2) ? ts_node_symbol(p2) : 0;
            if (p2Sym == m_symImportDeclaration || p2Sym == m_tokImport)
            {
                modifiers.isExternal = true;
            }
        }

        modifiers.isHandle = retInfo.isHandle || modifiers.isHandle;
        modifiers.isReturnReference = retInfo.isReference || modifiers.isReturnReference;

        uint32_t funcChildCount = ts_node_child_count(funcNode);
        for (uint32_t i = 0; i < funcChildCount; ++i)
        {
            TSNode child = ts_node_child(funcNode, i);
            if (ts_node_symbol(child) == m_tokDelete)
            {
                modifiers.isDelete = true;
            }
        }

        Symbol sym = CreateSymbol(SymbolType::Function, funcNode, nameNode, sourceCode, fileUri, ctx.containerPath);
        std::string funcText = GetNodeText(funcNode, sourceCode);
        FunctionSignature funcSig;
        funcSig.returnType = GetNodeText(typeNode, sourceCode);
        funcSig.returnBaseTypeName = retInfo.baseTypeName;
        funcSig.returnTemplateName = retInfo.templateName;
        funcSig.returnTypeKind = retInfo.kind;
        funcSig.returnIsArray = retInfo.isArray;
        funcSig.returnIsConst = retInfo.isConst;
        funcSig.returnHasPrimitiveHandle = retInfo.hasPrimitiveHandle;
        funcSig.returnArrayDepth = retInfo.arrayDepth;
        funcSig.modifiers = modifiers;
        funcSig.parameters = ExtractParameters(paramsNode, sourceCode);
        funcSig.hasBody = !ts_node_is_null(bodyNode);
        funcSig.defaultValue = funcSig.hasBody ? GetNodeText(bodyNode, sourceCode) : funcText;
        funcSig.isInterfaceMethod = (ts_node_symbol(funcNode) == m_symInterfaceMethod);

        sym.signature = funcSig;
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessClass(TSNode classNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = GetChildByFieldName(classNode, "name");
        Symbol sym = CreateSymbol(SymbolType::Class, classNode, nameNode, sourceCode, fileUri, ctx.containerPath);
        ClassSignature classSig;
        classSig.modifiers = ExtractModifiers(classNode, sourceCode);
        if (ts_node_symbol(classNode) == m_symMixinDeclaration)
        {
            classSig.modifiers.isMixin = true;
        }
        classSig.bases = ExtractBases(classNode, sourceCode);

        // Template parameters of `class array<T>`. The grammar gained the production in a61afd4;
        // before that this scanned for a "template_param" field the grammar never defined, so
        // isTemplate was permanently false and `<T>` reached here as an ERROR node.
        TSNode templateParams = GetChildByFieldName(classNode, "template_params");
        if (!ts_node_is_null(templateParams))
        {
            const uint32_t paramCount = ts_node_named_child_count(templateParams);
            for (uint32_t i = 0; i < paramCount; ++i)
            {
                std::string paramName = GetNodeText(ts_node_named_child(templateParams, i), sourceCode);
                if (!paramName.empty())
                {
                    classSig.isTemplate = true;
                    classSig.templateParams.push_back(std::move(paramName));
                }
            }
        }

        size_t angleInName = sym.name.find('<');
        if (angleInName != std::string::npos)
        {
            sym.name = sym.name.substr(0, angleInName);
            sym.qualifiedName = ctx.containerPath.empty() ? sym.name : ctx.containerPath + "::" + sym.name;
        }

        classSig.hasBraces = !ts_node_is_null(GetChildByFieldName(classNode, "body"));

        sym.signature = classSig;
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessNamespace(TSNode namespaceNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = GetChildByFieldName(namespaceNode, "name");
        Symbol sym = CreateSymbol(SymbolType::Namespace, namespaceNode, nameNode, sourceCode, fileUri, ctx.containerPath);
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessTypedef(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = GetChildByFieldName(node, "name");
        TSNode baseTypeNode = GetChildByFieldName(node, "base_type");

        Symbol sym = CreateSymbol(SymbolType::Typedef, node, nameNode, sourceCode, fileUri, ctx.containerPath);
        TypedefSignature typedefSig;

        if (!ts_node_is_null(baseTypeNode))
        {
            TypeExtractionResult info = ExtractTypeInfoFromAST(baseTypeNode, sourceCode);
            typedefSig.baseType = info.baseTypeName;
            typedefSig.typeKind = info.kind;
        }

        std::string_view rawNodeText = GetNodeView(node, sourceCode);
        while (!rawNodeText.empty() && isspace(static_cast<unsigned char>(rawNodeText.back())))
        {
            rawNodeText.remove_suffix(1);
        }
        typedefSig.hasSemicolon = (!rawNodeText.empty() && rawNodeText.back() == ';');

        sym.signature = typedefSig;
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessFuncdef(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = GetChildByFieldName(node, "name");
        TSNode typeNode = GetChildByFieldName(node, "return_type");
        TSNode paramsNode = GetChildByFieldName(node, "parameters");

        TypeExtractionResult retInfo = ExtractTypeInfoFromAST(typeNode, sourceCode);
        SymbolModifiers modifiers = ExtractModifiers(node, sourceCode);
        modifiers.isHandle = retInfo.isHandle || modifiers.isHandle;
        modifiers.isReturnReference = retInfo.isReference || modifiers.isReturnReference;
        modifiers.isConst = retInfo.isConst || modifiers.isConst;

        Symbol sym = CreateSymbol(SymbolType::Funcdef, node, nameNode, sourceCode, fileUri, ctx.containerPath);
        FuncdefSignature funcdefSig;
        funcdefSig.returnType = GetNodeText(typeNode, sourceCode);
        funcdefSig.returnBaseTypeName = retInfo.baseTypeName;
        funcdefSig.returnTemplateName = retInfo.templateName;
        funcdefSig.returnTypeKind = retInfo.kind;
        funcdefSig.returnIsArray = retInfo.isArray;
        funcdefSig.returnArrayDepth = retInfo.arrayDepth;
        funcdefSig.returnHasPrimitiveHandle = retInfo.hasPrimitiveHandle;
        funcdefSig.modifiers = modifiers;
        funcdefSig.parameters = ExtractParameters(paramsNode, sourceCode);

        sym.signature = funcdefSig;
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessEnum(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = GetChildByFieldName(node, "name");
        Symbol sym = CreateSymbol(SymbolType::Enum, node, nameNode, sourceCode, fileUri, ctx.containerPath);

        EnumSignature enumSig;
        enumSig.modifiers = ExtractModifiers(node, sourceCode);

        bool hasBraces = false;
        uint32_t cCount = ts_node_child_count(node);
        for (uint32_t c = 0; c < cCount; ++c)
        {
            if (ts_node_symbol(ts_node_child(node, c)) == m_tokOpenBrace)
            {
                hasBraces = true;
                break;
            }
        }
        enumSig.hasBraces = hasBraces;

        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_named_child(node, i);
            if (ts_node_symbol(child) != m_symEnumMember)
                continue;

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

        sym.signature = enumSig;
        symbolTable.AddSymbol(sym);

        struct MemberNodes
        {
            TSNode declNode;
            TSNode nameNode;
        };
        ankerl::unordered_dense::map<std::string, MemberNodes> memberNodeMap;
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_named_child(node, i);
            if (ts_node_symbol(child) == m_symEnumMember)
            {
                TSNode mNameNode = GetChildByFieldName(child, "name");
                std::string mName = GetNodeText(mNameNode, sourceCode);
                if (!mName.empty())
                {
                    memberNodeMap[mName] = MemberNodes{ child, mNameNode };
                }
            }
        }

        std::string enumContainer = ctx.containerPath.empty() ? sym.name : ctx.containerPath + "::" + sym.name;
        for (const auto &m : enumSig.members)
        {
            if (!m.name.empty())
            {
                VariableSignature varSig;
                varSig.typeName = sym.name;

                TSNode memberDeclNode = node;
                TSNode memberNameNode = nameNode;
                auto it = memberNodeMap.find(m.name);
                if (it != memberNodeMap.end())
                {
                    memberDeclNode = it->second.declNode;
                    memberNameNode = it->second.nameNode;
                }

                Symbol mSym = CreateSymbol(SymbolType::Variable, memberDeclNode, memberNameNode, sourceCode, fileUri, ctx.containerPath);
                mSym.name = m.name;
                mSym.qualifiedName = ctx.containerPath.empty() ? m.name : ctx.containerPath + "::" + m.name;
                mSym.containerName = ctx.containerPath;
                mSym.signature = varSig;
                symbolTable.AddSymbol(mSym);

                Symbol mSymScoped = CreateSymbol(SymbolType::Variable, memberDeclNode, memberNameNode, sourceCode, fileUri, enumContainer);
                mSymScoped.name = m.name;
                mSymScoped.qualifiedName = enumContainer + "::" + m.name;
                mSymScoped.containerName = enumContainer;
                mSymScoped.signature = varSig;
                symbolTable.AddSymbol(mSymScoped);
            }
        }
    }

    void SymbolCollector::ProcessProperty(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = GetChildByFieldName(node, "name");
        TSNode typeNode = GetChildByFieldName(node, "prop_type");

        TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, sourceCode);
        SymbolModifiers modifiers = ExtractModifiers(node, sourceCode);

        Symbol sym = CreateSymbol(SymbolType::Property, node, nameNode, sourceCode, fileUri, ctx.containerPath);
        VariableSignature varSig;
        varSig.typeName = GetNodeText(typeNode, sourceCode);
        varSig.baseTypeName = typeInfo.baseTypeName;
        varSig.templateName = typeInfo.templateName;
        varSig.typeKind = typeInfo.kind;
        varSig.isArray = typeInfo.isArray;
        varSig.hasPrimitiveHandle = typeInfo.hasPrimitiveHandle;
        varSig.arrayDepth = typeInfo.arrayDepth;
        varSig.modifiers = modifiers;
        varSig.defaultValue = GetNodeText(node, sourceCode);
        varSig.isVirtualProperty = true;

        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode accNode = ts_node_named_child(node, i);
            if (ts_node_symbol(accNode) == m_symAccessor)
            {
                TSNode kindNode = GetChildByFieldName(accNode, "kind");
                TSNode bodyNode = GetChildByFieldName(accNode, "body");
                TSSymbol kindSym = !ts_node_is_null(kindNode) ? ts_node_symbol(kindNode) : 0;
                bool isGet = (kindSym == m_symGet);
                bool isSet = (kindSym == m_symSet);
                bool hasBody = !ts_node_is_null(bodyNode);
                if (isGet)
                {
                    if (varSig.hasGet) varSig.hasDuplicateGet = true;
                    varSig.hasGet = true;
                    if (hasBody) varSig.hasBodyGet = true;
                }
                else if (isSet)
                {
                    if (varSig.hasSet) varSig.hasDuplicateSet = true;
                    varSig.hasSet = true;
                    if (hasBody) varSig.hasBodySet = true;
                }

                uint32_t accChildCount = ts_node_child_count(accNode);
                for (uint32_t c = 0; c < accChildCount; ++c)
                {
                    TSSymbol cSym = ts_node_symbol(ts_node_child(accNode, c));
                    if (cSym == m_tokConst && isGet) varSig.isGetConst = true;
                    else if (cSym == m_tokOverride) { if (isGet) varSig.isGetOverride = true; else if (isSet) varSig.isSetOverride = true; }
                    else if (cSym == m_tokFinal) { if (isGet) varSig.isGetFinal = true; else if (isSet) varSig.isSetFinal = true; }
                }
            }
        }

        sym.signature = varSig;
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessInterface(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = GetChildByFieldName(node, "name");
        Symbol sym = CreateSymbol(SymbolType::Interface, node, nameNode, sourceCode, fileUri, ctx.containerPath);
        InterfaceSignature ifaceSig;
        ifaceSig.modifiers = ExtractModifiers(node, sourceCode);
        ifaceSig.inheritedInterfaces = ExtractBases(node, sourceCode);

        sym.signature = ifaceSig;
        symbolTable.AddSymbol(sym);
    }

    // =========================================================================================
    // Reference & Out-of-Body Call Collectors
    // =========================================================================================

    void SymbolCollector::ProcessCallReference(TSNode callNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        // Only capture calls that live outside a function body (global/class/namespace
        // initializers, enum values, etc.) - calls inside statement blocks are handled
        // by the per-function body analysis instead.
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
            callSig.objectExpression = GetNodeText(GetChildByFieldName(functionNode, "object"), sourceCode);
            nameNode = memberNode;
        }
        else if (functionNodeSym != m_symScopedIdentifier)
        {
            // Call target is a computed expression (e.g. a lambda or an indexed value);
            // there is no stable name to record as a reference.
            return;
        }

        callSig.calleeName = GetNodeText(nameNode, sourceCode);
        if (callSig.calleeName.empty())
            return;

        Symbol sym = CreateSymbol(SymbolType::CallReference, callNode, nameNode, sourceCode, fileUri, ctx.containerPath);
        sym.signature = callSig;
        symbolTable.AddSymbol(sym);
    }

    // =========================================================================================
    // Validation Collectors
    // =========================================================================================

    void SymbolCollector::CheckUsingDeclarationCapture(TSNode usingNode, const std::string &sourceCode, const std::string &fileUri, std::vector<Diagnostic> &diagnostics, const angel_lsp::i18n::I18n *i18n) const
    {
        TSNode nameNode = GetChildByFieldName(usingNode, "name");
        std::string nameText = GetNodeText(nameNode, sourceCode);
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
        diag.fileUri = fileUri;
        std::string pattern = i18n ? i18n->GetMessage("as-err-reserved-keyword-name") : "Instead found reserved keyword '{}'.";
        diag.message = fmt::format(fmt::runtime(pattern), nameText);
        diagnostics.push_back(diag);
    }

    void SymbolCollector::CheckDuplicateModifierGroup(TSNode declNode, const std::string &sourceCode, const std::string &fileUri, std::vector<Diagnostic> &diagnostics, const angel_lsp::i18n::I18n *i18n) const
    {
        ankerl::unordered_dense::set<std::string> seenModifiers;
        uint32_t count = ts_node_child_count(declNode);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_child(declNode, i);
            TSSymbol childSym = ts_node_symbol(child);
            if (childSym != m_symDeclarationModifier && childSym != m_symSharedExternalModifier)
                continue;

            uint32_t modCount = ts_node_child_count(child);
            for (uint32_t m = 0; m < modCount; ++m)
            {
                TSNode modTokNode = ts_node_child(child, m);
                std::string modText(GetNodeText(modTokNode, sourceCode));
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
                    diag.fileUri = fileUri;
                    std::string pattern = i18n ? i18n->GetMessage("as-err-attribute-repeated") : "Attribute '{}' is informed multiple times.";
                    diag.message = fmt::format(fmt::runtime(pattern), modText);
                    diagnostics.push_back(diag);
                }
                else
                {
                    seenModifiers.insert(modText);
                }
            }
        }
    }

    // =========================================================================================
    // Diagnostics & Error Recovery
    // =========================================================================================

    void SymbolCollector::ReportParseErrors(TSNode node, const std::string &fileUri, const std::string &sourceCode, std::vector<Diagnostic> &diagnostics, const angel_lsp::i18n::I18n *i18n, int depth) const
    {
        if (!ts_node_has_error(node))
            return;
        if (depth >= 256)
            return;

        if (ts_node_is_missing(node) || ts_node_is_error(node))
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
            diag.fileUri = fileUri;

            std::string logMsg;

            if (ts_node_is_missing(node))
            {
                std::string missingToken = ts_node_type(node);
                std::string pattern = i18n ? i18n->GetMessage("as-syntax-error-missing") : "Syntax error: missing '{}'";
                diag.message = fmt::format(fmt::runtime(pattern), missingToken);
                logMsg = diag.message;
            }
            else
            {
                std::string rawErrText = GetNodeText(node, sourceCode);
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

                if (firstToken.empty())
                {
                    diag.message = i18n ? i18n->GetMessage("as-syntax-error-generic") : "Syntax error";
                }
                else
                {
                    std::string pattern = i18n ? i18n->GetMessage("as-syntax-error") : "Syntax error: \"{}\"";
                    diag.message = fmt::format(fmt::runtime(pattern), firstToken);
                }
                logMsg = diag.message;
            }

            diagnostics.push_back(diag);

            if (m_logger)
            {
                m_logger->LogWarning(fmt::format("[Tree-sitter Error] {} en [L{}:C{}] (File: {})",
                                                 logMsg, startPt.row + 1, startPt.column + 1, fileUri));
            }
            return;
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_child(node, i);
            if (ts_node_has_error(child))
            {
                ReportParseErrors(child, fileUri, sourceCode, diagnostics, i18n, depth + 1);
            }
        }
    }

    // =========================================================================================
    // AST/Text Extraction Helpers
    // =========================================================================================

    std::string SymbolCollector::GetNodeText(TSNode node, const std::string &sourceCode) const
    {
        if (ts_node_is_null(node))
            return "";

        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);

        if (start >= end || end > sourceCode.size())
            return "";

        return sourceCode.substr(start, end - start);
    }

    std::string_view SymbolCollector::GetNodeView(TSNode node, const std::string &sourceCode) const
    {
        if (ts_node_is_null(node))
            return {};

        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);

        if (start >= end || end > sourceCode.size())
            return {};

        return std::string_view(sourceCode.data() + start, end - start);
    }

    SymbolModifiers SymbolCollector::ExtractModifiers(TSNode node, const std::string &sourceCode) const
    {
        SymbolModifiers modifiers;
        if (ts_node_is_null(node))
            return modifiers;

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_child(node, i);

            if (ts_node_symbol(child) == m_symDeclarationModifier)
            {
                uint32_t modCount = ts_node_child_count(child);
                for (uint32_t m = 0; m < modCount; ++m)
                {
                    TSSymbol tokSym = ts_node_symbol(ts_node_child(child, m));
                    ApplyModifierToken(tokSym, modifiers);
                    // Track declaration-level modifiers separately from func_attributes
                    if (tokSym == m_tokFinal)
                        modifiers.isDeclarationFinal = true;
                    else if (tokSym == m_tokAbstract)
                        modifiers.isDeclarationAbstract = true;
                }
            }
            else if (ts_node_symbol(child) == m_symFuncAttributes ||
                     ts_node_symbol(child) == m_symSharedExternalModifier)
            {
                // shared_external_modifier is the strict subset the grammar gives to declarations
                // that admit neither 'abstract' nor 'final' - functions, funcdefs, enums and
                // interfaces. Without it here, `external void Think();` and `shared enum E` reached
                // the symbol table with no modifiers set at all.
                uint32_t modCount = ts_node_child_count(child);
                for (uint32_t m = 0; m < modCount; ++m)
                {
                    ApplyModifierToken(ts_node_symbol(ts_node_child(child, m)), modifiers);
                }
            }
            else if (!ts_node_is_named(child))
            {
                ApplyModifierToken(ts_node_symbol(child), modifiers);
            }
        }
        return modifiers;
    }

    void SymbolCollector::ApplyModifierToken(TSSymbol tokenSymbol, SymbolModifiers &modifiers) const
    {
        if (tokenSymbol == m_tokConst)
            modifiers.isConst = true;
        else if (tokenSymbol == m_tokIn)
            modifiers.paramModifier = ParameterModifier::In;
        else if (tokenSymbol == m_tokOut)
            modifiers.paramModifier = ParameterModifier::Out;
        else if (tokenSymbol == m_tokInout)
            modifiers.paramModifier = ParameterModifier::InOut;
        else if (tokenSymbol == m_tokAmp)
            modifiers.isReturnReference = true;
        else if (tokenSymbol == m_tokAt)
            modifiers.isHandle = true;
        else if (tokenSymbol == m_tokPrivate)
            modifiers.access = AccessModifier::Private;
        else if (tokenSymbol == m_tokProtected)
            modifiers.access = AccessModifier::Protected;
        else if (tokenSymbol == m_tokPublic)
            modifiers.access = AccessModifier::Public;
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

    ParameterInformation SymbolCollector::ExtractParameterInfo(TSNode paramNode, const std::string &sourceCode) const
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
        if (!ts_node_is_null(pTypeNode))
        {
            uint32_t typeChildCount = ts_node_child_count(pTypeNode);
            for (uint32_t i = 0; i < typeChildCount; ++i)
            {
                TSNode typeChild = ts_node_child(pTypeNode, i);
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
            }
        }

        uint32_t paramChildCount = ts_node_child_count(paramNode);
        for (uint32_t i = 0; i < paramChildCount; ++i)
        {
            TSNode child = ts_node_child(paramNode, i);

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
        }

        paramInfo.hasDoubleReference = (refCount > 1);
        paramInfo.isStandaloneRef = (paramInfo.isReference && paramInfo.modifier == ParameterModifier::None);
        return paramInfo;
    }

    std::vector<ParameterInformation> SymbolCollector::ExtractParameters(TSNode paramsNode, const std::string &sourceCode) const
    {
        std::vector<ParameterInformation> parameters;
        if (ts_node_is_null(paramsNode))
            return parameters;

        uint32_t paramCount = ts_node_named_child_count(paramsNode);
        parameters.reserve(paramCount);
        for (uint32_t p = 0; p < paramCount; ++p)
        {
            // Filtered by node type rather than taken as "every named child": comments are named
            // nodes too, so a trailing '/* ... */' inside the parentheses used to be collected as
            // an extra, empty parameter - inflating the arity of the declaration everywhere it is
            // shown or matched (hover, signature help, inlay hints, overload resolution).
            TSNode child = ts_node_named_child(paramsNode, p);
            if (ts_node_symbol(child) != m_symParameter)
                continue;

            parameters.push_back(ExtractParameterInfo(child, sourceCode));
        }
        if (parameters.size() == 1 && parameters[0].typeName == "void" && parameters[0].name.empty())
        {
            parameters.clear();
        }
        return parameters;
    }

    Symbol SymbolCollector::CreateSymbol(SymbolType type, TSNode node, TSNode nameNode,
                                          const std::string &sourceCode, const std::string &fileUri,
                                          const std::string &containerPath) const
    {
        TSPoint startPt = ts_node_start_point(node);
        TSPoint endPt = ts_node_end_point(node);
        TSPoint nameStartPt = ts_node_is_null(nameNode) ? startPt : ts_node_start_point(nameNode);
        TSPoint nameEndPt = ts_node_is_null(nameNode) ? endPt : ts_node_end_point(nameNode);

        Symbol sym;
        sym.type = type;
        sym.name = GetNodeText(nameNode, sourceCode);
        sym.containerName = containerPath;
        sym.qualifiedName = containerPath.empty() ? sym.name : containerPath + "::" + sym.name;
        sym.fileUri = fileUri;
        sym.startLine = startPt.row;
        sym.startCharacter = startPt.column;
        sym.endLine = endPt.row;
        sym.endCharacter = endPt.column;

        sym.fullRange = { startPt.row, startPt.column, endPt.row, endPt.column };
        sym.selectionRange = { nameStartPt.row, nameStartPt.column, nameEndPt.row, nameEndPt.column };

        return sym;
    }

    std::vector<std::string> SymbolCollector::ExtractBases(TSNode classNode, const std::string &sourceCode) const
    {
        std::vector<std::string> bases;

        uint32_t namedCount = ts_node_named_child_count(classNode);
        for (uint32_t i = 0; i < namedCount; ++i)
        {
            TSNode child = ts_node_named_child(classNode, i);
            if (ts_node_symbol(child) != m_symBaseClassList)
                continue;

            uint32_t baseCount = ts_node_named_child_count(child);
            bases.reserve(baseCount);
            for (uint32_t b = 0; b < baseCount; ++b)
            {
                std::string text = GetNodeText(ts_node_named_child(child, b), sourceCode);
                if (!text.empty())
                    bases.push_back(std::move(text));
            }
            break;
        }
        return bases;
    }
}
