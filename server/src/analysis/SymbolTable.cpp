#include "SymbolTable.h"
#include "analysis/OverloadResolver.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "spdlog/fmt/fmt.h"
#include "utils/LspLogger.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>

namespace angel_lsp::analysis
{
SymbolTable::SymbolTable() = default;

SymbolTable::~SymbolTable() = default;

namespace
{
std::string AccessModifierToString(AccessModifier access)
{
    switch (access)
    {
    case AccessModifier::Public:
        return "public";
    case AccessModifier::Private:
        return "private";
    case AccessModifier::Protected:
        return "protected";
    default:
        return "public";
    }
}

std::string ParameterModifierToString(ParameterModifier mod)
{
    switch (mod)
    {
    case ParameterModifier::In:
        return "&in";
    case ParameterModifier::Out:
        return "&out";
    case ParameterModifier::InOut:
        return "&inout";
    case ParameterModifier::None:
        return "";
    default:
        return "";
    }
}

std::string JoinStrings(const std::vector<std::string>& items, const char* sep = ", ")
{
    std::string result;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i > 0)
            result += sep;
        result += items[i];
    }
    return result;
}

std::string JoinFlags(const std::vector<std::string>& flags)
{
    if (flags.empty())
        return "[]";
    return "[" + JoinStrings(flags) + "]";
}

std::vector<Symbol>& MutableBucket(std::shared_ptr<std::vector<Symbol>>& bucket)
{
    if (!bucket)
        bucket = std::make_shared<std::vector<Symbol>>();
    else if (bucket.use_count() > 1)
        bucket = std::make_shared<std::vector<Symbol>>(*bucket);
    return *bucket;
}

void BuildParamStrings(const std::vector<ParameterInformation>& params, std::string& outParamsStr,
                       std::vector<std::string>& outParamLines)
{
    outParamsStr = "(";
    for (size_t i = 0; i < params.size(); ++i)
    {
        const auto& param = params[i];
        if (i > 0)
            outParamsStr += ", ";

        std::string modStr = ParameterModifierToString(param.modifier);
        std::string paramTypeStr = modStr.empty() ? param.typeName : param.typeName + " " + modStr;

        outParamsStr += paramTypeStr;
        if (!param.name.empty())
            outParamsStr += " " + param.name;
        if (!param.defaultValue.empty())
            outParamsStr += " = " + param.defaultValue;

        std::vector<std::string> pFlags;
        if (param.isConst)
            pFlags.push_back("const: true");
        if (param.isHandle)
            pFlags.push_back("handle: true");
        if (param.modifier == ParameterModifier::In)
            pFlags.push_back("in: true");
        else if (param.modifier == ParameterModifier::Out)
            pFlags.push_back("out: true");
        else if (param.modifier == ParameterModifier::InOut)
            pFlags.push_back("inout: true");

        std::string defaultStr =
            param.defaultValue.empty() ? "" : fmt::format(" | Default: \"{}\"", param.defaultValue);

        outParamLines.push_back(fmt::format("      \u2514\u2500 Parameter: Type: \"{}\" | Name: \"{}\"{}  | Flags: {}",
                                            paramTypeStr, param.name, defaultStr, JoinFlags(pFlags)));
    }
    outParamsStr += ")";
}
} // namespace

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
    MutableBucket(m_symbols[key]).push_back(symbol);
    IndexKeyForFileLocked(symbol.fileUri, key);

    const bool isClass = (symbol.type == SymbolType::Class && std::holds_alternative<ClassSignature>(symbol.signature));
    const bool isMixin = isClass && symbol.GetClass().modifiers.isMixin;

    std::vector<std::string> affectedFiles;
    if (isMixin)
    {
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

std::unique_ptr<SymbolTable> SymbolTable::CreateAnalysisSnapshot(const std::string& uriStr,
                                                                 const SymbolTable& staging) const
{
    IncrementTableCloneCount();
    auto snapshot = std::make_unique<SymbolTable>();
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        snapshot->m_symbols = m_symbols;
        snapshot->m_keysByFile = m_keysByFile;
        snapshot->m_version = m_version;
        snapshot->m_virtualMixinDocumentsEnabled = m_virtualMixinDocumentsEnabled;

        std::lock_guard<std::mutex> ruleGuard(m_ruleIndexMutex);
        snapshot->m_ruleIndex = m_ruleIndex;
        if (m_ruleIndexPartials)
        {
            snapshot->m_ruleIndexPartials =
                std::make_unique<ankerl::unordered_dense::map<std::string, rules::RuleIndexPartial>>(
                    *m_ruleIndexPartials);
        }
    }

    snapshot->ReplaceDocumentSymbols(uriStr, staging);
    return snapshot;
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
            }
            freshClassKeys.push_back(key);
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

