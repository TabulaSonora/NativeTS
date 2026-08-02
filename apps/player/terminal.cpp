#include "terminal.hpp"

#include <cstdint>

#ifdef _WIN32
#    include <conio.h>
#    include <io.h>
#    include <stdio.h>
#else
#    include <termios.h>
#    include <unistd.h>
#endif

namespace ts::player {

#ifdef _WIN32

// The console equivalent of raw mode comes for free: _kbhit and _getch talk to the console input
// buffer directly, below stdin's line buffering and echo, so there is no terminal state to take
// on the way in or restore on the way out.

RawTerminal::RawTerminal()
{
    interactive_ = _isatty(_fileno(stdin)) != 0;
}

RawTerminal::~RawTerminal() = default;

Key RawTerminal::poll() const
{
    if (!interactive_ || _kbhit() == 0) {
        return Key::none;
    }

    const int byte = _getch();

    // Arrow and navigation keys arrive as two _getch reads: a 0x00 or 0xE0 prefix, then a scan
    // code. The scan codes are fixed by the console API, not by any terminal's escape dialect.
    if (byte == 0x00 || byte == 0xE0) {
        switch (_getch()) {
        case 'K':
            return Key::left;
        case 'M':
            return Key::right;
        case 'G':
            return Key::home;
        default:
            return Key::none;
        }
    }

    switch (byte) {
    case ' ':
        return Key::space;
    case ',':
        return Key::comma;
    case '.':
        return Key::period;
    case 'q':
    case 'Q':
    case 0x1B:
        // Escape is a whole key here, never the start of a sequence -- the console reports
        // sequences through the prefix path above.
        return Key::quit;
    default:
        return Key::none;
    }
}

#else

namespace {

/// The mode the terminal was in before the player took it, restored on the way out.
termios original{};

/// Reads one byte if one is waiting, without blocking.
[[nodiscard]] bool read_byte(std::uint8_t& byte)
{
    return ::read(STDIN_FILENO, &byte, 1) == 1;
}

} // namespace

RawTerminal::RawTerminal()
{
    if (::isatty(STDIN_FILENO) == 0 || ::tcgetattr(STDIN_FILENO, &original) != 0) {
        return;
    }

    termios raw = original;
    raw.c_lflag = raw.c_lflag & ~static_cast<tcflag_t>(ICANON | ECHO);

    // Return immediately with whatever is waiting, including nothing at all: the transport polls
    // between blocks and must never sit in a read.
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
        interactive_ = true;
    }
}

RawTerminal::~RawTerminal()
{
    if (interactive_) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &original);
    }
}

Key RawTerminal::poll() const
{
    std::uint8_t byte = 0;
    if (!interactive_ || !read_byte(byte)) {
        return Key::none;
    }

    switch (byte) {
    case ' ':
        return Key::space;
    case ',':
        return Key::comma;
    case '.':
        return Key::period;
    case 'q':
    case 'Q':
        return Key::quit;
    case 0x1B:
        break;
    default:
        return Key::none;
    }

    // An escape byte is either Escape itself or the start of a cursor sequence. The two are told
    // apart by what follows immediately: a real key press has the rest of its sequence already in
    // the buffer, while a bare Escape has nothing behind it.
    std::uint8_t second = 0;
    if (!read_byte(second) || (second != '[' && second != 'O')) {
        return Key::quit;
    }

    std::uint8_t third = 0;
    if (!read_byte(third)) {
        return Key::none;
    }

    switch (third) {
    case 'C':
        return Key::right;
    case 'D':
        return Key::left;
    case 'H':
        return Key::home;
    case '1':
    case '7': {
        // Home arrives as `ESC [ 1 ~` or `ESC [ 7 ~` depending on the terminal; swallow the tilde.
        std::uint8_t tilde = 0;
        (void)read_byte(tilde);
        return Key::home;
    }
    default:
        return Key::none;
    }
}

#endif

} // namespace ts::player
