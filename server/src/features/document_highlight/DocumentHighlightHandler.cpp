#include "features/document_highlight/DocumentHighlightHandler.h"

#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TargetResolution.h"
#include "features/common/FeatureSharedUtils.h"
#include "utils/PositionEncoding.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Checks if an occurrence represents a declaration location.
 * @param[in] target Target symbol descriptor.
 * @param[in] range Occurrence source coordinate range.
 * @param[in] symbolTable Workspace symbol table.
 * @param[in] uri Document file URI.
 * @return DocumentHighlightKind if matching declaration; std::nullopt otherwise.
 */
static std::optional<lsp::DocumentHighlightKind> CheckDeclaration(const analysis::TargetDescriptor& target,
                                                                  const lsp::Range& range,
                                                                  const analysis::SymbolTable& symbolTable,
                                                                  std::string_view uri)
{
    if (target.kind == analysis::TargetKind::Local)
    {
        if (range.start.line == target.localDef.startLine && range.start.character == target.localDef.startCharacter)
        {
            return (target.localDef.kind == analysis::LocalDefinitionKind::Variable ||
                    target.localDef.kind == analysis::LocalDefinitionKind::Parameter)
                       ? lsp::DocumentHighlightKind::Write
                       : lsp::DocumentHighlightKind::Text;
        }
    }
    std::vector<analysis::Symbol> syms;
    if (target.kind == analysis::TargetKind::ClassMember && !target.declaringClass.empty())
    {
        syms = symbolTable.FindMemberSymbols(target.declaringClass, target.name);
    }
    else if (!target.qualifiedName.empty())
    {
        syms = symbolTable.FindSymbols(target.qualifiedName);
        if (syms.empty())
        {
            syms = symbolTable.FindSymbols(target.name);
        }
    }
    else
    {
        syms = symbolTable.FindSymbols(target.name);
    }

    for (const auto& sym : syms)
    {
        if (sym.fileUri == uri &&
            (sym.selectionRange.startLine == range.start.line || sym.startLine == range.start.line))
        {
            if (sym.type == analysis::SymbolType::Variable)
            {
                return lsp::DocumentHighlightKind::Write;
            }
            if (sym.type == analysis::SymbolType::Function || sym.type == analysis::SymbolType::Funcdef ||
                sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface ||
                sym.type == analysis::SymbolType::Typedef || sym.type == analysis::SymbolType::Enum)
            {
                return lsp::DocumentHighlightKind::Text;
            }
        }
    }
    return std::nullopt;
}

/**
 * @brief Determines if an identifier occurrence is mutated by an operator.
 * @param[in] sourceCode Source text buffer.
 * @param[in] startByte Starting byte offset of occurrence.
 * @param[in] endByte Ending byte offset of occurrence.
 * @return True if occurrence is subject to mutation.
 */
static bool IsAssignmentOrMutation(std::string_view sourceCode, size_t startByte, size_t endByte)
{
    size_t pre = startByte;
    while (pre > 0 && (sourceCode[pre - 1] == ' ' || sourceCode[pre - 1] == '\t'))
    {
        --pre;
    }
    if (pre >= 2 && (sourceCode.substr(pre - 2, 2) == "++" || sourceCode.substr(pre - 2, 2) == "--"))
    {
        return true;
    }
    size_t post = endByte;
    while (post < sourceCode.size() && (sourceCode[post] == ' ' || sourceCode[post] == '\t'))
    {
        ++post;
    }
    if (post + 1 < sourceCode.size() && (sourceCode.substr(post, 2) == "++" || sourceCode.substr(post, 2) == "--"))
    {
        return true;
    }
    if (post < sourceCode.size() && sourceCode[post] == '.')
    {
        return false;
    }
    if (post < sourceCode.size() && sourceCode[post] == '=')
    {
        return !(post + 1 < sourceCode.size() && (sourceCode[post + 1] == '=' || sourceCode[post + 1] == '>'));
    }
    for (std::string_view op : {">>>=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="})
    {
        if (post + op.size() <= sourceCode.size() && sourceCode.substr(post, op.size()) == op)
        {
            return true;
        }
    }
    return false;
}

