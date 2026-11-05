#include "analysis/index/NodeIndex.h"

#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/types/TypeExtraction.h"
#include "helpers/TestUtils.h"
#include "parser/GrammarNames.h"

#include <array>
#include <doctest/doctest.h>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

TEST_CASE("NodeIndex - Empty on null root")
{
    NodeIndex index;
    CHECK(index.Empty());
    CHECK(index.NodeCount() == 0);
    CHECK(index.Nodes("call_expression").empty());
}

TEST_CASE("NodeIndex - Indexes nodes in document order")
{
    const std::string code = R"(
        void foo()
        {
            int a = 1;
            int b = 2;
            bar(a, b);
        }

        void bar(int x, int y)
        {
            foo();
        }
    )";

    auto doc = angel_lsp::test::CreateTestDocument("file:///test_node_index.as", code);
    REQUIRE(doc->GetTree() != nullptr);

    NodeIndex index(ts_tree_root_node(doc->GetTree()));
    CHECK(!index.Empty());
    CHECK(index.NodeCount() > 0);

    auto funcDecls = index.Nodes(nodes::FuncDeclaration);
    CHECK(funcDecls.size() == 2);

    auto callExprs = index.Nodes(nodes::CallExpression);
    CHECK(callExprs.size() == 2);

    auto varDecls = index.Nodes(nodes::VariableDeclaration);
    CHECK(varDecls.size() == 2);

    // Verify symbol lookup
    TSSymbol funcSym = index.SymbolForName(nodes::FuncDeclaration);
    CHECK(funcSym != 0);
    auto funcDeclsBySym = index.Nodes(funcSym);
    CHECK(funcDeclsBySym.size() == 2);

    // Verify nonexistent symbol
    CHECK(index.Nodes("nonexistent_node_type").empty());
    CHECK(index.SymbolForName("nonexistent_node_type") == 0);

    CHECK(index.NodeCount() > 0);
    CHECK(index.ErrorRanges().empty());
}

TEST_CASE("NodeIndex - Preserves enclosing containers beyond 65535 declarations")
{
    const auto outer = angel_lsp::test::GenerateRandomSymbolName("Outer");
    const auto inner = angel_lsp::test::GenerateRandomSymbolName("Inner");
    std::string code;
    for (uint32_t i = 0; i < 65535; ++i)
    {
        code += "namespace " + outer + " {}\n";
    }
    code += "namespace " + outer + " { class " + inner + " { int value; } }";
    const auto doc = angel_lsp::test::CreateTestDocument("file:///many_containers.as", code);
    REQUIRE(doc->GetTree() != nullptr);

    const NodeIndex index(ts_tree_root_node(doc->GetTree()));
    const auto offset = static_cast<uint32_t>(code.find("value"));
    const auto containers = index.CollectEnclosingContainers(offset, code);
    REQUIRE(containers.size() == 2);
    CHECK(containers.front().qualifiedName == outer + "::" + inner);
    CHECK(containers.back().qualifiedName == outer);
}

TEST_CASE("NodeIndex - Indexed type metadata preserves deep templates and direct argument order")
{
    constexpr uint32_t depth = 80;
    std::string code = "const ";
    for (uint32_t i = 0; i < depth; ++i)
    {
        code += "array<";
    }
    code += "int@";
    code.append(depth, '>');
    code += "@ nested;\nmap<Space::Item@, bool> pair;";

    const auto doc = angel_lsp::test::CreateTestDocument("file:///indexed_nested_type.as", code);
    REQUIRE(doc->GetTree() != nullptr);
    const NodeIndex index(ts_tree_root_node(doc->GetTree()));
    REQUIRE(index.ErrorRanges().empty());

    const auto& gs = GetGrammarSymbols();
    const auto declarations = index.Nodes(gs.symVariableDeclaration);
    REQUIRE(declarations.size() == 2);
    const auto nested = SyntaxOperands(declarations[0], &index).Field(gs.fieldVarType);
    REQUIRE(index.FindOperandSyntax(nested));
    const auto nestedInfo = ExtractTypeInfoFromAST(nested, code, &index);
    CHECK(nestedInfo.kind == TypeKind::Array);
    CHECK(nestedInfo.arrayDepth == depth);
    CHECK(nestedInfo.isConst);
    CHECK(nestedInfo.isHandle);
    CHECK(nestedInfo.hasPrimitiveHandle);
    CHECK(nestedInfo.baseTypeName == "int");
    CHECK(nestedInfo.templateName == "array");
    REQUIRE(nestedInfo.templateArgumentTypes.size() == 1);
    CHECK(nestedInfo.templateArgumentTypes.front() == "int");

    const auto pair = SyntaxOperands(declarations[1], &index).Field(gs.fieldVarType);
    const auto pairInfo = ExtractTypeInfoFromAST(pair, code, &index);
    REQUIRE(pairInfo.templateArgumentTypes.size() == 2);
    CHECK(pairInfo.templateArgumentTypes[0] == "Space::Item");
    CHECK(pairInfo.templateArgumentTypes[1] == "bool");
}

