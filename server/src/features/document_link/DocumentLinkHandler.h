#pragma once

#include "analysis/diagnostics/Diagnostics.h"
#include "analysis/index/NodeIndex.h"
#include "i18n/i18n.h"
#include "utils/PreprocessorRegions.h"

#include <lsp/types.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Zero-allocation string view holder that safely retains temporary extension strings if provided.
 */
struct DocumentLinkExtension
{
    /**
     * @brief Default constructor creating an empty extension.
     */
    DocumentLinkExtension() = default;

    /**
     * @brief Constructs from a string view without allocation.
     * @param[in] sv String view to reference.
     */
    constexpr DocumentLinkExtension(std::string_view sv) noexcept : m_view(sv)
    {
    }

    /**
     * @brief Constructs from a C-string without allocation.
     * @param[in] s Null-terminated string or nullptr.
     */
    constexpr DocumentLinkExtension(const char* s) noexcept : m_view(s ? s : "")
    {
    }

    /**
     * @brief Constructs from an lvalue string reference without allocation.
     * @param[in] s String reference to view.
     */
    DocumentLinkExtension(const std::string& s) noexcept : m_view(s)
    {
    }

    /**
     * @brief Constructs from an rvalue string by taking ownership into local storage.
     * @param[in,out] s Temporary string to take ownership of.
     */
    DocumentLinkExtension(std::string&& s) noexcept : m_storage(std::move(s)), m_view(m_storage)
    {
    }

    /**
     * @brief Copy constructor updating view to point to local storage if active.
     * @param[in] other Object to copy.
     */
    DocumentLinkExtension(const DocumentLinkExtension& other) : m_storage(other.m_storage)
    {
        m_view = m_storage.empty() ? other.m_view : std::string_view(m_storage);
    }

    /**
     * @brief Move constructor updating view to point to local storage if active.
     * @param[in,out] other Object to move from.
     */
    DocumentLinkExtension(DocumentLinkExtension&& other) noexcept : m_storage(std::move(other.m_storage))
    {
        m_view = m_storage.empty() ? other.m_view : std::string_view(m_storage);
        other.m_view = {};
    }

    /**
     * @brief Copy assignment operator.
     * @param[in] other Object to copy.
     * @return Reference to this object.
     */
    DocumentLinkExtension& operator=(const DocumentLinkExtension& other)
    {
        if (this != &other)
        {
            m_storage = other.m_storage;
            m_view = m_storage.empty() ? other.m_view : std::string_view(m_storage);
        }
        return *this;
    }

    /**
     * @brief Move assignment operator.
     * @param[in,out] other Object to move from.
     * @return Reference to this object.
     */
    DocumentLinkExtension& operator=(DocumentLinkExtension&& other) noexcept
    {
        if (this != &other)
        {
            m_storage = std::move(other.m_storage);
            m_view = m_storage.empty() ? other.m_view : std::string_view(m_storage);
            other.m_view = {};
        }
        return *this;
    }

    /**
     * @brief Assigns a string view without allocation.
     * @param[in] sv String view to reference.
     * @return Reference to this object.
     */
    DocumentLinkExtension& operator=(std::string_view sv) noexcept
    {
        if (m_storage.empty() || sv.data() < m_storage.data() || sv.data() >= m_storage.data() + m_storage.size())
        {
            m_storage.clear();
            m_view = sv;
        }
        return *this;
    }

    /**
     * @brief Assigns an lvalue string reference without allocation.
     * @param[in] s String reference to view.
     * @return Reference to this object.
     */
    DocumentLinkExtension& operator=(const std::string& s) noexcept
    {
        if (&m_storage != &s)
        {
            m_storage.clear();
            m_view = s;
        }
        return *this;
    }

    /**
     * @brief Assigns an rvalue string by taking ownership into local storage.
     * @param[in,out] s Temporary string to take ownership of.
     * @return Reference to this object.
     */
    DocumentLinkExtension& operator=(std::string&& s) noexcept
    {
        if (&m_storage != &s)
        {
            m_storage = std::move(s);
            m_view = m_storage;
        }
        return *this;
    }

    /**
     * @brief Implicit conversion to std::string_view.
     * @return String view referencing the held string data.
     */
    [[nodiscard]] operator std::string_view() const noexcept
    {
        return m_view;
    }

    /**
     * @brief Checks whether the extension is empty.
     * @return True if empty; false otherwise.
     */
    [[nodiscard]] bool empty() const noexcept
    {
        return m_view.empty();
    }

    /**
     * @brief Returns a pointer to the character data.
     * @return Pointer to buffer.
     */
    [[nodiscard]] const char* data() const noexcept
    {
        return m_view.data();
    }

    /**
     * @brief Returns the length of the extension string.
     * @return Character count.
     */
    [[nodiscard]] size_t size() const noexcept
    {
        return m_view.size();
    }

    /**
     * @brief Returns the view representation.
     * @return String view of the held extension.
     */
    [[nodiscard]] std::string_view view() const noexcept
    {
        return m_view;
    }

  private:
    std::string m_storage;
    std::string_view m_view;
};

/**
 * @brief Input for both `textDocument/documentLink` and the unresolved-include check.
 */
struct DocumentLinkRequest
{
    /**
     * @brief Constructs a DocumentLinkRequest.
     * @param[in] u Source document URI.
     * @param[in] sc Document source code.
     * @param[in] sd Ordered list of include search directories.
     * @param[in] i18nPtr Optional localization provider pointer.
     */
    DocumentLinkRequest(std::string_view u, std::string_view sc, const std::vector<std::string>& sd,
                        const angel_lsp::i18n::I18n* i18nPtr = nullptr)
        : uri(u), sourceCode(sc), searchDirectories(sd), i18n(i18nPtr)
    {
    }

    std::string_view uri;
    std::string_view sourceCode;
    const std::vector<std::string>& searchDirectories;
    const angel_lsp::i18n::I18n* i18n = nullptr;

    /**
     * @brief Directories an `#include` is permitted to resolve into. Empty means unconfined.
     */
    std::vector<std::string> allowedRoots;

    /**
     * @brief Lines the preprocessor removes, so a directive inside one is not a directive.
     */
    std::vector<utils::ExcludedLineRange> excludedLineRanges;

    /**
     * @brief Suffix an unresolvable include may be retried with, or empty for none.
     */
    DocumentLinkExtension implicitExtension;

    /**
     * @brief Optional pre-computed syntax node index from pass 1.
     */
    const analysis::NodeIndex* nodeIndex = nullptr;
};

using DocumentLinkResult = std::vector<lsp::DocumentLink>;

/**
 * @brief Turns every resolvable `#include` directive into a clickable link.
 * @param[in] request Document and include search paths.
 * @return One link per resolvable directive, or std::nullopt when the document has none.
 */
std::optional<DocumentLinkResult> GetDocumentLinks(const DocumentLinkRequest& request);

/**
 * @brief Reports `#include` directives that resolve to no file on disk.
 * @param[in] request Document and include search paths.
 * @return One warning per unresolvable directive.
 */
std::vector<analysis::Diagnostic> GetUnresolvedIncludeDiagnostics(const DocumentLinkRequest& request);
} // namespace angel_lsp::features
