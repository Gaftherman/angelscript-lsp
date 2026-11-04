#include "analysis/index/NodeIndex.h"

#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <limits>

extern "C" const TSLanguage* tree_sitter_angelscript();

namespace angel_lsp::analysis
{
NodeIndex::NodeIndex(TSNode root, const TSLanguage* lang, TraversalBudget* budget, NodeIndexContents contents)
{
    Build(root, lang, budget, contents);
}

const ankerl::unordered_dense::map<std::string_view, TSSymbol>& NodeIndex::GetSymbolNameMap() noexcept
{
    static const auto s_map = []()
    {
        ankerl::unordered_dense::map<std::string_view, TSSymbol> map;
        const TSLanguage* lang = tree_sitter_angelscript();
        const uint32_t symCount = ts_language_symbol_count(lang);
        for (const std::string_view name : parser::k_allNodeTypes)
        {
            const TSSymbol sym =
                ts_language_symbol_for_name(lang, name.data(), static_cast<uint32_t>(name.length()), true);
            if (sym != 0)
            {
                map[name] = sym;
            }
        }
        for (uint32_t s = 0; s < symCount; ++s)
        {
            const char* name = ts_language_symbol_name(lang, static_cast<TSSymbol>(s));
            if (name)
            {
                map.try_emplace(std::string_view(name), static_cast<TSSymbol>(s));
            }
        }
        return map;
    }();
    return s_map;
}

void NodeIndex::PopulatePredefinedSymbols(const TSLanguage* lang)
{
    if (!lang)
    {
        return;
    }
    const uint32_t symCount = ts_language_symbol_count(lang);
    if (m_nodesBySymbol.size() < symCount)
    {
        m_nodesBySymbol.resize(symCount);
    }
}

static IndexedDeclarationKind DeclarationKind(TSSymbol symbol)
{
    static const auto bySymbol = []
    {
        using Kind = IndexedDeclarationKind;
        const auto& gs = parser::GetGrammarSymbols();
        std::array<Kind, UINT16_MAX + size_t{1}> kinds{};
        kinds[gs.symFuncDeclaration] = Kind::Function;
        kinds[gs.symInterfaceMethod] = Kind::Function;
        kinds[gs.symImportDeclaration] = Kind::Function;
        kinds[gs.symClassDeclaration] = Kind::Class;
        kinds[gs.symMixinDeclaration] = Kind::Class;
        kinds[gs.symInterfaceDeclaration] = Kind::Interface;
        kinds[gs.symNamespaceDeclaration] = Kind::Namespace;
        kinds[gs.symEnumDeclaration] = Kind::Enum;
        kinds[gs.symTypedefDeclaration] = Kind::Typedef;
        kinds[gs.symFuncdefDeclaration] = Kind::Funcdef;
        kinds[gs.symVirtualProperty] = Kind::Property;
        kinds[gs.symVariableDeclaration] = Kind::Variable;
        kinds[gs.symLambdaExpression] = Kind::Lambda;
        kinds[gs.symCallExpression] = Kind::Call;
        kinds[gs.symUsingDeclaration] = Kind::Using;
        kinds[0] = Kind::None;
        return kinds;
    }();
    return bySymbol[symbol];
}

size_t NodeIndex::IndexBucketNode(TSNode node, TSSymbol symbol, IndexedDeclarationKind kind)
{
    if (symbol >= m_nodesBySymbol.size())
    {
        m_nodesBySymbol.resize(static_cast<size_t>(symbol) + 1);
    }
    auto& bucket = m_nodesBySymbol[symbol];
    const size_t slot = bucket.size();
    bucket.push_back(node);
    if (kind != IndexedDeclarationKind::None)
    {
        MutableFlowFacts().declarations.push_back({slot, symbol, kind});
    }
    return slot;
}

size_t NodeIndex::IndexNode(TSNode node, TraversalBudget* budget, uint32_t parentSpanIdx, bool full)
{
    if (budget)
    {
        budget->RecordVisit(node);
    }

    ++m_nodeCount;
    const bool isError = ts_node_is_error(node);
    if (isError || ts_node_is_missing(node))
    {
        m_errorRanges.push_back(ErrorRange{ts_node_start_byte(node), ts_node_end_byte(node)});
        m_errorNodes.push_back(node);
    }

    const TSSymbol sym = ts_node_symbol(node);
    const auto& gs = parser::GetGrammarSymbols();
    const auto kind = DeclarationKind(sym);
    if (!full)
    {
        if (kind != IndexedDeclarationKind::None || (sym == gs.symSwitchStatement && ts_node_has_error(node)))
        {
            return IndexBucketNode(node, sym, kind);
        }
        return SIZE_MAX;
    }
    if (sym == m_symComment)
    {
        TSPoint sp = ts_node_start_point(node);
        TSPoint ep = ts_node_end_point(node);
        m_commentRanges.push_back(
            CommentRange{ts_node_start_byte(node), ts_node_end_byte(node), sp.row, sp.column, ep.row, ep.column});
    }

    const bool isQueriedAnonymous = sym == gs.tokShared || sym == gs.tokMixin || sym == m_symComment;
    if ((ts_node_is_named(node) || isQueriedAnonymous) && !isError && sym < 4096)
    {
        RecordContainerSpan(node, sym, parentSpanIdx);
        return IndexBucketNode(node, sym, kind);
    }
    return SIZE_MAX;
}

void NodeIndex::RecordContainerSpan(TSNode node, TSSymbol sym, uint32_t parentSpanIdx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symClassDeclaration || sym == gs.symMixinDeclaration || sym == gs.symInterfaceDeclaration ||
        sym == gs.symNamespaceDeclaration || sym == gs.symLambdaExpression)
    {
        IndexedContainerSpan span;
        span.startByte = ts_node_start_byte(node);
        span.endByte = ts_node_end_byte(node);
        span.parentSpanIndex = parentSpanIdx;
        span.symbol = sym;
        if (sym != gs.symLambdaExpression)
        {
            TSNode nameNode = parser::GetChildByField(node, gs.fieldName);
            if (!ts_node_is_null(nameNode))
            {
                span.nameStartByte = ts_node_start_byte(nameNode);
                span.nameEndByte = ts_node_end_byte(nameNode);
            }
        }
        span.isMixin =
            ((sym == gs.symMixinDeclaration) || !ts_node_is_null(parser::FindChildWithSymbol(node, gs.tokMixin))) ? 1
                                                                                                                  : 0;
        m_containerSpans.push_back(span);
    }
}

