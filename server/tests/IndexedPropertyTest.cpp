#include "analysis/DiagnosticCodes.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>
#include <string>
#include <vector>

namespace
{
using namespace angel_lsp;
using namespace angel_lsp::analysis;

std::vector<Diagnostic> AnalyzeSnippet(const std::string& code, const std::string& fileUri = "file:///props.as")
{
    angel_lsp::parser::AngelScriptParser asParser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    static angel_lsp::i18n::I18n i18n;

    collector.CollectSymbols(fileUri, code, asParser, table);

    SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
    std::shared_ptr<Scope> scopeRoot = scopes.CollectScopes(code, asParser);
    request.scopeRoot = scopeRoot;
    request.mutableScopeRoot = scopeRoot.get();
    request.sourceCode = code;
    request.tree = asParser.Parse(code);

    SemanticAnalyzer analyzer(nullptr);
    auto diags = analyzer.Analyze(request);
    if (request.tree)
    {
        ts_tree_delete(const_cast<TSTree*>(request.tree));
    }
    return diags;
}

bool HasCode(const std::vector<Diagnostic>& diags, std::string_view code)
{
    for (const auto& d : diags)
    {
        if (d.code == code)
        {
            return true;
        }
    }
    return false;
}

bool HasAnyError(const std::vector<Diagnostic>& diags)
{
    for (const auto& d : diags)
    {
        if (d.severity == DiagnosticSeverity::Error)
        {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_SUITE("IndexedProperties")
{
    TEST_CASE("IndexedProperties - Global indexed property declaration and element assignment produces zero errors")
    {
        const std::string propName = test::GenerateRandomSymbolName("strArr");
        const std::string first = test::GenerateRandomSymbolName("first");
        const std::string second = test::GenerateRandomSymbolName("second");

        const std::string script = "string " + first + ";\n" + "string " + second + ";\n" + "string get_" + propName +
                                   "(int idx) property\n"
                                   "{\n"
                                   "    switch(idx)\n"
                                   "    {\n"
                                   "    case 0: return " +
                                   first +
                                   ";\n"
                                   "    case 1: return " +
                                   second +
                                   ";\n"
                                   "    }\n"
                                   "    return \"\";\n"
                                   "}\n"
                                   "void set_" +
                                   propName +
                                   "(int idx, const string &in value) property\n"
                                   "{\n"
                                   "    switch(idx)\n"
                                   "    {\n"
                                   "    case 0: " +
                                   first + " = value; break;\n" + "    case 1: " + second +
                                   " = value; break;\n"
                                   "    }\n"
                                   "}\n"
                                   "void main()\n"
                                   "{\n"
                                   "    " +
                                   propName + "[0] = \"Hello\";\n" + "    " + propName +
                                   "[1] = \"World\";\n"
                                   "}\n";

        const auto diags = AnalyzeSnippet(script);
        CHECK_FALSE(HasCode(diags, "as-err-virtual-property-outside-class"));
        CHECK_FALSE(HasAnyError(diags));
    }

    TEST_CASE("IndexedProperties - Reading global indexed property resolves to string for concatenation")
    {
        const std::string propName = test::GenerateRandomSymbolName("strArr");
        const std::string script = "string get_" + propName +
                                   "(int idx) property\n"
                                   "{\n"
                                   "    return \"item\";\n"
                                   "}\n"
                                   "void print(const string &in msg) {}\n"
                                   "void main()\n"
                                   "{\n"
                                   "    print(" +
                                   propName + "[0] + \" \" + " + propName +
                                   "[1] + \"\\n\");\n"
                                   "}\n";

        const auto diags = AnalyzeSnippet(script);
        CHECK_FALSE(HasCode(diags, "as-err-virtual-property-outside-class"));
        CHECK_FALSE(HasAnyError(diags));
    }

    TEST_CASE("IndexedProperties - Compound assignment on global indexed property emits error")
    {
        const std::string propName = test::GenerateRandomSymbolName("strArr");
        const std::string script = "string get_" + propName +
                                   "(int idx) property { return \"\"; }\n"
                                   "void set_" +
                                   propName +
                                   "(int idx, const string &in val) property {}\n"
                                   "void main()\n"
                                   "{\n"
                                   "    " +
                                   propName +
                                   "[0] += \"append\";\n"
                                   "}\n";

        const auto diags = AnalyzeSnippet(script);
        CHECK(HasCode(diags, "as-err-compound-assign-on-indexed-prop"));
    }

    TEST_CASE("IndexedProperties - Assignment to read-only global indexed property emits error")
    {
        const std::string propName = test::GenerateRandomSymbolName("roArr");
        const std::string script = "string get_" + propName +
                                   "(int idx) property { return \"\"; }\n"
                                   "void main()\n"
                                   "{\n"
                                   "    " +
                                   propName +
                                   "[0] = \"write\";\n"
                                   "}\n";

        const auto diags = AnalyzeSnippet(script);
        CHECK(HasCode(diags, "as-err-read-only-property"));
    }

    TEST_CASE("IndexedProperties - Namespace-scoped indexed property resolves and assigns clean")
    {
        const std::string ns = test::GenerateRandomSymbolName("Economy");
        const std::string propName = test::GenerateRandomSymbolName("tier");
        const std::string script = "namespace " + ns + "\n" + "{\n" + "    int get_" + propName +
                                   "(int idx) property { return 0; }\n"
                                   "    void set_" +
                                   propName +
                                   "(int idx, int val) property {}\n"
                                   "}\n"
                                   "void main()\n"
                                   "{\n"
                                   "    " +
                                   ns + "::" + propName + "[0] = 5;\n" + "    int val = " + ns + "::" + propName +
                                   "[0];\n"
                                   "}\n";

        const auto diags = AnalyzeSnippet(script);
        CHECK_FALSE(HasCode(diags, "as-err-virtual-property-outside-class"));
        CHECK_FALSE(HasAnyError(diags));
    }

    TEST_CASE("IndexedProperties - Handle-indexed global property accessor (Sven Co-op player pattern)")
    {
        const std::string propName = test::GenerateRandomSymbolName("BuyPoints");
        const std::string script = "class CBasePlayer {};\n"
                                   "int get_" +
                                   propName +
                                   "(CBasePlayer@ p) property { return 0; }\n"
                                   "void set_" +
                                   propName +
                                   "(CBasePlayer@ p, int val) property {}\n"
                                   "void main()\n"
                                   "{\n"
                                   "    CBasePlayer@ p;\n"
                                   "    " +
                                   propName + "[p] = 500;\n" + "    int val = " + propName +
                                   "[p];\n"
                                   "}\n";

        const auto diags = AnalyzeSnippet(script);
        CHECK_FALSE(HasCode(diags, "as-err-virtual-property-outside-class"));
        CHECK_FALSE(HasAnyError(diags));
    }

    TEST_CASE("IndexedProperties - Class member indexed property supports indexing and rejects compound assign")
    {
        const std::string className = test::GenerateRandomSymbolName("Inventory");
        const std::string propName = test::GenerateRandomSymbolName("items");
        const std::string script = "class " + className + "\n" + "{\n" + "    string get_" + propName +
                                   "(int idx) property { return \"\"; }\n"
                                   "    void set_" +
                                   propName +
                                   "(int idx, const string &in val) property {}\n"
                                   "}\n"
                                   "void main()\n"
                                   "{\n"
                                   "    " +
                                   className + " inv;\n" + "    inv." + propName + "[0] = \"potion\";\n" + "    inv." +
                                   propName +
                                   "[0] += \"extra\";\n"
                                   "}\n";

        const auto diags = AnalyzeSnippet(script);
        CHECK(HasCode(diags, "as-err-compound-assign-on-indexed-prop"));
    }
}
