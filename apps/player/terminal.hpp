#pragma once

namespace ts::player {

/// A key the transport acts on.
enum class Key {
    none,
    space,
    left,
    right,
    comma,
    period,
    home,
    quit,
};

/// Puts the terminal into raw, non-blocking mode for as long as it lives.
///
/// Without this the tty stays line-buffered and no key reaches the transport until Enter is
/// pressed. The original mode is restored on destruction, including when an exception unwinds
/// through the player -- a terminal left in raw mode is unusable afterwards.
///
/// If stdin is not a terminal -- a piped or scripted run -- this does nothing and `poll` always
/// reports `none`, so the song simply plays straight through.
class RawTerminal {
public:
    RawTerminal();
    RawTerminal(const RawTerminal&) = delete;
    RawTerminal& operator=(const RawTerminal&) = delete;
    RawTerminal(RawTerminal&&) = delete;
    RawTerminal& operator=(RawTerminal&&) = delete;
    ~RawTerminal();

    /// Whether keys can be read at all.
    [[nodiscard]] bool interactive() const noexcept { return interactive_; }

    /// Reads the next key without blocking, or `none` if nothing is waiting.
    [[nodiscard]] Key poll() const;

private:
    bool interactive_ = false;
};

} // namespace ts::player
