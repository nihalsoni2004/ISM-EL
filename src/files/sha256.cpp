#include "files/sha256.h"

#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace raid {
namespace {

void requireSuccess(NTSTATUS status, const char* action) {
    if (status < 0) {
        throw std::runtime_error(std::string(action) + " failed with CNG status " + std::to_string(status));
    }
}

}  // namespace

struct Sha256::State {
    BCRYPT_ALG_HANDLE provider = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool finished = false;

    ~State() {
        if (hash) {
            BCryptDestroyHash(hash);
        }
        if (provider) {
            BCryptCloseAlgorithmProvider(provider, 0);
        }
    }
};

Sha256::Sha256() : state_(std::make_unique<State>()) {
    requireSuccess(BCryptOpenAlgorithmProvider(&state_->provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0),
                   "Open SHA-256 provider");
    requireSuccess(BCryptCreateHash(state_->provider, &state_->hash, nullptr, 0, nullptr, 0, 0),
                   "Create SHA-256 hash");
}

Sha256::~Sha256() = default;

void Sha256::update(const void* data, std::size_t size) {
    if (state_->finished || (!data && size != 0)) {
        throw std::logic_error("Invalid SHA-256 update");
    }
    auto bytes = const_cast<PUCHAR>(static_cast<const UCHAR*>(data));
    while (size != 0) {
        const auto part = static_cast<ULONG>(std::min<std::size_t>(size, std::numeric_limits<ULONG>::max()));
        requireSuccess(BCryptHashData(state_->hash, bytes, part, 0), "Hash SHA-256 data");
        bytes += part;
        size -= part;
    }
}

std::string Sha256::finish() {
    if (state_->finished) {
        throw std::logic_error("SHA-256 hash was already finalized");
    }
    std::array<UCHAR, 32> digest{};
    requireSuccess(BCryptFinishHash(state_->hash, digest.data(), static_cast<ULONG>(digest.size()), 0),
                   "Finalize SHA-256");
    state_->finished = true;
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto byte : digest) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

}  // namespace raid
