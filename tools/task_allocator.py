# pyright: basic
import copy
import math
from enum import IntEnum

import matplotlib.pyplot as plt
import numpy as np
import seaborn as sns
import yaml

# --- Paths ---
SYS_CONFIG_PATH = "config/system_config.yaml"
TASK_CONFIG_PATH = "config/tasks.yaml"

OUTPUT_H_SYS_CONFIG_PATH = "include/sys_config.h"
OUTPUT_C_TASK_ALLOC_PATH = "src/task_alloc.c"

REPORT_PATH = "target/reports"


class Processor:
    def __init__(self, processor_id: int, num_cores: int, sys_config):
        self.id: int = processor_id
        self.cores: list[Core] = [
            Core(core_id, processor_id, sys_config) for core_id in range(num_cores)
        ]

    def contains_task_primary(self, task_id: int) -> bool:
        for core in self.cores:
            for t in core.assigned_primaries:
                if t.id == task_id:
                    return True
        return False

    def contains_task_replica(self, task_id: int) -> bool:
        for core in self.cores:
            for t in core.assigned_replicas:
                if t.id == task_id:
                    return True
        return False


class Core:
    def __init__(self, id: int, processor_id: int, sys_config: dict):
        self.id: int = id
        self.processor_id: int = processor_id
        self.assigned_primaries: list = []
        self.assigned_replicas: list = []
        self.utilization: dict[str, float] = {
            level_name: 0.0 for level_name in sys_config["criticality_levels"]["levels"]
        }
        self.sys_config = sys_config

    def tune_mode(self, task, crit_level) -> bool:
        crit_num = self.sys_config["criticality_levels"]["levels"][crit_level]
        if crit_num == self.sys_config["criticality_levels"]["max_levels"] - 1:
            return True

        all_candidates = self.assigned_primaries + self.assigned_replicas
        all_candidates.append(task)

        initial_candidates = [
            t for t in all_candidates if t.criticality_level > crit_num
        ]

        if not initial_candidates:
            return True

        tuning_candidates = list(initial_candidates)

        mods = []

        m = crit_num
        m_prime = crit_num + 1

        for t in list(tuning_candidates):
            d = min(t.virtual_deadline[m], t.virtual_deadline[m_prime])
            if d < t.wcet[m]:
                return False
            t.virtual_deadline[m] = d
            if d == t.wcet[m]:
                tuning_candidates.remove(t)

        def dbf(t, m, m_prime, l):
            assert m == m_prime - 1

            if l < 0:
                return 0

            if t.criticality_level < m_prime:
                return 0

            vd = t.virtual_deadline
            dbf_val = 0
            if m == -1:
                dbf_val = max((math.floor((l - vd[0]) / t.period) + 1) * t.wcet[0], 0)
            else:
                full = max(
                    (math.floor((l - (vd[m_prime] - vd[m])) / t.period) + 1)
                    * t.wcet[m_prime],
                    0,
                )
                x = l % t.period
                done = 0
                if vd[m_prime] > x and x >= vd[m_prime] - vd[m]:
                    done = t.wcet[m] - x + vd[m_prime] - vd[m]
                done = max(0, min(done, t.wcet[m_prime]))
                dbf_val = full - done

            assert dbf_val >= 0
            return max(0, dbf_val)

        def compute_l_max(tasks, m, m_prime, cap):
            def get_mode_l_max(task_subset, mode, is_hi_mode=False):
                if not task_subset:
                    return 0

                util = sum(t.wcet[mode] / t.period for t in task_subset)
                hyperperiod = math.lcm(*[t.period for t in task_subset])

                if is_hi_mode:
                    deadlines = [
                        t.virtual_deadline[m_prime] - t.virtual_deadline[m]
                        for t in task_subset
                    ]
                else:
                    deadlines = [t.virtual_deadline[mode] for t in task_subset]

                max_deadline = max(deadlines)
                bound_hyper = hyperperiod + max_deadline

                if util < 1:
                    max_diff = max(t.period - d for t, d in zip(task_subset, deadlines))
                    bound_density = (util / (1 - util)) * max_diff
                    return min(bound_hyper, bound_density)
                else:
                    return bound_hyper

            m_tasks = [t for t in tasks if t.criticality_level >= m]
            mp_tasks = [t for t in tasks if t.criticality_level >= m_prime]

            l_max_m = get_mode_l_max(m_tasks, m, is_hi_mode=False)
            l_max_mp = get_mode_l_max(mp_tasks, m_prime, is_hi_mode=True)

            return (
                min(cap, int(max(l_max_m, l_max_mp)))
                if cap > 0
                else int(max(l_max_m, l_max_mp))
            )

        changed = True

        while changed:
            changed = False
            l_max = compute_l_max(all_candidates, m, m_prime, 10**6)

            for l in range(0, l_max + 1):
                if crit_num == 0:
                    sum_dbf = sum(dbf(t, -1, 0, l) for t in all_candidates)
                    if sum_dbf > l:
                        if not mods:
                            return False
                        t = mods.pop()
                        t.virtual_deadline[0] += 1
                        if t not in tuning_candidates:
                            tuning_candidates.append(t)
                        tuning_candidates.remove(t)
                        changed = True
                        break

                sum_dbf = sum(dbf(t, m, m_prime, l) for t in all_candidates)
                if sum_dbf > l:
                    if not tuning_candidates:
                        return False

                    target = tuning_candidates[0]
                    max_delta = 0
                    for t in tuning_candidates:
                        a = dbf(t, m, m_prime, l)
                        b = dbf(t, m, m_prime, l - 1)
                        if a - b > max_delta:
                            max_delta = a - b
                            target = t

                    target.virtual_deadline[m] -= 1
                    mods.append(target)
                    if target.virtual_deadline[m] == target.wcet[m]:
                        tuning_candidates.remove(target)
                    changed = True
                    break

        return True

    def tune_system(self, task, allocation_type, commit=True) -> bool:
        crit_levels = self.sys_config["criticality_levels"]["levels"]
        crit_levels_sorted = sorted(
            crit_levels.keys(), key=lambda k: crit_levels[k], reverse=True
        )

        affected_tasks = self.assigned_primaries + self.assigned_replicas + [task]
        original_vds = {t.id: list(t.virtual_deadline) for t in affected_tasks}

        for task in affected_tasks:
            task.reset_virtual_deadlines()

        success = True
        for level in crit_levels_sorted:
            crit_num = self.sys_config["criticality_levels"]["levels"][level]
            if (
                task.criticality_level >= crit_num
                and self.utilization[level] + task.get_utilization(level) > 1.0
            ):
                success = False
                break
            if not self.tune_mode(task, level):
                success = False
                break

        if not success:
            for t in affected_tasks:
                if t.id in original_vds:
                    t.virtual_deadline = original_vds[t.id]
            return False

        if not commit:
            for t in affected_tasks:
                if t.id in original_vds:
                    t.virtual_deadline = original_vds[t.id]
            return True

        if allocation_type == TaskType.Primary:
            self.assigned_primaries.append(task)
        if allocation_type == TaskType.Replica:
            self.assigned_replicas.append(task)

        for level_name in self.utilization.keys():
            crit_num = self.sys_config["criticality_levels"]["levels"][level_name]
            if task.criticality_level >= crit_num:
                self.utilization[level_name] += task.get_utilization(level_name)

        return True


