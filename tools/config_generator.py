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
        if crit_level == "ASIL_D":
            return True
        crit_num = self.sys_config["criticality_levels"]["levels"][crit_level]

        # Create a list of candidate tasks for this tuning operation
        all_candidates = self.assigned_primaries + self.assigned_replicas
        all_candidates.append(task)

        # Filter candidates based on criticality
        initial_candidates = [
            t for t in all_candidates if t.criticality_level > crit_num
        ]

        # No candidates to tune for this level
        if not initial_candidates:
            return True

        # Store temporary virtual deadlines to avoid permanent changes on failure
        temp_virtual_deadlines = {
            t.id: list(t.virtual_deadline) for t in all_candidates
        }

        # Work on a mutable copy of the candidate list
        tuning_candidates = list(initial_candidates)

        mods = []
        periods = [t.period for t in all_candidates]
        hyperperiod = math.lcm(*periods)
        m = crit_num
        m_prime = crit_num + 1

        # Initial adjustment of virtual deadlines
        for t in list(
            tuning_candidates
        ):  # Iterate over a copy as we might modify the list
            d = min(
                temp_virtual_deadlines[t.id][m], temp_virtual_deadlines[t.id][m_prime]
            )
            if d < t.wcet[m]:
                return False  # Fail without applying any changes
            temp_virtual_deadlines[t.id][m] = d
            if d == t.wcet[m]:
                tuning_candidates.remove(t)

        def dbf(t, m, m_prime, l):
            assert m == m_prime - 1
            vd = temp_virtual_deadlines[t.id]
            dbf_val = 0
            if m == -1:
                dbf_val = max(math.floor((l - vd[0]) / t.period) + 1, 0) * t.wcet[0]
            else:
                full = (
                    max(math.floor((l - (vd[m_prime] - vd[m])) / t.period) + 1, 0)
                    * t.wcet[m_prime]
                )
                x = l % t.period
                done = 0
                if vd[m_prime] > x and x >= vd[m_prime] - vd[m]:
                    done = t.wcet[m] - x + vd[m_prime] - vd[m]
                done = max(0, min(done, t.wcet[m_prime]))
                dbf_val = full - done

            # print("DBF", t.id, l, dbf_val)
            return max(0, dbf_val)

        changed = True
        while changed:
            changed = False
            for l in range(hyperperiod + 1):
                if crit_level == "QM":
                    sum_dbf = sum(dbf(t, -1, 0, l) for t in all_candidates)
                    if sum_dbf > l:
                        if not mods:
                            return False
                        t = mods.pop()
                        temp_virtual_deadlines[t.id][0] += 1
                        if t not in tuning_candidates:
                            tuning_candidates.append(t)
                        tuning_candidates.remove(t)
                        changed = True
                        break

                tasks_in_m_prime = [
                    t for t in all_candidates if t.criticality_level >= m_prime
                ]
                sum_dbf = sum(dbf(t, m, m_prime, l) for t in tasks_in_m_prime)
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

                    temp_virtual_deadlines[target.id][m] -= 1
                    mods.append(target)
                    if temp_virtual_deadlines[target.id][m] == target.wcet[m]:
                        tuning_candidates.remove(target)
                    changed = True
                    break

        # If all checks passed, commit the changes
        for t in initial_candidates:
            t.virtual_deadline = temp_virtual_deadlines[t.id]

        return True

    def _perform_tuning_check(self, task):
        crit_levels = self.sys_config["criticality_levels"]["levels"]
        crit_levels_sorted = sorted(
            crit_levels.keys(), key=lambda k: crit_levels[k], reverse=True
        )

        affected_tasks = self.assigned_primaries + self.assigned_replicas + [task]
        original_vds = {t.id: list(t.virtual_deadline) for t in affected_tasks}

        success = True
        for level in crit_levels_sorted:
            if self.utilization[level] + task.get_utilization(level) > 1.0:
                success = False
                break
            if not self.tune_mode(task, level):
                success = False
                break

        return success, original_vds, affected_tasks

    def tune_system(self, task, allocation_type) -> bool:
        success, original_vds, affected_tasks = self._perform_tuning_check(task)

        if not success:
            # If any check failed, restore all virtual deadlines from the backup
            for t in affected_tasks:
                if t.id in original_vds:
                    t.virtual_deadline = original_vds[t.id]
            return False

        # On complete success, add the task to the core
        if allocation_type == TaskType.Primary:
            self.assigned_primaries.append(task)
        if allocation_type == TaskType.Replica:
            self.assigned_replicas.append(task)

        for level_name in self.utilization.keys():
            self.utilization[level_name] += task.get_utilization(level_name)

        return True

    def can_tune_system(self, task) -> bool:
        success, original_vds, affected_tasks = self._perform_tuning_check(task)

        # Always restore the original virtual deadlines for a 'dry run'
        for t in affected_tasks:
            if t.id in original_vds:
                t.virtual_deadline = original_vds[t.id]

        return success


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

    def _calculate_utilization_tuple(self) -> tuple:
        """Creates a tuple of utilizations for all levels for sorting."""
        crit_levels = sorted(
            self._sys_config["criticality_levels"]["levels"].keys(),
            key=lambda k: self._sys_config["criticality_levels"]["levels"][k],
        )
        return tuple(self.get_utilization(level) for level in crit_levels)

    def get_utilization(self, level_str: str) -> float:
        """Calculates the task's utilization for a specific criticality level string."""
        crit_level_map = self._sys_config["criticality_levels"]["levels"]
        target_crit_num = crit_level_map.get(level_str)
        if target_crit_num is None:
            return 0.0

        wcet_val = self.wcet[target_crit_num]
        return wcet_val / self.period

    def __deepcopy__(self, memo):
        cls = self.__class__
        result = cls.__new__(cls)
        memo[id(self)] = result
        for k, v in self.__dict__.items():
            if k == "_sys_config":
                setattr(result, k, v)  # Shallow copy
            else:
                setattr(result, k, copy.deepcopy(v, memo))  # Deep copy
        return result


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
        for proc_id in range(self.num_procs_estimate):
            proc = self.processors[proc_id]
            for core in proc.cores:
                if core.can_tune_system(task) and not core.assigned_primaries:
                    core.tune_system(copy.deepcopy(task), TaskType.Primary)
                    return True

        min_num_of_primaries = float("inf")
        chosen_core = None
        chosen_proc = None

        for proc_id in range(self.num_procs_estimate):
            proc = self.processors[proc_id]
            for core in proc.cores:
                if core.can_tune_system(task):
                    y = len(core.assigned_primaries)
                    if y < min_num_of_primaries:
                        min_num_of_primaries = y
                        chosen_core = core
                        chosen_proc = proc

        if chosen_core and chosen_proc:
            chosen_core.tune_system(copy.deepcopy(task), TaskType.Primary)
            return True

        return False

    def allocate_replica(self, task):
        primary_proc = self.find_processor_of_primary(task.id)
        if not primary_proc:
            raise Exception(
                f"Cannot allocate replica for Task {task.id}, primary not found."
            )
        for proc_id in range(self.num_procs_estimate):
            proc = self.processors[proc_id]
            if proc.id == primary_proc.id or proc.contains_task_replica(task.id):
                continue

            for core in proc.cores:
                if core.can_tune_system(task):
                    is_safe = True
                    if task.replicas == 1:
                        for other_primary in core.assigned_primaries:
                            if primary_proc.contains_task_replica(other_primary.id):
                                is_safe = False
                                break
                    if is_safe:
                        core.tune_system(copy.deepcopy(task), TaskType.Replica)
                        return True
        return False

    def allocate_tasks(self, taskset):
        for task in taskset:
            while not self.allocate_primary(task):
                self.num_procs_estimate += 1

                if (
                    self.num_procs_estimate
                    > self.sys_config["system"]["num_processors"]
                ):
                    raise Exception("Insufficient processors to allocate all tasks.")

            for _ in range(task.replicas):
                while not self.allocate_replica(task):
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
            print("Allocating tasks of criticality level:", taskset[0].criticality_str)
            self.allocate_tasks(taskset)


