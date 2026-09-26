#pragma once

#include "analysis/Diagnostics.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "config/ServerConfig.h"
#include "document/Document.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>
#include <filesystem>
#include <lsp/messages.h>
#include <lsp/types.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace angel_lsp::test
{

/**
 * @brief High-level test harness fixture providing synchronous multi-file workspace
 *        management, semantic diagnostic assertions, feature fidelity inspections (Hover,
 *        Completion, Definition), and native AngelScript engine oracle verification.
 */
class LspSemanticHarnessFixture
{
  public:
    LspSemanticHarnessFixture();
    ~LspSemanticHarnessFixture();

    /** @brief Initializes temporary sandbox directory and locates native oracle binary. */
    void SetUp();

    /** @brief Removes temporary sandbox directory and clears document store. */
    void TearDown();

    /**
     * @brief Loads a predefined stub file into the symbol table.
     * @param[in] stubRelativePath Relative or absolute path to the .as.predefined stub.
     */
    void LoadPredefinedStub(const std::string& stubRelativePath);

    /**
     * @brief Adds a virtual document to the fixture, parses AST, collects symbols, and runs semantic analysis.
     * @param[in] uri Virtual document URI key (e.g. "file:///scripts/main.as").
     * @param[in] content AngelScript source code content.
     */
    void AddVirtualDocument(const std::string& uri, const std::string& content);

    /**
     * @brief Updates an existing virtual document, re-indexes symbols, and re-analyzes diagnostics.
     * @param[in] uri Virtual document URI key.
     * @param[in] newContent Updated AngelScript source code content.
     */
    void UpdateVirtualDocument(const std::string& uri, const std::string& newContent);

    /**
     * @brief Asserts that the specified document emits zero semantic diagnostics.
     * @param[in] uri Virtual document URI key.
     */
    void AssertNoDiagnostics(const std::string& uri);

    /**
     * @brief Asserts that a diagnostic with the expected code exists at the given line.
     * @param[in] uri Virtual document URI key.
     * @param[in] line 0-indexed line number.
     * @param[in] expectedCode Diagnostic code (e.g. "as-err-not-all-paths-return").
     */
    void AssertDiagnosticAt(const std::string& uri, uint32_t line, const std::string& expectedCode);

    /**
     * @brief Asserts that the total number of diagnostics emitted for a document matches expectedCount.
     * @param[in] uri Virtual document URI key.
     * @param[in] expectedCount Expected count of diagnostics.
     */
    void AssertDiagnosticsCount(const std::string& uri, size_t expectedCount);

    /**
     * @brief Requests hover information at (line, col) for a document.
     * @param[in] uri Virtual document URI key.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed character offset.
     * @return Hover response if available; std::nullopt otherwise.
     */
    std::optional<lsp::Hover> RequestHover(const std::string& uri, uint32_t line, uint32_t col);

    /**
     * @brief Requests completion candidates at (line, col) for a document.
     * @param[in] uri Virtual document URI key.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed character offset.
     * @return Vector of completion items.
     */
    std::vector<lsp::CompletionItem> RequestCompletion(const std::string& uri, uint32_t line, uint32_t col);

    /**
     * @brief Asserts that hovering at (line, col) produces a hover tooltip containing the expected signature.
     * @param[in] uri Virtual document URI key.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed character offset.
     * @param[in] expectedSignature Expected signature substring in the hover content.
     */
    void AssertHoverSignature(const std::string& uri, uint32_t line, uint32_t col,
                              const std::string& expectedSignature);

    /**
     * @brief Asserts that hovering at (line, col) produces a hover tooltip containing expected text.
     * @param[in] uri Virtual document URI key.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed character offset.
     * @param[in] expectedText Substring expected inside hover documentation or description.
     */
    void AssertHoverContains(const std::string& uri, uint32_t line, uint32_t col, const std::string& expectedText);

    /**
     * @brief Asserts that completion at (line, col) contains a specific label and kind.
     * @param[in] uri Virtual document URI key.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed character offset.
     * @param[in] expectedLabel Expected completion item label.
     * @param[in] expectedKind Expected LSP completion item kind.
     */
    void AssertCompletionContains(const std::string& uri, uint32_t line, uint32_t col, const std::string& expectedLabel,
                                  lsp::CompletionItemKind expectedKind);

    /**
     * @brief Asserts that completion at (line, col) excludes a specific label.
     * @param[in] uri Virtual document URI key.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed character offset.
     * @param[in] unexpectedLabel Label that must not appear in the candidate list.
     */
    void AssertCompletionExcludes(const std::string& uri, uint32_t line, uint32_t col,
                                  const std::string& unexpectedLabel);

    /**
     * @brief Asserts that Go-To-Definition at (line, col) resolves to the expected target URI and line.
     * @param[in] uri Virtual document URI key.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed character offset.
     * @param[in] expectedTargetUri Expected definition target URI.
     * @param[in] expectedTargetLine Expected definition target 0-indexed line.
     */
    void AssertDefinitionTarget(const std::string& uri, uint32_t line, uint32_t col,
                                const std::string& expectedTargetUri, uint32_t expectedTargetLine);

    /**
     * @brief Compiles a source snippet using the native AngelScript oracle compiler (asharness.exe /
     * angelscript_oracle).
     * @param[in] sourceSnippet AngelScript source code snippet.
     * @param[out] outCompilerError Captured compiler errors if rejected.
     * @return True if compiled successfully with exit code 0; false otherwise.
     */
    bool VerifyWithNativeOracle(const std::string& sourceSnippet, std::string& outCompilerError);

    /** @brief Returns reference to mutable ServerConfig for test configuration overrides. */
    config::ServerConfig& Config()
    {
        return m_config;
    }

    /** @brief Returns reference to the internal symbol table. */
    const analysis::SymbolTable& GetSymbolTable() const
    {
        return *m_symbolTable;
    }

    /** @brief Returns the temporary sandbox directory path. */
    const std::filesystem::path& GetSandboxDir() const
    {
        return m_sandboxDir;
    }

    /**
     * @brief Helper to convert a sandbox-relative file path to its canonical file:// URI.
     * @param[in] relativePath File path relative to sandbox root.
     * @return File URI string.
     */
    std::string SandboxUri(const std::string& relativePath) const;

    /**
     * @brief Returns all diagnostics currently recorded for the given document URI.
     * @param[in] uri Virtual document URI key.
     * @return Vector of diagnostics.
     */
    const std::vector<analysis::Diagnostic>& GetDiagnostics(const std::string& uri) const;

  private:
    std::filesystem::path m_sandboxDir;
    std::string m_oracleExe;
    parser::AngelScriptParser m_parser;
    analysis::SymbolCollector m_symbolCollector{nullptr};
    analysis::LocalScopeCollector m_localScopeCollector{nullptr};
    std::unique_ptr<analysis::SymbolTable> m_symbolTable;
    std::unique_ptr<analysis::ScopeIndex> m_scopeIndex;
    config::ServerConfig m_config;
    i18n::I18n m_i18n;

    std::map<std::string, std::string> m_documents;
    std::map<std::string, document::TreePtr> m_trees;
    std::map<std::string, std::vector<analysis::Diagnostic>> m_diagnostics;

    void ReanalyzeDocument(const std::string& uri);
    std::string UriToDiskPath(const std::string& uri) const;
    void DiscoverOracleBinary();
};

} // namespace angel_lsp::test