TEST_CASE("NodeIndex - Error range indexing and containment invariant")
{
    const std::string validFn = angel_lsp::test::GenerateRandomSymbolName();
    const std::string brokenFn = angel_lsp::test::GenerateRandomSymbolName();
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName();

    // Valid code has zero error ranges
    const std::string cleanCode = "void " + validFn + "() { int " + varName + " = 42; }\n";
    auto cleanDoc = angel_lsp::test::CreateTestDocument("file:///clean_node_index.as", cleanCode);
    REQUIRE(cleanDoc->GetTree() != nullptr);
    NodeIndex cleanIndex(ts_tree_root_node(cleanDoc->GetTree()));
    CHECK(cleanIndex.ErrorRanges().empty());
    CHECK_FALSE(cleanIndex.IsInsideError(0));
    CHECK_FALSE(cleanIndex.IsInsideError(10));
    CHECK_FALSE(cleanIndex.NodeHasSyntaxError(cleanIndex.Root()));

    // Broken code with syntax error produces error ranges
    const std::string brokenCode = "void " + brokenFn +
                                   "() {\n"
                                   "    int " +
                                   varName +
                                   " = ;\n" // missing expression syntax error
                                   "}\n";
    auto brokenDoc = angel_lsp::test::CreateTestDocument("file:///broken_node_index.as", brokenCode);
    REQUIRE(brokenDoc->GetTree() != nullptr);
    NodeIndex brokenIndex(ts_tree_root_node(brokenDoc->GetTree()));
    CHECK(!brokenIndex.ErrorRanges().empty());

    // The semicolon line has a syntax error
    const auto ranges = brokenIndex.ErrorRanges();
    CHECK(!ranges.empty());
    const uint32_t errStart = ranges.front().startByte;
    CHECK(brokenIndex.IsInsideError(errStart));
    CHECK_FALSE(brokenIndex.IsInsideError(0)); // Start of function is clean
}

TEST_CASE("NodeIndex - Container spans and innermost class lookup")
{
    const std::string className = angel_lsp::test::GenerateRandomSymbolName();
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName();
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName();
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName();

    const std::string code = "// header\n"
                             "int outsideVar = 0;\n"
                             "class " +
                             className +
                             " {\n"
                             "    void " +
                             fnName +
                             "() {\n"
                             "        int " +
                             varName +
                             " = 10;\n"
                             "        auto fn = function() { int inner = 20; };\n"
                             "    }\n"
                             "}\n"
                             "mixin class " +
                             mixinName +
                             " {\n"
                             "    int " +
                             varName +
                             ";\n"
                             "}\n";

    auto doc = angel_lsp::test::CreateTestDocument("file:///test_container_spans.as", code);
    REQUIRE(doc->GetTree() != nullptr);

    NodeIndex index(ts_tree_root_node(doc->GetTree()));
    CHECK(!index.Empty());

    // 1. Outside any class
    CHECK(index.FindInnermostClassSpan(0) == nullptr);

    // 2. Inside class method
    const size_t varPos = code.find(varName);
    REQUIRE(varPos != std::string::npos);
    const auto* spanClass = index.FindInnermostClassSpan(static_cast<uint32_t>(varPos));
    REQUIRE(spanClass != nullptr);
    CHECK_FALSE(spanClass->isMixin);
    std::string extractedClassName =
        code.substr(spanClass->nameStartByte, spanClass->nameEndByte - spanClass->nameStartByte);
    CHECK(extractedClassName == className);

    // 3. Inside lambda closure barrier
    const size_t innerPos = code.find("inner");
    REQUIRE(innerPos != std::string::npos);
    const auto* spanLambda = index.FindInnermostClassSpan(static_cast<uint32_t>(innerPos));
    CHECK(spanLambda == nullptr);

    // 4. Inside mixin class
    const size_t mixinVarPos = code.rfind(varName);
    REQUIRE(mixinVarPos != std::string::npos);
    const auto* spanMixin = index.FindInnermostClassSpan(static_cast<uint32_t>(mixinVarPos));
    REQUIRE(spanMixin != nullptr);
    CHECK(spanMixin->isMixin);
    std::string extractedMixinName =
        code.substr(spanMixin->nameStartByte, spanMixin->nameEndByte - spanMixin->nameStartByte);
    CHECK(extractedMixinName == mixinName);
}

