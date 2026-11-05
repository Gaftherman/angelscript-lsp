#include "SymbolTable.h"

#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "spdlog/fmt/fmt.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <functional>
#include <iterator>

namespace angel_lsp::analysis
{
SymbolTable::SymbolTable() = default;

SymbolTable::~SymbolTable() = default;

static std::vector<Symbol>& MutableBucket(std::shared_ptr<std::vector<Symbol>>& bucket)
{
    if (!bucket)
        bucket = std::make_shared<std::vector<Symbol>>();
    else if (bucket.use_count() > 1)
        bucket = std::make_shared<std::vector<Symbol>>(*bucket);
    return *bucket;
}

void SymbolTable::IndexKeyForFileLocked(const std::string& fileUri, const std::string& key)
{
    m_keysByFile[fileUri].insert(key);
}

void SymbolTable::EraseDocumentSymbolsOnlyLocked(const std::string& fileUri)
{
    const auto fileEntry = m_keysByFile.find(fileUri);
    if (fileEntry == m_keysByFile.end())
    {
        return;
    }

    for (const auto& key : fileEntry->second)
    {
        const auto bucket = m_symbols.find(key);
        if (bucket == m_symbols.end())
        {
            continue;
        }

        auto& vec = MutableBucket(bucket->second);
        for (const auto& sym : vec)
        {
            if (sym.fileUri == fileUri && sym.type == SymbolType::Class &&
                std::holds_alternative<ClassSignature>(sym.signature) && sym.GetClass().modifiers.isMixin)
            {
                if (m_mixinClassCount > 0)
                {
                    --m_mixinClassCount;
                }
            }
        }
        std::erase_if(vec, [&fileUri](const Symbol& sym) { return sym.fileUri == fileUri || sym.isSynthesized; });

        if (vec.empty())
        {
            m_symbols.erase(bucket);
        }
    }

    m_keysByFile.erase(fileEntry);
}

std::vector<const Symbol*> SymbolTable::GetDocumentSymbolPointersLocked(const std::string& fileUri) const
{
    std::vector<const Symbol*> result;
    const auto fileEntry = m_keysByFile.find(fileUri);
    if (fileEntry == m_keysByFile.end())
    {
        return result;
    }

    for (const auto& key : fileEntry->second)
    {
        const auto bucket = m_symbols.find(key);
        if (bucket == m_symbols.end() || !bucket->second)
        {
            continue;
        }

        for (const auto& sym : *bucket->second)
        {
            if (sym.fileUri == fileUri && !sym.isSynthesized)
            {
                result.push_back(&sym);
            }
        }
    }
    return result;
}

void SymbolTable::EraseDocumentLocked(const std::string& fileUri)
{
    EraseDocumentSymbolsOnlyLocked(fileUri);

    if (m_ruleIndexPartials)
    {
        auto partIt = m_ruleIndexPartials->find(fileUri);
        if (partIt != m_ruleIndexPartials->end())
        {
            std::lock_guard<std::mutex> guard(m_ruleIndexMutex);
            if (m_ruleIndex)
            {
                if (m_ruleIndex.use_count() > 1)
                {
                    m_ruleIndex = std::make_shared<rules::RuleIndex>(*m_ruleIndex);
                }
                m_ruleIndex->RemovePartial(partIt->second);
            }
            m_ruleIndexPartials->erase(partIt);
        }
    }
}

bool SymbolTable::HasMixinSymbolLocked(const std::string& fileUri) const
{
    if (m_mixinClassCount == 0)
    {
        return false;
    }

    const auto fileEntry = m_keysByFile.find(fileUri);
    if (fileEntry == m_keysByFile.end())
    {
        return false;
    }

    for (const auto& key : fileEntry->second)
    {
        const auto bucket = m_symbols.find(key);
        if (bucket != m_symbols.end() && bucket->second)
        {
            for (const auto& sym : *bucket->second)
            {
                if (sym.fileUri == fileUri && sym.type == SymbolType::Class &&
                    std::holds_alternative<ClassSignature>(sym.signature) && sym.GetClass().modifiers.isMixin)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

std::vector<std::pair<std::string, rules::RuleIndexPartial>>
SymbolTable::BuildAffectedPartialsLocked(const std::vector<std::string>& affectedFiles,
                                         const std::string& excludeUri) const
{
    ankerl::unordered_dense::set<std::string> uniqueAffected;
    for (const auto& affectedUri : affectedFiles)
    {
        if (affectedUri != excludeUri && !affectedUri.empty())
        {
            uniqueAffected.insert(affectedUri);
        }
    }

    std::vector<std::pair<std::string, rules::RuleIndexPartial>> affectedPartials;
    affectedPartials.reserve(uniqueAffected.size());
    for (const auto& affectedUri : uniqueAffected)
    {
        auto affectedPtrs = GetDocumentSymbolPointersLocked(affectedUri);
        affectedPartials.emplace_back(affectedUri, rules::RuleIndex::BuildPartial(affectedUri, affectedPtrs));
    }
    return affectedPartials;
}

void SymbolTable::ApplySingleSymbolPartialLocked(const Symbol& symbol)
{
    const Symbol* symPtr = &symbol;
    rules::RuleIndexPartial singlePartial =
        rules::RuleIndex::BuildPartial(symbol.fileUri, std::vector<const Symbol*>{symPtr});

    std::lock_guard<std::mutex> guard(m_ruleIndexMutex);
    if (!m_ruleIndex)
    {
        m_ruleIndex = std::make_shared<rules::RuleIndex>();
    }
    else if (m_ruleIndex.use_count() > 1)
    {
        m_ruleIndex = std::make_shared<rules::RuleIndex>(*m_ruleIndex);
    }

    if (!m_ruleIndexPartials)
    {
        m_ruleIndexPartials = std::make_unique<ankerl::unordered_dense::map<std::string, rules::RuleIndexPartial>>();
    }

    m_ruleIndex->ApplyPartial(singlePartial);
    auto& filePartial = (*m_ruleIndexPartials)[symbol.fileUri];
    filePartial.fileUri = symbol.fileUri;
    filePartial.Merge(std::move(singlePartial));
}

void SymbolTable::ApplyRuleIndexPartialsLocked(
    const std::string& fileUri, std::optional<rules::RuleIndexPartial> freshPartial,
    std::vector<std::pair<std::string, rules::RuleIndexPartial>> affectedPartials)
{
    std::lock_guard<std::mutex> guard(m_ruleIndexMutex);
    if (!freshPartial && (!m_ruleIndex || !m_ruleIndexPartials))
    {
        return;
    }

    if (!m_ruleIndex)
    {
        m_ruleIndex = std::make_shared<rules::RuleIndex>();
    }
    else if (m_ruleIndex.use_count() > 1)
    {
        m_ruleIndex = std::make_shared<rules::RuleIndex>(*m_ruleIndex);
    }

    if (!m_ruleIndexPartials)
    {
        m_ruleIndexPartials = std::make_unique<ankerl::unordered_dense::map<std::string, rules::RuleIndexPartial>>();
    }

    auto oldIt = m_ruleIndexPartials->find(fileUri);
    if (oldIt != m_ruleIndexPartials->end())
    {
        if (m_ruleIndex)
        {
            m_ruleIndex->RemovePartial(oldIt->second);
        }
        if (freshPartial)
        {
            m_ruleIndex->ApplyPartial(*freshPartial);
            oldIt->second = std::move(*freshPartial);
        }
        else
        {
            m_ruleIndexPartials->erase(oldIt);
        }
    }
    else if (freshPartial)
    {
        m_ruleIndex->ApplyPartial(*freshPartial);
        (*m_ruleIndexPartials)[fileUri] = std::move(*freshPartial);
    }

    for (auto& [affectedUri, newPartial] : affectedPartials)
    {
        auto affIt = m_ruleIndexPartials->find(affectedUri);
        if (affIt != m_ruleIndexPartials->end())
        {
            if (m_ruleIndex)
            {
                m_ruleIndex->RemovePartial(affIt->second);
            }
        }
        if (m_ruleIndex)
        {
            m_ruleIndex->ApplyPartial(newPartial);
        }
        (*m_ruleIndexPartials)[affectedUri] = std::move(newPartial);
    }
}

void SymbolTable::AddSymbol(const Symbol& symbol)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    const std::string& key = symbol.qualifiedName.empty() ? symbol.name : symbol.qualifiedName;
    auto& bucket = MutableBucket(m_symbols[key]);
    bucket.push_back(symbol);
    Symbol& stored = bucket.back();
    if (stored.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(stored.signature) &&
        stored.GetFunction().displaySignature.empty())
    {
        stored.GetFunction().displaySignature = FormatFunctionDeclaration(stored, true);
    }
    IndexKeyForFileLocked(symbol.fileUri, key);

    const bool isClass = (symbol.type == SymbolType::Class && std::holds_alternative<ClassSignature>(symbol.signature));
    const bool isMixin = isClass && symbol.GetClass().modifiers.isMixin;

    std::vector<std::string> affectedFiles;
    if (isMixin)
    {
        ++m_mixinClassCount;
        ResolveIncludedMixinsLocked(&affectedFiles);
    }
    else if (isClass)
    {
        ResolveIncludedMixinsForKeysLocked({key}, &affectedFiles);
    }

    if (!m_ruleIndex && !m_ruleIndexPartials)
    {
        ++m_version;
        return;
    }

    if (!isMixin && affectedFiles.empty())
    {
        ApplySingleSymbolPartialLocked(symbol);
        ++m_version;
        return;
    }

    auto freshPtrs = GetDocumentSymbolPointersLocked(symbol.fileUri);
    auto freshPartial = rules::RuleIndex::BuildPartial(symbol.fileUri, freshPtrs);
    auto affectedPartials = BuildAffectedPartialsLocked(affectedFiles, symbol.fileUri);
    ApplyRuleIndexPartialsLocked(symbol.fileUri, std::move(freshPartial), std::move(affectedPartials));

    ++m_version;
}

void SymbolTable::InsertSymbolsLocked(std::vector<Symbol>& symbols, std::vector<std::string>& freshClassKeys,
                                      bool& addedMixin, std::string& fileUri)
{
    for (auto& symbol : symbols)
    {
        if (fileUri.empty() && !symbol.fileUri.empty())
        {
            fileUri = symbol.fileUri;
        }

        const std::string& lookupKey = symbol.qualifiedName.empty() ? symbol.name : symbol.qualifiedName;
        auto& bucket = MutableBucket(m_symbols[lookupKey]);
        bucket.push_back(std::move(symbol));
        Symbol& stored = bucket.back();
        const std::string& key = stored.qualifiedName.empty() ? stored.name : stored.qualifiedName;

        if (stored.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(stored.signature) &&
            stored.GetFunction().displaySignature.empty())
        {
            stored.GetFunction().displaySignature = FormatFunctionDeclaration(stored, true);
        }
        IndexKeyForFileLocked(stored.fileUri, key);

        const bool isClass =
            (stored.type == SymbolType::Class && std::holds_alternative<ClassSignature>(stored.signature));
        if (isClass)
        {
            if (stored.GetClass().modifiers.isMixin)
            {
                addedMixin = true;
                ++m_mixinClassCount;
            }
            freshClassKeys.push_back(key);
        }
    }
}

void SymbolTable::AddSymbols(std::vector<Symbol>&& symbols)
{
    if (symbols.empty())
    {
        return;
    }

    std::unique_lock<std::shared_mutex> lock(m_mutex);
    bool addedMixin = false;
    std::vector<std::string> freshClassKeys;
    std::string fileUri;

    InsertSymbolsLocked(symbols, freshClassKeys, addedMixin, fileUri);

    std::vector<std::string> affectedFiles;
    if (addedMixin)
    {
        ResolveIncludedMixinsLocked(&affectedFiles);
    }
    else if (!freshClassKeys.empty())
    {
        ResolveIncludedMixinsForKeysLocked(freshClassKeys, &affectedFiles);
    }

    {
        std::lock_guard<std::mutex> guard(m_ruleIndexMutex);
        if (!m_ruleIndex && !m_ruleIndexPartials)
        {
            ++m_version;
            return;
        }
    }

    if (!fileUri.empty())
    {
        auto freshPtrs = GetDocumentSymbolPointersLocked(fileUri);
        auto freshPartial = rules::RuleIndex::BuildPartial(fileUri, freshPtrs);
        auto affectedPartials = BuildAffectedPartialsLocked(affectedFiles, fileUri);
        ApplyRuleIndexPartialsLocked(fileUri, std::move(freshPartial), std::move(affectedPartials));
    }

    ++m_version;
}

void SymbolTable::ClearDocumentSymbols(const std::string& fileUri)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    const bool erasedMixin = HasMixinSymbolLocked(fileUri);
    EraseDocumentSymbolsOnlyLocked(fileUri);

    std::vector<std::string> affectedFiles;
    if (erasedMixin)
    {
        ResolveIncludedMixinsLocked(&affectedFiles);
    }

    auto affectedPartials = BuildAffectedPartialsLocked(affectedFiles, fileUri);
    ApplyRuleIndexPartialsLocked(fileUri, std::nullopt, std::move(affectedPartials));

    ++m_version;
}

void SymbolTable::ReplaceDocumentSymbols(const std::string& fileUri, const SymbolTable& staging)
{
    // Snapshot staging first: it is a separate object with its own lock, and reading it while
    // holding this table's write lock is what keeps the replacement a single atomic step.
    std::vector<Symbol> fresh;
    staging.ForEachSymbol(
        [&fresh]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& symbols)
        { fresh.insert(fresh.end(), symbols.begin(), symbols.end()); });

    PublishDocumentSymbols(fileUri, std::move(fresh));
}

void SymbolTable::ReplaceDocumentSymbols(const std::string& fileUri, SymbolTable&& staging)
{
    std::vector<Symbol> fresh;

    // Read without taking staging's lock, and through its members rather than ForEachSymbol:
    // an rvalue reference is the caller stating that nothing else holds this table, which is
    // exactly the condition that makes both of those safe. It is left empty.
    for (auto& [key, bucket] : staging.m_symbols)
    {
        if (!bucket)
            continue;

        std::vector<Symbol>& symbols = MutableBucket(bucket);
        fresh.insert(fresh.end(), std::make_move_iterator(symbols.begin()), std::make_move_iterator(symbols.end()));
    }
    staging.m_symbols.clear();

    PublishDocumentSymbols(fileUri, std::move(fresh));
}

static bool ShouldRetainSymbol(const Symbol& sym, const std::string& fileUri,
                               const ankerl::unordered_dense::set<std::string>& stagingTypeNames)
{
    if (sym.fileUri != fileUri)
        return false;

    const bool isTopLevelType = (sym.type == SymbolType::Class || sym.type == SymbolType::Interface ||
                                 sym.type == SymbolType::Enum || sym.type == SymbolType::Funcdef);
    if (isTopLevelType)
        return !stagingTypeNames.contains(sym.name);

    if (!sym.containerName.empty())
        return !stagingTypeNames.contains(sym.containerName);

    return false;
}

void SymbolTable::RetainLastKnownGoodTypes(const std::string& fileUri, SymbolTable& staging) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    const auto fileEntry = m_keysByFile.find(fileUri);
    if (fileEntry == m_keysByFile.end())
        return;

    ankerl::unordered_dense::set<std::string> stagingTypeNames;
    staging.ForEachSymbolInFile(
        fileUri,
        [&stagingTypeNames]([[maybe_unused]] const std::string& key, const std::vector<Symbol>& syms)
        {
            for (const auto& s : syms)
            {
                if (s.type == SymbolType::Class || s.type == SymbolType::Interface || s.type == SymbolType::Enum ||
                    s.type == SymbolType::Funcdef)
                {
                    stagingTypeNames.insert(s.name);
                }
            }
        });

    std::vector<Symbol> toRetain;
    for (const auto& key : fileEntry->second)
    {
        const auto bucket = m_symbols.find(key);
        if (bucket == m_symbols.end() || !bucket->second)
            continue;

        for (const auto& sym : *bucket->second)
        {
            if (ShouldRetainSymbol(sym, fileUri, stagingTypeNames))
            {
                toRetain.push_back(sym);
            }
        }
    }

    if (!toRetain.empty())
    {
        staging.AddSymbols(std::move(toRetain));
    }
}

bool SymbolTable::IsFileAllowedForSnapshot(std::string_view fileUri, const SnapshotFilterContext& ctx) noexcept
{
    if (fileUri.empty() || fileUri == ctx.targetUri)
    {
        return true;
    }
    if (ctx.allowedModuleFileUris.contains(std::string(fileUri)))
    {
        return true;
    }
    return utils::IsPredefinedFile(fileUri, ctx.predefinedExtension);
}

void SymbolTable::CopyBucketForSnapshotLocked(const std::string& key,
                                              const std::shared_ptr<std::vector<Symbol>>& bucketPtr,
                                              const SnapshotFilterContext& ctx)
{
    if (!bucketPtr)
    {
        return;
    }
    const auto& bucket = *bucketPtr;
    const bool allAllowed = std::all_of(bucket.begin(), bucket.end(),
                                        [&](const Symbol& sym) { return IsFileAllowedForSnapshot(sym.fileUri, ctx); });

    if (allAllowed)
    {
        m_symbols.emplace(key, bucketPtr);
        return;
    }

    auto filtered = std::make_shared<std::vector<Symbol>>();
    for (const auto& sym : bucket)
    {
        if (IsFileAllowedForSnapshot(sym.fileUri, ctx))
        {
            filtered->push_back(sym);
        }
    }
    if (!filtered->empty())
    {
        m_symbols.emplace(key, std::move(filtered));
    }
}

void SymbolTable::CopyFilteredSymbolsLocked(SymbolTable& target, const SnapshotFilterContext& ctx) const
{
    for (const auto& [fUri, keys] : m_keysByFile)
    {
        if (!IsFileAllowedForSnapshot(fUri, ctx))
        {
            continue;
        }

        target.m_keysByFile[fUri] = keys;
        for (const auto& key : keys)
        {
            if (target.m_symbols.contains(key))
            {
                continue;
            }

            const auto it = m_symbols.find(key);
            if (it != m_symbols.end())
            {
                target.CopyBucketForSnapshotLocked(key, it->second, ctx);
            }
        }
    }
}

using SymbolBucketMap = ankerl::unordered_dense::map<std::string, std::shared_ptr<std::vector<Symbol>>,
                                                     TransparentStringHash, std::equal_to<>>;

static bool IsPredefinedOrBuiltinUri(std::string_view fileUri, std::string_view effectiveExt) noexcept
{
    return utils::IsPredefinedFile(fileUri, effectiveExt);
}

static void CopyPredefinedBucket(const std::string& key, const std::shared_ptr<std::vector<Symbol>>& bucket,
                                 std::string_view effectiveExt, SymbolBucketMap& dest)
{
    if (!bucket)
    {
        return;
    }
    bool allPredefined = true;
    for (const auto& sym : *bucket)
    {
        if (!sym.isPredefined && !IsPredefinedOrBuiltinUri(sym.fileUri, effectiveExt))
        {
            allPredefined = false;
            break;
        }
    }
    if (allPredefined)
    {
        dest.emplace(key, bucket);
        return;
    }
    auto filtered = std::make_shared<std::vector<Symbol>>();
    for (const auto& sym : *bucket)
    {
        if (sym.isPredefined || IsPredefinedOrBuiltinUri(sym.fileUri, effectiveExt))
        {
            filtered->push_back(sym);
        }
    }
    if (!filtered->empty())
    {
        dest.emplace(key, std::move(filtered));
    }
}

void SymbolTable::ClonePredefinedBaseFrom(const SymbolTable& source, std::string_view predefinedExtension)
{
    std::shared_lock<std::shared_mutex> lock(source.m_mutex);
    m_version = source.m_version;
    m_defaultArrayType = source.m_defaultArrayType;
    m_stringType = source.m_stringType;
    m_predefinedFileExtension = source.m_predefinedFileExtension;

    const std::string_view effectiveExt =
        predefinedExtension.empty() ? std::string_view(m_predefinedFileExtension) : predefinedExtension;

    for (const auto& [fUri, keys] : source.m_keysByFile)
    {
        if (!IsPredefinedOrBuiltinUri(fUri, effectiveExt))
        {
            continue;
        }
        m_keysByFile[fUri] = keys;
        for (const auto& key : keys)
        {
            if (m_symbols.contains(key))
            {
                continue;
            }
            const auto it = source.m_symbols.find(key);
            if (it != source.m_symbols.end())
            {
                CopyPredefinedBucket(key, it->second, effectiveExt, m_symbols);
            }
        }
    }

    std::lock_guard<std::mutex> ruleGuard(source.m_ruleIndexMutex);
    if (source.m_ruleIndexPartials)
    {
        m_ruleIndexPartials = std::make_unique<ankerl::unordered_dense::map<std::string, rules::RuleIndexPartial>>();
        auto index = std::make_shared<rules::RuleIndex>();
        for (const auto& [fUri, partial] : *source.m_ruleIndexPartials)
        {
            if (IsPredefinedOrBuiltinUri(fUri, effectiveExt))
            {
                (*m_ruleIndexPartials)[fUri] = partial;
                index->ApplyPartial(partial);
            }
        }
        m_ruleIndex = std::move(index);
    }
}

void SymbolTable::CopyActiveModuleSymbolsLocked(SymbolTable& snapshot, const SnapshotFilterContext& ctx) const
{
    for (const auto& modUri : ctx.allowedModuleFileUris)
    {
        if (snapshot.m_keysByFile.contains(modUri))
        {
            continue;
        }
        const auto itKeys = m_keysByFile.find(modUri);
        if (itKeys == m_keysByFile.end())
        {
            continue;
        }
        snapshot.m_keysByFile[modUri] = itKeys->second;
        for (const auto& key : itKeys->second)
        {
            const auto itSym = m_symbols.find(key);
            if (itSym != m_symbols.end())
            {
                snapshot.CopyBucketForSnapshotLocked(key, itSym->second, ctx);
            }
        }
        std::lock_guard<std::mutex> ruleGuard(m_ruleIndexMutex);
        if (snapshot.m_ruleIndex && snapshot.m_ruleIndexPartials)
        {
            if (!m_ruleIndexPartials)
            {
                m_ruleIndexPartials =
                    std::make_unique<ankerl::unordered_dense::map<std::string, rules::RuleIndexPartial>>();
            }
            auto itPartial = m_ruleIndexPartials->find(modUri);
            if (itPartial == m_ruleIndexPartials->end())
            {
                auto docSymbols = GetDocumentSymbolPointersLocked(modUri);
                rules::RuleIndexPartial partial = rules::RuleIndex::BuildPartial(modUri, docSymbols);
                itPartial = m_ruleIndexPartials->emplace(modUri, std::move(partial)).first;
            }
            snapshot.m_ruleIndex->ApplyPartial(itPartial->second);
            (*snapshot.m_ruleIndexPartials)[modUri] = itPartial->second;
        }
    }
}

void SymbolTable::PopulateSnapshotFromPredefinedBaseLocked(SymbolTable& snapshot, const SymbolTable& base)
{
    std::shared_lock<std::shared_mutex> baseLock(base.m_mutex);
    snapshot.m_symbols = base.m_symbols;
    snapshot.m_keysByFile = base.m_keysByFile;

    std::lock_guard<std::mutex> baseRuleGuard(base.m_ruleIndexMutex);
    if (base.m_ruleIndex)
    {
        snapshot.m_ruleIndex = std::make_shared<rules::RuleIndex>(*base.m_ruleIndex);
    }
    if (base.m_ruleIndexPartials)
    {
        snapshot.m_ruleIndexPartials =
            std::make_unique<ankerl::unordered_dense::map<std::string, rules::RuleIndexPartial>>(
                *base.m_ruleIndexPartials);
    }
}

std::unique_ptr<SymbolTable> SymbolTable::CreateAnalysisSnapshot(const AnalysisSnapshotRequest& request) const
{
    IncrementTableCloneCount();
    auto snapshot = std::make_unique<SymbolTable>();
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        snapshot->m_version = m_version;
        snapshot->m_virtualMixinDocumentsEnabled = m_virtualMixinDocumentsEnabled;
        snapshot->m_mixinClassCount = m_mixinClassCount;
        snapshot->m_classesWithSynthesizedMixins = m_classesWithSynthesizedMixins;
        snapshot->m_defaultArrayType = m_defaultArrayType;
        snapshot->m_stringType = m_stringType;
        snapshot->m_predefinedFileExtension = m_predefinedFileExtension;

        const std::string_view effectivePredefinedExt = request.predefinedExtension.empty()
                                                            ? std::string_view(m_predefinedFileExtension)
                                                            : request.predefinedExtension;

        if (!request.allowedModuleFileUris || request.allowedModuleFileUris->empty())
        {
            snapshot->m_symbols = m_symbols;
            snapshot->m_keysByFile = m_keysByFile;
            std::lock_guard<std::mutex> ruleGuard(m_ruleIndexMutex);
            snapshot->m_ruleIndex = m_ruleIndex;
            if (m_ruleIndexPartials)
            {
                snapshot->m_ruleIndexPartials =
                    std::make_unique<ankerl::unordered_dense::map<std::string, rules::RuleIndexPartial>>(
                        *m_ruleIndexPartials);
            }
        }
        else if (request.predefinedBase && request.predefinedBase->GetRuleIndex())
        {
            PopulateSnapshotFromPredefinedBaseLocked(*snapshot, *request.predefinedBase);
            const SnapshotFilterContext ctx{*request.allowedModuleFileUris, request.uriStr, effectivePredefinedExt};
            CopyActiveModuleSymbolsLocked(*snapshot, ctx);
        }
        else
        {
            const SnapshotFilterContext ctx{*request.allowedModuleFileUris, request.uriStr, effectivePredefinedExt};
            CopyFilteredSymbolsLocked(*snapshot, ctx);
        }
    }