static SourceRange FlowRange(TSNode node)
{
    const auto start = ts_node_start_point(node);
    const auto end = ts_node_end_point(node);
    return {start.row, start.column, end.row, end.column};
}

NodeIndex::FlowFacts& NodeIndex::MutableFlowFacts()
{
    if (!m_flowFacts)
    {
        m_flowFacts = std::make_shared<FlowFacts>();
    }
    return *m_flowFacts;
}

std::span<const IndexedFlowFinding> NodeIndex::FlowFindings() const noexcept
{
    return m_flowFacts ? std::span<const IndexedFlowFinding>(m_flowFacts->findings)
                       : std::span<const IndexedFlowFinding>{};
}

std::span<const SyntaxNodeId> NodeIndex::AssignmentCallables(bool lambdas) const noexcept
{
    if (!m_flowFacts)
    {
        return {};
    }
    return lambdas ? m_flowFacts->assignmentLambdas : m_flowFacts->assignmentFunctions;
}

const IndexedSyntaxNode* NodeIndex::SyntaxNode(SyntaxNodeId id) const noexcept
{
    return id == NoSyntaxNode ? nullptr : &m_flowFacts->syntaxNodes[id];
}

std::span<const NodeIndex::InitializerIdentifier> NodeIndex::InitializerIdentifiers(uint32_t startByte,
                                                                                    uint32_t endByte) const noexcept
{
    if (!m_flowFacts || startByte >= endByte)
    {
        return {};
    }
    const auto& names = m_flowFacts->initializerIdentifiers;
    const auto startOf = [](const InitializerIdentifier& item) { return item.name.startByte; };
    const auto first = std::ranges::lower_bound(names, startByte, {}, startOf);
    const auto last = std::ranges::lower_bound(first, names.end(), endByte, {}, startOf);
    return {first, last};
}

const IndexedSyntaxNode* NodeIndex::FindOperandSyntax(TSNode node) const noexcept
{
    if (!m_flowFacts || ts_node_is_null(node) || node.tree != m_root.tree)
    {
        return nullptr;
    }
    const auto& operands = m_flowFacts->operandSyntax;
    const auto found = operands.find({node.id, ts_node_start_byte(node)});
    return found == operands.end() ? nullptr : SyntaxNode(found->second);
}

TSNode NodeIndex::SyntaxHandle(const IndexedSyntaxNode& node) const noexcept
{
    if (node.inSymbolBucket)
    {
        if (node.symbol < m_nodesBySymbol.size() && node.syntaxSlot < m_nodesBySymbol[node.symbol].size())
        {
            return m_nodesBySymbol[node.symbol][node.syntaxSlot];
        }
        return {};
    }
    if (m_flowFacts && node.syntaxSlot < m_flowFacts->unbucketedSyntax.size())
    {
        return m_flowFacts->unbucketedSyntax[node.syntaxSlot];
    }
    return {};
}

TSNode NodeIndex::EnclosingDeclaration(TSNode node) const noexcept
{
    const auto* fact = FindOperandSyntax(node);
    if (!fact || !fact->inSymbolBucket)
    {
        return {};
    }

    const auto& gs = parser::GetGrammarSymbols();
    if (fact->symbol != gs.symFuncDeclaration && fact->symbol != gs.symReturnStatement)
    {
        return {};
    }

    const auto& owners =
        fact->symbol == gs.symFuncDeclaration ? m_flowFacts->classByFunctionSlot : m_flowFacts->callableByReturnSlot;
    if (fact->syntaxSlot >= owners.size())
    {
        return {};
    }
    const auto* enclosing = SyntaxNode(owners[fact->syntaxSlot]);
    return enclosing ? SyntaxHandle(*enclosing) : TSNode{};
}

bool NodeIndex::IsInsideConditionalStatement(uint32_t byteOffset) const noexcept
{
    if (!m_flowFacts)
    {
        return false;
    }
    const auto& ranges = m_flowFacts->conditionalStatements;
    const auto after = std::ranges::upper_bound(ranges, byteOffset, {}, &ErrorRange::startByte);
    return after != ranges.begin() && byteOffset < std::prev(after)->endByte;
}

NodeIndex::FlowFactCollector::FlowFactCollector(NodeIndex& index) : m_index(index)
{
}

static bool IsFlowCallable(TSSymbol symbol, const parser::GrammarSymbols& grammar)
{
    return symbol == grammar.symFuncDeclaration || symbol == grammar.symLambdaExpression;
}

void NodeIndex::FlowFactCollector::Enter(TSNode node, const TSTreeCursor& cursor, size_t syntaxSlot)
{
    const auto symbol = ts_node_symbol(node);
    Frame frame;
    frame.symbol = symbol;
    frame.isNamed = ts_node_is_named(node);
    frame.inCallable = IsFlowCallable(symbol, m_grammar) || (!m_frames.empty() && m_frames.back().inCallable);
    CaptureEnclosingContext(frame, node);
    if (symbol == m_grammar.symStatementBlock || symbol == m_grammar.symCaseClause)
    {
        frame.checkUnreachable = !ts_node_has_error(node);
    }
    if (!m_frames.empty())
    {
        auto& parentFrame = m_frames.back();
        const auto parent = parentFrame.symbol;
        ObserveFunctionReturnToken(parentFrame, symbol);
        ObserveChild(parentFrame, frame, node);
        if (symbol == m_grammar.symCaseClause && parent == m_grammar.symSwitchStatement)
        {
            frame.switchSlot = parentFrame.switchSlot;
            frame.clauseRange = FlowRange(node);
        }
        if (parentFrame.syntaxId != NoSyntaxNode || parent == m_grammar.symIfStatement)
        {
            frame.field = ts_tree_cursor_current_field_id(&cursor);
        }
        if (symbol == m_grammar.symStatementBlock && frame.field == m_grammar.fieldBody &&
            IsFlowCallable(parent, m_grammar))
        {
            auto& bodies = m_index.MutableFlowFacts().returningBodies;
            frame.bodySlot = bodies.size();
            bodies.push_back(ts_node_start_byte(node));
        }
    }
    if (symbol == m_grammar.symSwitchStatement)
    {
        auto& switches = m_index.MutableFlowFacts().switches;
        frame.switchSlot = switches.size();
        switches.push_back({ts_node_start_byte(node), {}});
    }
    CaptureSyntaxNode(frame, node, syntaxSlot);
    CaptureInitializerIdentifier(frame, node);
    CaptureUsingFacts(frame, node);
    m_frames.push_back(frame);
}

