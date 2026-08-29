#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeExtraction.h"
#include "parser/queries/BuiltQueries.h"
#include "utils/Constants.h"
#include "spdlog/fmt/fmt.h"

#include <algorithm>
#include <cstring>

extern "C" const TSLanguage *tree_sitter_angelscript();

namespace angel_lsp::analysis
{
    LocalScopeCollector::LocalScopeCollector(angel_lsp::utils::LspLogger *logger)
        : m_logger(logger), m_localsQuery(nullptr)
    {
        const TSLanguage *lang = tree_sitter_angelscript();

        m_symMemberExpression = ts_language_symbol_for_name(lang, "member_expression", static_cast<uint32_t>(strlen("member_expression")), true);
        m_symFuncDeclaration = ts_language_symbol_for_name(lang, "func_declaration", static_cast<uint32_t>(strlen("func_declaration")), true);
        m_symLambdaExpression = ts_language_symbol_for_name(lang, "lambda_expression", static_cast<uint32_t>(strlen("lambda_expression")), true);
        m_symVariableDeclarator = ts_language_symbol_for_name(lang, "variable_declarator", static_cast<uint32_t>(strlen("variable_declarator")), true);
        m_symParameter = ts_language_symbol_for_name(lang, "parameter", static_cast<uint32_t>(strlen("parameter")), true);
        m_symForeachVariable = ts_language_symbol_for_name(lang, "foreach_variable", static_cast<uint32_t>(strlen("foreach_variable")), true);

        uint32_t errorOffset = 0;
        TSQueryError errorType = TSQueryErrorNone;
        m_localsQuery = ts_query_new(lang, angel_lsp::parser::queries::LOCALS_QUERY,
                                     static_cast<uint32_t>(strlen(angel_lsp::parser::queries::LOCALS_QUERY)),
                                     &errorOffset, &errorType);

        if (!m_localsQuery)
        {
            if (m_logger)
                m_logger->LogError(fmt::format("Failed to compile LOCALS_QUERY at offset: {}", errorOffset));
            return;
        }

        uint32_t captureCount = ts_query_capture_count(m_localsQuery);
        m_captureKinds.assign(captureCount, CaptureKind::None);
        m_definitionKinds.assign(captureCount, LocalDefinitionKind::Variable);

        for (uint32_t i = 0; i < captureCount; ++i)
        {
            uint32_t nameLen = 0;
            const char *name = ts_query_capture_name_for_id(m_localsQuery, i, &nameLen);
            std::string_view captureName(name, nameLen);

            if (captureName == "local.scope")
            {
                m_captureKinds[i] = CaptureKind::Scope;
            }
            else if (captureName == "local.reference")
            {
                m_captureKinds[i] = CaptureKind::Reference;
            }
            else if (captureName == "local.definition.parameter")
            {
                m_captureKinds[i] = CaptureKind::Definition;
                m_definitionKinds[i] = LocalDefinitionKind::Parameter;
            }
            else if (captureName == "local.definition.var")
            {
                m_captureKinds[i] = CaptureKind::Definition;
                m_definitionKinds[i] = LocalDefinitionKind::Variable;
            }
            else if (captureName == "local.definition.field")
            {
                m_captureKinds[i] = CaptureKind::Definition;
                m_definitionKinds[i] = LocalDefinitionKind::Field;
            }
            else if (captureName == "local.definition.function")
            {
                m_captureKinds[i] = CaptureKind::Definition;
                m_definitionKinds[i] = LocalDefinitionKind::Function;
            }
            else if (captureName == "local.definition.method")
            {
                m_captureKinds[i] = CaptureKind::Definition;
                m_definitionKinds[i] = LocalDefinitionKind::Method;
            }
            else if (captureName == "local.definition.type")
            {
                m_captureKinds[i] = CaptureKind::Definition;
                m_definitionKinds[i] = LocalDefinitionKind::Type;
            }
            else if (captureName == "local.definition.constant")
            {
                m_captureKinds[i] = CaptureKind::Definition;
                m_definitionKinds[i] = LocalDefinitionKind::Constant;
            }
            else if (captureName == "local.definition.namespace")
            {
                m_captureKinds[i] = CaptureKind::Definition;
                m_definitionKinds[i] = LocalDefinitionKind::Namespace;
            }
            else if (captureName == "local.definition.import")
            {
                m_captureKinds[i] = CaptureKind::Definition;
                m_definitionKinds[i] = LocalDefinitionKind::Import;
            }
        }
    }