# Formats a Python list into a C array string, padding with 0 if needed
def format_array(arr, max_levels):
    if len(arr) > max_levels:
        raise ValueError("Task has more WCET entries than MAX_CRITICALITY_LEVELS")

    # Pad the list with 0s to match the required array size in C
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
        f"#define NUM_PROC {sys_config['system']['num_processors']}",
        f"#define NUM_CORES_PER_PROC {sys_config['system']['num_cores_per_processor']}",
        f"#define MAX_TASKS {sys_config['system']['max_tasks']}",
        f"#define MAX_CRITICALITY_LEVELS {sys_config['criticality_levels']['max_levels']}\n",
        "// Task Properties",
        "typedef enum {",
        enum_members_str,
        "} CriticalityLevel;\n",
        "#endif",
    ]

    with open(OUTPUT_H_SYS_CONFIG_PATH, "w") as f:
        f.write("\n".join(lines))
    print(f"Successfully generated {OUTPUT_H_SYS_CONFIG_PATH}")


def generate_task_config_c(allocator: Allocator):
    # Generate the static task definitions
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
            f"        .criticality_level = {t.criticality_str},\n"
            f"        .num_replicas = {t.replicas}\n"
            f"    }}"
        )
        task_definitions.append(task_def)

    # Generate the allocation map
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
#include "task_alloc.h"
#include "task_management.h"
#include <stdint.h>

