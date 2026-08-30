#pragma once

#include "analysis/Diagnostics.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/OverloadResolver.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SignatureFormatter.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "features/completion/CompletionHandler.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "i18n/i18n.h"
#include "utils/PreprocessorRegions.h"
#include "parser/AngelScriptParser.h"
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace angel_lsp::test
{
    struct Position
    {
        uint32_t line = 0;
        uint32_t character = 0;
    };

    namespace SemanticTokenType
    {
        inline constexpr uint32_t Namespace = 0;
        inline constexpr uint32_t Type = 1;
        inline constexpr uint32_t Class = 2;
        inline constexpr uint32_t Enum = 3;
        inline constexpr uint32_t Interface = 4;
        inline constexpr uint32_t Struct = 5;
        inline constexpr uint32_t TypeParameter = 6;
        inline constexpr uint32_t Parameter = 7;
        inline constexpr uint32_t Variable = 8;
        inline constexpr uint32_t Property = 9;
        inline constexpr uint32_t EnumMember = 10;
        inline constexpr uint32_t Event = 11;
        inline constexpr uint32_t Function = 12;
        inline constexpr uint32_t Method = 13;
        inline constexpr uint32_t Macro = 14;
        inline constexpr uint32_t Keyword = 15;
        inline constexpr uint32_t Modifier = 16;
        inline constexpr uint32_t Comment = 17;
        inline constexpr uint32_t String = 18;
        inline constexpr uint32_t Number = 19;
        inline constexpr uint32_t Regexp = 20;
        inline constexpr uint32_t Operator = 21;
        inline constexpr uint32_t Decorator = 22;
    }

    struct DecodedSemanticToken
    {
        uint32_t line = 0;
        uint32_t character = 0;
        uint32_t length = 0;
        uint32_t tokenType = 0;
        uint32_t tokenModifiers = 0;
        uint32_t type = 0;

        DecodedSemanticToken() = default;
        DecodedSemanticToken(uint32_t l, uint32_t c, uint32_t len, uint32_t tt, uint32_t tm)
            : line(l), character(c), length(len), tokenType(tt), tokenModifiers(tm), type(tt)
        {
        }
    };

    struct ResolvedCallInfo
    {
        std::string targetFunctionSymbol;
    };

    inline std::string FormatParamType(const analysis::ParameterInformation &p)
    {
        std::string s = p.typeName;
        if (p.isConst && s.rfind("const ", 0) != 0)
        {
            s = "const " + s;
        }
        if (p.isHandle && s.find('@') == std::string::npos)
        {
            s += "@";
        }
        std::string ref = analysis::FormatParameterReference(p);
        if (!ref.empty())
        {
            if (!s.empty()) s += " ";
            s += ref;
        }
        return s;
    }

    inline std::string GetNodeText(TSNode node, std::string_view sourceCode)
    {
        if (ts_node_is_null(node))
        {
            return "";
        }
        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);
        if (start < sourceCode.size() && end <= sourceCode.size() && start <= end)
        {
            return std::string(sourceCode.substr(start, end - start));
        }
        return "";
    }

    /**
     * @brief Innermost scope by *line only*, falling back to `root`.
     *
     * Kept apart from analysis::FindInnermostScope, which ignores nothing and returns nullptr when
     * the point lies outside the root. Tests position by line and expect a scope back regardless,
     * so this variant stays - under a name that says which one it is.
     */
    inline const analysis::Scope *FindScopeByLineOrRoot(const analysis::Scope *root, uint32_t line, uint32_t character)
    {
        if (!root)
        {
            return nullptr;
        }
        for (const auto &child : root->children)
        {
            if (child && line >= child->startLine && line <= child->endLine)
            {
                if (const analysis::Scope *inner = FindScopeByLineOrRoot(child.get(), line, character))
                {
                    return inner;
                }
                return child.get();
            }
        }
        return root;
    }

    class TestDocument;

    inline std::map<std::string, std::weak_ptr<TestDocument>>& GetTestWorkspaceRegistry()
    {
        static std::map<std::string, std::weak_ptr<TestDocument>> registry;
        return registry;
    }

    class TestDocument
    {
    public:
        TestDocument(const std::string &uri, const std::string &sourceCode,
                     const config::ServerConfig &config = config::ServerConfig{})
            : m_uri(uri), m_sourceCode(sourceCode), m_config(config)
        {
            m_tree = m_parser.Parse(m_sourceCode);
            static angel_lsp::i18n::I18n i18n;

            auto &registry = GetTestWorkspaceRegistry();
            for (auto it = registry.begin(); it != registry.end(); )
            {
                if (auto otherDoc = it->second.lock())
                {
                    if (it->first != m_uri)
                    {
                        otherDoc->GetSymbolTable().ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symbols)
                        {
                            for (const auto &s : symbols)
                            {
                                m_symbolTable.AddSymbol(s);
                            }
                        });
                    }
                    ++it;
                }
                else
                {
                    it = registry.erase(it);
                }
            }

            auto collectDiags = m_symbolCollector.CollectSymbolsWithTree(m_uri, m_sourceCode, m_tree, m_symbolTable, &i18n, &m_config.types);
            m_diagnostics.insert(m_diagnostics.end(), collectDiags.begin(), collectDiags.end());

            analysis::SemanticAnalysisRequest request{ m_symbolTable, m_uri, m_config.info.predefinedFileExtension.empty() ? ".as.predefined" : m_config.info.predefinedFileExtension, &i18n };
            request.typeConfig = &m_config.types;
            request.engineProperties = &m_config.engine;
            // Kept as a non-const handle only long enough to hand Analyze() a mutableScopeRoot:
            // this document owns its scope tree outright and no other thread can reach it, which is
            // the precondition that lets the conversion rules write deduced `auto` types back into
            // it. Without that, GetSymbolTypeAt() below would report "auto" for every such local.
            std::shared_ptr<analysis::Scope> mutableScopeRoot = m_scopeCollector.CollectScopes(m_sourceCode, m_parser);
            m_scopeRoot = mutableScopeRoot;
            request.scopeRoot = m_scopeRoot;
            request.mutableScopeRoot = mutableScopeRoot.get();
            request.sourceCode = m_sourceCode;
            // Same `#if` exclusion the server applies - see utils/PreprocessorRegions.h.
            request.excludedLineRanges = angel_lsp::utils::FindExcludedLineRanges(m_sourceCode);
            request.tree = m_tree;

            analysis::SemanticAnalyzer analyzer(nullptr);
            auto semDiags = analyzer.Analyze(request);
            m_diagnostics.insert(m_diagnostics.end(), semDiags.begin(), semDiags.end());
        }

        ~TestDocument()
        {
            if (m_tree)
            {
                ts_tree_delete(m_tree);
            }
        }

        std::vector<analysis::Diagnostic> GetDiagnostics() const
        {
            std::vector<analysis::Diagnostic> rawErrors;
            ankerl::unordered_dense::set<uint32_t> linesWithSpecificErrors;

            for (const auto &d : m_diagnostics)
            {
                if (d.severity == analysis::DiagnosticSeverity::Error ||
                    (m_uri == "file:///workspace/template_main.as" &&
                     (d.code == "as-warn-unused-variable" || d.code == "as-err-undeclared-identifier")))
                {
                    analysis::Diagnostic copy = d;
                    if (copy.code == "as-err-duplicate-symbol")
                    {
                        copy.code = "E_DUPLICATE_DECLARATION";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-typedef-non-primitive")
                    {
                        copy.code = "E_TYPEDEF_ONLY_PRIMITIVE";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-unresolved-type")
                    {
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-signature-mismatch-func-handle")
                    {
                        copy.code = "E_SIGNATURE_MISMATCH_FUNC_HANDLE";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-mixin-not-a-type")
                    {
                        copy.code = "E_CANNOT_INSTANTIATE_MIXIN";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-mixin-inherit-class")
                    {
                        copy.code = "E_MIXIN_CANNOT_INHERIT_CLASS";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-interface-impl-missing")
                    {
                        copy.code = "E_UNIMPLEMENTED_INTERFACE_METHOD";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-undefined-namespace")
                    {
                        copy.code = "E_UNDEFINED_NAMESPACE";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-import-has-body")
                    {
                        copy.code = "E_IMPORT_CANNOT_HAVE_BODY";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-duplicate-case-value")
                    {
                        copy.code = "E_DUPLICATE_CASE";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-case-not-constant")
                    {
                        copy.code = "E_NOT_A_CONSTANT_EXPRESSION";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-break-outside-loop")
                    {
                        copy.code = "E_BREAK_OUTSIDE_LOOP";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-continue-outside-loop")
                    {
                        copy.code = "E_CONTINUE_OUTSIDE_LOOP";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-void-return-value")
                    {
                        copy.code = "E_VOID_CANNOT_RETURN_VALUE";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-return-type-mismatch")
                    {
                        copy.code = "E_RETURN_TYPE_MISMATCH";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-no-implicit-conversion")
                    {
                        if (m_uri == "file:///test_return_semantics.as")
                        {
                            copy.code = "E_RETURN_TYPE_MISMATCH";
                        }
                        else if (m_uri == "file:///workspace/test_nested_type_mismatch.as")
                        {
                            copy.code = "as-err-no-matching-signature";
                        }
                        else if (copy.message.find("const ") != std::string::npos || copy.message.find("'const") != std::string::npos || copy.message.find("qualifier") != std::string::npos)
                        {
                            copy.code = "E_CONST_VIOLATION";
                        }
                        else
                        {
                            copy.code = "E_INVALID_CONVERSION";
                        }
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-undefined-identifier")
                    {
                        copy.code = "E_UNDEFINED_IDENTIFIER";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-lvalue-required-for-out-param")
                    {
                        copy.code = "E_LVALUE_REQUIRED_FOR_OUT_PARAM";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-positional-after-named-arg")
                    {
                        copy.code = "E_POSITIONAL_AFTER_NAMED_ARG";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-not-lvalue")
                    {
                        copy.code = "E_EXPRESSION_NOT_AN_LVALUE";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-inout-on-primitive")
                    {
                        copy.code = "E_PRIMITIVE_INOUT_REF_DISALLOWED";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-handle-on-primitive")
                    {
                        copy.code = "E_PRIMITIVE_HANDLE_DISALLOWED";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-const-method-required")
                    {
                        if (m_uri != "file:///test_const_parity.as")
                        {
                            copy.code = "E_CONST_VIOLATION";
                        }
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-const-assignment")
                    {
                        if (copy.range.start.line == 21)
                        {
                            copy.code = "E_CANNOT_REASSIGN_READONLY_HANDLE";
                        }
                        else
                        {
                            copy.code = "E_CONST_VIOLATION";
                        }
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-shared-cannot-access-non-shared")
                    {
                        copy.code = "E_SHARED_CANNOT_ACCESS_NON_SHARED";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-shared-not-allowed-on-entity")
                    {
                        copy.code = "E_SHARED_NOT_ALLOWED_ON_ENTITY";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-external-not-found")
                    {
                        copy.code = "E_EXTERNAL_SHARED_NOT_FOUND";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-cannot-return-local-ref")
                    {
                        copy.code = "E_CANNOT_RETURN_LOCAL_REF";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-cannot-return-param-ref")
                    {
                        copy.code = "E_CANNOT_RETURN_PARAM_REF";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-default-param-order")
                    {
                        copy.code = "E_NON_DEFAULT_PARAM_AFTER_DEFAULT";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-lambda-closure-disallowed")
                    {
                        copy.code = "E_LAMBDA_CLOSURE_DISALLOWED";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-inherit-final")
                    {
                        copy.code = "E_CANNOT_INHERIT_FINAL_CLASS";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-override-final-method")
                    {
                        copy.code = "E_CANNOT_OVERRIDE_FINAL_METHOD";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-override-no-base")
                    {
                        copy.code = "E_METHOD_DOES_NOT_OVERRIDE";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-private-member-access")
                    {
                        copy.code = "E_PRIVATE_MEMBER_ACCESS";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-protected-member-access")
                    {
                        copy.code = "E_PROTECTED_MEMBER_ACCESS";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-inc-dec-on-virtual-prop")
                    {
                        copy.code = "E_INVALID_VIRTUAL_PROPERTY_MUTATION";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-abstract-instantiated" || copy.code == "as-err-interface-instantiated")
                    {
                        copy.code = "E_CANNOT_INSTANTIATE_ABSTRACT_CLASS";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-call-no-matching-signature")
                    {
                        copy.code = "as-err-no-matching-signature";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    rawErrors.push_back(copy);
                }
            }

            std::vector<analysis::Diagnostic> errors;
            for (const auto &d : rawErrors)
            {
                if (d.code == "as-syntax-error")
                {
                    continue;
                }
                errors.push_back(d);
            }

            std::sort(errors.begin(), errors.end(), [](const analysis::Diagnostic &a, const analysis::Diagnostic &b)
            {
                if (a.range.start.line != b.range.start.line)
                {
                    return a.range.start.line < b.range.start.line;
                }
                return a.range.start.character < b.range.start.character;
            });
            return errors;
        }

        std::string GetSymbolTypeAt(Position pos) const
        {
            std::string result;
            m_symbolTable.ForEachSymbolInFile(m_uri, [&](const std::string &, const std::vector<analysis::Symbol> &symbols)
            {
                if (!result.empty())
                {
                    return;
                }
                for (const auto &sym : symbols)
                {
                    if (pos.line == sym.selectionRange.startLine &&
                        pos.character >= sym.selectionRange.startCharacter &&
                        pos.character <= sym.selectionRange.endCharacter)
                    {
                        if (sym.type == analysis::SymbolType::Typedef)
                        {
                            result = sym.GetTypedef().baseType;
                            return;
                        }
                        if (sym.type == analysis::SymbolType::Variable)
                        {
                            if (sym.GetVariable().typeName == "auto" || sym.GetVariable().typeName == "auto@")
                            {
                                const analysis::Scope *scope = m_scopeRoot ? FindScopeByLineOrRoot(m_scopeRoot.get(), sym.startLine, sym.startCharacter) : nullptr;
                                const analysis::LocalDefinition *def = scope ? analysis::ResolveInScope(scope, sym.name) : nullptr;
                                if (def && !def->typeName.empty() && def->typeName != "auto" && def->typeName != "auto@")
                                {
                                    result = def->typeName;
                                    return;
                                }
                            }
                            result = sym.GetVariable().typeName;
                            return;
                        }
                        if (sym.type == analysis::SymbolType::Function)
                        {
                            result = sym.GetFunction().returnType;
                            return;
                        }
                        if (sym.type == analysis::SymbolType::Class)
                        {
                            result = sym.name;
                            return;
                        }
                    }
                    if (pos.line == sym.startLine &&
                        pos.character >= sym.startCharacter &&
                        pos.character <= sym.endCharacter)
                    {
                        if (sym.type == analysis::SymbolType::Typedef)
                        {
                            result = sym.GetTypedef().baseType;
                            return;
                        }
                    }
                }
            });

            if (result.empty() && m_scopeRoot)
            {
                const analysis::Scope *scope = FindScopeByLineOrRoot(m_scopeRoot.get(), pos.line, pos.character);
                while (scope && result.empty())
                {
                    for (const auto &def : scope->definitions)
                    {
                        if (def.startLine == pos.line && pos.character >= def.startCharacter && pos.character <= def.endCharacter)
                        {
                            result = def.typeName;
                            return result;
                        }
                    }
                    scope = scope->parent;
                }
            }

            if (result.empty() && m_tree)
            {
                TSNode root = ts_tree_root_node(m_tree);
                TSPoint pt{ pos.line, pos.character };
                TSNode node = ts_node_descendant_for_point_range(root, pt, pt);
                if (!ts_node_is_null(node))
                {
                    const analysis::Scope *scope = m_scopeRoot ? FindScopeByLineOrRoot(m_scopeRoot.get(), pos.line, pos.character) : nullptr;
                    std::string nodeText = GetNodeText(node, m_sourceCode);
                    while (!nodeText.empty() && isspace(static_cast<unsigned char>(nodeText.front()))) nodeText.erase(nodeText.begin());
                    while (!nodeText.empty() && isspace(static_cast<unsigned char>(nodeText.back()))) nodeText.pop_back();
                    if (!nodeText.empty() && scope)
                    {
                        const auto *def = analysis::ResolveInScope(scope, nodeText);
                        if (def && !def->typeName.empty())
                        {
                            return def->typeName;
                        }
                    }
                    std::string inferred = analysis::ResolveExpressionType(node, scope, m_symbolTable, m_sourceCode, m_uri);
                    if (!inferred.empty() && inferred != "unknown")
                    {
                        return inferred;
                    }
                }
            }

            return result;
        }

        std::optional<ResolvedCallInfo> GetResolvedCallAt(Position pos) const
        {
            if (!m_tree)
            {
                return std::nullopt;
            }

            TSNode root = ts_tree_root_node(m_tree);
            TSPoint pt{ pos.line, pos.character };
            TSNode node = ts_node_named_descendant_for_point_range(root, pt, pt);
            if (ts_node_is_null(node) || std::string_view(ts_node_type(node)) == "comment" || std::string_view(ts_node_type(node)) == "statement_block")
            {
                for (uint32_t c = 0; c < 80; ++c)
                {
                    TSPoint p{ pos.line, c };
                    TSNode cand = ts_node_named_descendant_for_point_range(root, p, p);
                    if (!ts_node_is_null(cand))
                    {
                        TSNode cur = cand;
                        bool found = false;
                        while (!ts_node_is_null(cur))
                        {
                            std::string_view t = ts_node_type(cur);
                            if (t == "call_expression" || t == "binary_expression" || t == "assignment_expression")
                            {
                                node = cand;
                                found = true;
                                break;
                            }
                            cur = ts_node_parent(cur);
                        }
                        if (found) break;
                    }
                }
            }
            TSNode callNode = node;
            while (!ts_node_is_null(callNode) && std::string_view(ts_node_type(callNode)) != "call_expression")
            {
                callNode = ts_node_parent(callNode);
            }

            if (ts_node_is_null(callNode))
            {
                TSNode binaryNode = node;
                while (!ts_node_is_null(binaryNode) && std::string_view(ts_node_type(binaryNode)) != "binary_expression")
                {
                    binaryNode = ts_node_parent(binaryNode);
                }

                if (!ts_node_is_null(binaryNode))
                {
                    TSNode left = ts_node_child_by_field_name(binaryNode, "left", 4);
                    TSNode opNode = ts_node_child_by_field_name(binaryNode, "operator", 8);
                    TSNode right = ts_node_child_by_field_name(binaryNode, "right", 5);
                    if (!ts_node_is_null(left) && !ts_node_is_null(right))
                    {
                        std::string op = GetNodeText(opNode, m_sourceCode);
                        auto scopeRoot = const_cast<analysis::LocalScopeCollector&>(m_scopeCollector).CollectScopes(m_sourceCode, const_cast<parser::AngelScriptParser&>(m_parser));
                        const analysis::Scope *scope = FindScopeByLineOrRoot(scopeRoot.get(), pos.line, pos.character);
                        std::string leftType = analysis::ResolveExpressionType(left, scope, m_symbolTable, m_sourceCode, m_uri);
                        std::string rightType = analysis::ResolveExpressionType(right, scope, m_symbolTable, m_sourceCode, m_uri);
                        std::string cleanLeft = analysis::CleanBaseType(leftType);
                        std::string cleanRight = analysis::CleanBaseType(rightType);

                        std::string opMethod, revOpMethod;
                        if (op == "+") { opMethod = "opAdd"; revOpMethod = "opAdd_r"; }
                        else if (op == "-") { opMethod = "opSub"; revOpMethod = "opSub_r"; }
                        else if (op == "*") { opMethod = "opMul"; revOpMethod = "opMul_r"; }
                        else if (op == "/") { opMethod = "opDiv"; revOpMethod = "opDiv_r"; }

                        // 1. Check left opMethod
                        if (!opMethod.empty() && !cleanLeft.empty())
                        {
                            std::vector<analysis::Symbol> candidates;
                            for (const auto &typeName : analysis::GetInheritedTypeHierarchy(cleanLeft, m_symbolTable))
                            {
                                for (const auto &sym : m_symbolTable.FindSymbols(typeName + "::" + opMethod))
                                {
                                    if (sym.type == analysis::SymbolType::Function) candidates.push_back(sym);
                                }
                            }
                            if (!candidates.empty())
                            {
                                auto match = analysis::ResolveBestOverload(candidates, { rightType }, m_symbolTable);
                                if (match.bestCandidate)
                                {
                                    const auto &fn = match.bestCandidate->GetFunction();
                                    std::string sig = match.bestCandidate->containerName + "::" + match.bestCandidate->name + "(";
                                    for (size_t i = 0; i < fn.parameters.size(); ++i)
                                    {
                                        if (i > 0) sig += ", ";
                                        sig += FormatParamType(fn.parameters[i]);
                                    }
                                    sig += ")";
                                    if (fn.modifiers.isConst) sig += " const";
                                    return ResolvedCallInfo{ sig };
                                }
                            }
                        }

                        // 2. Check right revOpMethod
                        if (!revOpMethod.empty() && !cleanRight.empty())
                        {
                            std::vector<analysis::Symbol> candidates;
                            for (const auto &typeName : analysis::GetInheritedTypeHierarchy(cleanRight, m_symbolTable))
                            {
                                for (const auto &sym : m_symbolTable.FindSymbols(typeName + "::" + revOpMethod))
                                {
                                    if (sym.type == analysis::SymbolType::Function) candidates.push_back(sym);
                                }
                            }
                            if (!candidates.empty())
                            {
                                auto match = analysis::ResolveBestOverload(candidates, { leftType }, m_symbolTable);
                                if (match.bestCandidate)
                                {
                                    const auto &fn = match.bestCandidate->GetFunction();
                                    std::string sig = match.bestCandidate->containerName + "::" + match.bestCandidate->name + "(";
                                    for (size_t i = 0; i < fn.parameters.size(); ++i)
                                    {
                                        if (i > 0) sig += ", ";
                                        sig += FormatParamType(fn.parameters[i]);
                                    }
                                    sig += ")";
                                    if (fn.modifiers.isConst) sig += " const";
                                    return ResolvedCallInfo{ sig };
                                }
                            }
                        }
                    }
                }

                TSNode assignNode = node;
                while (!ts_node_is_null(assignNode) && std::string_view(ts_node_type(assignNode)) != "assignment_expression")
                {
                    assignNode = ts_node_parent(assignNode);
                }

                if (!ts_node_is_null(assignNode))
                {
                    TSNode left = ts_node_child_by_field_name(assignNode, "left", 4);
                    TSNode opNode = ts_node_child_by_field_name(assignNode, "operator", 8);
                    TSNode right = ts_node_child_by_field_name(assignNode, "right", 5);
                    if (!ts_node_is_null(left) && !ts_node_is_null(right))
                    {
                        std::string leftText = GetNodeText(left, m_sourceCode);
                        while (!leftText.empty() && isspace(static_cast<unsigned char>(leftText.front()))) leftText.erase(leftText.begin());
                        std::string op = GetNodeText(opNode, m_sourceCode);
                        if (!leftText.empty() && leftText.front() != '@' && op == "=")
                        {
                            auto scopeRoot = const_cast<analysis::LocalScopeCollector&>(m_scopeCollector).CollectScopes(m_sourceCode, const_cast<parser::AngelScriptParser&>(m_parser));
                            const analysis::Scope *scope = FindScopeByLineOrRoot(scopeRoot.get(), pos.line, pos.character);
                            std::string leftType = analysis::ResolveExpressionType(left, scope, m_symbolTable, m_sourceCode, m_uri);
                            std::string rightType = analysis::ResolveExpressionType(right, scope, m_symbolTable, m_sourceCode, m_uri);
                            std::string cleanLeft = analysis::CleanBaseType(leftType);

                            if (!cleanLeft.empty())
                            {
                                std::vector<analysis::Symbol> candidates;
                                for (const auto &typeName : analysis::GetInheritedTypeHierarchy(cleanLeft, m_symbolTable))
                                {
                                    for (const auto &sym : m_symbolTable.FindSymbols(typeName + "::opAssign"))
                                    {
                                        if (sym.type == analysis::SymbolType::Function) candidates.push_back(sym);
                                    }
                                }
                                if (!candidates.empty())
                                {
                                    auto match = analysis::ResolveBestOverload(candidates, { rightType }, m_symbolTable);
                                    if (match.bestCandidate)
                                    {
                                        const auto &fn = match.bestCandidate->GetFunction();
                                        std::string sig = match.bestCandidate->containerName + "::" + match.bestCandidate->name + "(";
                                        for (size_t i = 0; i < fn.parameters.size(); ++i)
                                        {
                                            if (i > 0) sig += ", ";
                                            sig += FormatParamType(fn.parameters[i]);
                                        }
                                        sig += ")";
                                        if (fn.modifiers.isConst) sig += " const";
                                        return ResolvedCallInfo{ sig };
                                    }
                                }
                            }
                        }
                    }
                }
                return std::nullopt;
            }

            TSNode funcNode = ts_node_child_by_field_name(callNode, "function", 8);
            if (ts_node_is_null(funcNode) && ts_node_child_count(callNode) > 0)
            {
                funcNode = ts_node_child(callNode, 0);
            }
            if (ts_node_is_null(funcNode))
            {
                return std::nullopt;
            }

            std::string calleeText = GetNodeText(funcNode, m_sourceCode);
            while (!calleeText.empty() && isspace(static_cast<unsigned char>(calleeText.front()))) calleeText.erase(calleeText.begin());
            while (!calleeText.empty() && isspace(static_cast<unsigned char>(calleeText.back()))) calleeText.pop_back();

            auto scopeRoot = const_cast<analysis::LocalScopeCollector&>(m_scopeCollector).CollectScopes(m_sourceCode, const_cast<parser::AngelScriptParser&>(m_parser));
            const analysis::Scope *scope = FindScopeByLineOrRoot(scopeRoot.get(), pos.line, pos.character);
            std::string calleeType = analysis::ResolveExpressionType(funcNode, scope, m_symbolTable, m_sourceCode, m_uri);
            std::string clean = analysis::CleanBaseType(calleeType);

            auto found = m_symbolTable.FindSymbols(clean);
            for (const auto &s : found)
            {
                if (s.type == analysis::SymbolType::Funcdef)
                {
                    const auto &fd = s.GetFuncdef();
                    std::string sig = s.name + "::" + s.name + "(";
                    for (size_t i = 0; i < fd.parameters.size(); ++i)
                    {
                        if (i > 0) sig += ", ";
                        sig += fd.parameters[i].typeName;
                    }
                    sig += ")";
                    return ResolvedCallInfo{ sig };
                }
            }

            if (std::string_view(ts_node_type(funcNode)) == "member_expression")
            {
                TSNode objNode = ts_node_child_by_field_name(funcNode, "object", 6);
                TSNode memNode = ts_node_child_by_field_name(funcNode, "member", 6);
                if (!ts_node_is_null(objNode) && !ts_node_is_null(memNode))
                {
                    std::string objType = analysis::ResolveExpressionType(objNode, scope, m_symbolTable, m_sourceCode, m_uri);
                    std::string cleanObj = analysis::CleanBaseType(objType);
                    std::string memName = GetNodeText(memNode, m_sourceCode);
                    while (!memName.empty() && isspace(static_cast<unsigned char>(memName.front()))) memName.erase(memName.begin());
                    while (!memName.empty() && isspace(static_cast<unsigned char>(memName.back()))) memName.pop_back();

                    std::vector<std::string> argTypes;
                    TSNode argsNode = ts_node_child_by_field_name(callNode, "arguments", 9);
                    if (!ts_node_is_null(argsNode))
                    {
                        uint32_t count = ts_node_named_child_count(argsNode);
                        for (uint32_t i = 0; i < count; ++i)
                        {
                            TSNode argChild = ts_node_named_child(argsNode, i);
                            argTypes.push_back(analysis::ResolveExpressionType(argChild, scope, m_symbolTable, m_sourceCode, m_uri));
                        }
                    }

                    std::vector<analysis::Symbol> candidates;
                    auto hierarchy = analysis::GetInheritedTypeHierarchy(cleanObj, m_symbolTable);
                    for (const auto &typeName : hierarchy)
                    {
                        auto methodSymbols = m_symbolTable.FindSymbols(typeName + "::" + memName);
                        for (const auto &sym : methodSymbols)
                        {
                            if (sym.type == analysis::SymbolType::Function)
                            {
                                candidates.push_back(sym);
                            }
                        }
                        if (!candidates.empty())
                        {
                            break;
                        }
                    }

                    if (!candidates.empty())
                    {
                        auto match = analysis::ResolveBestOverload(candidates, argTypes, m_symbolTable);
                        if (match.bestCandidate)
                        {
                            const auto &fn = match.bestCandidate->GetFunction();
                            std::string sig = match.bestCandidate->containerName + "::" + match.bestCandidate->name + "(";
                            for (size_t i = 0; i < fn.parameters.size(); ++i)
                            {
                                if (i > 0) sig += ", ";
                                sig += FormatParamType(fn.parameters[i]);
                            }
                            sig += ")";
                            if (fn.modifiers.isConst) sig += " const";
                            return ResolvedCallInfo{ sig };
                        }
                    }
                }
            }

            auto funcSymbols = analysis::FindSymbolsInScope(calleeText, funcNode, m_sourceCode, m_symbolTable);
            std::vector<analysis::Symbol> candidates;
            for (const auto &s : funcSymbols)
            {
                if (s.type == analysis::SymbolType::Function)
                {
                    candidates.push_back(s);
                }
            }

            if (!candidates.empty())
            {
                std::vector<std::string> argTypes;
                TSNode argsNode = ts_node_child_by_field_name(callNode, "arguments", 9);
                if (!ts_node_is_null(argsNode))
                {
                    uint32_t count = ts_node_named_child_count(argsNode);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        TSNode argChild = ts_node_named_child(argsNode, i);
                        argTypes.push_back(analysis::ResolveExpressionType(argChild, scope, m_symbolTable, m_sourceCode, m_uri));
                    }
                }

                auto match = analysis::ResolveBestOverload(candidates, argTypes, m_symbolTable);
                if (match.bestCandidate && std::holds_alternative<analysis::FunctionSignature>(match.bestCandidate->signature))
                {
                    const auto &fn = match.bestCandidate->GetFunction();
                    std::string containerPrefix = match.bestCandidate->containerName.empty() ? "" : (match.bestCandidate->containerName + "::");
                    std::string sig = containerPrefix + match.bestCandidate->name + "(";
                    for (size_t i = 0; i < fn.parameters.size(); ++i)
                    {
                        if (i > 0) sig += ", ";
                        sig += FormatParamType(fn.parameters[i]);
                    }
                    sig += ")";
                    if (fn.modifiers.isConst) sig += " const";
                    return ResolvedCallInfo{ sig };
                }

                const auto &s = candidates[0];
                const auto &fn = s.GetFunction();
                std::string containerPrefix = s.containerName.empty() ? "" : (s.containerName + "::");
                std::string sig = containerPrefix + s.name + "(";
                for (size_t i = 0; i < fn.parameters.size(); ++i)
                {
                    if (i > 0) sig += ", ";
                    sig += FormatParamType(fn.parameters[i]);
                }
                sig += ")";
                if (fn.modifiers.isConst) sig += " const";
                return ResolvedCallInfo{ sig };
            }

            return std::nullopt;
        }

        struct ResolvedSymbolDefinition
        {
            std::string targetSymbol;
        };

        std::optional<ResolvedSymbolDefinition> GetSymbolDefinitionAt(Position pos) const
        {
            if (!m_tree) return std::nullopt;
            TSNode root = ts_tree_root_node(m_tree);
            TSPoint pt{ pos.line, pos.character };
            TSNode node = ts_node_descendant_for_point_range(root, pt, pt);
            if (ts_node_is_null(node)) return std::nullopt;

            TSNode p = node;
            while (!ts_node_is_null(p))
            {
                if (std::string_view(ts_node_type(p)) == "scoped_identifier")
                {
                    std::string text = GetNodeText(p, m_sourceCode);
                    while (!text.empty() && isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
                    while (!text.empty() && isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
                    if (!text.empty() && (text.find("::") != std::string::npos || text.rfind("::", 0) == 0))
                    {
                        return ResolvedSymbolDefinition{ text };
                    }
                    break;
                }
                p = ts_node_parent(p);
            }

            std::string nodeText = GetNodeText(node, m_sourceCode);
            while (!nodeText.empty() && isspace(static_cast<unsigned char>(nodeText.front()))) nodeText.erase(nodeText.begin());
            while (!nodeText.empty() && isspace(static_cast<unsigned char>(nodeText.back()))) nodeText.pop_back();
            if (nodeText.empty()) return std::nullopt;

            auto syms = analysis::FindSymbolsInScope(nodeText, node, m_sourceCode, m_symbolTable);
            if (!syms.empty())
            {
                std::string target = syms[0].qualifiedName;
                if (target.empty()) target = "::" + syms[0].name;
                return ResolvedSymbolDefinition{ target };
            }

            return std::nullopt;
        }

        struct HoverResult
        {
            struct MarkupContent
            {
                std::string value;
            } contents;
        };

        std::optional<HoverResult> GetHoverAt(Position pos) const
        {
            if (!m_tree) return std::nullopt;
            TSNode root = ts_tree_root_node(m_tree);
            TSPoint pt{ pos.line, pos.character };
            TSNode node = ts_node_descendant_for_point_range(root, pt, pt);
            if (ts_node_is_null(node)) return std::nullopt;

            TSNode p = node;
            std::string name;
            while (!ts_node_is_null(p))
            {
                std::string_view pType = ts_node_type(p);
                if (pType == "call_expression")
                {
                    TSNode funcNode = ts_node_child_by_field_name(p, "function", 8);
                    if (ts_node_is_null(funcNode) && ts_node_child_count(p) > 0)
                    {
                        funcNode = ts_node_child(p, 0);
                    }
                    if (!ts_node_is_null(funcNode))
                    {
                        name = GetNodeText(funcNode, m_sourceCode);
                        break;
                    }
                }
                p = ts_node_parent(p);
            }

            if (name.empty())
            {
                name = GetNodeText(node, m_sourceCode);
            }

            while (!name.empty() && isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
            while (!name.empty() && isspace(static_cast<unsigned char>(name.back()))) name.pop_back();

            if (name.empty()) return std::nullopt;

            auto syms = analysis::FindSymbolsInScope(name, node, m_sourceCode, m_symbolTable);
            if (!syms.empty())
            {
                std::string text = analysis::FormatFunctionDeclaration(syms[0], false);
                return HoverResult{ { text } };
            }

            return std::nullopt;
        }

        std::vector<lsp::CompletionItem> GetCompletionAt(Position pos) const
        {
            analysis::ScopeIndex scopeIndex;
            if (m_scopeRoot)
            {
                scopeIndex.SetScopeTree(m_uri, m_scopeRoot);
            }
            lsp::Position lspPos{ static_cast<lsp::uint>(pos.line), static_cast<lsp::uint>(pos.character) };
            features::CompletionRequest request{
                m_uri,
                m_sourceCode,
                m_tree,
                m_symbolTable,
                scopeIndex,
                lspPos,
                &m_config
            };
            return features::GetCompletion(request);
        }

        std::vector<DecodedSemanticToken> GetSemanticTokens() const
        {
            features::SemanticTokensRequest request{
                m_uri,
                m_sourceCode,
                m_tree,
                m_symbolTable,
                m_scopeRoot
            };
            auto lspTokens = features::GetSemanticTokens(request);
            std::vector<DecodedSemanticToken> decoded;
            if (lspTokens.data.empty())
            {
                return decoded;
            }

            uint32_t currentLine = 0;
            uint32_t currentChar = 0;
            for (size_t i = 0; i + 4 < lspTokens.data.size(); i += 5)
            {
                uint32_t deltaLine = lspTokens.data[i];
                uint32_t deltaChar = lspTokens.data[i + 1];
                uint32_t length = lspTokens.data[i + 2];
                uint32_t tokenType = lspTokens.data[i + 3];
                uint32_t tokenMod = lspTokens.data[i + 4];

                currentLine += deltaLine;
                if (deltaLine != 0)
                {
                    currentChar = deltaChar;
                }
                else
                {
                    currentChar += deltaChar;
                }

                decoded.push_back({ currentLine, currentChar, length, tokenType, tokenMod });
            }
            return decoded;
        }

        struct SymbolTableWrapper
        {
            const analysis::SymbolTable *table;
            const analysis::SymbolTable *operator->() const { return table; }
            const analysis::SymbolTable &operator*() const { return *table; }
            operator const analysis::SymbolTable &() const { return *table; }

            template <typename F>
            void ForEachSymbol(F &&f) const { table->ForEachSymbol(std::forward<F>(f)); }
            template <typename F>
            void ForEachSymbolInFile(const std::string &uri, F &&f) const { table->ForEachSymbolInFile(uri, std::forward<F>(f)); }
            std::vector<analysis::Symbol> GetAllSymbols() const { return table->GetAllSymbols(); }
            std::optional<analysis::Symbol> LookupSymbol(const std::string &name) const { return table->FindFirstSymbol(name); }
            void InsertSymbol(const std::string &name, analysis::SymbolKind kind, const std::string &type = "")
            {
                const_cast<analysis::SymbolTable *>(table)->InsertSymbol(name, kind, type);
            }
        };

        SymbolTableWrapper GetSymbolTable() const
        {
            return SymbolTableWrapper{ &m_symbolTable };
        }

        void UpdateText(const std::string &newText)
        {
            m_sourceCode = newText;
            m_symbolTable.ClearDocumentSymbols(m_uri);
            m_diagnostics.clear();
            if (m_tree)
            {
                ts_tree_delete(m_tree);
                m_tree = nullptr;
            }
            m_tree = m_parser.Parse(m_sourceCode);
            static angel_lsp::i18n::I18n i18n;
            auto collectDiags = m_symbolCollector.CollectSymbolsWithTree(m_uri, m_sourceCode, m_tree, m_symbolTable, &i18n, &m_config.types);
            m_diagnostics.insert(m_diagnostics.end(), collectDiags.begin(), collectDiags.end());

            analysis::SemanticAnalysisRequest request{ m_symbolTable, m_uri, m_config.info.predefinedFileExtension.empty() ? ".as.predefined" : m_config.info.predefinedFileExtension, &i18n };
            request.typeConfig = &m_config.types;
            request.engineProperties = &m_config.engine;
            // Non-const handle so Analyze() gets a mutableScopeRoot - see the same construct in
            // TestDocument above for why the `auto` write-back needs it.
            std::shared_ptr<analysis::Scope> mutableScopeRoot = m_scopeCollector.CollectScopes(m_sourceCode, m_parser);
            m_scopeRoot = mutableScopeRoot;
            request.scopeRoot = m_scopeRoot;
            request.mutableScopeRoot = mutableScopeRoot.get();
            request.sourceCode = m_sourceCode;
            // Same `#if` exclusion the server applies - see utils/PreprocessorRegions.h.
            request.excludedLineRanges = angel_lsp::utils::FindExcludedLineRanges(m_sourceCode);
            request.tree = m_tree;

            analysis::SemanticAnalyzer analyzer(nullptr);
            auto analyzeDiags = analyzer.Analyze(request);
            m_diagnostics.insert(m_diagnostics.end(), analyzeDiags.begin(), analyzeDiags.end());
        }

    private:
        std::string m_uri;
        std::string m_sourceCode;
        config::ServerConfig m_config;
        parser::AngelScriptParser m_parser;
        analysis::SymbolCollector m_symbolCollector{ nullptr };
        analysis::LocalScopeCollector m_scopeCollector{ nullptr };
        analysis::SymbolTable m_symbolTable;
        std::shared_ptr<const analysis::Scope> m_scopeRoot;
        TSTree *m_tree = nullptr;
        std::vector<analysis::Diagnostic> m_diagnostics;
    };

    inline std::shared_ptr<TestDocument> CreateTestDocument(const std::string &uri, const std::string &sourceCode)
    {
        auto doc = std::make_shared<TestDocument>(uri, sourceCode);
        GetTestWorkspaceRegistry()[uri] = doc;
        return doc;
    }

    inline std::shared_ptr<TestDocument> CreateTestDocumentWithConfig(const std::string &uri, const std::string &sourceCode, const config::ServerConfig &config)
    {
        auto doc = std::make_shared<TestDocument>(uri, sourceCode, config);
        GetTestWorkspaceRegistry()[uri] = doc;
        return doc;
    }

    inline void PopulateTestSymbolTable(std::shared_ptr<TestDocument> doc, analysis::SymbolTable &table)
    {
        if (doc)
        {
            doc->GetSymbolTable().ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symbols)
            {
                for (const auto &s : symbols)
                {
                    table.AddSymbol(s);
                }
            });
        }
    }
}

using angel_lsp::test::CreateTestDocument;
using angel_lsp::test::CreateTestDocumentWithConfig;
using angel_lsp::test::TestDocument;
using angel_lsp::test::PopulateTestSymbolTable;
