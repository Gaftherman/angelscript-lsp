#include "features/definition/DefinitionHandler.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/OverloadResolver.h"
#include "utils/Utils.h"
#include <unordered_set>
#include <vector>
#include "parser/GrammarNames.h"

namespace angel_lsp::features
{
    namespace
    {

        std::string GetNodeTextAt(const DefinitionRequest &request, TSNode &outNode)
        {
            if (!request.tree || request.sourceCode.empty())
            {
                return "";
            }

            TSNode rootNode = ts_tree_root_node(request.tree);
            TSPoint point = { request.position.line, request.position.character };
            TSNode node = ts_node_descendant_for_point_range(rootNode, point, point);

            if (ts_node_is_null(node))
            {
                return "";
            }

            std::string_view nodeType = ts_node_type(node);
            if (nodeType != "identifier" && nodeType != "primitive_type" && nodeType != "scoped_identifier")
            {
                if (request.position.character > 0)
                {
                    TSPoint prevPoint = { request.position.line, request.position.character - 1 };
                    TSNode prevNode = ts_node_descendant_for_point_range(rootNode, prevPoint, prevPoint);
                    if (!ts_node_is_null(prevNode))
                    {
                        std::string_view prevType = ts_node_type(prevNode);
                        if (prevType == "identifier" || prevType == "primitive_type" || prevType == "scoped_identifier")
                        {
                            node = prevNode;
                            nodeType = prevType;
                        }
                    }
                }
            }

            if (nodeType != "identifier" && nodeType != "primitive_type" && nodeType != "scoped_identifier")
            {
                return "";
            }

            uint32_t startByte = ts_node_start_byte(node);
            uint32_t endByte = ts_node_end_byte(node);
            if (startByte >= request.sourceCode.size() || endByte > request.sourceCode.size() || startByte >= endByte)
            {
                return "";
            }

            outNode = node;
            return request.sourceCode.substr(startByte, endByte - startByte);
        }

        const analysis::Scope *FindScopeDeclaringDefinition(const analysis::Scope *current, const analysis::LocalDefinition &def)
        {
            if (!current)
            {
                return nullptr;
            }

            for (const auto &d : current->definitions)
            {
                if (d.name == def.name &&
                    d.startLine == def.startLine &&
                    d.startCharacter == def.startCharacter)
                {
                    return current;
                }
            }

            for (const auto &child : current->children)
            {
                if (const auto *found = FindScopeDeclaringDefinition(child.get(), def))
                {
                    return found;
                }
            }

            return nullptr;
        }

        /**
         * @brief If candidate symbols contain multiple functions and the cursor is on a call's callee,
         * uses overload resolution to filter candidates down to the best matching function.
         */
        std::vector<analysis::Symbol> FilterOverloadsForCall(
            TSNode node,
            const std::vector<analysis::Symbol> &candidates,
            const DefinitionRequest &request,
            const analysis::Scope *scope)
        {
            if (candidates.empty())
            {
                return candidates;
            }

            bool hasFunction = false;
            for (const auto &sym : candidates)
            {
                if (sym.type == analysis::SymbolType::Function)
                {
                    hasFunction = true;
                    break;
                }
            }
            if (!hasFunction)
            {
                return candidates;
            }

            TSNode callNode{};
            TSNode parent = ts_node_parent(node);
            if (!ts_node_is_null(parent))
            {
                std::string_view pType = ts_node_type(parent);
                if (pType == "call_expression")
                {
                    TSNode fn = parser::GetChildByField(parent, parser::fields::Function);
                    if (!ts_node_is_null(fn) && (ts_node_eq(fn, node) || ts_node_start_byte(fn) == ts_node_start_byte(node)))
                    {
                        callNode = parent;
                    }
                }
                else if (pType == "member_expression" || pType == "scoped_identifier")
                {
                    TSNode grandParent = ts_node_parent(parent);
                    if (!ts_node_is_null(grandParent) && std::string_view(ts_node_type(grandParent)) == "call_expression")
                    {
                        TSNode fn = parser::GetChildByField(grandParent, parser::fields::Function);
                        if (!ts_node_is_null(fn) && (ts_node_eq(fn, parent) || ts_node_start_byte(fn) == ts_node_start_byte(parent)))
                        {
                            callNode = grandParent;
                        }
                    }
                }
            }

            if (ts_node_is_null(callNode))
            {
                return candidates;
            }

            TSNode argListNode = parser::GetChildByField(callNode, parser::fields::Arguments);
            if (ts_node_is_null(argListNode))
            {
                for (uint32_t i = 0; i < ts_node_child_count(callNode); ++i)
                {
                    TSNode ch = ts_node_child(callNode, i);
                    if (std::string_view(ts_node_type(ch)) == "argument_list")
                    {
                        argListNode = ch;
                        break;
                    }
                }
            }

            std::vector<std::string> argTypes;
            if (!ts_node_is_null(argListNode))
            {
                uint32_t count = ts_node_child_count(argListNode);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode ch = ts_node_child(argListNode, i);
                    std::string_view ct = ts_node_type(ch);
                    if (ct == "(" || ct == ")" || ct == "," || ct == "comment" || ct == ":")
                    {
                        continue;
                    }
                    const char *fieldName = ts_node_field_name_for_child(argListNode, i);
                    if (fieldName && std::string_view(fieldName) == "arg_name")
                    {
                        continue;
                    }
                    std::string aType = analysis::ResolveExpressionType(
                        ch, scope, request.symbolTable, request.sourceCode, request.uri);
                    argTypes.push_back(std::move(aType));
                }
            }