static std::pair<size_t, size_t> FindEnclosingCallParen(std::string_view sourceCode, size_t startByte)
{
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;
    size_t commaCount = 0;
    for (size_t i = startByte; i > 0; --i)
    {
        char ch = sourceCode[i - 1];
        if (ch == ';')
        {
            break;
        }
        if (ch == ')')
        {
            ++parenDepth;
        }
        else if (ch == '(')
        {
            if (parenDepth > 0)
            {
                --parenDepth;
            }
            else
            {
                return {i - 1, commaCount};
            }
        }
        else if (ch == ']')
        {
            ++bracketDepth;
        }
        else if (ch == '[')
        {
            if (bracketDepth > 0)
            {
                --bracketDepth;
            }
        }
        else if (ch == '}')
        {
            ++braceDepth;
        }
        else if (ch == '{')
        {
            if (braceDepth > 0)
            {
                --braceDepth;
            }
        }
        else if (ch == ',' && parenDepth == 0 && bracketDepth == 0 && braceDepth == 0)
        {
            ++commaCount;
        }
    }
    return {std::string_view::npos, commaCount};
}

static bool IsMutableParam(const analysis::ParameterInformation& param)
{
    return param.modifier == analysis::ParameterModifier::Out || param.modifier == analysis::ParameterModifier::InOut ||
           (param.isReference && !param.isConst && param.modifier != analysis::ParameterModifier::In);
}

static std::string ResolveReceiverType(std::string_view objName, const DocumentHighlightRequest& req, uint32_t line)
{
    if (objName.empty())
    {
        return {};
    }
    auto rootScope = req.scopeIndex.GetRoot(req.uri);
    if (rootScope)
    {
        const analysis::Scope* s = FindInnermostScope(rootScope.get(), line, 0);
        if (s)
        {
            const analysis::LocalDefinition* oDef = analysis::ResolveInScope(s, objName);
            if (oDef && !oDef->typeName.empty())
            {
                return std::string(analysis::CleanBaseType(oDef->typeName));
            }
        }
    }
    auto gSyms = req.symbolTable.FindSymbols(objName);
    for (const auto& gs : gSyms)
    {
        if (gs.type == analysis::SymbolType::Variable && !gs.GetVariable().typeName.empty())
        {
            return std::string(analysis::CleanBaseType(gs.GetVariable().typeName));
        }
    }
    return {};
}

static std::vector<analysis::Symbol> FindCalleeSymbols(std::string_view sourceCode, size_t fnStart,
                                                       std::string_view callee, const DocumentHighlightRequest& req,
                                                       uint32_t line)
{
    std::vector<analysis::Symbol> syms;
    if (fnStart > 0 && sourceCode[fnStart - 1] == '.')
    {
        size_t objEnd = fnStart - 1;
        while (objEnd > 0 && (sourceCode[objEnd - 1] == ' ' || sourceCode[objEnd - 1] == '\t'))
        {
            --objEnd;
        }
        size_t objStart = objEnd;
        while (objStart > 0 && (std::isalnum(static_cast<unsigned char>(sourceCode[objStart - 1])) ||
                                sourceCode[objStart - 1] == '_' || sourceCode[objStart - 1] == ':'))
        {
            --objStart;
        }
        if (objStart < objEnd)
        {
            std::string_view objName = sourceCode.substr(objStart, objEnd - objStart);
            std::string recvType = ResolveReceiverType(objName, req, line);
            if (!recvType.empty())
            {
                syms = req.symbolTable.FindMemberSymbols(recvType, callee);
            }
        }
    }
    if (syms.empty())
    {
        syms = req.symbolTable.FindSymbols(callee);
    }
    if (syms.empty())
    {
        syms = req.symbolTable.FindSymbols(utils::LastScopeSegment(callee));
    }
    return syms;
}

/**
 * @brief Tests whether an identifier is passed as an &out or &inout argument.
 * @param[in] sourceCode Source text buffer.
 * @param[in] startByte Starting byte offset of argument identifier.
 * @param[in] req Document highlight request.
 * @param[in] line 0-based source line.
 * @return True if matching parameter is mutable by the call.
 */
