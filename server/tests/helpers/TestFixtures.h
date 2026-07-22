#pragma once

#include <doctest/doctest.h>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <vector>

#include "analysis/ValidationOracle.h"
#include "document/Document.h"
#include "analysis/SymbolTable.h"
#include "analysis/SymbolCollector.h"

namespace fixtures
{
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

        void Dump() const
        {
            for (const auto &d : diags)
            {
                spdlog::debug("  [{},{}] {}", d.range.start.line, d.range.start.character, d.message);
            }
        }
    };

    inline DiagResult Validate(const std::string &code, const analysis::SymbolTable *globalTable = nullptr)
    {
        analysis::ValidationOracle oracle;
        DiagResult result;
        result.diags = oracle.ValidateSync(code, "file:///test.as", nullptr, globalTable);
        result.Dump();
        return result;
    }

    inline Document MakeDoc(const std::string &code,
                            const std::string &uri = "file:///test.as")
    {
        return Document(uri, code);
    }
}