TEST_CASE("NodeIndex - Namespace imports follow the enclosing block and its later declarations")
{
    const std::string code = R"(
        using namespace RootFirst;
        namespace Project {
            void probe() {
                { leftCall(); using namespace Left; }
                { rightCall(); using namespace Right; }
                outerCall();
            }
            using namespace OuterLater;
        }
        using namespace RootLast;
    )";
    const auto doc = angel_lsp::test::CreateTestDocument("file:///lexical_imports.as", code);
    const auto root = ts_tree_root_node(doc->GetTree());
    const NodeIndex index(root);
    REQUIRE(index.ErrorRanges().empty());
    const auto calls = index.Nodes(GetGrammarSymbols().symCallExpression);
    REQUIRE(calls.size() == 3);
    const auto left = CollectVisibleUsingNamespaces(calls[0], code, &index);
    const auto right = CollectVisibleUsingNamespaces(calls[1], code, &index);
    const auto outer = CollectVisibleUsingNamespaces(calls[2], code, &index);
    CHECK(left == std::vector<std::string>{"Left", "OuterLater", "RootFirst", "RootLast"});
    CHECK(right == std::vector<std::string>{"Right", "OuterLater", "RootFirst", "RootLast"});
    CHECK(outer == std::vector<std::string>{"OuterLater", "RootFirst", "RootLast"});
    for (const TSNode call : calls)
    {
        CHECK(CollectVisibleUsingNamespaces(call, code, &index) == CollectVisibleUsingNamespaces(call, code, nullptr));
    }
    CHECK(index.CollectUsingNamespaces(code) ==
          std::vector<std::string>{"RootFirst", "Left", "Right", "OuterLater", "RootLast"});
}

TEST_CASE("NodeIndex - Lookup containers match declaration names, base lists and lambda barriers")
{
    const std::string code = R"(
        namespace Project::Nested {
            class Parent {}
            class Derived : Parent {
                int member;
                void run() {
                    auto callback = function() { int local = 1; };
                }
            }
        }
    )";
    const auto doc = angel_lsp::test::CreateTestDocument("file:///lexical_container_headers.as", code);
    const TSNode root = ts_tree_root_node(doc->GetTree());
    const NodeIndex index(root);
    REQUIRE(index.ErrorRanges().empty());

    const auto& gs = GetGrammarSymbols();
    for (const TSSymbol symbol :
         {gs.symScript, gs.symNamespaceDeclaration, gs.symClassDeclaration, gs.symBaseClassList, gs.symScopedIdentifier,
          gs.symIdentifier, gs.symLambdaExpression, gs.symStatementBlock})
    {
        for (const TSNode node : index.Nodes(symbol))
        {
            INFO(NodeText(node, code));
            const auto oldContainers = GetEnclosingContainers(node, code);
            const auto indexedContainers = GetEnclosingContainers(node, code, &index);
            REQUIRE(indexedContainers.size() == oldContainers.size());
            for (size_t i = 0; i < oldContainers.size(); ++i)
            {
                CHECK(indexedContainers[i].qualifiedName == oldContainers[i].qualifiedName);
                CHECK(indexedContainers[i].kind == oldContainers[i].kind);
            }
        }
    }
}