    if (request.staging)
    {
        snapshot->ReplaceDocumentSymbols(request.uriStr, *request.staging);
    }
    return snapshot;
}

std::unique_ptr<SymbolTable>
SymbolTable::CreateAnalysisSnapshot(const std::string& uriStr, const SymbolTable& staging,
                                    const ankerl::unordered_dense::set<std::string>* allowedModuleFileUris,
                                    std::string_view predefinedExtension) const
{
    return CreateAnalysisSnapshot(AnalysisSnapshotRequest{
        .uriStr = uriStr,
        .staging = &staging,
        .allowedModuleFileUris = allowedModuleFileUris,
        .predefinedExtension = predefinedExtension,
        .predefinedBase = nullptr,
    });
}

void SymbolTable::PublishDocumentSymbols(const std::string& fileUri, std::vector<Symbol>&& fresh)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    const bool erasedMixin = HasMixinSymbolLocked(fileUri);
    EraseDocumentSymbolsOnlyLocked(fileUri);

    bool addedMixin = false;
    std::vector<std::string> freshClassKeys;
    for (auto& symbol : fresh)
    {
        const std::string& key = symbol.qualifiedName.empty() ? symbol.name : symbol.qualifiedName;

        if (symbol.type == SymbolType::Class && std::holds_alternative<ClassSignature>(symbol.signature))
        {
            if (symbol.GetClass().modifiers.isMixin)
            {
                addedMixin = true;
                ++m_mixinClassCount;
            }
            freshClassKeys.push_back(key);
        }

        if (symbol.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(symbol.signature) &&
            symbol.GetFunction().displaySignature.empty())
        {
            symbol.GetFunction().displaySignature = FormatFunctionDeclaration(symbol, true);
        }

        // Indexed before the move, not after: the index is keyed by the symbol's own file, not
        // by the argument - a symbol filed under a different URI would otherwise be indexed
        // here and never erased by its own document's clear - and after the move the symbol
        // that knew which file that was is gone.
        IndexKeyForFileLocked(symbol.fileUri, key);
        MutableBucket(m_symbols[key]).push_back(std::move(symbol));
    }

    std::vector<std::string> affectedFiles;
    if (erasedMixin || addedMixin)
    {
        // A mixin class was added or removed; existing classes across the entire workspace might be affected
        ResolveIncludedMixinsLocked(&affectedFiles);
    }
    else if (!freshClassKeys.empty())
    {
        // Only resolve included mixins for the classes declared in this document
        ResolveIncludedMixinsForKeysLocked(freshClassKeys, &affectedFiles);
    }

    {
        // If RuleIndex has not been lazily initialized yet, skip partial construction.
        // EnsureRuleIndex() or GetRuleIndex() will build the complete index over all
        // documents on first access.
        std::lock_guard<std::mutex> guard(m_ruleIndexMutex);
        if (!m_ruleIndex && !m_ruleIndexPartials)
        {
            ++m_version;
            return;
        }
    }

    // Build partial for fileUri after mixin resolution
    auto freshPtrs = GetDocumentSymbolPointersLocked(fileUri);
    auto freshPartial = rules::RuleIndex::BuildPartial(fileUri, freshPtrs);
    auto affectedPartials = BuildAffectedPartialsLocked(affectedFiles, fileUri);
    ApplyRuleIndexPartialsLocked(fileUri, std::move(freshPartial), std::move(affectedPartials));

    ++m_version;
}