void SymbolTable::CleanSynthesizedSymbolsForClassesLocked(const std::vector<std::string>& classKeys)
{
    for (const auto& key : classKeys)
    {
        auto it = m_symbols.find(key);
        if (it == m_symbols.end() || !it->second)
        {
            continue;
        }
        for (const auto& sym : *it->second)
        {
            if (sym.type != SymbolType::Class)
            {
                continue;
            }
            const std::string hostQName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            const auto fileIt = m_keysByFile.find(sym.fileUri);
            if (fileIt == m_keysByFile.end())
            {
                continue;
            }
            const std::string prefix = hostQName + "::";
            std::vector<std::string> keysToClean;
            for (const auto& k : fileIt->second)
            {
                if (k.starts_with(prefix))
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
                fileIt->second.erase(k);
            }
        }
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
    const std::string synthKey = hostQName + "::" + mSym.name;
    if (HasSynthesizedMemberConflictLocked(synthKey, mSym))
    {
        return;
    }

    Symbol synth = mSym;
    synth.containerName = mSym.containerName.empty() ? mixinName : mSym.containerName;
    synth.qualifiedName = synthKey;
    synth.fileUri = mSym.fileUri;
    synth.isSynthesized = true;
    if (m_virtualMixinDocumentsEnabled)
    {
        synth.virtualFileUri = BuildVirtualMixinUri(hostQName, mixinName);
    }

    MutableBucket(m_symbols[synthKey]).push_back(std::move(synth));
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
    if (classKeys.empty())
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

std::string SymbolTable::ExtractVirtualHostClass(std::string_view uri)
{
    const auto colonPos = uri.find(':');
    if (colonPos == std::string_view::npos)
    {
        return "";
    }

    std::string_view scheme = uri.substr(0, colonPos);
    if (scheme != "angelscript-virtual")
    {
        return "";
    }

    std::string_view rest = uri.substr(colonPos + 1);
    while (!rest.empty() && rest.front() == '/')
    {
        rest.remove_prefix(1);
    }

    auto slashPos = rest.find('/');
    if (slashPos == std::string_view::npos)
    {
        return "";
    }

    return utils::UrlDecode(rest.substr(0, slashPos));
}

std::string SymbolTable::ExtractVirtualMixinName(std::string_view uri)
{
    const auto colonPos = uri.find(':');
    if (colonPos == std::string_view::npos)
    {
        return "";
    }

    std::string_view scheme = uri.substr(0, colonPos);
    if (scheme != "angelscript-virtual")
    {
        return "";
    }

    std::string_view rest = uri.substr(colonPos + 1);
    while (!rest.empty() && rest.front() == '/')
    {
        rest.remove_prefix(1);
    }

    auto slashPos = rest.find('/');
    std::string_view mixinPart = (slashPos != std::string_view::npos) ? rest.substr(slashPos + 1) : rest;
    if (mixinPart.ends_with(".as"))
    {
        mixinPart.remove_suffix(3);
    }

    return utils::UrlDecode(mixinPart);
}

bool SymbolTable::HasDocumentSymbols(const std::string& fileUri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_keysByFile.find(fileUri);
    return it != m_keysByFile.end() && !it->second.empty();
}

static inline std::string_view CleanScope(std::string_view name)
{
    if (name.starts_with("::"))
        return name.substr(2);
    return name;
}

bool SymbolTable::HasSymbol(std::string_view qualifiedName) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::string_view search = CleanScope(qualifiedName);
    return m_symbols.contains(search);
}

std::shared_ptr<const std::vector<Symbol>> SymbolTable::FindSymbolsPtr(std::string_view qualifiedName) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::string_view search = CleanScope(qualifiedName);
    auto it = m_symbols.find(search);
    return it != m_symbols.end() ? it->second : nullptr;
}