TEST_CASE("NodeIndex - Indexed operands preserve child fields and recovery order")
{
    const std::string code = R"(
        int global = 1;
        void probe(int value) {
            for (; value < global; ++value) { consume(value /* retained comment */); }
            auto incomplete = ;
            if ((value)) { return; }
        }
    )";
    const auto doc = angel_lsp::test::CreateTestDocument("file:///indexed_operands.as", code);
    REQUIRE(doc->GetTree() != nullptr);
    const NodeIndex index(ts_tree_root_node(doc->GetTree()));
    REQUIRE_FALSE(index.ErrorRanges().empty());
    const auto& gs = GetGrammarSymbols();
    for (const TSSymbol symbol : {gs.symVariableDeclaration, gs.symVariableDeclarator, gs.symForStatement,
                                  gs.symIfStatement, gs.symArgumentList, gs.symCallExpression, gs.symReturnStatement})
    {
        REQUIRE_FALSE(index.Nodes(symbol).empty());
        for (const TSNode node : index.Nodes(symbol))
        {
            INFO(NodeText(node, code));
            REQUIRE(index.FindOperandSyntax(node) != nullptr);
            const SyntaxOperands operands(node, &index);
            const SyntaxOperands treeOnly(node, nullptr);
            for (const TSFieldId field : {gs.fieldName, gs.fieldValue, gs.fieldVarType, gs.fieldCondition, gs.fieldBody,
                                          gs.fieldFunction, gs.fieldArguments})
            {
                CHECK(ts_node_eq(operands.Field(field), treeOnly.Field(field)));
            }
            const uint32_t count = ts_node_named_child_count(node);
            uint32_t ordinal = 0;
            operands.ForEachNamed(
                [&](TSNode child)
                {
                    CHECK(ts_node_eq(child, ts_node_named_child(node, ordinal++)));
                    return true;
                });
            CHECK(ordinal == count);
            CHECK(ts_node_eq(operands.OnlyNamed(), treeOnly.OnlyNamed()));
            CHECK(ts_node_eq(operands.NamedAt(count), treeOnly.NamedAt(count)));
            if (symbol == gs.symVariableDeclarator)
            {
                CHECK(ts_node_eq(operands.DeclaratorValue(code), treeOnly.DeclaratorValue(code)));
            }
        }
    }
}

TEST_CASE("NodeIndex - Conversion context preserves callable boundaries and operand roles")
{
    const std::string code = R"(
        class Holder {
            int stored;
            int &borrow(bool ready) {
                auto callback = function() {
                    if (ready) { return stored; }
                    return stored;
                };
                do { this.stored = 1; } while (ready);
                if (ready) { this.stored++; }
                else if (!ready) { consume(this.stored); }
                for (; ready; ) { return stored; }
                return stored;
            }
        }
        int outside() { return 1; }
    )";
    const auto doc = angel_lsp::test::CreateTestDocument("file:///conversion_context.as", code);
    const NodeIndex index(ts_tree_root_node(doc->GetTree()));
    CHECK_FALSE(index.HasSyntaxErrors());
    const auto& gs = GetGrammarSymbols();
    const std::array callableKinds{gs.symFuncDeclaration, gs.symLambdaExpression};
    size_t references = 0;
    for (const TSNode node : index.Nodes(gs.symFuncDeclaration))
    {
        const TSNode expected = FindAncestorWithSymbol(GetParent(node), gs.symClassDeclaration);
        CHECK(ts_node_eq(index.EnclosingDeclaration(node), expected));
        const auto* facts = index.FindOperandSyntax(node);
        REQUIRE(facts);
        const bool hasRef = !ts_node_is_null(FindChildWithSymbol(node, gs.tokAmp));
        CHECK(facts->hasReturnReferenceToken == hasRef);
        references += hasRef;
    }
    CHECK(references == 1);

    REQUIRE(index.Nodes(gs.symReturnStatement).size() == 5);
    for (const TSNode node : index.Nodes(gs.symReturnStatement))
    {
        const TSNode expected = FindAncestorWithAnySymbol(GetParent(node), callableKinds);
        CHECK(ts_node_eq(index.EnclosingDeclaration(node), expected));
    }

    const std::array conditionalKinds{gs.symIfStatement, gs.symWhileStatement, gs.symForStatement};
    for (const auto symbol : {gs.symIfStatement, gs.symMemberExpression, gs.symReturnStatement})
    {
        REQUIRE_FALSE(index.Nodes(symbol).empty());
        for (const TSNode node : index.Nodes(symbol))
        {
            const auto* facts = index.FindOperandSyntax(node);
            REQUIRE(facts);
            const auto parent = GetParent(node);
            REQUIRE_FALSE(ts_node_is_null(parent));
            CHECK(facts->parentSymbol == ts_node_symbol(parent));
            CHECK(index.IsInsideConditionalStatement(ts_node_start_byte(node)) ==
                  !ts_node_is_null(FindAncestorWithAnySymbol(node, conditionalKinds)));
        }
    }
}