class TaskType(IntEnum):
    Primary = 1
    Replica = 2


class Task:
    def __init__(self, task_dict, sys_config):
        self.id: int = task_dict["taskId"]
        self.name: str = task_dict["name"]
        self.period: int = task_dict["period"]
        self.deadline: int = task_dict["deadline"]
        self.virtual_deadline: list[int] = [
            task_dict["deadline"] for _ in range(len(task_dict["wcet"]))
        ]
        self.criticality_str: str = task_dict["criticality"]
        self.wcet: list[int] = task_dict["wcet"]
        self.replicas: int = task_dict.get("replicas", 0)
        self._sys_config = sys_config
        self.criticality_level: int = self._sys_config["criticality_levels"]["levels"][
            self.criticality_str
        ]
        self.utilization_tuple: tuple = self._calculate_utilization_tuple()

    def get_utilization(self, level_str: str) -> float:
        """Calculates the task's utilization for a specific criticality level string."""
        crit_level_map = self._sys_config["criticality_levels"]["levels"]
        target_crit_num = crit_level_map.get(level_str)
        if target_crit_num is None:
            return 0.0

        wcet_val = self.wcet[target_crit_num]
        return wcet_val / self.period

    def reset_virtual_deadlines(self):
        self.virtual_deadline = [
            self.deadline for _ in range(len(self.virtual_deadline))
        ]

    def _calculate_utilization_tuple(self) -> tuple:
        """Creates a tuple of utilizations for all levels for sorting."""
        crit_levels = sorted(
            self._sys_config["criticality_levels"]["levels"].keys(),
            key=lambda k: self._sys_config["criticality_levels"]["levels"][k],
        )
        return tuple(self.get_utilization(level) for level in crit_levels)

    def clone(self):
        new_task = copy.deepcopy(self)
        return new_task