// This file is auto-generated by tools/config_generator.py
// DO NOT EDIT BY HAND

// Definitions for the system's static task set.
const Task system_tasks[] = {{
{task_definitions_str}
}};
const uint32_t SYSTEM_TASKS_SIZE = sizeof(system_tasks) / sizeof(Task);

// Definitions for the offline-calculated task allocation map.
const TaskAllocationMap allocation_map[] = {{
{map_entries_str}
}};
const uint32_t ALLOCATION_MAP_SIZE = sizeof(allocation_map) / sizeof(TaskAllocationMap);
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

    # Get the criticality levels sorted from low to high for plotting
    crit_levels = sorted(
        allocator.sys_config["criticality_levels"]["levels"].keys(),
        key=lambda k: allocator.sys_config["criticality_levels"]["levels"][k],
    )

    # --- Prepare the data for plotting ---
    # Create a dictionary to hold the utilization data for each level
    util_data = {level: [] for level in crit_levels}
    core_labels = []

    for proc in processors:
        for core in proc.cores:
            core_labels.append(f"P{proc.id}C{core.id}")
            for level in crit_levels:
                util_data[level].append(core.utilization.get(level, 0.0))

    # --- Create the plot ---
    width = 0.6  # Width of the bars
    fig, ax = plt.subplots(figsize=(16, 8))
    bottom = np.zeros(num_procs * num_cores)  # Start the first bar at the bottom

    for level in crit_levels:
        util_values = np.array(util_data[level])
        ax.bar(core_labels, util_values, width, label=level, bottom=bottom)
        bottom += util_values  # Stack the next bar on top of the current one

    # --- Customize and save the plot ---
    ax.set_title("Per-Core Utilization by Criticality Level", fontsize=16)
    ax.set_ylabel("Cumulative Utilization")
    ax.set_xlabel("Processor and Core ID")
    ax.set_ylim(0, max(1.1, bottom.max() * 1.1))  # Set y-axis limit
    ax.legend(title="Criticality Level")

    plt.xticks(rotation=45, ha="right")  # Rotate x-axis labels for readability
    plt.tight_layout()  # Adjust layout to prevent labels from overlapping
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

    x = np.arange(len(core_labels))  # the label locations
    width = 0.15  # the width of the bars
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

    # Create subdirectories
    heatmap_dir = f"{REPORT_PATH}/heatmaps"
    os.makedirs(heatmap_dir, exist_ok=True)

    # Generate reports
    write_allocation_report(allocator, f"{REPORT_PATH}/allocation_report.txt")
    generate_utilization_heatmaps(allocator, heatmap_dir)
    generate_utilization_stacked_chart(allocator, f"{REPORT_PATH}/stacked_chart.png")
    generate_utilization_grouped_chart(allocator, f"{REPORT_PATH}/grouped_chart.png")


# --- Main Execution ---
def main():
    global sys_config
    print("--- Configuration Generator ---")

    # 1. Parse YAML files
    with open(SYS_CONFIG_PATH, "r") as f:
        sys_config = yaml.safe_load(f)
    with open(TASK_CONFIG_PATH, "r") as f:
        task_config = yaml.safe_load(f)

    tasks = task_config.get("tasks", [])
    if not tasks:
        print("No tasks found. Exiting.")
        return

    # 2. Generate config.h
    generate_sys_config_h(sys_config)

    # 3. Perform allocation
    allocator = Allocator(sys_config, [Task(t, sys_config) for t in tasks])
    allocator.run()

    # 4. Generate task_config.h and task_config.c
    generate_task_config_c(allocator)

    # 5. Reporting and Visualization
    generate_reports(allocator)

    print("--- Generation Complete ---")


if __name__ == "__main__":
    main()
