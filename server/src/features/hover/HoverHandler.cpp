#include "features/hover/HoverHandler.h"
#include "analysis/SemanticHelpers.h"
#include "utils/Utils.h"
#include <sstream>
#include <vector>
#include <algorithm>
#include <unordered_set>

namespace angel_lsp::features
{
    namespace
    {
        bool IsInsideScope(const analysis::Scope &scope, uint32_t line, uint32_t character)
        {
            if (line < scope.startLine || line > scope.endLine)
            {
                return false;
            }
            if (line == scope.startLine && character < scope.startCharacter)
            {
                return false;
            }
            if (line == scope.endLine && character > scope.endCharacter)
            {
                return false;
            }
            return true;
        }

        const analysis::Scope *FindInnermostScope(const analysis::Scope *root, uint32_t line, uint32_t character)
        {
            if (!root || !IsInsideScope(*root, line, character))
            {
                return nullptr;
            }

            const analysis::Scope *current = root;
            bool foundChild = true;
            while (foundChild)
            {
                foundChild = false;
                for (const auto &child : current->children)
                {
                    if (child && IsInsideScope(*child, line, character))
                    {
                        current = child.get();
                        foundChild = true;
                        break;
                    }
                }
            }
            return current;
        }

        std::string CleanBaseType(std::string typeName)
        {
            while (!typeName.empty() && (typeName.back() == '@' || typeName.back() == '&' || typeName.back() == ' '))
            {
                typeName.pop_back();
            }
            if (typeName.starts_with("const "))
            {
                typeName = typeName.substr(6);
            }
            while (!typeName.empty() && typeName.back() == ' ')
            {
                typeName.pop_back();
            }
            return typeName;
        }

        /**
         * @brief Recursively collects the class and interface inheritance hierarchy for a type.
         * @param symbolTable The symbol table to look up class and interface definitions.
         * @param initialTypeName The starting type name.
         * @return Vector of type names in the hierarchy including initialTypeName and its transitive bases.
         */
        std::vector<std::string> GetInheritedTypeHierarchy(const analysis::SymbolTable &symbolTable, const std::string &initialTypeName)
        {
            std::vector<std::string> hierarchy;
            std::unordered_set<std::string> visited;
            std::vector<std::string> queue;

            std::string rootType = CleanBaseType(initialTypeName);
            if (rootType.empty())
            {
                return hierarchy;
            }

            visited.insert(rootType);
            queue.push_back(rootType);

            size_t head = 0;
            while (head < queue.size())
            {
                std::string curType = queue[head++];
                hierarchy.push_back(curType);

                auto symbols = symbolTable.FindSymbols(curType);
                for (const auto &sym : symbols)
                {
                    if (sym.type == analysis::SymbolType::Class)
                    {
                        const auto &cls = sym.GetClass();
                        for (const auto &base : cls.bases)
                        {
                            std::string cleanBase = CleanBaseType(base);
                            if (!cleanBase.empty() && visited.insert(cleanBase).second)
                            {
                                queue.push_back(cleanBase);
                            }
                        }
                    }
                    else if (sym.type == analysis::SymbolType::Interface)
                    {
                        const auto &iface = sym.GetInterface();
                        for (const auto &base : iface.inheritedInterfaces)
                        {
                            std::string cleanBase = CleanBaseType(base);
                            if (!cleanBase.empty() && visited.insert(cleanBase).second)
                            {
                                queue.push_back(cleanBase);
                            }
                        }
                    }
                }
            }

            return hierarchy;
        }

        std::vector<std::string> SplitLines(const std::string &str)
        {
            std::vector<std::string> lines;
            std::string line;
            std::istringstream stream(str);
            while (std::getline(stream, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                lines.push_back(line);
            }
            return lines;
        }

        std::string Trim(const std::string &str)
        {
            size_t first = str.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                return "";
            }
            size_t last = str.find_last_not_of(" \t\r\n");
            return str.substr(first, (last - first + 1));
        }

