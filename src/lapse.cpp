// lapse — a tiny time machine for any folder
// SPDX-License-Identifier: AGPL-3.0-only
//
// One executable. Zero config. No third-party libraries.
//
//   lapse snap -m "before refactor"     # snapshot the current folder
//   lapse log                           # browse the timeline
//   lapse restore last notes.txt        # bring a file back from the past
//
// Design (deliberately boring):
//   .lapse/objects/aa/bbbb...   files, stored once, named by SHA-256 (dedup)
//   .lapse/snapshots/*.snap     plain-text manifests: hash, mode, mtime, path
//
// Everything is plain files. You can read a snapshot with `cat` and recover
// data with `cp` even if this binary disappears. That is a feature.

#include "sha256.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define LAPSE_ISATTY _isatty
#define LAPSE_FILENO _fileno
#define LAPSE_GETPID _getpid
#else
#include <unistd.h>
#define LAPSE_ISATTY isatty
#define LAPSE_FILENO fileno
#define LAPSE_GETPID getpid
#endif

namespace fs = std::filesystem;
using lapse::Sha256;

namespace {

#if defined(LAPSE_VERSION)
constexpr const char* kVersion = LAPSE_VERSION;
#else
constexpr const char* kVersion = "development";
#endif
constexpr const char* kRepoDir = ".lapse";
constexpr const char* kIgnoreFile = ".lapseignore";

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

bool g_color = false;

#if defined(_WIN32)
std::error_code win32_error(DWORD code) {
    return {static_cast<int>(code), std::system_category()};
}
#endif

std::string c_(const char* code, const std::string& s) {
    if (!g_color) return s;
    return std::string("\033[") + code + "m" + s + "\033[0m";
}
std::string bold(const std::string& s)   { return c_("1", s); }
std::string dim(const std::string& s)    { return c_("2", s); }
std::string green(const std::string& s)  { return c_("32", s); }
std::string yellow(const std::string& s) { return c_("33", s); }
std::string red(const std::string& s)    { return c_("31", s); }
std::string cyan(const std::string& s)   { return c_("36", s); }

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool ascii_case_equal(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(left[i]);
        unsigned char b = static_cast<unsigned char>(right[i]);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + 32);
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + 32);
        if (a != b) return false;
    }
    return true;
}

bool is_lower_hex(const std::string& value, std::size_t length) {
    return value.size() == length &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool has_terminal_control(const std::string& value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(value[i]);
        if (c < 0x20 || c == 0x7f) return true;
        // UTF-8 encodings of the C1 control block U+0080..U+009F.
        if (c == 0xc2 && i + 1 < value.size()) {
            unsigned char next = static_cast<unsigned char>(value[i + 1]);
            if (next >= 0x80 && next <= 0x9f) return true;
        }
    }
    return false;
}

bool is_valid_utf8(const std::string& value) {
    const auto* bytes =
        reinterpret_cast<const unsigned char*>(value.data());
    std::size_t i = 0;
    while (i < value.size()) {
        unsigned char c = bytes[i++];
        if (c <= 0x7f) continue;

        std::size_t continuation_count = 0;
        unsigned char second_min = 0x80;
        unsigned char second_max = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) {
            continuation_count = 1;
        } else if (c == 0xe0) {
            continuation_count = 2;
            second_min = 0xa0;
        } else if ((c >= 0xe1 && c <= 0xec) ||
                   (c >= 0xee && c <= 0xef)) {
            continuation_count = 2;
        } else if (c == 0xed) {
            continuation_count = 2;
            second_max = 0x9f;
        } else if (c == 0xf0) {
            continuation_count = 3;
            second_min = 0x90;
        } else if (c >= 0xf1 && c <= 0xf3) {
            continuation_count = 3;
        } else if (c == 0xf4) {
            continuation_count = 3;
            second_max = 0x8f;
        } else {
            return false;
        }

        if (i + continuation_count > value.size()) return false;
        if (bytes[i] < second_min || bytes[i] > second_max) return false;
        ++i;
        for (std::size_t n = 1; n < continuation_count; ++n, ++i)
            if (bytes[i] < 0x80 || bytes[i] > 0xbf) return false;
    }
    return true;
}

std::string escape_terminal_controls(const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(value[i]);
        if (c == 0xc2 && i + 1 < value.size()) {
            unsigned char next = static_cast<unsigned char>(value[i + 1]);
            if (next >= 0x80 && next <= 0x9f) {
                escaped += "\\u00";
                escaped.push_back(hex[next >> 4]);
                escaped.push_back(hex[next & 0x0f]);
                ++i;
                continue;
            }
        }
        if (c < 0x20 || c == 0x7f) {
            escaped += "\\x";
            escaped.push_back(hex[c >> 4]);
            escaped.push_back(hex[c & 0x0f]);
        } else {
            escaped.push_back(static_cast<char>(c));
        }
    }
    return escaped;
}

std::string display_path(const fs::path& path) {
    return escape_terminal_controls(path.string());
}

bool is_windows_reserved_component(const std::string& part) {
    std::string stem = part.substr(0, part.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
        return true;
    return stem.size() == 4 &&
           (starts_with(stem, "COM") || starts_with(stem, "LPT")) &&
           stem[3] >= '1' && stem[3] <= '9';
}

enum class PathPolicy {
    NativeV1,
    PortableV2,
};

bool is_safe_manifest_path(
    const std::string& value,
    PathPolicy policy = PathPolicy::PortableV2) {
    if (value.empty() || value.find('\0') != std::string::npos ||
        value.find('\n') != std::string::npos ||
        value.find('\r') != std::string::npos)
        return false;
    if (policy == PathPolicy::PortableV2 &&
        (value.find('\\') != std::string::npos || !is_valid_utf8(value) ||
         has_terminal_control(value)))
        return false;

    // Version 1 used native path rules, which made a backslash a valid
    // filename byte on POSIX. Version 2 uses a portable subset everywhere.
#if defined(_WIN32)
    (void)policy;
    bool windows_rules = true;
#else
    bool windows_rules = policy == PathPolicy::PortableV2;
#endif
    std::string separated = value;
    if (windows_rules)
        std::replace(separated.begin(), separated.end(), '\\', '/');
    if (separated.front() == '/' ||
        (windows_rules && separated.size() >= 2 && separated[1] == ':'))
        return false;

    bool first = true;
    std::size_t begin = 0;
    while (begin <= separated.size()) {
        std::size_t end = separated.find('/', begin);
        std::string part = separated.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (part.empty() || part == "." || part == "..") return false;
        if (first) {
            bool repository_component = part == kRepoDir;
            if (policy == PathPolicy::PortableV2 || windows_rules)
                repository_component =
                    repository_component ||
                    ascii_case_equal(part, kRepoDir);
            if (repository_component) return false;
        }
        if (windows_rules) {
            if (part.back() == '.' || part.back() == ' ' ||
                is_windows_reserved_component(part))
                return false;
            for (unsigned char c : part) {
                if (c < 0x20 || c == ':' || c == '<' || c == '>' || c == '"' ||
                    c == '|' || c == '?' || c == '*')
                    return false;
            }
        }
        first = false;
        if (end == std::string::npos) break;
        begin = end + 1;
    }

    fs::path path = fs::u8path(value);
    return path.is_relative() && !path.has_root_name() &&
           !path.has_root_directory();
}

[[noreturn]] void invalid_snapshot(const fs::path& file,
                                   const std::string& reason) {
    throw std::runtime_error("invalid snapshot " + display_path(file) + ": " +
                             reason);
}

std::uint64_t parse_u64(const std::string& value, int base,
                        const fs::path& file, const char* field) {
    std::uint64_t parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    auto result = std::from_chars(begin, end, parsed, base);
    if (value.empty() || result.ec != std::errc{} || result.ptr != end)
        invalid_snapshot(file, std::string("invalid ") + field);
    return parsed;
}

std::int64_t parse_i64(const std::string& value, const fs::path& file,
                       const char* field) {
    std::int64_t parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    auto result = std::from_chars(begin, end, parsed, 10);
    if (value.empty() || result.ec != std::errc{} || result.ptr != end)
        invalid_snapshot(file, std::string("invalid ") + field);
    return parsed;
}

std::string human_size(std::uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char buf[32];
    if (u == 0) std::snprintf(buf, sizeof buf, "%.0f %s", v, units[u]);
    else        std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    return buf;
}

std::string format_time(std::int64_t unix_secs) {
    std::time_t t = static_cast<std::time_t>(unix_secs);
    std::tm tmv{};
#if defined(_WIN32)
    if (localtime_s(&tmv, &t) != 0) return "(invalid time)";
#else
    if (localtime_r(&t, &tmv) == nullptr) return "(invalid time)";
#endif
    char buf[32];
    if (std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv) == 0)
        return "(invalid time)";
    return buf;
}

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[noreturn]] void die(const std::string& msg) {
    std::cerr << red("error: ") << msg << "\n";
    std::exit(1);
}

