"""Decompile functions of the Dark Souls Remastered exe to C with Ghidra.

  python tools/decomp.py 2BC650 80C400 ...     addresses in hex, exe-relative or absolute
  python tools/decomp.py --refs NAME ...       functions that refer to symbols whose name contains NAME
                                               (e.g. an RTTI class name); prints build/decomp/refs.txt

Writes build/decomp/<exe-relative address>.c for the function containing each address, with a list of
its callers on top. Needs the one-time analysis to have finished (TOOLS/ghidra_analyze_dsr.cmd, which
creates the Ghidra project TOOLS/ghidra_projects/dsr); each run takes about a minute to open the project.

Ghidra and Java live outside the repository, in TOOLS (default C:/Users/<you>/tools, or the
ULTRASOULS_TOOLS environment variable).
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "build", "decomp"))
TOOLS = os.environ.get("ULTRASOULS_TOOLS", os.path.expanduser(r"~\tools"))
PROGRAM = "DarkSoulsRemastered.exe"


def _one(pattern):
    found = sorted(glob.glob(os.path.join(TOOLS, pattern)))
    if not found:
        sys.exit("not found: " + os.path.join(TOOLS, pattern))
    return found[-1]


def run_script(script, script_args):
    env = dict(os.environ, JAVA_HOME=_one("jdk-21*"), GHIDRA_HEADLESS_MAXMEM="6G")
    env["PATH"] = os.path.join(env["JAVA_HOME"], "bin") + os.pathsep + env["PATH"]
    cmd = [os.path.join(_one("ghidra_*_PUBLIC"), "support", "analyzeHeadless.bat"),
           os.path.join(TOOLS, "ghidra_projects"), "dsr", "-process", PROGRAM, "-noanalysis", "-readOnly",
           "-scriptPath", os.path.join(HERE, "ghidra"), "-postScript", script] + list(script_args)
    r = subprocess.run(cmd, env=env, capture_output=True, text=True, errors="replace")
    lines = [l for l in r.stdout.splitlines() if script + ">" in l or "ERROR" in l]
    print("\n".join(lines) if lines else r.stdout[-2000:] + r.stderr[-2000:])


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if sys.argv[1] == "--refs":
        refs = os.path.join(OUT, "refs.txt")
        run_script("FindRefs.java", [refs] + sys.argv[2:])
        print(open(refs, encoding="utf-8").read())
    else:
        run_script("Decomp.java", [OUT] + sys.argv[1:])
