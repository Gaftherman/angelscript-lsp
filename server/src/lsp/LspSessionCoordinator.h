#pragma once

#include "utils/PositionEncoding.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace lsp
{
class MessageHandler;
}

namespace angel_lsp
{

/**
 * @brief Coordinates LSP server session lifecycle, capabilities negotiation,
 *        and transport message dispatching.
 */
class LspSessionCoordinator
{
  public:
    /**
     * @brief Constructs an LspSessionCoordinator with default session parameters.
     */
    LspSessionCoordinator();

    /**
     * @brief Destructor.
     */
    ~LspSessionCoordinator() = default;

    LspSessionCoordinator(const LspSessionCoordinator&) = delete;
    LspSessionCoordinator& operator=(const LspSessionCoordinator&) = delete;
    LspSessionCoordinator(LspSessionCoordinator&&) = delete;
    LspSessionCoordinator& operator=(LspSessionCoordinator&&) = delete;

    /**
     * @brief Marks the server session as active/running.
     */
    void Start() noexcept;

    /**
     * @brief Requests session shutdown, stopping the message processing loop.
     */
    void RequestShutdown() noexcept;

    /**
     * @brief Checks if the LSP session is currently running.
     * @return True if session is running.
     */
    [[nodiscard]] bool IsRunning() const noexcept;

    /**
     * @brief Marks whether initial background workspace scan has finished.
     * @param[in] complete True if initial scan is complete.
     */
    void SetWorkspaceScanComplete(bool complete) noexcept;

    /**
     * @brief Checks if initial background workspace scan has finished.
     * @return True if scan is complete.
     */
    [[nodiscard]] bool IsWorkspaceScanComplete() const noexcept;

    /**
     * @brief Sets barrier signaling that predefined stubs and engine profiles are loaded.
     * @param[in] ready True when predefined files are fully indexed.
     */
    void SetPredefinedReady(bool ready);

    /**
     * @brief Checks if predefined stubs and engine profiles have completed loading.
     * @return True if predefined stubs are ready.
     */
    [[nodiscard]] bool IsPredefinedReady() const noexcept;

    /**
     * @brief Blocks calling thread until predefined stubs and engine profiles are loaded.
     */
    void WaitUntilPredefinedReady() const;

    /**
     * @brief Blocks calling thread until predefined stubs are loaded or timeout elapses.
     * @param[in] timeout Maximum duration to wait.
     * @return True if ready, false if timed out.
     */
    bool WaitUntilPredefinedReady(std::chrono::milliseconds timeout) const;

    /**
     * @brief Configures whether formatter uses K&R brace placement.
     * @param[in] kr True for K&R style, false for Allman style.
     */
    void SetFormatBraceStyleKR(bool kr) noexcept;

    /**
     * @brief Checks whether formatter is configured for K&R brace placement.
     * @return True if K&R style, false if Allman.
     */
    [[nodiscard]] bool GetFormatBraceStyleKR() const noexcept;

    /**
     * @brief Atomically increments and returns the configuration revision counter.
     * @return Incremented revision number.
     */
    uint64_t IncrementConfigRevision() noexcept;

    /**
     * @brief Gets current configuration revision number.
     * @return Configuration revision integer.
     */
    [[nodiscard]] uint64_t GetConfigRevision() const noexcept;

    /**
     * @brief Sets negotiated LSP position encoding (UTF-8, UTF-16, UTF-32).
     * @param[in] encoding Negotiated position encoding.
     */
    void SetPositionEncoding(utils::PositionEncoding encoding) noexcept;

    /**
     * @brief Gets active LSP position encoding.
     * @return Current position encoding.
     */
    [[nodiscard]] utils::PositionEncoding GetPositionEncoding() const noexcept;

    /**
     * @brief Records whether client supports completion snippet syntax.
     * @param[in] supported True if snippets are supported.
     */
    void SetClientSupportsSnippets(bool supported) noexcept;

    /**
     * @brief Checks whether client supports completion snippet syntax.
     * @return True if snippets supported.
     */
    [[nodiscard]] bool GetClientSupportsSnippets() const noexcept;

    /**
     * @brief Records whether client supports Markdown documentation markup.
     * @param[in] supported True if Markdown supported.
     */
    void SetClientSupportsMarkdown(bool supported) noexcept;

    /**
     * @brief Checks whether client supports Markdown documentation markup.
     * @return True if Markdown supported.
     */
    [[nodiscard]] bool GetClientSupportsMarkdown() const noexcept;

    /**
     * @brief Records whether client pulls diagnostics rather than server pushing them.
     * @param[in] pulls True if client pulls diagnostics.
     */
    void SetClientPullsDiagnostics(bool pulls) noexcept;

    /**
     * @brief Checks whether client pulls diagnostics.
     * @return True if client pulls diagnostics.
     */
    [[nodiscard]] bool GetClientPullsDiagnostics() const noexcept;

    /**
     * @brief Records whether client supports workspace/diagnostic/refresh.
     * @param[in] refresh True if client supports refresh.
     */
    void SetClientSupportsDiagnosticRefresh(bool refresh) noexcept;

    /**
     * @brief Checks whether client supports workspace/diagnostic/refresh.
     * @return True if refresh supported.
     */
    [[nodiscard]] bool GetClientSupportsDiagnosticRefresh() const noexcept;

    /**
     * @brief Processes a single iteration of incoming transport messages.
     * @param[in,out] handler Message handler to process incoming RPCs.
     * @return True if message processed without error, false on failure or transport closed.
     */
    bool ProcessMessageStep(lsp::MessageHandler& handler);

  private:
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_workspaceScanComplete{false};
    std::atomic<bool> m_predefinedReady{false};
    mutable std::mutex m_predefinedMutex;
    mutable std::condition_variable m_predefinedCv;

    std::atomic<bool> m_formatBraceStyleKR{false};
    std::atomic<uint64_t> m_configRevision{0};

    utils::PositionEncoding m_positionEncoding{utils::PositionEncoding::Utf16};
    bool m_clientSupportsSnippets{false};
    bool m_clientSupportsMarkdown{false};
    bool m_clientPullsDiagnostics{false};
    bool m_clientSupportsDiagnosticRefresh{false};
};

} // namespace angel_lsp