// Iterative glob: supports '*' (any run, including '/') and '?' (one char).
bool glob_match(const std::string& pat, const std::string& str) {
    std::size_t p = 0, s = 0, star = std::string::npos, ss = 0;
    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) { ++p; ++s; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; ss = s; }
        else if (star != std::string::npos) { p = star + 1; s = ++ss; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

// ---------------------------------------------------------------------------
// Repository layout & discovery
// ---------------------------------------------------------------------------

struct Repo {
    fs::path root;                                  // directory being tracked
    fs::path dir() const       { return root / kRepoDir; }
    fs::path objects() const   { return dir() / "objects"; }
    fs::path snapshots() const { return dir() / "snapshots"; }
};

void set_private_permissions(const fs::path& path, fs::perms permissions) {
#if !defined(_WIN32)
    std::error_code ec;
    fs::permissions(path, permissions, fs::perm_options::replace, ec);
    if (ec)
        throw std::runtime_error("cannot secure " + display_path(path) + ": " +
                                 ec.message());
#else
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD code = GetLastError();
        throw std::runtime_error(
            "cannot inspect the current Windows user while securing " +
            display_path(path) + ": " +
            std::error_code(static_cast<int>(code), std::system_category())
                .message());
    }

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        DWORD code = GetLastError();
        CloseHandle(token);
        throw std::runtime_error(
            "cannot size the Windows user token while securing " +
            display_path(path) + ": " +
            std::error_code(static_cast<int>(code), std::system_category())
                .message());
    }
    std::vector<unsigned char> token_info(needed);
    if (!GetTokenInformation(token, TokenUser, token_info.data(), needed,
                             &needed)) {
        DWORD code = GetLastError();
        CloseHandle(token);
        throw std::runtime_error(
            "cannot read the Windows user token while securing " +
            display_path(path) + ": " +
            std::error_code(static_cast<int>(code), std::system_category())
                .message());
    }
    CloseHandle(token);

    // Windows ACLs protect repository privacy, while the POSIX-style mode is
    // still recorded separately in the manifest. The owner needs full
    // control to prune, repair, and change DOS file attributes later.
    (void)permissions;
    DWORD access_mask = GENERIC_ALL;

    EXPLICIT_ACCESSW access[2]{};
    access[0].grfAccessPermissions = access_mask;
    access[0].grfAccessMode = SET_ACCESS;
    std::error_code status_ec;
    bool directory = fs::is_directory(path, status_ec);
    if (status_ec)
        throw std::runtime_error("cannot inspect " + display_path(path) +
                                 " while securing it: " +
                                 status_ec.message());
    access[0].grfInheritance =
        directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    auto* user = reinterpret_cast<TOKEN_USER*>(token_info.data());
    BuildTrusteeWithSidW(&access[0].Trustee, user->User.Sid);

    std::vector<unsigned char> system_sid(SECURITY_MAX_SID_SIZE);
    DWORD system_sid_size = static_cast<DWORD>(system_sid.size());
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid.data(),
                            &system_sid_size))
        throw std::runtime_error(
            "cannot identify the Windows SYSTEM account while securing " +
            display_path(path));
    access[1].grfAccessPermissions = GENERIC_ALL;
    access[1].grfAccessMode = SET_ACCESS;
    access[1].grfInheritance = access[0].grfInheritance;
    BuildTrusteeWithSidW(&access[1].Trustee, system_sid.data());

    PACL acl = nullptr;
    DWORD result = SetEntriesInAclW(2, access, nullptr, &acl);
    if (result != ERROR_SUCCESS)
        throw std::runtime_error(
            "cannot create a private Windows ACL for " + display_path(path) +
            ": " +
            std::error_code(static_cast<int>(result), std::system_category())
                .message());

    std::wstring native = path.wstring();
    result = SetNamedSecurityInfoW(
        native.data(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    if (result != ERROR_SUCCESS)
        throw std::runtime_error(
            "cannot apply a private Windows ACL to " + display_path(path) + ": " +
            std::error_code(static_cast<int>(result), std::system_category())
                .message());
#endif
}

void ensure_private_directory(const fs::path& path) {
    std::error_code ec;
    fs::file_status status = fs::symlink_status(path, ec);
    if (!ec && fs::exists(status)) {
        if (fs::is_symlink(status) || !fs::is_directory(status))
            throw std::runtime_error("expected a real directory: " +
                                     display_path(path));
    } else {
        ec.clear();
    }
    fs::create_directories(path);
    set_private_permissions(path, fs::perms::owner_all);
}

void secure_repo_layout(const Repo& repo) {
    ensure_private_directory(repo.dir());
    ensure_private_directory(repo.objects());
    ensure_private_directory(repo.snapshots());
}

std::optional<Repo> find_repo(fs::path start) {
    start = fs::absolute(start);
    for (fs::path p = start;; p = p.parent_path()) {
        if (fs::is_directory(p / kRepoDir)) return Repo{p};
        if (p == p.parent_path()) return std::nullopt;
    }
}

Repo init_repo(const fs::path& where) {
    Repo r{fs::absolute(where)};
    if (!fs::exists(r.root)) fs::create_directories(r.root);
    if (!fs::is_directory(r.root))
        throw std::runtime_error("not a directory: " + display_path(r.root));
    secure_repo_layout(r);
    return r;
}

// ---------------------------------------------------------------------------
// Ignore rules (.lapseignore: one glob per line, '#' comments)
// ---------------------------------------------------------------------------

struct IgnoreRules {
    std::vector<std::string> patterns;

    void load(const fs::path& root) {
        patterns.clear();
        fs::path file = root / kIgnoreFile;
        std::ifstream in(file);
        if (!in) {
            std::error_code ec;
            if (fs::exists(file, ec))
                die("cannot read ignore file: " + display_path(file));
            return;
        }
        std::string line;
        while (std::getline(in, line)) {
            // trim
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            std::size_t i = line.find_first_not_of(' ');
            if (i == std::string::npos) continue;
            line = line.substr(i);
            if (line.empty() || line[0] == '#') continue;
            while (!line.empty() && line.back() == '/') line.pop_back();
            if (!line.empty()) patterns.push_back(line);
        }
        if (in.bad()) die("error while reading ignore file: " +
                          display_path(file));
    }

    // rel: forward-slash relative path, e.g. "src/main.cpp"
    bool ignored(const std::string& rel) const {
        // The repo's own data is always invisible to itself.
        if (rel == kRepoDir || starts_with(rel, std::string(kRepoDir) + "/"))
            return true;

        for (const auto& pat : patterns) {
            if (pat.find('/') != std::string::npos) {
                if (glob_match(pat, rel)) return true;
                if (rel == pat || starts_with(rel, pat + "/")) return true;
            } else {
                // Slashless pattern: match any single path component.
                std::size_t b = 0;
                while (b <= rel.size()) {
                    std::size_t e = rel.find('/', b);
                    std::string comp = rel.substr(b, e == std::string::npos
                                                          ? std::string::npos
                                                          : e - b);
                    if (glob_match(pat, comp)) return true;
                    if (e == std::string::npos) break;
                    b = e + 1;
                }
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Manifest entries & snapshots
// ---------------------------------------------------------------------------

struct Entry {
    std::string path;     // relative, forward slashes
    std::string hash;     // sha256 hex of contents
    std::uint32_t mode;   // fs::perms bits (octal on disk)
    std::int64_t mtime;   // fs::file_time_type ticks (platform-consistent)
    std::uint64_t size;
};

struct Snapshot {
    int format_version = 2;
    std::string id;         // 12 hex chars
    std::uint64_t seq = 0;  // monotonic order, embedded in the filename
    std::int64_t time = 0;  // unix seconds
    std::string message;
    std::vector<Entry> entries;
    fs::path file;        // where it lives on disk

    std::uint64_t total_size() const {
        std::uint64_t s = 0;
        for (const auto& e : entries) s += e.size;
        return s;
    }
};

fs::path unique_temporary_path(const fs::path& destination);

std::string entries_text(const std::vector<Entry>& es) {
    std::ostringstream os;
    for (const auto& e : es) {
        os << e.hash << '\t' << std::oct << e.mode << std::dec << '\t'
           << e.mtime << '\t' << e.size << '\t' << e.path << '\n';
    }
    return os.str();
}

std::string snapshot_identity_input(
    int format_version, std::uint64_t sequence, std::int64_t time,
    const std::string& entries) {
    std::string identity = std::to_string(sequence) + "\n" +
                           std::to_string(time) + "\n" + entries;
    if (format_version >= 2)
        identity = "lapse " + std::to_string(format_version) + "\n" +
                   identity;
    return identity;
}

void write_snapshot(const Repo& repo, Snapshot& snap) {
    snap.format_version = 2;
    std::string body = entries_text(snap.entries);
    snap.id = Sha256::hex_digest(snapshot_identity_input(
                                     snap.format_version, snap.seq,
                                     snap.time, body))
                  .substr(0, 12);

    std::string msg = snap.message;
    std::replace(msg.begin(), msg.end(), '\n', ' ');
    std::replace(msg.begin(), msg.end(), '\r', ' ');
    snap.message = msg;

    char seqbuf[24];
    std::snprintf(seqbuf, sizeof seqbuf, "%010llu",
                  static_cast<unsigned long long>(snap.seq));
    fs::path file = repo.snapshots() /
                    (std::string(seqbuf) + "-" + std::to_string(snap.time) +
                     "-" + snap.id + ".snap");
    fs::path tmp = unique_temporary_path(file);
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) die("cannot write snapshot file: " + display_path(tmp));
        out << "lapse 2\n"
            << "id " << snap.id << "\n"
            << "time " << snap.time << "\n"
            << "message " << msg << "\n"
            << "files " << snap.entries.size() << "\n"
            << body;
        out.flush();
        if (!out) {
            std::error_code ec;
            fs::remove(tmp, ec);
            die("cannot finish snapshot file: " + display_path(tmp));
        }
    }
    set_private_permissions(
        tmp, fs::perms::owner_read | fs::perms::owner_write);
    fs::rename(tmp, file);
    snap.file = file;
}

Snapshot read_snapshot(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) invalid_snapshot(file, "cannot open file");

    Snapshot s;
    s.file = file;
    std::string line;
    std::size_t nfiles = 0;

    auto value_of = [&](const std::string& l, const char* key) -> std::string {
        std::string k = std::string(key) + " ";
        if (!starts_with(l, k))
            invalid_snapshot(file, "expected '" + std::string(key) + "' header");
        return l.substr(k.size());
    };

    if (!std::getline(in, line))
        invalid_snapshot(file, "unsupported or missing format header");
    if (line == "lapse 1")
        s.format_version = 1;
    else if (line == "lapse 2")
        s.format_version = 2;
    else
        invalid_snapshot(file, "unsupported or missing format header");
    if (!std::getline(in, line)) invalid_snapshot(file, "missing id header");
    s.id = value_of(line, "id");
    if (!is_lower_hex(s.id, 12)) invalid_snapshot(file, "invalid id");

    if (!std::getline(in, line)) invalid_snapshot(file, "missing time header");
    s.time = parse_i64(value_of(line, "time"), file, "time");

    if (!std::getline(in, line))
        invalid_snapshot(file, "missing message header");
    s.message = value_of(line, "message");
    if (s.message.find('\0') != std::string::npos)
        invalid_snapshot(file, "invalid message");
    std::replace(s.message.begin(), s.message.end(), '\r', ' ');

    if (!std::getline(in, line)) invalid_snapshot(file, "missing files header");
    std::uint64_t declared =
        parse_u64(value_of(line, "files"), 10, file, "file count");
    if (declared > std::numeric_limits<std::size_t>::max())
        invalid_snapshot(file, "file count is too large");
    nfiles = static_cast<std::size_t>(declared);

    std::set<std::string> paths;
    while (std::getline(in, line)) {
        if (line.empty()) invalid_snapshot(file, "empty manifest entry");
        // hash \t mode(octal) \t mtime \t size \t path (path may contain tabs)
        std::size_t t1 = line.find('\t');
        if (t1 == std::string::npos)
            invalid_snapshot(file, "manifest entry has too few fields");
        std::size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos)
            invalid_snapshot(file, "manifest entry has too few fields");
        std::size_t t3 = line.find('\t', t2 + 1);
        if (t3 == std::string::npos)
            invalid_snapshot(file, "manifest entry has too few fields");
        std::size_t t4 = line.find('\t', t3 + 1);
        if (t4 == std::string::npos)
            invalid_snapshot(file, "manifest entry has too few fields");
        Entry e;
        e.hash  = line.substr(0, t1);
        if (!is_lower_hex(e.hash, 64))
            invalid_snapshot(file, "invalid object hash");
        std::uint64_t mode =
            parse_u64(line.substr(t1 + 1, t2 - t1 - 1), 8, file, "mode");
        auto permission_mask =
            static_cast<std::uint32_t>(fs::perms::mask);
        if (mode > permission_mask)
            invalid_snapshot(file, "mode is out of range");
        e.mode = static_cast<std::uint32_t>(mode);
        e.mtime =
            parse_i64(line.substr(t2 + 1, t3 - t2 - 1), file, "mtime");
        e.size =
            parse_u64(line.substr(t3 + 1, t4 - t3 - 1), 10, file, "size");
        e.path  = line.substr(t4 + 1);
        PathPolicy policy = s.format_version == 1
                                ? PathPolicy::NativeV1
                                : PathPolicy::PortableV2;
        if (!is_safe_manifest_path(e.path, policy))
            invalid_snapshot(file, "unsafe path in manifest entry");
        if (!paths.insert(e.path).second)
            invalid_snapshot(file, "duplicate path: " +
                                      escape_terminal_controls(e.path));
        s.entries.push_back(std::move(e));
    }
    if (in.bad()) invalid_snapshot(file, "read failed");
    if (s.entries.size() != nfiles)
        invalid_snapshot(file, "declared file count does not match entries");
    return s;
}

// All snapshots, oldest first (ordered by their monotonic sequence number).
std::vector<Snapshot> load_snapshots(const Repo& repo) {
    std::vector<Snapshot> out;
    if (!fs::is_directory(repo.snapshots())) return out;
    for (const auto& de : fs::directory_iterator(repo.snapshots())) {
        if (de.path().extension() != ".snap") continue;
        std::error_code ec;
        fs::file_status status = de.symlink_status(ec);
        if (ec || fs::is_symlink(status) || !fs::is_regular_file(status))
            invalid_snapshot(de.path(), "snapshot is not a regular file");

        Snapshot s = read_snapshot(de.path());
        std::string stem = de.path().stem().string();
        std::size_t first_dash = stem.find('-');
        std::size_t last_dash = stem.rfind('-');
        if (first_dash == std::string::npos || last_dash == first_dash)
            invalid_snapshot(de.path(), "invalid filename");
        s.seq = parse_u64(stem.substr(0, first_dash), 10, de.path(),
                          "filename sequence");
        if (s.seq == 0) invalid_snapshot(de.path(), "sequence must be positive");
        if (stem.substr(last_dash + 1) != s.id)
            invalid_snapshot(de.path(), "filename id does not match manifest");
        if (stem.substr(first_dash + 1, last_dash - first_dash - 1) !=
            std::to_string(s.time))
            invalid_snapshot(de.path(), "filename time does not match manifest");

        std::string expected =
            Sha256::hex_digest(snapshot_identity_input(
                                   s.format_version, s.seq, s.time,
                                   entries_text(s.entries)))
                .substr(0, 12);
        if (s.id != expected)
            invalid_snapshot(de.path(), "manifest id check failed");
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(),
              [](const Snapshot& a, const Snapshot& b) {
                  if (a.seq != b.seq) return a.seq < b.seq;
                  return a.time != b.time ? a.time < b.time : a.id < b.id;
              });
    std::set<std::string> ids;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (!ids.insert(out[i].id).second)
            invalid_snapshot(out[i].file, "duplicate snapshot id");
        if (i == 0) continue;
        if (out[i - 1].seq == out[i].seq)
            invalid_snapshot(out[i].file, "duplicate snapshot sequence");
    }
    return out;
}