/// Carries lexical context down the cursor stack and stores only the outermost conditional spans.
void NodeIndex::FlowFactCollector::CaptureEnclosingContext(Frame& frame, TSNode node)
{
    if (!m_frames.empty())
    {
        const auto& parent = m_frames.back();
        frame.parentSymbol = parent.symbol;
        frame.enclosingClass = parent.enclosingClass;
        frame.enclosingCallable = parent.enclosingCallable;
        frame.inConditionalStatement = parent.inConditionalStatement;
    }

    const auto& gs = m_grammar;
    if (!frame.inConditionalStatement && (frame.symbol == gs.symIfStatement || frame.symbol == gs.symWhileStatement ||
                                          frame.symbol == gs.symForStatement))
    {
        frame.inConditionalStatement = true;
        m_index.MutableFlowFacts().conditionalStatements.push_back({ts_node_start_byte(node), ts_node_end_byte(node)});
    }
}

void NodeIndex::FlowFactCollector::ObserveFunctionReturnToken(const Frame& parent, TSSymbol child)
{
    if (parent.symbol == m_grammar.symFuncDeclaration && child == m_grammar.tokAmp && parent.syntaxId != NoSyntaxNode)
    {
        m_index.MutableFlowFacts().syntaxNodes[parent.syntaxId].hasReturnReferenceToken = true;
    }
}

void NodeIndex::FlowFactCollector::CaptureUsingFacts(Frame& frame, TSNode node)
{
    const auto symbol = frame.symbol;
    if (symbol == m_grammar.symScript || symbol == m_grammar.symNamespaceBody ||
        symbol == m_grammar.symStatementBlock || symbol == m_grammar.symError)
    {
        frame.usingScope = m_usingScopes.size();
        m_usingScopes.push_back({ts_node_start_byte(node), ts_node_end_byte(node)});
    }
    if (symbol == m_grammar.symUsingDeclaration)
    {
        CaptureUsingName(node);
    }
    if (symbol == m_grammar.symBaseClassList)
    {
        m_index.MutableFlowFacts().baseClassLists.push_back({ts_node_start_byte(node), ts_node_end_byte(node)});
    }
}

void NodeIndex::FlowFactCollector::CaptureUsingName(TSNode node)
{
    const TSNode name = parser::GetChildByField(node, m_grammar.fieldName);
    if (ts_node_is_null(name))
    {
        return;
    }
    auto& names = m_index.MutableFlowFacts().usingNames;
    const size_t id = names.size();
    names.push_back({{ts_node_start_byte(name), ts_node_end_byte(name)}});
    if (m_frames.empty() || m_frames.back().usingScope == SIZE_MAX)
    {
        return;
    }

    auto& owner = m_frames.back();
    if (owner.lastUsingName == SIZE_MAX)
    {
        m_usingScopes[owner.usingScope].firstName = id;
    }
    else
    {
        names[owner.lastUsingName].next = id;
    }
    owner.lastUsingName = id;
}

/// Keeps only scopes with imports and links them in preorder, including imports declared after nested blocks.
void NodeIndex::FlowFactCollector::FinishUsingScopes()
{
    if (!m_index.m_flowFacts || m_index.m_flowFacts->usingNames.empty())
    {
        return;
    }
    auto& scopes = m_index.m_flowFacts->usingScopes;
    size_t enclosing = SIZE_MAX;
    for (auto scope : m_usingScopes)
    {
        if (scope.firstName == SIZE_MAX)
        {
            continue;
        }
        while (enclosing != SIZE_MAX && scope.startByte >= scopes[enclosing].endByte)
        {
            enclosing = scopes[enclosing].parentSpanIndex;
        }
        scope.parentSpanIndex = enclosing;
        enclosing = scopes.size();
        scopes.push_back(scope);
    }
}

bool NodeIndex::FlowFactCollector::IsAssignmentStatement(TSSymbol symbol) const noexcept
{
    const auto& gs = m_grammar;
    return IsFlowCallable(symbol, gs) || symbol == gs.symStatementBlock || symbol == gs.symExpressionStatement ||
           symbol == gs.symVariableDeclaration || symbol == gs.symVariableDeclarator || symbol == gs.symIfStatement ||
           symbol == gs.symSwitchStatement || symbol == gs.symCaseClause || symbol == gs.symWhileStatement ||
           symbol == gs.symDoWhileStatement || symbol == gs.symForStatement || symbol == gs.symReturnStatement ||
           symbol == gs.symBreakStatement || symbol == gs.symContinueStatement;
}

/// Uses a grammar-symbol mask so capture eligibility is constant per visited node.
bool NodeIndex::FlowFactCollector::CapturesOperands(TSSymbol symbol) const noexcept
{
    static const auto operandParents = []
    {
        const auto& gs = parser::GetGrammarSymbols();
        std::bitset<static_cast<size_t>(std::numeric_limits<TSSymbol>::max()) + 1> symbols;
        for (const auto site : gs.conversionTargetSymbols)
        {
            symbols.set(site);
        }
        for (const auto site :
             {gs.symForeachVariable, gs.symStatementBlock, gs.symVariableDeclarator, gs.symLambdaExpression,
              gs.symClassDeclaration, gs.symScopedIdentifier, gs.symArgumentList, gs.symParenthesizedExpression,
              gs.symFunctionalCastExpression, gs.symTypedInitializerList, gs.symType, gs.symDatatype,
              gs.symPrimitiveType, gs.symTemplateTypeList, gs.symParameterList, gs.symParameter,
              gs.symLambdaParameterList})
        {
            symbols.set(site);
        }
        symbols.reset(0);
        return symbols;
    }();
    return operandParents[symbol];
}

