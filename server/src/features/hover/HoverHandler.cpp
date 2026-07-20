#include "HoverHandler.h"
#include "features/hover/HoverInfo.h"
#include "utils/DoxygenParser.h"
#include "analysis/SymbolResolver.h"
#include <angelscript.h>
#include <set>
#include <sstream>

namespace angel_lsp
{
    namespace features
    {
        static std::string BuildFullNamespace(const analysis::Symbol* nsSym) {
            std::string ns = nsSym->name;
            const analysis::Symbol* curr = nsSym->parent;
            while (curr && curr->kind == analysis::SymbolKind::Namespace) {
                ns = curr->name + "::" + ns;
                curr = curr->parent;
            }
            return ns;
        }

        static std::string BuildScopeContext(const analysis::Symbol* sym) {
            if (!sym || !sym->parent) return "";
            
            if (sym->parent->kind == analysis::SymbolKind::Class || 
                sym->parent->kind == analysis::SymbolKind::Interface || 
                sym->parent->kind == analysis::SymbolKind::Mixin) {
                std::string scope = sym->parent->name;
                if (!sym->parent->templateParam.empty()) {
                    scope += "<" + sym->parent->templateParam + ">";
                }
                if (sym->parent->parent && sym->parent->parent->kind == analysis::SymbolKind::Namespace) {
                    scope = BuildFullNamespace(sym->parent->parent) + "::" + scope;
                }
                return scope;
            }
            
            if (sym->parent->kind == analysis::SymbolKind::Namespace) {
                return BuildFullNamespace(sym->parent);
            }
            
            return "";
        }
        
        static std::string BuildSignatureHelper(const analysis::Symbol *renderSym, const analysis::Symbol *originalSym, const std::string& dynamicDisplayName, const std::string& templateSubstitution)
        {
            bool isVarLike = renderSym->kind == analysis::SymbolKind::Parameter ||
                             renderSym->kind == analysis::SymbolKind::Variable ||
                             renderSym->kind == analysis::SymbolKind::Property;
            std::string dispName = (!isVarLike && !dynamicDisplayName.empty() && originalSym && renderSym->name == originalSym->name)
                                       ? dynamicDisplayName
                                       : renderSym->name;

            bool needsParentClass = false;
            if (originalSym && originalSym->parent && (originalSym->parent->kind == analysis::SymbolKind::Class || originalSym->parent->kind == analysis::SymbolKind::Interface || originalSym->parent->kind == analysis::SymbolKind::Mixin || originalSym->parent->kind == analysis::SymbolKind::Enum) &&
                (originalSym->kind == analysis::SymbolKind::Method || originalSym->kind == analysis::SymbolKind::Constructor || originalSym->kind == analysis::SymbolKind::Destructor || originalSym->kind == analysis::SymbolKind::Property || originalSym->kind == analysis::SymbolKind::EnumMember))
            {
                needsParentClass = true;
            }
            std::string sig = renderSym->BuildSignature(needsParentClass, dispName);

            if (renderSym->kind == analysis::SymbolKind::Parameter && renderSym->parent)
            {
                bool parentNeedsClass = false;
                if (renderSym->parent->parent && (renderSym->parent->parent->kind == analysis::SymbolKind::Class || renderSym->parent->parent->kind == analysis::SymbolKind::Interface || renderSym->parent->parent->kind == analysis::SymbolKind::Mixin) &&
                    (renderSym->parent->kind == analysis::SymbolKind::Method || renderSym->parent->kind == analysis::SymbolKind::Constructor || renderSym->parent->kind == analysis::SymbolKind::Destructor))
                {
                    parentNeedsClass = true;
                }
                sig = renderSym->parent->BuildSignature(parentNeedsClass, renderSym->parent->name);
            }

            if (renderSym->parent && renderSym->parent->templateParam.length() > 0)
            {
                std::string typeToSub = templateSubstitution;
                if (typeToSub.empty() && originalSym && !originalSym->templateType.empty())
                    typeToSub = originalSym->templateType;

                if (!typeToSub.empty())
                {
                    std::string paramName = renderSym->parent->templateParam;
                    size_t pos = sig.find(paramName);
                    while (pos != std::string::npos)
                    {
                        bool match = true;
                        if (pos > 0 && isalnum(sig[pos - 1]))
                            match = false;
                        if (pos + paramName.length() < sig.length() && isalnum(sig[pos + paramName.length()]))
                            match = false;

                        if (match)
                        {
                            sig.replace(pos, paramName.length(), typeToSub);
                            pos += typeToSub.length();
                        }
                        else
                        {
                            pos += paramName.length();
                        }
                        pos = sig.find(paramName, pos);
                    }
                }
            }
            return sig;
        }

