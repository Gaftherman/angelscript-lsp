#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/ClassRuleFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/InterfaceImplementation.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/ClassRules.h"
#include "analysis/types/TypeSanitization.h"
#include "document/Document.h"
#include "spdlog/fmt/fmt.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Depth-first walk over the base lists starting at current, keeping only the active path so diamonds are not
 *        mistaken for cycles.
 * @param[in,out] path Types on the current walk; current is pushed on entry and popped on exit.
 * @param[in] current Type whose bases are expanded next.
 * @param[in] table Symbol table used to resolve each base.
 * @return True when a base on the walk is already on path, i.e. a cycle closes.
 */
bool WalkInheritancePath(std::vector<std::string_view>& path, std::string_view current, const SymbolTable& table)
{
    if (std::find(path.begin(), path.end(), current) != path.end())
    {
        return true;
    }
    if (path.size() > 64)
    {
        return false;
    }

    path.push_back(current);

    const auto symbols = table.FindSymbolsPtr(current);
    if (symbols)
    {
        for (const auto& sym : *symbols)
        {
            if (sym.type == SymbolType::Class)
            {
                for (const auto& base : sym.GetClass().bases)
                {
                    const std::string_view cleanBase = CleanBaseTypeView(base);
                    if (!cleanBase.empty() && WalkInheritancePath(path, cleanBase, table))
                    {
                        path.pop_back();
                        return true;
                    }
                }
            }
            else if (sym.type == SymbolType::Interface)
            {
                for (const auto& base : sym.GetInterface().inheritedInterfaces)
                {
                    const std::string_view cleanBase = CleanBaseTypeView(base);
                    if (!cleanBase.empty() && WalkInheritancePath(path, cleanBase, table))
                    {
                        path.pop_back();
                        return true;
                    }
                }
            }
        }
    }

    path.pop_back();
    return false;
}

bool HasInheritanceCycle(std::string_view typeName, const SymbolTable& table)
{
    const auto symbols = table.FindSymbolsPtr(typeName);
    if (!symbols)
    {
        return false;
    }

    std::vector<std::string_view> path;
    for (const auto& sym : *symbols)
    {
        auto checkBases = [&](const auto& bases)
        {
            path.assign(1, typeName);
            for (const auto& base : bases)
            {
                const std::string_view cleanBase = CleanBaseTypeView(base);
                if (!cleanBase.empty() && WalkInheritancePath(path, cleanBase, table))
                {
                    return true;
                }
            }
            return false;
        };

        if (sym.type == SymbolType::Class && checkBases(sym.GetClass().bases))
        {
            return true;
        }
        if (sym.type == SymbolType::Interface && checkBases(sym.GetInterface().inheritedInterfaces))
        {
            return true;
        }
    }

    return false;
}

} // namespace angel_lsp::analysis::rules