size_t NodeIndex::FlowFactCollector::StoreUnbucketedSyntax(TSNode node)
{
    auto& extra = m_index.MutableFlowFacts().unbucketedSyntax;
    const size_t slot = extra.size();
    extra.push_back(node);
    return slot;
}

SyntaxNodeId NodeIndex::FlowFactCollector::AddSyntaxFact(Frame& frame, TSNode node, size_t syntaxSlot, bool afterAssign)
{
    const bool inBucket = syntaxSlot != SIZE_MAX;
    auto& facts = m_index.MutableFlowFacts();
    const SyntaxNodeId id = facts.syntaxNodes.size();
    facts.syntaxNodes.push_back({inBucket ? syntaxSlot : StoreUnbucketedSyntax(node), NoSyntaxNode, NoSyntaxNode,
                                 frame.symbol, frame.field, frame.parentSymbol, frame.isNamed, afterAssign,
                                 frame.inCallable && frame.isNamed && IsAssignmentStatement(frame.symbol), false,
                                 inBucket});
    if (frame.captureOperands)
    {
        facts.operandSyntax.try_emplace(std::make_pair(node.id, ts_node_start_byte(node)), id);
    }
    if (frame.symbol == m_grammar.symFuncDeclaration)
    {
        facts.assignmentFunctions.push_back(id);
        facts.classByFunctionSlot.push_back(frame.enclosingClass);
        frame.enclosingCallable = id;
    }
    else if (frame.symbol == m_grammar.symLambdaExpression)
    {
        facts.assignmentLambdas.push_back(id);
        frame.enclosingCallable = id;
    }
    else if (frame.symbol == m_grammar.symClassDeclaration)
    {
        frame.enclosingClass = id;
    }
    else if (frame.symbol == m_grammar.symReturnStatement)
    {
        facts.callableByReturnSlot.push_back(frame.enclosingCallable);
    }
    return id;
}

/// Keeps modifier and separator tokens used by type and lambda parameter readers.
static bool CapturesAnonymousOperands(TSSymbol symbol, const parser::GrammarSymbols& gs) noexcept
{
    return symbol == gs.symType || symbol == gs.symPrimitiveType || symbol == gs.symLambdaParameterList;
}

void NodeIndex::FlowFactCollector::CaptureSyntaxNode(Frame& frame, TSNode node, size_t syntaxSlot)
{
    frame.captureOperands = frame.isNamed && CapturesOperands(frame.symbol);
    Frame* parent = m_frames.empty() ? nullptr : &m_frames.back();
    const bool hasParent = parent && parent->syntaxId != NoSyntaxNode && (frame.inCallable || parent->captureOperands);
    const bool afterAssign = hasParent && ObserveAssignmentChild(*parent, frame.symbol, frame.field);
    const bool callable = IsFlowCallable(frame.symbol, m_grammar);
    const bool linkedOperand = hasParent && (frame.isNamed || frame.field != 0 || afterAssign ||
                                             CapturesAnonymousOperands(parent->symbol, m_grammar));
    if (!callable && !frame.captureOperands && !linkedOperand)
    {
        return;
    }

    frame.syntaxId = AddSyntaxFact(frame, node, syntaxSlot, afterAssign);
    if (hasParent)
    {
        LinkSyntaxChild(*parent, frame.syntaxId);
    }
}

/// Captures read candidates without including member selectors, parameter names or text literals.
void NodeIndex::FlowFactCollector::CaptureInitializerIdentifier(Frame& frame, TSNode node)
{
    const auto& gs = m_grammar;
    if (m_frames.empty() || frame.symbol == gs.symComment || frame.symbol == gs.symStringLiteral ||
        frame.symbol == gs.symConcatenatedString)
    {
        return;
    }

    const auto& parent = m_frames.back();
    frame.inDeclarator = parent.inDeclarator || parent.symbol == gs.symVariableDeclarator;
    if (!frame.inDeclarator || frame.symbol != gs.symIdentifier || parent.symbol == gs.symParameter)
    {
        return;
    }
    if (parent.symbol == gs.symMemberExpression && frame.field == gs.fieldMember)
    {
        return;
    }

    InitializerIdentifier name{{ts_node_start_byte(node), ts_node_end_byte(node)}};
    if (parent.symbol == gs.symScopedIdentifier)
    {
        if (m_frames.size() >= 2 && m_frames[m_frames.size() - 2].symbol == gs.symMemberExpression &&
            parent.field == gs.fieldMember)
        {
            return;
        }
        const TSNode scoped = m_index.SyntaxHandle(*m_index.SyntaxNode(parent.syntaxId));
        name.scopedName = {ts_node_start_byte(scoped), ts_node_end_byte(scoped)};
    }
    m_index.MutableFlowFacts().initializerIdentifiers.push_back(name);
}

/// Remembers the child after '=' in a recovered declarator and the keyword of a case clause.
bool NodeIndex::FlowFactCollector::ObserveAssignmentChild(Frame& parent, TSSymbol symbol, TSFieldId field)
{
    if (parent.symbol == m_grammar.symCaseClause && !parent.hasChild)
    {
        m_index.MutableFlowFacts().syntaxNodes[parent.syntaxId].defaultClause =
            (field == m_grammar.fieldDefaultClause || symbol == m_grammar.tokDefault);
    }
    const bool afterAssign = parent.takeInitializer;
    if (parent.symbol == m_grammar.symVariableDeclarator)
    {
        parent.takeInitializer = symbol == m_grammar.tokAssign;
    }
    return afterAssign;
}

void NodeIndex::FlowFactCollector::LinkSyntaxChild(Frame& parent, SyntaxNodeId child)
{
    auto& nodes = m_index.MutableFlowFacts().syntaxNodes;
    if (parent.lastSyntaxChild == NoSyntaxNode)
    {
        nodes[parent.syntaxId].first = child;
    }
    else
    {
        nodes[parent.lastSyntaxChild].next = child;
    }
    parent.lastSyntaxChild = child;
}

