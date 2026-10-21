/**
 * @file parser_support.hpp
 * @brief Drive parser events through arbitrary read boundaries with bounded progress checks.
 * @note Include after importing std and mhttp and including test_support.hpp.
 */
#pragma once

namespace mhttp::test {

    /**
     * @brief Own observations from one parsed message independently of the input and parser.
     * @tparam Parser Parser type being observed.
     */
    template <typename Parser>
    struct ParsedMessage {
        std::remove_cvref_t<decltype(std::declval<Parser>().Head())> head; ///< Observed metadata.
        detail::MessageFraming framing; ///< Observed framing after the header event.
        Headers trailers; ///< Allowed trailing fields.
        std::string body; ///< Copied body event bytes.
        std::size_t consumed{}; ///< Total input consumed across all supplied spans.
        std::size_t header_events{}; ///< Number of final header events.
        bool complete{}; ///< A Complete event was observed.
        std::optional<detail::ProtocolFailure> failure; ///< Terminal parse failure, if any.
    };

    /**
     * @brief Drain events from one read buffer without dropping its unconsumed suffix.
     * @tparam Parser Parser type.
     * @param parser Borrowed stateful decoder.
     * @param input Borrowed read buffer.
     * @param observed Owned observation updated by this call.
     */
    template <typename Parser>
    auto drain_parser(
        Parser& parser,
        std::span<char const> input,
        ParsedMessage<Parser>& observed
    ) -> void {
        for (;;) {
            auto const step = parser.Feed(input);
            if (!Check(step.consumed <= input.size(), "parser consumption stays within the supplied span")) {
                return;
            }
            observed.consumed += step.consumed;
            switch (step.event) {
            case detail::ParseEvent::HeadersComplete:
                ++observed.header_events;
                observed.head = parser.Head();
                observed.framing = parser.Framing();
                break;
            case detail::ParseEvent::Body:
                if (!Check(!step.data.empty() && step.data.size() <= step.consumed,
                    "body events contain a nonempty consumed span")) { return; }
                if (!Check(step.data.data() >= input.data()
                    && step.data.data() + step.data.size() <= input.data() + step.consumed,
                    "body view borrows the supplied input")) { return; }
                observed.body.append(step.data.data(), step.data.size());
                break;
            case detail::ParseEvent::Complete:
                observed.complete = true;
                observed.head = parser.Head();
                observed.trailers = parser.Trailers();
                CheckEq(parser.BodyBytes(), static_cast<std::uint64_t>(observed.body.size()),
                    "body counter excludes chunk framing");
                return;
            case detail::ParseEvent::Failed:
                Check(step.failure.has_value(), "failed event includes its error");
                observed.failure = step.failure;
                return;
            case detail::ParseEvent::NeedMore:
                CheckEq(step.consumed, input.size(), "NeedMore consumes the entire supplied span");
                return;
            }
            if (!Check(step.consumed != 0, "nonterminal parser events make progress")) { return; }
            input = input.subspan(step.consumed);
        }
    }

    /**
     * @brief Parse a wire message through specified buffer boundaries, then signal EOF.
     * @tparam Parser Parser type.
     * @param wire Borrowed complete input; may include a pipelined successor.
     * @param cuts Increasing buffer-end offsets, ending at wire.size().
     * @param parser Fresh parser, optionally configured for limits or a response method.
     * @return Owned observations, retaining consumption for pipelined input checks.
     */
    template <typename Parser>
    auto parse_wire(
        std::string_view wire,
        std::span<std::size_t const> cuts,
        Parser parser = Parser{}
    ) -> ParsedMessage<Parser> {
        ParsedMessage<Parser> result;
        std::size_t begin{};
        for (auto end : cuts) {
            drain_parser(parser, std::span{ wire.data(), wire.size() }.subspan(begin, end - begin), result);
            begin = end;
            if (result.complete || result.failure) { break; }
        }
        if (!result.complete && !result.failure) {
            auto const step = parser.Finish();
            result.complete = step.event == detail::ParseEvent::Complete;
            result.failure = step.failure;
            if (result.complete) {
                result.head = parser.Head();
                result.trailers = parser.Trailers();
                CheckEq(parser.BodyBytes(), static_cast<std::uint64_t>(result.body.size()),
                    "EOF-delimited body counter matches delivered bytes");
            }
        }
        return result;
    }

    /**
     * @brief Check every two-piece split and one fully bytewise delivery of a small wire message.
     * @tparam Parser Parser type.
     * @tparam Verify Observation callback type.
     * @param wire Borrowed complete wire message.
     * @param verify Callback invoked with owned observations for each delivery pattern.
     * @param limits Parser limits.
     */
    template <typename Parser, typename Verify>
    auto check_all_splits(
        std::string_view wire,
        Verify verify,
        detail::ParserLimits limits = {}
    ) -> void {
        for (std::size_t split{}; split <= wire.size(); ++split) {
            std::array const cuts{ split, wire.size() };
            verify(parse_wire<Parser>(wire, cuts, Parser{ limits }));
        }
        std::vector<std::size_t> cuts;
        for (std::size_t end{ 1 }; end <= wire.size(); ++end) { cuts.push_back(end); }
        verify(parse_wire<Parser>(wire, cuts, Parser{ limits }));
    }

} // namespace mhttp::test
