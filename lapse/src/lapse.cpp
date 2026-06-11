// lapse — a tiny time machine for any folder
// SPDX-License-Identifier: MIT
//
// One static binary. Zero config. Zero dependencies.
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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define LAPSE_ISATTY _isatty
#define LAPSE_FILENO _fileno
#else
#include <unistd.h>
#define LAPSE_ISATTY isatty
#define LAPSE_FILENO fileno
#endif

namespace fs = std::filesystem;
using lapse::Sha256;

namespace {

constexpr const char* kVersion = "0.1.0";
constexpr const char* kRepoDir = ".lapse";
constexpr const char* kIgnoreFile = ".lapseignore";

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

bool g_color = false;

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
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
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

std::optional<Repo> find_repo(fs::path start) {
    start = fs::absolute(start);
    for (fs::path p = start;; p = p.parent_path()) {
        if (fs::is_directory(p / kRepoDir)) return Repo{p};
        if (p == p.parent_path()) return std::nullopt;
    }
}

Repo init_repo(const fs::path& where) {
    Repo r{fs::absolute(where)};
    fs::create_directories(r.objects());
    fs::create_directories(r.snapshots());
    return r;
}

// ---------------------------------------------------------------------------
// Ignore rules (.lapseignore: one glob per line, '#' comments)
// ---------------------------------------------------------------------------

struct IgnoreRules {
    std::vector<std::string> patterns;

    void load(const fs::path& root) {
        patterns.clear();
        std::ifstream in(root / kIgnoreFile);
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

std::string entries_text(const std::vector<Entry>& es) {
    std::ostringstream os;
    for (const auto& e : es) {
        os << e.hash << '\t' << std::oct << e.mode << std::dec << '\t'
           << e.mtime << '\t' << e.size << '\t' << e.path << '\n';
    }
    return os.str();
}

void write_snapshot(const Repo& repo, Snapshot& snap) {
    std::string body = entries_text(snap.entries);
    snap.id = Sha256::hex_digest(std::to_string(snap.seq) + "\n" +
                                 std::to_string(snap.time) + "\n" + body)
                  .substr(0, 12);

    std::string msg = snap.message;
    std::replace(msg.begin(), msg.end(), '\n', ' ');

    char seqbuf[24];
    std::snprintf(seqbuf, sizeof seqbuf, "%010llu",
                  static_cast<unsigned long long>(snap.seq));
    fs::path file = repo.snapshots() /
                    (std::string(seqbuf) + "-" + std::to_string(snap.time) +
                     "-" + snap.id + ".snap");
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) die("cannot write snapshot file: " + tmp.string());
        out << "lapse 1\n"
            << "id " << snap.id << "\n"
            << "time " << snap.time << "\n"
            << "message " << msg << "\n"
            << "files " << snap.entries.size() << "\n"
            << body;
    }
    fs::rename(tmp, file);
    snap.file = file;
}

std::optional<Snapshot> read_snapshot(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;

    Snapshot s;
    s.file = file;
    std::string line;
    std::size_t nfiles = 0;

    auto value_of = [](const std::string& l, const char* key) -> std::string {
        std::string k = std::string(key) + " ";
        return starts_with(l, k) ? l.substr(k.size()) : std::string();
    };

    if (!std::getline(in, line) || !starts_with(line, "lapse ")) return std::nullopt;
    while (std::getline(in, line)) {
        if (starts_with(line, "id "))      s.id = value_of(line, "id");
        else if (starts_with(line, "time ")) s.time = std::stoll(value_of(line, "time"));
        else if (starts_with(line, "message ")) s.message = value_of(line, "message");
        else if (starts_with(line, "files ")) { nfiles = std::stoull(value_of(line, "files")); break; }
    }

    s.entries.reserve(nfiles);
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        // hash \t mode(octal) \t mtime \t size \t path (path may contain tabs)
        std::size_t t1 = line.find('\t');
        std::size_t t2 = line.find('\t', t1 + 1);
        std::size_t t3 = line.find('\t', t2 + 1);
        std::size_t t4 = line.find('\t', t3 + 1);
        if (t4 == std::string::npos) continue;
        Entry e;
        e.hash  = line.substr(0, t1);
        e.mode  = std::uint32_t(std::stoul(line.substr(t1 + 1, t2 - t1 - 1), nullptr, 8));
        e.mtime = std::stoll(line.substr(t2 + 1, t3 - t2 - 1));
        e.size  = std::stoull(line.substr(t3 + 1, t4 - t3 - 1));
        e.path  = line.substr(t4 + 1);
        s.entries.push_back(std::move(e));
    }
    return s;
}