void NodeIndex::FlowFactCollector::ObserveChild(Frame& parent, const Frame& child, TSNode node)
{
    if (!child.isNamed)
    {
        return;
    }
    if (parent.checkUnreachable)
    {
        ObserveUnreachable(parent, child.symbol, node);
    }
    if (parent.switchSlot != SIZE_MAX)
    {
        ObserveSwitchChild(parent, child.symbol, node);
    }
}

void NodeIndex::FlowFactCollector::ObserveUnreachable(Frame& parent, TSSymbol symbol, TSNode node)
{
    if (symbol == m_grammar.symComment)
    {
        return;
    }
    if (symbol == m_grammar.symPreprocDirective)
    {
        parent.checkUnreachable = false;
        return;
    }
    if (parent.terminated)
    {
        m_index.MutableFlowFacts().findings.push_back({FlowRange(node), IndexedFlowFindingKind::Unreachable});
        parent.checkUnreachable = false;
        return;
    }
    parent.terminated = symbol == m_grammar.symReturnStatement || symbol == m_grammar.symBreakStatement ||
                        symbol == m_grammar.symContinueStatement;
}

void NodeIndex::FlowFactCollector::ObserveSwitchChild(const Frame& parent, TSSymbol symbol, TSNode node)
{
    if (parent.symbol == m_grammar.symCaseClause && !parent.isDefault && parent.namedChildren == 0)
    {
        if (parent.isCase)
        {
            m_index.MutableFlowFacts().switches[parent.switchSlot].labels.push_back(node);
        }
        return;
    }
    if (symbol == m_grammar.symVariableDeclaration)
    {
        m_index.MutableFlowFacts().findings.push_back({FlowRange(node), IndexedFlowFindingKind::VariableInCase});
    }
}

void NodeIndex::FlowFactCollector::Leave()
{
    const Frame frame = m_frames.back();
    m_frames.pop_back();
    const bool returns = Returns(frame);
    if (!returns && frame.bodySlot != SIZE_MAX)
    {
        m_index.MutableFlowFacts().returningBodies[frame.bodySlot] = UINT32_MAX;
    }
    if (frame.symbol == m_grammar.symSwitchStatement && frame.hasDefault && !frame.defaultIsLast)
    {
        m_index.MutableFlowFacts().findings.push_back({frame.clauseRange, IndexedFlowFindingKind::DefaultNotLast});
    }
    if (!m_frames.empty())
    {
        AddChild(m_frames.back(), frame, returns);
    }
}

bool NodeIndex::FlowFactCollector::IsBreakBoundary(TSSymbol symbol) const
{
    return symbol == m_grammar.symWhileStatement || symbol == m_grammar.symDoWhileStatement ||
           symbol == m_grammar.symForStatement || symbol == m_grammar.symForeachStatement ||
           symbol == m_grammar.symSwitchStatement;
}

void NodeIndex::FlowFactCollector::AddChild(Frame& parent, const Frame& child, bool returns) const
{
    if (!parent.hasChild)
    {
        parent.isDefault = child.field == m_grammar.fieldDefaultClause || child.symbol == m_grammar.tokDefault;
        parent.isCase = child.symbol == m_grammar.tokCase;
        parent.hasChild = true;
    }
    if (parent.symbol != m_grammar.symSwitchStatement || child.symbol == m_grammar.symCaseClause)
    {
        parent.hasEscapingBreak |=
            child.symbol == m_grammar.symBreakStatement || (child.hasEscapingBreak && !IsBreakBoundary(child.symbol));
    }
    if (!child.isNamed)
    {
        return;
    }

    ++parent.namedChildren;
    if (parent.symbol == m_grammar.symSwitchStatement)
    {
        AddSwitchClause(parent, child, returns);
    }
    else if (parent.symbol == m_grammar.symIfStatement)
    {
        parent.consequenceReturns |= child.field == m_grammar.fieldConsequence && returns;
        parent.alternativeReturns |= child.field == m_grammar.fieldAlternative && returns;
    }
    else if (parent.symbol != m_grammar.symCaseClause || parent.isDefault || parent.namedChildren > 1)
    {
        parent.returnsFromChild |= returns;
        parent.allChildrenReturn &= returns;
    }
}

void NodeIndex::FlowFactCollector::AddSwitchClause(Frame& parent, const Frame& clause, bool returns) const
{
    if (clause.symbol != m_grammar.symCaseClause)
    {
        return;
    }
    if (clause.isDefault)
    {
        parent.hasDefault = true;
        parent.defaultReturns = returns;
        parent.clauseRange = clause.clauseRange;
    }
    parent.defaultIsLast = clause.isDefault;
    const uint32_t labelCount = clause.isDefault ? 0u : 1u;
    parent.allChildrenReturn &= clause.namedChildren <= labelCount || returns;
}

bool NodeIndex::FlowFactCollector::Returns(const Frame& frame) const
{
    if (frame.symbol == m_grammar.symReturnStatement)
    {
        return true;
    }
    if (frame.symbol == m_grammar.symStatementBlock || frame.symbol == m_grammar.symCaseClause)
    {
        return frame.returnsFromChild;
    }
    if (frame.symbol == m_grammar.symIfStatement)
    {
        return frame.consequenceReturns && frame.alternativeReturns;
    }
    if (frame.symbol == m_grammar.symSwitchStatement)
    {
        return frame.hasDefault && !frame.hasEscapingBreak && (frame.allChildrenReturn || frame.defaultReturns);
    }
    return frame.symbol == m_grammar.symTryStatement && frame.namedChildren != 0 && frame.allChildrenReturn;
}

bool NodeIndex::FlowFactCollector::Advance(TSTreeCursor& cursor, bool full)
{
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        return true;
    }
    do
    {
        if (full)
        {
            Leave();
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            return true;
        }
    } while (ts_tree_cursor_goto_parent(&cursor));
    return false;
}

static void MaintainActiveContainers(std::vector<uint32_t>& activeContainers,
                                     std::span<const IndexedContainerSpan> spans, uint32_t startByte)
{
    while (!activeContainers.empty() && startByte >= spans[activeContainers.back()].endByte)
    {
        activeContainers.pop_back();
    }
}