// Resolve "last", a full id, or a unique id prefix.
const Snapshot* resolve(const std::vector<Snapshot>& snaps, const std::string& ref) {
    if (snaps.empty()) return nullptr;
    if (ref.empty()) return nullptr;
    if (ref == "last" || ref == "latest" || ref == "@") return &snaps.back();

    const Snapshot* found = nullptr;
    for (const auto& s : snaps) {
        if (s.id == ref) return &s;
        if (starts_with(s.id, ref)) {
            if (found) die("snapshot reference '" + ref + "' is ambiguous");
            found = &s;
        }
    }
    return found;
}

// ---------------------------------------------------------------------------
// Object store
// ---------------------------------------------------------------------------

fs::path object_path(const Repo& repo, const std::string& hash) {
    return repo.objects() / hash.substr(0, 2) / hash.substr(2);
}

std::string hash_stream(std::istream& in, const std::string& description,
                        std::uint64_t* byte_count = nullptr) {
    Sha256 h;
    std::uint64_t total = 0;
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), std::streamsize(buf.size()));
        std::streamsize n = in.gcount();
        if (n > 0) {
            h.update(buf.data(), std::size_t(n));
            total += static_cast<std::uint64_t>(n);
        }
    }
    if (in.bad()) die("error while reading " + description);
    if (byte_count) *byte_count = total;
    return Sha256::to_hex(h.finish());
}

std::string hash_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) die("cannot read file: " + display_path(p));
    return hash_stream(in, "file: " + display_path(p));
}

fs::path verified_object_path(const Repo& repo, const Entry& e) {
    fs::path obj = object_path(repo, e.hash);
    std::error_code ec;
    fs::file_status status = fs::symlink_status(obj, ec);
    if (ec || fs::is_symlink(status) || !fs::is_regular_file(status))
        die("missing or invalid object for " +
            escape_terminal_controls(e.path) + " (store corrupted?)");
    std::uint64_t actual_size = fs::file_size(obj, ec);
    if (ec || actual_size != e.size || hash_file(obj) != e.hash)
        die("object checksum mismatch for " +
            escape_terminal_controls(e.path) + " (store corrupted?)");
    return obj;
}

fs::path unique_temporary_path(const fs::path& destination) {
    static std::atomic<std::uint64_t> counter{0};
    for (int attempt = 0; attempt < 1000; ++attempt) {
        fs::path tmp = destination;
        tmp += ".tmp-" + std::to_string(static_cast<long long>(LAPSE_GETPID())) +
               "-" + std::to_string(counter.fetch_add(1));
        std::error_code ec;
        if (!fs::exists(fs::symlink_status(tmp, ec))) return tmp;
    }
    die("cannot allocate temporary path next to " +
        display_path(destination));
}

