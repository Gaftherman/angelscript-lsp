#include "analysis/SymbolCollector.h"
#include "parser/queries/BuiltQueries.h"
#include "spdlog/fmt/fmt.h"

#include <cstring>
#include <cctype>

extern "C" const TSLanguage *tree_sitter_angelscript();

namespace angel_lsp::analysis
{
    void ApplyModifierToken(const char *nodeType, SymbolModifiers &modifiers)
    {
        if (strcmp(nodeType, "const") == 0)
            modifiers.isConst = true;
        else if (strcmp(nodeType, "in") == 0)
            modifiers.paramModifier = ParameterModifier::In;
        else if (strcmp(nodeType, "out") == 0)
            modifiers.paramModifier = ParameterModifier::Out;
        else if (strcmp(nodeType, "inout") == 0)
            modifiers.paramModifier = ParameterModifier::InOut;
        else if (strcmp(nodeType, "&") == 0)
            modifiers.isReturnReference = true;
        else if (strcmp(nodeType, "@") == 0)
            modifiers.isHandle = true;
        else if (strcmp(nodeType, "private") == 0)
            modifiers.access = AccessModifier::Private;
        else if (strcmp(nodeType, "protected") == 0)
            modifiers.access = AccessModifier::Protected;
        else if (strcmp(nodeType, "public") == 0)
            modifiers.access = AccessModifier::Public;
        else if (strcmp(nodeType, "shared") == 0)
            modifiers.isShared = true;
        else if (strcmp(nodeType, "mixin") == 0)
            modifiers.isMixin = true;
        else if (strcmp(nodeType, "abstract") == 0)
            modifiers.isAbstract = true;
        else if (strcmp(nodeType, "final") == 0)
            modifiers.isFinal = true;
        else if (strcmp(nodeType, "override") == 0)
            modifiers.isOverride = true;
        else if (strcmp(nodeType, "explicit") == 0)
            modifiers.isExplicit = true;
        else if (strcmp(nodeType, "property") == 0)
            modifiers.isProperty = true;
        else if (strcmp(nodeType, "delete") == 0)
            modifiers.isDelete = true;
        else if (strcmp(nodeType, "external") == 0)
            modifiers.isExternal = true;
    }

    SymbolCollector::SymbolCollector(angel_lsp::utils::LspLogger *logger)
        : m_logger(logger)
    {
        const TSLanguage *lang = tree_sitter_angelscript();

        m_symPrimitiveType = ts_language_symbol_for_name(lang, "primitive_type", 14, true);
        m_symDatatype = ts_language_symbol_for_name(lang, "datatype", 8, true);
        m_symTemplateTypeList = ts_language_symbol_for_name(lang, "template_type_list", 18, true);
        m_symIdentifier = ts_language_symbol_for_name(lang, "identifier", 10, true);
        m_symDeclarationModifier = ts_language_symbol_for_name(lang, "declaration_modifier", 20, true);
        m_symType = ts_language_symbol_for_name(lang, "type", 4, true);
        m_symParameter = ts_language_symbol_for_name(lang, "parameter", 9, true);
        m_symClassBody = ts_language_symbol_for_name(lang, "class_body", 10, true);
        m_symNamespaceBody = ts_language_symbol_for_name(lang, "namespace_body", 14, true);
        m_symEnumMember = ts_language_symbol_for_name(lang, "enum_member", 11, true);
        m_symFuncDeclaration = ts_language_symbol_for_name(lang, "func_declaration", 16, true);
        m_symStatementBlock = ts_language_symbol_for_name(lang, "statement_block", 15, true);

        auto addPrimitive = [&](const char *name, uint32_t len, TypeKind kind)
        {
            TSSymbol sym = ts_language_symbol_for_name(lang, name, len, false);
            if (sym != 0)
            {
                m_primitiveKindMap.emplace(sym, kind);
            }
        };

        addPrimitive("void", 4, TypeKind::Void);
        addPrimitive("int", 3, TypeKind::Int32);
        addPrimitive("int32", 5, TypeKind::Int32);
        addPrimitive("int8", 4, TypeKind::Int8);
        addPrimitive("int16", 5, TypeKind::Int16);
        addPrimitive("int64", 5, TypeKind::Int64);
        addPrimitive("uint", 4, TypeKind::UInt32);
        addPrimitive("uint32", 6, TypeKind::UInt32);
        addPrimitive("uint8", 5, TypeKind::UInt8);
        addPrimitive("uint16", 6, TypeKind::UInt16);
        addPrimitive("uint64", 6, TypeKind::UInt64);
        addPrimitive("float", 5, TypeKind::Float);
        addPrimitive("double", 6, TypeKind::Double);
        addPrimitive("bool", 4, TypeKind::Bool);
        addPrimitive("string", 6, TypeKind::String);
        addPrimitive("auto", 4, TypeKind::Auto);

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

            for (uint32_t i = 0; i < captureCount; ++i)
            {
                uint32_t nameLen = 0;
                const char *name = ts_query_capture_name_for_id(m_tagsQuery, i, &nameLen);
                std::string_view captureName(name, nameLen);

                if (captureName == "definition.function")
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

    void SymbolCollector::ReportParseErrors(TSNode node, const std::string &fileUri, const std::string &sourceCode, std::vector<Diagnostic> &diagnostics, const angel_lsp::i18n::I18n *i18n) const
    {
        if (!ts_node_has_error(node))
            return;

        if (ts_node_is_missing(node) || (ts_node_is_named(node) && strcmp(ts_node_type(node), "ERROR") == 0))
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
                ReportParseErrors(child, fileUri, sourceCode, diagnostics, i18n);
            }
        }
    }

