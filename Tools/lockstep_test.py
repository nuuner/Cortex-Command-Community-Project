#!/usr/bin/env python3
"""Checks that two instances of the game stay in sync when playing in lockstep over the network.

A host and a joiner are run on this machine, each with a random player mashing their keyboard and waving their mouse about.
Each of them records a trace of determinism checkpoints, and the two traces have to come out the same. The game also compares state hashes between the two as it goes.

Run this from the game's working directory. For example:
    python Tools/lockstep_test.py
    python Tools/lockstep_test.py --activity GAScripted "Brain vs Brain" "Ketanot Hills" --updates 3000 --windowed
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from determinism_test import write_random_input, write_test_settings


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", default="Cortex Command.exe", help="the game executable to test (default: %(default)s)")
    parser.add_argument("--activity", nargs=3, default=["GAScripted", "Brain vs Brain", "Ketanot Hills"], metavar=("TYPE", "ACTIVITY", "SCENE"), help="the Activity and Scene to play (default: %(default)s)")
    parser.add_argument("--updates", type=int, default=1500, help="how many sim updates to play for (default: %(default)s)")
    parser.add_argument("--input-seeds", type=int, nargs=2, default=[1, 2], metavar=("HOST", "JOINER"), help="the seeds to make each player's random input from (default: %(default)s)")
    parser.add_argument("--port", type=int, default=47531, help="the port to play over (default: %(default)s)")
    parser.add_argument("--windowed", action="store_true", help="run both with the game window, audio and real time pacing, instead of headless")
    parser.add_argument("--trace-dir", type=Path, help="where to keep the inputs and traces, instead of a temporary directory")
    parser.add_argument("--timeout", type=int, default=600, help="how many seconds the two can take before they are given up on (default: %(default)s)")
    options = parser.parse_args()
    exe = str(Path(options.exe).resolve())

    with tempfile.TemporaryDirectory() as temp_dir:
        trace_dir = (options.trace_dir or Path(temp_dir)).resolve()
        trace_dir.mkdir(parents=True, exist_ok=True)

        processes, traces = [], []
        for name, seed, net_args in zip(("host", "joiner"), options.input_seeds, (["-net-host", str(options.port)], ["-net-join", f"127.0.0.1:{options.port}"])):
            player_input, trace, settings = trace_dir / f"{name}.input", trace_dir / f"{name}.trace", trace_dir / f"{name}.Settings.ini"
            write_random_input(player_input, seed, options.updates)
            write_test_settings(settings)
            traces.append(trace)
            args = [exe, "-activity", *options.activity, "-sim-updates", str(options.updates), "-input-replay", str(player_input), "-determinism-record", str(trace), *net_args]
            if not options.windowed:
                args.append("-headless")
            processes.append(subprocess.Popen(args, stderr=subprocess.PIPE, text=True, env={**os.environ, "CCCP_SETTINGSPATH": str(settings)}))

        failed = False
        for name, process in zip(("host", "joiner"), processes):
            try:
                report = process.communicate(timeout=options.timeout)[1]
            except subprocess.TimeoutExpired:
                process.kill()
                report = process.communicate()[1] + f"\nIt was still going after {options.timeout} seconds."
            report = "\n".join(line for line in report.splitlines() if "Lockstep" in line or "break" in line or "seconds" in line)
            print(f"{name}: exit code {process.returncode}\n{report}")
            failed = failed or process.returncode != 0

        host_trace, joiner_trace = (trace.read_text().splitlines() if trace.exists() else [] for trace in traces)
        if not host_trace or host_trace != joiner_trace:
            failed = True
            difference = next((line for line, (host, joiner) in enumerate(zip(host_trace, joiner_trace)) if host != joiner), min(len(host_trace), len(joiner_trace)))
            print(f"The traces differ from checkpoint {difference + 1}, of {len(host_trace)} for the host and {len(joiner_trace)} for the joiner.")
            for name, trace in (("host", host_trace), ("joiner", joiner_trace)):
                print(f"  {name}: {trace[difference] if difference < len(trace) else 'nothing'}")
        else:
            print(f"The traces match, all {len(host_trace)} checkpoints.")

    print("FAILED" if failed else "The two stayed in sync.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