// Returns bytes added to the store (0 if the object already existed).
std::uint64_t store_object(const Repo& repo, const fs::path& src,
                           const std::string& hash, std::uint64_t size) {
    fs::path obj = object_path(repo, hash);
    std::error_code ec;
    fs::file_status existing = fs::symlink_status(obj, ec);
    if (!ec && fs::exists(existing)) {
        if (fs::is_symlink(existing) || !fs::is_regular_file(existing))
            die("invalid object-store entry: " + display_path(obj));
        std::uint64_t existing_size = fs::file_size(obj, ec);
        if (ec || existing_size != size || hash_file(obj) != hash)
            die("object checksum mismatch for " + display_path(src) +
                " (store corrupted?)");
        return 0;
    }
    ec.clear();
    ensure_private_directory(obj.parent_path());

    fs::path tmp = unique_temporary_path(obj);
    fs::copy_file(src, tmp, fs::copy_options::none, ec);
    if (ec) {
        std::string message = ec.message();
        std::error_code remove_ec;
        fs::remove(tmp, remove_ec);
        die("cannot store " + display_path(src) + ": " + message);
    }
    std::uint64_t copied_size = fs::file_size(tmp, ec);
    if (ec || copied_size != size || hash_file(tmp) != hash) {
        std::error_code remove_ec;
        fs::remove(tmp, remove_ec);
        die("file changed while snapshotting: " + display_path(src) +
            " (retry the snapshot)");
    }
    set_private_permissions(
        tmp, fs::perms::owner_read | fs::perms::owner_write);
    fs::rename(tmp, obj, ec);
    if (ec) {
        // Another snapshot process may have installed the same object.
        std::error_code status_ec;
        fs::file_status raced = fs::symlink_status(obj, status_ec);
        std::error_code remove_ec;
        fs::remove(tmp, remove_ec);
        if (status_ec || fs::is_symlink(raced) ||
            !fs::is_regular_file(raced))
            die("cannot install object " + display_path(obj) + ": " +
                ec.message());
        std::uint64_t raced_size = fs::file_size(obj, status_ec);
        if (status_ec || raced_size != size || hash_file(obj) != hash)
            die("concurrently installed object failed verification: " +
                display_path(obj));
        return 0;
    }
    set_private_permissions(obj, fs::perms::owner_read);
    return size;
}

bool extract_object(const Repo& repo, const Entry& e, const fs::path& target,
                    bool overwrite) {
    fs::path obj = verified_object_path(repo, e);

    fs::path tmp = unique_temporary_path(target);
    std::error_code ec;
    fs::copy_file(obj, tmp, fs::copy_options::none, ec);
    if (ec) die("cannot restore " + display_path(target) + ": " +
                ec.message());
#if defined(_WIN32)
    // CopyFile carries FILE_ATTRIBUTE_READONLY from a stored object. Clear it
    // while staging so checksums and timestamps can be applied; the snapshot
    // mode is restored immediately before installation.
    fs::permissions(tmp, fs::perms::owner_write,
                    fs::perm_options::add, ec);
    if (ec) {
        std::error_code remove_ec;
        fs::remove(tmp, remove_ec);
        die("cannot prepare staged restore " + display_path(tmp) + ": " +
            ec.message());
    }
#endif
    std::uint64_t staged_size = fs::file_size(tmp, ec);
    if (ec || staged_size != e.size || hash_file(tmp) != e.hash) {
        std::error_code remove_ec;
        fs::remove(tmp, remove_ec);
        die("staged object checksum mismatch for " +
            escape_terminal_controls(e.path) +
            " (store changed during restore?)");
    }
    fs::last_write_time(
        tmp, fs::file_time_type(fs::file_time_type::duration(e.mtime)), ec);
    if (ec) {
        std::error_code remove_ec;
        fs::remove(tmp, remove_ec);
        die("cannot restore timestamp for " + display_path(target) + ": " +
            ec.message());
    }
    fs::permissions(tmp, static_cast<fs::perms>(e.mode),
                    fs::perm_options::replace, ec);
    if (ec) {
        std::error_code remove_ec;
        fs::remove(tmp, remove_ec);
        die("cannot restore permissions for " + display_path(target) + ": " +
            ec.message());
    }

    if (!overwrite) {
        // Installing a hard link is an atomic no-clobber operation on common
        // filesystems. Fall back to copy_file(none), which also refuses an
        // existing destination, when hard links are unavailable.
#if defined(_WIN32)
        // Deleting a read-only staging hardlink is not permitted on Windows.
        // CopyFile with fail-if-exists provides the same no-clobber property.
        ec = std::make_error_code(std::errc::operation_not_supported);
#else
        fs::create_hard_link(tmp, target, ec);
        if (!ec) {
            if (!fs::remove(tmp, ec) || ec)
                die("restored " + display_path(target) +
                    " but could not remove its temporary name: " +
                    ec.message());
            return true;
        }
#endif

        std::error_code status_ec;
        fs::file_status status = fs::symlink_status(target, status_ec);
        if (!status_ec && fs::exists(status)) {
            std::error_code remove_ec;
            fs::remove(tmp, remove_ec);
            return false;
        }

        ec.clear();
#if defined(_WIN32)
        fs::permissions(tmp, fs::perms::owner_write,
                        fs::perm_options::add, ec);
        if (ec) {
            std::error_code remove_ec;
            fs::remove(tmp, remove_ec);
            die("cannot prepare staged file for restore " +
                display_path(target) + ": " + ec.message());
        }
        ec.clear();
#endif
        fs::copy_file(tmp, target, fs::copy_options::none, ec);
        if (ec) {
            status_ec.clear();
            status = fs::symlink_status(target, status_ec);
            std::error_code remove_ec;
            fs::remove(tmp, remove_ec);
            if (!status_ec && fs::exists(status)) return false;
            die("cannot install restored file " + display_path(target) + ": " +
                ec.message());
        }
        std::uint64_t installed_size = fs::file_size(target, ec);
        if (ec || installed_size != e.size || hash_file(target) != e.hash) {
            std::error_code remove_ec;
            fs::remove(target, remove_ec);
            fs::remove(tmp, remove_ec);
            die("restored file failed verification: " +
                display_path(target));
        }
        fs::last_write_time(
            target, fs::file_time_type(fs::file_time_type::duration(e.mtime)),
            ec);
        if (!ec)
            fs::permissions(target, static_cast<fs::perms>(e.mode),
                            fs::perm_options::replace, ec);
        std::error_code remove_ec;
        fs::remove(tmp, remove_ec);
        if (ec)
            die("cannot apply metadata to restored file " +
                display_path(target) +
                ": " + ec.message());
        return true;
    }

#if defined(_WIN32)
    // Move a new file without replace semantics. When the destination already
    // exists, deliberately enter the ReplaceFile path below so its ACL is
    // preserved consistently across standard-library implementations.
    std::error_code replace_status_ec;
    fs::file_status replace_status =
        fs::symlink_status(target, replace_status_ec);
    if (replace_status_ec == std::errc::no_such_file_or_directory) {
        replace_status_ec.clear();
        replace_status = fs::file_status(fs::file_type::not_found);
    }
    if (replace_status_ec) {
        ec = replace_status_ec;
    } else if (fs::exists(replace_status)) {
        ec = std::make_error_code(std::errc::file_exists);
    } else if (MoveFileExW(tmp.c_str(), target.c_str(),
                           MOVEFILE_WRITE_THROUGH)) {
        ec.clear();
    } else {
        ec = win32_error(GetLastError());
    }
#else
    fs::rename(tmp, target, ec);
#endif
    if (ec) {
#if defined(_WIN32)
        // std::filesystem::rename does not replace an existing file on
        // Windows. ReplaceFile preserves the original ACL and keeps a rollback
        // name until the replacement is known to have succeeded.
        std::error_code status_ec;
        fs::file_status status = fs::symlink_status(target, status_ec);
        if (status_ec || fs::is_symlink(status) ||
            !fs::is_regular_file(status)) {
            std::error_code remove_ec;
            fs::remove(tmp, remove_ec);
            die("cannot replace restore target " + display_path(target) + ": " +
                ec.message());
        }

        // ReplaceFile rejects a read-only replacement. Snapshot permissions
        // are re-applied to the installed target after a successful swap.
        fs::permissions(tmp, fs::perms::owner_write,
                        fs::perm_options::add, status_ec);
        if (status_ec) {
            std::error_code remove_ec;
            fs::remove(tmp, remove_ec);
            die("cannot make staged restore replaceable " +
                display_path(tmp) + ": " + status_ec.message());
        }

        DWORD original_attributes = GetFileAttributesW(target.c_str());
        if (original_attributes == INVALID_FILE_ATTRIBUTES) {
            DWORD code = GetLastError();
            std::error_code remove_ec;
            fs::remove(tmp, remove_ec);
            die("cannot inspect Windows attributes for " +
                display_path(target) + ": " + win32_error(code).message());
        }
        bool original_read_only =
            (original_attributes & FILE_ATTRIBUTE_READONLY) != 0;
        if (original_read_only) {
            fs::permissions(target, fs::perms::owner_write,
                            fs::perm_options::add, status_ec);
            if (status_ec) {
                std::error_code remove_ec;
                fs::remove(tmp, remove_ec);
                die("cannot make restore target replaceable " +
                    display_path(target) + ": " + status_ec.message());
            }
        }

        auto restore_original_read_only =
            [&](const fs::path& original) -> std::error_code {
            std::error_code attribute_ec;
            if (original_read_only)
                fs::permissions(original, fs::perms::owner_write,
                                fs::perm_options::remove, attribute_ec);
            return attribute_ec;
        };

        fs::path backup = unique_temporary_path(target);
        if (ReplaceFileW(target.c_str(), tmp.c_str(), backup.c_str(), 0,
                         nullptr, nullptr)) {
            fs::last_write_time(
                target,
                fs::file_time_type(fs::file_time_type::duration(e.mtime)),
                status_ec);
            if (!status_ec)
                fs::permissions(target, static_cast<fs::perms>(e.mode),
                                fs::perm_options::replace, status_ec);
            std::error_code cleanup_ec;
            fs::permissions(backup, fs::perms::owner_write,
                            fs::perm_options::add, cleanup_ec);
            cleanup_ec.clear();
            fs::remove(backup, cleanup_ec);
            if (cleanup_ec)
                die("restored " + display_path(target) +
                    " but could not remove rollback backup " +
                    display_path(backup) + ": " + cleanup_ec.message());
            if (status_ec)
                die("restored " + display_path(target) +
                    " but could not apply snapshot metadata: " +
                    status_ec.message());
        } else {
            DWORD replace_code = GetLastError();
            if (replace_code == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
                if (!MoveFileExW(backup.c_str(), target.c_str(),
                                 MOVEFILE_WRITE_THROUGH)) {
                    DWORD rollback_code = GetLastError();
                    std::error_code attribute_ec =
                        restore_original_read_only(backup);
                    std::string attribute_suffix;
                    if (attribute_ec)
                        attribute_suffix =
                            "; original attributes could not be restored: " +
                            attribute_ec.message();
                    std::error_code staged_attribute_ec;
                    fs::permissions(
                        tmp, static_cast<fs::perms>(e.mode),
                        fs::perm_options::replace, staged_attribute_ec);
                    if (staged_attribute_ec)
                        attribute_suffix +=
                            "; staged attributes could not be restored: " +
                            staged_attribute_ec.message();
                    die("cannot replace " + display_path(target) +
                        "; rollback also failed: " +
                        win32_error(rollback_code).message() +
                        " (original remains at " + display_path(backup) +
                        ", restored data remains at " + display_path(tmp) +
                        attribute_suffix + ")");
                }
            }

            std::error_code attribute_ec =
                restore_original_read_only(target);
            std::error_code cleanup_ec;
            fs::permissions(tmp, fs::perms::owner_write,
                            fs::perm_options::add, cleanup_ec);
            cleanup_ec.clear();
            fs::remove(tmp, cleanup_ec);
            std::string suffix;
            if (cleanup_ec) {
                std::error_code staged_attribute_ec;
                fs::permissions(
                    tmp, static_cast<fs::perms>(e.mode),
                    fs::perm_options::replace, staged_attribute_ec);
                suffix = " (staged data remains at " + display_path(tmp) +
                         ")";
                if (staged_attribute_ec)
                    suffix +=
                        " (staged attributes could not be restored: " +
                        staged_attribute_ec.message() + ")";
            }
            if (attribute_ec)
                suffix += " (original attributes could not be restored: " +
                          attribute_ec.message() + ")";
            die("cannot replace restore target " + display_path(target) +
                ": " + win32_error(replace_code).message() + suffix);
        }
#else
        std::string message = ec.message();
        std::error_code remove_ec;
        fs::remove(tmp, remove_ec);
        die("cannot replace restore target " + display_path(target) + ": " +
            message);
#endif
    }
    return true;
}

