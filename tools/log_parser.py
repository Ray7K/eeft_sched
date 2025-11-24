# pyright: basic
# eeft_sched log parser (per-tick state-based)
# Usage: place logs as target/logs/log_p{proc}.txt and run this script.
# Requires: pandas, matplotlib

import glob
import os
import re
from collections import defaultdict

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

LOG_DIR = "target/logs/"
REPORTS_DIR = "target/reports/"
ROLLING_WINDOW = 10  # ticks for smoothing

# --- Regex patterns for log parsing ---
RE_STATUS_IDLE = re.compile(r"^\[(\d+)\].*\[P(\d+):\s*C(\d+)\].*Status:\s*IDLE")
RE_STATUS_RUN = re.compile(
    r"^\[(\d+)\].*\[P(\d+):\s*C(\d+)\].*Status:\s*RUNNING\s*->\s*Job\s+(\d+)"
)
RE_LOW_POWER = re.compile(r"^\[(\d+)\].*\[P(\d+):\s*C(\d+)\].*low power state")
RE_SCALING = re.compile(
    r"^\[(\d+)\].*\[P(\d+):\s*C(\d+)\].*DVFS Level:\s*(\d+),\s*Frequency Scaling:\s*([\d.]+)"
)
RE_MODE_CHANGE = re.compile(r"^\[(\d+)\].*\[P(\d+):\s*C(\d+)\].*Mode Change to\s+(\d+)")


RE_DISCARDED_JOB = re.compile(
    r"\[(\d+)\].*\[P(\d+):\s*C(\d+)\].*Accommodating discarded job (\d+) \(Original Core ID: (\d+)\)"
)
RE_AWARD_JOB = re.compile(
    r"\[(\d+)\].*\[P(\d+):\s*C(\d+)\].*Awarding Job (\d+) \(Arrival: (\d+)\) to Core (\d+)"
)
RE_AWARD_FUTURE_JOB = re.compile(
    r"\[(\d+)\].*\[P(\d+):\s*C(\d+)\].*Awarding future Job (\d+) \(Arrival: (\d+) WCET: ([\d.]+)\) to Core (\d+)"
)
RE_PUSH_MIGRATION = re.compile(
    r"\[(\d+)\].*\[P(\d+):\s*C(\d+)\].*Awarded Job (\d+) is a future job arriving at (\d+) with wcet ([\d.]+)"
)
RE_RECEIVED_MIGRATION = re.compile(
    r"^\[(\d+)\].*\[P(\d+):\s*C(\d+)\].*Received\s+award\s+notification\s+for\s+Job\s+(\d+)"
)
RE_DISPATCH = re.compile(r"\[(\d+)\].*\[P(\d+):\s*C(\d+)\].*Dispatching Job (\d+)")
RE_PREEMPT = r"\[(\d+)\].*\[P(\d+):\s*C(\d+)\].*Preempting Job (\d+)"
RE_COMPLETE = r"\[(\d+)\].*Job\s+(\d+)\s+completed"


def find_log_files():
    return sorted(glob.glob(os.path.join(LOG_DIR, "log_p*.txt")))