class Allocator:
    def __init__(self, sys_config: dict, tasks: list):
        self.sys_config = sys_config
        self.tasks = tasks

        # Initialize the hardware model that the allocator will manage
        num_procs = self.sys_config["system"]["num_processors"]
        num_cores_per_proc = self.sys_config["system"]["num_cores_per_processor"]
        self.processors: list[Processor] = [
            Processor(proc_id, num_cores_per_proc, self.sys_config)
            for proc_id in range(num_procs)
        ]
        self.num_procs_estimate = self._calculate_num_proc_estimate()
        self.next_proc_index = 0

    def _sort_tasks(self) -> list:
        crit_levels_high_to_low = sorted(
            self.sys_config["criticality_levels"]["levels"].keys(),
            key=lambda k: self.sys_config["criticality_levels"]["levels"][k],
            reverse=True,
        )

        grouped_tasks = {level: [] for level in crit_levels_high_to_low}
        for task in self.tasks:
            grouped_tasks[task.criticality_str].append(task)

        sorted_tasks = []
        for level in crit_levels_high_to_low:
            group = grouped_tasks[level]
            group.sort(key=lambda t: t.utilization_tuple, reverse=True)
            sorted_tasks.append(group)

        return sorted_tasks

    def _calculate_num_proc_estimate(self) -> int:
        total_utilization = 0.0
        max_replicas = 0
        for task in self.tasks:
            total_utilization += task.utilization_tuple[task.criticality_level] * (
                task.replicas + 1
            )
            max_replicas = max(max_replicas, task.replicas)

        num_procs_est_by_util = math.ceil(
            total_utilization / self.sys_config["system"]["num_cores_per_processor"]
        )
        num_procs_est_by_replicas = max_replicas + 1

        num_procs_est = max(num_procs_est_by_util, num_procs_est_by_replicas)

        if num_procs_est > self.sys_config["system"]["num_processors"]:
            raise Exception("Initial processor estimate exceeds available processors.")

        return num_procs_est

    def find_processor_of_primary(self, task_id: int):
        for proc in self.processors:
            if proc.contains_task_primary(task_id):
                return proc
        return None

    def allocate_primary(self, task):
        start = self.next_proc_index
        for offset in range(self.num_procs_estimate):
            proc_id = (start + offset) % self.num_procs_estimate
            proc = self.processors[proc_id]
            for core in proc.cores:
                if core.tune_system(task, TaskType.Primary):
                    self.next_proc_index = (proc_id + 1) % self.num_procs_estimate
                    return True

        return False

    def allocate_replica(self, task):
        start = self.next_proc_index
        primary_proc = self.find_processor_of_primary(task.id)
        if not primary_proc:
            raise Exception(
                f"Cannot allocate replica for Task {task.id}, primary not found."
            )
        for offset in range(self.num_procs_estimate):
            proc_id = (start + offset) % self.num_procs_estimate
            proc = self.processors[proc_id]
            if proc.id == primary_proc.id or proc.contains_task_replica(task.id):
                continue

            for core in proc.cores:
                is_safe = True
                if task.replicas == 1:
                    for other_primary in core.assigned_primaries:
                        if primary_proc.contains_task_replica(other_primary.id):
                            is_safe = False
                            break
                if is_safe and core.tune_system(task, TaskType.Replica):
                    self.next_proc_index = (proc_id + 1) % self.num_procs_estimate
                    return True
        return False

    def allocate_tasks(self, taskset):
        for task in taskset:
            new_task_instance = task.clone()
            while not self.allocate_primary(new_task_instance):
                self.num_procs_estimate += 1

                if (
                    self.num_procs_estimate
                    > self.sys_config["system"]["num_processors"]
                ):
                    raise Exception("Insufficient processors to allocate all tasks.")

            for _ in range(task.replicas):
                new_task_instance = task.clone()
                while not self.allocate_replica(new_task_instance):
                    self.num_procs_estimate += 1

                    if (
                        self.num_procs_estimate
                        > self.sys_config["system"]["num_processors"]
                    ):
                        raise Exception(
                            "Insufficient processors to allocate all tasks."
                        )

    def run(self):
        sorted_tasks = self._sort_tasks()

        for taskset in sorted_tasks:
            if not taskset:
                continue
            print("Allocating tasks of criticality level:", taskset[0].criticality_str)
            self.allocate_tasks(taskset)


