// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/core/io.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace helios::core {

Result<std::vector<std::uint8_t>> readBinaryFile(std::string_view path, std::uint64_t max_size) {
    std::error_code ec;
    const auto size = fs::file_size(std::string(path), ec);
    if (ec) {
        return makeError(ErrorCode::Io, "cannot stat file: " + std::string(path) + " (" + ec.message() + ")");
    }
    if (size > max_size) {
        return makeError(ErrorCode::OutOfRange,
                         "file exceeds size limit (" + std::to_string(size) + " > " + std::to_string(max_size) +
                             "): " + std::string(path));
    }
    std::ifstream in(std::string(path), std::ios::binary);
    if (!in) {
        return makeError(ErrorCode::Io, "cannot open file for reading: " + std::string(path));
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!in && !in.eof()) {
        return makeError(ErrorCode::Io, "read failed: " + std::string(path));
    }
    return data;
}

Result<std::string> readTextFile(std::string_view path, std::uint64_t max_size) {
    auto bytes = readBinaryFile(path, max_size);
    if (bytes.failed()) {
        return bytes.error();
    }
    return std::string(bytes.value().begin(), bytes.value().end());
}

Result<void> writeFileAtomic(std::string_view path, void const* data, std::size_t size) {
    const fs::path target(std::string(path));
    const fs::path dir = target.parent_path().empty() ? fs::path(".") : target.parent_path();
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return makeError(ErrorCode::Io, "directory does not exist: " + dir.string());
    }
    static std::atomic<std::uint64_t> counter{0};
    const fs::path tmp = dir / (target.filename().string() + ".tmp" +
                                std::to_string(counter.fetch_add(1)) + "." + std::to_string(std::hash<std::string>{}(std::string(path))));
    std::uint64_t
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return makeError(ErrorCode::Io, "cannot open temp file for writing: " + tmp.string());
        }
        out.write(static_cast<char const*>(data), static_cast<std::streamsize>(size));
        out.flush();
        if (!out) {
            fs::remove(tmp, ec);
            return makeError(ErrorCode::Io, "write failed: " + tmp.string());
        }
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        // Windows rename-over-existing requires remove first.
        fs::remove(target, ec);
        fs::rename(tmp, target, ec);
        if (ec) {
            return makeError(ErrorCode::Io, "rename failed: " + target.string() + " (" + ec.message() + ")");
        }
    }
    return Result<void>{};
}

Result<void> writeTextFileAtomic(std::string_view path, std::string_view text) {
    return writeFileAtomic(path, text.data(), text.size());
}

bool fileExists(std::string_view path) {
    std::error_code ec;
    return fs::exists(std::string(path), ec);
}

} // namespace helios::core
