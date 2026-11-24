#pragma once

#include <string>
#include <string_view>

namespace raid {

enum class DiskState { Online, Failed, Spare, Rebuilding };
enum class FailureCause { None, Injected, MissingImage, ImageChanged };

struct MemberFailure {
    DiskState state = DiskState::Online;
    FailureCause cause = FailureCause::None;
};

struct MemberStatus {
    std::string id;
    MemberFailure failure;
    bool imagePresent;
    bool available;
};

constexpr std::string_view diskStateName(DiskState state) noexcept {
    switch (state) {
    case DiskState::Online: return "ONLINE";
    case DiskState::Failed: return "FAILED";
    case DiskState::Spare: return "SPARE";
    case DiskState::Rebuilding: return "REBUILDING";
    }
    return "UNKNOWN";
}

constexpr std::string_view failureCauseName(FailureCause cause) noexcept {
    switch (cause) {
    case FailureCause::None: return "NONE";
    case FailureCause::Injected: return "INJECTED";
    case FailureCause::MissingImage: return "MISSING_IMAGE";
    case FailureCause::ImageChanged: return "IMAGE_CHANGED";
    }
    return "UNKNOWN";
}

}  // namespace raid