// ---------------------------------------------------------------------------
// Working-tree scan
// ---------------------------------------------------------------------------

struct ScanItem {
    std::string rel;
    fs::path abs;
    std::uint64_t size;
    std::int64_t mtime;
    std::uint32_t mode;
};

std::vector<ScanItem> scan_tree(const Repo& repo, const IgnoreRules& ig) {
    std::vector<ScanItem> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(repo.root, fs::directory_options::none,
                                        ec);
    if (ec) die("cannot scan " + display_path(repo.root) + ": " +
                ec.message());

    auto end = fs::recursive_directory_iterator();
    auto advance = [&]() {
        it.increment(ec);
        if (ec)
            die("cannot continue scanning " + display_path(repo.root) + ": " +
                ec.message());
    };
    while (it != end) {
        const auto& de = *it;
        std::string rel =
            fs::relative(de.path(), repo.root, ec).generic_u8string();
        if (ec) die("cannot resolve path while scanning: " + ec.message());
        if (rel.empty() || rel == ".") {
            advance();
            continue;
        }

        fs::file_status status = de.symlink_status(ec);
        if (ec) die("cannot inspect " + display_path(de.path()) + ": " +
                    ec.message());
        if (fs::is_symlink(status)) {
            advance();
            continue;
        }
        if (fs::is_directory(status)) {
            if (ig.ignored(rel)) it.disable_recursion_pending();
            else if (!is_safe_manifest_path(rel))
                die("cannot snapshot path that is unsafe in a manifest: " +
                    escape_terminal_controls(rel));
            advance();
            continue;
        }
        if (ig.ignored(rel)) {
            advance();
            continue;
        }
        if (!is_safe_manifest_path(rel))
            die("cannot snapshot path that is unsafe in a manifest: " +
                escape_terminal_controls(rel));
        if (!fs::is_regular_file(status)) {
            advance();
            continue;
        }

        ScanItem item;
        item.rel = rel;
        item.abs = de.path();
        item.size = de.file_size(ec);
        if (ec) die("cannot read size of " + display_path(de.path()) + ": " +
                    ec.message());
        item.mtime = de.last_write_time(ec).time_since_epoch().count();
        if (ec) die("cannot read timestamp of " + display_path(de.path()) + ": " +
                    ec.message());
        item.mode = static_cast<std::uint32_t>(de.status(ec).permissions());
        if (ec) die("cannot read permissions of " + display_path(de.path()) + ": " +
                    ec.message());
        out.push_back(std::move(item));
        advance();
    }
    std::sort(out.begin(), out.end(),
              [](const ScanItem& a, const ScanItem& b) { return a.rel < b.rel; });
    return out;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

struct SnapResult {
    bool created = false;
    Snapshot snap;
    std::uint64_t new_objects = 0;
    std::uint64_t new_bytes = 0;
};

SnapResult do_snap(const Repo& repo, const std::string& message) {
    IgnoreRules ig;
    ig.load(repo.root);

    auto snaps = load_snapshots(repo);

    auto items = scan_tree(repo, ig);

    SnapResult r;
    if (!snaps.empty() &&
        snaps.back().seq == std::numeric_limits<std::uint64_t>::max())
        die("snapshot sequence is exhausted");
    r.snap.seq = snaps.empty() ? 1 : snaps.back().seq + 1;
    r.snap.time = now_unix();
    r.snap.message = message;
    r.snap.entries.reserve(items.size());

    for (const auto& i : items) {
        Entry e;
        e.path = i.rel;
        e.size = i.size;
        e.mtime = i.mtime;
        e.mode = i.mode;
        // A timestamp and size are only hints, not proof of identity. Snapshot
        // tools must not miss content changed while preserving both.
        e.hash = hash_file(i.abs);
        std::error_code stable_ec;
        std::uint64_t current_size = fs::file_size(i.abs, stable_ec);
        if (stable_ec)
            die("file changed while snapshotting: " + display_path(i.abs) +
                " (retry the snapshot)");
        auto current_mtime = fs::last_write_time(i.abs, stable_ec);
        if (stable_ec)
            die("file changed while snapshotting: " + display_path(i.abs) +
                " (retry the snapshot)");
        fs::perms current_mode = fs::status(i.abs, stable_ec).permissions();
        if (stable_ec || current_size != i.size ||
            current_mtime.time_since_epoch().count() != i.mtime ||
            static_cast<std::uint32_t>(current_mode) != i.mode)
            die("file changed while snapshotting: " + display_path(i.abs) +
                " (retry the snapshot)");

        std::uint64_t added = store_object(repo, i.abs, e.hash, e.size);
        if (added > 0) {
            ++r.new_objects;
            r.new_bytes += added;
        }
        r.snap.entries.push_back(std::move(e));
    }

    // Identical to the previous snapshot? Don't clutter the timeline.
    if (!snaps.empty() &&
        entries_text(snaps.back().entries) == entries_text(r.snap.entries)) {
        r.created = false;
        r.snap = snaps.back();
        return r;
    }

    write_snapshot(repo, r.snap);
    r.created = true;
    return r;
}

int cmd_snap(const Repo& repo, const std::string& message) {
    auto t0 = std::chrono::steady_clock::now();
    SnapResult r = do_snap(repo, message);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();

    if (!r.created) {
        std::cout << dim("nothing changed since ") << cyan(r.snap.id)
                  << dim(" — no snapshot created") << "\n";
        return 0;
    }
    std::cout << green("✓ ") << bold(r.snap.id) << "  "
              << r.snap.entries.size() << " files ("
              << human_size(r.snap.total_size()) << "), " << r.new_objects
              << " new objects (" << human_size(r.new_bytes) << " stored), "
              << ms << " ms\n";
    if (!r.snap.message.empty())
        std::cout << "  "
                  << dim("\"" + escape_terminal_controls(r.snap.message) +
                         "\"")
                  << "\n";
    return 0;
}

int cmd_log(const Repo& repo) {
    auto snaps = load_snapshots(repo);
    if (snaps.empty()) {
        std::cout << dim("no snapshots yet — run `lapse snap` to create one") << "\n";
        return 0;
    }
    for (auto it = snaps.rbegin(); it != snaps.rend(); ++it) {
        std::cout << yellow("*") << " " << bold(it->id) << "  "
                  << format_time(it->time) << "  " << dim(
                         std::to_string(it->entries.size()) + " files, " +
                         human_size(it->total_size()))
                  << "\n";
        std::cout << "    "
                  << (it->message.empty()
                          ? dim("(no message)")
                          : escape_terminal_controls(it->message))
                  << "\n";
    }
    return 0;
}

// Map path -> entry for diffing.
std::map<std::string, Entry> entry_map(const std::vector<Entry>& es) {
    std::map<std::string, Entry> m;
    for (const auto& e : es) m[e.path] = e;
    return m;
}

struct DiffLine {
    char kind;          // 'A' added, 'M' modified, 'D' deleted
    std::string path;
};

void print_diff(const std::vector<DiffLine>& lines) {
    std::size_t a = 0, m = 0, d = 0;
    for (const auto& l : lines) {
        std::string shown = escape_terminal_controls(l.path);
        if (l.kind == 'A') { std::cout << green("A  ") << shown << "\n"; ++a; }
        if (l.kind == 'M') { std::cout << yellow("M  ") << shown << "\n"; ++m; }
        if (l.kind == 'D') { std::cout << red("D  ") << shown << "\n"; ++d; }
    }
    if (lines.empty())
        std::cout << dim("no differences") << "\n";
    else
        std::cout << dim(std::to_string(a) + " added, " + std::to_string(m) +
                         " modified, " + std::to_string(d) + " deleted")
                  << "\n";
}

// Diff two entry sets (old -> new).
std::vector<DiffLine> diff_entries(const std::map<std::string, Entry>& oldm,
                                   const std::map<std::string, Entry>& newm) {
    std::vector<DiffLine> out;
    for (const auto& [path, e] : newm) {
        auto it = oldm.find(path);
        if (it == oldm.end()) out.push_back({'A', path});
        else if (it->second.hash != e.hash || it->second.size != e.size ||
                 it->second.mode != e.mode || it->second.mtime != e.mtime)
            out.push_back({'M', path});
    }
    for (const auto& [path, e] : oldm) {
        (void)e;
        if (!newm.count(path)) out.push_back({'D', path});
    }
    return out;
}

// Scan and hash the working tree for status and diff-against-worktree.
std::map<std::string, Entry> worktree_entries(const Repo& repo) {
    IgnoreRules ig;
    ig.load(repo.root);

    std::map<std::string, Entry> out;
    for (const auto& i : scan_tree(repo, ig)) {
        Entry e;
        e.path = i.rel;
        e.size = i.size;
        e.mtime = i.mtime;
        e.mode = i.mode;
        e.hash = hash_file(i.abs);
        out[e.path] = std::move(e);
    }
    return out;
}

int cmd_status(const Repo& repo) {
    auto snaps = load_snapshots(repo);
    if (snaps.empty()) {
        std::cout << dim("no snapshots yet — everything is new") << "\n";
        return 0;
    }
    const Snapshot& last = snaps.back();
    auto diff = diff_entries(entry_map(last.entries),
                             worktree_entries(repo));
    std::cout << dim("changes since ") << cyan(last.id) << dim(" (") 
              << dim(format_time(last.time)) << dim(")") << "\n";
    print_diff(diff);
    return 0;
}

int cmd_diff(const Repo& repo, const std::string& a, const std::string& b) {
    auto snaps = load_snapshots(repo);
    const Snapshot* sa = resolve(snaps, a);
    if (!sa) die("unknown snapshot: " + a);

    if (b.empty()) {
        // snapshot -> working tree
        auto diff = diff_entries(entry_map(sa->entries),
                                 worktree_entries(repo));
        std::cout << dim("diff ") << cyan(sa->id) << dim(" -> working tree") << "\n";
        print_diff(diff);
    } else {
        const Snapshot* sb = resolve(snaps, b);
        if (!sb) die("unknown snapshot: " + b);
        auto diff = diff_entries(entry_map(sa->entries), entry_map(sb->entries));
        std::cout << dim("diff ") << cyan(sa->id) << dim(" -> ") << cyan(sb->id) << "\n";
        print_diff(diff);
    }
    return 0;
}

int cmd_show(const Repo& repo, const std::string& ref) {
    auto snaps = load_snapshots(repo);
    const Snapshot* s = resolve(snaps, ref);
    if (!s) die("unknown snapshot: " + ref);
    std::cout << bold(s->id) << "  " << format_time(s->time) << "  "
              << (s->message.empty()
                      ? dim("(no message)")
                      : escape_terminal_controls(s->message))
              << "\n";
    for (const auto& e : s->entries)
        std::cout << "  " << dim(e.hash.substr(0, 12)) << "  "
                  << human_size(e.size) << "\t"
                  << escape_terminal_controls(e.path) << "\n";
    std::cout << dim(std::to_string(s->entries.size()) + " files, " +
                     human_size(s->total_size()))
              << "\n";
    return 0;
}

int cmd_cat(const Repo& repo, const std::string& ref, const std::string& path) {
    auto snaps = load_snapshots(repo);
    const Snapshot* s = resolve(snaps, ref);
    if (!s) die("unknown snapshot: " + ref);
    PathPolicy policy = s->format_version == 1
                            ? PathPolicy::NativeV1
                            : PathPolicy::PortableV2;
    if (!is_safe_manifest_path(path, policy))
        die("cat path must be a safe relative path");
    for (const auto& e : s->entries) {
        if (e.path == path) {
            fs::path obj = verified_object_path(repo, e);
            std::ifstream in(obj, std::ios::binary);
            if (!in) die("missing object for " +
                         escape_terminal_controls(path));
            std::uint64_t streamed_size = 0;
            std::string streamed_hash =
                hash_stream(in, "object for " +
                                escape_terminal_controls(path),
                            &streamed_size);
            if (streamed_size != e.size || streamed_hash != e.hash)
                die("object checksum mismatch for " +
                    escape_terminal_controls(path) +
                    " (store changed while reading?)");
            in.clear();
            in.seekg(0);
            if (!in) die("cannot rewind object for " +
                         escape_terminal_controls(path));
#if defined(_WIN32)
            if (_setmode(LAPSE_FILENO(stdout), _O_BINARY) == -1)
                die("cannot set stdout to binary mode");
#endif
            std::cout << in.rdbuf();
            if (!std::cout) die("cannot write file to stdout");
            return 0;
        }
    }
    die("'" + escape_terminal_controls(path) +
        "' is not in snapshot " + s->id);
}

bool path_has_prefix(const fs::path& base, const fs::path& candidate) {
    auto b = base.begin();
    auto c = candidate.begin();
    for (; b != base.end(); ++b, ++c) {
        if (c == candidate.end() || *b != *c) return false;
    }
    return true;
}

bool path_is_within_existing_directory(const fs::path& directory,
                                       const fs::path& candidate) {
    if (path_has_prefix(directory, candidate)) return true;

    for (fs::path current = candidate;; current = current.parent_path()) {
        std::error_code ec;
        if (fs::equivalent(directory, current, ec)) return true;
        if (current == current.parent_path()) break;
    }
    return false;
}

fs::path prepare_restore_base(const fs::path& requested) {
    std::error_code ec;
    fs::path base = fs::weakly_canonical(requested, ec);
    if (ec)
        throw std::runtime_error("cannot resolve restore directory " +
                                 display_path(requested) + ": " + ec.message());
    fs::create_directories(base, ec);
    if (ec)
        throw std::runtime_error("cannot create restore directory " +
                                 display_path(base) + ": " + ec.message());
    fs::file_status status = fs::symlink_status(base, ec);
    if (ec || fs::is_symlink(status) || !fs::is_directory(status))
        throw std::runtime_error("restore destination is not a real directory: " +
                                 display_path(base));
    return base;
}

void ensure_restore_parents(const fs::path& base,
                            const std::string& relative) {
    std::vector<fs::path> parts;
    for (const auto& part : fs::u8path(relative)) parts.push_back(part);

    fs::path current = base;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        current /= parts[i];
        std::error_code ec;
        fs::file_status status = fs::symlink_status(current, ec);
        if (ec == std::errc::no_such_file_or_directory ||
            (!ec && !fs::exists(status))) {
            ec.clear();
            if (!fs::create_directory(current, ec) || ec)
                throw std::runtime_error(
                    "cannot create restore directory " + display_path(current) +
                    ": " + ec.message());
            status = fs::symlink_status(current, ec);
        }
        if (ec)
            throw std::runtime_error("cannot inspect restore directory " +
                                     display_path(current) + ": " +
                                     ec.message());
        if (fs::is_symlink(status) || !fs::is_directory(status))
            throw std::runtime_error(
                "refusing to restore through a non-directory or symlink: " +
                display_path(current));
    }
}