TEST_CASE("Indexed namespace lookup reaches enclosing declarations beyond 32 scopes")
{
    const std::string outer = angel_lsp::test::GenerateRandomSymbolName("Outer");
    const std::string name = angel_lsp::test::GenerateRandomSymbolName("Visible");
    const std::string uri = "file:///deep_namespace.as";
    constexpr int depth = 48;
    std::string source = "namespace " + outer + " { void " + name + "() {}\n";
    for (int level = 0; level < depth; ++level)
    {
        source += "namespace Nested" + std::to_string(level) + " {\n";
    }
    source += "void Probe() { " + name + "(); }\n";
    source.append(depth + 1, '}');

    const auto doc = angel_lsp::test::CreateTestDocument(uri, source);
    REQUIRE(doc->GetTree());
    const NodeIndex index(ts_tree_root_node(doc->GetTree()));
    REQUIRE_FALSE(index.HasSyntaxErrors());
    const auto calls = index.Nodes(GetGrammarSymbols().symCallExpression);
    REQUIRE(calls.size() == 1);

    SymbolTable table;
    SymbolCollector collector(nullptr);
    const auto diagnostics = collector.CollectSymbolsWithIndex({uri, source}, index, table);
    CHECK(diagnostics.empty());
    const auto found = FindSymbolsInScope(name, calls.front(), source, table, &index);
    REQUIRE(found.size() == 1);
    CHECK(found.front().qualifiedName == outer + "::" + name);
}

TEST_CASE("SyntaxOperands navigation chains child facts without hash map lookups")
{
    const std::string source = R"(
        void Foo()
        {
            int val = (((42)));
            Bar(val);
        }
    )";
    const auto doc = angel_lsp::test::CreateTestDocument("file:///syntax_operands_chain.as", source);
    REQUIRE(doc->GetTree());
    const NodeIndex index(ts_tree_root_node(doc->GetTree()));
    const auto& gs = GetGrammarSymbols();

    const auto calls = index.Nodes(gs.symCallExpression);
    REQUIRE(calls.size() == 1);
    const SyntaxOperands callSyntax(calls.front(), &index);
    REQUIRE(callSyntax.Fact() != nullptr);

    const SyntaxOperands callee = callSyntax.CalleeOperands();
    CHECK(callee.Fact() != nullptr);
    CHECK((callee.Symbol() == gs.symIdentifier || callee.Symbol() == gs.symScopedIdentifier));
    CHECK_FALSE(callee.IsNull());
    CHECK(ts_node_eq(callee.Node(), callSyntax.Callee()));

    const SyntaxOperands args = callSyntax.FieldOperands(gs.fieldArguments);
    CHECK(args.Fact() != nullptr);
    CHECK(args.Symbol() == gs.symArgumentList);

    const auto vars = index.Nodes(gs.symVariableDeclarator);
    REQUIRE(vars.size() == 1);
    const SyntaxOperands varSyntax(vars.front(), &index);
    const SyntaxOperands valSyntax = varSyntax.DeclaratorValueOperands(source);
    REQUIRE(valSyntax.Fact() != nullptr);
    CHECK(valSyntax.Symbol() == gs.symParenthesizedExpression);

    const SyntaxOperands unwrapped = UnwrapIndexedParentheses(valSyntax);
    CHECK(unwrapped.Fact() != nullptr);
    CHECK(unwrapped.Symbol() == gs.symNumberLiteral);
    CHECK(ts_node_symbol(unwrapped.Node()) == gs.symNumberLiteral);

    // Null index safety invariant
    const SyntaxOperands detached(valSyntax.Fact(), nullptr);
    CHECK(detached.FieldOperands(gs.fieldValue).IsNull());
    CHECK(detached.FirstNamedOperands().IsNull());
    CHECK(detached.NamedAtOperands(0).IsNull());
    CHECK(detached.OnlyNamedOperands().IsNull());
    CHECK(detached.DeclaratorValueOperands(source).IsNull());

    // Out-of-bounds SyntaxHandle safety invariant
    IndexedSyntaxNode outOfBoundsBucket{SIZE_MAX, NoSyntaxNode, NoSyntaxNode, gs.symIdentifier, 0, 0, true, false,
                                        false,    false,        true};
    CHECK(ts_node_is_null(index.SyntaxHandle(outOfBoundsBucket)));
    IndexedSyntaxNode outOfBoundsUnbucketed{SIZE_MAX, NoSyntaxNode, NoSyntaxNode, gs.symIdentifier, 0, 0, true, false,
                                            false,    false,        false};
    CHECK(ts_node_is_null(index.SyntaxHandle(outOfBoundsUnbucketed)));
}