bool SymbolTable::IsMixinClassLocked(const std::string& cleanName) const
{
    auto baseIt = m_symbols.find(cleanName);
    if (baseIt == m_symbols.end() || !baseIt->second)
    {
        return false;
    }
    for (const auto& cand : *baseIt->second)
    {
        if (cand.type == SymbolType::Class && std::holds_alternative<ClassSignature>(cand.signature) &&
            cand.GetClass().modifiers.isMixin)
        {
            return true;
        }
    }
    return false;
}

ankerl::unordered_dense::set<std::string>
SymbolTable::UpdateIncludedMixinsForClassesLocked(const std::vector<std::string>& classKeys,
                                                  std::vector<std::string>* outAffectedFiles)
{
    ankerl::unordered_dense::set<std::string> neededMixins;

    for (const auto& key : classKeys)
    {
        auto it = m_symbols.find(key);
        if (it == m_symbols.end() || !it->second)
        {
            continue;
        }

        std::vector<Symbol>& symbols = MutableBucket(it->second);
        for (auto& sym : symbols)
        {
            if (sym.type == SymbolType::Class && std::holds_alternative<ClassSignature>(sym.signature))
            {
                auto& sig = std::get<ClassSignature>(sym.signature);
                const auto oldIncludedMixins = sig.includedMixins;
                sig.includedMixins.clear();
                for (const auto& b : sig.bases)
                {
                    std::string clean = CleanBaseType(b);
                    if (IsMixinClassLocked(clean))
                    {
                        sig.includedMixins.push_back(clean);
                        neededMixins.insert(clean);
                    }
                }

                if (outAffectedFiles && sig.includedMixins != oldIncludedMixins)
                {
                    outAffectedFiles->push_back(sym.fileUri);
                }
            }
        }
    }
    return neededMixins;
}