# Formats a Python list into a C array string, padding with 0 if needed
def format_array(arr, max_levels):
    if len(arr) > max_levels:
        raise ValueError("Task has more WCET entries than MAX_CRITICALITY_LEVELS")

    padded_arr = arr + [0] * (max_levels - len(arr))
    return f"{{ {', '.join(map(str, padded_arr))} }}"


# --- File Generation Functions ---
def generate_sys_config_h(sys_config):
    enum_members = []
    for name, value in sys_config["criticality_levels"]["levels"].items():
        enum_members.append(f"  {name} = {value},")

    enum_members.reverse()
    enum_members_str = "\n".join(enum_members)

    lines = [
        "#ifndef SYS_CONFIG_H",
        "#define SYS_CONFIG_H\n",
        "// This file is auto-generated by tools/config_generator.py",
        "// DO NOT EDIT BY HAND\n",
        "// System Config",
        f"#ifdef NUM_FAULTS\n"
        f"#define NUM_PROC ({sys_config['system']['num_processors']} - NUM_FAULTS)",
        "#else",
        f"#define NUM_PROC {sys_config['system']['num_processors']}",
        "#endif\n",
        f"#define NUM_CORES_PER_PROC {sys_config['system']['num_cores_per_processor']}",
        f"#define MAX_TASKS {sys_config['system']['max_tasks']}",
        f"#define MAX_CRITICALITY_LEVELS {sys_config['criticality_levels']['max_levels']}\n",
        "// Task Properties",
        "typedef enum {",
        enum_members_str,
        "} criticality_level;\n",
        "#endif",
    ]

    with open(OUTPUT_H_SYS_CONFIG_PATH, "w") as f:
        f.write("\n".join(lines))
    print(f"Successfully generated {OUTPUT_H_SYS_CONFIG_PATH}")


def generate_task_config_c(allocator: Allocator):
    task_definitions = []
    max_crit_levels = allocator.sys_config["criticality_levels"]["max_levels"]
    for t in allocator.tasks:
        wcet_str = format_array(t.wcet, max_crit_levels)
        task_def = (
            f"    {{\n"
            f"        .id = {t.id},\n"
            f"        .period = {t.period},\n"
            f"        .deadline = {t.deadline},\n"
            f"        .wcet = {wcet_str},\n"
            f"        .crit_level = {t.criticality_str},\n"
            f"        .num_replicas = {t.replicas}\n"
            f"    }}"
        )
        task_definitions.append(task_def)

    map_entries = []
    for proc in allocator.processors:
        for core in proc.cores:
            for task in core.assigned_primaries:
                virtual_deadline_str = format_array(
                    task.virtual_deadline, max_crit_levels
                )
                map_entries.append(
                    f"    {{ .task_id = {task.id}, .task_type = Primary , .proc_id = {proc.id}, .core_id = {core.id}, .tuned_deadlines = {virtual_deadline_str}}}"
                )
            for task in core.assigned_replicas:
                virtual_deadline_str = format_array(
                    task.virtual_deadline, max_crit_levels
                )
                map_entries.append(
                    f"    {{ .task_id = {task.id}, .task_type = Replica , .proc_id = {proc.id}, .core_id = {core.id}, .tuned_deadlines = {virtual_deadline_str}}}"
                )

    task_definitions_str = ",\n".join(task_definitions)
    map_entries_str = ",\n".join(map_entries)
    c_content = f"""#include "task_alloc.h"
#include "sys_config.h"
#include "task_management.h"
#include <stdint.h>

// This file is auto-generated by tools/config_generator.py
// DO NOT EDIT BY HAND

// Definitions for the system's static task set.
const task_struct system_tasks[] = {{
{task_definitions_str}
}};
const uint32_t SYSTEM_TASKS_SIZE = sizeof(system_tasks) / sizeof(task_struct);

// Definitions for the offline-calculated task allocation map.
const task_alloc_map allocation_map[] = {{
{map_entries_str}
}};
const uint32_t ALLOCATION_MAP_SIZE = sizeof(allocation_map) / sizeof(task_alloc_map);
"""
    with open(OUTPUT_C_TASK_ALLOC_PATH, "w") as f:
        f.write(c_content)
    print(f"Successfully generated {OUTPUT_C_TASK_ALLOC_PATH}")