/**
 * @brief Traverses the AST tree iteratively using a flat cursor and indexes nodes of interest.
 * @param[in] root Root AST node to traverse.
 * @param[in] budget Traversal budget guard to prevent runaway recursion or unbounded visits.
 */
void NodeIndex::TraverseTree(TSNode root, TraversalBudget* budget, bool full)
{
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    std::vector<uint32_t> activeContainers;
    FlowFactCollector flow(*this);

    while (true)
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        MaintainActiveContainers(activeContainers, m_containerSpans, ts_node_start_byte(node));

        const uint32_t parentSpanIdx = activeContainers.empty() ? UINT32_MAX : activeContainers.back();
        const size_t containerCount = m_containerSpans.size();
        const size_t syntaxSlot = IndexNode(node, budget, parentSpanIdx, full);
        if (full)
        {
            flow.Enter(node, cursor, syntaxSlot);
        }
        if (m_containerSpans.size() > containerCount)
        {
            activeContainers.push_back(static_cast<uint32_t>(containerCount));
        }

        if (!flow.Advance(cursor, full))
        {
            break;
        }
    }

    ts_tree_cursor_delete(&cursor);
    flow.FinishUsingScopes();
    if (m_flowFacts)
    {
        std::erase(m_flowFacts->returningBodies, UINT32_MAX);
    }
}

bool NodeIndex::CallableBodyDefinitelyReturns(uint32_t bodyStartByte) const noexcept
{
    return m_flowFacts && std::ranges::binary_search(m_flowFacts->returningBodies, bodyStartByte);
}

std::span<const TSNode> NodeIndex::SwitchLabels(uint32_t switchStartByte) const noexcept
{
    if (!m_flowFacts)
    {
        return {};
    }
    const auto& switches = m_flowFacts->switches;
    const auto entry = std::ranges::lower_bound(switches, switchStartByte, {}, &IndexedSwitch::startByte);
    if (entry == switches.end() || entry->startByte != switchStartByte)
    {
        return {};
    }
    return entry->labels;
}

void NodeIndex::Build(TSNode root, const TSLanguage* lang, TraversalBudget* budget, NodeIndexContents contents)
{
    Clear();
    m_root = root;
    if (ts_node_is_null(root))
    {
        return;
    }

    m_hasSyntaxErrors = ts_node_has_error(root) || ts_node_is_error(root);
    m_language = lang ? lang : tree_sitter_angelscript();
    PopulatePredefinedSymbols(m_language);
    m_symComment = SymbolForName("comment");
    TraverseTree(root, budget, contents == NodeIndexContents::Full);
    PostProcessErrorRanges();
    PostProcessCommentRanges();
}

std::span<const TSNode> NodeIndex::Nodes(TSSymbol symbol) const noexcept
{
    if (symbol == parser::GetGrammarSymbols().symError || symbol == static_cast<TSSymbol>(-1))
    {
        return m_errorNodes;
    }
    if (static_cast<size_t>(symbol) < m_nodesBySymbol.size())
    {
        return m_nodesBySymbol[symbol];
    }
    return {};
}

std::span<const TSNode> NodeIndex::Nodes(std::string_view typeName) const noexcept
{
    if (typeName == "ERROR")
    {
        return m_errorNodes;
    }
    const auto& map = GetSymbolNameMap();
    const auto it = map.find(typeName);
    if (it != map.end())
    {
        return Nodes(it->second);
    }
    return {};
}

TSSymbol NodeIndex::SymbolForName(std::string_view typeName) const noexcept
{
    const auto& map = GetSymbolNameMap();
    const auto it = map.find(typeName);
    if (it != map.end())
    {
        return it->second;
    }
    return 0;
}

TSNode NodeIndex::Root() const noexcept
{
    return m_root;
}

size_t NodeIndex::NodeCount() const noexcept
{
    return m_nodeCount;
}

bool NodeIndex::Empty() const noexcept
{
    return m_nodeCount == 0;
}

template <typename RangeType> void MergeSortedRanges(std::vector<RangeType>& ranges)
{
    if (ranges.size() <= 1)
    {
        return;
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const RangeType& a, const RangeType& b) { return a.startByte < b.startByte; });
    std::vector<RangeType> merged;
    merged.reserve(ranges.size());
    merged.push_back(ranges.front());
    for (size_t i = 1; i < ranges.size(); ++i)
    {
        auto& last = merged.back();
        if (ranges[i].startByte <= last.endByte)
        {
            if (ranges[i].endByte > last.endByte)
            {
                last.endByte = ranges[i].endByte;
                if constexpr (requires { last.endLine; })
                {
                    last.endLine = ranges[i].endLine;
                    last.endColumn = ranges[i].endColumn;
                }
            }
        }
        else
        {
            merged.push_back(ranges[i]);
        }
    }
    ranges = std::move(merged);
}

template <typename RangeType> bool IsByteInRanges(std::span<const RangeType> ranges, uint32_t byteOffset) noexcept
{
    if (ranges.empty())
    {
        return false;
    }
    auto it = std::upper_bound(ranges.begin(), ranges.end(), byteOffset,
                               [](uint32_t val, const RangeType& r) { return val < r.startByte; });
    if (it == ranges.begin())
    {
        return false;
    }
    --it;
    return byteOffset >= it->startByte && byteOffset < it->endByte;
}

void NodeIndex::PostProcessErrorRanges()
{
    MergeSortedRanges(m_errorRanges);
}

void NodeIndex::PostProcessCommentRanges()
{
    MergeSortedRanges(m_commentRanges);
}

bool NodeIndex::IsInsideError(uint32_t byteOffset) const noexcept
{
    return IsByteInRanges(std::span<const ErrorRange>(m_errorRanges), byteOffset);
}

bool NodeIndex::IsInsideComment(uint32_t byteOffset) const noexcept
{
    return IsByteInRanges(std::span<const CommentRange>(m_commentRanges), byteOffset);
}