void SymbolTable::CleanSynthesizedMembersForClassLocked(const Symbol& sym,
                                                        ankerl::unordered_dense::set<std::string>& fileKeys)
{
    const std::string hostQName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    std::vector<std::string> keysToClean;
    for (const auto& k : fileKeys)
    {
        if (utils::HasScopePrefix(k, hostQName))
        {
            auto bIt = m_symbols.find(k);
            if (bIt != m_symbols.end() && bIt->second)
            {
                auto& vec = MutableBucket(bIt->second);
                std::erase_if(vec, [](const Symbol& s) { return s.isSynthesized; });
                if (vec.empty())
                {
                    m_symbols.erase(bIt);
                    keysToClean.push_back(k);
                }
            }
        }
    }
    for (const auto& k : keysToClean)
    {
        fileKeys.erase(k);
    }
}

void SymbolTable::CleanSynthesizedSymbolsForClassesLocked(const std::vector<std::string>& classKeys)
{
    if (m_classesWithSynthesizedMixins.empty())
    {
        return;
    }
    for (const auto& key : classKeys)
    {
        if (!m_classesWithSynthesizedMixins.contains(key))
        {
            continue;
        }
        auto it = m_symbols.find(key);
        if (it == m_symbols.end() || !it->second)
        {
            m_classesWithSynthesizedMixins.erase(key);
            continue;
        }
        for (const auto& sym : *it->second)
        {
            if (sym.type == SymbolType::Class)
            {
                auto fileIt = m_keysByFile.find(sym.fileUri);
                if (fileIt != m_keysByFile.end())
                {
                    CleanSynthesizedMembersForClassLocked(sym, fileIt->second);
                }
            }
        }
        m_classesWithSynthesizedMixins.erase(key);
    }
}

