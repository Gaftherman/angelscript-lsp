#include "helpers/UnifiedFeatureAuditHarness.h"

#include <algorithm>
#include <doctest/doctest.h>

namespace angel_lsp::test
{

std::optional<std::vector<lsp::Location>> UnifiedFeatureAuditHarness::RequestDefinition(const std::string& uri,
                                                                                        uint32_t line, uint32_t col)
{
    auto itTree = m_trees.find(uri);
    if (itTree == m_trees.end())
    {
        return std::nullopt;
    }

    features::DefinitionRequest req{
        uri, m_documents[uri], itTree->second.get(), *m_symbolTable, *m_scopeIndex, lsp::Position{line, col},
        {},  ".as.predefined", &m_config.engineRules};
    return features::GetDefinition(req);
}

std::optional<std::vector<lsp::Location>> UnifiedFeatureAuditHarness::RequestReferences(const std::string& uri,
                                                                                        uint32_t line, uint32_t col)
{
    auto itTree = m_trees.find(uri);
    if (itTree == m_trees.end())
    {
        return std::nullopt;
    }

    features::ReferencesRequest req{uri,  m_documents[uri], itTree->second.get(), lsp::Position{line, col},
                                    true, *m_symbolTable,   *m_scopeIndex,        nullptr};
    return features::GetReferences(req);
}

std::optional<std::vector<lsp::CodeAction>> UnifiedFeatureAuditHarness::RequestCodeActions(const std::string& uri,
                                                                                           const lsp::Range& range)
{
    auto itTree = m_trees.find(uri);
    if (itTree == m_trees.end())
    {
        return std::nullopt;
    }

    lsp::CodeActionContext context;
    const auto& fileDiags = GetDiagnostics(uri);
    for (const auto& d : fileDiags)
    {
        lsp::Diagnostic ld;
        ld.code = d.code;
        ld.message = d.message;
        ld.range = lsp::Range{{d.range.start.line, d.range.start.character}, {d.range.end.line, d.range.end.character}};
        context.diagnostics.push_back(std::move(ld));
    }

    features::CodeActionRequest req{
        uri,       m_documents[uri], itTree->second.get(), range, std::move(context), *m_symbolTable, *m_scopeIndex, {},
        &m_config, &m_i18n};
    return features::GetCodeActions(req);
}

lsp::SemanticTokens UnifiedFeatureAuditHarness::RequestSemanticTokens(const std::string& uri)
{
    auto itTree = m_trees.find(uri);
    if (itTree == m_trees.end())
    {
        return {};
    }

    features::SemanticTokensRequest req{uri, m_documents[uri], itTree->second.get(), *m_symbolTable};
    req.scopeRoot = m_scopeIndex->GetRoot(uri);
    return features::GetSemanticTokens(req);
}

std::optional<lsp::Hover> UnifiedFeatureAuditHarness::AuditHover(const std::string& uri, uint32_t line, uint32_t col,
                                                                 double maxMs)
{
    (void)RequestHover(uri, line, col);
    auto t0 = std::chrono::high_resolution_clock::now();
    auto result = RequestHover(uri, line, col);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    INFO("Hover latency: " << ms << " ms, budget: " << maxMs << " ms");
    CHECK(ms <= maxMs * 5.0);
    return result;
}

std::vector<lsp::CompletionItem> UnifiedFeatureAuditHarness::AuditCompletion(const std::string& uri, uint32_t line,
                                                                             uint32_t col, double maxMs)
{
    (void)RequestCompletion(uri, line, col);
    auto t0 = std::chrono::high_resolution_clock::now();
    auto result = RequestCompletion(uri, line, col);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    INFO("Completion latency: " << ms << " ms, budget: " << maxMs << " ms");
    CHECK(ms <= maxMs * 5.0);
    return result;
}

std::optional<std::vector<lsp::Location>>
UnifiedFeatureAuditHarness::AuditDefinition(const std::string& uri, uint32_t line, uint32_t col, double maxMs)
{
    (void)RequestDefinition(uri, line, col);
    auto t0 = std::chrono::high_resolution_clock::now();
    auto result = RequestDefinition(uri, line, col);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    INFO("Definition latency: " << ms << " ms, budget: " << maxMs << " ms");
    CHECK(ms <= maxMs * 5.0);
    return result;
}

std::optional<std::vector<lsp::Location>>
UnifiedFeatureAuditHarness::AuditReferences(const std::string& uri, uint32_t line, uint32_t col, double maxMs)
{
    (void)RequestReferences(uri, line, col);
    auto t0 = std::chrono::high_resolution_clock::now();
    auto result = RequestReferences(uri, line, col);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    INFO("References latency: " << ms << " ms, budget: " << maxMs << " ms");
    CHECK(ms <= maxMs * 5.0);
    return result;
}

std::optional<std::vector<lsp::CodeAction>>
UnifiedFeatureAuditHarness::AuditCodeActions(const std::string& uri, const lsp::Range& range, double maxMs)
{
    (void)RequestCodeActions(uri, range);
    auto t0 = std::chrono::high_resolution_clock::now();
    auto result = RequestCodeActions(uri, range);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    INFO("CodeActions latency: " << ms << " ms, budget: " << maxMs << " ms");
    CHECK(ms <= maxMs * 5.0);
    return result;
}

lsp::SemanticTokens UnifiedFeatureAuditHarness::AuditSemanticTokens(const std::string& uri, double maxMs)
{
    (void)RequestSemanticTokens(uri);
    auto t0 = std::chrono::high_resolution_clock::now();
    auto result = RequestSemanticTokens(uri);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    INFO("SemanticTokens latency: " << ms << " ms, budget: " << maxMs << " ms");
    CHECK(ms <= maxMs * 5.0);
    return result;
}

std::string UnifiedFeatureAuditHarness::VerifyGoodPath(const std::string& relativePath, const std::string& content)
{
    std::string uri = SandboxUri(relativePath);
    AddVirtualDocument(uri, content);
    AssertNoDiagnostics(uri);
    return uri;
}

std::string UnifiedFeatureAuditHarness::VerifyBadPath(const std::string& relativePath, const std::string& content,
                                                      const std::string& expectedCode)
{
    std::string uri = SandboxUri(relativePath);
    AddVirtualDocument(uri, content);
    const auto& diags = GetDiagnostics(uri);
    bool found = false;
    for (const auto& d : diags)
    {
        if (d.code == expectedCode)
        {
            found = true;
            break;
        }
    }
    std::string list;
    if (!found)
    {
        for (const auto& d : diags)
        {
            list += "\n  [" + d.code + "] (line " + std::to_string(d.range.start.line) + "): " + d.message;
        }
    }
    INFO("Expected diagnostic [" << expectedCode << "] not found. Actual diagnostics:" << list);
    CHECK(found);
    return uri;
}

} // namespace angel_lsp::test
