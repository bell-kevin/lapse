<a name="readme-top"></a>

# lapse

**A tiny time machine for any folder.** One executable, zero config, and no
third-party libraries.

```
$ lapse snap -m "before I try something stupid"
✓ a3f9c12b04de  1204 files (312.5 MB), 9 new objects (1.1 MB stored), 1284 ms

$ lapse restore a3f9 thesis.docx --force
✓ thesis.docx
```

Everyone has lost work to a bad save, an overzealous `rm`, a script that
clobbered the wrong directory, or an app with no undo. Git solves this for
programmers who remember to commit. `lapse` solves it for *everything else* —
your documents, your configs, your photo edits, your kid's Minecraft saves —
with no staging area, no branches, no merge conflicts, and nothing to learn
beyond `snap` and `restore`.

## Why it's different

- **No third-party libraries.** `lapse` uses C++17 and the standard library.
  There is no libgit2, SQLite, or OpenSSL dependency; SHA-256 is implemented
  in-tree. A normal C++17 toolchain produces one native executable.
- **Content-addressed & deduplicated.** Every file is stored once, named by
  its SHA-256. A hundred snapshots of a 1 GB folder where one file changes
  costs you 1 GB plus the changed bytes.
- **Content-correct.** Every snapshot hashes the current bytes instead of
  trusting timestamps, so same-size edits cannot be missed. The object store
  still writes each distinct file content only once.
- **Unkillable format.** Snapshots are plain-text manifests; objects are
  plain files. If this binary vanished tomorrow, you could recover everything
  with `cat`, `grep`, and `cp`. Your data is never held hostage by a format.
- **Safe by default.** `restore` refuses to overwrite files that differ
  unless you pass `--force`, and can restore to a separate directory with
  `--to`.

## Quick start

```sh
git clone https://github.com/bell-kevin/lapse.git
cd lapse
make test
sudo make install

cd ~/Documents/thesis
lapse snap -m "draft 1"          # first snap auto-initializes .lapse/
# ... write, break, regret ...
lapse status                     # what changed since the last snapshot?
lapse log                        # browse the timeline
lapse diff last                  # snapshot vs. right now
lapse cat last chapter3.md       # peek at the old version
lapse restore last chapter3.md --force
```

Set it and forget it:

```sh
lapse watch --interval 60        # auto-snapshot whenever something changes
```

Identical states are never recorded twice, so `watch` produces a clean
timeline of *actual* changes, not noise.

## Commands

| Command | What it does |
|---|---|
| `lapse snap [-m msg]` | Snapshot the folder (auto-initializes on first use) |
| `lapse log` | Show the timeline |
| `lapse status` | What changed since the last snapshot |
| `lapse show <id>` | List every file in a snapshot |
| `lapse diff <id> [<id>]` | Compare two snapshots, or one against the working tree |
| `lapse cat <id> <path>` | Print a file as it was |
| `lapse restore <id> [paths…]` | Bring files back (`--force`, `--to <dir>`) |
| `lapse watch [--interval N]` | Auto-snapshot on change |
| `lapse prune --keep <n>` | Drop old snapshots and garbage-collect their data |

`<id>` is a snapshot id from `lapse log`, any unique prefix of one, or the
word `last`.

Drop glob patterns into a `.lapseignore` file to exclude things:

```
# .lapseignore
*.tmp
node_modules
build/
```

## How it works

`lapse` keeps everything in a `.lapse/` directory at the root of the tracked
folder:

```
.lapse/
├── objects/
│   └── a9/48904f2f0f479b8f81...   ← file contents, stored once, named by SHA-256
└── snapshots/
    └── 0000000042-1718100000-d2bac4ecb173.snap
```

A snapshot is just a manifest — one line per file:

```
lapse 2
id d2bac4ecb173
time 1718100000
message edited notes
files 3
a948904f…	644	1718099991	12	notes.txt
7364d374…	644	1718099812	13	src/main.cpp
```

Taking a snapshot means: walk the tree, hash every tracked file, copy unseen
objects into the store, and write a manifest. Restoring means: read and
validate the manifest, verify its objects, stage the selected files, then
reapply permissions and mtimes. The contents remain ordinary files that can
also be recovered with standard filesystem tools. Version 2 manifests use a
portable path subset; historical version 1 manifests remain readable with
their original platform-native path semantics.

## How it compares

| | lapse | git | Time Machine / File History | Dropbox et al. |
|---|---|---|---|---|
| Works on any folder | ✓ | ✓ | system-wide only | synced dirs only |
| Binary files | ✓ great | poor | ✓ | ✓ |
| Zero setup / zero learning curve | ✓ | ✗ | ~ | ~ |
| Local & private | ✓ | ✓ | ✓ | ✗ |
| Single native executable | ✓ | ✗ | ✗ | ✗ |
| Deduplicated storage | ✓ | ✓ | hardlinks | n/a |

`lapse` is not a backup tool (the history lives next to the data — pair it
with real backups) and not a version-control system for collaboration. It's
the missing *undo button for your filesystem*.

## Building

Building the executable requires CMake 3.20+ or a C++17 compiler and Make.
Running the integration tests also requires Python 3.9+.

With the Makefile:

```sh
make
make test
sudo make install
```

Or with CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure --no-tests=error
sudo cmake --install build --config Release
```

The application itself has no third-party runtime dependencies. CI builds and
tests it on Linux, macOS, and Windows.

## Limitations & roadmap

- Symbolic links and empty directories are currently skipped.
- Run only one mutating `lapse` command per repository at a time, and do not
  modify a restore destination concurrently. Repository locking and
  handle-relative restore operations are not implemented yet.
- File timestamp encoding is implementation-specific, so restore metadata with
  the same platform/toolchain that created the snapshot. File contents remain
  portable.
- `watch` hashes every tracked file on each polling interval to avoid missing
  content changes with preserved timestamps.
- Objects are stored uncompressed (simple > clever, for now). Optional zstd
  compression is on the roadmap.
- Planned: `lapse mount` (browse history as a FUSE filesystem),
  inotify-based `watch` on Linux, block-level chunking for huge files that
  change a little (databases, VM images).

Contributions for any of the above are very welcome — the codebase is three
source files and deliberately easy to read end-to-end.

## License

GNU Affero General Public License v3.0 only
([AGPL-3.0-only](LICENSE)).

--------------------------------------------------------------------------------------------------------------------------
== We're Using GitHub Under Protest ==

This project is currently hosted on GitHub.  This is not ideal; GitHub is a
proprietary, trade-secret system that is not Free and Open Source Software
(FOSS).  We are deeply concerned about using a proprietary system like GitHub
to develop our FOSS project. I have a [website](https://bellKevin.me) where the
project contributors are actively discussing how we can move away from GitHub
in the long term.  We urge you to read about the [Give up GitHub](https://GiveUpGitHub.org) campaign 
from [the Software Freedom Conservancy](https://sfconservancy.org) to understand some of the reasons why GitHub is not 
a good place to host FOSS projects.

If you are a contributor who personally has already quit using GitHub, please
email me at **kevinBell@Linux.com** for how to send us contributions without
using GitHub directly.

Any use of this project's code by GitHub Copilot, past or present, is done
without our permission.  We do not consent to GitHub's use of this project's
code in Copilot.

![Logo of the GiveUpGitHub campaign](https://sfconservancy.org/img/GiveUpGitHub.png)

<p align="right"><a href="#readme-top">back to top</a></p>
