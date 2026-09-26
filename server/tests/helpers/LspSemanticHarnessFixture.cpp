#include "helpers/LspSemanticHarnessFixture.h"

#include "analysis/NodeIndex.h"
#include "features/completion/CompletionHandler.h"
#include "features/definition/DefinitionHandler.h"
#include "features/hover/HoverHandler.h"
#include "utils/Utils.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#define POP_ENV(var, val)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        char* buf = nullptr;                                                                                           \
        size_t sz = 0;                                                                                                 \
        if (_dupenv_s(&buf, &sz, var) == 0 && buf)                                                                     \
        {                                                                                                              \
            val = buf;                                                                                                 \
            free(buf);                                                                                                 \
        }                                                                                                              \
    } while (0)
#else
#define POP_ENV(var, val)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        const char* p = std::getenv(var);                                                                              \
        if (p)                                                                                                         \
            val = p;                                                                                                   \
    } while (0)
#endif

namespace angel_lsp::test
{

LspSemanticHarnessFixture::LspSemanticHarnessFixture()
{
    SetUp();
}

LspSemanticHarnessFixture::~LspSemanticHarnessFixture()
{
    TearDown();
}

void LspSemanticHarnessFixture::SetUp()
{
    const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    m_sandboxDir = std::filesystem::temp_directory_path() / ("angel_sem_harness_" + std::to_string(seed));
    std::filesystem::create_directories(m_sandboxDir);

    std::error_code ec;
    auto canon = std::filesystem::canonical(m_sandboxDir, ec);
    if (!ec)
    {
        m_sandboxDir = std::move(canon);
    }

    m_symbolTable = std::make_unique<analysis::SymbolTable>();
    m_scopeIndex = std::make_unique<analysis::ScopeIndex>();
    m_config.features.enableTypeConversionChecks = true;
    DiscoverOracleBinary();
}

void LspSemanticHarnessFixture::TearDown()
{
    std::error_code ec;
    if (!m_sandboxDir.empty() && std::filesystem::exists(m_sandboxDir))
    {
        std::filesystem::remove_all(m_sandboxDir, ec);
    }

    m_documents.clear();
    m_trees.clear();
    m_diagnostics.clear();
    m_symbolTable.reset();
    m_scopeIndex.reset();
}

void LspSemanticHarnessFixture::DiscoverOracleBinary()
{
    std::string envPath;
    POP_ENV("ASHARNESS_EXE", envPath);
    if (!envPath.empty() && std::filesystem::exists(envPath))
    {
        m_oracleExe = envPath;
        return;
    }

    const std::vector<std::string> candidates = {
        "E:/Github/src/AS-Harness/build_release/Release/asharness.exe",
        "E:/Github/src/AS-Harness/build/Debug/asharness.exe",
        (std::filesystem::path(ANGELSCRIPT_REPO_ROOT) / "server/build/bin/angelscript_oracle.exe").generic_string(),
        (std::filesystem::path(ANGELSCRIPT_REPO_ROOT) / "server/build/angelscript_oracle").generic_string(),
    };

    for (const auto& path : candidates)
    {
        if (std::filesystem::exists(path))
        {
            m_oracleExe = path;
            return;
        }
    }
}

std::string LspSemanticHarnessFixture::SandboxUri(const std::string& relativePath) const
{
    return utils::PathToUri((m_sandboxDir / relativePath).generic_string());
}

std::string LspSemanticHarnessFixture::UriToDiskPath(const std::string& uri) const
{
    std::string path = utils::UriToPath(uri);
    if (!path.empty())
    {
        return path;
    }
    return (m_sandboxDir / "temp_file.as").generic_string();
}

void LspSemanticHarnessFixture::LoadPredefinedStub(const std::string& stubRelativePath)
{
    std::filesystem::path target = stubRelativePath;
    if (!std::filesystem::exists(target))
    {
        target = std::filesystem::path(ANGELSCRIPT_FIXTURE_DIR) / stubRelativePath;
    }
    if (!std::filesystem::exists(target))
    {
        target = std::filesystem::path(ANGELSCRIPT_REPO_ROOT) / stubRelativePath;
    }
    if (!std::filesystem::exists(target))
    {
        target = std::filesystem::path(ANGELSCRIPT_REPO_ROOT) / "server" / stubRelativePath;
    }
    if (!std::filesystem::exists(target))
    {
        return;
    }

    std::ifstream in(target, std::ios::binary);
    if (!in.is_open())
    {
        return;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string content = ss.str();
    const std::string stubUri = utils::PathToUri(target.generic_string());

    TSTree* tree = m_parser.Parse(content);
    if (tree)
    {
        analysis::SymbolTable staging;
        m_symbolCollector.CollectSymbolsWithTree({stubUri, content, &m_i18n}, tree, staging);
        m_symbolTable->ReplaceDocumentSymbols(stubUri, std::move(staging));
        ts_tree_delete(tree);
    }
}

void LspSemanticHarnessFixture::AddVirtualDocument(const std::string& uri, const std::string& content)
{
    m_documents[uri] = content;

    const std::string diskPath = UriToDiskPath(uri);
    std::filesystem::path p(diskPath);
    if (p.has_parent_path())
    {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    std::ofstream out(diskPath, std::ios::binary);
    if (out.is_open())
    {
        out << content;
    }

    ReanalyzeDocument(uri);
}

void LspSemanticHarnessFixture::UpdateVirtualDocument(const std::string& uri, const std::string& newContent)
{
    AddVirtualDocument(uri, newContent);
}

const std::vector<analysis::Diagnostic>& LspSemanticHarnessFixture::GetDiagnostics(const std::string& uri) const
{
    static const std::vector<analysis::Diagnostic> kEmpty;
    auto it = m_diagnostics.find(uri);
    return it != m_diagnostics.end() ? it->second : kEmpty;
}

void LspSemanticHarnessFixture::ReanalyzeDocument(const std::string& uri)
{
    const std::string& text = m_documents[uri];
    TSTree* tree = m_parser.Parse(text);
    if (!tree)
    {
        m_diagnostics[uri].clear();
        return;
    }

    m_trees.insert_or_assign(uri, document::MakeTreePtr(tree));

    analysis::SymbolTable staging;
    auto diagnostics = m_symbolCollector.CollectSymbolsWithTree({uri, text, &m_i18n}, tree, staging);
    m_symbolTable->ReplaceDocumentSymbols(uri, std::move(staging));

    const TSNode root = ts_tree_root_node(tree);
    std::shared_ptr<analysis::Scope> sharedScope = m_localScopeCollector.CollectScopesFromTree(root, text);
    m_scopeIndex->SetScopeTree(uri, sharedScope);

    analysis::NodeIndex nodeIndex(root);
    analysis::SemanticAnalysisRequest req{*m_symbolTable, uri, ".as.predefined", &m_i18n};
    req.sourceCode = text;
    req.tree = tree;
    req.scopeRoot = sharedScope;
    req.mutableScopeRoot = sharedScope.get();
    req.nodeIndex = &nodeIndex;
    req.typeConfig = &m_config.types;
    req.engineProperties = &m_config.engine;
    req.diagnostics = &m_config.diagnostics;
    req.enableTypeConversionChecks = true;

    analysis::SemanticAnalyzer analyzer(nullptr);
    auto semDiags = analyzer.Analyze(req);
    diagnostics.insert(diagnostics.end(), semDiags.begin(), semDiags.end());

    m_diagnostics[uri] = std::move(diagnostics);
}

void LspSemanticHarnessFixture::AssertNoDiagnostics(const std::string& uri)
{
    const auto& diags = GetDiagnostics(uri);
    if (!diags.empty())
    {
        std::string list;
        for (const auto& d : diags)
        {
            list += "\n  [" + d.code + "] (line " + std::to_string(d.range.start.line) + "): " + d.message;
        }
        INFO("Unexpected diagnostics for " << uri << ":" << list);
        CHECK(diags.empty());
    }
    else
    {
        CHECK(diags.empty());
    }
}

void LspSemanticHarnessFixture::AssertDiagnosticAt(const std::string& uri, uint32_t line,
                                                   const std::string& expectedCode)
{
    const auto& diags = GetDiagnostics(uri);
    bool found = false;
    for (const auto& d : diags)
    {
        if (d.range.start.line == line && d.code == expectedCode)
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
    INFO("Expected diagnostic [" << expectedCode << "] at line " << line << " not found. Actual:" << list);
    CHECK(found);
}

void LspSemanticHarnessFixture::AssertDiagnosticsCount(const std::string& uri, size_t expectedCount)
{
    const auto& diags = GetDiagnostics(uri);
    CHECK(diags.size() == expectedCount);
}

std::optional<lsp::Hover> LspSemanticHarnessFixture::RequestHover(const std::string& uri, uint32_t line, uint32_t col)
{
    auto itTree = m_trees.find(uri);
    if (itTree == m_trees.end())
    {
        return std::nullopt;
    }

    features::HoverRequest req{uri,           m_documents[uri],        itTree->second.get(), *m_symbolTable,
                               *m_scopeIndex, lsp::Position{line, col}};
    req.readDocument = [this](const std::string& docUri) -> const std::string*
    {
        auto it = m_documents.find(docUri);
        return it != m_documents.end() ? &it->second : nullptr;
    };
    req.config = &m_config;

    return features::GetHover(req);
}

std::vector<lsp::CompletionItem> LspSemanticHarnessFixture::RequestCompletion(const std::string& uri, uint32_t line,
                                                                             uint32_t col)
{
    auto itTree = m_trees.find(uri);
    if (itTree == m_trees.end())
    {
        return {};
    }

    features::CompletionRequest req{
        uri,      m_documents[uri], itTree->second.get(), *m_symbolTable, *m_scopeIndex, lsp::Position{line, col},
        &m_config};

    return features::GetCompletion(req);
}

void LspSemanticHarnessFixture::AssertHoverSignature(const std::string& uri, uint32_t line, uint32_t col,
                                                     const std::string& expectedSignature)
{
    auto hover = RequestHover(uri, line, col);
    REQUIRE(hover.has_value());

    const auto* markup = std::get_if<lsp::MarkupContent>(&hover->contents);
    REQUIRE(markup != nullptr);
    CHECK(markup->value.find(expectedSignature) != std::string::npos);
}

void LspSemanticHarnessFixture::AssertHoverContains(const std::string& uri, uint32_t line, uint32_t col,
                                                    const std::string& expectedText)
{
    auto hover = RequestHover(uri, line, col);
    REQUIRE(hover.has_value());

    const auto* markup = std::get_if<lsp::MarkupContent>(&hover->contents);
    REQUIRE(markup != nullptr);
    CHECK(markup->value.find(expectedText) != std::string::npos);
}

void LspSemanticHarnessFixture::AssertCompletionContains(const std::string& uri, uint32_t line, uint32_t col,
                                                         const std::string& expectedLabel,
                                                         lsp::CompletionItemKind expectedKind)
{
    auto completion = RequestCompletion(uri, line, col);
    bool found = false;
    for (const auto& item : completion)
    {
        if (item.label == expectedLabel && item.kind == expectedKind)
        {
            found = true;
            break;
        }
    }
    std::string list;
    if (!found)
    {
        for (const auto& item : completion)
        {
            const int kindInt = item.kind.has_value() ? static_cast<int>(*item.kind) : -1;
            list += "\n  " + item.label + " (kind: " + std::to_string(kindInt) + ")";
        }
    }
    INFO("Expected completion item [" << expectedLabel << "] kind " << static_cast<int>(expectedKind)
                                      << " at (" << line << ", " << col << ") not found. Total items: "
                                      << completion.size() << ". Actual:" << list);
    CHECK(found);
}

void LspSemanticHarnessFixture::AssertCompletionExcludes(const std::string& uri, uint32_t line, uint32_t col,
                                                         const std::string& unexpectedLabel)
{
    auto completion = RequestCompletion(uri, line, col);
    bool found = false;
    for (const auto& item : completion)
    {
        if (item.label == unexpectedLabel)
        {
            found = true;
            break;
        }
    }
    CHECK_FALSE(found);
}

void LspSemanticHarnessFixture::AssertDefinitionTarget(const std::string& uri, uint32_t line, uint32_t col,
                                                       const std::string& expectedTargetUri,
                                                       uint32_t expectedTargetLine)
{
    auto itTree = m_trees.find(uri);
    REQUIRE(itTree != m_trees.end());

    features::DefinitionRequest req{uri,           m_documents[uri],        itTree->second.get(), *m_symbolTable,
                                    *m_scopeIndex, lsp::Position{line, col}};
    req.resolveInclude = [this](const std::string& inc)
    {
        auto p = m_sandboxDir / inc;
        return std::filesystem::exists(p) ? p.generic_string() : "";
    };

    auto defs = features::GetDefinition(req);
    REQUIRE(defs.has_value());
    REQUIRE(!defs->empty());

    bool matched = false;
    for (const auto& loc : *defs)
    {
        if (loc.range.start.line == expectedTargetLine)
        {
            if (expectedTargetUri.empty() || loc.uri.toString().find(expectedTargetUri) != std::string::npos)
            {
                matched = true;
                break;
            }
        }
    }
    CHECK(matched);
}

bool LspSemanticHarnessFixture::VerifyWithNativeOracle(const std::string& sourceSnippet, std::string& outCompilerError)
{
    if (m_oracleExe.empty() || !std::filesystem::exists(m_oracleExe))
    {
        return false;
    }

    const std::filesystem::path tempScript = m_sandboxDir / "oracle_probe.as";
    const std::filesystem::path tempOut = m_sandboxDir / "oracle_probe_out.txt";

    std::ofstream out(tempScript, std::ios::binary);
    if (!out.is_open())
    {
        outCompilerError = "Failed to create oracle probe file";
        return false;
    }
    out << sourceSnippet;
    out.close();

#if defined(_WIN32)
    std::string cmd = "cd /d \"" + m_sandboxDir.string() + "\" && \"" + m_oracleExe + "\" \"" + tempScript.string() +
                      "\" --json --no-pause > \"" + tempOut.string() + "\" 2>&1";
#else
    std::string cmd = "cd \"" + m_sandboxDir.string() + "\" && \"" + m_oracleExe + "\" \"" + tempScript.string() +
                      "\" --json --no-pause > \"" + tempOut.string() + "\" 2>&1";
#endif
    const int exitCode = std::system(cmd.c_str());

    std::ifstream in(tempOut, std::ios::binary);
    if (in.is_open())
    {
        std::ostringstream ss;
        ss << in.rdbuf();
        outCompilerError = ss.str();
    }

    return (exitCode == 0);
}

} // namespace angel_lsp::test
