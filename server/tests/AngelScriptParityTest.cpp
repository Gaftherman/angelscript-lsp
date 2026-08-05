#include <doctest/doctest.h>

#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"
#include <tree_sitter/api.h>

#include <angelscript.h>

#include <string>
#include <vector>
#include <cstdlib>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    struct AsDiag
    {
        int type = 0;
        int row = 0; // 1-based, as reported by AngelScript
        int col = 0;
        std::string message;
    };

    struct AsCompileResult
    {
        std::vector<AsDiag> messages;
        int buildResult = 0;
    };

    void AsMessageCallback(const asSMessageInfo *msg, void *param)
    {
        auto *out = static_cast<std::vector<AsDiag> *>(param);
        out->push_back(AsDiag{msg->type, msg->row, msg->col, msg->message});
    }

    // Compiles the source with the real AngelScript engine and captures its diagnostics.
    // Note: only builtin types are available (no registered add-ons), so corpus cases
    // must stick to int/float/bool/void and core language constructs.
    AsCompileResult CompileWithRealAS(const std::string &sourceCode)
    {
        AsCompileResult result;
        asIScriptEngine *engine = asCreateScriptEngine();
        engine->SetMessageCallback(asFUNCTION(AsMessageCallback), &result.messages, asCALL_CDECL);
        asIScriptModule *mod = engine->GetModule("parity", asGM_ALWAYS_CREATE);
        mod->AddScriptSection("parity.as", sourceCode.c_str(), sourceCode.size());
        result.buildResult = mod->Build();
        engine->ShutDownAndRelease();
        return result;
    }

    std::vector<Diagnostic> AnalyzeWithLsp(const std::string &sourceCode, SymbolTable &table)
    {
        const std::string fileUri = "file:///parity.as";
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        std::vector<Diagnostic> diags = collector.CollectSymbols(fileUri, sourceCode, parser, table);
        SemanticAnalyzer analyzer;
        angel_lsp::i18n::I18n i18n("en");
        SemanticAnalysisRequest req{table, fileUri, "", &i18n};
        std::vector<Diagnostic> semantic = analyzer.Analyze(req);
        diags.insert(diags.end(), semantic.begin(), semantic.end());
        return diags;
    }

    std::vector<AsDiag> RealASErrors(const AsCompileResult &result)
    {
        std::vector<AsDiag> errors;
        for (const auto &m : result.messages)
        {
            if (m.type == asMSGTYPE_ERROR)
                errors.push_back(m);
        }
        return errors;
    }

    std::vector<Diagnostic> LspErrors(const std::vector<Diagnostic> &diags)
    {
        std::vector<Diagnostic> errors;
        for (const auto &d : diags)
        {
            if (d.severity == DiagnosticSeverity::Error)
                errors.push_back(d);
        }
        return errors;
    }

    // Core 1:1 parity check: the real AngelScript compiler and the LSP analysis must
    // agree on whether the source is erroneous, and on the line of the first error.
    void CheckErrorParity(const std::string &sourceCode)
    {
        AsCompileResult real = CompileWithRealAS(sourceCode);
        SymbolTable table;
        std::vector<Diagnostic> lsp = AnalyzeWithLsp(sourceCode, table);

        std::vector<AsDiag> realErrors = RealASErrors(real);
        std::vector<Diagnostic> lspErrors = LspErrors(lsp);

        INFO("source: ", sourceCode);
        if (realErrors.empty())
        {
            INFO("real AS accepted the source, LSP must not emit errors");
            INFO("first LSP error: ", lspErrors.empty() ? "<none>" : lspErrors.front().message);
            CHECK(lspErrors.empty());
        }
        else
        {
            INFO("real AS error: line ", realErrors.front().row, ": ", realErrors.front().message);
            INFO("first LSP error: ", lspErrors.empty() ? "<none>" : lspErrors.front().message);
            CHECK(!lspErrors.empty());
            if (!lspErrors.empty())
            {
                // AS row 0 means it could not build the module at all; only presence matters.
                if (realErrors.front().row != 0)
                {
                    // AS reports at the token where parsing failed (often the next token or EOF,
                    // i.e. one line past the declaration); the LSP reports at the declaration itself.
                    uint32_t lspLine = lspErrors.front().range.start.line + 1;
                    uint32_t asRow = static_cast<uint32_t>(realErrors.front().row);
                    CHECK((lspLine == asRow || lspLine + 1 == asRow));
                }
            }
        }
    }
    // Known gaps in LSP analysis (real AS errors that the LSP does not yet flag).
    // Enabled only when AS_PARITY_GAPS=1 so the default suite stays green while
    // these represent outstanding work items. Valid-control cases still run always.
    bool GapTestsEnabled()
    {
        return std::getenv("AS_PARITY_GAPS") != nullptr;
    }
}