class TemporaryRestoreProbe {
public:
    explicit TemporaryRestoreProbe(const fs::path& base) {
        static std::atomic<std::uint64_t> counter{0};
        for (int attempt = 0; attempt < 1000; ++attempt) {
            path_ = base / (".lapse-restore-probe-" +
                            std::to_string(
                                static_cast<long long>(LAPSE_GETPID())) +
                            "-" + std::to_string(counter.fetch_add(1)));
            std::error_code ec;
            if (fs::create_directory(path_, ec)) {
                try {
                    set_private_permissions(path_, fs::perms::owner_all);
                } catch (...) {
                    std::error_code remove_ec;
                    fs::remove_all(path_, remove_ec);
                    path_.clear();
                    throw;
                }
                return;
            }
            if (!ec || ec == std::errc::file_exists) continue;
            throw std::runtime_error("cannot create restore validation directory " +
                                     display_path(path_) + ": " + ec.message());
        }
        throw std::runtime_error(
            "cannot allocate a restore validation directory in " +
            display_path(base));
    }

    TemporaryRestoreProbe(const TemporaryRestoreProbe&) = delete;
    TemporaryRestoreProbe& operator=(const TemporaryRestoreProbe&) = delete;

    ~TemporaryRestoreProbe() {
        if (path_.empty()) return;
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

    void remove() {
        std::error_code ec;
        fs::remove_all(path_, ec);
        if (ec)
            throw std::runtime_error(
                "cannot remove restore validation directory " +
                display_path(path_) + ": " + ec.message());
        path_.clear();
    }

private:
    fs::path path_;
};

void validate_destination_mapping(
    const fs::path& base,
    const std::vector<const Entry*>& entries) {
    TemporaryRestoreProbe probe(base);
    fs::path tree = probe.path() / "tree";
    std::error_code ec;
    if (!fs::create_directory(tree, ec) || ec)
        throw std::runtime_error("cannot create restore validation tree: " +
                                 ec.message());

    fs::path seed = probe.path() / "seed";
    {
        std::ofstream out(seed, std::ios::binary);
        if (!out)
            throw std::runtime_error(
                "cannot create restore validation seed in " +
                display_path(probe.path()));
    }

    std::set<std::string> logical_directories;
    for (const Entry* entry : entries) {
        std::vector<fs::path> parts;
        for (const auto& part : fs::u8path(entry->path))
            parts.push_back(part);

        fs::path actual = tree;
        std::string logical;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            std::string component = parts[i].generic_u8string();
            logical += (logical.empty() ? "" : "/") + component;
            actual /= parts[i];
            if (!logical_directories.insert(logical).second) continue;

            ec.clear();
            bool created = fs::create_directory(actual, ec);
            if (ec)
                throw std::runtime_error(
                    "destination filesystem cannot represent snapshot path '" +
                    escape_terminal_controls(entry->path) + "': " +
                    ec.message());
            if (!created)
                throw std::runtime_error(
                    "snapshot paths map to the same destination directory: " +
                    escape_terminal_controls(entry->path));
        }

        fs::path target = actual / parts.back();
        ec.clear();
        fs::create_hard_link(seed, target, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(seed, target, fs::copy_options::none, ec);
        }
        if (ec) {
            std::error_code status_ec;
            bool exists = fs::exists(fs::symlink_status(target, status_ec));
            if (!status_ec && exists)
                throw std::runtime_error(
                    "snapshot paths map to the same destination file: " +
                    escape_terminal_controls(entry->path));
            throw std::runtime_error(
                "destination filesystem cannot represent snapshot path '" +
                escape_terminal_controls(entry->path) + "': " +
                ec.message());
        }
    }
    probe.remove();
}