bool NodeIndex::IsInsideCommentLineCol(uint32_t line, uint32_t col) const noexcept
{
    if (m_commentRanges.empty())
    {
        return false;
    }
    auto it = std::upper_bound(m_commentRanges.begin(), m_commentRanges.end(), line,
                               [](uint32_t val, const CommentRange& r) { return val < r.startLine; });
    if (it == m_commentRanges.begin())
    {
        return false;
    }
    --it;
    if (line < it->startLine || line > it->endLine)
    {
        return false;
    }
    if (line == it->startLine && col < it->startColumn)
    {
        return false;
    }
    if (line == it->endLine && col >= it->endColumn)
    {
        return false;
    }
    return true;
}

std::span<const NodeIndex::CommentRange> NodeIndex::CommentRanges() const noexcept
{
    return m_commentRanges;
}

bool NodeIndex::NodeHasSyntaxError(TSNode node) const noexcept
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    return ts_node_has_error(node) || IsInsideError(ts_node_start_byte(node));
}

std::span<const NodeIndex::ErrorRange> NodeIndex::ErrorRanges() const noexcept
{
    return m_errorRanges;
}

void NodeIndex::Clear() noexcept
{
    m_root = TSNode{};
    m_hasSyntaxErrors = false;
    m_language = nullptr;
    m_nodeCount = 0;
    m_errorNodes.clear();
    m_nodesBySymbol.clear();
    m_errorRanges.clear();
    m_commentRanges.clear();
    m_containerSpans.clear();
    m_flowFacts.reset();
    m_symComment = 0;
}

TSNode NodeIndex::FindEnclosingNode(uint32_t byteOffset, TSSymbol targetSym) const noexcept
{
    TSNode bestNode{};
    uint32_t bestSpan = UINT32_MAX;
    for (TSNode node : Nodes(targetSym))
    {
        const uint32_t start = ts_node_start_byte(node);
        if (start > byteOffset)
        {
            break;
        }
        const uint32_t end = ts_node_end_byte(node);
        if (byteOffset < end)
        {
            const uint32_t span = end - start;
            if (span < bestSpan)
            {
                bestSpan = span;
                bestNode = node;
            }
        }
    }
    return bestNode;
}

TSNode NodeIndex::FindEnclosingNodeAny(uint32_t byteOffset, std::span<const TSSymbol> targetSyms) const noexcept
{
    TSNode bestNode{};
    uint32_t bestSpan = UINT32_MAX;
    for (TSSymbol sym : targetSyms)
    {
        for (TSNode node : Nodes(sym))
        {
            const uint32_t start = ts_node_start_byte(node);
            if (start > byteOffset)
            {
                break;
            }
            const uint32_t end = ts_node_end_byte(node);
            if (byteOffset < end)
            {
                const uint32_t span = end - start;
                if (span < bestSpan)
                {
                    bestSpan = span;
                    bestNode = node;
                }
            }
        }
    }
    return bestNode;
}

TSNode NodeIndex::FindEnclosingClassOrMixin(uint32_t byteOffset) const noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol targets[] = {gs.symClassDeclaration, gs.symInterfaceDeclaration, gs.symMixinDeclaration};
    return FindEnclosingNodeAny(byteOffset, targets);
}

/// The last span to start at the position can only be enclosed by its recorded ancestors.
template <typename Span> static const Span* FindInnermostSpan(std::span<const Span> spans, uint32_t byteOffset)
{
    const auto upper = std::ranges::upper_bound(spans, byteOffset, {}, &Span::startByte);
    if (upper == spans.begin())
    {
        return nullptr;
    }
    const Span* span = &*(upper - 1);
    while (byteOffset >= span->endByte)
    {
        if (span->parentSpanIndex >= spans.size())
        {
            return nullptr;
        }
        span = &spans[span->parentSpanIndex];
    }
    return span;
}

const IndexedContainerSpan* NodeIndex::FindInnermostClassSpan(uint32_t byteOffset) const noexcept
{
    const auto* span = FindInnermostSpan(std::span<const IndexedContainerSpan>(m_containerSpans), byteOffset);
    const auto& gs = parser::GetGrammarSymbols();
    return span && (span->symbol == gs.symClassDeclaration || span->symbol == gs.symMixinDeclaration) ? span : nullptr;
}

TSNode NodeIndex::FindEnclosingCallable(uint32_t byteOffset) const noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol targets[] = {gs.symFuncDeclaration, gs.symLambdaExpression};
    return FindEnclosingNodeAny(byteOffset, targets);
}

bool NodeIndex::IsInsideLambdaClosure(uint32_t byteOffset) const noexcept
{
    const TSNode callable = FindEnclosingCallable(byteOffset);
    return !ts_node_is_null(callable) && ts_node_symbol(callable) == parser::GetGrammarSymbols().symLambdaExpression;
}

/**
 * @brief Classify index container kind.
 * @param[in] sym Tree-sitter grammar symbol.
 * @return Found item if present; std::nullopt otherwise.
 */

std::optional<ContainerKind> ClassifyContainerKind(TSSymbol sym) noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symClassDeclaration || sym == gs.symMixinDeclaration)
    {
        return ContainerKind::Class;
    }
    if (sym == gs.symInterfaceDeclaration)
    {
        return ContainerKind::Interface;
    }
    if (sym == gs.symNamespaceDeclaration)
    {
        return ContainerKind::Namespace;
    }
    return std::nullopt;
}

/**
 * @brief Extracts container name and byte range from a name AST node and appends a ContainerInfo entry.
 * @param[in] nameNode AST node of the container name identifier.
 * @param[in] kind Categorical ContainerKind (e.g. Class, Interface, Namespace).
 * @param[in] sourceCode Document source text buffer.
 * @param[in,out] rawContainers Output vector accumulating parsed container definitions.
 */
void AppendRawContainers(TSNode nameNode, ContainerKind kind, std::string_view sourceCode,
                         std::vector<ContainerInfo>& rawContainers)
{
    if (ts_node_is_null(nameNode))
    {
        return;
    }
    const uint32_t start = ts_node_start_byte(nameNode);
    const uint32_t end = ts_node_end_byte(nameNode);
    if (start >= end || end > sourceCode.size())
    {
        return;
    }

    std::string text(sourceCode.substr(start, end - start));
    if (kind == ContainerKind::Namespace && utils::ContainsScopeSeparator(text))
    {
        auto segs = SplitScopeSegments(text);
        for (auto it = segs.rbegin(); it != segs.rend(); ++it)
        {
            if (!it->empty())
            {
                rawContainers.push_back({*it, "", kind});
            }
        }
    }
    else
    {
        rawContainers.push_back({std::move(text), "", kind});
    }
}

