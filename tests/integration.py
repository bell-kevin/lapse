#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Portable end-to-end tests for the lapse command-line program."""

import hashlib
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


BINARY = None


class LapseIntegrationTests(unittest.TestCase):
    maxDiff = None

    def setUp(self):
        self._temp = tempfile.TemporaryDirectory(prefix="lapse-integration-")
        self.base = Path(self._temp.name)
        self.repo = self.base / "project"
        self.repo.mkdir()

    def tearDown(self):
        # Objects are deliberately read-only. Make them removable on Windows.
        for path in self.base.rglob("*"):
            if path.is_file():
                try:
                    path.chmod(stat.S_IRUSR | stat.S_IWUSR)
                except OSError:
                    pass
        self._temp.cleanup()

    def lapse(self, *args, cwd=None, ok=True):
        command = [str(BINARY)] + [str(arg) for arg in args]
        result = subprocess.run(
            command,
            cwd=str(cwd or self.repo),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding="utf-8",
            errors="replace",
            timeout=20,
            check=False,
        )
        details = (
            "command: {0}\nexit: {1}\nstdout:\n{2}\nstderr:\n{3}".format(
                command, result.returncode, result.stdout, result.stderr
            )
        )
        if ok:
            self.assertEqual(result.returncode, 0, details)
        else:
            self.assertNotEqual(result.returncode, 0, details)
        return result

    def lapse_bytes(self, *args, cwd=None):
        command = [str(BINARY)] + [str(arg) for arg in args]
        result = subprocess.run(
            command,
            cwd=str(cwd or self.repo),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=20,
            check=False,
        )
        details = (
            "command: {0}\nexit: {1}\nstdout: {2!r}\nstderr: {3!r}".format(
                command, result.returncode, result.stdout, result.stderr
            )
        )
        self.assertEqual(result.returncode, 0, details)
        return result

    def snapshots(self):
        return sorted((self.repo / ".lapse" / "snapshots").glob("*.snap"))

    @staticmethod
    def snapshot_id(path):
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.startswith("id "):
                return line[3:]
        raise AssertionError("snapshot has no id: {0}".format(path))

    @staticmethod
    def snapshot_entries(path):
        entries = []
        for raw_line in path.read_bytes().splitlines():
            parts = raw_line.decode("utf-8").split("\t", 4)
            if len(parts) == 5:
                entries.append(
                    {
                        "hash": parts[0],
                        "mode": parts[1],
                        "mtime": parts[2],
                        "size": int(parts[3]),
                        "path": parts[4],
                    }
                )
        return entries

    @staticmethod
    def write_manifest_lines(manifest, lines):
        time_value = None
        files_index = None
        for index, line in enumerate(lines):
            if line.startswith("time "):
                time_value = int(line[5:])
            if line.startswith("files "):
                files_index = index

        if time_value is None or files_index is None:
            raise AssertionError("could not parse manifest headers")

        sequence = int(manifest.name.split("-", 1)[0])
        body = "".join(line + "\n" for line in lines[files_index + 1 :])
        identity = "{0}\n{1}\n".format(sequence, time_value) + body
        if lines[0] != "lapse 1":
            identity = lines[0] + "\n" + identity
        snapshot_id = hashlib.sha256(identity.encode("utf-8")).hexdigest()[:12]
        for index, line in enumerate(lines):
            if line.startswith("id "):
                lines[index] = "id " + snapshot_id
                break

        rewritten = manifest.with_name(
            "{0:010d}-{1}-{2}.snap".format(sequence, time_value, snapshot_id)
        )
        with rewritten.open("w", encoding="utf-8", newline="\n") as output:
            output.write("\n".join(lines) + "\n")
        if rewritten != manifest:
            manifest.unlink()
        return rewritten

    @staticmethod
    def rewrite_manifest_path(manifest, old_path, new_path):
        lines = manifest.read_text(encoding="utf-8").splitlines()
        replaced = False
        for index, line in enumerate(lines):
            parts = line.split("\t", 4)
            if len(parts) == 5 and parts[4] == old_path:
                parts[4] = new_path
                lines[index] = "\t".join(parts)
                replaced = True
        if not replaced:
            raise AssertionError("could not rewrite manifest entry")
        return LapseIntegrationTests.write_manifest_lines(manifest, lines)

    @staticmethod
    def tree_bytes(root):
        return {
            path.relative_to(root).as_posix(): path.read_bytes()
            for path in root.rglob("*")
            if path.is_file()
        }

    def test_snapshot_status_restore_diff_and_prune(self):
        src = self.repo / "src"
        src.mkdir()
        notes = self.repo / "notes.txt"
        notes.write_text("hello\n", encoding="utf-8")
        (src / "main.cpp").write_text("int main(){}\n", encoding="utf-8")

        self.lapse("snap", "-m", "first")
        first_manifest = self.snapshots()[0]
        first = self.snapshot_id(first_manifest)

        notes.write_text("hello world\n", encoding="utf-8")
        (self.repo / "scratch.tmp").write_text("junk\n", encoding="utf-8")
        (self.repo / ".lapseignore").write_text("*.tmp\n", encoding="utf-8")

        status_result = self.lapse("status")
        self.assertIn("M  notes.txt", status_result.stdout)
        self.assertNotIn("scratch.tmp", status_result.stdout)

        self.lapse("snap", "-m", "second")
        self.assertEqual(len(self.snapshots()), 2)
        self.assertIn("second", self.lapse("log").stdout)
        self.assertNotIn("scratch.tmp", self.lapse("show", "last").stdout)

        no_op = self.lapse("snap")
        self.assertIn("nothing changed", no_op.stdout)
        self.assertEqual(len(self.snapshots()), 2)

        self.assertEqual(self.lapse("cat", first, "notes.txt").stdout, "hello\n")
        self.assertEqual(
            self.lapse("cat", "last", "notes.txt").stdout, "hello world\n"
        )

        self.lapse("restore", first, "notes.txt", ok=False)
        self.assertEqual(notes.read_text(encoding="utf-8"), "hello world\n")

        self.lapse("restore", first, "notes.txt", "--force")
        self.assertEqual(notes.read_text(encoding="utf-8"), "hello\n")

        export = self.base / "export"
        self.lapse("restore", "last", "--to", export)
        self.assertEqual(
            (export / "notes.txt").read_text(encoding="utf-8"), "hello world\n"
        )
        self.assertTrue((export / "src" / "main.cpp").is_file())

        diff_result = self.lapse("diff", first, "last")
        self.assertIn("M  notes.txt", diff_result.stdout)

        self.lapse("prune", "--keep", "1")
        self.assertEqual(len(self.snapshots()), 1)
        self.assertEqual(
            self.lapse("cat", "last", "notes.txt").stdout, "hello world\n"
        )

    def test_restore_to_does_not_clobber_without_force(self):
        (self.repo / "notes.txt").write_text("from snapshot\n", encoding="utf-8")
        self.lapse("snap")

        destination = self.base / "destination"
        destination.mkdir()
        restored = destination / "notes.txt"
        restored.write_text("keep me\n", encoding="utf-8")

        self.lapse("restore", "last", "--to", destination, ok=False)
        self.assertEqual(restored.read_text(encoding="utf-8"), "keep me\n")

        self.lapse("restore", "last", "--to", destination, "--force")
        self.assertEqual(restored.read_text(encoding="utf-8"), "from snapshot\n")

    def test_same_size_change_with_preserved_mtime_is_detected(self):
        tracked = self.repo / "stable.txt"
        tracked.write_bytes(b"AAAA")
        original = tracked.stat()
        self.lapse("snap")

        tracked.write_bytes(b"BBBB")
        os.utime(
            tracked,
            ns=(original.st_atime_ns, original.st_mtime_ns),
        )
        changed = tracked.stat()
        self.assertEqual(changed.st_size, original.st_size)
        self.assertEqual(changed.st_mtime_ns, original.st_mtime_ns)

        self.assertIn("M  stable.txt", self.lapse("status").stdout)
        self.lapse("snap")
        self.assertEqual(len(self.snapshots()), 2)
        self.assertEqual(self.lapse("cat", "last", "stable.txt").stdout, "BBBB")

    def test_prune_fails_closed_on_a_corrupt_manifest(self):
        tracked = self.repo / "versions.txt"
        for index, content in enumerate(
            ("version one\n", "version number two\n", "version number three!!!\n")
        ):
            tracked.write_text(content, encoding="utf-8")
            self.lapse("snap", "-m", "version {0}".format(index + 1))

        manifests = self.snapshots()
        self.assertEqual(len(manifests), 3)
        manifests[1].write_bytes(b"not a lapse manifest\n")

        lapse_dir = self.repo / ".lapse"
        before = self.tree_bytes(lapse_dir)
        self.lapse("prune", "--keep", "1", ok=False)
        self.assertEqual(self.tree_bytes(lapse_dir), before)

    def test_corrupt_objects_are_rejected_before_output_or_restore(self):
        payload = b"trusted payload"
        (self.repo / "a-good.bin").write_bytes(b"must not be restored")
        (self.repo / "z-payload.bin").write_bytes(payload)
        self.lapse("snap")

        entry = next(
            item
            for item in self.snapshot_entries(self.snapshots()[0])
            if item["path"] == "z-payload.bin"
        )
        object_file = (
            self.repo
            / ".lapse"
            / "objects"
            / entry["hash"][:2]
            / entry["hash"][2:]
        )
        object_file.chmod(stat.S_IRUSR | stat.S_IWUSR)
        tampered = b"X" * len(payload)
        object_file.write_bytes(tampered)

        cat_result = self.lapse("cat", "last", "z-payload.bin", ok=False)
        self.assertEqual(cat_result.stdout, "")

        export = self.base / "corrupt-export"
        self.lapse("restore", "last", "--to", export, ok=False)
        self.assertFalse((export / "a-good.bin").exists())
        self.assertFalse((export / "z-payload.bin").exists())

    def test_manifest_path_traversal_is_rejected(self):
        (self.repo / "safe.txt").write_text("safe\n", encoding="utf-8")
        self.lapse("snap")
        manifest = self.snapshots()[0]

        self.rewrite_manifest_path(manifest, "safe.txt", "../escaped.txt")

        export = self.base / "traversal-export"
        escaped = self.base / "escaped.txt"
        self.lapse("restore", "last", "--to", export, "--force", ok=False)
        self.assertFalse(escaped.exists())

    @unittest.skipIf(os.name == "nt", "backslash is a native separator on Windows")
    def test_version_one_posix_backslash_filename_remains_readable(self):
        (self.repo / "safe.txt").write_text("legacy\n", encoding="utf-8")
        self.lapse("snap")
        manifest = self.rewrite_manifest_path(
            self.snapshots()[0], "safe.txt", r"folder\..\legacy.txt"
        )
        lines = manifest.read_text(encoding="utf-8").splitlines()
        lines[0] = "lapse 1"
        manifest = self.write_manifest_lines(manifest, lines)

        self.assertIn(
            r"folder\..\legacy.txt", self.lapse("show", "last").stdout
        )
        export = self.base / "legacy-export"
        self.lapse("restore", "last", "--to", export)
        self.assertEqual(
            (export / r"folder\..\legacy.txt").read_text(encoding="utf-8"),
            "legacy\n",
        )

    @unittest.skipIf(os.name == "nt", "control bytes are invalid Windows names")
    def test_version_one_control_filename_is_readable_and_escaped(self):
        (self.repo / "safe.txt").write_text("legacy control\n", encoding="utf-8")
        self.lapse("snap")
        legacy_name = "legacy\t\x1b-name.txt"
        manifest = self.rewrite_manifest_path(
            self.snapshots()[0], "safe.txt", legacy_name
        )
        lines = manifest.read_text(encoding="utf-8").splitlines()
        lines[0] = "lapse 1"
        self.write_manifest_lines(manifest, lines)

        shown = self.lapse("show", "last")
        self.assertNotIn("\t\x1b", shown.stdout)
        self.assertNotIn("\x1b", shown.stdout)
        self.assertIn(r"legacy\x09\x1b-name.txt", shown.stdout)

        export = self.base / "legacy-control-export"
        restored = self.lapse("restore", "last", "--to", export)
        self.assertNotIn("\x1b", restored.stdout)
        self.assertEqual(
            (export / legacy_name).read_text(encoding="utf-8"),
            "legacy control\n",
        )

    @unittest.skipIf(os.name == "nt", "backslash is a native separator on Windows")
    def test_version_two_rejects_portable_backslash_traversal(self):
        (self.repo / "safe.txt").write_text("safe\n", encoding="utf-8")
        self.lapse("snap")
        self.rewrite_manifest_path(
            self.snapshots()[0], "safe.txt", r"folder\..\escaped.txt"
        )

        export = self.base / "portable-path-export"
        self.lapse("restore", "last", "--to", export, ok=False)
        self.assertFalse((export / r"folder\..\escaped.txt").exists())

    @unittest.skipIf(os.name == "nt", "backslash is a native separator on Windows")
    def test_new_snapshots_reject_literal_backslashes(self):
        name = r"literal\backslash.txt"
        (self.repo / name).write_text("not portable\n", encoding="utf-8")
        self.lapse("snap", ok=False)
        self.assertEqual(self.snapshots(), [])

    @unittest.skipIf(os.name == "nt", "Windows filenames are Unicode")
    def test_new_snapshots_reject_invalid_utf8_filenames(self):
        raw_path = os.fsencode(self.repo) + b"/invalid-\xff-name.txt"
        descriptor = os.open(raw_path, os.O_WRONLY | os.O_CREAT, 0o600)
        try:
            os.write(descriptor, b"not portable\n")
        finally:
            os.close(descriptor)

        self.lapse("snap", ok=False)
        self.assertEqual(self.snapshots(), [])

    def test_absolute_manifest_path_is_rejected(self):
        (self.repo / "safe.txt").write_text("safe\n", encoding="utf-8")
        self.lapse("snap")
        manifest = self.snapshots()[0]
        escaped = self.base / "absolute-escaped.txt"
        self.rewrite_manifest_path(manifest, "safe.txt", escaped.as_posix())

        self.lapse(
            "restore",
            "last",
            "--to",
            self.base / "absolute-export",
            "--force",
            ok=False,
        )
        self.assertFalse(escaped.exists())

    @unittest.skipIf(os.name == "nt", "creating symlinks needs extra Windows rights")
    def test_restore_refuses_symlink_ancestors(self):
        nested = self.repo / "nested"
        nested.mkdir()
        (nested / "safe.txt").write_text("safe\n", encoding="utf-8")
        self.lapse("snap")

        export = self.base / "symlink-export"
        outside = self.base / "outside"
        export.mkdir()
        outside.mkdir()
        (export / "nested").symlink_to(outside, target_is_directory=True)

        self.lapse("restore", "last", "--to", export, "--force", ok=False)
        self.assertFalse((outside / "safe.txt").exists())

    @unittest.skipIf(os.name == "nt", "named pipes are POSIX-specific")
    def test_restore_refuses_non_regular_targets(self):
        tracked = self.repo / "special-target"
        tracked.write_bytes(b"")
        self.lapse("snap")
        tracked.unlink()
        os.mkfifo(tracked)

        self.lapse("restore", "last", "special-target", "--force", ok=False)
        self.assertTrue(stat.S_ISFIFO(tracked.stat().st_mode))

    def test_restore_refuses_repository_data_as_destination(self):
        (self.repo / "safe.txt").write_text("safe\n", encoding="utf-8")
        self.lapse("snap")
        before = self.tree_bytes(self.repo / ".lapse")

        self.lapse(
            "restore",
            "last",
            "--to",
            self.repo / ".lapse",
            "--force",
            ok=False,
        )
        self.assertEqual(self.tree_bytes(self.repo / ".lapse"), before)

    def test_restore_refuses_case_aliases_of_repository_data(self):
        (self.repo / "safe.txt").write_text("safe\n", encoding="utf-8")
        self.lapse("snap")
        alias = self.repo / ".LAPSE"
        if not alias.exists():
            self.skipTest("test filesystem is case-sensitive")

        before = self.tree_bytes(self.repo / ".lapse")
        self.lapse(
            "restore",
            "last",
            "--to",
            alias,
            "--force",
            ok=False,
        )
        self.assertEqual(self.tree_bytes(self.repo / ".lapse"), before)

        manifest = self.rewrite_manifest_path(
            self.snapshots()[0], "safe.txt", ".LAPSE/poison.txt"
        )
        before = self.tree_bytes(self.repo / ".lapse")
        self.lapse("restore", self.snapshot_id(manifest), "--force", ok=False)
        self.assertEqual(self.tree_bytes(self.repo / ".lapse"), before)

    def test_restore_rejects_file_directory_destination_conflicts(self):
        (self.repo / "first.txt").write_text("first\n", encoding="utf-8")
        (self.repo / "second.txt").write_text("second\n", encoding="utf-8")
        self.lapse("snap")
        manifest = self.rewrite_manifest_path(
            self.snapshots()[0], "first.txt", "conflict"
        )
        self.rewrite_manifest_path(manifest, "second.txt", "conflict/child.txt")

        export = self.base / "conflict-export"
        self.lapse("restore", "last", "--to", export, "--force", ok=False)
        self.assertFalse((export / "conflict").exists())

    def test_restore_checks_destination_filename_equivalence(self):
        pairs = [
            ("Case-Alias.txt", "case-alias.txt"),
            ("Å-alias.txt", "å-alias.txt"),
            ("café-alias.txt", "cafe\u0301-alias.txt"),
        ]
        alias_pair = None
        for first, second in pairs:
            probe = self.base / first
            probe.write_bytes(b"probe")
            try:
                if (self.base / second).exists():
                    alias_pair = (first, second)
                    break
            finally:
                probe.unlink()
        if alias_pair is None:
            self.skipTest("test filesystem preserves all candidate spellings")

        (self.repo / "first.txt").write_text("first\n", encoding="utf-8")
        (self.repo / "second.txt").write_text("second\n", encoding="utf-8")
        self.lapse("snap")
        manifest = self.rewrite_manifest_path(
            self.snapshots()[0], "first.txt", alias_pair[0]
        )
        self.rewrite_manifest_path(manifest, "second.txt", alias_pair[1])

        one_export = self.base / "one-alias-export"
        self.lapse("restore", "last", alias_pair[0], "--to", one_export)
        self.assertEqual(
            (one_export / alias_pair[0]).read_text(encoding="utf-8"),
            "first\n",
        )

        all_export = self.base / "all-alias-export"
        self.lapse("restore", "last", "--to", all_export, "--force", ok=False)
        self.assertFalse((all_export / alias_pair[0]).exists())
        self.assertFalse((all_export / alias_pair[1]).exists())

    @unittest.skipIf(os.name == "nt", "POSIX permission bits are not portable")
    def test_repository_storage_is_private(self):
        (self.repo / "private.txt").write_text("secret\n", encoding="utf-8")
        self.lapse("snap")

        lapse_dir = self.repo / ".lapse"
        manifest = self.snapshots()[0]
        entry = self.snapshot_entries(manifest)[0]
        shard = lapse_dir / "objects" / entry["hash"][:2]
        object_file = shard / entry["hash"][2:]

        self.assertEqual(stat.S_IMODE(lapse_dir.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(shard.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(manifest.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(object_file.stat().st_mode), 0o400)

    @unittest.skipIf(os.name == "nt", "POSIX permission bits are not portable")
    def test_restore_tracks_metadata_changes(self):
        tracked = self.repo / "metadata.txt"
        tracked.write_text("same content\n", encoding="utf-8")
        tracked.chmod(0o640)
        self.lapse("snap")
        original_mtime = tracked.stat().st_mtime_ns

        tracked.chmod(0o600)
        os.utime(tracked, ns=(tracked.stat().st_atime_ns, original_mtime + 1_000_000))
        self.assertIn("M  metadata.txt", self.lapse("status").stdout)

        self.lapse("restore", "last", "metadata.txt", ok=False)
        self.assertEqual(stat.S_IMODE(tracked.stat().st_mode), 0o600)

        self.lapse("restore", "last", "metadata.txt", "--force")
        self.assertEqual(stat.S_IMODE(tracked.stat().st_mode), 0o640)
        self.assertEqual(tracked.stat().st_mtime_ns, original_mtime)

    @unittest.skipUnless(os.name == "nt", "Windows read-only attribute test")
    def test_restore_tracks_windows_read_only_attribute(self):
        tracked = self.repo / "readonly.txt"
        tracked.write_text("same content\n", encoding="utf-8")
        tracked.chmod(stat.S_IREAD)
        self.lapse("snap")

        tracked.chmod(stat.S_IREAD | stat.S_IWRITE)
        self.lapse("restore", "last", "readonly.txt", ok=False)
        self.assertTrue(tracked.stat().st_mode & stat.S_IWRITE)

        self.lapse("restore", "last", "readonly.txt", "--force")
        self.assertFalse(tracked.stat().st_mode & stat.S_IWRITE)

        export = self.base / "readonly-export"
        self.lapse("restore", "last", "readonly.txt", "--to", export)
        self.assertFalse(
            (export / "readonly.txt").stat().st_mode & stat.S_IWRITE
        )

    @unittest.skipUnless(os.name == "nt", "Windows read-only attribute test")
    def test_force_restore_replaces_windows_read_only_target(self):
        tracked = self.repo / "readonly-target.txt"
        tracked.write_text("snapshot content\n", encoding="utf-8")
        self.lapse("snap")

        tracked.write_text("newer content\n", encoding="utf-8")
        tracked.chmod(stat.S_IREAD)
        self.lapse("restore", "last", "readonly-target.txt", "--force")
        self.assertEqual(
            tracked.read_text(encoding="utf-8"), "snapshot content\n"
        )
        self.assertTrue(tracked.stat().st_mode & stat.S_IWRITE)

    def test_sha256_known_answer(self):
        # FIPS 180-4's SHA-256 digest for the three bytes "abc".
        expected = (
            "ba7816bf8f01cfea414140de5dae2223"
            "b00361a396177a9cb410ff61f20015ad"
        )
        (self.repo / "vector.bin").write_bytes(b"abc")
        self.lapse("snap")

        entries = self.snapshot_entries(self.snapshots()[0])
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]["hash"], expected)
        self.assertEqual(entries[0]["size"], 3)
        self.assertEqual(entries[0]["path"], "vector.bin")
        self.assertEqual(
            (
                self.repo
                / ".lapse"
                / "objects"
                / expected[:2]
                / expected[2:]
            ).read_bytes(),
            b"abc",
        )

    def test_cat_preserves_binary_bytes(self):
        payload = b"\x00first line\n\x1a\xff\r\nlast line\n"
        (self.repo / "binary.dat").write_bytes(payload)
        self.lapse("snap")
        result = self.lapse_bytes("cat", "last", "binary.dat")
        self.assertEqual(result.stdout, payload)

    def test_non_ascii_filename_round_trip(self):
        name = "café-雪.txt"
        (self.repo / name).write_text("unicode path\n", encoding="utf-8")
        self.lapse("snap")

        export = self.base / "unicode-export"
        self.lapse("restore", "last", "--to", export)
        self.assertEqual(
            (export / name).read_text(encoding="utf-8"), "unicode path\n"
        )

    def test_snapshot_messages_cannot_inject_terminal_controls(self):
        (self.repo / "safe.txt").write_text("safe\n", encoding="utf-8")
        message = "before\x1b]0;spoofed\x07after"
        snap = self.lapse("snap", "-m", message)
        log = self.lapse("log")
        show = self.lapse("show", "last")

        for output in (snap.stdout, log.stdout, show.stdout):
            self.assertNotIn("\x1b", output)
            self.assertNotIn("\x07", output)
        self.assertIn(r"\x1b", log.stdout)
        self.assertIn(r"\x07", show.stdout)

    @unittest.skipIf(os.name == "nt", "Windows rejects control bytes in names")
    def test_control_bytes_in_filenames_are_rejected_safely(self):
        unsafe_name = "unsafe-\x1b-file.txt"
        (self.repo / unsafe_name).write_text("unsafe\n", encoding="utf-8")
        result = self.lapse("snap", ok=False)
        self.assertNotIn("\x1b", result.stdout)
        self.assertNotIn("\x1b", result.stderr)


def main():
    global BINARY
    if len(sys.argv) < 2:
        print("usage: integration.py PATH-TO-LAPSE [unittest options]", file=sys.stderr)
        return 2

    BINARY = Path(sys.argv.pop(1)).resolve()
    if not BINARY.is_file():
        print("lapse binary not found: {0}".format(BINARY), file=sys.stderr)
        return 2

    program = unittest.main(verbosity=2, exit=False)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