std::shared_ptr<const std::vector<Symbol>> SymbolTable::FindMemberSymbolPtr(std::string_view scope,
                                                                            std::string_view member) const
{
    if (scope.empty() || member.empty())
    {
        return nullptr;
    }
    constexpr size_t kStackBufSize = 256;
    const size_t totalLen = scope.size() + 2 + member.size();
    if (totalLen < kStackBufSize)
    {
        char buf[kStackBufSize];
        std::memcpy(buf, scope.data(), scope.size());
        buf[scope.size()] = ':';
        buf[scope.size() + 1] = ':';
        std::memcpy(buf + scope.size() + 2, member.data(), member.size());
        return FindSymbolsPtr(std::string_view(buf, totalLen));
    }
    std::string key;
    key.reserve(totalLen);
    key.append(scope).append("::").append(member);
    return FindSymbolsPtr(std::string_view(key));
}

std::vector<Symbol> SymbolTable::FindSymbols(std::string_view qualifiedName) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::string_view search = CleanScope(qualifiedName);
    auto it = m_symbols.find(search);
    return it != m_symbols.end() ? *it->second : std::vector<Symbol>{};
}

std::optional<Symbol> SymbolTable::FindFirstSymbol(std::string_view qualifiedName) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::string_view search = CleanScope(qualifiedName);
    auto it = m_symbols.find(search);
    if (it != m_symbols.end() && !it->second->empty())
        return it->second->front();
    return std::nullopt;
}

std::optional<Symbol> SymbolTable::LookupSymbol(std::string_view name) const
{
    return FindFirstSymbol(name);
}

std::vector<Symbol> SymbolTable::FindTypeSymbolsByShortName(const std::string& shortName) const
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
    std::string_view searchName = CleanScope(name);

    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        if (m_symbols.find(searchName) != m_symbols.end() || m_symbols.find(name) != m_symbols.end())
            return true;
    }

    const auto index = GetRuleIndex();
    return index && (index->allNames.contains(searchName) || index->allNames.contains(name));
}

void SymbolTable::ForEachSymbolWithPrefix(
    std::string_view prefix, const std::function<void(const std::string&, const std::vector<Symbol>&)>& visitor) const
{
    std::vector<std::pair<const std::string*, std::shared_ptr<const std::vector<Symbol>>>> snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        if (prefix.empty())
        {
            snapshot.reserve(m_symbols.size());
            for (const auto& [key, symbols] : m_symbols)
            {
                snapshot.emplace_back(&key, symbols);
            }
        }
        else
        {
            for (const auto& [key, symbols] : m_symbols)
            {
                if (key.starts_with(prefix) ||
                    (key.starts_with("get_") && std::string_view(key).substr(4).starts_with(prefix)) ||
                    (key.starts_with("set_") && std::string_view(key).substr(4).starts_with(prefix)))
                {
                    snapshot.emplace_back(&key, symbols);
                }
            }
        }
    }

    for (const auto& [key, symbols] : snapshot)
    {
        visitor(*key, *symbols);
    }
}

