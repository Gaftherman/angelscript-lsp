#include "analysis/NamespaceChecker.h"
#include "analysis/SemanticHelpers.h"

#include <cctype>
#include <string>
#include <vector>
#include "parser/GrammarNames.h"

namespace angel_lsp::analysis
{
    namespace
    {
        /**
         * @brief Whether a node sits inside a `mixin class` body.
         *
         * A mixin is compiled into the class that includes it, so an unqualified name in its body
         * may name a member of a class this file has never seen. See the note at the caller.
         */
        bool IsInsideMixinBody(TSNode node)
        {
            for (TSNode current = node; !ts_node_is_null(current); current = ts_node_parent(current))
            {
                if (std::string_view(ts_node_type(current)) == "mixin_declaration")
                {
                    return true;
                }
            }
            return false;
        }
    }

    void CheckNamespacesAndScopes(const NamespaceCheckRequest &request, DiagnosticContext &ctx)
    {
        const TSNode root = request.root;
        const std::string_view sourceCode = request.sourceCode;

        const auto &table = ctx.request.symbolTable;
        auto usings = CollectUsingNamespaces(root, sourceCode);

        std::vector<TSNode> stack = { root };
        while (!stack.empty())
        {
            TSNode node = stack.back();
            stack.pop_back();

            std::string_view type = ts_node_type(node);

            if (type == "scoped_identifier")
            {
                uint32_t childCount = ts_node_child_count(node);
                if (childCount >= 3)
                {
                    TSNode firstChild = ts_node_child(node, 0);
                    std::string_view firstType = ts_node_type(firstChild);
                    if (firstType == "identifier")
                    {
                        std::string prefix = GetNodeText(firstChild, sourceCode);
                        while (!prefix.empty() && isspace(static_cast<unsigned char>(prefix.front()))) prefix.erase(prefix.begin());
                        while (!prefix.empty() && isspace(static_cast<unsigned char>(prefix.back()))) prefix.pop_back();

                        if (!prefix.empty() && !IsKnownScope(prefix, node, sourceCode, table))
                        {
                            TSPoint startPt = ts_node_start_point(firstChild);
                            TSPoint endPt = ts_node_end_point(firstChild);
                            ctx.EmitAtRange(startPt.row, startPt.column, endPt.row, endPt.column,
                                            "as-err-undefined-namespace", prefix, DiagnosticSeverity::Error);
                        }
                    }
                }
            }

            if (type == "call_expression")
            {
                TSNode funcNode = parser::GetChildByField(node, parser::fields::Function);
                if (ts_node_is_null(funcNode) && ts_node_child_count(node) > 0)
                {
                    funcNode = ts_node_child(node, 0);
                }

                if (!ts_node_is_null(funcNode))
                {
                    std::string calleeName = GetNodeText(funcNode, sourceCode);
                    while (!calleeName.empty() && isspace(static_cast<unsigned char>(calleeName.front()))) calleeName.erase(calleeName.begin());
                    while (!calleeName.empty() && isspace(static_cast<unsigned char>(calleeName.back()))) calleeName.pop_back();

                    if (!calleeName.empty() && calleeName.find("::") == std::string::npos && calleeName.find('.') == std::string::npos)
                    {
                        const Scope *scope = ctx.request.scopeRoot ? FindEnclosingScope(ctx.request.scopeRoot.get(), ts_node_start_point(node).row, ts_node_start_point(node).column) : nullptr;
                        const LocalDefinition *localDef = scope ? ResolveInScope(scope, calleeName) : nullptr;
                        auto inScopeSyms = FindSymbolsInScope(calleeName, node, sourceCode, table);
                        // `super(1)` is the base-constructor call, and it resolves to nothing by
                        // design - there is no symbol named `super`. Every other use of the word
                        // genuinely is undefined, so this excuses the one shape rather than the
                        // name. See IsBaseConstructorCall.
                        if (calleeName == "super" && IsBaseConstructorCall(funcNode, sourceCode))
                        {
                            // Nothing to report, and nothing below applies either: the ambiguity
                            // check that follows would look `super` up in every using-namespace.
                        }
                        // Not inside a mixin: a mixin's members are copied into whatever class
                        // includes it and compiled there, so an unqualified call here can name a
                        // member of a class that is not in this file and may not be in the
                        // workspace at all. Measured on a real Sven Co-op plugin: 12 errors in one
                        // file, every one of them a member of the including class.
                        //
                        // The compiler does report a genuinely undefined name inside a mixin, so
                        // this gives up a true positive. That is the trade this project has already
                        // chosen - an error on code the compiler accepts is the one failure it
                        // treats as fatal, and from in here the world is not visible.
                        //
                        // Mixin bodies cannot be fully checked until included in a concrete host class,
                        // because symbols referenced inside the mixin body may be provided by the host class
                        // or other mixins included alongside it.
                        else if (!localDef && inScopeSyms.empty() && !ctx.request.IsRegisteredSymbol(calleeName) && !table.HasSymbol(calleeName) && !IsInsideMixinBody(node))
                        {
                            TSPoint startPt = ts_node_start_point(funcNode);
                            TSPoint endPt = ts_node_end_point(funcNode);
                            ctx.EmitAtRange(startPt.row, startPt.column, endPt.row, endPt.column,
                                            "as-err-undefined-identifier", calleeName, DiagnosticSeverity::Error);
                        }
                        // Two using-directives that both declare the name used to be reported
                        // here as an ambiguous symbol. The compiler does not agree, and never did:
                        //
                        //     namespace A { void f(string s) {} }
                        //     namespace B { void f(int i) {} }
                        //     using namespace A;  using namespace B;
                        //     void g() { f(1); }        // compiles - picks B::f
                        //
                        // A directive does not shadow and does not stop the search; every imported
                        // namespace contributes at once and ordinary overload resolution decides.
                        // Ambiguity is a property of the signatures, not of the scope count, and
                        // the compiler says so only when resolution itself cannot choose -
                        // "Multiple matching signatures to 'f(const string)'". CallChecker reaches
                        // that verdict through ResolveBestOverload and reports it as
                        // as-err-call-ambiguous, which is the only place it can honestly be
                        // decided. Two namespaces declaring the same *variable* are not reported
                        // by the compiler at all.
                    }
                }
            }

            if (type == "import_declaration" || type == "ERROR")
            {
                std::string nodeText = GetNodeText(node, sourceCode);
                size_t importPos = nodeText.find("import ");
                if (importPos != std::string::npos)
                {
                    size_t fromPos = nodeText.find("from", importPos);
                    if (fromPos != std::string::npos && nodeText.find('{', fromPos) != std::string::npos)
                    {
                        TSPoint startPt = ts_node_start_point(node);
                        TSPoint endPt = ts_node_end_point(node);
                        ctx.EmitAtRange(startPt.row, startPt.column, endPt.row, endPt.column,
                                        "as-err-import-has-body", "import", DiagnosticSeverity::Error);
                    }
                }
            }

            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                stack.push_back(ts_node_child(node, i));
            }
        }
    }
}