TEST_CASE("NodeIndex - Hierarchical selection ranges query by line/col and byte offset")
{
    const std::string code = "class Entity {\n"
                             "    void Think() {\n"
                             "        int ticks = 0;\n"
                             "    }\n"
                             "}\n";

    auto doc = angel_lsp::test::CreateTestDocument("file:///selection_test.as", code);
    REQUIRE(doc->GetTree() != nullptr);

    NodeIndex index(ts_tree_root_node(doc->GetTree()), NodeIndexOptions{.source = code});
    const auto rangesLineCol = index.FindEnclosingSelectionRanges(2, 12);
    REQUIRE(rangesLineCol.size() >= 3);
    for (size_t i = 1; i < rangesLineCol.size(); ++i)
    {
        const auto& inner = rangesLineCol[i - 1];
        const auto& outer = rangesLineCol[i];
        const bool startsBefore = outer.startLine < inner.startLine ||
                                  (outer.startLine == inner.startLine && outer.startCharacter <= inner.startCharacter);
        const bool endsAfter = outer.endLine > inner.endLine ||
                               (outer.endLine == inner.endLine && outer.endCharacter >= inner.endCharacter);
        CHECK(startsBefore);
        CHECK(endsAfter);
    }

    const uint32_t byteOffset = static_cast<uint32_t>(code.find("ticks"));
    const auto rangesByte = index.FindEnclosingSelectionRanges(byteOffset);
    REQUIRE_FALSE(rangesByte.empty());
    CHECK(rangesByte.front().startLine == 2);
}

TEST_CASE("NodeIndex - First-pass foldable spans capture blocks and multi-line comments")
{
    const std::string code = "/* multi-line\n"
                             "   comment */\n"
                             "class Entity\n"
                             "{\n"
                             "    void Foo()\n"
                             "    {\n"
                             "    }\n"
                             "}\n";

    auto doc = angel_lsp::test::CreateTestDocument("file:///foldable_test.as", code);
    REQUIRE(doc->GetTree() != nullptr);

    NodeIndex index(ts_tree_root_node(doc->GetTree()), NodeIndexOptions{.source = code});
    const auto spans = index.FoldableSpans();
    REQUIRE(spans.size() >= 2);

    bool foundComment = false;
    bool foundClass = false;
    for (const auto& span : spans)
    {
        if (span.kind == IndexedFoldableKind::Comment && span.range.startLine == 0 && span.range.endLine == 1)
        {
            foundComment = true;
        }
        if (span.range.startLine == 2 && span.range.endLine == 7)
        {
            foundClass = true;
        }
    }
    CHECK(foundComment);
    CHECK(foundClass);
}

TEST_CASE("NodeIndex - First-pass include directives extraction and options handling")
{
    const std::string code = "#include \"util.as\"\n"
                             "#include <vector>\n"
                             "void Main() {}\n";

    auto doc = angel_lsp::test::CreateTestDocument("file:///include_test.as", code);
    REQUIRE(doc->GetTree() != nullptr);

    NodeIndex index(ts_tree_root_node(doc->GetTree()), NodeIndexOptions{.source = code});
    const auto includes = index.IncludeDirectives();
    REQUIRE(includes.size() == 2);

    CHECK(includes[0].rawPath == "util.as");
    CHECK(includes[0].line == 0);
    CHECK_FALSE(includes[0].isAngled);

    CHECK(includes[1].rawPath == "vector");
    CHECK(includes[1].line == 1);
    CHECK(includes[1].isAngled);
}