ankerl::unordered_dense::map<std::string, std::vector<Symbol>>
SymbolTable::CollectMixinMemberFunctionsLocked(const ankerl::unordered_dense::set<std::string>& neededMixins) const
{
    ankerl::unordered_dense::map<std::string, std::vector<Symbol>> mixinMembers;
    for (const auto& [k, bucket] : m_symbols)
    {
        if (!bucket)
        {
            continue;
        }
        for (const auto& sym : *bucket)
        {
            if (!sym.isSynthesized && sym.type == SymbolType::Function && !sym.containerName.empty() &&
                neededMixins.contains(sym.containerName))
            {
                mixinMembers[sym.containerName].push_back(sym);
            }
        }
    }
    return mixinMembers;
}

bool SymbolTable::HasSynthesizedMemberConflictLocked(const std::string& synthKey, const Symbol& mSym) const
{
    auto existingBucketIt = m_symbols.find(synthKey);
    if (existingBucketIt == m_symbols.end() || !existingBucketIt->second)
    {
        return false;
    }
    for (const auto& existingSym : *existingBucketIt->second)
    {
        if (mSym.type == SymbolType::Function && existingSym.type == SymbolType::Function)
        {
            if (HasSameParameterList(existingSym, mSym))
            {
                return true;
            }
        }
        else
        {
            return true;
        }
    }
    return false;
}

void SymbolTable::SynthesizeSingleMixinMemberLocked(const std::string& hostQName, const std::string& hostFileUri,
                                                    const std::string& mixinName, const Symbol& mSym)
{
    const std::string synthKey = utils::JoinScopedName(hostQName, mSym.name);
    if (HasSynthesizedMemberConflictLocked(synthKey, mSym))
    {
        return;
    }

    Symbol synth = mSym;
    synth.containerName = mSym.containerName.empty() ? mixinName : mSym.containerName;
    synth.containerKind = ContainerKind::Class;
    synth.qualifiedName = synthKey;
    synth.fileUri = mSym.fileUri;
    synth.isSynthesized = true;
    if (m_virtualMixinDocumentsEnabled)
    {
        synth.virtualFileUri = BuildVirtualMixinUri(hostQName, mixinName);
    }
    if (synth.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(synth.signature))
    {
        synth.GetFunction().displaySignature.clear();
        synth.GetFunction().displaySignature = FormatFunctionDeclaration(synth, true);
    }

    MutableBucket(m_symbols[synthKey]).push_back(std::move(synth));
    m_classesWithSynthesizedMixins.insert(hostQName);
    IndexKeyForFileLocked(hostFileUri, synthKey);
}

void SymbolTable::SynthesizeMixinMembersIntoClassesLocked(
    const std::vector<std::string>& classKeys,
    const ankerl::unordered_dense::map<std::string, std::vector<Symbol>>& mixinMembers)
{
    for (const auto& key : classKeys)
    {
        auto it = m_symbols.find(key);
        if (it == m_symbols.end() || !it->second)
        {
            continue;
        }

        for (const auto& classSym : *it->second)
        {
            if (classSym.type != SymbolType::Class || !std::holds_alternative<ClassSignature>(classSym.signature))
            {
                continue;
            }

            const auto& sig = std::get<ClassSignature>(classSym.signature);
            if (sig.includedMixins.empty())
            {
                continue;
            }

            const std::string hostQName = classSym.qualifiedName.empty() ? classSym.name : classSym.qualifiedName;
            const std::string& hostFileUri = classSym.fileUri;

            for (const auto& mixinName : sig.includedMixins)
            {
                auto mIt = mixinMembers.find(mixinName);
                if (mIt == mixinMembers.end())
                {
                    continue;
                }

                for (const auto& mSym : mIt->second)
                {
                    SynthesizeSingleMixinMemberLocked(hostQName, hostFileUri, mixinName, mSym);
                }
            }
        }
    }
}

void SymbolTable::ResolveIncludedMixinsForKeysLocked(const std::vector<std::string>& classKeys,
                                                     std::vector<std::string>* outAffectedFiles)
{
    if (classKeys.empty() || (m_mixinClassCount == 0 && m_classesWithSynthesizedMixins.empty()))
    {
        return;
    }

    const auto neededMixins = UpdateIncludedMixinsForClassesLocked(classKeys, outAffectedFiles);
    CleanSynthesizedSymbolsForClassesLocked(classKeys);

    if (neededMixins.empty())
    {
        return;
    }

    const auto mixinMembers = CollectMixinMemberFunctionsLocked(neededMixins);
    SynthesizeMixinMembersIntoClassesLocked(classKeys, mixinMembers);
}

void SymbolTable::ResolveIncludedMixinsLocked(std::vector<std::string>* outAffectedFiles)
{
    if (m_mixinClassCount == 0 && m_classesWithSynthesizedMixins.empty())
    {
        return;
    }
    std::vector<std::string> classKeys;
    for (const auto& [name, bucket] : m_symbols)
    {
        if (!bucket)
        {
            continue;
        }
        for (const auto& sym : *bucket)
        {
            if (sym.type == SymbolType::Class && std::holds_alternative<ClassSignature>(sym.signature))
            {
                classKeys.push_back(name);
                break;
            }
        }
    }
    ResolveIncludedMixinsForKeysLocked(classKeys, outAffectedFiles);
}

void SymbolTable::ResolveIncludedMixins()
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    ResolveIncludedMixinsLocked();
    ++m_version;
}

void SymbolTable::SetVirtualMixinDocumentsEnabled(bool enabled)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_virtualMixinDocumentsEnabled = enabled;
}

bool SymbolTable::IsVirtualMixinDocumentsEnabled() const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_virtualMixinDocumentsEnabled;
}

std::string SymbolTable::BuildVirtualMixinUri(std::string_view hostClass, std::string_view mixinName)
{
    std::string mixinClean(mixinName);
    if (mixinClean.ends_with(".as"))
    {
        return fmt::format("angelscript-virtual://{}/{}", hostClass, mixinClean);
    }
    return fmt::format("angelscript-virtual://{}/{}.as", hostClass, mixinClean);
}

/**
 * @brief Parses an angelscript-virtual URI into host class and mixin name components without heap allocation.
 * @param[in] uri Virtual document URI string.
 * @param[out] hostClass Extracted host class component string view.
 * @param[out] mixinPart Extracted mixin component string view.
 * @return True if the URI was successfully parsed as a virtual mixin URI; false otherwise.
 */
bool SymbolTable::ParseVirtualMixinUri(std::string_view uri, std::string_view& hostClass,
                                       std::string_view& mixinPart) noexcept
{
    hostClass = {};
    mixinPart = {};
    const auto colonPos = uri.find(':');
    if (colonPos == std::string_view::npos)
    {
        return false;
    }

    const std::string_view scheme = uri.substr(0, colonPos);
    if (scheme != "angelscript-virtual")
    {
        return false;
    }

    std::string_view rest = uri.substr(colonPos + 1);
    while (!rest.empty() && rest.front() == '/')
    {
        rest.remove_prefix(1);
    }

    const auto slashPos = rest.find('/');
    if (slashPos != std::string_view::npos)
    {
        hostClass = rest.substr(0, slashPos);
        mixinPart = rest.substr(slashPos + 1);
    }
    else
    {
        mixinPart = rest;
    }

    if (mixinPart.ends_with(".as"))
    {
        mixinPart.remove_suffix(3);
    }

    return true;
}

/**
 * @brief Extracts the host class identifier from an angelscript-virtual URI.
 * @param[in] uri Virtual document URI string.
 * @return Extracted host class name, or empty string if not a virtual URI.
 */
std::string SymbolTable::ExtractVirtualHostClass(std::string_view uri)
{
    std::string_view hostClass{};
    std::string_view mixinPart{};
    if (ParseVirtualMixinUri(uri, hostClass, mixinPart))
    {
        return utils::UrlDecode(hostClass);
    }
    return {};
}

