#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace raid {

class Sha256 {
public:
    Sha256();
    ~Sha256();
    void update(const void* data, std::size_t size);
    std::string finish();

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace raid