        std::string FormatFunctionSignature(const analysis::Symbol &sym)
        {
            if (sym.type != analysis::SymbolType::Function && sym.type != analysis::SymbolType::Funcdef)
            {
                return "";
            }

            std::ostringstream oss;
            std::string returnType;
            const std::vector<analysis::ParameterInformation> *parameters = nullptr;

            if (sym.type == analysis::SymbolType::Funcdef)
            {
                oss << "funcdef ";
                const auto &sig = sym.GetFuncdef();
                returnType = sig.returnType;
                parameters = &sig.parameters;
            }
            else
            {
                const auto &sig = sym.GetFunction();
                returnType = sig.returnType;
                parameters = &sig.parameters;
            }

            if (!returnType.empty())
            {
                oss << returnType << " ";
            }
            else
            {
                oss << "void ";
            }

            if (!sym.containerName.empty())
            {
                oss << sym.containerName << "::";
            }
            oss << sym.name << "(";

            if (parameters)
            {
                for (size_t i = 0; i < parameters->size(); ++i)
                {
                    if (i > 0)
                    {
                        oss << ", ";
                    }
                    const auto &param = (*parameters)[i];
                    if (!param.typeName.empty())
                    {
                        oss << param.typeName;
                    }
                    if (!param.name.empty())
                    {
                        oss << " " << param.name;
                    }
                    if (!param.defaultValue.empty())
                    {
                        oss << " = " << param.defaultValue;
                    }
                }
            }

            oss << ")";
            return oss.str();
        }

        std::string FormatClassSignature(const analysis::Symbol &sym)
        {
            if (sym.type != analysis::SymbolType::Class && sym.type != analysis::SymbolType::Interface)
            {
                return "";
            }

            std::ostringstream oss;
            if (sym.type == analysis::SymbolType::Class)
            {
                const auto &sig = sym.GetClass();
                if (sig.modifiers.isAbstract)
                {
                    oss << "abstract ";
                }
                if (sig.modifiers.isFinal)
                {
                    oss << "final ";
                }
                if (sig.modifiers.isShared)
                {
                    oss << "shared ";
                }
                oss << "class " << sym.name;
                if (!sig.bases.empty())
                {
                    oss << " : ";
                    for (size_t i = 0; i < sig.bases.size(); ++i)
                    {
                        if (i > 0)
                        {
                            oss << ", ";
                        }
                        oss << sig.bases[i];
                    }
                }
            }
            else
            {
                const auto &sig = sym.GetInterface();
                oss << "interface " << sym.name;
                if (!sig.inheritedInterfaces.empty())
                {
                    oss << " : ";
                    for (size_t i = 0; i < sig.inheritedInterfaces.size(); ++i)
                    {
                        if (i > 0)
                        {
                            oss << ", ";
                        }
                        oss << sig.inheritedInterfaces[i];
                    }
                }
            }
            return oss.str();
        }
    }