            const uint32_t argCount = static_cast<uint32_t>(argTypes.size());

            std::vector<analysis::Symbol> funcCandidates;
            for (const auto &sym : candidates)
            {
                if (sym.type == analysis::SymbolType::Function)
                {
                    funcCandidates.push_back(sym);
                }
            }

            // Check if any existing candidate matches this call's argument count
            bool hasArityMatch = false;
            for (const auto &sym : funcCandidates)
            {
                if (std::holds_alternative<analysis::FunctionSignature>(sym.signature))
                {
                    const auto &sig = sym.GetFunction();
                    uint32_t requiredParams = 0;
                    uint32_t maxParams = 0;
                    bool isVariadic = false;
                    for (const auto &param : sig.parameters)
                    {
                        if (param.rawText.find("...") != std::string::npos)
                        {
                            isVariadic = true;
                            continue;
                        }
                        ++maxParams;
                        if (param.defaultValue.empty())
                        {
                            ++requiredParams;
                        }
                    }
                    if (argCount >= requiredParams && (isVariadic || argCount <= maxParams))
                    {
                        hasArityMatch = true;
                        break;
                    }
                }
            }

            // If candidates don't have an arity match, look up overloads from enclosing class hierarchy & mixins
            if (!hasArityMatch)
            {
                std::string targetMethodName = ts_node_is_null(node) ? "" : analysis::GetNodeText(node, request.sourceCode);
                auto containers = analysis::GetEnclosingContainers(node, request.sourceCode);
                for (const auto &c : containers)
                {
                    if (c.kind == analysis::ContainerKind::Class)
                    {
                        auto hierarchy = analysis::GetInheritedTypeHierarchy(c.qualifiedName.empty() ? c.name : c.qualifiedName, request.symbolTable);
                        for (const auto &cls : hierarchy)
                        {
                            auto found = request.symbolTable.FindSymbols(cls + "::" + targetMethodName);
                            for (const auto &sym : found)
                            {
                                if (sym.type == analysis::SymbolType::Function && std::holds_alternative<analysis::FunctionSignature>(sym.signature))
                                {
                                    if (std::none_of(funcCandidates.begin(), funcCandidates.end(), [&](const analysis::Symbol &existing) {
                                        return existing.qualifiedName == sym.qualifiedName && analysis::HasSameParameterList(existing, sym);
                                    }))
                                    {
                                        funcCandidates.push_back(sym);
                                    }
                                }
                            }
                        }

                        auto hostSyms = request.symbolTable.FindSymbols(c.qualifiedName.empty() ? c.name : c.qualifiedName);
                        for (const auto &hs : hostSyms)
                        {
                            if (hs.type == analysis::SymbolType::Class && std::holds_alternative<analysis::ClassSignature>(hs.signature))
                            {
                                for (const auto &mixinName : hs.GetClass().includedMixins)
                                {
                                    auto mixinMethods = request.symbolTable.FindSymbols(mixinName + "::" + targetMethodName);
                                    for (const auto &sym : mixinMethods)
                                    {
                                        if (sym.type == analysis::SymbolType::Function && std::holds_alternative<analysis::FunctionSignature>(sym.signature))
                                        {
                                            if (std::none_of(funcCandidates.begin(), funcCandidates.end(), [&](const analysis::Symbol &existing) {
                                                return existing.qualifiedName == sym.qualifiedName && analysis::HasSameParameterList(existing, sym);
                                            }))
                                            {
                                                funcCandidates.push_back(sym);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        break;
                    }
                }
            }

            if (funcCandidates.size() <= 1 && hasArityMatch)
            {
                return candidates;
            }

            auto match = analysis::ResolveBestOverload(funcCandidates, argTypes, request.symbolTable);
            if (match.bestCandidate != nullptr)
            {
                return { *match.bestCandidate };
            }

            // Fallback: match by arity if an exact type match was not found
            const analysis::Symbol *bestFallback = nullptr;
            int bestFallbackScore = -10000;

            for (const auto &sym : funcCandidates)
            {
                if (!std::holds_alternative<analysis::FunctionSignature>(sym.signature))
                {
                    continue;
                }

                const auto &sig = sym.GetFunction();
                uint32_t requiredParams = 0;
                uint32_t maxParams = 0;
                bool isVariadic = false;

                for (const auto &param : sig.parameters)
                {
                    if (param.rawText.find("...") != std::string::npos)
                    {
                        isVariadic = true;
                        continue;
                    }
                    ++maxParams;
                    if (param.defaultValue.empty())
                    {
                        ++requiredParams;
                    }
                }

                int score = 0;
                bool arityMatches = (argCount >= requiredParams) && (isVariadic || argCount <= maxParams);
                if (arityMatches)
                {
                    score += 100;
                    if (argCount == sig.parameters.size())
                    {
                        score += 50;
                    }
                }
                else
                {
                    int diff = std::abs(static_cast<int>(argCount) - static_cast<int>(sig.parameters.size()));
                    score -= diff * 20;
                }

                for (size_t i = 0; i < argTypes.size() && i < sig.parameters.size(); ++i)
                {
                    if (!argTypes[i].empty())
                    {
                        int pScore = analysis::ScoreArgumentMatch(argTypes[i], sig.parameters[i], request.symbolTable);
                        if (pScore < 999)
                        {
                            score += 10;
                        }
                    }
                }

                if (score > bestFallbackScore)
                {
                    bestFallbackScore = score;
                    bestFallback = &sym;
                }
            }

            if (bestFallback != nullptr && bestFallbackScore > 0)
            {
                return { *bestFallback };
            }

            // Secondary fallback: match any function candidate with exact parameter count
            for (const auto &sym : funcCandidates)
            {
                if (std::holds_alternative<analysis::FunctionSignature>(sym.signature))
                {
                    if (sym.GetFunction().parameters.size() == argCount)
                    {
                        return { sym };
                    }
                }
            }

            return candidates;
        }

        /**
         * @brief Converts a Symbol into an LSP Location, routing to virtual mixin documents if enabled.
         * @param sym Resolved symbol.
         * @param request Definition context containing the symbol table.
         * @return Formatted LSP Location with appropriate URI and mapped line range.
         */
        lsp::Location MakeLocation(const analysis::Symbol &sym, const DefinitionRequest &request)
        {
            if (request.uri.starts_with("angelscript-virtual:"))
            {
                std::string_view s = request.uri;
                if (s.starts_with("angelscript-virtual://"))
                {
                    s.remove_prefix(22);
                }
                else if (s.starts_with("angelscript-virtual:"))
                {
                    s.remove_prefix(20);
                }
                auto slashPos = s.find('/');
                std::string mixinPart = slashPos != std::string_view::npos ? std::string(s.substr(slashPos + 1)) : std::string(s);
                if (mixinPart.ends_with(".as"))
                {
                    mixinPart = mixinPart.substr(0, mixinPart.size() - 3);
                }
                mixinPart = utils::UrlDecode(mixinPart);

                const analysis::Symbol *mSym = nullptr;
                auto cand = request.symbolTable.FindSymbols(mixinPart);
                for (const auto &c : cand)
                {
                    if (c.type == analysis::SymbolType::Class)
                    {
                        mSym = &c;
                        break;
                    }
                }
                if (!mSym)
                {
                    std::string shortName = mixinPart;
                    auto lastScope = shortName.rfind("::");
                    if (lastScope != std::string::npos)
                    {
                        shortName = shortName.substr(lastScope + 2);
                    }
                    auto sCand = request.symbolTable.FindTypeSymbolsByShortName(shortName);
                    for (const auto &c : sCand)
                    {
                        if (c.type == analysis::SymbolType::Class)
                        {
                            mSym = &c;
                            break;
                        }
                    }
                }

                if (mSym && sym.fileUri == mSym->fileUri)
                {
                    uint32_t mappedStartLine = 3 + (sym.startLine >= mSym->startLine ? (sym.startLine - mSym->startLine) : sym.startLine);
                    uint32_t lineDiff = sym.endLine >= sym.startLine ? (sym.endLine - sym.startLine) : 0;
                    uint32_t mappedEndLine = mappedStartLine + lineDiff;
                    return lsp::Location{
                        lsp::DocumentUri::parse(request.uri),
                        lsp::Range{
                            lsp::Position{ mappedStartLine, sym.startCharacter },
                            lsp::Position{ mappedEndLine, sym.endCharacter }
                        }
                    };
                }
            }


            return lsp::Location{
                lsp::DocumentUri::parse(sym.fileUri),
                lsp::Range{
                    lsp::Position{ sym.startLine, sym.startCharacter },
                    lsp::Position{ sym.endLine, sym.endCharacter }
                }
            };
        }
    }

    std::optional<std::vector<lsp::Location>> GetDefinition(const DefinitionRequest &request)
    {
        // 0. Include directive lookup: #include "..." or #include <...>
        if (request.resolveInclude)
        {
            const auto lineSpan = [&request]() -> std::pair<size_t, size_t>
            {
                size_t start = 0;
                for (uint32_t current = 0; current < request.position.line; ++current)
                {
                    const size_t nextBreak = request.sourceCode.find('\n', start);
                    if (nextBreak == std::string::npos)
                    {
                        return { std::string::npos, std::string::npos };
                    }
                    start = nextBreak + 1;
                }
                const size_t end = request.sourceCode.find('\n', start);
                return { start, end == std::string::npos ? request.sourceCode.size() : end };
            }();

            if (lineSpan.first != std::string::npos)
            {
                const std::string_view line(request.sourceCode.data() + lineSpan.first,
                                            lineSpan.second - lineSpan.first);
                const size_t hash = line.find_first_not_of(" \t");
                if (hash != std::string_view::npos && line[hash] == '#' &&
                    line.compare(hash + 1, 7, "include") == 0)
                {
                    size_t openDelim = line.find_first_of("\"<", hash + 8);
                    if (openDelim != std::string_view::npos)
                    {
                        char closeChar = line[openDelim] == '<' ? '>' : '"';
                        size_t closeDelim = line.find(closeChar, openDelim + 1);
                        if (closeDelim != std::string_view::npos)
                        {
                            const auto character = static_cast<size_t>(request.position.character);
                            if (character >= hash && character <= closeDelim)
                            {
                                std::string rawPath(line.substr(openDelim + 1, closeDelim - openDelim - 1));
                                std::string resolved = request.resolveInclude(rawPath);
                                if (!resolved.empty())
                                {
                                    lsp::DocumentUri targetUri = resolved.rfind("file://", 0) == 0
                                        ? lsp::DocumentUri::parse(resolved)
                                        : lsp::Uri::fileUriFromPath(resolved);
                                    return std::vector<lsp::Location>{
                                        lsp::Location{
                                            targetUri,
                                            lsp::Range{
                                                lsp::Position{ 0, 0 },
                                                lsp::Position{ 0, 0 }
                                            }
                                        }
                                    };
                                }
                            }
                        }
                    }
                }
            }
        }

        TSNode node{};
        std::string nodeText = GetNodeTextAt(request, node);
        if (nodeText.empty() || ts_node_is_null(node))
        {
            return std::nullopt;
        }

        std::vector<lsp::Location> locations;

        // Check if cursor is in a virtual mixin document
        bool isVirtualDoc = request.uri.starts_with("angelscript-virtual:");
        std::string virtualHostClass;
        std::string virtualMixinName;
        const analysis::Symbol *virtualMixinSym = nullptr;

        if (isVirtualDoc)
        {
            std::string_view s = request.uri;
            if (s.starts_with("angelscript-virtual://"))
            {
                s.remove_prefix(22);
            }
            else if (s.starts_with("angelscript-virtual:"))
            {
                s.remove_prefix(20);
            }

            auto slashPos = s.find('/');
            if (slashPos != std::string_view::npos)
            {
                virtualHostClass = utils::UrlDecode(s.substr(0, slashPos));
                std::string_view mixinPart = s.substr(slashPos + 1);
                if (mixinPart.ends_with(".as"))
                {
                    mixinPart.remove_suffix(3);
                }
                virtualMixinName = utils::UrlDecode(mixinPart);
            }
            else
            {
                std::string_view mixinPart = s;
                if (mixinPart.ends_with(".as"))
                {
                    mixinPart.remove_suffix(3);
                }
                virtualMixinName = utils::UrlDecode(mixinPart);
            }

            auto candidates = request.symbolTable.FindSymbols(virtualMixinName);
            for (const auto &cand : candidates)
            {
                if (cand.type == analysis::SymbolType::Class)
                {
                    virtualMixinSym = &cand;
                    break;
                }
            }
            if (!virtualMixinSym)
            {
                std::string shortName = virtualMixinName;
                auto lastScope = shortName.rfind("::");
                if (lastScope != std::string::npos)
                {
                    shortName = shortName.substr(lastScope + 2);
                }
                auto shortCandidates = request.symbolTable.FindTypeSymbolsByShortName(shortName);
                for (const auto &cand : shortCandidates)
                {
                    if (cand.type == analysis::SymbolType::Class)
                    {
                        virtualMixinSym = &cand;
                        break;
                    }
                }
            }
        }

        // 1. Member Access vs Local Scope Precedence
        auto rootScope = (isVirtualDoc && virtualMixinSym)
            ? request.scopeIndex.GetRoot(virtualMixinSym->fileUri)
            : request.scopeIndex.GetRoot(request.uri);

        uint32_t queryLine = (isVirtualDoc && virtualMixinSym && request.position.line >= 3)
            ? (virtualMixinSym->startLine + (request.position.line - 3))
            : request.position.line;

        TSNode parent = ts_node_parent(node);

        // Check if cursor node is the member child of a member_expression (e.g. "prop" in "obj.prop")
        bool isMemberChildOfExpression = false;
        if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "member_expression")
        {
            TSNode memNode = parser::GetChildByField(parent, parser::fields::Member);
            if (!ts_node_is_null(memNode) && (ts_node_eq(memNode, node) || ts_node_start_byte(memNode) == ts_node_start_byte(node)))
            {
                isMemberChildOfExpression = true;
            }
        }

        auto resolveMemberDefinition = [&]() -> std::optional<std::vector<lsp::Location>>
        {
            if (ts_node_is_null(parent) || std::string_view(ts_node_type(parent)) != "member_expression")
            {
                return std::nullopt;
            }

            TSNode objectNode = parser::GetChildByField(parent, parser::fields::Object);
            if (ts_node_is_null(objectNode))
            {
                return std::nullopt;
            }

            std::string receiverTypeName;
            if (rootScope)
            {
                const analysis::Scope *scope = FindInnermostScope(rootScope.get(), queryLine, request.position.character);
                receiverTypeName = analysis::ResolveExpressionType(objectNode, scope, request.symbolTable, request.sourceCode, request.uri);
            }
            else
            {
                receiverTypeName = analysis::ResolveExpressionType(objectNode, nullptr, request.symbolTable, request.sourceCode, request.uri);
            }

            if (!receiverTypeName.empty())
            {
                receiverTypeName = analysis::MemberOwnerType(receiverTypeName);
            }

            if (receiverTypeName.empty())
            {
                uint32_t objStart = ts_node_start_byte(objectNode);
                uint32_t objEnd = ts_node_end_byte(objectNode);
                if (objStart < request.sourceCode.size() && objEnd <= request.sourceCode.size() && objStart < objEnd)
                {
                    std::string objText = request.sourceCode.substr(objStart, objEnd - objStart);

                    if (objText == "this" || objText == "self")
                    {
                        if (isVirtualDoc && !virtualHostClass.empty())
                        {
                            receiverTypeName = virtualHostClass;
                        }
                        else
                        {
                            auto containers = analysis::GetEnclosingContainers(node, request.sourceCode);
                            for (const auto &c : containers)
                            {
                                if (c.kind == analysis::ContainerKind::Class || c.kind == analysis::ContainerKind::Interface)
                                {
                                    receiverTypeName = c.qualifiedName.empty() ? c.name : c.qualifiedName;
                                    break;
                                }
                            }
                        }
                    }
                    else if (objText == "BaseClass")
                    {
                        std::string host = (isVirtualDoc && !virtualHostClass.empty()) ? virtualHostClass : "";
                        if (!host.empty())
                        {
                            auto hier = analysis::GetInheritedTypeHierarchy(host, request.symbolTable);
                            for (size_t i = 1; i < hier.size(); ++i)
                            {
                                if (!analysis::IsMixinClass(hier[i], request.symbolTable))
                                {
                                    receiverTypeName = hier[i];
                                    break;
                                }
                            }
                            if (receiverTypeName.empty() && hier.size() > 1)
                            {
                                receiverTypeName = hier[1];
                            }
                        }
                        else
                        {
                            auto containers = analysis::GetEnclosingContainers(node, request.sourceCode);
                            for (const auto &c : containers)
                            {
                                if (c.kind == analysis::ContainerKind::Class || c.kind == analysis::ContainerKind::Interface)
                                {
                                    auto hier = analysis::GetInheritedTypeHierarchy(c.qualifiedName.empty() ? c.name : c.qualifiedName, request.symbolTable);
                                    for (size_t i = 1; i < hier.size(); ++i)
                                    {
                                        if (!analysis::IsMixinClass(hier[i], request.symbolTable))
                                        {
                                            receiverTypeName = hier[i];
                                            break;
                                        }
                                    }
                                    if (receiverTypeName.empty() && hier.size() > 1)
                                    {
                                        receiverTypeName = hier[1];
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    else if (rootScope)
                    {
                        const analysis::Scope *scope = FindInnermostScope(rootScope.get(), queryLine, request.position.character);
                        if (scope)
                        {
                            const analysis::LocalDefinition *objDef = analysis::ResolveInScope(scope, objText);
                            if (objDef && !objDef->typeName.empty())
                            {
                                receiverTypeName = analysis::CleanBaseType(objDef->typeName);
                            }
                        }
                    }

                    if (receiverTypeName.empty())
                    {
                        auto globSyms = request.symbolTable.FindSymbols(objText);
                        for (const auto &sym : globSyms)
                        {
                            if (sym.type == analysis::SymbolType::Variable)
                            {
                                const auto &var = sym.GetVariable();
                                if (!var.typeName.empty())
                                {
                                    receiverTypeName = analysis::CleanBaseType(var.typeName);
                                    break;
                                }
                            }
                            else if (sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Namespace)
                            {
                                receiverTypeName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                                break;
                            }
                        }
                    }

                    if (receiverTypeName.empty())
                    {
                        auto shortMatches = request.symbolTable.FindTypeSymbolsByShortName(objText);
                        for (const auto &sym : shortMatches)
                        {
                            if (sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Namespace)
                            {
                                receiverTypeName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                                break;
                            }
                        }
                    }
                }
            }

            if (!receiverTypeName.empty())
            {
                if (receiverTypeName.find("::") == std::string::npos && !request.symbolTable.HasSymbol(receiverTypeName))
                {
                    auto shortMatches = request.symbolTable.FindTypeSymbolsByShortName(receiverTypeName);
                    for (const auto &sym : shortMatches)
                    {
                        if (sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface)
                        {
                            receiverTypeName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                            break;
                        }
                    }
                }

                std::vector<analysis::Symbol> memberSymbols;
                auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverTypeName, request.symbolTable);
                for (const auto &typeName : hierarchy)
                {
                    std::string qualifiedMember = typeName + "::" + nodeText;
                    auto found = request.symbolTable.FindSymbols(qualifiedMember);
                    if (!found.empty())
                    {
                        memberSymbols = std::move(found);
                        break;
                    }
                }

                if (memberSymbols.empty())
                {
                    for (const auto &typeName : hierarchy)
                    {
                        auto accessors = analysis::FindPropertyAccessors(typeName, nodeText, request.symbolTable, false);
                        if (!accessors.empty())
                        {
                            memberSymbols = std::move(accessors);
                            break;
                        }
                    }
                }

                if (memberSymbols.empty())
                {
                    auto directMatches = request.symbolTable.FindSymbols(receiverTypeName + "::" + nodeText);
                    if (!directMatches.empty())
                    {
                        memberSymbols = std::move(directMatches);
                    }
                }

                const analysis::Scope *scope = nullptr;
                if (rootScope)
                {
                    scope = FindInnermostScope(rootScope.get(), queryLine, request.position.character);
                }
                memberSymbols = FilterOverloadsForCall(node, memberSymbols, request, scope);

                std::vector<lsp::Location> memLocations;
                for (const auto &sym : memberSymbols)
                {
                    if (sym.type != analysis::SymbolType::CallReference)
                    {
                        memLocations.push_back(MakeLocation(sym, request));
                    }
                }

                if (!memLocations.empty())
                {
                    return memLocations;
                }
            }

            return std::nullopt;
        };

        // If cursor is on the member child of obj.prop, member resolution takes precedence over local scope!
        if (isMemberChildOfExpression)
        {
            auto memberLocs = resolveMemberDefinition();
            if (memberLocs.has_value())
            {
                return memberLocs;
            }
        }

        // 2. Local Scope Definition (Parameters and Variables in Function Scope)
        if (rootScope)
        {
            const analysis::Scope *scope = FindInnermostScope(rootScope.get(), queryLine, request.position.character);
            if (scope)
            {
                const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, nodeText);
                if (def && (def->kind == analysis::LocalDefinitionKind::Parameter ||
                            def->kind == analysis::LocalDefinitionKind::Variable))
                {
                    const analysis::Scope *declaringScope = FindScopeDeclaringDefinition(rootScope.get(), *def);
                    bool isInsideFunction = false;
                    for (const analysis::Scope *s = declaringScope; s != nullptr; s = s->parent)
                    {
                        if (s->isFunctionScope)
                        {
                            isInsideFunction = true;
                            break;
                        }
                    }

                    if (isInsideFunction)
                    {
                        uint32_t sLine = (def->fullEndLine > 0 || def->fullEndCharacter > 0) ? def->fullStartLine : def->startLine;
                        uint32_t sChar = (def->fullEndLine > 0 || def->fullEndCharacter > 0) ? def->fullStartCharacter : def->startCharacter;
                        uint32_t eLine = (def->fullEndLine > 0 || def->fullEndCharacter > 0) ? def->fullEndLine : def->endLine;
                        uint32_t eChar = (def->fullEndLine > 0 || def->fullEndCharacter > 0) ? def->fullEndCharacter : def->endCharacter;

                        if (isVirtualDoc && virtualMixinSym && sLine >= virtualMixinSym->startLine)
                        {
                            sLine = 3 + (sLine - virtualMixinSym->startLine);
                            eLine = 3 + (eLine - virtualMixinSym->startLine);
                        }

                        locations.push_back(lsp::Location{
                            lsp::DocumentUri::parse(request.uri),
                            lsp::Range{
                                lsp::Position{ sLine, sChar },
                                lsp::Position{ eLine, eChar }
                            }
                        });
                        return locations;
                    }
                }
            }
        }

        // 3. Fallback Member Access Resolution if not already resolved
        if (!isMemberChildOfExpression)
        {
            auto memberLocs = resolveMemberDefinition();
            if (memberLocs.has_value())
            {
                return memberLocs;
            }
        }

        // 4. Container / Scoped / Global Symbol Lookup
        std::vector<analysis::Symbol> symbols;
        if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "scoped_identifier")
        {
            uint32_t pStart = ts_node_start_byte(parent);
            uint32_t pEnd = ts_node_end_byte(parent);
            if (pStart < request.sourceCode.size() && pEnd <= request.sourceCode.size())
            {
                std::string scopedText = request.sourceCode.substr(pStart, pEnd - pStart);
                symbols = analysis::FindSymbolsInScope(scopedText, node, request.sourceCode, request.symbolTable);
            }
        }
        if (symbols.empty())
        {
            symbols = analysis::FindSymbolsInScope(nodeText, node, request.sourceCode, request.symbolTable);
        }

        // Global property accessors fallback (e.g. g_Module -> get_g_Module)
        if (symbols.empty())
        {
            symbols = analysis::FindGlobalPropertyAccessors(nodeText, request.symbolTable, false);
        }

        // Fallback for virtual documents: lookup in host class hierarchy
        if (symbols.empty() && isVirtualDoc && !virtualHostClass.empty())
        {
            auto hier = analysis::GetInheritedTypeHierarchy(virtualHostClass, request.symbolTable);
            for (const auto &cls : hier)
            {
                auto found = request.symbolTable.FindSymbols(cls + "::" + nodeText);
                if (!found.empty())
                {
                    symbols = std::move(found);
                    break;
                }
            }
            if (symbols.empty())
            {
                for (const auto &cls : hier)
                {
                    auto accessors = analysis::FindPropertyAccessors(cls, nodeText, request.symbolTable, false);
                    if (!accessors.empty())
                    {
                        symbols = std::move(accessors);
                        break;
                    }
                }
            }
        }

        // Fallback to local scope definition (e.g. Field or non-function scope definition) if not in SymbolTable
        if (symbols.empty() && rootScope)
        {
            const analysis::Scope *scope = FindInnermostScope(rootScope.get(), queryLine, request.position.character);
            if (scope)
            {
                const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, nodeText);
                if (def)
                {
                    uint32_t sLine = (def->fullEndLine > 0 || def->fullEndCharacter > 0) ? def->fullStartLine : def->startLine;
                    uint32_t sChar = (def->fullEndLine > 0 || def->fullEndCharacter > 0) ? def->fullStartCharacter : def->startCharacter;
                    uint32_t eLine = (def->fullEndLine > 0 || def->fullEndCharacter > 0) ? def->fullEndLine : def->endLine;
                    uint32_t eChar = (def->fullEndLine > 0 || def->fullEndCharacter > 0) ? def->fullEndCharacter : def->endCharacter;

                    if (isVirtualDoc && virtualMixinSym && sLine >= virtualMixinSym->startLine)
                    {
                        sLine = 3 + (sLine - virtualMixinSym->startLine);
                        eLine = 3 + (eLine - virtualMixinSym->startLine);
                    }

                    locations.push_back(lsp::Location{
                        lsp::DocumentUri::parse(request.uri),
                        lsp::Range{
                            lsp::Position{ sLine, sChar },
                            lsp::Position{ eLine, eChar }
                        }
                    });
                    return locations;
                }
            }
        }

        if (nodeText == "this" || nodeText == "self")
        {
            std::string targetClass;
            if (isVirtualDoc && !virtualHostClass.empty())
            {
                targetClass = virtualHostClass;
            }
            else
            {
                auto containers = analysis::GetEnclosingContainers(node, request.sourceCode);
                for (const auto &c : containers)
                {
                    if (c.kind == analysis::ContainerKind::Class || c.kind == analysis::ContainerKind::Interface)
                    {
                        targetClass = c.qualifiedName.empty() ? c.name : c.qualifiedName;
                        break;
                    }
                }
            }

            if (!targetClass.empty())
            {
                auto cands = request.symbolTable.FindSymbols(targetClass);
                for (const auto &c : cands)
                {
                    if (c.type == analysis::SymbolType::Class || c.type == analysis::SymbolType::Interface)
                    {
                        locations.push_back(MakeLocation(c, request));
                    }
                }
                if (!locations.empty())
                {
                    return locations;
                }
            }
        }

        const analysis::Scope *scope = nullptr;
        if (rootScope)
        {
            scope = FindInnermostScope(rootScope.get(), queryLine, request.position.character);
        }
        symbols = FilterOverloadsForCall(node, symbols, request, scope);

        for (const auto &sym : symbols)
        {
            if (sym.type != analysis::SymbolType::CallReference)
            {
                locations.push_back(MakeLocation(sym, request));
            }
        }

        if (!locations.empty())
        {
            return locations;
        }

        return std::nullopt;
    }

    std::optional<std::vector<lsp::Location>> GetTypeDefinition(const DefinitionRequest &request)
    {
        TSNode node{};
        std::string nodeText = GetNodeTextAt(request, node);
        if (nodeText.empty() || ts_node_is_null(node))
        {
            return std::nullopt;
        }

        std::string typeNameToFind;

        // 1. Check local scope
        auto rootScope = request.scopeIndex.GetRoot(request.uri);
        if (rootScope)
        {
            const analysis::Scope *scope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
            if (scope)
            {
                const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, nodeText);
                if (def && !def->typeName.empty())
                {
                    typeNameToFind = analysis::CleanBaseType(def->typeName);
                }
            }
        }

        // 2. Check global symbols
        if (typeNameToFind.empty())
        {
            auto symbols = request.symbolTable.FindSymbols(nodeText);
            for (const auto &sym : symbols)
            {
                if (sym.type == analysis::SymbolType::Variable)
                {
                    const auto &var = sym.GetVariable();
                    if (!var.typeName.empty())
                    {
                        typeNameToFind = analysis::CleanBaseType(var.typeName);
                        break;
                    }
                }
                else if (sym.type == analysis::SymbolType::Function)
                {
                    const auto &fn = sym.GetFunction();
                    if (!fn.returnType.empty())
                    {
                        typeNameToFind = analysis::CleanBaseType(fn.returnType);
                        break;
                    }
                }
                else if (sym.type == analysis::SymbolType::Class ||
                         sym.type == analysis::SymbolType::Interface ||
                         sym.type == analysis::SymbolType::Enum ||
                         sym.type == analysis::SymbolType::Typedef ||
                         sym.type == analysis::SymbolType::Funcdef)
                {
                    typeNameToFind = sym.name;
                    break;
                }
            }
        }

        // 3. Fallback: maybe nodeText itself is a type name (e.g. Player in Player@ p)
        if (typeNameToFind.empty())
        {
            typeNameToFind = analysis::CleanBaseType(nodeText);
        }

        if (typeNameToFind.empty() || analysis::IsPrimitiveTypeName(typeNameToFind))
        {
            return std::nullopt;
        }

        std::vector<lsp::Location> locations;
        auto typeSymbols = request.symbolTable.FindSymbols(typeNameToFind);

        for (const auto &sym : typeSymbols)
        {
            if (sym.type == analysis::SymbolType::Class ||
                sym.type == analysis::SymbolType::Interface ||
                sym.type == analysis::SymbolType::Enum ||
                sym.type == analysis::SymbolType::Typedef ||
                sym.type == analysis::SymbolType::Funcdef)
            {
                locations.push_back(MakeLocation(sym, request));
            }
        }

        if (!locations.empty())
        {
            return locations;
        }

        return std::nullopt;
    }
}
