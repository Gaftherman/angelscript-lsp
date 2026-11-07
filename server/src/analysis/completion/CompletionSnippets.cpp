#include "analysis/completion/CompletionInternal.h"
#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/CompletionFacts.h"
#include "analysis/resolution/CompletionReceiverResolution.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/AngelScriptParser.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <string>
#include <utility>

namespace angel_lsp::analysis::completion
{

static bool IsInClassBodyOutsideFunction(const analysis::Scope* innermostScope, const CompletionQuery& request)
{
    if (innermostScope && innermostScope->kind == analysis::ScopeKind::Class)
    {
        return true;
    }
    return request.nodeIndex && analysis::IsInClassBodyOutsideCallable(*request.nodeIndex, request.position.line,
                                                                       request.position.character);
}

void CollectClassOperatorSnippets(const analysis::Scope* innermostScope, CompletionCollector& collector)
{
    if (!IsInClassBodyOutsideFunction(innermostScope, collector.request))
    {
        return;
    }
    static constexpr OperatorSnippet k_operatorSnippets[] = {
        {"opCall", "${1:void} opCall(${2})\n{\n    $0\n}", "Call operator overload"},
        {"opIndex", "${1:int}& opIndex(${2:uint} index)\n{\n    $0\n}", "Index operator overload"},
        {"opAssign", "${1:Type}& opAssign(const ${1:Type} &in other)\n{\n    $0\n    return this;\n}",
         "Assignment operator overload"},
        {"opEquals", "bool opEquals(const ${1:Type} &in other) const\n{\n    $0\n}", "Equality operator overload"},
        {"opCmp", "int opCmp(const ${1:Type} &in other) const\n{\n    $0\n}", "Comparison operator overload"},
        {"opAdd", "${1:Type} opAdd(const ${1:Type} &in other) const\n{\n    $0\n}", "Addition operator overload"},
        {"opSub", "${1:Type} opSub(const ${1:Type} &in other) const\n{\n    $0\n}", "Subtraction operator overload"},
        {"opMul", "${1:Type} opMul(const ${1:Type} &in other) const\n{\n    $0\n}", "Multiplication operator overload"},
        {"opDiv", "${1:Type} opDiv(const ${1:Type} &in other) const\n{\n    $0\n}", "Division operator overload"},
    };

    for (const auto& op : k_operatorSnippets)
    {
        CompletionEntry item;
        item.label = op.label;
        item.kind = CompletionEntryKind::Function;
        item.insertText = collector.request.snippetSupport ? op.snippet : op.label;
        if (collector.request.snippetSupport)
        {
            item.isSnippet = true;
        }
        item.sortText = FormatProximitySortText(SymbolProximity::Snippet, op.label);
        item.detail = op.detail;
        collector.items.push_back(std::move(item));
    }
}

void CollectDeclarationSnippets(CompletionCollector& collector)
{
    if (!collector.request.snippetSupport)
    {
        return;
    }
    const std::pair<const char*, const char*> declarationSnippets[] = {
        {"class", "class ${1:Name}\n{\n    ${1:Name}()\n    {\n        $0\n    }\n\n    ~${1:Name}()\n    {\n    }\n}"},
        {"interface", "interface ${1:Name}\n{\n    void ${2:DoThing}();\n}"},
        {"mixin", "mixin class ${1:Name}\n{\n    $0\n}"},
        {"function", "function(${1:int value})\n{\n    $0\n}"},
        {"enum", "enum ${1:Name}\n{\n    ${2:Member} = 0\n}"},
        {"funcdef", "funcdef ${1:void} ${2:Name}(${3:int value});"},
        {"switch", "switch (${1:value})\n{\ncase ${2:0}:\n    $0\n    break;\n\ndefault:\n    break;\n}"},
        {"if", "if (${1:condition})\n{\n    $0\n}"},
        {"else", "else\n{\n    $0\n}"},
        {"for", "for (uint ${1:i} = 0; ${1:i} < ${2:count}; ${1:i}++)\n{\n    $0\n}"},
        {"while", "while (${1:condition})\n{\n    $0\n}"},
        {"do", "do\n{\n    $0\n}\nwhile (${1:condition});"},
        {"try", "try\n{\n    $0\n}\ncatch\n{\n}"},
    };

    const std::pair<const char*, const char*> snippetDetails[] = {
        {"class", "declaration, with a constructor and a destructor"},
        {"interface", "declaration, with one method"},
        {"mixin", "mixin class declaration"},
        {"function", "anonymous function, for a funcdef parameter or handle"},
        {"enum", "declaration, with one member"},
        {"funcdef", "function-pointer type declaration"},
        {"switch", "block, with a case and a default"},
        {"if", "block"},
        {"else", "block"},
        {"for", "loop over a counter"},
        {"while", "loop"},
        {"do", "loop, with the test at the end"},
        {"try", "block, with its catch"},
    };

    for (size_t i = 0; i < std::size(declarationSnippets); ++i)
    {
        CompletionEntry item;
        item.label = declarationSnippets[i].first;
        item.kind = CompletionEntryKind::Snippet;
        item.insertText = declarationSnippets[i].second;
        item.isSnippet = true;
        item.sortText = FormatProximitySortText(SymbolProximity::Snippet, declarationSnippets[i].first);
        item.detail = snippetDetails[i].second;
        collector.items.push_back(std::move(item));
    }

    CompletionEntry include;
    include.label = "#include";
    include.kind = CompletionEntryKind::Snippet;
    include.insertText = "#include \"$1\"";
    include.isSnippet = true;
    include.sortText = FormatProximitySortText(SymbolProximity::Snippet, "#include");
    include.detail = "directive, with the path left open";
    collector.items.push_back(std::move(include));
}

void CollectKeywords(CompletionCollector& collector)
{
    const std::string enclosingClassName = FindEnclosingClassName(collector.request);
    const bool inClass = !enclosingClassName.empty();
    bool hasBaseClass = false;
    if (inClass)
    {
        auto classSymbols = collector.request.symbolTable.FindSymbols(enclosingClassName);
        for (const auto& sym : classSymbols)
        {
            if (sym.type == analysis::SymbolType::Class &&
                std::holds_alternative<analysis::ClassSignature>(sym.signature))
            {
                if (!sym.GetClass().bases.empty())
                {
                    hasBaseClass = true;
                    break;
                }
            }
        }
    }

    for (const auto& kw : GetKeywords())
    {
        if (kw == parser::keywords::This && !inClass)
        {
            continue;
        }
        if (kw == "super" && (!inClass || !hasBaseClass))
        {
            continue;
        }
        AddItemIfNew(collector, {kw, CompletionEntryKind::Keyword, "", "", "", "",
                                 FormatProximitySortText(SymbolProximity::Keyword, kw)});
    }
}

} // namespace angel_lsp::analysis::completion