def parse_all_logs():
    """
    Parses logs and returns:
    - states: list of {time, proc, core, state, job(optional)}
    - scaling_records
    - mode_changes
    """
    states = []
    scaling_records = []
    mode_changes = []

    for path in find_log_files():
        with open(path, "r") as f:
            for raw in f:
                line = raw.strip()
                if not line:
                    continue

                # RUNNING
                if m := RE_STATUS_RUN.match(line):
                    t, proc, core, job = m.groups()
                    states.append(
                        {
                            "time": int(t),
                            "proc": int(proc),
                            "core": int(core),
                            "state": "BUSY",
                            "job": int(job),
                        }
                    )
                    continue

                # IDLE
                if m := RE_STATUS_IDLE.match(line):
                    t, proc, core = m.groups()
                    states.append(
                        {
                            "time": int(t),
                            "proc": int(proc),
                            "core": int(core),
                            "state": "IDLE",
                        }
                    )
                    continue

                # LOW POWER
                if m := RE_LOW_POWER.match(line):
                    t, proc, core = m.groups()
                    states.append(
                        {
                            "time": int(t),
                            "proc": int(proc),
                            "core": int(core),
                            "state": "SLEEP",
                        }
                    )
                    continue

                # SCALING
                if m := RE_SCALING.match(line):
                    t, proc, core, dvfs, scaling = m.groups()
                    scaling_records.append(
                        {
                            "processor": int(proc),
                            "core": int(core),
                            "time": int(t),
                            "dvfs": int(dvfs),
                            "scaling": float(scaling) * 100.0,
                        }
                    )
                    continue

                # MODE CHANGE
                if m := RE_MODE_CHANGE.match(line):
                    t, proc, core, mode = m.groups()
                    mode_changes.append(
                        {
                            "processor": int(proc),
                            "core": int(core),
                            "time": int(t),
                            "mode": int(mode),
                        }
                    )
                    continue

    states = sorted(states, key=lambda x: x["time"])
    scaling_records = sorted(
        scaling_records, key=lambda x: (x["processor"], x["core"], x["time"])
    )
    mode_changes = sorted(mode_changes, key=lambda x: x["time"])

    return states, scaling_records, mode_changes


def parse_qos_migration_events():
    events = []
    for path in find_log_files():
        with open(path, "r") as f:
            for raw in f:
                line = raw.strip()
                if not line:
                    continue

                if m := RE_DISCARDED_JOB.match(line):
                    t, proc, core, job, orig = m.groups()
                    events.append(
                        dict(
                            time=int(t),
                            processor=int(proc),
                            core=int(core),
                            type="discarded",
                            job=int(job),
                            original_core=int(orig),
                        )
                    )
                    continue

                if m := RE_AWARD_JOB.match(line):
                    t, proc, core, job, arrival, target = m.groups()
                    events.append(
                        dict(
                            time=int(t),
                            processor=int(proc),
                            core=int(core),
                            type="award",
                            job=int(job),
                            arrival=int(arrival),
                            target_core=int(target),
                        )
                    )
                    continue

                if m := RE_AWARD_FUTURE_JOB.match(line):
                    t, proc, core, job, arrival, wcet, target = m.groups()
                    events.append(
                        dict(
                            time=int(t),
                            processor=int(proc),
                            core=int(core),
                            type="award_future",
                            job=int(job),
                            arrival=int(arrival),
                            wcet=float(wcet),
                            target_core=int(target),
                        )
                    )
                    continue

                if m := RE_PUSH_MIGRATION.match(line):
                    t, proc, core, job, arrival, wcet = m.groups()
                    events.append(
                        dict(
                            time=int(t),
                            processor=int(proc),
                            core=int(core),
                            type="push_migration",
                            job=int(job),
                            arrival=int(arrival),
                            wcet=float(wcet),
                        )
                    )
                    continue

                if m := RE_RECEIVED_MIGRATION.match(line):
                    t, proc, core, job = m.groups()
                    events.append(
                        dict(
                            time=int(t),
                            processor=int(proc),
                            core=int(core),
                            type="recv_migration",
                            job=int(job),
                        )
                    )
                    continue

    return pd.DataFrame(events)


def build_core_time_series(states, scaling_records):
    """Builds per-core timeline of busy/idle/sleep/scaling directly from state logs."""
    cores = set((s["proc"], s["core"]) for s in states)
    for s in scaling_records:
        cores.add((s["processor"], s["core"]))

    core_series = {}

    # Determine global time bounds
    all_times = [s["time"] for s in states]
    all_times += [s["time"] for s in scaling_records]
    if not all_times:
        return core_series
    global_min, global_max = min(all_times), max(all_times)

    # Build per-core state map
    state_map = defaultdict(list)
    for s in states:
        key = (s["proc"], s["core"])
        state_map[key].append((s["time"], s["state"]))

    scaling_map = defaultdict(dict)
    for s in scaling_records:
        scaling_map[(s["processor"], s["core"])][s["time"]] = s["scaling"]

    for core in cores:
        proc, cid = core
        idx = pd.Index(range(global_min, global_max + 1), name="time")
        df = pd.DataFrame(index=idx)
        df["busy"] = 0
        df["idle"] = 0
        df["sleep"] = 0
        df["scaling"] = float("nan")

        # Fill state transitions
        cur_state = "IDLE"
        events = sorted(state_map.get(core, []))
        for i, (t, state) in enumerate(events):
            end_t = events[i + 1][0] - 1 if i + 1 < len(events) else global_max
            if state == "BUSY":
                df.loc[t:end_t, "busy"] = 1
            elif state == "IDLE":
                df.loc[t:end_t, "idle"] = 1
            elif state == "SLEEP":
                df.loc[t:end_t, "sleep"] = 1

        # Fill scaling timeline
        core_scaling = scaling_map.get(core, {})
        for t, val in core_scaling.items():
            if global_min <= t <= global_max:
                df.at[t, "scaling"] = val
        df["scaling"] = df["scaling"].ffill().bfill()

        core_series[core] = df

    return core_series