    std::vector<Diagnostic> SymbolCollector::CollectSymbols(const std::string &fileUri, const std::string &sourceCode, angel_lsp::parser::AngelScriptParser &parser, SymbolTable &symbolTable, const angel_lsp::i18n::I18n *i18n)
    {
        std::vector<Diagnostic> diagnostics;
        TSTree *tree = parser.Parse(sourceCode);

        if (!tree)
            return diagnostics;

        TSNode rootNode = ts_tree_root_node(tree);

        if (ts_node_has_error(rootNode))
        {
            ReportParseErrors(rootNode, fileUri, sourceCode, diagnostics, i18n);
        }

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
            }
        }

        ts_query_cursor_delete(cursor);
        ts_tree_delete(tree);
        return diagnostics;
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

            if (sym == m_symClassBody || sym == m_symNamespaceBody)
            {
                TSNode parentDecl = ts_node_parent(current);
                TSNode nameNode = ts_node_child_by_field_name(parentDecl, "name", 4);
                std::string name = GetNodeText(nameNode, sourceCode);
                ctx.containerPath = ctx.containerPath.empty() ? name : name + "::" + ctx.containerPath;
            }

            current = ts_node_parent(current);
        }
        return ctx;
    }

    TypeKind SymbolCollector::LookupPrimitiveKind(TSSymbol symbol) const
    {
        auto it = m_primitiveKindMap.find(symbol);
        if (it != m_primitiveKindMap.end())
        {
            return it->second;
        }
        return TypeKind::Unknown;
    }

    SymbolCollector::TypeExtractionResult SymbolCollector::ExtractTypeInfo(TSNode typeNode, const std::string &sourceCode) const
    {
        TypeExtractionResult result;
        if (ts_node_is_null(typeNode))
            return result;

        TSSymbol nodeSymbol = ts_node_symbol(typeNode);

        if (nodeSymbol == m_symPrimitiveType)
        {
            result.baseTypeName = GetNodeText(typeNode, sourceCode);
            TSNode parentDatatype = ts_node_parent(typeNode);
            if (!ts_node_is_null(parentDatatype))
            {
                TSNode innerToken = ts_node_child(typeNode, 0);
                if (!ts_node_is_null(innerToken))
                {
                    result.kind = LookupPrimitiveKind(ts_node_symbol(innerToken));
                }
            }
            if (result.kind == TypeKind::Unknown)
            {
                result.kind = LookupPrimitiveKind(nodeSymbol);
            }
            return result;
        }

        if (nodeSymbol == m_symIdentifier)
        {
            result.baseTypeName = GetNodeText(typeNode, sourceCode);
            result.kind = TypeKind::Unknown;
            return result;
        }

        std::string datatypeText;
        uint32_t count = ts_node_child_count(typeNode);
        TSNode prevChild = ts_node_child(typeNode, 0);

        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_child(typeNode, i);
            TSSymbol childSym = ts_node_symbol(child);

            if (childSym == m_symDatatype)
            {
                TSNode inner = ts_node_named_child(child, 0);
                if (!ts_node_is_null(inner))
                {
                    TypeExtractionResult innerInfo = ExtractTypeInfo(inner, sourceCode);
                    result.baseTypeName = innerInfo.baseTypeName;
                    result.kind = innerInfo.kind;
                    datatypeText = innerInfo.baseTypeName;
                }
                else
                {
                    datatypeText = GetNodeText(child, sourceCode);
                    result.baseTypeName = datatypeText;
                }
                prevChild = child;
            }
            else if (childSym == m_symTemplateTypeList)
            {
                result.isArray = true;
                result.arrayDepth++;
                result.templateName = datatypeText;

                uint32_t tCount = ts_node_named_child_count(child);
                for (uint32_t t = 0; t < tCount; ++t)
                {
                    TSNode innerType = ts_node_named_child(child, t);
                    if (ts_node_symbol(innerType) == m_symType)
                    {
                        TypeExtractionResult inner = ExtractTypeInfo(innerType, sourceCode);
                        result.baseTypeName = inner.baseTypeName;
                        result.isHandle = inner.isHandle || result.isHandle;
                        result.hasPrimitiveHandle = inner.hasPrimitiveHandle || result.hasPrimitiveHandle;
                        result.arrayDepth += inner.arrayDepth;
                        if (inner.kind != TypeKind::Unknown)
                        {
                            result.kind = inner.kind;
                        }
                        break;
                    }
                }
                prevChild = child;
            }
            else if (!ts_node_is_named(child))
            {
                std::string_view tok = GetNodeView(child, sourceCode);
                if (tok == "[")
                {
                    result.isArray = true;
                    result.arrayDepth++;
                }
                else if (tok == "@")
                {
                    result.isHandle = true;
                    if (!result.isArray && !ts_node_is_null(prevChild) && ts_node_symbol(prevChild) == m_symDatatype)
                    {
                        TSNode innerChild = ts_node_named_child(prevChild, 0);
                        if (!ts_node_is_null(innerChild) && ts_node_symbol(innerChild) == m_symPrimitiveType)
                        {
                            result.hasPrimitiveHandle = true;
                        }
                    }
                }
                else if (tok == "&")
                {
                    result.isReference = true;
                }
            }
        }

        if (result.isArray)
        {
            result.kind = TypeKind::Array;
        }
        else if (result.isHandle && result.kind == TypeKind::Unknown)
        {
            result.kind = TypeKind::Handle;
        }

        return result;
    }

    void SymbolCollector::ProcessVariable(TSNode varDeclNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        if (ctx.isInsideFunction)
            return;

        TSNode typeNode = ts_node_child_by_field_name(varDeclNode, "var_type", 8);
        std::string typeStr = GetNodeText(typeNode, sourceCode);
        TypeExtractionResult typeInfo = ExtractTypeInfo(typeNode, sourceCode);
        SymbolModifiers modifiers = ExtractModifiers(varDeclNode, sourceCode);
        modifiers.isHandle = typeInfo.isHandle || modifiers.isHandle;

        uint32_t count = ts_node_named_child_count(varDeclNode);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode declaratorNode = ts_node_named_child(varDeclNode, i);
            if (strcmp(ts_node_type(declaratorNode), "variable_declarator") != 0)
                continue;

            TSNode nameNode = ts_node_child_by_field_name(declaratorNode, "name", 4);
            TSNode valueNode = ts_node_child_by_field_name(declaratorNode, "value", 5);

            Symbol sym = CreateSymbol(SymbolType::Variable, varDeclNode, nameNode, sourceCode, fileUri, ctx.containerPath);
            VariableSignature varSig;
            varSig.typeName = typeStr;
            varSig.baseTypeName = typeInfo.baseTypeName;
            varSig.templateName = typeInfo.templateName;
            varSig.typeKind = typeInfo.kind;
            varSig.isArray = typeInfo.isArray;
            varSig.hasPrimitiveHandle = typeInfo.hasPrimitiveHandle;
            varSig.arrayDepth = typeInfo.arrayDepth;
            varSig.defaultValue = GetNodeText(valueNode, sourceCode);
            varSig.modifiers = modifiers;

            sym.signature = varSig;
            symbolTable.AddSymbol(sym);
        }
    }

    ParameterInformation SymbolCollector::ExtractParameterInfo(TSNode paramNode, const std::string &sourceCode) const
    {
        TSNode pNameNode = ts_node_child_by_field_name(paramNode, "name", 4);
        TSNode pTypeNode = ts_node_child_by_field_name(paramNode, "param_type", 10);
        TSNode pDefaultNode = ts_node_child_by_field_name(paramNode, "default_value", 13);

        TypeExtractionResult pInfo = ExtractTypeInfo(pTypeNode, sourceCode);

        TSPoint startPt = ts_node_start_point(paramNode);
        TSPoint endPt = ts_node_end_point(paramNode);

        ParameterInformation paramInfo;
        paramInfo.name = GetNodeText(pNameNode, sourceCode);
        paramInfo.typeName = GetNodeText(pTypeNode, sourceCode);
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
                }
            }
        }

        uint32_t paramChildCount = ts_node_child_count(paramNode);
        for (uint32_t i = 0; i < paramChildCount; ++i)
        {
            TSNode child = ts_node_child(paramNode, i);
            if (ts_node_is_named(child))
                continue;

            std::string_view tok = GetNodeView(child, sourceCode);
            if (tok == "&")
            {
                paramInfo.isReference = true;
            }
            else if (tok == "out")
            {
                paramInfo.modifier = ParameterModifier::Out;
            }
            else if (tok == "inout")
            {
                paramInfo.modifier = ParameterModifier::InOut;
            }
            else if (tok == "in")
            {
                paramInfo.modifier = ParameterModifier::In;
            }
        }

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
            parameters.push_back(ExtractParameterInfo(ts_node_named_child(paramsNode, p), sourceCode));
        }
        return parameters;
    }

    void SymbolCollector::ProcessFunction(TSNode funcNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = ts_node_child_by_field_name(funcNode, "name", 4);
        TSNode typeNode = ts_node_child_by_field_name(funcNode, "return_type", 11);
        TSNode paramsNode = ts_node_child_by_field_name(funcNode, "parameters", 10);
        TSNode bodyNode = ts_node_child_by_field_name(funcNode, "body", 4);

        TypeExtractionResult retInfo = ExtractTypeInfo(typeNode, sourceCode);
        SymbolModifiers modifiers = ExtractModifiers(funcNode, sourceCode);
        modifiers.isHandle = retInfo.isHandle || modifiers.isHandle;
        modifiers.isReturnReference = retInfo.isReference || modifiers.isReturnReference;

        Symbol sym = CreateSymbol(SymbolType::Function, funcNode, nameNode, sourceCode, fileUri, ctx.containerPath);
        FunctionSignature funcSig;
        funcSig.returnType = GetNodeText(typeNode, sourceCode);
        funcSig.returnBaseTypeName = retInfo.baseTypeName;
        funcSig.returnTemplateName = retInfo.templateName;
        funcSig.returnTypeKind = retInfo.kind;
        funcSig.returnIsArray = retInfo.isArray;
        funcSig.returnHasPrimitiveHandle = retInfo.hasPrimitiveHandle;
        funcSig.returnArrayDepth = retInfo.arrayDepth;
        funcSig.modifiers = modifiers;
        funcSig.parameters = ExtractParameters(paramsNode, sourceCode);
        funcSig.hasBody = !ts_node_is_null(bodyNode);

        sym.signature = funcSig;
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessClass(TSNode classNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = ts_node_child_by_field_name(classNode, "name", 4);
        TSNode tParamNode = ts_node_child_by_field_name(classNode, "template_param", 14);

        Symbol sym = CreateSymbol(SymbolType::Class, classNode, nameNode, sourceCode, fileUri, ctx.containerPath);
        ClassSignature classSig;
        classSig.modifiers = ExtractModifiers(classNode, sourceCode);
        classSig.bases = ExtractBases(classNode, sourceCode);
        classSig.isTemplate = !ts_node_is_null(tParamNode);

        sym.signature = classSig;
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessNamespace(TSNode namespaceNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = ts_node_child_by_field_name(namespaceNode, "name", 4);
        Symbol sym = CreateSymbol(SymbolType::Namespace, namespaceNode, nameNode, sourceCode, fileUri, ctx.containerPath);
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessEnum(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
        Symbol sym = CreateSymbol(SymbolType::Enum, node, nameNode, sourceCode, fileUri, ctx.containerPath);

        EnumSignature enumSig;
        enumSig.modifiers = ExtractModifiers(node, sourceCode);

        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_named_child(node, i);
            if (ts_node_symbol(child) != m_symEnumMember)
                continue;

            TSNode memberName = ts_node_child_by_field_name(child, "name", 4);
            TSNode memberValue = ts_node_child_by_field_name(child, "value", 5);

            EnumMemberInformation member;
            member.name = GetNodeText(memberName, sourceCode);
            member.value = GetNodeText(memberValue, sourceCode);
            enumSig.members.push_back(std::move(member));
        }

        sym.signature = enumSig;
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessTypedef(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
        TSNode baseTypeNode = ts_node_child_by_field_name(node, "base_type", 9);

        Symbol sym = CreateSymbol(SymbolType::Typedef, node, nameNode, sourceCode, fileUri, ctx.containerPath);
        TypedefSignature typedefSig;

        if (!ts_node_is_null(baseTypeNode))
        {
            TypeExtractionResult info = ExtractTypeInfo(baseTypeNode, sourceCode);
            typedefSig.baseType = info.baseTypeName;
            typedefSig.typeKind = info.kind;
        }

        sym.signature = typedefSig;
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessFuncdef(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
        TSNode typeNode = ts_node_child_by_field_name(node, "return_type", 11);
        TSNode paramsNode = ts_node_child_by_field_name(node, "parameters", 10);

        TypeExtractionResult retInfo = ExtractTypeInfo(typeNode, sourceCode);
        SymbolModifiers modifiers = ExtractModifiers(node, sourceCode);
        modifiers.isHandle = retInfo.isHandle || modifiers.isHandle;
        modifiers.isReturnReference = retInfo.isReference || modifiers.isReturnReference;

        Symbol sym = CreateSymbol(SymbolType::Funcdef, node, nameNode, sourceCode, fileUri, ctx.containerPath);
        FunctionSignature funcSig;
        funcSig.returnType = GetNodeText(typeNode, sourceCode);
        funcSig.returnBaseTypeName = retInfo.baseTypeName;
        funcSig.returnTypeKind = retInfo.kind;
        funcSig.returnIsArray = retInfo.isArray;
        funcSig.returnArrayDepth = retInfo.arrayDepth;
        funcSig.modifiers = modifiers;
        funcSig.parameters = ExtractParameters(paramsNode, sourceCode);

        sym.signature = funcSig;
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessInterface(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
        Symbol sym = CreateSymbol(SymbolType::Interface, node, nameNode, sourceCode, fileUri, ctx.containerPath);
        InterfaceSignature ifaceSig;
        ifaceSig.modifiers = ExtractModifiers(node, sourceCode);
        ifaceSig.inheritedInterfaces = ExtractBases(node, sourceCode);

        sym.signature = ifaceSig;
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessProperty(TSNode node, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable, const CollectionContext &ctx)
    {
        TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
        TSNode typeNode = ts_node_child_by_field_name(node, "prop_type", 9);

        TypeExtractionResult typeInfo = ExtractTypeInfo(typeNode, sourceCode);
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

        sym.signature = varSig;
        symbolTable.AddSymbol(sym);
    }

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
                    ApplyModifierToken(ts_node_type(ts_node_child(child, m)), modifiers);
                }
            }
            else if (!ts_node_is_named(child))
            {
                ApplyModifierToken(ts_node_type(child), modifiers);
            }
        }
        return modifiers;
    }

    Symbol SymbolCollector::CreateSymbol(SymbolType type, TSNode node, TSNode nameNode,
                                          const std::string &sourceCode, const std::string &fileUri,
                                          const std::string &containerPath) const
    {
        TSPoint startPt = ts_node_start_point(node);
        TSPoint endPt = ts_node_end_point(node);

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

        return sym;
    }

    std::vector<std::string> SymbolCollector::ExtractBases(TSNode classNode, const std::string &sourceCode) const
    {
        std::vector<std::string> bases;

        uint32_t namedCount = ts_node_named_child_count(classNode);
        for (uint32_t i = 0; i < namedCount; ++i)
        {
            TSNode child = ts_node_named_child(classNode, i);
            if (strcmp(ts_node_type(child), "base_class_list") != 0)
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