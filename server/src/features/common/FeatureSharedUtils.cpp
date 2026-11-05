#include "features/common/FeatureSharedUtils.h"

namespace angel_lsp::features::common
{

lsp::SymbolKind ToLspSymbolKind(analysis::SymbolType type, bool isMember) noexcept
{
    switch (type)
    {
    case analysis::SymbolType::Class:
        return lsp::SymbolKind::Class;
    case analysis::SymbolType::Interface:
        return lsp::SymbolKind::Interface;
    case analysis::SymbolType::Enum:
        return lsp::SymbolKind::Enum;
    case analysis::SymbolType::Namespace:
        return lsp::SymbolKind::Namespace;
    case analysis::SymbolType::Funcdef:
        return lsp::SymbolKind::Function;
    case analysis::SymbolType::Typedef:
        return lsp::SymbolKind::Class;
    case analysis::SymbolType::Property:
        return lsp::SymbolKind::Property;
    case analysis::SymbolType::Function:
        return isMember ? lsp::SymbolKind::Method : lsp::SymbolKind::Function;
    case analysis::SymbolType::Variable:
        return isMember ? lsp::SymbolKind::Field : lsp::SymbolKind::Variable;
    default:
        return lsp::SymbolKind::Variable;
    }
}

} // namespace angel_lsp::features::common