def compute_util_and_scaling(core_series, window=ROLLING_WINDOW):
    results = {}
    for core, df in core_series.items():
        df = df.copy()
        df["time"] = df.index
        df["util_inst"] = df["busy"] * 100.0
        df["util_smooth"] = df["util_inst"].rolling(window=window, min_periods=1).mean()
        df["scaling_smooth"] = (
            df["scaling"].rolling(window=window, min_periods=1).mean()
        )

        # --- New: instantaneous and smoothed active work (% × frequency scaling) ---
        df["active_work_inst"] = df["util_inst"] * (df["scaling"] / 100.0)
        df["active_work_smooth"] = (
            df["active_work_inst"].rolling(window=window, min_periods=1).mean()
        )

        results[core] = df
    return results


def plot_core_time_breakdown(core_series):
    os.makedirs(REPORTS_DIR, exist_ok=True)
    by_proc = defaultdict(list)
    for p, c in core_series.keys():
        by_proc[p].append(c)

    for proc, cores in sorted(by_proc.items()):
        fig, axes = plt.subplots(
            1,
            len(cores),
            figsize=(4 * len(cores), 4),
            squeeze=False,
            subplot_kw=dict(aspect="equal"),
            constrained_layout=True,
        )
        for ax, cid in zip(axes[0], cores):
            df = core_series[(proc, cid)]
            total = len(df)
            t_busy, t_idle, t_sleep = (
                df["busy"].sum(),
                df["idle"].sum(),
                df["sleep"].sum(),
            )

            vals = np.array([t_busy, t_idle, t_sleep], float)
            perc = vals / vals.sum() * 100
            labels = ["Active", "Idle", "DPM"]

            # --- New: aggregate work done (DVFS-weighted busy time) ---
            work_done = (df["busy"] * df["scaling"].fillna(0)).sum() / 100.0

            # --- New: count sleep transitions and wakeups ---
            sleep_entries = (df["sleep"].diff() == 1).sum()
            wakeups = (df["sleep"].diff() == -1).sum()

            # In case the trace starts or ends mid-sleep, adjust counts
            if df["sleep"].iloc[0] == 1:
                sleep_entries += 1  # started asleep
            if df["sleep"].iloc[-1] == 1:
                wakeups += 1  # ended asleep, count as incomplete wake

            ax.pie(
                vals,
                labels=[f"{l}\n{p:.1f}%" for l, p in zip(labels, perc)],
                colors=["tab:green", "tab:gray", "tab:blue"],
                wedgeprops=dict(width=0.5, edgecolor="white"),
            )

            # Add multi-line title with work done and transition counts
            ax.set_title(
                f"P{proc}:C{cid}\n"
                f"Work: {work_done:.1f} eq. ticks | "
                f"Sleep entries: {int(sleep_entries)}, Wakeups: {int(wakeups)}",
                fontsize=8,
            )
        fig.suptitle(f"Processor {proc} — Core Time Breakdown (%)", fontsize=13)
        plt.savefig(os.path.join(REPORTS_DIR, f"time_breakdown_p{proc}.png"), dpi=150)
        plt.close(fig)


