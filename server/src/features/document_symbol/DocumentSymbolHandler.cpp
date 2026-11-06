#include "features/document_symbol/DocumentSymbolHandler.h"

#include "analysis/resolution/SemanticHelpers.h"
#include "features/common/FeatureSharedUtils.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Outline range of a symbol: its full declaration span when indexed, else its start/end coordinates.
 * @param[in] sym Indexed symbol.
 * @return LSP range enclosing the whole declaration.
 */
static inline lsp::Range SymbolRange(const analysis::Symbol& sym) noexcept
{
    if (sym.fullRange.endLine != 0 || sym.fullRange.endCharacter != 0)
    {
        return common::ToLspRange(sym.fullRange);
    }
    return lsp::Range{lsp::Position{sym.startLine, sym.startCharacter}, lsp::Position{sym.endLine, sym.endCharacter}};
}

/**
 * @brief Range the client highlights when the outline entry is picked: the name span when indexed, else start/end.
 * @param[in] sym Indexed symbol.
 * @return LSP range of the declared name.
 */
static inline lsp::Range SymbolSelectionRange(const analysis::Symbol& sym) noexcept
{
    if (sym.selectionRange.endLine != 0 || sym.selectionRange.endCharacter != 0)
    {
        return common::ToLspRange(sym.selectionRange);
    }
    return lsp::Range{lsp::Position{sym.startLine, sym.startCharacter}, lsp::Position{sym.endLine, sym.endCharacter}};
}

/**
 * @brief Checks whether one range lies entirely inside another, endpoints inclusive.
 * @param[in] parent Candidate enclosing range.
 * @param[in] child Candidate nested range.
 * @return True if child starts at or after parent's start and ends at or before parent's end.
 */
static inline bool ContainsRange(const lsp::Range& parent, const lsp::Range& child) noexcept
{
    if (child.start.line < parent.start.line ||
        (child.start.line == parent.start.line && child.start.character < parent.start.character))
    {
        return false;
    }
    if (child.end.line > parent.end.line ||
        (child.end.line == parent.end.line && child.end.character > parent.end.character))
    {
        return false;
    }
    return true;
}

/**
 * @brief Appends a parenthesized "(type name, ...)" parameter list to a protocol detail string.
 * @param[in] params Declared parameters in source order.
 * @param[in,out] out Detail text being built in place.
 */
static void AppendParameterList(const std::vector<analysis::ParameterInformation>& params, lsp::String& out)
{
    out += '(';
    for (size_t i = 0; i < params.size(); ++i)
    {
        if (i > 0)
        {
            out += ", ";
        }
        out += params[i].typeName;
        if (!params[i].name.empty())
        {
            out += ' ';
            out += params[i].name;
        }
    }
    out += ')';
}

/**
 * @brief Writes the outline detail of a symbol (kind keyword, type or signature) directly into the protocol field.
 * @param[in] sym Symbol being described.
 * @param[in] isCtorOrDtor True for constructors and destructors, whose detail omits the return type.
 * @param[in,out] out Detail text; left empty when the symbol kind has no detail.
 */
static void AppendSymbolDetail(const analysis::Symbol& sym, bool isCtorOrDtor, lsp::String& out)
{
    switch (sym.type)
    {
    case analysis::SymbolType::Namespace:
        out += "namespace";
        return;
    case analysis::SymbolType::Class:
        out += (std::holds_alternative<analysis::ClassSignature>(sym.signature) && sym.GetClass().modifiers.isMixin)
                   ? "mixin"
                   : "class";
        return;
    case analysis::SymbolType::Interface:
        out += "interface";
        return;
    case analysis::SymbolType::Enum:
        out += "enum";
        return;
    case analysis::SymbolType::Function:
        if (std::holds_alternative<analysis::FunctionSignature>(sym.signature))
        {
            const auto& sig = sym.GetFunction();
            if (!isCtorOrDtor && !sig.returnType.empty())
            {
                out += sig.returnType;
                out += ' ';
            }
            AppendParameterList(sig.parameters, out);
        }
        return;
    case analysis::SymbolType::Variable:
    case analysis::SymbolType::Property:
        if (std::holds_alternative<analysis::VariableSignature>(sym.signature))
        {
            out += sym.GetVariable().typeName;
        }
        return;
    case analysis::SymbolType::Typedef:
        out += "typedef";
        if (std::holds_alternative<analysis::TypedefSignature>(sym.signature))
        {
            out += ' ';
            out += sym.GetTypedef().baseType;
        }
        return;
    case analysis::SymbolType::Funcdef:
        out += "funcdef";
        if (std::holds_alternative<analysis::FuncdefSignature>(sym.signature))
        {
            const auto& fs = sym.GetFuncdef();
            out += ' ';
            out += fs.returnType;
            out += ' ';
            AppendParameterList(fs.parameters, out);
        }
        return;
    default:
        return;
    }
}

