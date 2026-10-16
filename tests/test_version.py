"""The build-injected version string (cmake/Version.cmake).

The number (major.minor.patch) comes from project() in CMakeLists.txt; the script adds the
channel from git: nothing for a build sitting exactly on a v* tag with a clean tree, the short
commit otherwise, and a LOCAL mark when the tree is dirty or there is no git at all.

Driven as the build drives it, with cmake -P over throwaway repositories:
  python3 tests/test_version.py            (CMAKE=<path to cmake> if it is not on PATH)
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "cmake", "Version.cmake")
CMAKE = os.environ.get("CMAKE", "cmake")

failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def git(repo, *args):
    subprocess.run(("git",) + args, cwd=repo, check=True, capture_output=True)


def repo(dirty=False, tags=(), extra_commits=0):
    """A throwaway repository: one commit, then `tags`, then `extra_commits` more on top."""
    d = tempfile.mkdtemp()
    git(d, "init", "-q", "-b", "main")
    git(d, "config", "user.email", "t@example.com")
    git(d, "config", "user.name", "T")
    open(os.path.join(d, "a.txt"), "w").write("one\n")
    git(d, "add", "a.txt")
    git(d, "commit", "-qm", "one")
    for t in tags:
        git(d, "tag", t)
    for i in range(extra_commits):
        open(os.path.join(d, "a.txt"), "a").write(f"more {i}\n")
        git(d, "commit", "-qam", f"more {i}")
    if dirty:
        open(os.path.join(d, "a.txt"), "a").write("uncommitted\n")
    return d


def derive(src, number="1.0.0"):
    """Run the script the way the build does and return the version string it reports."""
    out = subprocess.run([CMAKE, "-DSQ8L_NUMBER=" + number, "-DSQ8L_SRC=" + src, "-P", SCRIPT],
                         capture_output=True, text=True)
    if out.returncode != 0:
        return f"<cmake failed: {out.stderr.strip()}>"
    for line in (out.stdout + out.stderr).splitlines():
        line = line.strip().lstrip("- ").strip()
        if line.startswith("SQ8L_VERSION="):
            return line[len("SQ8L_VERSION="):]
    return "<not reported>"


def short(src):
    return subprocess.run(("git", "rev-parse", "--short", "HEAD"), cwd=src, check=True,
                          capture_output=True, text=True).stdout.strip()


def main():
    if not os.path.exists(SCRIPT):
        print(f"FAILED: {SCRIPT} does not exist")
        return 1

    made = []
    try:
        print("a clean tree exactly on a v* tag is a release: the number alone")
        d = repo(tags=("v1.0.0",))
        made.append(d)
        check(derive(d) == "1.0.0", f"-> {derive(d)!r}")

        print("commits past the tag make it a nightly: the number and the commit in parentheses")
        d = repo(tags=("v1.0.0",), extra_commits=2)
        made.append(d)
        check(derive(d) == f"1.0.0 ({short(d)})", f"-> {derive(d)!r}")

        print("the rolling nightly tag must not read as a release (it captures git describe)")
        d = repo(tags=("v1.0.0",), extra_commits=2)
        made.append(d)
        git(d, "tag", "nightly")
        check(derive(d) == f"1.0.0 ({short(d)})", f"-> {derive(d)!r}")

        print("a nightly tag sitting on the same commit as a v* tag is still a release")
        d = repo(tags=("v1.0.0", "nightly"))
        made.append(d)
        check(derive(d) == "1.0.0", f"-> {derive(d)!r}")

        print("an uncommitted change marks the build LOCAL")
        d = repo(dirty=True, tags=("v1.0.0",), extra_commits=1)
        made.append(d)
        check(derive(d) == f"1.0.0 ({short(d)}) LOCAL", f"-> {derive(d)!r}")

        print("a dirty tree on the tag itself is LOCAL too, not a release")
        d = repo(dirty=True, tags=("v1.0.0",))
        made.append(d)
        check(derive(d) == f"1.0.0 ({short(d)}) LOCAL", f"-> {derive(d)!r}")

        print("a repository with no v* tag at all is a nightly, not a release")
        d = repo(extra_commits=1)
        made.append(d)
        check(derive(d) == f"1.0.0 ({short(d)})", f"-> {derive(d)!r}")

        print("no git at all (an unpacked source archive): the number alone")
        d = tempfile.mkdtemp()
        made.append(d)
        check(derive(d) == "1.0.0", f"-> {derive(d)!r}")

        print("the number is whatever project() says, not baked into the script")
        d = repo(tags=("v2.5.9",))
        made.append(d)
        check(derive(d, "2.5.9") == "2.5.9", f"-> {derive(d, '2.5.9')!r}")
    finally:
        for d in made:
            shutil.rmtree(d, ignore_errors=True)

    print(f"{'FAILED' if failures else 'OK'}: {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
