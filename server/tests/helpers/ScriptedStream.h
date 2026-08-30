#pragma once

#include <lsp/io/stream.h>

#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace angel_lsp::test
{
    /**
     * @brief In-memory lsp::io::Stream that replays a scripted request sequence and records replies.
     *
     * The Server owns a real JSON-RPC connection, so the only way to exercise its handlers is to
     * speak the protocol at it. Over stdio that is impossible inside a test binary - the test
     * process needs its own stdin and stdout - which is why Server takes the stream by reference.
     *
     * Reads drain the scripted input and then report end of file by throwing lsp::io::Error, which
     * is what a closed editor looks like to the framework and what makes Server::Run() return.
     */
    class ScriptedStream final : public lsp::io::Stream
    {
    public:
        /** @brief Wraps one JSON body in the Content-Length framing the protocol requires. */
        static std::string Frame(const std::string &jsonBody)
        {
            return "Content-Length: " + std::to_string(jsonBody.size()) + "\r\n\r\n" + jsonBody;
        }

        /** @brief Appends a framed message to the scripted input. */
        void Push(const std::string &jsonBody)
        {
            m_input += Frame(jsonBody);
        }

        /**
         * @brief Schedules a side effect to run once the server has consumed everything pushed so far.
         *
         * Needed because the whole script is built before the server starts: a plain statement
         * between two Push() calls would run before the first message is ever read. Anything that
         * models the world changing mid-session - a file being deleted on disk between two
         * notifications - has to be scheduled here instead.
         */
        void PushAction(std::function<void()> action)
        {
            m_actions.push_back({ m_input.size(), std::move(action) });
        }

        void read(char *buffer, std::size_t size) override
        {
            if (m_readOffset + size > m_input.size())
            {
                // Everything scripted has been consumed. Reported the same way a closed transport
                // is, so the server's message loop ends instead of blocking the test forever.
                throw lsp::io::Error("end of scripted input");
            }

            std::memcpy(buffer, m_input.data() + m_readOffset, size);
            m_readOffset += size;

            RunDueActions();
        }

        void write(const char *buffer, std::size_t size) override
        {
            std::lock_guard<std::mutex> lock(m_outputMutex);
            m_output.append(buffer, size);
        }

        /** @brief Everything the server has written back, framing included. */
        std::string Output() const
        {
            std::lock_guard<std::mutex> lock(m_outputMutex);
            return m_output;
        }

        /** @brief True if the server wrote anything containing the given fragment. */
        bool OutputContains(const std::string &fragment) const
        {
            std::lock_guard<std::mutex> lock(m_outputMutex);
            return m_output.find(fragment) != std::string::npos;
        }

        /**
         * @brief The body of the reply to one request id, or an empty string if there was none.
         *
         * Assertions about a specific answer belong here rather than over the whole transcript:
         * the server also writes log notifications and diagnostics from its background threads, so
         * searching everything makes a test depend on thread timing.
         */
        std::string ResponseFor(int id) const
        {
            std::lock_guard<std::mutex> lock(m_outputMutex);
            const std::string idField = "\"id\":" + std::to_string(id);

            size_t pos = 0;
            while (pos < m_output.size())
            {
                const size_t headerStart = m_output.find("Content-Length:", pos);
                if (headerStart == std::string::npos)
                    break;

                const size_t bodyStart = m_output.find("\r\n\r\n", headerStart);
                if (bodyStart == std::string::npos)
                    break;

                const size_t contentStart = bodyStart + 4;
                const size_t nextHeader = m_output.find("Content-Length:", contentStart);
                const size_t bodyLength = (nextHeader == std::string::npos) ? (m_output.size() - contentStart) : (nextHeader - contentStart);

                std::string body = m_output.substr(contentStart, bodyLength);
                if (body.find(idField) != std::string::npos && (body.find("\"result\"") != std::string::npos || body.find("\"error\"") != std::string::npos))
                {
                    return body;
                }

                pos = contentStart + bodyLength;
            }

            return "";
        }

        /** @brief Number of times a fragment appears in everything written back. */
        size_t CountInOutput(const std::string &fragment) const
        {
            if (fragment.empty())
            {
                return 0;
            }

            std::lock_guard<std::mutex> lock(m_outputMutex);
            size_t count = 0;
            for (size_t pos = m_output.find(fragment); pos != std::string::npos;
                 pos = m_output.find(fragment, pos + fragment.size()))
            {
                ++count;
            }
            return count;
        }

    private:
        /** @brief Fires every scheduled action the reader has now passed, in order, exactly once. */
        void RunDueActions()
        {
            while (m_nextAction < m_actions.size() && m_actions[m_nextAction].offset <= m_readOffset)
            {
                m_actions[m_nextAction].action();
                ++m_nextAction;
            }
        }

        struct ScheduledAction
        {
            std::size_t offset = 0;
            std::function<void()> action;
        };

        std::string m_input;
        std::string m_output;
        mutable std::mutex m_outputMutex;
        std::size_t m_readOffset = 0;
        std::vector<ScheduledAction> m_actions;
        std::size_t m_nextAction = 0;
    };
}
