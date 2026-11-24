#include "ipc/json_server.h"

#include "core/engine.h"
#include "disk/image_file.h"
#include "ipc/operations.h"

#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <iostream>

namespace raid::ipc {
namespace {

constexpr std::size_t maxRequest = 32 * 1024;
constexpr std::size_t maxResponse = 8 * 1024 * 1024;
constexpr unsigned protocolVersion = 1;

enum class Line { Complete, TooLarge, Eof };

Line readRequest(std::string& line) {
    line.clear();
    bool tooLarge = false;
    for (int ch; (ch = std::cin.get()) != EOF;) {
        if (ch == '\n') {
            return tooLarge ? Line::TooLarge : Line::Complete;
        }
        if (line.size() < maxRequest) {
            line.push_back(static_cast<char>(ch));
        } else {
            tooLarge = true;
        }
    }
    // An unterminated frame is never dispatched, even when it contains valid JSON.
    return Line::Eof;
}

bool validRequestId(std::string_view id) {
    return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_';
    });
}

Json failure(const Json& id, const Error& error) {
    return {{"request_id", id}, {"protocol_version", protocolVersion}, {"ok", false},
            {"error", {{"code", error.code}, {"message", error.what()}}}};
}

Json dispatch(Operations& operations, const std::string& line, bool& stopping) {
    Json id = nullptr;
    std::string operation;
    try {
        const auto request = Json::parse(line, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 24) {
                throw Error("INVALID_REQUEST", "JSON nesting limit exceeded.");
            }
            return true;
        });
        if (!request.is_object() || !request.contains("request_id") || !request["request_id"].is_string() ||
            !validRequestId(request["request_id"].get_ref<const std::string&>())) {
            throw Error("INVALID_REQUEST", "A valid request_id is required.");
        }
        id = request["request_id"];
        if (request.contains("protocol_version") &&
            (!request["protocol_version"].is_number_integer() || request["protocol_version"] != protocolVersion)) {
            throw Error("PROTOCOL_VERSION_UNSUPPORTED", "This engine supports protocol version 1.");
        }
        if (!request.contains("operation") || !request["operation"].is_string() ||
            !request.contains("params") || !request["params"].is_object()) {
            throw Error("INVALID_REQUEST", "Expected a string operation and an object of parameters.");
        }
        operation = request["operation"].get<std::string>();
        if (operation.size() > 64) {
            throw Error("UNKNOWN_OPERATION", "Operation is not supported.");
        }
        auto data = operations.execute(operation, request["params"]);
        stopping = operation == "engine.shutdown";
        return {{"request_id", id}, {"protocol_version", protocolVersion}, {"ok", true}, {"data", std::move(data)}};
    } catch (const Json::parse_error&) {
        return failure(id, Error("MALFORMED_JSON", "The request is not valid JSON."));
    } catch (const Error& error) {
        return failure(id, error);
    } catch (const std::exception& error) {
        // Avoid logging the raw frame or user-supplied content. Public errors use sanitized messages.
        const auto publicError = storageError(operation, error);
        std::cerr << "Storage dispatch " << publicError.code << ": "
                  << std::string_view(error.what()).substr(0, 1500) << '\n';
        return failure(id, publicError);
    }
}

}  // namespace

int serve(const std::filesystem::path& root) {
    StorageEngine lifetime;
    lifetime.initialize(root);
    disk_file::prepareDirectory(root, false);
    disk_file::prepareDirectory(root / config::defaultDataDirectory, true);
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) {
        throw std::runtime_error("Cannot configure JSON pipes");
    }
    Operations operations(root);
    std::string line;
    bool stopping = false;
    while (!stopping) {
        const auto result = readRequest(line);
        if (result == Line::Eof) {
            break;
        }
        auto response = result == Line::TooLarge
            ? failure(nullptr, Error("REQUEST_TOO_LARGE", "Request exceeds 32 KiB."))
            : dispatch(operations, line, stopping);
        auto encoded = response.dump(-1, ' ', false, Json::error_handler_t::replace);
        if (encoded.size() > maxResponse) {
            encoded = failure(response["request_id"], Error("RESPONSE_TOO_LARGE", "Response exceeds 8 MiB.")).dump();
        }
        if (!(std::cout << encoded << '\n' << std::flush)) {
            break;
        }
    }
    operations.shutdown();
    lifetime.shutdown();
    return 0;
}

}  // namespace raid::ipc