# --- Report and Visualization Generation Functions ---
def write_allocation_report(allocator: Allocator, filename: str):
    report_lines = ["=" * 50, "--- Final Allocation Report ---", "=" * 50]

    num_tasks = len(allocator.tasks)
    procs_used = 0
    cores_used = 0
    assigned_tasks_count = 0

    for proc in allocator.processors:
        proc_task_num = 0

        for core in proc.cores:
            proc_task_num += len(core.assigned_primaries) + len(core.assigned_replicas)
            if len(core.assigned_primaries) + len(core.assigned_replicas) > 0:
                cores_used += 1

        assigned_tasks_count += proc_task_num

        if proc_task_num > 0:
            procs_used += 1

    report_lines.append(f"\n[Summary]")
    report_lines.append(f"  - Total Unique Tasks: {num_tasks}")
    report_lines.append(
        f"  - Total Instances (Primaries + Replicas): {assigned_tasks_count}"
    )
    report_lines.append(
        f"  - Processors Used: {procs_used} / {allocator.sys_config['system']['num_processors']}"
    )
    report_lines.append(f"  - Cores Used: {cores_used}")
    report_lines.append("-" * 50)

    for proc in allocator.processors:
        report_lines.append(f"\nProcessor {proc.id}:")
        if not any(
            core.assigned_primaries or core.assigned_replicas for core in proc.cores
        ):
            report_lines.append("  <Unused>")
            continue

        for core in proc.cores:
            primaries = [t.name for t in core.assigned_primaries]
            replicas = [f"{t.name}(R)" for t in core.assigned_replicas]

            report_lines.append(f"\n  Core {core.id}:")
            if not primaries and not replicas:
                report_lines.append("    <Empty>")
                continue

            util_strings = []
            crit_levels_rev = sorted(
                core.utilization.keys(),
                key=lambda k: allocator.sys_config["criticality_levels"]["levels"][k],
                reverse=True,
            )
            for level in crit_levels_rev:
                util_strings.append(f"{level}: {core.utilization.get(level, 0.0):.2f}")
            report_lines.append(f"    Util:      | {' | '.join(util_strings)}")

            if primaries:
                report_lines.append(f"    Primaries: | {', '.join(primaries)}")
            if replicas:
                report_lines.append(f"    Replicas:  | {', '.join(replicas)}")

    with open(filename, "w") as f:
        f.write("\n".join(report_lines))
    print(f"Successfully generated text report at '{filename}'")


def generate_utilization_heatmaps(allocator: Allocator, output_dir: str):
    """Generates a graphical heatmap of core utilizations for each criticality level."""
    print(f"Generating utilization heatmaps in '{output_dir}'...")
    num_procs = len(allocator.processors)
    num_cores = allocator.sys_config["system"]["num_cores_per_processor"]
    crit_levels = allocator.sys_config["criticality_levels"]["levels"].keys()

    for level in crit_levels:
        util_data = np.zeros((num_procs, num_cores))
        for proc in allocator.processors:
            for core in proc.cores:
                util_data[proc.id, core.id] = core.utilization.get(level, 0.0)

        plt.figure(figsize=(10, 8))
        heatmap = sns.heatmap(
            util_data,
            annot=True,
            fmt=".2f",
            linewidths=0.5,
            cmap="viridis",
            vmin=0.0,
            vmax=1.0,
        )
        heatmap.set_title(
            f"Core Utilization @ {level}", fontdict={"fontsize": 16}, pad=12
        )
        heatmap.set_xlabel("Core ID")
        heatmap.set_ylabel("Processor ID")

        plt.savefig(f"{output_dir}/heatmap_{level}.png")
        plt.close()
    print("Heatmap generation complete.")