// All snapshots, oldest first (ordered by their monotonic sequence number).
std::vector<Snapshot> load_snapshots(const Repo& repo) {
    std::vector<Snapshot> out;
    if (!fs::is_directory(repo.snapshots())) return out;
    for (const auto& de : fs::directory_iterator(repo.snapshots())) {
        if (!de.is_regular_file()) continue;
        if (de.path().extension() != ".snap") continue;
        if (auto s = read_snapshot(de.path())) {
            std::string stem = de.path().stem().string();
            try {
                s->seq = std::stoull(stem.substr(0, stem.find('-')));
            } catch (...) {
                s->seq = 0;
            }
            out.push_back(std::move(*s));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Snapshot& a, const Snapshot& b) {
                  if (a.seq != b.seq) return a.seq < b.seq;
                  return a.time != b.time ? a.time < b.time : a.id < b.id;
              });
    return out;
}

// Resolve "last", a full id, or a unique id prefix.
const Snapshot* resolve(const std::vector<Snapshot>& snaps, const std::string& ref) {
    if (snaps.empty()) return nullptr;
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

std::string hash_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) die("cannot read file: " + p.string());
    Sha256 h;
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), std::streamsize(buf.size()));
        std::streamsize n = in.gcount();
        if (n > 0) h.update(buf.data(), std::size_t(n));
    }
    return Sha256::to_hex(h.finish());
}

// Returns bytes added to the store (0 if the object already existed).
std::uint64_t store_object(const Repo& repo, const fs::path& src,
                           const std::string& hash, std::uint64_t size) {
    fs::path obj = object_path(repo, hash);
    std::error_code ec;
    if (fs::exists(obj, ec)) return 0;
    fs::create_directories(obj.parent_path());

    fs::path tmp = obj;
    tmp += ".tmp";
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing);
    fs::rename(tmp, obj);
    fs::permissions(obj,
                    fs::perms::owner_read | fs::perms::group_read |
                        fs::perms::others_read,
                    fs::perm_options::replace, ec);
    return size;
}

void extract_object(const Repo& repo, const Entry& e, const fs::path& target) {
    fs::path obj = object_path(repo, e.hash);
    if (!fs::exists(obj)) die("missing object for " + e.path + " (store corrupted?)");
    fs::create_directories(target.parent_path());

    std::error_code ec;
    fs::copy_file(obj, target, fs::copy_options::overwrite_existing, ec);
    if (ec) die("cannot restore " + target.string() + ": " + ec.message());
    fs::permissions(target, static_cast<fs::perms>(e.mode),
                    fs::perm_options::replace, ec);
    fs::last_write_time(
        target, fs::file_time_type(fs::file_time_type::duration(e.mtime)), ec);
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
    fs::recursive_directory_iterator it(
        repo.root, fs::directory_options::skip_permission_denied, ec);
    if (ec) die("cannot scan " + repo.root.string() + ": " + ec.message());

    for (auto end = fs::recursive_directory_iterator(); it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& de = *it;
        std::string rel = fs::relative(de.path(), repo.root, ec).generic_string();
        if (ec || rel.empty() || rel == ".") { ec.clear(); continue; }

        if (de.is_directory(ec)) {
            if (ig.ignored(rel)) it.disable_recursion_pending();
            continue;
        }
        if (ig.ignored(rel)) continue;
        if (de.is_symlink(ec) || !de.is_regular_file(ec)) continue;

        ScanItem item;
        item.rel = rel;
        item.abs = de.path();
        item.size = de.file_size(ec);
        if (ec) { ec.clear(); continue; }
        item.mtime = de.last_write_time(ec).time_since_epoch().count();
        if (ec) { ec.clear(); continue; }
        item.mode = static_cast<std::uint32_t>(de.status(ec).permissions());
        out.push_back(std::move(item));
    }
    std::sort(out.begin(), out.end(),
              [](const ScanItem& a, const ScanItem& b) { return a.rel < b.rel; });
    return out;
}