    std::string ExtractDocComment(const std::string &sourceCode, uint32_t declStartLine)
    {
        if (declStartLine == 0 || sourceCode.empty())
        {
            return "";
        }

        auto lines = SplitLines(sourceCode);
        if (declStartLine > lines.size())
        {
            return "";
        }

        std::vector<std::string> commentLines;
        int currentLine = static_cast<int>(declStartLine) - 1;

        while (currentLine >= 0 && Trim(lines[currentLine]).empty())
        {
            currentLine--;
        }

        if (currentLine < 0)
        {
            return "";
        }

        std::string trimmed = Trim(lines[currentLine]);
        if (trimmed.ends_with("*/"))
        {
            // Block comment: scan upwards to find /* or /**
            while (currentLine >= 0)
            {
                std::string l = lines[currentLine];
                commentLines.push_back(l);
                if (l.find("/*") != std::string::npos)
                {
                    break;
                }
                currentLine--;
            }
            std::reverse(commentLines.begin(), commentLines.end());
        }
        else if (trimmed.starts_with("//"))
        {
            // Line comments: scan upwards as long as lines start with //
            while (currentLine >= 0)
            {
                std::string l = Trim(lines[currentLine]);
                if (l.starts_with("//"))
                {
                    commentLines.push_back(l);
                    currentLine--;
                }
                else
                {
                    break;
                }
            }
            std::reverse(commentLines.begin(), commentLines.end());
        }
        else
        {
            return "";
        }

        // Clean comment lines
        std::vector<std::string> cleanLines;
        for (const auto &raw : commentLines)
        {
            std::string l = Trim(raw);
            if (l.starts_with("/**"))
            {
                l = l.substr(3);
            }
            else if (l.starts_with("/*"))
            {
                l = l.substr(2);
            }
            else if (l.starts_with("///"))
            {
                l = l.substr(3);
            }
            else if (l.starts_with("//"))
            {
                l = l.substr(2);
            }

            if (l.ends_with("*/"))
            {
                l = l.substr(0, l.size() - 2);
            }

            l = Trim(l);
            if (l.starts_with("*"))
            {
                l = Trim(l.substr(1));
            }

            if (!l.empty() || !cleanLines.empty())
            {
                cleanLines.push_back(l);
            }
        }

        while (!cleanLines.empty() && cleanLines.back().empty())
        {
            cleanLines.pop_back();
        }

        if (cleanLines.empty())
        {
            return "";
        }

        // Parse Doxygen tags
        std::ostringstream out;
        std::vector<std::string> mainText;
        std::vector<std::pair<std::string, std::string>> params;
        std::vector<std::string> returns;
        std::vector<std::string> notes;
        std::vector<std::string> warnings;
        std::vector<std::string> sees;

        for (const auto &line : cleanLines)
        {
            if (line.starts_with("@brief ") || line.starts_with("\\brief "))
            {
                mainText.push_back(line.substr(7));
            }
            else if (line.starts_with("@param ") || line.starts_with("\\param "))
            {
                std::string rem = Trim(line.substr(7));
                size_t spacePos = rem.find_first_of(" \t");
                if (spacePos != std::string::npos)
                {
                    std::string paramName = rem.substr(0, spacePos);
                    std::string paramDesc = Trim(rem.substr(spacePos + 1));
                    params.emplace_back(paramName, paramDesc);
                }
                else
                {
                    params.emplace_back(rem, "");
                }
            }
            else if (line.starts_with("@tparam ") || line.starts_with("\\tparam "))
            {
                std::string rem = Trim(line.substr(8));
                size_t spacePos = rem.find_first_of(" \t");
                if (spacePos != std::string::npos)
                {
                    std::string paramName = rem.substr(0, spacePos);
                    std::string paramDesc = Trim(rem.substr(spacePos + 1));
                    params.emplace_back("<" + paramName + ">", paramDesc);
                }
                else
                {
                    params.emplace_back("<" + rem + ">", "");
                }
            }
            else if (line.starts_with("@return ") || line.starts_with("\\return ") ||
                     line.starts_with("@returns ") || line.starts_with("\\returns "))
            {
                size_t tagLen = (line.starts_with("@returns ") || line.starts_with("\\returns ")) ? 9 : 8;
                returns.push_back(Trim(line.substr(tagLen)));
            }
            else if (line.starts_with("@note ") || line.starts_with("\\note "))
            {
                notes.push_back(Trim(line.substr(6)));
            }
            else if (line.starts_with("@warning ") || line.starts_with("\\warning "))
            {
                warnings.push_back(Trim(line.substr(9)));
            }
            else if (line.starts_with("@see ") || line.starts_with("\\see "))
            {
                sees.push_back(Trim(line.substr(5)));
            }
            else
            {
                mainText.push_back(line);
            }
        }

        bool hasContent = false;
        for (const auto &m : mainText)
        {
            if (hasContent)
            {
                out << "\n";
            }
            out << m;
            hasContent = true;
        }

        if (!params.empty())
        {
            if (hasContent)
            {
                out << "\n\n";
            }
            out << "**Parameters:**\n";
            for (const auto &p : params)
            {
                out << "- `" << p.first << "`" << (p.second.empty() ? "" : ": " + p.second) << "\n";
            }
            hasContent = true;
        }

        if (!returns.empty())
        {
            if (hasContent)
            {
                out << "\n\n";
            }
            out << "**Returns:**\n";
            for (const auto &r : returns)
            {
                out << "- " << r << "\n";
            }
            hasContent = true;
        }

        if (!notes.empty())
        {
            if (hasContent)
            {
                out << "\n\n";
            }
            for (const auto &n : notes)
            {
                out << "> **Note:** " << n << "\n";
            }
            hasContent = true;
        }

        if (!warnings.empty())
        {
            if (hasContent)
            {
                out << "\n\n";
            }
            for (const auto &w : warnings)
            {
                out << "> **Warning:** " << w << "\n";
            }
            hasContent = true;
        }

        if (!sees.empty())
        {
            if (hasContent)
            {
                out << "\n\n";
            }
            out << "**See also:** ";
            for (size_t i = 0; i < sees.size(); ++i)
            {
                if (i > 0)
                {
                    out << ", ";
                }
                out << sees[i];
            }
            out << "\n";
        }

        return Trim(out.str());
    }