static lsp::DocumentSymbol MakeDocumentSymbol(const analysis::Symbol& sym, std::string_view sourceCode,
                                              bool isInsideClass)
{
    const bool isDtor =
        (sym.type == analysis::SymbolType::Function && analysis::IsDestructorDeclaration(sym, sourceCode));
    const bool isCtor = (sym.type == analysis::SymbolType::Function &&
                         (isDtor || (isInsideClass && sym.name == utils::LastScopeSegment(sym.containerName))));

    lsp::DocumentSymbol docSym;
    docSym.name = isDtor ? ("~" + sym.name) : sym.name;

    if (isDtor || isCtor)
    {
        docSym.kind = lsp::SymbolKind::Constructor;
    }
    else if (sym.type == analysis::SymbolType::Function)
    {
        docSym.kind = isInsideClass ? lsp::SymbolKind::Method : lsp::SymbolKind::Function;
    }
    else if (sym.type == analysis::SymbolType::Variable)
    {
        docSym.kind = isInsideClass ? lsp::SymbolKind::Field : lsp::SymbolKind::Variable;
    }
    else
    {
        docSym.kind = common::ToLspSymbolKind(sym.type, isInsideClass);
    }

    docSym.range = SymbolRange(sym);
    docSym.selectionRange = SymbolSelectionRange(sym);

    if (docSym.selectionRange.start.line < docSym.range.start.line ||
        (docSym.selectionRange.start.line == docSym.range.start.line &&
         docSym.selectionRange.start.character < docSym.range.start.character))
    {
        docSym.range.start = docSym.selectionRange.start;
    }
    if (docSym.selectionRange.end.line > docSym.range.end.line ||
        (docSym.selectionRange.end.line == docSym.range.end.line &&
         docSym.selectionRange.end.character > docSym.range.end.character))
    {
        docSym.range.end = docSym.selectionRange.end;
    }

    lsp::String detail;
    AppendSymbolDetail(sym, isDtor || isCtor, detail);
    if (!detail.empty())
    {
        docSym.detail = std::move(detail);
    }

    return docSym;
}

static void PopulateEnumMembers(const analysis::Symbol& enumSym, const analysis::SymbolTable& table,
                                lsp::DocumentSymbol& docSym)
{
    if (!std::holds_alternative<analysis::EnumSignature>(enumSym.signature))
    {
        return;
    }
    const auto& enumSig = enumSym.GetEnum();
    if (enumSig.members.empty())
    {
        return;
    }

    std::vector<lsp::DocumentSymbol> members;
    members.reserve(enumSig.members.size());

    const std::string_view enumContainer = enumSym.qualifiedName.empty() ? enumSym.name : enumSym.qualifiedName;
    for (const auto& m : enumSig.members)
    {
        lsp::DocumentSymbol mSym;
        mSym.name = m.name;
        mSym.kind = lsp::SymbolKind::EnumMember;
        if (!m.value.empty())
        {
            mSym.detail = "= " + m.value;
        }

        auto memberSymbols = table.FindMemberSymbolPtr(enumContainer, m.name);
        if (memberSymbols && !memberSymbols->empty())
        {
            mSym.range = SymbolRange((*memberSymbols)[0]);
            mSym.selectionRange = SymbolSelectionRange((*memberSymbols)[0]);
        }
        else
        {
            mSym.range = docSym.range;
            mSym.selectionRange = docSym.range;
        }

        if (mSym.selectionRange.start.line < mSym.range.start.line ||
            (mSym.selectionRange.start.line == mSym.range.start.line &&
             mSym.selectionRange.start.character < mSym.range.start.character))
        {
            mSym.range.start = mSym.selectionRange.start;
        }
        if (mSym.selectionRange.end.line > mSym.range.end.line ||
            (mSym.selectionRange.end.line == mSym.range.end.line &&
             mSym.selectionRange.end.character > mSym.range.end.character))
        {
            mSym.range.end = mSym.selectionRange.end;
        }

        members.push_back(std::move(mSym));
    }

    docSym.children = std::move(members);
}