fs::path safe_restore_target(const fs::path& base,
                             const std::string& relative,
                             PathPolicy policy) {
    if (!is_safe_manifest_path(relative, policy))
        die("unsafe path in snapshot: " +
            escape_terminal_controls(relative));

    std::error_code ec;
    fs::path canonical_base = fs::weakly_canonical(base, ec);
    if (ec)
        die("cannot resolve restore directory " + display_path(base) + ": " +
            ec.message());
    fs::path target =
        (canonical_base / fs::u8path(relative)).lexically_normal();
    if (!path_has_prefix(canonical_base, target) || target == canonical_base)
        die("snapshot path escapes restore directory: " +
            escape_terminal_controls(relative));

    fs::path current = canonical_base;
    std::vector<fs::path> parts;
    for (const auto& part : fs::u8path(relative)) parts.push_back(part);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        current /= parts[i];
        fs::file_status status = fs::symlink_status(current, ec);
        if (ec) {
            if (ec == std::errc::no_such_file_or_directory) {
                ec.clear();
                continue;
            }
            die("cannot inspect restore path " + display_path(current) + ": " +
                ec.message());
        }
        if (fs::is_symlink(status))
            die("refusing to restore through symlink: " +
                display_path(current));
        if (i + 1 < parts.size() && fs::exists(status) &&
            !fs::is_directory(status))
            die("restore path component is not a directory: " +
                display_path(current));
    }

    fs::path resolved_target = fs::weakly_canonical(target, ec);
    if (ec)
        die("cannot resolve restore target " + display_path(target) + ": " +
            ec.message());
    if (!path_has_prefix(canonical_base, resolved_target) ||
        resolved_target == canonical_base)
        die("snapshot path resolves outside restore directory: " +
            escape_terminal_controls(relative));
    return resolved_target;
}

int cmd_restore(const Repo& repo, const std::string& ref,
                const std::vector<std::string>& paths, const fs::path& to,
                bool force) {
    auto snaps = load_snapshots(repo);
    const Snapshot* s = resolve(snaps, ref);
    if (!s) die("unknown snapshot: " + ref);
    PathPolicy policy = s->format_version == 1
                            ? PathPolicy::NativeV1
                            : PathPolicy::PortableV2;

    std::vector<std::string> normalized_paths;
    normalized_paths.reserve(paths.size());
    for (const auto& raw : paths) {
        std::string normalized = fs::path(raw).generic_string();
        while (!normalized.empty() && normalized.back() == '/')
            normalized.pop_back();
        if (!is_safe_manifest_path(normalized, policy))
            die("restore paths must be safe relative paths: " + raw);
        normalized_paths.push_back(std::move(normalized));
    }

    auto selected = [&](const std::string& p) {
        if (normalized_paths.empty()) return true;
        for (const auto& want : normalized_paths)
            if (p == want || starts_with(p, want + "/")) return true;
        return false;
    };

    fs::path base = to.empty() ? repo.root : fs::absolute(to);
    std::size_t restored = 0, skipped = 0, untouched = 0;

    // Validate every selected object and destination before modifying any
    // files, so a corrupt later entry cannot leave a partial export.
    std::vector<const Entry*> pending;
    std::error_code repo_ec;
    fs::path canonical_repo_dir = fs::weakly_canonical(repo.dir(), repo_ec);
    if (repo_ec)
        die("cannot resolve repository data directory: " +
            repo_ec.message());
    for (const auto& e : s->entries) {
        if (!selected(e.path)) continue;
        fs::path target = safe_restore_target(base, e.path, policy);
        if (path_is_within_existing_directory(canonical_repo_dir, target))
            die("refusing to restore into repository data: " +
                display_path(target));
        (void)verified_object_path(repo, e);
        pending.push_back(&e);
    }

    if (pending.empty())
        die("nothing in snapshot " + s->id +
            " matches the given path(s)");

    base = prepare_restore_base(base);
    validate_destination_mapping(base, pending);

    for (const Entry* entry : pending) {
        const Entry& e = *entry;
        fs::path target = safe_restore_target(base, e.path, policy);
        ensure_restore_parents(base, e.path);
        target = safe_restore_target(base, e.path, policy);
        if (path_is_within_existing_directory(canonical_repo_dir, target))
            die("refusing to restore into repository data: " +
                display_path(target));

        std::error_code ec;
        fs::file_status target_status = fs::symlink_status(target, ec);
        if (ec == std::errc::no_such_file_or_directory) {
            ec.clear();
            target_status = fs::file_status(fs::file_type::not_found);
        } else if (ec) {
            die("cannot inspect restore target " + display_path(target) + ": " +
                ec.message());
        }
        bool target_exists = fs::exists(target_status);
        if (target_exists) {
            if (fs::is_symlink(target_status) ||
                !fs::is_regular_file(target_status))
                die("refusing to replace non-regular restore target: " +
                    display_path(target));
            std::uint64_t cur_size = fs::file_size(target, ec);
            bool same = !ec && cur_size == e.size &&
                        hash_file(target) == e.hash;
            if (same) {
                fs::perms cur_mode = fs::status(target, ec).permissions();
                if (ec)
                    same = false;
                else
                    same = static_cast<std::uint32_t>(cur_mode) == e.mode;
            }
            if (same) {
                auto cur_mtime = fs::last_write_time(target, ec);
                if (ec)
                    same = false;
                else
                    same =
                        cur_mtime.time_since_epoch().count() == e.mtime;
            }
            if (same) { ++untouched; continue; }
            if (!force) {
                std::cout << yellow("! ")
                          << escape_terminal_controls(e.path)
                          << dim("  differs — use --force to overwrite") << "\n";
                ++skipped;
                continue;
            }
        }
        if (!extract_object(repo, e, target, force)) {
            std::cout << yellow("! ")
                      << escape_terminal_controls(e.path)
                      << dim("  appeared during restore — not overwritten")
                      << "\n";
            ++skipped;
            continue;
        }
        std::cout << green("✓ ")
                  << escape_terminal_controls(e.path) << "\n";
        ++restored;
    }

    std::cout << dim(std::to_string(restored) + " restored, " +
                     std::to_string(untouched) + " already identical, " +
                     std::to_string(skipped) + " skipped")
              << "\n";
    return skipped > 0 ? 1 : 0;
}