    namespace
    {
        bool ExtractHoverNode(TSNode rootNode, const std::string &sourceCode, uint32_t line, uint32_t character,
                              TSNode &outNode, std::string &outText, lsp::Range &outRange)
        {
            TSPoint point = { line, character };
            TSNode node = ts_node_descendant_for_point_range(rootNode, point, point);
            if (ts_node_is_null(node))
            {
                return false;
            }

            auto tryExtract = [&](TSNode n) -> bool
            {
                if (ts_node_is_null(n)) return false;
                uint32_t sb = ts_node_start_byte(n);
                uint32_t eb = ts_node_end_byte(n);
                if (sb >= sourceCode.size() || eb > sourceCode.size() || sb >= eb) return false;

                std::string txt = sourceCode.substr(sb, eb - sb);
                std::string_view type = ts_node_type(n);

                if (type == "identifier" || type == "scoped_identifier" || type == "primitive_type" ||
                    analysis::IsPrimitiveTypeName(txt) || analysis::IsReservedKeyword(txt))
                {
                    outNode = n;
                    outText = txt;
                    TSPoint sp = ts_node_start_point(n);
                    TSPoint ep = ts_node_end_point(n);
                    outRange = lsp::Range{ { sp.row, sp.column }, { ep.row, ep.column } };
                    return true;
                }

                // Check if text is a word/identifier
                if (!txt.empty() && (isalpha(static_cast<unsigned char>(txt[0])) || txt[0] == '_'))
                {
                    bool allWord = true;
                    for (char c : txt)
                    {
                        if (!isalnum(static_cast<unsigned char>(c)) && c != '_')
                        {
                            allWord = false;
                            break;
                        }
                    }
                    if (allWord)
                    {
                        outNode = n;
                        outText = txt;
                        TSPoint sp = ts_node_start_point(n);
                        TSPoint ep = ts_node_end_point(n);
                        outRange = lsp::Range{ { sp.row, sp.column }, { ep.row, ep.column } };
                        return true;
                    }
                }
                return false;
            };

            if (tryExtract(node)) return true;

            // Try parent
            TSNode parent = ts_node_parent(node);
            if (tryExtract(parent)) return true;

            // Try prev character if at end of word
            if (character > 0)
            {
                TSPoint prevPt = { line, character - 1 };
                TSNode prevNode = ts_node_descendant_for_point_range(rootNode, prevPt, prevPt);
                if (tryExtract(prevNode)) return true;
                if (!ts_node_is_null(prevNode) && tryExtract(ts_node_parent(prevNode))) return true;
            }

            return false;
        }
    }