std::optional<DocumentSymbolResult> GetDocumentSymbols(const DocumentSymbolRequest& request)
{
    if (request.sourceCode.empty())
    {
        return DocumentSymbolResult{};
    }

    std::vector<const analysis::Symbol*> rawSymbols;
    ankerl::unordered_dense::set<std::pair<std::string_view, uint64_t>> seen;

    request.symbolTable.ForEachSymbolInFile(
        request.uri,
        [&]([[maybe_unused]] std::string_view key, const std::vector<analysis::Symbol>& bucket)
        {
            for (const auto& sym : bucket)
            {
                if (sym.isSynthesized || sym.type == analysis::SymbolType::CallReference || sym.fileUri != request.uri)
                {
                    continue;
                }
                if (sym.type == analysis::SymbolType::Variable &&
                    std::holds_alternative<analysis::VariableSignature>(sym.signature) &&
                    sym.GetVariable().isEnumConstant)
                {
                    continue;
                }
                const uint64_t coordKey =
                    (static_cast<uint64_t>(sym.startLine) << 32) | static_cast<uint64_t>(sym.startCharacter);
                if (seen.insert({sym.name, coordKey}).second)
                {
                    rawSymbols.push_back(&sym);
                }
            }
        });

    std::sort(rawSymbols.begin(), rawSymbols.end(),
              [](const analysis::Symbol* a, const analysis::Symbol* b)
              {
                  const auto aRange = SymbolRange(*a);
                  const auto bRange = SymbolRange(*b);
                  if (aRange.start.line != bRange.start.line)
                  {
                      return aRange.start.line < bRange.start.line;
                  }
                  if (aRange.start.character != bRange.start.character)
                  {
                      return aRange.start.character < bRange.start.character;
                  }
                  if (aRange.end.line != bRange.end.line)
                  {
                      return aRange.end.line > bRange.end.line;
                  }
                  return aRange.end.character > bRange.end.character;
              });

    std::vector<std::pair<lsp::Range, lsp::DocumentSymbol*>> stack;
    DocumentSymbolResult roots;

    for (const auto* sym : rawSymbols)
    {
        lsp::Range symRange = SymbolRange(*sym);
        while (!stack.empty() && !ContainsRange(stack.back().first, symRange))
        {
            stack.pop_back();
        }

        const bool isInsideClass = !stack.empty() && (stack.back().second->kind == lsp::SymbolKind::Class ||
                                                      stack.back().second->kind == lsp::SymbolKind::Interface);

        lsp::DocumentSymbol docSym = MakeDocumentSymbol(*sym, request.sourceCode, isInsideClass);
        if (sym->type == analysis::SymbolType::Enum)
        {
            PopulateEnumMembers(*sym, request.symbolTable, docSym);
        }

        const bool isContainer =
            (sym->type == analysis::SymbolType::Namespace || sym->type == analysis::SymbolType::Class ||
             sym->type == analysis::SymbolType::Interface);

        lsp::DocumentSymbol* target = nullptr;
        if (stack.empty())
        {
            roots.push_back(std::move(docSym));
            target = &roots.back();
        }
        else
        {
            if (!stack.back().second->children.has_value())
            {
                stack.back().second->children = std::vector<lsp::DocumentSymbol>{};
            }
            stack.back().second->children->push_back(std::move(docSym));
            target = &stack.back().second->children->back();
        }

        if (isContainer)
        {
            stack.push_back({target->range, target});
        }
    }

    return roots;
}

} // namespace angel_lsp::features