TEST_CASE("Parity - typedef declaration")
{
    CheckErrorParity("typedef int MyInt;\n");
    CheckErrorParity("typedef int MyInt\n"); // missing ';' -> real AS: ERR_sExpectedToken
}

TEST_CASE("Parity - global variable declaration")
{
    CheckErrorParity("int g_value = 0;\n");
    CheckErrorParity("int g_value = 0\n"); // missing ';'
}

TEST_CASE("Parity - class member declaration missing semicolon")
{
    CheckErrorParity("class Foo { int x; };\n");
    CheckErrorParity("class Foo { int x };\n"); // missing ';' after class member declaration
}

TEST_CASE("Parity - funcdef declaration missing semicolon")
{
    CheckErrorParity("funcdef void Callback();\n");
    CheckErrorParity("funcdef void Callback()\n"); // missing ';' after funcdef
}

TEST_CASE("Parity - enum closing brace missing semicolon")
{
    CheckErrorParity("enum State { A, B };\n");
    CheckErrorParity("enum State { A, B }\n"); // missing ';' after enum closing brace
}

TEST_CASE("Parity - namespace-level function prototype declaration missing semicolon")
{
    CheckErrorParity("namespace N { void f(); }\n");
    CheckErrorParity("namespace N { void f() }\n"); // missing ';' after namespace-level function prototype declaration
}

TEST_CASE("Parity - return statement inside function missing semicolon")
{
    CheckErrorParity("void f() { return; }\n");
    CheckErrorParity("void f() { return }\n"); // missing ';' after return
}

TEST_CASE("Parity - break inside while loop missing semicolon")
{
    CheckErrorParity("void f() { while (true) { break; } }\n");
    CheckErrorParity("void f() { while (true) { break } }\n"); // missing ';' after break
}

TEST_CASE("Parity - continue inside for loop missing semicolon")
{
    CheckErrorParity("void f() { for (int i = 0; i < 10; i++) { continue; } }\n");
    CheckErrorParity("void f() { for (int i = 0; i < 10; i++) { continue } }\n"); // missing ';' after continue
}

TEST_CASE("Parity - import statement missing semicolon")
{
    CheckErrorParity("import int f() from \"mod\";\n");
    CheckErrorParity("import int f() from \"mod\"\n"); // missing ';' after import
}

TEST_CASE("Parity - property accessor block closing missing semicolon")
{
    CheckErrorParity("int prop { get { return 0; } };\n");
    CheckErrorParity("int prop { get { return 0; } }\n"); // missing ';' after property accessor block closing
}

TEST_CASE("Parity - missing closing paren in function call")
{
    CheckErrorParity("void g(int x) {}\nvoid f() { g(1); }\n");
    CheckErrorParity("void g(int x) {}\nvoid f() { g(1; }\n"); // missing ')' in function call
}

TEST_CASE("Parity - missing closing brace in function body")
{
    CheckErrorParity("void f() {}\n");
    CheckErrorParity("void f() {\n  int x = 0;\n"); // missing '}' in function body
}

TEST_CASE("Parity - unterminated block comment")
{
    CheckErrorParity("/* comment */\nvoid f() {}\n");
    CheckErrorParity("/* comment\nvoid f() {}\n"); // unterminated block comment
}

TEST_CASE("Parity - duplicate const modifier on a global")
{
    CheckErrorParity("const int g_val = 0;\n");
    CheckErrorParity("const const int g_val = 0;\n"); // duplicate const modifier on a global
}

TEST_CASE("Parity - shared non-class function")
{
    CheckErrorParity("shared class C {}\n");
    CheckErrorParity("shared void f() {}\n"); // shared non-class function
}

TEST_CASE("Parity - int x = str type mismatch")
{
    CheckErrorParity("int str = 1;\nint x = str;\n");
    CheckErrorParity("bool str = true;\nint x = str;\n");
}

TEST_CASE("Parity - call of undeclared function")
{
    CheckErrorParity("void g() {}\nvoid f() { g(); }\n");
    CheckErrorParity("void f() { undeclared_func(); }\n");
}

TEST_CASE("Parity - use of undeclared identifier")
{
    CheckErrorParity("void f() { int x = 0; int y = x; }\n");
    CheckErrorParity("void f() { int y = x; }\n");
}