def generate_utilization_stacked_chart(
    allocator: Allocator, filename="allocation_stacked_chart.png"
):
    print(f"\nGenerating utilization stacked bar chart at '{filename}'...")

    processors = allocator.processors
    num_procs = len(processors)
    num_cores = allocator.sys_config["system"]["num_cores_per_processor"]

    crit_levels = sorted(
        allocator.sys_config["criticality_levels"]["levels"].keys(),
        key=lambda k: allocator.sys_config["criticality_levels"]["levels"][k],
    )

    util_data = {level: [] for level in crit_levels}
    core_labels = []

    for proc in processors:
        for core in proc.cores:
            core_labels.append(f"P{proc.id}C{core.id}")
            for level in crit_levels:
                util_data[level].append(core.utilization.get(level, 0.0))

    width = 0.6
    fig, ax = plt.subplots(figsize=(16, 8))
    bottom = np.zeros(num_procs * num_cores)

    for level in crit_levels:
        util_values = np.array(util_data[level])
        ax.bar(core_labels, util_values, width, label=level, bottom=bottom)
        bottom += util_values

    # --- Customize and save the plot ---
    ax.set_title("Per-Core Utilization by Criticality Level", fontsize=16)
    ax.set_ylabel("Cumulative Utilization")
    ax.set_xlabel("Processor and Core ID")
    ax.set_ylim(0, max(1.1, bottom.max() * 1.1))
    ax.legend(title="Criticality Level")

    plt.xticks(rotation=45, ha="right")
    plt.tight_layout()
    plt.savefig(filename)
    plt.close()

    print("Stacked bar chart generation complete.")


def generate_utilization_grouped_chart(
    allocator: Allocator, filename="allocation_grouped_chart.png"
):
    print(f"\nGenerating utilization grouped bar chart at '{filename}'...")

    processors = allocator.processors
    core_labels = [
        f"P{p.id}C{c.id}" for p in processors for c in processors[p.id].cores
    ]
    crit_levels = sorted(
        allocator.sys_config["criticality_levels"]["levels"].keys(),
        key=lambda k: allocator.sys_config["criticality_levels"]["levels"][k],
    )

    util_data = {
        level: [c.utilization.get(level, 0.0) for p in processors for c in p.cores]
        for level in crit_levels
    }

    x = np.arange(len(core_labels))
    width = 0.15
    multiplier = 0

    fig, ax = plt.subplots(figsize=(20, 8))

    for level, utilization_values in util_data.items():
        offset = width * multiplier
        rects = ax.bar(x + offset, utilization_values, width, label=level)
        multiplier += 1

    ax.set_title("Per-Core Utilization by Criticality Level", fontsize=16)
    ax.set_ylabel("Utilization")
    ax.set_xticks(x + width * (len(crit_levels) - 1) / 2)
    ax.set_xticklabels(core_labels, rotation=45, ha="right")
    ax.legend(loc="upper left", ncols=len(crit_levels))
    ax.set_ylim(0, 1.1)

    plt.tight_layout()
    plt.savefig(filename)
    plt.close()

    print("Grouped bar chart generation complete.")


import os


def generate_reports(allocator: Allocator):
    """Orchestrates the creation of all reports and visualizations."""
    print("\n--- Generating Reports and Visualizations ---")

    heatmap_dir = f"{REPORT_PATH}/heatmaps"
    os.makedirs(heatmap_dir, exist_ok=True)

    write_allocation_report(allocator, f"{REPORT_PATH}/allocation_report.txt")
    generate_utilization_heatmaps(allocator, heatmap_dir)
    generate_utilization_stacked_chart(allocator, f"{REPORT_PATH}/stacked_chart.png")
    generate_utilization_grouped_chart(allocator, f"{REPORT_PATH}/grouped_chart.png")


# --- Main Execution ---
def main():
    global sys_config
    print("--- Task Allocator ---")

    with open(SYS_CONFIG_PATH, "r") as f:
        sys_config = yaml.safe_load(f)
    with open(TASK_CONFIG_PATH, "r") as f:
        task_config = yaml.safe_load(f)

    tasks = task_config.get("tasks", [])
    if not tasks:
        print("No tasks found. Exiting.")
        return

    generate_sys_config_h(sys_config)

    allocator = Allocator(sys_config, [Task(t, sys_config) for t in tasks])
    allocator.run()
    print("Allocation complete")

    generate_task_config_c(allocator)
    print("C source/header files generation complete")

    generate_reports(allocator)
    print("Report generation complete")


if __name__ == "__main__":
    main()
