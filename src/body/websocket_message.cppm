/**
 * @file websocket_message.cppm
 * @brief Provide WebSocketMessage: an owned WebSocket payload together with its frame type.
 */
export module mhttp:websocket_message;

import std;

export namespace mhttp {

    /**
     * @brief One WebSocket message (port of httplib::websocket_message).
     *
     * Carrying the text/binary flag with the bytes avoids separate got_text()/got_binary()
     * queries, and owning the payload decouples the message from the read buffer.
     */
    class WebSocketMessage {
    private:
        std::string m_data;     ///< Payload.
        bool m_binary{ false }; ///< Binary frame; text otherwise.

    public:
        WebSocketMessage() = default;

        /**
         * @brief Create a message.
         * @param data Payload; text messages must be valid UTF-8.
         * @param binary True for a binary frame.
         */
        WebSocketMessage(std::string data, bool binary) : m_data{ std::move(data) }, m_binary{ binary } {}

        [[nodiscard]] auto IsBinary() const noexcept -> bool {
            return m_binary;
        }

        [[nodiscard]] auto IsText() const noexcept -> bool {
            return !m_binary;
        }

        auto SetBinary(bool binary) noexcept -> void {
            m_binary = binary;
        }

        [[nodiscard]] auto Data() const& noexcept -> std::string const& {
            return m_data;
        }

        [[nodiscard]] auto Data() & noexcept -> std::string& {
            return m_data;
        }

        [[nodiscard]] auto Data() && noexcept -> std::string&& {
            return std::move(m_data);
        }

        [[nodiscard]] auto View() const noexcept -> std::string_view {
            return m_data;
        }

        [[nodiscard]] auto Size() const noexcept -> std::size_t {
            return m_data.size();
        }

        [[nodiscard]] auto Empty() const noexcept -> bool {
            return m_data.empty();
        }
    };

} // namespace mhttp