/**
 * @brief Extracts the mixin class name from an angelscript-virtual URI.
 * @param[in] uri Virtual document URI string.
 * @return Extracted mixin class name, or empty string if not a virtual URI.
 */
std::string SymbolTable::ExtractVirtualMixinName(std::string_view uri)
{
    std::string_view hostClass{};
    std::string_view mixinPart{};
    if (ParseVirtualMixinUri(uri, hostClass, mixinPart))
    {
        return utils::UrlDecode(mixinPart);
    }
    return {};
}

bool SymbolTable::HasDocumentSymbols(const std::string& fileUri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_keysByFile.find(fileUri);
    return it != m_keysByFile.end() && !it->second.empty();
}

bool SymbolTable::HasSymbol(std::string_view qualifiedName) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::string_view search = utils::StripGlobalPrefix(qualifiedName);
    return m_symbols.contains(search);
}

/**
 * @brief Symbol table has scoped symbol.
 * @param[in] scope Enclosing lexical scope.
 * @param[in] name Symbol or identifier name.
 * @return True if the condition is satisfied; false otherwise.
 */
bool SymbolTable::HasScopedSymbol(std::string_view scope, std::string_view name) const
{
    if (name.empty())
    {
        return false;
    }
    if (scope.empty())
    {
        return HasSymbol(name);
    }
    return WithCompositeMemberKey(scope, name, [this](std::string_view key) { return HasSymbol(key); });
}

bool SymbolTable::IsReachableInContainerScope(std::string_view cleanName, std::string_view containerScope) const
{
    if (cleanName.empty())
    {
        return false;
    }
    std::string_view current = containerScope;
    while (!current.empty())
    {
        if (HasScopedSymbol(current, cleanName))
        {
            return true;
        }
        current = utils::ParentScopeView(current);
    }
    return false;
}

std::shared_ptr<const std::vector<Symbol>> SymbolTable::FindSymbolsPtr(std::string_view qualifiedName) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::string_view search = utils::StripGlobalPrefix(qualifiedName);
    auto it = m_symbols.find(search);
    return it != m_symbols.end() ? it->second : nullptr;
}

/**
 * @brief Looks up a member symbol within a container scope returning a shared vector pointer.
 * @param[in] scope Fully qualified name of the enclosing container class or namespace.
 * @param[in] member Unqualified name of the member symbol to find.
 * @return Shared pointer to matching symbols vector, or nullptr if none found.
 */
std::shared_ptr<const std::vector<Symbol>> SymbolTable::FindMemberSymbolPtr(std::string_view scope,
                                                                            std::string_view member) const
{
    if (scope.empty() || member.empty())
    {
        return nullptr;
    }
    return WithCompositeMemberKey(scope, member, [this](std::string_view key) { return FindSymbolsPtr(key); });
}

std::vector<Symbol> SymbolTable::FindMemberSymbols(std::string_view scope, std::string_view member) const
{
    auto ptr = FindMemberSymbolPtr(scope, member);
    return ptr ? *ptr : std::vector<Symbol>{};
}

std::vector<Symbol> SymbolTable::FindSymbols(std::string_view qualifiedName) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::string_view search = utils::StripGlobalPrefix(qualifiedName);
    auto it = m_symbols.find(search);
    return it != m_symbols.end() ? *it->second : std::vector<Symbol>{};
}

std::optional<Symbol> SymbolTable::FindFirstSymbol(std::string_view qualifiedName) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::string_view search = utils::StripGlobalPrefix(qualifiedName);
    auto it = m_symbols.find(search);
    if (it != m_symbols.end() && !it->second->empty())
        return it->second->front();
    return std::nullopt;
}

std::optional<Symbol> SymbolTable::LookupSymbol(std::string_view name) const
{
    return FindFirstSymbol(name);
}

std::vector<Symbol> SymbolTable::FindTypeSymbolsByShortName(std::string_view shortName) const
{
    std::vector<Symbol> result;
    const auto index = GetRuleIndex();
    if (!index)
    {
        return result;
    }

    const auto it = index->qualifiedTypesByShortName.find(shortName);
    if (it != index->qualifiedTypesByShortName.end())
    {
        for (const auto& qKey : it->second)
        {
            auto ptr = FindSymbolsPtr(qKey);
            if (ptr)
            {
                result.insert(result.end(), ptr->begin(), ptr->end());
            }
        }
    }
    return result;
}

bool SymbolTable::HasFuncdefByShortName(std::string_view shortName) const noexcept
{
    const auto index = GetRuleIndex();
    if (!index)
    {
        return false;
    }

    const auto it = index->qualifiedTypesByShortName.find(shortName);
    if (it != index->qualifiedTypesByShortName.end())
    {
        for (const auto& qKey : it->second)
        {
            auto ptr = FindSymbolsPtr(qKey);
            if (ptr)
            {
                for (const auto& sym : *ptr)
                {
                    if (sym.type == SymbolType::Funcdef)
                    {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void SymbolTable::InsertSymbol(const std::string& name, SymbolKind kind, const std::string& type)
{
    Symbol sym;
    sym.name = name;
    sym.type = kind;
    VariableSignature varSig;
    varSig.typeName = type;
    varSig.baseTypeName = type;
    sym.signature = varSig;
    AddSymbol(sym);
}

bool SymbolTable::HasSymbolAnywhere(std::string_view name) const
{
    std::string_view searchName = utils::StripGlobalPrefix(name);

    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        if (m_symbols.find(searchName) != m_symbols.end() || m_symbols.find(name) != m_symbols.end())
            return true;
    }

    const auto index = GetRuleIndex();
    return index && (index->allNames.contains(searchName) || index->allNames.contains(name));
}

void SymbolTable::EnsureSortedKeysLocked() const
{
    std::lock_guard<std::mutex> sortLock(m_sortedKeysMutex);
    if (m_sortedKeysVersion == m_version)
    {
        return;
    }
    m_sortedKeys.clear();
    m_sortedKeys.reserve(m_symbols.size());
    for (const auto& [key, _] : m_symbols)
    {
        m_sortedKeys.push_back(key);
    }
    std::sort(m_sortedKeys.begin(), m_sortedKeys.end());
    m_sortedKeysVersion = m_version;
}

void SymbolTable::EnsureGlobalTypesLocked() const
{
    std::lock_guard<std::mutex> typeLock(m_globalTypesMutex);
    if (m_globalTypesVersion == m_version)
    {
        return;
    }
    m_globalTypeSymbols.clear();
    for (const auto& [_, bucket] : m_symbols)
    {
        for (const auto& sym : *bucket)
        {
            if (sym.containerName.empty() &&
                (sym.type == SymbolType::Class || sym.type == SymbolType::Interface || sym.type == SymbolType::Enum ||
                 sym.type == SymbolType::Typedef || sym.type == SymbolType::Funcdef))
            {
                m_globalTypeSymbols.push_back(sym);
            }
        }
    }
    m_globalTypesVersion = m_version;
}

void SymbolTable::ForEachGlobalTypeSymbol(const std::function<void(const Symbol&)>& visitor) const
{
    std::vector<Symbol> snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        EnsureGlobalTypesLocked();
        snapshot = m_globalTypeSymbols;
    }
    for (const auto& sym : snapshot)
    {
        visitor(sym);
    }
}

void SymbolTable::ForEachSymbolWithPrefix(
    std::string_view prefix, const std::function<void(const std::string&, const std::vector<Symbol>&)>& visitor) const
{
    if (prefix.empty())
    {
        ForEachSymbol(visitor);
        return;
    }

    std::vector<std::pair<const std::string*, std::shared_ptr<const std::vector<Symbol>>>> snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        EnsureSortedKeysLocked();

        ankerl::unordered_dense::set<const std::string*> seenKeys;

        auto collectMatching = [&](std::string_view p)
        {
            if (p.empty())
            {
                return;
            }
            char c = p[0];
            char cUpper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            char cLower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            auto scanChar = [&](char firstChar)
            {
                std::string startPattern(1, firstChar);
                auto it = std::lower_bound(m_sortedKeys.begin(), m_sortedKeys.end(), startPattern);
                for (; it != m_sortedKeys.end(); ++it)
                {
                    if (it->empty() || (*it)[0] != firstChar)
                    {
                        break;
                    }
                    if (angel_lsp::utils::CaseInsensitiveStartsWith(*it, p))
                    {
                        auto mapIt = m_symbols.find(*it);
                        if (mapIt != m_symbols.end() && seenKeys.insert(&mapIt->first).second)
                        {
                            snapshot.emplace_back(&mapIt->first, mapIt->second);
                        }
                    }
                }
            };

            scanChar(cUpper);
            if (cLower != cUpper)
            {
                scanChar(cLower);
            }
        };

        collectMatching(prefix);
        collectMatching("get_" + std::string(prefix));
        collectMatching("set_" + std::string(prefix));
    }

    for (const auto& [key, symbols] : snapshot)
    {
        visitor(*key, *symbols);
    }
}

void SymbolTable::ForEachSymbol(
    const std::function<void(const std::string&, const std::vector<Symbol>&)>& visitor) const
{
    std::vector<std::pair<const std::string*, std::shared_ptr<const std::vector<Symbol>>>> snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        snapshot.reserve(m_symbols.size());
        for (const auto& [key, symbols] : m_symbols)
        {
            snapshot.emplace_back(&key, symbols);
        }
    }

    for (const auto& [key, symbols] : snapshot)
    {
        visitor(*key, *symbols);
    }
}

std::vector<Symbol> SymbolTable::GetAllSymbols() const
{
    std::vector<Symbol> result;
    ForEachSymbol([&result]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& syms)
                  { result.insert(result.end(), syms.begin(), syms.end()); });
    return result;
}

void SymbolTable::ForEachSymbolInFile(
    const std::string& fileUri,
    const std::function<void(const std::string&, const std::vector<Symbol>&)>& visitor) const
{
    std::vector<std::pair<const std::string*, std::shared_ptr<const std::vector<Symbol>>>> snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        const auto fileEntry = m_keysByFile.find(fileUri);
        if (fileEntry == m_keysByFile.end())
        {
            return;
        }

        snapshot.reserve(fileEntry->second.size());
        for (const auto& key : fileEntry->second)
        {
            const auto bucket = m_symbols.find(key);
            if (bucket != m_symbols.end())
            {
                snapshot.emplace_back(&bucket->first, bucket->second);
            }
        }
    }

    for (const auto& [key, symbols] : snapshot)
        visitor(*key, *symbols);
}