// Cheap signature of the tree shape (paths + sizes + mtimes). Used by `watch`
// and by `snap` to detect "nothing changed" without hashing anything.
std::string tree_signature(const std::vector<ScanItem>& items) {
    Sha256 h;
    for (const auto& i : items) {
        std::string line = i.rel + "|" + std::to_string(i.size) + "|" +
                           std::to_string(i.mtime) + "\n";
        h.update(line.data(), line.size());
    }
    return Sha256::to_hex(h.finish());
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

    // Index of the previous snapshot for fast incremental hashing:
    // if (size, mtime) match, trust the old hash and skip reading the file.
    std::map<std::string, const Entry*> prev;
    if (!snaps.empty())
        for (const auto& e : snaps.back().entries) prev[e.path] = &e;

    auto items = scan_tree(repo, ig);

    SnapResult r;
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

        auto p = prev.find(i.rel);
        if (p != prev.end() && p->second->size == i.size &&
            p->second->mtime == i.mtime) {
            e.hash = p->second->hash;          // unchanged — reuse
        } else {
            e.hash = hash_file(i.abs);          // new or modified — hash it
        }

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
        std::cout << "  " << dim("\"" + r.snap.message + "\"") << "\n";
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
                  << (it->message.empty() ? dim("(no message)") : it->message)
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
        if (l.kind == 'A') { std::cout << green("A  ") << l.path << "\n"; ++a; }
        if (l.kind == 'M') { std::cout << yellow("M  ") << l.path << "\n"; ++m; }
        if (l.kind == 'D') { std::cout << red("D  ") << l.path << "\n"; ++d; }
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
        else if (it->second.hash != e.hash) out.push_back({'M', path});
    }
    for (const auto& [path, e] : oldm) {
        (void)e;
        if (!newm.count(path)) out.push_back({'D', path});
    }
    return out;
}

// Scan the working tree and produce entries, hashing only what `since`
// can't vouch for. Used by status and diff-against-worktree.
std::map<std::string, Entry> worktree_entries(const Repo& repo,
                                              const Snapshot* since) {
    IgnoreRules ig;
    ig.load(repo.root);
    std::map<std::string, const Entry*> prev;
    if (since)
        for (const auto& e : since->entries) prev[e.path] = &e;

    std::map<std::string, Entry> out;
    for (const auto& i : scan_tree(repo, ig)) {
        Entry e;
        e.path = i.rel;
        e.size = i.size;
        e.mtime = i.mtime;
        e.mode = i.mode;
        auto p = prev.find(i.rel);
        if (p != prev.end() && p->second->size == i.size &&
            p->second->mtime == i.mtime)
            e.hash = p->second->hash;
        else
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
                             worktree_entries(repo, &last));
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
                                 worktree_entries(repo, &snaps.back()));
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
              << (s->message.empty() ? dim("(no message)") : s->message) << "\n";
    for (const auto& e : s->entries)
        std::cout << "  " << dim(e.hash.substr(0, 12)) << "  "
                  << human_size(e.size) << "\t" << e.path << "\n";
    std::cout << dim(std::to_string(s->entries.size()) + " files, " +
                     human_size(s->total_size()))
              << "\n";
    return 0;
}

int cmd_cat(const Repo& repo, const std::string& ref, const std::string& path) {
    auto snaps = load_snapshots(repo);
    const Snapshot* s = resolve(snaps, ref);
    if (!s) die("unknown snapshot: " + ref);
    for (const auto& e : s->entries) {
        if (e.path == path) {
            std::ifstream in(object_path(repo, e.hash), std::ios::binary);
            if (!in) die("missing object for " + path);
            std::cout << in.rdbuf();
            return 0;
        }
    }
    die("'" + path + "' is not in snapshot " + s->id);
}

int cmd_restore(const Repo& repo, const std::string& ref,
                const std::vector<std::string>& paths, const fs::path& to,
                bool force) {
    auto snaps = load_snapshots(repo);
    const Snapshot* s = resolve(snaps, ref);
    if (!s) die("unknown snapshot: " + ref);

    auto selected = [&](const std::string& p) {
        if (paths.empty()) return true;
        for (const auto& want : paths)
            if (p == want || starts_with(p, want + "/")) return true;
        return false;
    };

    fs::path base = to.empty() ? repo.root : fs::absolute(to);
    std::size_t restored = 0, skipped = 0, untouched = 0;

    for (const auto& e : s->entries) {
        if (!selected(e.path)) continue;
        fs::path target = base / fs::path(e.path);

        std::error_code ec;
        if (fs::exists(target, ec) && to.empty()) {
            // Restoring into the live tree: be careful with existing files.
            std::uint64_t cur_size = fs::file_size(target, ec);
            bool same = !ec && cur_size == e.size && hash_file(target) == e.hash;
            if (same) { ++untouched; continue; }
            if (!force) {
                std::cout << yellow("! ") << e.path
                          << dim("  differs — use --force to overwrite") << "\n";
                ++skipped;
                continue;
            }
        }
        extract_object(repo, e, target);
        std::cout << green("✓ ") << e.path << "\n";
        ++restored;
    }

    if (restored + skipped + untouched == 0)
        die("nothing in snapshot " + s->id + " matches the given path(s)");

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
    std::set<std::string> live;
    for (std::size_t i = drop; i < snaps.size(); ++i)
        for (const auto& e : snaps[i].entries) live.insert(e.hash);

    // Remove old snapshot manifests.
    for (std::size_t i = 0; i < drop; ++i) {
        std::cout << red("- ") << snaps[i].id << "  "
                  << format_time(snaps[i].time) << "\n";
        if (!dry_run) fs::remove(snaps[i].file);
    }

    // Garbage-collect unreferenced objects.
    std::uint64_t freed = 0;
    std::size_t removed = 0;
    std::error_code ec;
    if (fs::is_directory(repo.objects())) {
        for (const auto& shard : fs::directory_iterator(repo.objects())) {
            if (!shard.is_directory()) continue;
            for (const auto& obj : fs::directory_iterator(shard.path())) {
                std::string hash =
                    shard.path().filename().string() + obj.path().filename().string();
                if (live.count(hash)) continue;
                freed += obj.file_size(ec);
                ++removed;
                if (!dry_run) {
                    fs::permissions(obj.path(), fs::perms::owner_write,
                                    fs::perm_options::add, ec);
                    fs::remove(obj.path(), ec);
                }
            }
            if (!dry_run) fs::remove(shard.path(), ec); // removes only if empty
        }
    }

    std::cout << dim((dry_run ? std::string("[dry run] would remove ")
                              : std::string("removed ")) +
                     std::to_string(drop) + " snapshots and " +
                     std::to_string(removed) + " objects (" + human_size(freed) +
                     ")")
              << "\n";
    return 0;
}

