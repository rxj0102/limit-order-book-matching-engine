#!/usr/bin/env python3
"""Manual mutation sanity check for the differential test.

Not part of CI and not a full mutation analysis: a fixed list of hand written
bugs (mutants.py), each applied to include/lobkit/book/book.hpp in turn, built
into the test binary, and run against the differential test. Prints whether
each mutant was killed (test failed or the binary crashed) or survived, and
compares that with what the mutant list expects.

    python3 -I tools/mutation/run.py --build-dir build/release [--ops 200000] [--only NAME]
    python3 -I tools/mutation/run.py --check        # only verify that fragments still match

The source file is restored in a finally block, but run it on a clean working
tree anyway. Never run two builds in the same tree while it is running.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mutants import FILE, MUTANTS  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default="build/release")
    ap.add_argument("--ops", default="200000", help="operations per differential scenario")
    ap.add_argument("--only")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    root = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip()
    path = os.path.join(root, FILE)
    original = open(path).read()

    bad = 0
    for m in MUTANTS:
        if original.count(m["old"]) != 1:
            print(f"{m['name']}: fragment matches {original.count(m['old'])} times (need exactly 1)")
            bad += 1
    if args.check or bad:
        sys.exit(1 if bad else 0)

    backup = tempfile.NamedTemporaryFile(delete=False)
    backup.close()
    shutil.copy(path, backup.name)
    mismatches = 0
    try:
        for m in MUTANTS:
            if args.only and m["name"] != args.only:
                continue
            with open(path, "w") as f:
                f.write(original.replace(m["old"], m["new"], 1))
            build = subprocess.run(["cmake", "--build", args.build_dir, "--target", "lobkit_tests"],
                                   cwd=root, capture_output=True, text=True)
            if build.returncode != 0:
                print(f"{m['name']}: mutant does not compile\n{build.stderr[-400:]}")
                mismatches += 1
                continue
            env = dict(os.environ, LOBKIT_DIFF_OPS=str(args.ops))
            run = subprocess.run([os.path.join(args.build_dir, "tests", "lobkit_tests"),
                                  "--gtest_filter=*Differential*"],
                                 cwd=root, env=env, capture_output=True, text=True)
            outcome = "killed" if run.returncode != 0 else "survived"
            expected = "killed" if m["expect"] == "killed" else "survived"
            flag = "" if outcome == expected else "   <-- UNEXPECTED"
            if flag:
                mismatches += 1
            note = f" (rc={run.returncode})" if run.returncode not in (0, 1) else ""
            print(f"{m['name']}: {outcome}{note}, expected {expected}{flag}")
    finally:
        shutil.copy(backup.name, path)
        os.unlink(backup.name)
        subprocess.run(["cmake", "--build", args.build_dir, "--target", "lobkit_tests"],
                       cwd=root, capture_output=True)
    sys.exit(1 if mismatches else 0)


if __name__ == "__main__":
    main()