static bool IsPassedAsOutOrInOutArg(std::string_view sourceCode, size_t startByte, const DocumentHighlightRequest& req,
                                    uint32_t line)
{
    if (startByte == 0 || startByte > sourceCode.size())
    {
        return false;
    }
    auto [openParen, commaCount] = FindEnclosingCallParen(sourceCode, startByte);
    if (openParen == std::string_view::npos || openParen == 0)
    {
        return false;
    }
    size_t fnEnd = openParen;
    while (fnEnd > 0 && (sourceCode[fnEnd - 1] == ' ' || sourceCode[fnEnd - 1] == '\t'))
    {
        --fnEnd;
    }
    size_t fnStart = fnEnd;
    while (fnStart > 0 && (std::isalnum(static_cast<unsigned char>(sourceCode[fnStart - 1])) ||
                           sourceCode[fnStart - 1] == '_' || sourceCode[fnStart - 1] == ':'))
    {
        --fnStart;
    }
    if (fnStart == fnEnd)
    {
        return false;
    }
    std::string_view callee = sourceCode.substr(fnStart, fnEnd - fnStart);
    auto syms = FindCalleeSymbols(sourceCode, fnStart, callee, req, line);
    for (const auto& sym : syms)
    {
        if (sym.type == analysis::SymbolType::Function)
        {
            const auto& sig = sym.GetFunction();
            if (commaCount < sig.parameters.size() && IsMutableParam(sig.parameters[commaCount]))
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Classifies an occurrence coordinate into Write, Read, or Text highlight kind.
 * @param[in] request Document highlight request.
 * @param[in] target Target symbol descriptor.
 * @param[in] range Coordinate range of occurrence.
 * @return DocumentHighlightKind corresponding to the syntactic role.
 */
static lsp::DocumentHighlightKind ClassifyOccurrence(const DocumentHighlightRequest& request,
                                                     const analysis::TargetDescriptor& target, const lsp::Range& range)
{
    if (auto declKind = CheckDeclaration(target, range, request.symbolTable, request.uri))
    {
        return *declKind;
    }
    auto syms = request.symbolTable.FindSymbols(target.name);
    bool isTypeOrNamespace = !syms.empty() && std::all_of(syms.begin(), syms.end(),
                                                          [](const analysis::Symbol& s)
                                                          {
                                                              return s.type == analysis::SymbolType::Class ||
                                                                     s.type == analysis::SymbolType::Interface ||
                                                                     s.type == analysis::SymbolType::Typedef ||
                                                                     s.type == analysis::SymbolType::Namespace ||
                                                                     s.type == analysis::SymbolType::Enum;
                                                          });
    if (isTypeOrNamespace)
    {
        return lsp::DocumentHighlightKind::Text;
    }
    if (target.isFunction)
    {
        return lsp::DocumentHighlightKind::Read;
    }

    size_t sByte = utils::PositionToOffset(request.sourceCode, range.start.line, range.start.character,
                                           utils::PositionEncoding::Utf16);
    size_t eByte = utils::PositionToOffset(request.sourceCode, range.end.line, range.end.character,
                                           utils::PositionEncoding::Utf16);
    if (IsAssignmentOrMutation(request.sourceCode, sByte, eByte))
    {
        return lsp::DocumentHighlightKind::Write;
    }
    if (IsPassedAsOutOrInOutArg(request.sourceCode, sByte, request, range.start.line))
    {
        return lsp::DocumentHighlightKind::Write;
    }
    return lsp::DocumentHighlightKind::Read;
}

std::optional<DocumentHighlightResult> GetDocumentHighlights(const DocumentHighlightRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::nullopt;
    }

    auto resolveReq = analysis::MakeTargetRequest(
        request.uri, request.sourceCode, analysis::TargetPosition{request.position.line, request.position.character},
        request.symbolTable, request.scopeIndex);
    auto targetOpt = analysis::ResolveTargetSymbol(resolveReq);
    if (!targetOpt.has_value())
    {
        return std::nullopt;
    }

    auto occReq = analysis::MakeOccurrencesRequest(*targetOpt, request.uri, request.sourceCode, request.symbolTable,
                                                   request.scopeIndex);
    occReq.includeDeclaration = true;
    const auto occurrences = analysis::CollectOccurrences(occReq);

    std::vector<lsp::Range> ranges;
    for (const auto& occ : occurrences)
    {
        if (occ.fileUri == request.uri)
        {
            ranges.push_back(lsp::Range{
                lsp::Position{occ.range.startLine, occ.range.startCharacter},
                lsp::Position{occ.range.endLine, occ.range.endCharacter},
            });
        }
    }
    if (ranges.empty())
    {
        return std::nullopt;
    }

    std::sort(ranges.begin(), ranges.end(), common::CompareByPosition<lsp::Range>);
    ranges.erase(std::unique(ranges.begin(), ranges.end(), common::SameRange), ranges.end());

    DocumentHighlightResult results;
    results.reserve(ranges.size());
    for (const auto& r : ranges)
    {
        results.push_back(lsp::DocumentHighlight{
            .range = r,
            .kind = ClassifyOccurrence(request, *targetOpt, r),
        });
    }

    return results;
}

} // namespace angel_lsp::features