        static HoverInfo BuildHoverInfo(
            const analysis::Symbol* sym,
            const analysis::Symbol* originalSym,
            const std::vector<const analysis::Symbol*>& multiResults,
            const Document& doc,
            const analysis::SymbolTable& table,
            const i18n::LspStrings& s,
            i18n::Locale locale,
            const std::string& dynamicDisplayName,
            const std::string& templateSubstitution)
        {
            HoverInfo info;
            info.name = sym->name;
            info.kind = sym->kind;

            // 1. rawSignature
            info.rawSignature = BuildSignatureHelper(sym, originalSym, dynamicDisplayName, templateSubstitution);

            // 2. localScope
            info.localScope = BuildScopeContext(sym);

            // 3. parameters
            if (sym->kind == analysis::SymbolKind::Function || sym->kind == analysis::SymbolKind::Method ||
                sym->kind == analysis::SymbolKind::Constructor || sym->kind == analysis::SymbolKind::Destructor ||
                sym->kind == analysis::SymbolKind::Funcdef)
            {
                info.parameters.emplace();
                for (const auto& p : sym->params) {
                    HoverParam hp;
                    hp.typeName     = p.typeName;
                    hp.name         = p.name;
                    hp.defaultValue = p.defaultValue;
                    info.parameters->push_back(hp);
                }
                info.returnType = sym->typeInfo;
            }

            // 4. template parameters
            if (!sym->templateParam.empty()) {
                info.templateParameters.emplace();
                HoverParam tp;
                tp.typeName = "class";
                tp.name     = sym->templateParam;
                info.templateParameters->push_back(tp);
            }

            // 5. parse documentation
            std::string docSource = sym->docComment;
            std::string targetParam = "";
            if (sym->kind == analysis::SymbolKind::Parameter && sym->parent) {
                docSource    = sym->parent->docComment;
                targetParam  = sym->name;
            }
            if (!docSource.empty()) {
                utils::FillHoverInfoFromDoxygen(docSource, info, targetParam);
            }

            // 7. extras
            if (sym->kind == analysis::SymbolKind::EnumMember && !sym->value.empty())
                info.enumValue = sym->value;

            // overloads
            if (multiResults.size() > 1) {
                // Determine how many unique overloads exist
                std::set<std::string> uniqueSigs;
                for (const auto* r : multiResults) {
                    uniqueSigs.insert(BuildSignatureHelper(r, nullptr, "", ""));
                }
                if (uniqueSigs.size() > 1) {
                    info.overloadCount = (int)uniqueSigs.size() - 1;
                }
            }

            return info;
        }