void SymbolTable::ForEachSymbol(
    const std::function<void(const std::string&, const std::vector<Symbol>&)>& visitor) const
{
    ForEachSymbolWithPrefix({}, visitor);
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

namespace
{
void PrintFunctionSymbol(angel_lsp::utils::LspLogger* logger, const Symbol& sym, const std::string& typeStr,
                         const std::string& range)
{
    const auto& sig = sym.GetFunction();
    const std::string nameStr = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    std::vector<std::string> flags;
    if (sig.modifiers.isShared)
        flags.push_back("shared");
    if (sig.modifiers.isOverride)
        flags.push_back("override");
    if (sig.modifiers.isFinal)
        flags.push_back("final");
    if (sig.modifiers.isReturnReference)
        flags.push_back("ref_return");
    if (sig.modifiers.isDelete)
        flags.push_back("delete");
    if (sig.modifiers.isExternal)
        flags.push_back("external");
    if (sig.modifiers.isExplicit)
        flags.push_back("explicit");

    std::string paramsStr;
    std::vector<std::string> paramLines;
    BuildParamStrings(sig.parameters, paramsStr, paramLines);

    logger->LogInfo(fmt::format(
        "  \u2022 [{}] Access: {} | Ret: \"{}\" | Name: \"{}\" | Params: \"{}\" | Flags: {} | {}", typeStr,
        AccessModifierToString(sig.modifiers.access), sig.returnType, nameStr, paramsStr, JoinFlags(flags), range));

    for (const auto& line : paramLines)
        logger->LogInfo(line);
}

void PrintVariableSymbol(angel_lsp::utils::LspLogger* logger, const Symbol& sym, const std::string& typeStr,
                         const std::string& range)
{
    const auto& sig = sym.GetVariable();
    const std::string nameStr = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    std::vector<std::string> flags;
    if (sig.modifiers.isShared)
        flags.push_back("shared");
    if (sig.modifiers.isConst)
        flags.push_back("const");
    if (sig.modifiers.isHandle)
        flags.push_back("handle");

    std::string defaultStr = sig.defaultValue.empty() ? "" : fmt::format(" | Default: \"{}\"", sig.defaultValue);

    logger->LogInfo(fmt::format("  \u2022 [{}] Access: {} | Type: \"{}\" | Name: \"{}\"{}  | Flags: {} | {}", typeStr,
                                AccessModifierToString(sig.modifiers.access), sig.typeName, nameStr, defaultStr,
                                JoinFlags(flags), range));
}

void PrintClassSymbol(angel_lsp::utils::LspLogger* logger, const Symbol& sym, const std::string& typeStr,
                      const std::string& range)
{
    const auto& sig = sym.GetClass();
    const std::string nameStr = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    std::vector<std::string> flags;
    if (sig.modifiers.isShared)
        flags.push_back("shared");
    if (sig.modifiers.isMixin)
        flags.push_back("mixin");
    if (sig.modifiers.isAbstract)
        flags.push_back("abstract");
    if (sig.modifiers.isFinal)
        flags.push_back("final");

    std::string basesStr = sig.bases.empty() ? "" : fmt::format(" | Bases: \"{}\"", JoinStrings(sig.bases));

    logger->LogInfo(fmt::format("  \u2022 [{}] Access: {} | Name: \"{}\"{}  | Flags: {} | {}", typeStr,
                                AccessModifierToString(sig.modifiers.access), nameStr, basesStr, JoinFlags(flags),
                                range));
}

void PrintInterfaceSymbol(angel_lsp::utils::LspLogger* logger, const Symbol& sym, const std::string& typeStr,
                          const std::string& range)
{
    const auto& sig = sym.GetInterface();
    const std::string nameStr = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    std::vector<std::string> flags;
    if (sig.modifiers.isShared)
        flags.push_back("shared");

    std::string basesStr =
        sig.inheritedInterfaces.empty() ? "" : fmt::format(" | Extends: \"{}\"", JoinStrings(sig.inheritedInterfaces));

    logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\"{}  | Flags: {} | {}", typeStr, nameStr, basesStr,
                                JoinFlags(flags), range));
}

void PrintOtherSymbol(angel_lsp::utils::LspLogger* logger, const Symbol& sym, const std::string& typeStr,
                      const std::string& range)
{
    const std::string nameStr = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    switch (sym.type)
    {
    case SymbolType::Enum:
    {
        const auto& sig = sym.GetEnum();
        std::vector<std::string> flags;
        if (sig.modifiers.isShared)
            flags.push_back("shared");
        logger->LogInfo(
            fmt::format("  \u2022 [{}] Name: \"{}\" | Flags: {} | {}", typeStr, nameStr, JoinFlags(flags), range));
        break;
    }
    case SymbolType::CallReference:
    {
        const auto& sig = sym.GetCallReference();
        std::string objStr = sig.objectExpression.empty() ? "" : fmt::format(" | Object: \"{}\"", sig.objectExpression);
        logger->LogInfo(fmt::format("  \u2022 [{}] Callee: \"{}\"{}  | Method: {} | {}", typeStr, sig.calleeName,
                                    objStr, sig.isMethodCall ? "true" : "false", range));
        break;
    }
    default:
        logger->LogInfo(fmt::format("  \u2022 [{}] Name: \"{}\" | {}", typeStr, nameStr, range));
        break;
    }
}