std::vector<std::string> SymbolTable::GetIndexedFileUris() const
{
    std::vector<std::string> uris;
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    uris.reserve(m_keysByFile.size());
    for (const auto& [fileUri, keys] : m_keysByFile)
    {
        if (!fileUri.empty() && !keys.empty())
        {
            uris.push_back(fileUri);
        }
    }
    return uris;
}

bool SymbolTable::ContainsOnlyFiles(const ankerl::unordered_dense::set<std::string>& allowedFileUris,
                                    std::string_view predefinedExtension) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    for (const auto& [fileUri, keys] : m_keysByFile)
    {
        if (fileUri.empty() || keys.empty())
        {
            continue;
        }
        if (!predefinedExtension.empty() && utils::IsPredefinedFile(fileUri, predefinedExtension))
        {
            continue;
        }
        if (!allowedFileUris.contains(fileUri))
        {
            return false;
        }
    }
    return true;
}

uint64_t SymbolTable::Version() const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_version;
}

std::shared_ptr<const rules::RuleIndex> SymbolTable::GetRuleIndex() const
{
    {
        std::lock_guard<std::mutex> guard(m_ruleIndexMutex);
        if (m_ruleIndex)
        {
            return m_ruleIndex;
        }
    }

    std::shared_lock<std::shared_mutex> tableLock(m_mutex);
    std::lock_guard<std::mutex> ruleGuard(m_ruleIndexMutex);
    if (m_ruleIndex)
    {
        return m_ruleIndex;
    }

    if (!m_ruleIndexPartials)
    {
        m_ruleIndexPartials = std::make_unique<ankerl::unordered_dense::map<std::string, rules::RuleIndexPartial>>();
    }

    auto index = std::make_shared<rules::RuleIndex>();
    index->baseClassDelegateProperties = m_baseClassDelegateProperties;
    for (const auto& prop : m_baseClassDelegateProperties)
    {
        index->baseClassDelegatePropertySet.insert(prop);
    }
    for (const auto& [fileUri, keys] : m_keysByFile)
    {
        auto docSymbols = GetDocumentSymbolPointersLocked(fileUri);
        rules::RuleIndexPartial partial = rules::RuleIndex::BuildPartial(fileUri, docSymbols);
        index->ApplyPartial(partial);
        (*m_ruleIndexPartials)[fileUri] = std::move(partial);
    }
    m_ruleIndex = std::move(index);
    return m_ruleIndex;
}

void SymbolTable::EnsureRuleIndex() const
{
    // Delegate to the lazy builder. If already constructed, this is a no-op
    // (one mutex acquisition + null check).
    GetRuleIndex();
}

void SymbolTable::SetBaseClassDelegateProperties(std::vector<std::string> properties)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_baseClassDelegateProperties = std::move(properties);
    std::lock_guard<std::mutex> ruleGuard(m_ruleIndexMutex);
    if (m_ruleIndex)
    {
        auto modifiableIndex = std::const_pointer_cast<rules::RuleIndex>(m_ruleIndex);
        modifiableIndex->baseClassDelegateProperties = m_baseClassDelegateProperties;
        modifiableIndex->baseClassDelegatePropertySet.clear();
        for (const auto& prop : m_baseClassDelegateProperties)
        {
            modifiableIndex->baseClassDelegatePropertySet.insert(prop);
        }
    }
}

const std::vector<std::string>& SymbolTable::GetBaseClassDelegateProperties() const noexcept
{
    return m_baseClassDelegateProperties;
}

static inline void HashCombine(uint64_t& h, uint64_t val)
{
    h ^= val + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
}

static inline void HashString(uint64_t& h, std::string_view s)
{
    uint64_t strHash = ankerl::unordered_dense::hash<std::string_view>{}(s);
    HashCombine(h, strHash);
}

static inline void HashModifiers(uint64_t& h, const SymbolModifiers& m)
{
    uint32_t flags = static_cast<uint32_t>(m.access) | (static_cast<uint32_t>(m.isConst) << 2) |
                     (static_cast<uint32_t>(m.isHandle) << 3) | (static_cast<uint32_t>(m.isShared) << 4) |
                     (static_cast<uint32_t>(m.isMixin) << 5) | (static_cast<uint32_t>(m.isAbstract) << 6) |
                     (static_cast<uint32_t>(m.isFinal) << 7) | (static_cast<uint32_t>(m.isOverride) << 8) |
                     (static_cast<uint32_t>(m.isExplicit) << 9) | (static_cast<uint32_t>(m.isProperty) << 10) |
                     (static_cast<uint32_t>(m.isDelete) << 11) | (static_cast<uint32_t>(m.isExternal) << 12) |
                     (static_cast<uint32_t>(m.isReturnReference) << 13);
    HashCombine(h, flags);
}

/**
 * @brief Mixes parameter information into an ongoing 64-bit signature hash.
 * @param[in,out] h Ongoing hash accumulator.
 * @param[in] p Parameter information to hash.
 */
static inline void HashParameter(uint64_t& h, const ParameterInformation& p)
{
    HashString(h, p.name);
    HashString(h, p.typeName);
    HashCombine(h, static_cast<uint64_t>(p.typeKind));
    uint32_t flags = (p.isArray ? 1 : 0) | ((p.isConst ? 1 : 0) << 1) | ((p.isReference ? 1 : 0) << 2) |
                     ((p.isHandle ? 1 : 0) << 3) | (static_cast<uint32_t>(p.modifier) << 4);
    HashCombine(h, flags);
    HashString(h, p.defaultValue);
}

/**
 * @brief Mixes function signature metadata into an ongoing 64-bit signature hash.
 * @param[in,out] h Ongoing hash accumulator.
 * @param[in] fn Function signature descriptor to hash.
 */