/**
 * @brief Compute index container qualified names.
 * @param[in,out] containers Vector of ContainerInfo structs whose qualified names are computed.
 */
void ComputeContainerQualifiedNames(std::vector<ContainerInfo>& containers)
{
    std::string qualified;
    for (auto it = containers.rbegin(); it != containers.rend(); ++it)
    {
        if (!qualified.empty())
        {
            qualified += "::";
        }
        qualified += it->name;
        it->qualifiedName = qualified;
    }
}

static void AppendContainerFromSpan(const IndexedContainerSpan& span, ContainerKind kind, std::string_view sourceCode,
                                    std::vector<ContainerInfo>& rawContainers)
{
    if (span.nameStartByte >= span.nameEndByte || span.nameEndByte > sourceCode.size())
    {
        return;
    }
    std::string text(sourceCode.substr(span.nameStartByte, span.nameEndByte - span.nameStartByte));
    if (kind == ContainerKind::Namespace && utils::ContainsScopeSeparator(text))
    {
        auto segs = SplitScopeSegments(text);
        for (auto rit = segs.rbegin(); rit != segs.rend(); ++rit)
        {
            if (!rit->empty())
            {
                rawContainers.push_back({*rit, "", kind});
            }
        }
    }
    else
    {
        rawContainers.push_back({std::move(text), "", kind});
    }
}

std::vector<ContainerInfo> NodeIndex::CollectContainersFrom(const IndexedContainerSpan* span,
                                                            std::string_view sourceCode) const
{
    std::vector<ContainerInfo> rawContainers;
    const auto& gs = parser::GetGrammarSymbols();
    bool crossedLambda = false;
    while (span)
    {
        if (span->symbol == gs.symLambdaExpression)
        {
            crossedLambda = true;
        }
        else if (const auto kind = ClassifyContainerKind(span->symbol))
        {
            if (!(crossedLambda && (*kind == ContainerKind::Class || *kind == ContainerKind::Interface)))
            {
                AppendContainerFromSpan(*span, *kind, sourceCode, rawContainers);
            }
        }
        span = span->parentSpanIndex == UINT32_MAX ? nullptr : &m_containerSpans[span->parentSpanIndex];
    }
    ComputeContainerQualifiedNames(rawContainers);
    return rawContainers;
}

std::vector<ContainerInfo> NodeIndex::CollectEnclosingContainers(uint32_t byteOffset, std::string_view sourceCode) const
{
    return CollectContainersFrom(FindInnermostSpan(std::span<const IndexedContainerSpan>(m_containerSpans), byteOffset),
                                 sourceCode);
}

bool NodeIndex::IsInsideBaseClassList(uint32_t startByte, uint32_t endByte) const noexcept
{
    if (!m_flowFacts)
    {
        return false;
    }
    const auto& lists = m_flowFacts->baseClassLists;
    const auto upper = std::ranges::upper_bound(lists, startByte, {}, &ErrorRange::startByte);
    return upper != lists.begin() && startByte < (upper - 1)->endByte && endByte <= (upper - 1)->endByte;
}

std::vector<ContainerInfo> NodeIndex::CollectLookupContainers(uint32_t startByte, uint32_t endByte, TSSymbol symbol,
                                                              std::string_view sourceCode) const
{
    const auto& gs = parser::GetGrammarSymbols();
    if (symbol == gs.symScript)
    {
        return {};
    }

    auto* span = FindInnermostSpan(std::span<const IndexedContainerSpan>(m_containerSpans), startByte);
    while (span && endByte > span->endByte)
    {
        span = span->parentSpanIndex == UINT32_MAX ? nullptr : &m_containerSpans[span->parentSpanIndex];
    }
    if (span)
    {
        const bool onDeclaration = span->symbol == symbol && span->startByte == startByte && span->endByte == endByte;
        const bool onName =
            span->nameStartByte == startByte && span->nameEndByte == endByte && span->nameStartByte < span->nameEndByte;
        const bool onBaseClass = symbol != gs.symBaseClassList && IsInsideBaseClassList(startByte, endByte);
        if (onDeclaration || onName || onBaseClass)
        {
            span = span->parentSpanIndex == UINT32_MAX ? nullptr : &m_containerSpans[span->parentSpanIndex];
        }
    }
    return CollectContainersFrom(span, sourceCode);
}

static void AppendUsingName(std::vector<std::string>& names, std::string_view sourceCode, uint32_t start, uint32_t end)
{
    if (start < end && end <= sourceCode.size())
    {
        const auto text = parser::Trim(sourceCode.substr(start, end - start));
        if (!text.empty())
        {
            names.emplace_back(text);
        }
    }
}

std::vector<std::string> NodeIndex::CollectUsingNamespaces(std::string_view sourceCode,
                                                           std::optional<uint32_t> byteOffset) const
{
    std::vector<std::string> result;
    if (!m_flowFacts)
    {
        return result;
    }
    const auto& names = m_flowFacts->usingNames;
    if (!byteOffset)
    {
        result.reserve(names.size());
        for (const auto& entry : names)
        {
            AppendUsingName(result, sourceCode, entry.name.startByte, entry.name.endByte);
        }
        return result;
    }
    const auto& scopes = m_flowFacts->usingScopes;
    auto* scope = FindInnermostSpan(std::span<const IndexedUsingScope>(scopes), *byteOffset);
    for (; scope; scope = scope->parentSpanIndex == SIZE_MAX ? nullptr : &scopes[scope->parentSpanIndex])
    {
        for (size_t id = scope->firstName; id != SIZE_MAX; id = names[id].next)
        {
            AppendUsingName(result, sourceCode, names[id].name.startByte, names[id].name.endByte);
        }
    }
    return result;
}
} // namespace angel_lsp::analysis