int cmd_prune(const Repo& repo, std::size_t keep, bool dry_run) {
    auto snaps = load_snapshots(repo);
    if (keep == 0) die("--keep must be at least 1");
    if (snaps.size() <= keep) {
        std::cout << dim("nothing to prune (" + std::to_string(snaps.size()) +
                         " snapshots, keeping " + std::to_string(keep) + ")")
                  << "\n";
        return 0;
    }

    std::size_t drop = snaps.size() - keep;
    std::map<std::string, std::uint64_t> live;
    for (std::size_t i = drop; i < snaps.size(); ++i) {
        for (const auto& e : snaps[i].entries) {
            auto [it, inserted] = live.emplace(e.hash, e.size);
            if (!inserted && it->second != e.size)
                die("inconsistent sizes for object " + e.hash);
        }
    }

    // Preflight the whole object store before deleting any manifest. Unknown
    // entries and symlinks indicate corruption or concurrent activity; either
    // case must stop garbage collection rather than risk following them.
    std::vector<fs::path> garbage;
    std::vector<fs::path> shards;
    std::set<std::string> seen_live;
    std::uint64_t freed = 0;
    std::error_code ec;
    if (fs::is_directory(repo.objects())) {
        for (const auto& shard : fs::directory_iterator(repo.objects())) {
            fs::file_status shard_status = shard.symlink_status(ec);
            std::string shard_name = shard.path().filename().string();
            if (ec || fs::is_symlink(shard_status) ||
                !fs::is_directory(shard_status) ||
                !is_lower_hex(shard_name, 2))
                die("invalid object-store shard: " +
                    display_path(shard.path()));
            shards.push_back(shard.path());
            for (const auto& obj : fs::directory_iterator(shard.path())) {
                fs::file_status object_status = obj.symlink_status(ec);
                std::string leaf = obj.path().filename().string();
                if (ec || fs::is_symlink(object_status) ||
                    !fs::is_regular_file(object_status) ||
                    !is_lower_hex(leaf, 62))
                    die("invalid object-store entry: " +
                        display_path(obj.path()));
                std::string hash = shard_name + leaf;
                std::uint64_t size = obj.file_size(ec);
                if (ec)
                    die("cannot inspect object " + display_path(obj.path()) +
                        ": " +
                        ec.message());
                auto live_entry = live.find(hash);
                if (live_entry != live.end()) {
                    if (size != live_entry->second ||
                        hash_file(obj.path()) != hash)
                        die("kept snapshot references a corrupt object: " +
                            hash);
                    seen_live.insert(hash);
                    continue;
                }
                freed += size;
                garbage.push_back(obj.path());
            }
        }
    }
    if (seen_live.size() != live.size())
        die("kept snapshots reference missing objects; refusing to prune");

    // The complete preflight succeeded, so mutations can begin.
    for (std::size_t i = 0; i < drop; ++i) {
        std::cout << red("- ") << snaps[i].id << "  "
                  << format_time(snaps[i].time) << "\n";
        if (!dry_run) {
            if (!fs::remove(snaps[i].file, ec) || ec)
                die("cannot remove snapshot " +
                    display_path(snaps[i].file) + ": " +
                    ec.message());
        }
    }

    if (!dry_run) {
        for (const auto& object : garbage) {
            fs::permissions(object, fs::perms::owner_write,
                            fs::perm_options::add, ec);
            if (ec)
                die("cannot make object removable " + display_path(object) +
                    ": " +
                    ec.message());
            if (!fs::remove(object, ec) || ec)
                die("cannot remove object " + display_path(object) + ": " +
                    ec.message());
        }
        for (const auto& shard : shards) {
            ec.clear();
            bool empty = fs::is_empty(shard, ec);
            if (ec)
                die("cannot clean object-store shard " + display_path(shard) +
                    ": " +
                    ec.message());
            if (!empty) continue;
            if (!fs::remove(shard, ec) || ec)
                die("cannot remove empty object-store shard " +
                    display_path(shard) + ": " + ec.message());
        }
    }

    std::cout << dim((dry_run ? std::string("[dry run] would remove ")
                              : std::string("removed ")) +
                     std::to_string(drop) + " snapshots and " +
                     std::to_string(garbage.size()) + " objects (" +
                     human_size(freed) + ")")
              << "\n";
    return 0;
}

int cmd_watch(const Repo& repo, int interval) {
    if (interval < 1) interval = 1;
    std::cout << "watching " << bold(display_path(repo.root)) << " every "
              << interval
              << "s — Ctrl+C to stop\n";

    auto report = [](const SnapResult& r) {
        if (r.created)
            std::cout << dim(format_time(now_unix())) << "  " << green("✓ ")
                      << bold(r.snap.id) << "  " << r.snap.entries.size()
                      << " files, " << human_size(r.new_bytes) << " stored\n";
    };

    // Establish a recoverable baseline immediately, then hash on each poll.
    // Metadata-only signatures can miss same-size edits with preserved mtimes.
    report(do_snap(repo, "auto (watch)"));

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        report(do_snap(repo, "auto (watch)"));
    }
    return 0;
}

int cmd_help() {
    std::cout <<
        bold("lapse") << " " << kVersion <<
        " — a tiny time machine for any folder\n"
        "\n"
        "usage: lapse <command> [args]\n"
        "\n" << bold("recording") << "\n"
        "  init [dir]              start tracking a folder (snap auto-inits too)\n"
        "  snap [-m <message>]     take a snapshot of the folder\n"
        "  watch [--interval N]    auto-snapshot whenever something changes\n"
        "\n" << bold("browsing") << "\n"
        "  log                     show the timeline of snapshots\n"
        "  status                  what changed since the last snapshot\n"
        "  show <id>               list every file inside a snapshot\n"
        "  diff <id> [<id>]        compare two snapshots (or one vs. now)\n"
        "  cat <id> <path>         print a file as it was in a snapshot\n"
        "\n" << bold("recovering") << "\n"
        "  restore <id> [path...]  bring files back (use --force to overwrite,\n"
        "                          --to <dir> to restore somewhere else)\n"
        "\n" << bold("housekeeping") << "\n"
        "  prune --keep <n>        drop old snapshots, free unreferenced data\n"
        "\n"
        "  <id> is a snapshot id from `lapse log`, a unique prefix, or `last`.\n"
        "  Put glob patterns in " << kIgnoreFile << " to exclude files.\n";
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    g_color = LAPSE_ISATTY(LAPSE_FILENO(stdout)) != 0;

    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return cmd_help();
    std::string cmd = args[0];
    args.erase(args.begin());

    if (cmd == "help" || cmd == "--help" || cmd == "-h") return cmd_help();
    if (cmd == "version" || cmd == "--version") {
        std::cout << "lapse " << kVersion << "\n";
        return 0;
    }

    try {
        if (cmd == "init") {
            if (args.size() > 1) die("usage: lapse init [dir]");
            fs::path where = args.empty() ? fs::current_path() : fs::path(args[0]);
            if (find_repo(where))
                die("already inside a lapse repository");
            Repo r = init_repo(where);
            std::cout << green("✓ ") << "tracking "
                      << bold(display_path(r.root))
                      << "  "
                      << dim("(data lives in " + display_path(r.dir()) + ")")
                      << "\n";
            return 0;
        }

        // Every other command runs inside a repo. `snap` auto-initializes.
        auto repo = find_repo(fs::current_path());
        if (!repo) {
            if (cmd == "snap") {
                repo = init_repo(fs::current_path());
                std::cout << dim("initialized lapse repository in ") 
                          << dim(display_path(repo->dir())) << "\n";
            } else {
                die("not inside a lapse repository (run `lapse snap` to start)");
            }
        }
        secure_repo_layout(*repo);

        if (cmd == "snap") {
            std::string message;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if ((args[i] == "-m" || args[i] == "--message") && i + 1 < args.size())
                    message = args[++i];
                else
                    die("unknown argument to snap: " + args[i]);
            }
            return cmd_snap(*repo, message);
        }
        if (cmd == "log") {
            if (!args.empty()) die("usage: lapse log");
            return cmd_log(*repo);
        }
        if (cmd == "status") {
            if (!args.empty()) die("usage: lapse status");
            return cmd_status(*repo);
        }
        if (cmd == "show") {
            if (args.size() != 1) die("usage: lapse show <id>");
            return cmd_show(*repo, args[0]);
        }
        if (cmd == "diff") {
            if (args.empty() || args.size() > 2)
                die("usage: lapse diff <id> [<id>]");
            return cmd_diff(*repo, args[0], args.size() > 1 ? args[1] : "");
        }
        if (cmd == "cat") {
            if (args.size() != 2) die("usage: lapse cat <id> <path>");
            std::string path = fs::path(args[1]).generic_string();
            return cmd_cat(*repo, args[0], path);
        }
        if (cmd == "restore") {
            if (args.empty()) die("usage: lapse restore <id> [path...] [--force] [--to <dir>]");
            std::string ref = args[0];
            std::vector<std::string> paths;
            fs::path to;
            bool force = false;
            bool parse_options = true;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (parse_options && args[i] == "--") {
                    parse_options = false;
                }
                else if (parse_options &&
                         (args[i] == "--force" || args[i] == "-f")) {
                    force = true;
                }
                else if (parse_options && args[i] == "--to") {
                    if (i + 1 >= args.size())
                        die("--to requires a directory");
                    to = args[++i];
                }
                else if (parse_options && starts_with(args[i], "-"))
                    die("unknown argument to restore: " + args[i]);
                else paths.push_back(args[i]);
            }
            return cmd_restore(*repo, ref, paths, to, force);
        }
        if (cmd == "prune") {
            std::size_t keep = 0;
            bool dry = false;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (args[i] == "--keep") {
                    if (i + 1 >= args.size())
                        die("--keep requires a count");
                    const std::string& value = args[++i];
                    if (value.empty() ||
                        !std::all_of(value.begin(), value.end(),
                                     [](unsigned char c) {
                                         return c >= '0' && c <= '9';
                                     }))
                        die("--keep must be a positive integer");
                    std::uint64_t parsed = std::stoull(value);
                    if (parsed > std::numeric_limits<std::size_t>::max())
                        die("--keep is too large");
                    keep = static_cast<std::size_t>(parsed);
                }
                else if (args[i] == "--dry-run" || args[i] == "-n") dry = true;
                else die("unknown argument to prune: " + args[i]);
            }
            if (keep == 0) die("usage: lapse prune --keep <n> [--dry-run]");
            return cmd_prune(*repo, keep, dry);
        }
        if (cmd == "watch") {
            int interval = 30;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (args[i] == "--interval") {
                    if (i + 1 >= args.size())
                        die("--interval requires a number of seconds");
                    const std::string& value = args[++i];
                    if (value.empty() ||
                        !std::all_of(value.begin(), value.end(),
                                     [](unsigned char c) {
                                         return c >= '0' && c <= '9';
                                     }))
                        die("--interval must be a positive integer");
                    std::uint64_t parsed = std::stoull(value);
                    if (parsed == 0 ||
                        parsed > static_cast<std::uint64_t>(
                                     std::numeric_limits<int>::max()))
                        die("--interval is out of range");
                    interval = static_cast<int>(parsed);
                }
                else die("unknown argument to watch: " + args[i]);
            }
            return cmd_watch(*repo, interval);
        }

        die("unknown command: " + cmd + " (try `lapse help`)");
    } catch (const fs::filesystem_error& e) {
        die(std::string("filesystem error: ") + e.what());
    } catch (const std::exception& e) {
        die(e.what());
    }
}
