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

static inline lsp::Range SymbolRange(const analysis::Symbol& sym) noexcept
{
    if (sym.fullRange.endLine != 0 || sym.fullRange.endCharacter != 0)
    {
        return common::ToLspRange(sym.fullRange);
    }
    return lsp::Range{lsp::Position{sym.startLine, sym.startCharacter}, lsp::Position{sym.endLine, sym.endCharacter}};
}

static inline lsp::Range SymbolSelectionRange(const analysis::Symbol& sym) noexcept
{
    if (sym.selectionRange.endLine != 0 || sym.selectionRange.endCharacter != 0)
    {
        return common::ToLspRange(sym.selectionRange);
    }
    return lsp::Range{lsp::Position{sym.startLine, sym.startCharacter}, lsp::Position{sym.endLine, sym.endCharacter}};
}

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

static std::string BuildFunctionDetail(const analysis::FunctionSignature& sig, bool isCtorOrDtor)
{
    std::string paramsStr = "(";
    for (size_t i = 0; i < sig.parameters.size(); ++i)
    {
        if (i > 0)
        {
            paramsStr += ", ";
        }
        paramsStr += sig.parameters[i].typeName;
        if (!sig.parameters[i].name.empty())
        {
            paramsStr += " ";
            paramsStr += sig.parameters[i].name;
        }
    }
    paramsStr += ")";

    if (isCtorOrDtor)
    {
        return paramsStr;
    }
    if (!sig.returnType.empty())
    {
        return sig.returnType + " " + paramsStr;
    }
    return paramsStr;
}

static std::string FormatSymbolDetail(const analysis::Symbol& sym, bool isDestructor, bool isConstructor)
{
    switch (sym.type)
    {
    case analysis::SymbolType::Namespace:
        return "namespace";
    case analysis::SymbolType::Class:
        if (std::holds_alternative<analysis::ClassSignature>(sym.signature) && sym.GetClass().modifiers.isMixin)
        {
            return "mixin";
        }
        return "class";
    case analysis::SymbolType::Interface:
        return "interface";
    case analysis::SymbolType::Enum:
        return "enum";
    case analysis::SymbolType::Function:
        if (std::holds_alternative<analysis::FunctionSignature>(sym.signature))
        {
            return BuildFunctionDetail(sym.GetFunction(), isDestructor || isConstructor);
        }
        return "";
    case analysis::SymbolType::Variable:
        if (std::holds_alternative<analysis::VariableSignature>(sym.signature))
        {
            return sym.GetVariable().typeName;
        }
        return "";
    case analysis::SymbolType::Property:
        if (std::holds_alternative<analysis::VariableSignature>(sym.signature))
        {
            return sym.GetVariable().typeName;
        }
        return "";
    case analysis::SymbolType::Typedef:
        if (std::holds_alternative<analysis::TypedefSignature>(sym.signature))
        {
            return "typedef " + sym.GetTypedef().baseType;
        }
        return "typedef";
    case analysis::SymbolType::Funcdef:
        if (std::holds_alternative<analysis::FuncdefSignature>(sym.signature))
        {
            const auto& fs = sym.GetFuncdef();
            std::string p = "(";
            for (size_t i = 0; i < fs.parameters.size(); ++i)
            {
                if (i > 0)
                {
                    p += ", ";
                }
                p += fs.parameters[i].typeName;
                if (!fs.parameters[i].name.empty())
                {
                    p += " ";
                    p += fs.parameters[i].name;
                }
            }
            p += ")";
            return "funcdef " + fs.returnType + " " + p;
        }
        return "funcdef";
    default:
        return "";
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

    std::string detail = FormatSymbolDetail(sym, isDtor, isCtor);
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

    const std::string enumContainer = enumSym.qualifiedName.empty() ? enumSym.name : enumSym.qualifiedName;
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
        std::string(request.uri),
        [&]([[maybe_unused]] const std::string& key, const std::vector<analysis::Symbol>& bucket)
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