    std::optional<lsp::Hover> GetHover(const HoverRequest &request)
    {
        if (!request.tree || request.sourceCode.empty())
        {
            return std::nullopt;
        }

        TSNode rootNode = ts_tree_root_node(request.tree);
        TSNode node{};
        std::string nodeText;
        lsp::Range range{};

        if (!ExtractHoverNode(rootNode, request.sourceCode, request.position.line, request.position.character, node, nodeText, range))
        {
            return std::nullopt;
        }

        // 1. Primitive type check
        if (analysis::IsPrimitiveTypeName(nodeText))
        {
            std::string md = "```angelscript\n(primitive type) " + nodeText + "\n```";
            return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), md }, range };
        }

        auto rootScope = request.scopeIndex.GetRoot(request.uri);
        TSNode parent = ts_node_parent(node);

        // Check if cursor node is the member child of a member_expression (e.g. "prop" in "obj.prop")
        bool isMemberChildOfExpression = false;
        if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "member_expression")
        {
            TSNode memNode = ts_node_child_by_field_name(parent, "member", 6);
            if (!ts_node_is_null(memNode) && (ts_node_eq(memNode, node) || ts_node_start_byte(memNode) == ts_node_start_byte(node)))
            {
                isMemberChildOfExpression = true;
            }
        }

        auto resolveMemberAccess = [&]() -> std::optional<lsp::Hover>
        {
            if (ts_node_is_null(parent) || std::string_view(ts_node_type(parent)) != "member_expression")
            {
                return std::nullopt;
            }

            TSNode objectNode = ts_node_child_by_field_name(parent, "object", 6);
            if (ts_node_is_null(objectNode))
            {
                return std::nullopt;
            }

            uint32_t objStart = ts_node_start_byte(objectNode);
            uint32_t objEnd = ts_node_end_byte(objectNode);
            if (objStart >= request.sourceCode.size() || objEnd > request.sourceCode.size() || objStart >= objEnd)
            {
                return std::nullopt;
            }

            std::string objText = request.sourceCode.substr(objStart, objEnd - objStart);
            std::string receiverTypeName;

            if (objText == "this")
            {
                // Look for enclosing class from symbols
                request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                    {
                        if (sym.type == analysis::SymbolType::Class && sym.fileUri == request.uri)
                        {
                            if (request.position.line >= sym.startLine && request.position.line <= sym.endLine)
                            {
                                receiverTypeName = sym.name;
                            }
                        }
                    }
                });
            }
            else if (rootScope)
            {
                const analysis::Scope *scope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
                if (scope)
                {
                    const analysis::LocalDefinition *objDef = analysis::ResolveInScope(scope, objText);
                    if (objDef && !objDef->typeName.empty())
                    {
                        receiverTypeName = CleanBaseType(objDef->typeName);
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
                            receiverTypeName = CleanBaseType(var.typeName);
                            break;
                        }
                    }
                }
            }

            if (!receiverTypeName.empty())
            {
                std::vector<analysis::Symbol> memberSymbols;
                auto hierarchy = GetInheritedTypeHierarchy(request.symbolTable, receiverTypeName);
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

                if (!memberSymbols.empty())
                {
                    std::ostringstream oss;
                    oss << "```angelscript\n";
                    for (size_t i = 0; i < memberSymbols.size(); ++i)
                    {
                        if (i > 0)
                        {
                            oss << "\n";
                        }
                        const auto &sym = memberSymbols[i];
                        if (sym.type == analysis::SymbolType::Function)
                        {
                            oss << FormatFunctionSignature(sym);
                        }
                        else if (sym.type == analysis::SymbolType::Variable)
                        {
                            const auto &var = sym.GetVariable();
                            oss << "(property) " << (var.typeName.empty() ? "auto" : var.typeName) << " " << sym.name;
                        }
                        else if (sym.type == analysis::SymbolType::Property)
                        {
                            oss << "(property) " << sym.name;
                        }
                    }
                    oss << "\n```";

                    std::string doc = ExtractDocComment(request.sourceCode, memberSymbols[0].startLine);
                    if (!doc.empty())
                    {
                        oss << "\n\n" << doc;
                    }

                    return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str() }, range };
                }
            }

            return std::nullopt;
        };

        // If cursor is on the member child of obj.prop, member access resolution takes precedence over local scope!
        if (isMemberChildOfExpression)
        {
            auto memberHover = resolveMemberAccess();
            if (memberHover.has_value())
            {
                return memberHover;
            }
        }

        // 2. Local Scope Resolution (Variables and Parameters)
        if (rootScope)
        {
            const analysis::Scope *scope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
            if (scope)
            {
                const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, nodeText);
                if (def && (def->kind == analysis::LocalDefinitionKind::Parameter ||
                            def->kind == analysis::LocalDefinitionKind::Variable ||
                            def->kind == analysis::LocalDefinitionKind::Field))
                {
                    std::string typeName = def->typeName;
                    if (typeName.empty())
                    {
                        TSNode cur = node;
                        for (int i = 0; i < 4 && !ts_node_is_null(cur); ++i, cur = ts_node_parent(cur))
                        {
                            std::string_view cType = ts_node_type(cur);
                            if (cType == "parameter" || cType == "variable_declaration")
                            {
                                TSNode typeNode = ts_node_child_by_field_name(cur, "param_type", 10);
                                if (ts_node_is_null(typeNode))
                                {
                                    typeNode = ts_node_child_by_field_name(cur, "var_type", 8);
                                }
                                if (ts_node_is_null(typeNode))
                                {
                                    typeNode = ts_node_child_by_field_name(cur, "type", 4);
                                }
                                if (!ts_node_is_null(typeNode))
                                {
                                    uint32_t tStart = ts_node_start_byte(typeNode);
                                    uint32_t tEnd = ts_node_end_byte(typeNode);
                                    if (tStart < request.sourceCode.size() && tEnd <= request.sourceCode.size() && tStart < tEnd)
                                    {
                                        typeName = request.sourceCode.substr(tStart, tEnd - tStart);
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    std::ostringstream oss;
                    oss << "```angelscript\n";
                    switch (def->kind)
                    {
                    case analysis::LocalDefinitionKind::Parameter:
                        oss << "(parameter) ";
                        if (!typeName.empty())
                        {
                            oss << typeName << " ";
                        }
                        oss << def->name;
                        break;
                    case analysis::LocalDefinitionKind::Variable:
                        oss << "(local variable) ";
                        if (!typeName.empty())
                        {
                            oss << typeName << " ";
                        }
                        oss << def->name;
                        break;
                    case analysis::LocalDefinitionKind::Field:
                        oss << "(field) ";
                        if (!typeName.empty())
                        {
                            oss << typeName << " ";
                        }
                        oss << def->name;
                        break;
                    default:
                        oss << def->name;
                        break;
                    }
                    oss << "\n```";

                    std::string doc = ExtractDocComment(request.sourceCode, def->startLine);
                    if (!doc.empty())
                    {
                        oss << "\n\n" << doc;
                    }

                    return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str() }, range };
                }
            }
        }

        // 3. Fallback Member Access Resolution if not already resolved
        if (!isMemberChildOfExpression)
        {
            auto memberHover = resolveMemberAccess();
            if (memberHover.has_value())
            {
                return memberHover;
            }
        }

        // 4. Global / Scoped Symbol Lookup
        auto symbols = request.symbolTable.FindSymbols(nodeText);
        if (symbols.empty())
        {
            // Try resolving if inside a scoped identifier
            if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "scoped_identifier")
            {
                uint32_t pStart = ts_node_start_byte(parent);
                uint32_t pEnd = ts_node_end_byte(parent);
                if (pStart < request.sourceCode.size() && pEnd <= request.sourceCode.size())
                {
                    std::string scopedText = request.sourceCode.substr(pStart, pEnd - pStart);
                    symbols = request.symbolTable.FindSymbols(scopedText);
                    if (!symbols.empty())
                    {
                        TSPoint pStartPt = ts_node_start_point(parent);
                        TSPoint pEndPt = ts_node_end_point(parent);
                        range = lsp::Range{ { pStartPt.row, pStartPt.column }, { pEndPt.row, pEndPt.column } };
                    }
                }
            }
        }

        if (symbols.empty())
        {
            return std::nullopt;
        }

        std::ostringstream oss;
        oss << "```angelscript\n";

        for (size_t i = 0; i < symbols.size(); ++i)
        {
            if (i > 0)
            {
                oss << "\n";
            }
            const auto &sym = symbols[i];
            switch (sym.type)
            {
            case analysis::SymbolType::Function:
            case analysis::SymbolType::Funcdef:
                oss << FormatFunctionSignature(sym);
                break;
            case analysis::SymbolType::Class:
            case analysis::SymbolType::Interface:
                oss << FormatClassSignature(sym);
                break;
            case analysis::SymbolType::Enum:
                oss << "enum " << sym.name;
                break;
            case analysis::SymbolType::Variable:
                {
                    const auto &var = sym.GetVariable();
                    oss << "(global variable) " << (var.typeName.empty() ? "auto" : var.typeName) << " " << sym.name;
                }
                break;
            case analysis::SymbolType::Typedef:
                {
                    const auto &td = sym.GetTypedef();
                    oss << "typedef " << td.baseType << " " << sym.name;
                }
                break;
            case analysis::SymbolType::Namespace:
                oss << "namespace " << sym.name;
                break;
            case analysis::SymbolType::Property:
                oss << "(property) " << sym.name;
                break;
            default:
                oss << sym.name;
                break;
            }
        }
        oss << "\n```";

        std::string doc = ExtractDocComment(request.sourceCode, symbols[0].startLine);
        if (!doc.empty())
        {
            oss << "\n\n" << doc;
        }

        return lsp::Hover{ lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str() }, range };
    }
}