    LocalScopeCollector::~LocalScopeCollector()
    {
        if (m_localsQuery)
        {
            ts_query_delete(m_localsQuery);
        }
    }

    std::unique_ptr<Scope> LocalScopeCollector::CollectScopes(const std::string &sourceCode, angel_lsp::parser::AngelScriptParser &parser) const
    {
        TSTree *tree = parser.Parse(sourceCode);
        if (!tree)
            return nullptr;

        TSNode rootNode = ts_tree_root_node(tree);
        std::unique_ptr<Scope> root = CollectScopesFromTree(rootNode, sourceCode);

        ts_tree_delete(tree);
        return root;
    }

    std::unique_ptr<Scope> LocalScopeCollector::CollectScopesFromTree(TSNode rootNode, const std::string &sourceCode) const
    {
        if (!m_localsQuery)
            return nullptr;

        std::vector<RawCapture> captures;

        TSQueryCursor *cursor = ts_query_cursor_new();
        ts_query_cursor_exec(cursor, m_localsQuery, rootNode);

        TSQueryMatch match;
        while (ts_query_cursor_next_match(cursor, &match))
        {
            for (uint32_t i = 0; i < match.capture_count; ++i)
            {
                uint32_t captureIdx = match.captures[i].index;
                if (captureIdx >= m_captureKinds.size())
                    continue;

                CaptureKind kind = m_captureKinds[captureIdx];
                if (kind == CaptureKind::None)
                    continue;

                captures.push_back(RawCapture{
                    match.captures[i].node,
                    kind,
                    kind == CaptureKind::Definition ? m_definitionKinds[captureIdx] : LocalDefinitionKind::Variable});
            }
        }

        ts_query_cursor_delete(cursor);

        return BuildScopeTree(captures, sourceCode);
    }

