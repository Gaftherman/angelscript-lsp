#pragma once
#include <doctest/doctest.h>
#include <angelscript.h>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <vector>

#include "analysis/ValidationOracle.h"
#include "document/Document.h"

namespace fixtures
{
    // ─── RAII Engine Guard ─────────────────────────────────────────────────────
    struct EngineGuard
    {
        asIScriptEngine *engine;
        explicit EngineGuard(asIScriptEngine *e) : engine(e) {}
        ~EngineGuard()
        {
            if (engine)
            {
                engine->ShutDownAndRelease();
            }
        }
        operator asIScriptEngine *() const { return engine; }
        asIScriptEngine *operator->() const { return engine; }
    };

    // ─── Engine Factories ─────────────────────────────────────────────────────
    inline asIScriptEngine *CreateBaseEngine()
    {
        asIScriptEngine *engine = asCreateScriptEngine();
        engine->SetEngineProperty(asEP_SCRIPT_SCANNER, 1);
        return engine;
    }

    inline void DummyPrint(std::string &) {}
    inline void DummyPrintln(std::string &) {}
    inline float DummyMath(float a) { return a; }

    inline asIScriptEngine *CreateGameEngine()
    {
        asIScriptEngine *engine = CreateBaseEngine();
        // Mock functions so we can test with game-like code
        engine->RegisterGlobalFunction("void print(const string& in)", asFUNCTION(DummyPrint), asCALL_CDECL);
        engine->RegisterGlobalFunction("void println(const string& in)", asFUNCTION(DummyPrintln), asCALL_CDECL);
        engine->RegisterGlobalFunction("float sin(float)", asFUNCTION(DummyMath), asCALL_CDECL);
        engine->RegisterGlobalFunction("float cos(float)", asFUNCTION(DummyMath), asCALL_CDECL);
        engine->RegisterGlobalFunction("float abs(float)", asFUNCTION(DummyMath), asCALL_CDECL);
        return engine;
    }

    // ─── Diagnostic Helper ────────────────────────────────────────────────────
    struct DiagResult
    {
        std::vector<lsp::Diagnostic> diags;

        bool HasError() const
        {
            for (const auto &d : diags)
            {
                if (d.severity == lsp::DiagnosticSeverity::Error)
                {
                    return true;
                }
            }
            return false;
        }

        bool HasMessage(const std::string &fragment) const
        {
            for (const auto &d : diags)
            {
                if (d.message.find(fragment) != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }

        bool IsClean() const
        {
            return diags.empty();
        }

        // Print all diagnostics (for debugging failing tests)
        void Dump() const
        {
            for (const auto &d : diags)
            {
                spdlog::debug("  [{},{}] {}", d.range.start.line, d.range.start.character, d.message);
            }
        }
    };

    inline DiagResult Validate(asIScriptEngine *engine, const std::string &code)
    {
        analysis::ValidationOracle oracle(engine);
        DiagResult result;
        result.diags = oracle.ValidateSync(code);
        result.Dump();
        return result;
    }

    // ─── Document Helper ───────────────────────────────────────────────────────
    inline Document MakeDoc(const std::string &code,
                            const std::string &uri = "file:///test.as")
    {
        return Document(uri, code);
    }
}
