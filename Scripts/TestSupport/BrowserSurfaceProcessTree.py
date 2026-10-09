#!/usr/bin/env python3
"""Process-tree oracles for Scripts/TestBrowserSurface.sh (Linux /proc only).

  descendants PID          print "pid starttime" for every live descendant of PID
  alive FILE               print the entries of a descendants snapshot that are still alive
  profile-processes PATH   print pids whose command line carries --user-data-dir under PATH
  cpu PID SECONDS          sample the CPU of PID plus all descendants over SECONDS

A pid is only "the same process" when its start time also matches, so pid reuse
cannot hide an orphan or fake a survivor. Zombies count as dead.
"""
import os
import sys
import time

CLOCK_TICKS = os.sysconf("SC_CLK_TCK")


def read_stat(pid):
    try:
        with open("/proc/%d/stat" % pid) as stat_file:
            raw = stat_file.read()
    except OSError:
        return None
    # The command name may contain spaces and parentheses; fields follow the last ')'.
    fields = raw[raw.rindex(")") + 2:].split()
    return {
        "state": fields[0],
        "ppid": int(fields[1]),
        "utime": int(fields[11]),
        "stime": int(fields[12]),
        "starttime": int(fields[19]),
    }


def all_pids():
    return [int(name) for name in os.listdir("/proc") if name.isdigit()]


def descendants(root):
    children = {}
    stats = {}
    for pid in all_pids():
        stat = read_stat(pid)
        if stat is None:
            continue
        stats[pid] = stat
        children.setdefault(stat["ppid"], []).append(pid)
    found = []
    pending = list(children.get(root, []))
    while pending:
        pid = pending.pop()
        found.append(pid)
        pending.extend(children.get(pid, []))
    return [(pid, stats[pid]) for pid in sorted(found) if stats[pid]["state"] != "Z"]


def command(args):
    if len(args) == 2 and args[0] == "descendants":
        for pid, stat in descendants(int(args[1])):
            print(pid, stat["starttime"])
    elif len(args) == 2 and args[0] == "alive":
        with open(args[1]) as snapshot:
            for line in snapshot:
                pid, starttime = (int(value) for value in line.split())
                stat = read_stat(pid)
                if stat is not None and stat["state"] != "Z" and stat["starttime"] == starttime:
                    print(pid, starttime)
    elif len(args) == 2 and args[0] == "profile-processes":
        needle = "--user-data-dir=" + args[1]
        for pid in all_pids():
            try:
                with open("/proc/%d/cmdline" % pid, "rb") as cmdline:
                    text = cmdline.read().decode("utf-8", "replace")
            except OSError:
                continue
            stat = read_stat(pid)
            if needle in text and stat is not None and stat["state"] != "Z":
                print(pid)
    elif len(args) == 3 and args[0] == "cpu":
        root = int(args[1])
        window = float(args[2])

        def ticks():
            total = {}
            root_stat = read_stat(root)
            if root_stat is not None:
                total[(root, root_stat["starttime"])] = root_stat["utime"] + root_stat["stime"]
            for pid, stat in descendants(root):
                total[(pid, stat["starttime"])] = stat["utime"] + stat["stime"]
            return total

        before = ticks()
        started = time.monotonic()
        time.sleep(window)
        after = ticks()
        elapsed = time.monotonic() - started
        # A process that appeared during the window contributes all of its ticks; one
        # that exited contributes nothing, which can only understate, so the script
        # also requires the tree to be stable.
        consumed = sum(value - before.get(key, 0) for key, value in after.items())
        print("percent=%.3f processes=%d stable=%d" % (
            100.0 * consumed / CLOCK_TICKS / elapsed, len(after), 1 if set(before) == set(after) else 0))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    command(sys.argv[1:])