    std::unique_ptr<Scope> LocalScopeCollector::BuildScopeTree(std::vector<RawCapture> &captures, const std::string &sourceCode) const
    {
        // Standard tree-sitter "locals" nesting algorithm (the same one used by editor
        // integrations for locals.scm): sort every capture by where it starts in the source -
        // ties broken by putting the larger range first, so a scope that shares a start byte
        // with its first child still opens before that child - then walk the sorted list once
        // with a scope stack. A @local.scope capture pushes a new child scope; a definition or
        // reference attaches to whichever scope is innermost (top of stack) at that point. Query
        // match order alone doesn't guarantee this nesting order across different capture
        // patterns, which is why this is a separate accumulate-then-sort pass instead of the
        // live per-match dispatch SymbolCollector uses for TAGS_QUERY.
        std::sort(captures.begin(), captures.end(), [](const RawCapture &a, const RawCapture &b)
        {
            uint32_t aStart = ts_node_start_byte(a.node);
            uint32_t bStart = ts_node_start_byte(b.node);
            if (aStart != bStart)
                return aStart < bStart;
            return ts_node_end_byte(a.node) > ts_node_end_byte(b.node);
        });

        // LOCALS_QUERY has both a generic pattern (any variable_declarator/func_declaration)
        // and a more specific class_body-nested pattern for fields/methods; tree-sitter matches
        // every pattern independently, so a class field or method produces two Definition
        // captures on the exact same node (e.g. a field is both @local.definition.var and
        // @local.definition.field). The sort above already orders by (startByte asc, endByte
        // desc), so duplicates on an identical node are always adjacent - collapse them here,
        // keeping only the more specific kind (Field over Variable, Method over Function).
        auto isMoreSpecific = [](LocalDefinitionKind kind)
        {
            return kind == LocalDefinitionKind::Field || kind == LocalDefinitionKind::Method;
        };

        std::vector<RawCapture> deduped;
        deduped.reserve(captures.size());
        for (size_t i = 0; i < captures.size(); ++i)
        {
            if (i + 1 < captures.size()
                && captures[i].kind == CaptureKind::Definition
                && captures[i + 1].kind == CaptureKind::Definition
                && ts_node_start_byte(captures[i].node) == ts_node_start_byte(captures[i + 1].node)
                && ts_node_end_byte(captures[i].node) == ts_node_end_byte(captures[i + 1].node))
            {
                bool secondIsMoreSpecific = !isMoreSpecific(captures[i].definitionKind) && isMoreSpecific(captures[i + 1].definitionKind);
                deduped.push_back(secondIsMoreSpecific ? captures[i + 1] : captures[i]);
                ++i;
                continue;
            }
            deduped.push_back(captures[i]);
        }
        captures = std::move(deduped);

        struct OpenScope
        {
            Scope *scope;
            uint32_t endByte;

            // Only set for a func_declaration-opened scope: the byte range of its own "name"
            // field. func_declaration is the only scope-opening node whose range also contains
            // its own name (see BuildScopeTree's handling below for why that needs special
            // treatment). Sentinel InvalidByteOffset/0 (an inverted, unmatchable range) means "no
            // exclusion needed" for every other scope kind.
            uint32_t ownNameStartByte = constants::InvalidByteOffset;
            uint32_t ownNameEndByte = 0;
        };

        std::unique_ptr<Scope> root;
        std::vector<OpenScope> stack;

        for (const RawCapture &capture : captures)
        {
            uint32_t startByte = ts_node_start_byte(capture.node);

            while (!stack.empty() && startByte >= stack.back().endByte)
                stack.pop_back();

            if (capture.kind == CaptureKind::Scope)
            {
                TSPoint startPt = ts_node_start_point(capture.node);
                TSPoint endPt = ts_node_end_point(capture.node);

                auto newScope = std::make_unique<Scope>();
                TSSymbol scopeNodeSymbol = ts_node_symbol(capture.node);
                newScope->isFunctionScope = (scopeNodeSymbol == m_symFuncDeclaration || scopeNodeSymbol == m_symLambdaExpression);
                newScope->startLine = startPt.row;
                newScope->startCharacter = startPt.column;
                newScope->endLine = endPt.row;
                newScope->endCharacter = endPt.column;

                uint32_t endByte = ts_node_end_byte(capture.node);
                Scope *scopePtr = nullptr;

                if (stack.empty())
                {
                    root = std::move(newScope);
                    scopePtr = root.get();
                }
                else
                {
                    newScope->parent = stack.back().scope;
                    stack.back().scope->children.push_back(std::move(newScope));
                    scopePtr = stack.back().scope->children.back().get();
                }

                OpenScope openScope{scopePtr, endByte};
                if (scopeNodeSymbol == m_symFuncDeclaration)
                {
                    TSNode ownNameNode = ts_node_child_by_field_name(capture.node, "name", static_cast<uint32_t>(strlen("name")));
                    if (!ts_node_is_null(ownNameNode))
                    {
                        openScope.ownNameStartByte = ts_node_start_byte(ownNameNode);
                        openScope.ownNameEndByte = ts_node_end_byte(ownNameNode);
                    }
                }
                stack.push_back(openScope);
                continue;
            }

            // A definition/reference outside any @local.scope can't happen in practice - LOCALS_QUERY
            // always opens with (script) @local.scope covering the whole file - but skip defensively
            // rather than dereference an empty stack if the query is ever edited to no longer guarantee it.
            if (stack.empty())
                continue;

            Scope *current = stack.back().scope;
            TSPoint startPt = ts_node_start_point(capture.node);
            TSPoint endPt = ts_node_end_point(capture.node);

            if (capture.kind == CaptureKind::Definition)
            {
                // A function/method's own name is textually inside the scope it just opened
                // (func_declaration's range covers name + parameters + body together), but it
                // should be visible to callers in the *enclosing* scope, not only recursively
                // from within its own body. Redirect only this exact node - parameters and
                // everything else inside the body still attach to the newly-opened scope as usual.
                uint32_t defStartByte = ts_node_start_byte(capture.node);
                if (defStartByte == stack.back().ownNameStartByte
                    && ts_node_end_byte(capture.node) == stack.back().ownNameEndByte
                    && stack.size() >= 2)
                {
                    current = stack[stack.size() - 2].scope;
                }

                LocalDefinition def{
                    GetNodeText(capture.node, sourceCode),
                    capture.definitionKind,
                    startPt.row, startPt.column, endPt.row, endPt.column};

                if (capture.definitionKind == LocalDefinitionKind::Variable ||
                    capture.definitionKind == LocalDefinitionKind::Parameter)
                {
                    ReadVariableTypeInfo(capture.node, sourceCode, def);
                }

                current->definitions.push_back(std::move(def));
            }
            else if (capture.kind == CaptureKind::Reference)
            {
                LocalReference ref{
                    GetNodeText(capture.node, sourceCode),
                    startPt.row, startPt.column, endPt.row, endPt.column};

                TSNode parent = ts_node_parent(capture.node);
                if (!ts_node_is_null(parent))
                {
                    std::string_view parentType = ts_node_type(parent);
                    if (parentType == "datatype" || parentType == "template_type_list" || parentType == "base_class_list" || parentType == "type")
                    {
                        ref.isTypeSpecifier = true;
                    }
                    else if (parentType == "scoped_identifier")
                    {
                        TSNode grandParent = ts_node_parent(parent);
                        if (!ts_node_is_null(grandParent))
                        {
                            std::string_view grandParentType = ts_node_type(grandParent);
                            if (grandParentType == "datatype" || grandParentType == "template_type_list" || grandParentType == "base_class_list" || grandParentType == "type")
                            {
                                ref.isTypeSpecifier = true;
                            }
                        }
                    }
                    else if (ts_node_symbol(parent) == m_symMemberExpression)
                    {
                        // The grammar always names this field; positional fallbacks used to stand in for
                        // it here and would mis-identify the member in anything but the simplest access.
                        TSNode memberField = ts_node_child_by_field_name(parent, "member", static_cast<uint32_t>(strlen("member")));
                        ref.isMemberAccess = ts_node_eq(memberField, capture.node) ||
                            (!ts_node_is_null(memberField) && ts_node_start_byte(memberField) == ts_node_start_byte(capture.node));
                    }
                }

                current->references.push_back(std::move(ref));
            }
        }

        return root;
    }