static void HashFunctionSignature(uint64_t& h, const FunctionSignature& fn)
{
    HashString(h, fn.returnType);
    HashCombine(h, static_cast<uint64_t>(fn.returnTypeKind));
    uint32_t fnFlags =
        (fn.returnIsArray ? 1 : 0) | ((fn.returnIsConst ? 1 : 0) << 1) | ((fn.isInterfaceMethod ? 1 : 0) << 2);
    HashCombine(h, fnFlags);
    HashModifiers(h, fn.modifiers);
    HashCombine(h, fn.parameters.size());
    for (const auto& p : fn.parameters)
    {
        HashParameter(h, p);
    }
}

/**
 * @brief Mixes variable signature metadata into an ongoing 64-bit signature hash.
 * @param[in,out] h Ongoing hash accumulator.
 * @param[in] v Variable signature descriptor to hash.
 */
static void HashVariableSignature(uint64_t& h, const VariableSignature& v)
{
    HashString(h, v.typeName);
    HashCombine(h, static_cast<uint64_t>(v.typeKind));
    uint32_t vFlags = (v.isArray ? 1 : 0) | ((v.isVirtualProperty ? 1 : 0) << 1) | ((v.hasGet ? 1 : 0) << 2) |
                      ((v.hasSet ? 1 : 0) << 3);
    HashCombine(h, vFlags);
    HashModifiers(h, v.modifiers);
}

/**
 * @brief Mixes class signature metadata into an ongoing 64-bit signature hash.
 * @param[in,out] h Ongoing hash accumulator.
 * @param[in] cls Class signature descriptor to hash.
 */
static void HashClassSignature(uint64_t& h, const ClassSignature& cls)
{
    HashModifiers(h, cls.modifiers);
    for (const auto& b : cls.bases)
    {
        HashString(h, b);
    }
    for (const auto& m : cls.includedMixins)
    {
        HashString(h, m);
    }
    for (const auto& t : cls.templateParams)
    {
        HashString(h, t);
    }
    h = h * 1099511628211ULL ^ (cls.hasListPattern ? 1ULL : 0ULL);
}

/**
 * @brief Mixes interface signature metadata into an ongoing 64-bit signature hash.
 * @param[in,out] h Ongoing hash accumulator.
 * @param[in] iface Interface signature descriptor to hash.
 */
static void HashInterfaceSignature(uint64_t& h, const InterfaceSignature& iface)
{
    HashModifiers(h, iface.modifiers);
    for (const auto& b : iface.inheritedInterfaces)
    {
        HashString(h, b);
    }
}

/**
 * @brief Mixes enum signature metadata into an ongoing 64-bit signature hash.
 * @param[in,out] h Ongoing hash accumulator.
 * @param[in] enm Enum signature descriptor to hash.
 */
static void HashEnumSignature(uint64_t& h, const EnumSignature& enm)
{
    HashModifiers(h, enm.modifiers);
    for (const auto& m : enm.members)
    {
        HashString(h, m.name);
        HashString(h, m.value);
    }
}

/**
 * @brief Mixes funcdef signature metadata into an ongoing 64-bit signature hash.
 * @param[in,out] h Ongoing hash accumulator.
 * @param[in] fd Funcdef signature descriptor to hash.
 */
static void HashFuncdefSignature(uint64_t& h, const FuncdefSignature& fd)
{
    HashString(h, fd.returnType);
    HashCombine(h, static_cast<uint64_t>(fd.returnTypeKind));
    HashModifiers(h, fd.modifiers);
    HashCombine(h, fd.parameters.size());
    for (const auto& p : fd.parameters)
    {
        HashParameter(h, p);
    }
}

static void HashSymbolInterface(uint64_t& h, const Symbol& sym)
{
    HashCombine(h, static_cast<uint64_t>(sym.type));
    HashString(h, sym.name);
    HashString(h, sym.containerName);
    HashString(h, sym.qualifiedName);

    if (std::holds_alternative<FunctionSignature>(sym.signature))
    {
        HashFunctionSignature(h, std::get<FunctionSignature>(sym.signature));
    }
    else if (std::holds_alternative<VariableSignature>(sym.signature))
    {
        HashVariableSignature(h, std::get<VariableSignature>(sym.signature));
    }
    else if (std::holds_alternative<ClassSignature>(sym.signature))
    {
        HashClassSignature(h, std::get<ClassSignature>(sym.signature));
    }
    else if (std::holds_alternative<InterfaceSignature>(sym.signature))
    {
        HashInterfaceSignature(h, std::get<InterfaceSignature>(sym.signature));
    }
    else if (std::holds_alternative<EnumSignature>(sym.signature))
    {
        HashEnumSignature(h, std::get<EnumSignature>(sym.signature));
    }
    else if (std::holds_alternative<TypedefSignature>(sym.signature))
    {
        const auto& td = std::get<TypedefSignature>(sym.signature);
        HashString(h, td.baseType);
        HashCombine(h, static_cast<uint64_t>(td.typeKind));
    }
    else if (std::holds_alternative<FuncdefSignature>(sym.signature))
    {
        HashFuncdefSignature(h, std::get<FuncdefSignature>(sym.signature));
    }
}

uint64_t SymbolTable::ComputeDocumentInterfaceHash(const std::string& fileUri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return ComputeDocumentInterfaceHashLocked(fileUri);
}

uint64_t SymbolTable::ComputeDocumentInterfaceHashLocked(const std::string& fileUri) const
{
    const auto fileEntry = m_keysByFile.find(fileUri);
    if (fileEntry == m_keysByFile.end())
    {
        return 0;
    }

    std::vector<std::string> keys(fileEntry->second.begin(), fileEntry->second.end());
    std::sort(keys.begin(), keys.end());

    uint64_t h = 0xcbf29ce484222325ULL;
    for (const auto& key : keys)
    {
        const auto bucket = m_symbols.find(key);
        if (bucket == m_symbols.end() || !bucket->second)
        {
            continue;
        }

        std::vector<const Symbol*> docSymbols;
        for (const auto& sym : *bucket->second)
        {
            if (sym.fileUri == fileUri)
            {
                docSymbols.push_back(&sym);
            }
        }

        std::sort(docSymbols.begin(), docSymbols.end(),
                  [](const Symbol* a, const Symbol* b)
                  {
                      if (a->type != b->type)
                      {
                          return a->type < b->type;
                      }
                      if (std::holds_alternative<FunctionSignature>(a->signature) &&
                          std::holds_alternative<FunctionSignature>(b->signature))
                      {
                          const auto& fnA = std::get<FunctionSignature>(a->signature);
                          const auto& fnB = std::get<FunctionSignature>(b->signature);
                          if (fnA.parameters.size() != fnB.parameters.size())
                          {
                              return fnA.parameters.size() < fnB.parameters.size();
                          }
                          for (size_t i = 0; i < fnA.parameters.size(); ++i)
                          {
                              if (fnA.parameters[i].typeName != fnB.parameters[i].typeName)
                              {
                                  return fnA.parameters[i].typeName < fnB.parameters[i].typeName;
                              }
                          }
                      }
                      return a->name < b->name;
                  });

        for (const auto* sym : docSymbols)
        {
            HashSymbolInterface(h, *sym);
        }
    }

    return h;
}

std::string SymbolTable::FindEnclosingClassName(std::string_view uri, uint32_t line, std::string_view excludeName) const
{
    std::string enclosingClass;
    std::string uriStr(uri);
    uint32_t bestSpan = UINT32_MAX;
    ForEachSymbolInFile(uriStr,
                        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& symbols)
                        {
                            for (const auto& sym : symbols)
                            {
                                if ((sym.type == SymbolType::Class || sym.type == SymbolType::Interface) &&
                                    sym.fileUri == uri && (excludeName.empty() || sym.name != excludeName))
                                {
                                    if (line >= sym.startLine && line <= sym.endLine)
                                    {
                                        uint32_t span = sym.endLine - sym.startLine;
                                        if (span <= bestSpan)
                                        {
                                            bestSpan = span;
                                            enclosingClass = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                                        }
                                    }
                                }
                            }
                        });
    return enclosingClass;
}

std::string SymbolTable::QualifyShortTypeName(std::string_view typeName) const
{
    if (!utils::ContainsScopeSeparator(typeName) && !HasSymbol(typeName))
    {
        auto shortMatches = FindTypeSymbolsByShortName(typeName);
        for (const auto& sym : shortMatches)
        {
            if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface)
            {
                return sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            }
        }
    }
    return std::string(typeName);
}

} // namespace angel_lsp::analysis
