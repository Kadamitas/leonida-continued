#pragma once

#include <memory>

namespace rt::window_placement {

// Declare after Loader and before its first load. Destruction stops callbacks before guest images are unmapped.
class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace rt::window_placement