        void ProcessHover(lsp::requests::TextDocument_Hover::Result &result,
                          const lsp::requests::TextDocument_Hover::Params &req,
                          const Document &doc,
                          const analysis::SymbolTable &table,
                          const analysis::DiagnosticCache *diagCache,
                          i18n::Locale locale,
                          const asIScriptEngine *engine)
        {
            uint32_t line = req.position.line;
            uint32_t col = req.position.character;

            std::string markdown = "";
            const auto &s = i18n::GetStrings(locale);

            std::vector<const analysis::Symbol *> multiResults;
            const analysis::Symbol *sym = analysis::SymbolResolver::ResolveAt(doc, table, line, col, &multiResults);

            std::string dynamicDisplayName = "";
            if (sym != nullptr && (sym->kind == analysis::SymbolKind::Class || sym->kind == analysis::SymbolKind::Enum ||
                                   sym->kind == analysis::SymbolKind::Interface || sym->kind == analysis::SymbolKind::Mixin ||
                                   sym->kind == analysis::SymbolKind::Typedef))
            {
                TSNode nodeUnder = doc.NodeAt(line, col);
                if (!ts_node_is_null(nodeUnder))
                {
                    TSNode current = nodeUnder;
                    while (!ts_node_is_null(current))
                    {
                        std::string_view typeStr = ts_node_type(current);
                        if (typeStr == "type" || typeStr == "datatype")
                        {
                            TSNode prevSibling = ts_node_prev_sibling(current);
                            if (!ts_node_is_null(prevSibling))
                            {
                                std::string_view prevType = ts_node_type(prevSibling);
                                if (prevType == "scope" || prevType == "ERROR")
                                {
                                    std::string_view scopeSv = doc.SourceAt(prevSibling);
                                    std::string scopeStr(scopeSv.begin(), scopeSv.end());
                                    std::string_view typeSv = doc.SourceAt(current);
                                    std::string typeStrText(typeSv.begin(), typeSv.end());
                                    if (!scopeStr.empty())
                                    {
                                        if (typeStrText.starts_with("::"))
                                            dynamicDisplayName = scopeStr + typeStrText;
                                        else
                                            dynamicDisplayName = scopeStr + (scopeStr.ends_with("::") ? "" : "::") + typeStrText;
                                    }
                                }
                            }
                            break;
                        }
                        current = ts_node_parent(current);
                    }
                }
            }

            std::string templateSubstitution = "";
            if (sym != nullptr && sym->kind == analysis::SymbolKind::Method)
            {
                TSNode nodeUnder = doc.NodeAt(line, col);
                if (!ts_node_is_null(nodeUnder))
                {
                    TSNode parent = ts_node_parent(nodeUnder);
                    if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "member_expression")
                    {
                        TSNode memberNode = ts_node_child_by_field_name(parent, "member", 6);
                        if (!ts_node_is_null(memberNode) && ts_node_eq(nodeUnder, memberNode))
                        {
                            TSNode objectNode = ts_node_child_by_field_name(parent, "object", 6);
                            if (!ts_node_is_null(objectNode))
                            {
                                std::string_view objSv = doc.SourceAt(objectNode);
                                std::string objText(objSv.begin(), objSv.end());
                                const analysis::Symbol *objSym = table.FindLocalByName(objText);
                                if (!objSym) objSym = table.FindGlobalByName(objText);

                                if (objSym && !objSym->typeInfo.empty())
                                {
                                    size_t openT = objSym->typeInfo.find('<');
                                    size_t closeT = objSym->typeInfo.rfind('>');
                                    if (openT != std::string::npos && closeT != std::string::npos && closeT > openT)
                                    {
                                        templateSubstitution = objSym->typeInfo.substr(openT + 1, closeT - openT - 1);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (sym != nullptr)
            {
                HoverInfo info = BuildHoverInfo(sym, sym, multiResults, doc, table, s, locale, dynamicDisplayName, templateSubstitution);
                markdown = info.ToMarkdown(locale);
            }
            else
            {
                TSNode nodeUnder = doc.NodeAt(line, col);
                if (!ts_node_is_null(nodeUnder))
                {
                    std::string_view sv = doc.SourceAt(nodeUnder);
                    std::string name(sv.begin(), sv.end());

                    if (engine)
                    {
                        for (asUINT i = 0; i < engine->GetGlobalFunctionCount(); i++)
                        {
                            asIScriptFunction *func = engine->GetGlobalFunctionByIndex(i);
                            if (func && std::string(func->GetName()) == name)
                            {
                                std::string decl = func->GetDeclaration(true, true, true);
                                HoverInfo info;
                                info.name = name;
                                info.kind = analysis::SymbolKind::Function;
                                info.rawSignature = decl;
                                info.builtinLabel = s.hoverBuiltinFunc;
                                info.isBuiltin = true;
                                markdown = info.ToMarkdown(locale);
                                break;
                            }
                        }
                        if (markdown.empty())
                        {
                            int typeId = engine->GetTypeIdByDecl(name.c_str());
                            if (typeId >= 0)
                            {
                                asITypeInfo *type = engine->GetTypeInfoById(typeId);
                                if (type)
                                {
                                    HoverInfo info;
                                    info.name = name;
                                    info.kind = analysis::SymbolKind::Class;
                                    info.rawSignature = "class " + std::string(type->GetName());
                                    info.builtinLabel = s.hoverBuiltinType;
                                    info.isBuiltin = true;
                                    markdown = info.ToMarkdown(locale);
                                }
                            }
                        }
                    }
                }
            }

            // Append Engine Diagnostics
            if (diagCache)
            {
                auto diags = diagCache->GetAt(req.textDocument.uri.toString(), line, col);
                for (const auto *d : diags)
                {
                    if (!markdown.empty())
                        markdown += "\n\n---\n\n";
                    std::string prefix = d->severity == lsp::DiagnosticSeverity::Error ? "**" + std::string(s.hoverEngineError) + "**" : "**" + std::string(s.hoverEngineWarn) + "**";
                    markdown += prefix + " `" + d->message + "`";
                }
            }

            if (markdown.empty())
                return;

            result = lsp::Hover{};
            auto &hover = *result;
            lsp::MarkupContent markup;
            markup.kind = lsp::MarkupKind::Markdown;
            markup.value = markdown;
            hover.contents = markup;
        }
    }
}