int cmd_watch(const Repo& repo, int interval) {
    if (interval < 1) interval = 1;
    std::cout << "watching " << bold(repo.root.string()) << " every " << interval
              << "s — Ctrl+C to stop\n";

    IgnoreRules ig;
    std::string last_sig;
    {
        ig.load(repo.root);
        last_sig = tree_signature(scan_tree(repo, ig));
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        ig.load(repo.root);
        std::string sig = tree_signature(scan_tree(repo, ig));
        if (sig == last_sig) continue;

        SnapResult r = do_snap(repo, "auto (watch)");
        last_sig = sig;
        if (r.created)
            std::cout << dim(format_time(now_unix())) << "  " << green("✓ ")
                      << bold(r.snap.id) << "  " << r.snap.entries.size()
                      << " files, " << human_size(r.new_bytes) << " stored\n";
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
            fs::path where = args.empty() ? fs::current_path() : fs::path(args[0]);
            if (find_repo(where))
                die("already inside a lapse repository");
            Repo r = init_repo(where);
            std::cout << green("✓ ") << "tracking " << bold(r.root.string())
                      << "  " << dim("(data lives in " + r.dir().string() + ")")
                      << "\n";
            return 0;
        }

        // Every other command runs inside a repo. `snap` auto-initializes.
        auto repo = find_repo(fs::current_path());
        if (!repo) {
            if (cmd == "snap") {
                repo = init_repo(fs::current_path());
                std::cout << dim("initialized lapse repository in ") 
                          << dim(repo->dir().string()) << "\n";
            } else {
                die("not inside a lapse repository (run `lapse snap` to start)");
            }
        }

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
        if (cmd == "log")    return cmd_log(*repo);
        if (cmd == "status") return cmd_status(*repo);
        if (cmd == "show") {
            if (args.empty()) die("usage: lapse show <id>");
            return cmd_show(*repo, args[0]);
        }
        if (cmd == "diff") {
            if (args.empty()) die("usage: lapse diff <id> [<id>]");
            return cmd_diff(*repo, args[0], args.size() > 1 ? args[1] : "");
        }
        if (cmd == "cat") {
            if (args.size() < 2) die("usage: lapse cat <id> <path>");
            return cmd_cat(*repo, args[0], args[1]);
        }
        if (cmd == "restore") {
            if (args.empty()) die("usage: lapse restore <id> [path...] [--force] [--to <dir>]");
            std::string ref = args[0];
            std::vector<std::string> paths;
            fs::path to;
            bool force = false;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--force" || args[i] == "-f") force = true;
                else if (args[i] == "--to" && i + 1 < args.size()) to = args[++i];
                else paths.push_back(args[i]);
            }
            return cmd_restore(*repo, ref, paths, to, force);
        }
        if (cmd == "prune") {
            std::size_t keep = 0;
            bool dry = false;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (args[i] == "--keep" && i + 1 < args.size())
                    keep = std::stoull(args[++i]);
                else if (args[i] == "--dry-run" || args[i] == "-n") dry = true;
                else die("unknown argument to prune: " + args[i]);
            }
            if (keep == 0) die("usage: lapse prune --keep <n> [--dry-run]");
            return cmd_prune(*repo, keep, dry);
        }
        if (cmd == "watch") {
            int interval = 30;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (args[i] == "--interval" && i + 1 < args.size())
                    interval = std::stoi(args[++i]);
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