void PrintSingleSymbol(angel_lsp::utils::LspLogger* logger, const Symbol& sym)
{
    const std::string typeStr = SymbolTypeToString(sym.type);
    const std::string range = fmt::format("[L{}:C{}-L{}:C{}]", sym.startLine + 1, sym.startCharacter + 1,
                                          sym.endLine + 1, sym.endCharacter + 1);

    switch (sym.type)
    {
    case SymbolType::Function:
        PrintFunctionSymbol(logger, sym, typeStr, range);
        break;
    case SymbolType::Variable:
        PrintVariableSymbol(logger, sym, typeStr, range);
        break;
    case SymbolType::Class:
        PrintClassSymbol(logger, sym, typeStr, range);
        break;
    case SymbolType::Interface:
        PrintInterfaceSymbol(logger, sym, typeStr, range);
        break;
    default:
        PrintOtherSymbol(logger, sym, typeStr, range);
        break;
    }
}
} // namespace

void SymbolTable::PrintSymbols(angel_lsp::utils::LspLogger* logger) const
{
    if (!logger)
        return;

    std::shared_lock<std::shared_mutex> lock(m_mutex);
    logger->LogInfo(fmt::format("=== SYMBOL TABLE ({} unique keys) ===", m_symbols.size()));

    for (const auto& [key, symbols] : m_symbols)
    {
        for (const auto& sym : *symbols)
        {
            PrintSingleSymbol(logger, sym);
        }
    }
}

namespace
{
inline void HashCombine(uint64_t& h, uint64_t val)
{
    h ^= val + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
}

inline void HashString(uint64_t& h, std::string_view s)
{
    uint64_t strHash = ankerl::unordered_dense::hash<std::string_view>{}(s);
    HashCombine(h, strHash);
}

inline void HashModifiers(uint64_t& h, const SymbolModifiers& m)
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

inline void HashParameter(uint64_t& h, const ParameterInformation& p)
{
    HashString(h, p.name);
    HashString(h, p.typeName);
    HashCombine(h, static_cast<uint64_t>(p.typeKind));
    uint32_t flags = (p.isArray ? 1 : 0) | ((p.isConst ? 1 : 0) << 1) | ((p.isReference ? 1 : 0) << 2) |
                     ((p.isHandle ? 1 : 0) << 3) | (static_cast<uint32_t>(p.modifier) << 4);
    HashCombine(h, flags);
    HashString(h, p.defaultValue);
}

void HashFunctionSignature(uint64_t& h, const FunctionSignature& fn)
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

void HashVariableSignature(uint64_t& h, const VariableSignature& v)
{
    HashString(h, v.typeName);
    HashCombine(h, static_cast<uint64_t>(v.typeKind));
    uint32_t vFlags = (v.isArray ? 1 : 0) | ((v.isVirtualProperty ? 1 : 0) << 1) | ((v.hasGet ? 1 : 0) << 2) |
                      ((v.hasSet ? 1 : 0) << 3);
    HashCombine(h, vFlags);
    HashModifiers(h, v.modifiers);
}

void HashClassSignature(uint64_t& h, const ClassSignature& cls)
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
}

void HashInterfaceSignature(uint64_t& h, const InterfaceSignature& iface)
{
    HashModifiers(h, iface.modifiers);
    for (const auto& b : iface.inheritedInterfaces)
    {
        HashString(h, b);
    }
}

void HashEnumSignature(uint64_t& h, const EnumSignature& enm)
{
    HashModifiers(h, enm.modifiers);
    for (const auto& m : enm.members)
    {
        HashString(h, m.name);
        HashString(h, m.value);
    }
}

void HashFuncdefSignature(uint64_t& h, const FuncdefSignature& fd)
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

void HashSymbolInterface(uint64_t& h, const Symbol& sym)
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
} // namespace

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
} // namespace angel_lsp::analysis