def plot_percore_utilization(results):
    os.makedirs(REPORTS_DIR, exist_ok=True)
    by_proc = defaultdict(list)
    for p, c in results.keys():
        by_proc[p].append(c)

    for proc, cores in sorted(by_proc.items()):
        fig, axes = plt.subplots(
            len(cores),
            1,
            figsize=(14, 3 * len(cores)),
            sharex=True,
            constrained_layout=True,
        )
        if len(cores) == 1:
            axes = [axes]
        for ax, cid in zip(axes, cores):
            df = results[(proc, cid)]

            # Plot utilization and active work
            ax.plot(df.index, df["util_smooth"], label="Utilization (%)", linewidth=1.5)
            ax.plot(
                df.index,
                df["active_work_smooth"],
                label="Active Work (%)",
                linewidth=1.5,
                linestyle="--",
            )

            ax.set_ylabel("Percent (%)")
            ax.set_ylim(0, 105)
            ax.set_title(f"P{proc}:C{cid}")

            # Highlight sleep intervals
            in_sleep = False
            start = None
            for t, val in df["sleep"].items():
                if val == 1 and not in_sleep:
                    in_sleep = True
                    start = t
                if val == 0 and in_sleep:
                    in_sleep = False
                    ax.axvspan(start, t, color="gray", alpha=0.2)
            if in_sleep:
                ax.axvspan(start, df.index[-1], color="gray", alpha=0.2)

            ax.grid(True, linestyle="--", alpha=0.4)
            ax.legend(loc="upper right", fontsize=8)

        axes[-1].set_xlabel("System Time (ticks)")
        plt.savefig(os.path.join(REPORTS_DIR, f"util_p{proc}_percore.png"), dpi=150)
        plt.close()


def plot_system_power(results):
    os.makedirs(REPORTS_DIR, exist_ok=True)

    DVFS = {
        100: (2000e6, 1.00),
        90: (1800e6, 0.95),
        75: (1500e6, 0.90),
        60: (1200e6, 0.85),
        50: (1000e6, 0.80),
        40: (800e6, 0.76),
    }

    LEAK_COEFF = 0.30 * (1.00**2 * 2000e6)

    SLEEP_LEAK_FACTOR = 0.01

    combined = None
    core_energy = {} 

    for core_key, df in results.items():

        df = df.copy()

        freqs = []
        volts = []
        for s in df["scaling"].fillna(0).astype(int):
            if s in DVFS:
                f_s, v_s = DVFS[s]
            else:
                f_s, v_s = (0.0, 0.0)
            freqs.append(f_s)
            volts.append(v_s)

        df["freq"] = freqs
        df["volt"] = volts

        df["activity"] = 0.0
        df.loc[df["busy"] == 1, "activity"] = 1.0
        df.loc[(df["busy"] == 0) & (df["sleep"] == 0), "activity"] = 0.1
        df.loc[df["sleep"] == 1, "activity"] = 0.0

        df["dyn_power"] = df["activity"] * (df["volt"] ** 2) * df["freq"]

        df["leak_power"] = LEAK_COEFF * df["volt"]

        df.loc[df["sleep"] == 1, "leak_power"] *= SLEEP_LEAK_FACTOR

        df["total_power"] = df["dyn_power"] + df["leak_power"]

        core_energy[core_key] = df["total_power"].sum()

        df_local = df[["time", "total_power"]].copy()
        if combined is None:
            combined = df_local
        else:
            combined["total_power"] += df_local["total_power"]

    if combined is None:
        return

    max_power = combined["total_power"].max()
    combined["norm_power"] = combined["total_power"] / max_power

    system_energy = combined["total_power"].sum()

    combined["smooth"] = (
        combined["norm_power"].rolling(window=ROLLING_WINDOW, min_periods=1).mean()
    )

    plt.figure(figsize=(12, 4), constrained_layout=True)
    plt.plot(combined["time"], combined["smooth"], linewidth=1.2)
    plt.title("System Power (Normalized V²f + Leakage)")
    plt.xlabel("System Time (ticks)")
    plt.ylabel("Normalized Power")
    plt.grid(True, linestyle="--", alpha=0.4)
    plt.savefig(os.path.join(REPORTS_DIR, "system_power_timeline.png"), dpi=150)
    plt.close()

    with open(os.path.join(REPORTS_DIR, "energy_report.txt"), "w") as f:
        f.write("=== ENERGY REPORT ===\n")
        f.write(f"Total system energy (normalized units): {system_energy:.3e}\n\n")
        f.write("--- Per-core energy (normalized units) ---\n")
        for (proc, core), e in sorted(core_energy.items()):
            f.write(f"P{proc}:C{core} = {e:.3e}\n")

    print(f"[Energy] Total system energy = {system_energy:.3e}")