    std::string LocalScopeCollector::GetNodeText(TSNode node, const std::string &sourceCode) const
    {
        if (ts_node_is_null(node))
            return "";

        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);

        if (start >= end || end > sourceCode.size())
            return "";

        return sourceCode.substr(start, end - start);
    }

    void LocalScopeCollector::ReadVariableTypeInfo(TSNode nameNode, const std::string &sourceCode, LocalDefinition &def) const
    {
        TSNode declaratorNode = ts_node_parent(nameNode);
        if (ts_node_is_null(declaratorNode))
            return;

        auto populateTypeRanges = [&](TSNode tNode)
        {
            TSPoint typeStart = ts_node_start_point(tNode);
            TSPoint typeEnd = ts_node_end_point(tNode);
            def.typeStartLine = typeStart.row;
            def.typeStartCharacter = typeStart.column;
            def.typeEndLine = typeEnd.row;
            def.typeEndCharacter = typeEnd.column;

            uint32_t count = ts_node_child_count(tNode);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(tNode, i);
                std::string_view childType(ts_node_type(child));
                if (childType == "datatype")
                {
                    TSPoint dtStart = ts_node_start_point(child);
                    TSPoint dtEnd = ts_node_end_point(child);
                    def.typeStartLine = dtStart.row;
                    def.typeStartCharacter = dtStart.column;
                    def.typeEndLine = dtEnd.row;
                    def.typeEndCharacter = dtEnd.column;
                }
                else if (childType == "template_type_list")
                {
                    uint32_t tCount = ts_node_named_child_count(child);
                    for (uint32_t t = 0; t < tCount; ++t)
                    {
                        TSNode innerType = ts_node_named_child(child, t);
                        TSPoint argStart = ts_node_start_point(innerType);
                        TSPoint argEnd = ts_node_end_point(innerType);
                        std::string argText = GetNodeText(innerType, sourceCode);
                        def.templateArgPositions.push_back({
                            CleanBaseType(argText),
                            argStart.row,
                            argStart.column,
                            argEnd.row,
                            argEnd.column
                        });
                    }
                }
            }
        };

        // A parameter carries its type on the same node as its name rather than on a parent
        // declarator, so it is read here and not below. Without it a parameter reached the scope
        // tree with no type at all, and every consumer that asks a scope what a name is - the
        // expression resolver, and so the access and const passes behind it - resolved nothing for
        // the most common object in any function body.
        if (ts_node_symbol(declaratorNode) == m_symParameter)
        {
            TSNode paramTypeNode = ts_node_child_by_field_name(declaratorNode, "param_type", static_cast<uint32_t>(strlen("param_type")));
            if (!ts_node_is_null(paramTypeNode))
            {
                TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(paramTypeNode, sourceCode);
                def.isHandleType = typeInfo.isHandle;
                def.typeKind = typeInfo.kind;
                def.typeName = GetNodeText(paramTypeNode, sourceCode);
                populateTypeRanges(paramTypeNode);
            }
            return;
        }

        // A foreach variable carries its type on the same node as its name, the way a parameter
        // does - `foreach (auto value : container)` puts `auto` in the foreach_variable's own `type`
        // field. Without this branch the function fell through the variable_declarator check below
        // and returned with no type at all, so the loop variable reached the scope tree unnamed by
        // any type: no hover, no completion after `value.`, and nothing for the expression resolver
        // to work from. The type read here is nearly always `auto`; ResolveForeachVariableTypes in
        // the analyzer replaces it with the container's element type.
        if (ts_node_symbol(declaratorNode) == m_symForeachVariable)
        {
            TSNode foreachTypeNode = ts_node_child_by_field_name(declaratorNode, "type", static_cast<uint32_t>(strlen("type")));
            if (!ts_node_is_null(foreachTypeNode))
            {
                TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(foreachTypeNode, sourceCode);
                def.isHandleType = typeInfo.isHandle;
                def.typeKind = typeInfo.kind;
                def.typeName = GetNodeText(foreachTypeNode, sourceCode);
                populateTypeRanges(foreachTypeNode);
            }
            return;
        }

        if (ts_node_symbol(declaratorNode) != m_symVariableDeclarator)
            return;

        TSNode declarationNode = ts_node_parent(declaratorNode);
        if (ts_node_is_null(declarationNode))
            return;

        TSNode typeNode = ts_node_child_by_field_name(declarationNode, "var_type", static_cast<uint32_t>(strlen("var_type")));
        if (ts_node_is_null(typeNode))
        {
            typeNode = ts_node_child_by_field_name(declarationNode, "type", static_cast<uint32_t>(strlen("type")));
        }
        if (ts_node_is_null(typeNode) && ts_node_named_child_count(declarationNode) > 0)
        {
            TSNode firstChild = ts_node_named_child(declarationNode, 0);
            if (!ts_node_eq(firstChild, declaratorNode))
            {
                typeNode = firstChild;
            }
        }
        if (!ts_node_is_null(typeNode))
        {
            TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, sourceCode);
            def.isHandleType = typeInfo.isHandle;
            def.typeKind = typeInfo.kind;
            def.typeName = GetNodeText(typeNode, sourceCode);
            populateTypeRanges(typeNode);
        }

        // variable_declarator's '=' RHS isn't exposed as a named field in this grammar (confirmed
        // via node-types.json - "value"/"initializer" checks below are defensive, matching
        // SymbolCollector::ProcessVariable's identical fallback scan) - so find it by scanning for
        // the '=' token and taking the next child.
        TSNode valueNode = ts_node_child_by_field_name(declaratorNode, "value", static_cast<uint32_t>(strlen("value")));
        if (ts_node_is_null(valueNode))
            valueNode = ts_node_child_by_field_name(declaratorNode, "initializer", static_cast<uint32_t>(strlen("initializer")));
        if (ts_node_is_null(valueNode))
        {
            uint32_t childCount = ts_node_child_count(declaratorNode);
            bool foundEq = false;
            for (uint32_t i = 0; i < childCount; ++i)
            {
                TSNode child = ts_node_child(declaratorNode, i);
                if (foundEq)
                {
                    valueNode = child;
                    break;
                }
                if (GetNodeText(child, sourceCode) == "=")
                    foundEq = true;
            }
        }

        def.hasNullInitializer = IsNullInitializer(valueNode);
    }
}
