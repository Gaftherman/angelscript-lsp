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
        uint32_t errorOffset = 0;
        TSQueryError errorType = TSQueryErrorNone;

        m_tagsQuery = ts_query_new(tree_sitter_angelscript(), angel_lsp::parser::queries::TAGS_QUERY,
                                   static_cast<uint32_t>(strlen(angel_lsp::parser::queries::TAGS_QUERY)),
                                   &errorOffset, &errorType);

        if (!m_tagsQuery && m_logger)
        {
            m_logger->LogError(fmt::format("Error al compilar TAGS_QUERY en offset: {}", errorOffset));
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
                TSQueryCapture capture = match.captures[i];
                uint32_t length = 0;
                const char *tagName = ts_query_capture_name_for_id(m_tagsQuery, capture.index, &length);

                if (strcmp(tagName, "definition.function") == 0)
                {
                    ProcessFunction(capture.node, sourceCode, fileUri, symbolTable);
                }
                else if (strcmp(tagName, "definition.class") == 0)
                {
                    ProcessClass(capture.node, sourceCode, fileUri, symbolTable);
                }
                else if (strcmp(tagName, "definition.variable") == 0)
                {
                    ProcessVariable(capture.node, sourceCode, fileUri, symbolTable);
                }
                else if (strcmp(tagName, "definition.namespace") == 0)
                {
                    ProcessNamespace(capture.node, sourceCode, fileUri, symbolTable);
                }
                else if (strcmp(tagName, "definition.enum") == 0)
                {
                    ProcessEnum(capture.node, tagName, sourceCode, fileUri, symbolTable);
                }
                else if (strcmp(tagName, "definition.typedef") == 0)
                {
                    ProcessTypedef(capture.node, tagName, sourceCode, fileUri, symbolTable);
                }
                else if (strcmp(tagName, "definition.funcdef") == 0)
                {
                    ProcessFuncdef(capture.node, tagName, sourceCode, fileUri, symbolTable);
                }
                else if (strcmp(tagName, "definition.interface") == 0)
                {
                    ProcessInterface(capture.node, tagName, sourceCode, fileUri, symbolTable);
                }
                else if (strcmp(tagName, "definition.property") == 0)
                {
                    ProcessProperty(capture.node, tagName, sourceCode, fileUri, symbolTable);
                }
            }
        }

        ts_query_cursor_delete(cursor);
        ts_tree_delete(tree);
        return diagnostics;
    }

    SymbolCollector::NodeContext SymbolCollector::GetNodeContext(TSNode node, const std::string &sourceCode) const
    {
        NodeContext ctx;
        TSNode current = ts_node_parent(node);

        while (!ts_node_is_null(current))
        {
            const char *nodeType = ts_node_type(current);

            if (!ctx.isInsideFunction &&
                (strcmp(nodeType, "func_declaration") == 0 || strcmp(nodeType, "statement_block") == 0))
            {
                ctx.isInsideFunction = true;
            }

            if (strcmp(nodeType, "class_body") == 0 || strcmp(nodeType, "namespace_body") == 0)
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

    static bool IsPrimitiveTypeName(const std::string &name)
    {
        return name == "void" || name == "int" || name == "int8" || name == "int16" || name == "int32" || name == "int64"
            || name == "uint" || name == "uint8" || name == "uint16" || name == "uint32" || name == "uint64"
            || name == "float" || name == "double" || name == "bool";
    }

    SymbolCollector::TypeExtractionResult SymbolCollector::ExtractTypeInfo(TSNode typeNode, const std::string &sourceCode) const
    {
        TypeExtractionResult result;
        if (ts_node_is_null(typeNode))
            return result;

        const char *nodeType = ts_node_type(typeNode);
        if (strcmp(nodeType, "primitive_type") == 0 || strcmp(nodeType, "identifier") == 0)
        {
            result.baseTypeName = GetNodeText(typeNode, sourceCode);
            result.kind = ParseTypeKind(result.baseTypeName);
            return result;
        }

        bool hasTemplateList = false;
        std::string datatypeText;

        uint32_t count = ts_node_child_count(typeNode);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_child(typeNode, i);
            const char *childType = ts_node_type(child);

            if (strcmp(childType, "datatype") == 0)
            {
                if (ts_node_named_child_count(child) > 0)
                {
                    datatypeText = GetNodeText(ts_node_named_child(child, 0), sourceCode);
                }
                else
                {
                    datatypeText = GetNodeText(child, sourceCode);
                }
            }
            else if (strcmp(childType, "template_type_list") == 0)
            {
                hasTemplateList = true;
                result.isArray = true;
                result.arrayDepth++;

                uint32_t tCount = ts_node_named_child_count(child);
                for (uint32_t t = 0; t < tCount; ++t)
                {
                    TSNode innerType = ts_node_named_child(child, t);
                    if (strcmp(ts_node_type(innerType), "type") == 0)
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
            }
            else if (strcmp(childType, "[") == 0)
            {
                result.isArray = true;
                result.arrayDepth++;
            }
            else if (strcmp(childType, "@") == 0)
            {
                result.isHandle = true;
                if (i > 0)
                {
                    TSNode prevChild = ts_node_child(typeNode, i - 1);
                    if (strcmp(ts_node_type(prevChild), "datatype") == 0 && IsPrimitiveTypeName(datatypeText))
                    {
                        result.hasPrimitiveHandle = true;
                    }
                }
            }
        }

        if (hasTemplateList)
        {
            result.templateName = datatypeText;
        }
        else if (!datatypeText.empty())
        {
            result.baseTypeName = datatypeText;
            result.kind = ParseTypeKind(datatypeText);
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

    void SymbolCollector::ProcessVariable(TSNode varDeclNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable)
    {
        NodeContext ctx = GetNodeContext(varDeclNode, sourceCode);
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
            sym.variableSignature.typeName = typeStr;
            sym.variableSignature.baseTypeName = typeInfo.baseTypeName;
            sym.variableSignature.templateName = typeInfo.templateName;
            sym.variableSignature.typeKind = typeInfo.kind;
            sym.variableSignature.isArray = typeInfo.isArray;
            sym.variableSignature.hasPrimitiveHandle = typeInfo.hasPrimitiveHandle;
            sym.variableSignature.arrayDepth = typeInfo.arrayDepth;
            sym.variableSignature.defaultValue = GetNodeText(valueNode, sourceCode);
            sym.variableSignature.modifiers = modifiers;

            symbolTable.AddSymbol(sym);
        }
    }

    ParameterInformation SymbolCollector::ExtractParameterInfo(TSNode paramNode, const std::string &sourceCode) const
    {
        TSNode pNameNode = ts_node_child_by_field_name(paramNode, "name", 4);
        TSNode pTypeNode = ts_node_child_by_field_name(paramNode, "param_type", 10);
        TSNode pDefaultNode = ts_node_child_by_field_name(paramNode, "default_value", 13);

        TypeExtractionResult pInfo = ExtractTypeInfo(pTypeNode, sourceCode);
        SymbolModifiers mods = ExtractModifiers(paramNode, sourceCode);

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
        paramInfo.isConst = mods.isConst;
        paramInfo.isHandle = pInfo.isHandle || mods.isHandle;
        paramInfo.modifier = mods.paramModifier;
        paramInfo.startLine = startPt.row;
        paramInfo.startCharacter = startPt.column;
        paramInfo.endLine = endPt.row;
        paramInfo.endCharacter = endPt.column;

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

    void SymbolCollector::ProcessFunction(TSNode funcNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable)
    {
        NodeContext ctx = GetNodeContext(funcNode, sourceCode);

        TSNode nameNode = ts_node_child_by_field_name(funcNode, "name", 4);
        TSNode typeNode = ts_node_child_by_field_name(funcNode, "return_type", 11);
        TSNode paramsNode = ts_node_child_by_field_name(funcNode, "parameters", 10);
        TSNode bodyNode = ts_node_child_by_field_name(funcNode, "body", 4);

        TypeExtractionResult retInfo = ExtractTypeInfo(typeNode, sourceCode);
        SymbolModifiers modifiers = ExtractModifiers(funcNode, sourceCode);
        modifiers.isHandle = retInfo.isHandle || modifiers.isHandle;

        Symbol sym = CreateSymbol(SymbolType::Function, funcNode, nameNode, sourceCode, fileUri, ctx.containerPath);
        sym.functionSignature.returnType = GetNodeText(typeNode, sourceCode);
        sym.functionSignature.returnBaseTypeName = retInfo.baseTypeName;
        sym.functionSignature.returnTemplateName = retInfo.templateName;
        sym.functionSignature.returnTypeKind = retInfo.kind;
        sym.functionSignature.returnIsArray = retInfo.isArray;
        sym.functionSignature.returnHasPrimitiveHandle = retInfo.hasPrimitiveHandle;
        sym.functionSignature.returnArrayDepth = retInfo.arrayDepth;
        sym.functionSignature.modifiers = modifiers;
        sym.functionSignature.parameters = ExtractParameters(paramsNode, sourceCode);
        sym.functionSignature.hasBody = !ts_node_is_null(bodyNode);

        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessClass(TSNode classNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable)
    {
        NodeContext ctx = GetNodeContext(classNode, sourceCode);
        TSNode nameNode = ts_node_child_by_field_name(classNode, "name", 4);
        TSNode tParamNode = ts_node_child_by_field_name(classNode, "template_param", 14);

        Symbol sym = CreateSymbol(SymbolType::Class, classNode, nameNode, sourceCode, fileUri, ctx.containerPath);
        sym.classSignature.modifiers = ExtractModifiers(classNode, sourceCode);
        sym.classSignature.bases = ExtractBases(classNode, sourceCode);
        sym.classSignature.isTemplate = !ts_node_is_null(tParamNode);

        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessNamespace(TSNode namespaceNode, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable)
    {
        NodeContext ctx = GetNodeContext(namespaceNode, sourceCode);
        TSNode nameNode = ts_node_child_by_field_name(namespaceNode, "name", 4);
        Symbol sym = CreateSymbol(SymbolType::Namespace, namespaceNode, nameNode, sourceCode, fileUri, ctx.containerPath);
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessEnum(TSNode node, const std::string &tagName, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable)
    {
        NodeContext ctx = GetNodeContext(node, sourceCode);
        TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
        Symbol sym = CreateSymbol(SymbolType::Enum, node, nameNode, sourceCode, fileUri, ctx.containerPath);
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessTypedef(TSNode node, const std::string &tagName, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable)
    {
        NodeContext ctx = GetNodeContext(node, sourceCode);
        TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
        TSNode baseTypeNode = ts_node_child_by_field_name(node, "base_type", 9);

        Symbol sym = CreateSymbol(SymbolType::Typedef, node, nameNode, sourceCode, fileUri, ctx.containerPath);
        if (!ts_node_is_null(baseTypeNode))
        {
            TypeExtractionResult info = ExtractTypeInfo(baseTypeNode, sourceCode);
            sym.typedefSignature.baseType = info.baseTypeName;
            sym.typedefSignature.typeKind = info.kind;
        }
        else
        {
            uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_named_child(node, i);
                if (child.id != nameNode.id)
                {
                    sym.typedefSignature.baseType = GetNodeText(child, sourceCode);
                    sym.typedefSignature.typeKind = ParseTypeKind(sym.typedefSignature.baseType);
                    break;
                }
            }
        }
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessFuncdef(TSNode node, const std::string &tagName, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable)
    {
        NodeContext ctx = GetNodeContext(node, sourceCode);
        TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
        TSNode typeNode = ts_node_child_by_field_name(node, "return_type", 11);
        TSNode paramsNode = ts_node_child_by_field_name(node, "parameters", 10);

        TypeExtractionResult retInfo = ExtractTypeInfo(typeNode, sourceCode);
        SymbolModifiers modifiers = ExtractModifiers(node, sourceCode);
        modifiers.isHandle = retInfo.isHandle || modifiers.isHandle;

        Symbol sym = CreateSymbol(SymbolType::Funcdef, node, nameNode, sourceCode, fileUri, ctx.containerPath);
        sym.functionSignature.returnType = GetNodeText(typeNode, sourceCode);
        sym.functionSignature.returnBaseTypeName = retInfo.baseTypeName;
        sym.functionSignature.returnTypeKind = retInfo.kind;
        sym.functionSignature.returnIsArray = retInfo.isArray;
        sym.functionSignature.returnArrayDepth = retInfo.arrayDepth;
        sym.functionSignature.modifiers = modifiers;
        sym.functionSignature.parameters = ExtractParameters(paramsNode, sourceCode);

        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessInterface(TSNode node, const std::string &tagName, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable)
    {
        NodeContext ctx = GetNodeContext(node, sourceCode);
        TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
        Symbol sym = CreateSymbol(SymbolType::Interface, node, nameNode, sourceCode, fileUri, ctx.containerPath);
        symbolTable.AddSymbol(sym);
    }

    void SymbolCollector::ProcessProperty(TSNode node, const std::string &tagName, const std::string &sourceCode, const std::string &fileUri, SymbolTable &symbolTable)
    {
        NodeContext ctx = GetNodeContext(node, sourceCode);
        TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
        Symbol sym = CreateSymbol(SymbolType::Property, node, nameNode, sourceCode, fileUri, ctx.containerPath);
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

    SymbolModifiers SymbolCollector::ExtractModifiers(TSNode node, const std::string &sourceCode) const
    {
        SymbolModifiers modifiers;
        if (ts_node_is_null(node))
            return modifiers;

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_child(node, i);
            const char *childType = ts_node_type(child);

            if (strcmp(childType, "declaration_modifier") == 0)
            {
                uint32_t modCount = ts_node_child_count(child);
                for (uint32_t m = 0; m < modCount; ++m)
                {
                    ApplyModifierToken(ts_node_type(ts_node_child(child, m)), modifiers);
                }
            }
            else if (strcmp(childType, "type") == 0)
            {
                uint32_t typeCount = ts_node_child_count(child);
                for (uint32_t t = 0; t < typeCount; ++t)
                {
                    ApplyModifierToken(ts_node_type(ts_node_child(child, t)), modifiers);
                }
            }
            else
            {
                ApplyModifierToken(childType, modifiers);
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

    TypeKind SymbolCollector::ParseTypeKind(const std::string &typeName) const
    {
        if (typeName == "void")
            return TypeKind::Void;
        if (typeName == "int" || typeName == "int32")
            return TypeKind::Int32;
        if (typeName == "int8")
            return TypeKind::Int8;
        if (typeName == "int16")
            return TypeKind::Int16;
        if (typeName == "int64")
            return TypeKind::Int64;
        if (typeName == "uint" || typeName == "uint32")
            return TypeKind::UInt32;
        if (typeName == "uint8")
            return TypeKind::UInt8;
        if (typeName == "uint16")
            return TypeKind::UInt16;
        if (typeName == "uint64")
            return TypeKind::UInt64;
        if (typeName == "float")
            return TypeKind::Float;
        if (typeName == "double")
            return TypeKind::Double;
        if (typeName == "bool")
            return TypeKind::Bool;
        if (typeName == "string")
            return TypeKind::String;
        if (typeName == "auto")
            return TypeKind::Auto;

        if (!typeName.empty() && typeName.back() == '@')
            return TypeKind::Handle;
        if (typeName.rfind("array<", 0) == 0)
            return TypeKind::Array;

        return TypeKind::Unknown;
    }
}