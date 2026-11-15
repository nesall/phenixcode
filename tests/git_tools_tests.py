"""Exercise native Git tools using disposable repositories; never touch user Git state."""
import hashlib
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable, git = map(lambda value: str(Path(value).resolve()), sys.argv[1:3])
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("GIT_")}
    env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT="0")
    def run(directory, *args):
        return subprocess.run([git, "-C", str(directory), *args], env=env, check=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20,
                              creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    with tempfile.TemporaryDirectory(prefix="phenixcode-git-tools-") as temporary:
        root = Path(temporary)
        repo = root / "repo space"
        repo.mkdir()
        def init(directory):
            run(directory, "init", "-b", "main")
            run(directory, "config", "user.name", "Fixture")
            run(directory, "config", "user.email", "fixture@example.invalid")
            run(directory, "config", "core.autocrlf", "input")
            run(directory, "config", "commit.gpgsign", "false")
        init(repo)
        (repo / "src").mkdir()
        main_file = repo / "src/main.cpp"
        main_file.write_text("int original_content;\n", encoding="utf-8")
        (repo / "src/odd [x];$(echo).cpp").write_text("int literal_filename;\n", encoding="utf-8")
        (repo / "src/big.cpp").write_text('// "escaped" \u03b1\n' * 5000, encoding="utf-8")
        (repo / "src/caf\u00e9.cpp").write_text("int unicode_filename;\n", encoding="utf-8")
        (repo / "src/blob.cpp").write_bytes(b"binary\0payload")
        (repo / "excluded.env").write_text("EXCLUDED_SECRET initial\n", encoding="utf-8")
        run(repo, "add", ".")
        run(repo, "commit", "-m", "initial source commit")
        run(repo, "tag", "initial-tag")
        main_file.write_text("int original_content;\nint committed_change;\n", encoding="utf-8")
        (repo / "excluded.env").write_text("EXCLUDED_SECRET second\n", encoding="utf-8")
        run(repo, "add", ".")
        run(repo, "commit", "-m", "second source commit")
        (repo / "excluded.env").write_text("EXCLUDED_SECRET final\n", encoding="utf-8")
        run(repo, "add", ".")
        run(repo, "commit", "-m", "excluded-only commit")
        run(repo, "checkout", "-b", "base-branch", "initial-tag")
        main_file.write_text("int original_content;\nint base_only_change;\n", encoding="utf-8")
        run(repo, "add", "src/main.cpp")
        run(repo, "commit", "-m", "base branch change")
        run(repo, "checkout", "main")
        orphan = run(repo, "commit-tree", "HEAD^{tree}", "-m", "unrelated root").stdout.decode().strip()
        run(repo, "update-ref", "refs/heads/unrelated", orphan)
        run(repo, "worktree", "add", "--detach", str(root / "worktree space"), "HEAD")
        main_file.write_text(main_file.read_text(encoding="utf-8") + "int staged_change;\n", encoding="utf-8")
        run(repo, "add", "src/main.cpp")
        main_file.write_text(main_file.read_text(encoding="utf-8") + "int unstaged_change;\n", encoding="utf-8")
        (repo / "src/odd [x];$(echo).cpp").write_text("int literal_filename_change;\n", encoding="utf-8")
        (repo / "excluded.env").write_text("EXCLUDED_SECRET worktree\n", encoding="utf-8")
        for number in range(60):
            (repo / f"src/untracked_{number}.cpp").write_text("int new_file;\n", encoding="utf-8")
        if os.name != "nt":
            (repo / "src/colon:name.cpp").write_text("int colon_filename;\n", encoding="utf-8")
            run(repo, "add", "src/colon:name.cpp")
            run(repo, "commit", "-m", "colon filename fixture")
            # Avoid changing the main fixture's ancestry assertions.
            run(repo, "tag", "colon-tag")
            run(repo, "reset", "--soft", "HEAD~1")
            run(repo, "reset", "src/colon:name.cpp")
        (repo / "src/new.cpp").write_text("int untracked_content;\n", encoding="utf-8")
        (repo / ".gitattributes").write_text("src/main.cpp diff=forbidden filter=forbidden\n", encoding="utf-8")
        run(repo, "config", "diff.external", "THIS_COMMAND_MUST_NOT_RUN")
        run(repo, "config", "diff.forbidden.textconv", "THIS_COMMAND_MUST_NOT_RUN")
        run(repo, "config", "filter.forbidden.clean", "THIS_COMMAND_MUST_NOT_RUN")
        run(repo, "config", "filter.forbidden.process", "THIS_COMMAND_MUST_NOT_RUN")
        run(repo, "config", "filter.forbidden.required", "true")
        run(repo, "config", "core.fsmonitor", "THIS_COMMAND_MUST_NOT_RUN")
        other = root / "other"
        other.mkdir()
        init(other)
        (other / "other.cpp").write_text("OTHER_REPO_SECRET\n", encoding="utf-8")
        run(other, "add", ".")
        run(other, "commit", "-m", "OTHER_REPO_SECRET")
        (root / "no git").mkdir()
        (root / "no git/plain.cpp").write_text("int outside;\n", encoding="utf-8")
        (root / "empty").mkdir()
        init(root / "empty")
        (root / "empty/unborn.cpp").write_text("int unborn_content;\n", encoding="utf-8")
        run(root / "empty", "add", ".")
        filters = root / "many-filters"
        filters.mkdir()
        init(filters)
        (filters / "source.cpp").write_text("int filter_fixture;\n", encoding="utf-8")
        run(filters, "add", ".")
        run(filters, "commit", "-m", "filter fixture")
        for number in range(33):
            run(filters, "config", f"filter.driver{number}.clean", "THIS_COMMAND_MUST_NOT_RUN")
        fake_bin = root / "fake-bin"
        fake_bin.mkdir()
        shutil.copyfile(executable, fake_bin / ("git.exe" if os.name == "nt" else "git"))
        if os.name != "nt":
            (fake_bin / "git").chmod(0o755)
        def snapshot():
            return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in root.rglob("*") if path.is_file()}
        before = snapshot()
        # Deliberately wrong inherited Git settings must not redirect child commands.
        child_env = dict(env, GIT_DIR=str(other / ".git"), GIT_WORK_TREE=str(other),
                         GIT_INDEX_FILE=str(root / "must-not-create-index"),
                         GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="alias.show", GIT_CONFIG_VALUE_0="!THIS_COMMAND_MUST_NOT_RUN")
        tested = subprocess.run([executable, str(root)], env=child_env, timeout=45, capture_output=True,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        print(tested.stdout.decode("utf-8", errors="replace"), end="")
        if tested.returncode:
            raise AssertionError(tested.stderr.decode("utf-8", errors="replace"))
        missing_env = dict(child_env, PATH=str(Path(sys.executable).parent) if os.name == "nt" else "")
        for mode, process_env in (("--expect-missing", missing_env),
                                  ("--expect-timeout", dict(child_env, PATH=str(fake_bin) + os.pathsep + child_env.get("PATH", ""))),
                                  ("--expect-main-timeout", dict(child_env, PATH=str(fake_bin) + os.pathsep + child_env.get("PATH", ""), PHENIXCODE_GIT_HELPER="slow-preparation"))):
            tested = subprocess.run([executable, str(root), mode], env=process_env, timeout=20, capture_output=True,
                                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            if tested.returncode:
                raise AssertionError(tested.stderr.decode("utf-8", errors="replace"))
            print(tested.stdout.decode("utf-8", errors="replace"), end="")
        if snapshot() != before:
            raise AssertionError("Read-only Git tools changed fixture files or repository metadata")
        print("Git fixture contents, indexes and repository metadata remained unchanged.")


if __name__ == "__main__":
    main()