def plot_mode_change_timeline(mode_changes):
    """Plot mode change events over time (system-wide)."""
    if not mode_changes:
        return

    os.makedirs(REPORTS_DIR, exist_ok=True)
    times = [m["time"] for m in mode_changes]
    modes = [m["mode"] for m in mode_changes]

    plt.figure(figsize=(12, 3), constrained_layout=True)
    plt.step(times, modes, where="post", linewidth=1.5)
    plt.scatter(times, modes, color="red", s=25, zorder=3)
    plt.title("Mode Change Timeline")
    plt.xlabel("System Time (ticks)")
    plt.ylabel("Mode ID")
    plt.grid(True, linestyle="--", alpha=0.4)
    plt.savefig(os.path.join(REPORTS_DIR, "mode_change_timeline.png"), dpi=150)
    plt.close()


def plot_sleep_duration_distribution(core_series):
    """Plot distribution of sleep interval lengths across all cores."""
    os.makedirs(REPORTS_DIR, exist_ok=True)
    sleep_durations = []

    for df in core_series.values():
        in_sleep = False
        start = None
        for t, val in df["sleep"].items():
            if val == 1 and not in_sleep:
                in_sleep = True
                start = t
            if val == 0 and in_sleep:
                in_sleep = False
                sleep_durations.append(t - start)
        # Handle case where trace ends mid-sleep
        if in_sleep and start is not None:
            sleep_durations.append(df.index[-1] - start)

    if not sleep_durations:
        return

    plt.figure(figsize=(8, 4), constrained_layout=True)
    plt.hist(sleep_durations, bins=30, color="tab:blue", alpha=0.7)
    plt.title("Distribution of Sleep Durations")
    plt.xlabel("Sleep Duration (ticks)")
    plt.ylabel("Count")
    plt.grid(True, linestyle="--", alpha=0.4)
    plt.savefig(os.path.join(REPORTS_DIR, "sleep_duration_hist.png"), dpi=150)
    plt.close()


def plot_dvfs_distribution(results):
    """Plot histogram of DVFS scaling levels per core."""
    os.makedirs(REPORTS_DIR, exist_ok=True)

    by_proc = defaultdict(list)
    for p, c in results.keys():
        by_proc[p].append(c)

    for proc, cores in sorted(by_proc.items()):
        plt.figure(figsize=(6, 4), constrained_layout=True)
        for cid in cores:
            df = results[(proc, cid)]
            plt.hist(
                df["scaling"].dropna(),
                bins=30,
                alpha=0.5,
                label=f"C{cid}",
            )
        plt.title(f"Processor {proc} — DVFS Scaling Distribution")
        plt.xlabel("Frequency Scaling (%)")
        plt.ylabel("Count")
        plt.legend(fontsize=8)
        plt.grid(True, linestyle="--", alpha=0.4)
        plt.savefig(os.path.join(REPORTS_DIR, f"dvfs_dist_p{proc}.png"), dpi=150)
        plt.close()


def plot_awards_per_core_per_proc(df):
    os.makedirs(REPORTS_DIR, exist_ok=True)
    for proc, group in df.groupby("processor"):
        subset = group[group["type"].isin(["award", "award_future"])]
        if subset.empty:
            continue
        counts = subset["core"].value_counts().sort_index()
        plt.figure(figsize=(6, 4), constrained_layout=True)
        counts.plot(kind="bar", color="tab:green")
        plt.title(f"Processor {proc} — Jobs Awarded per Core")
        plt.xlabel("Core ID")
        plt.ylabel("Count")
        plt.grid(axis="y", linestyle="--", alpha=0.4)
        plt.savefig(os.path.join(REPORTS_DIR, f"awards_p{proc}_percore.png"), dpi=150)
        plt.close()


