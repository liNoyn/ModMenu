#pragma once
#include <array>
#include <mutex>

namespace be::input {
// A release belongs to the recipient of its press, even if the menu changed
// visibility in between. In particular, opening Mods must not strand a game
// button in its pressed state, or leak a menu click back into the game.
class Ownership {
    enum Owner : unsigned char { None, Game, Menu };
    std::array<Owner, 256> held{};
    std::mutex mutex;
public:
    bool forward(unsigned code, bool down, bool blocked) {
        std::lock_guard lock(mutex);
        if (code >= held.size()) return !blocked;
        auto& owner = held[code];
        if (down) {
            if (owner == None) owner = blocked ? Menu : Game;
            return !blocked && owner == Game;
        }
        auto previous = owner;
        owner = None;
        return previous == Game || (previous == None && !blocked);
    }
};
}
