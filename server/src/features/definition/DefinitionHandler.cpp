/**
 * @file DefinitionHandler.cpp
 * @brief Implementation of textDocument/definition and textDocument/typeDefinition handlers.
 * @ingroup Features
 */

#include "DefinitionHandler.h"
#include "analysis/SymbolResolver.h"
#include <angelscript.h>
#include <ankerl/unordered_dense.h>
#include <vector>

namespace angel_lsp::features::definition
{
        static lsp::Location SymbolToLocation(const analysis::Symbol *sym, const std::string &fallbackUri)
        {
            lsp::Location loc;
            std::string targetUri = sym->uri.empty() ? fallbackUri : sym->uri;
            loc.uri = lsp::DocumentUri::parse(targetUri);
            loc.range = sym->selectionRange;
            return loc;
        }

        lsp::requests::TextDocument_Definition::Result ProcessDefinition(
            const lsp::requests::TextDocument_Definition::Params &req,
            const Document &doc,
            const analysis::SymbolTable &table,
            const asIScriptEngine *engine)
        {
            std::vector<const analysis::Symbol *> multiResults;
            const analysis::Symbol *sym = analysis::SymbolResolver::ResolveAt(doc, table, req.position.line, req.position.character, &multiResults);

            if (!sym && multiResults.empty())
                return {};

            std::vector<const analysis::Symbol *> candidates;
            ankerl::unordered_dense::set<std::string> seen;

            auto addCandidate = [&](const analysis::Symbol *s) {
                if (!s) return;
                std::string key = (s->uri.empty() ? doc.GetUri() : s->uri) + ":" +
                                  std::to_string(s->selectionRange.start.line) + ":" +
                                  std::to_string(s->selectionRange.start.character);
                if (seen.insert(key).second) {
                    candidates.push_back(s);
                }
            };

            if (sym) {
                addCandidate(sym);
            }
            for (const auto *r : multiResults) {
                addCandidate(r);
            }

            if (candidates.empty())
                return {};

            if (candidates.size() == 1)
            {
                return SymbolToLocation(candidates[0], doc.GetUri());
            }

            lsp::Array<lsp::Location> locations;
            for (const auto *c : candidates)
            {
                locations.push_back(SymbolToLocation(c, doc.GetUri()));
            }
            return locations;
        }

        lsp::requests::TextDocument_TypeDefinition::Result ProcessTypeDefinition(
            const lsp::requests::TextDocument_TypeDefinition::Params &req,
            const Document &doc,
            const analysis::SymbolTable &table,
            const asIScriptEngine *engine)
        {
            const analysis::Symbol *sym = analysis::SymbolResolver::ResolveAt(doc, table, req.position.line, req.position.character);
            if (!sym)
                return {};

            if (sym->kind == analysis::SymbolKind::Class ||
                sym->kind == analysis::SymbolKind::Interface ||
                sym->kind == analysis::SymbolKind::Enum ||
                sym->kind == analysis::SymbolKind::Mixin ||
                sym->kind == analysis::SymbolKind::Typedef)
            {
                return SymbolToLocation(sym, doc.GetUri());
            }

            if (!sym->typeInfo.empty())
            {
                std::string typeName = analysis::SymbolResolver::CleanTypeName(sym->typeInfo);
                const analysis::Symbol *typeSym = table.FindByNameDeep(typeName);
                if (typeSym)
                {
                    return SymbolToLocation(typeSym, doc.GetUri());
                }
            }

            return {};
        }

} // namespace angel_lsp::features::definition