def plot_wcet_distribution_per_core(df):
    os.makedirs(REPORTS_DIR, exist_ok=True)
    for (proc, core), group in df.groupby(["processor", "core"]):
        wcet_jobs = group[group["type"].isin(["award_future", "push_migration"])]
        if wcet_jobs.empty:
            continue
        plt.figure(figsize=(6, 3), constrained_layout=True)
        plt.hist(wcet_jobs["wcet"], bins=25, color="tab:orange", alpha=0.7)
        plt.title(f"P{proc}:C{core} — WCET Distribution")
        plt.xlabel("WCET (ticks)")
        plt.ylabel("Count")
        plt.grid(True, linestyle="--", alpha=0.4)
        plt.savefig(os.path.join(REPORTS_DIR, f"wcet_p{proc}_c{core}.png"), dpi=150)
        plt.close()


def plot_migrations_per_core(df):
    os.makedirs(REPORTS_DIR, exist_ok=True)
    for proc, group in df.groupby("processor"):
        cores = sorted(group["core"].unique())
        push_counts = group[group["type"] == "push_migration"]["core"].value_counts()
        recv_counts = group[group["type"] == "recv_migration"]["core"].value_counts()

        values = []
        labels = []
        for c in cores:
            push = push_counts.get(c, 0)
            recv = recv_counts.get(c, 0)
            labels.append(f"C{c}")
            values.append((push, recv))

        if not values:
            continue

        pushes, recvs = zip(*values)
        x = np.arange(len(cores))
        plt.figure(figsize=(7, 3), constrained_layout=True)
        plt.bar(x - 0.2, pushes, 0.4, label="Pushes", color="tab:purple")
        plt.bar(x + 0.2, recvs, 0.4, label="Received", color="tab:gray")
        plt.xticks(x, labels)
        plt.title(f"Processor {proc} — Migration Events per Core")
        plt.ylabel("Count")
        plt.legend()
        plt.grid(axis="y", linestyle="--", alpha=0.4)
        plt.savefig(os.path.join(REPORTS_DIR, f"migrations_p{proc}.png"), dpi=150)
        plt.close()


def plot_job_timeline_per_proc(df):
    os.makedirs(REPORTS_DIR, exist_ok=True)
    for proc, group in df.groupby("processor"):
        plt.figure(figsize=(12, 4), constrained_layout=True)

        event_map = {
            "award": "tab:green",
            "award_future": "tab:blue",
            "discarded": "tab:red",
            "push_migration": "tab:purple",
            "recv_migration": "tab:gray",
        }

        y_map = {etype: i for i, etype in enumerate(event_map.keys())}

        for etype, color in event_map.items():
            subset = group[group["type"] == etype]
            if "arrival" in subset and not subset.empty:
                plt.scatter(
                    subset["arrival"], [y_map[etype]] * len(subset), s=10, c=color
                )

        plt.title(f"Processor {proc} — Job Event Timeline")
        plt.xlabel("System Time (ticks)")
        plt.yticks(ticks=list(y_map.values()), labels=list(y_map.keys()))
        plt.grid(True, linestyle="--", alpha=0.4)
        plt.savefig(os.path.join(REPORTS_DIR, f"timeline_p{proc}.png"), dpi=150)
        plt.close()


def main():
    print("Parsing logs...")
    states, scaling_records, mode_changes = parse_all_logs()
    print(f"Parsed {len(states)} state entries, {len(scaling_records)} scaling records")

    core_series = build_core_time_series(states, scaling_records)
    if not core_series:
        print("No data; exiting.")
        return

    results = compute_util_and_scaling(core_series)

    # Existing
    plot_core_time_breakdown(core_series)
    plot_percore_utilization(results)

    # New
    plot_system_power(results)
    plot_mode_change_timeline(mode_changes)
    plot_sleep_duration_distribution(core_series)
    plot_dvfs_distribution(results)

    print("Parsing QoS and migration events...")
    df = parse_qos_migration_events()
    if df.empty:
        print("No QoS/migration events found.")
        return

    plot_awards_per_core_per_proc(df)
    plot_wcet_distribution_per_core(df)
    plot_migrations_per_core(df)
    plot_job_timeline_per_proc(df)
    print(f"Generated QoS and migration graphs for {len(df)} events.")

    print("Done.")


if __name__ == "__main__":
    main()
