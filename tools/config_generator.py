# pyright: basic
import math
from enum import IntEnum

import yaml

# --- Paths ---
SYS_CONFIG_PATH = "config/system_config.yaml"
TASK_CONFIG_PATH = "config/tasks.yaml"

OUTPUT_H_SYS_CONFIG_PATH = "include/sys_config.h"
OUTPUT_C_TASK_ALLOC_PATH = "src/task_alloc.c"


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

    def can_add_task(self, task) -> bool:
        for level_name in self.utilization.keys():
            task_util_at_level = task.get_utilization(level_name)
            if self.utilization[level_name] + task_util_at_level > 1.0:
                return False
        return True

    def add_task(self, task, task_type) -> None:
        if not self.can_add_task(task):
            raise Exception(
                f"Cannot add task {task.id} to core {self.processor_id}:{self.id}, exceeds utilization."
            )

        if task_type == TaskType.Primary:
            self.assigned_primaries.append(task)
        if task_type == TaskType.Replica:
            self.assigned_replicas.append(task)

        # Update the utilization for every criticality level
        for level_name in self.utilization.keys():
            self.utilization[level_name] += task.get_utilization(level_name)


class TaskType(IntEnum):
    Primary = 1
    Replica = 2


class Task:
    def __init__(self, task_dict, sys_config):
        self.id: int = task_dict["taskId"]
        self.name: str = task_dict["name"]
        self.period: list[int] = task_dict["period"]
        self.deadline: list[int] = task_dict["deadline"]
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
        return wcet_val / self.period[target_crit_num]


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
                if core.can_add_task(task) and not core.assigned_primaries:
                    core.add_task(task, TaskType.Primary)
                    return True

        min_num_of_primaries = float("inf")
        chosen_core = None
        chosen_proc = None

        for proc_id in range(self.num_procs_estimate):
            proc = self.processors[proc_id]
            for core in proc.cores:
                if core.can_add_task(task):
                    y = len(core.assigned_primaries)
                    if y < min_num_of_primaries:
                        min_num_of_primaries = y
                        chosen_core = core
                        chosen_proc = proc

        if chosen_core and chosen_proc:
            chosen_core.add_task(task, TaskType.Primary)
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
                if core.can_add_task(task):
                    is_safe = True
                    if task.replicas == 1:
                        for other_primary in core.assigned_primaries:
                            if primary_proc.contains_task_replica(other_primary.id):
                                is_safe = False
                                break
                    if is_safe:
                        core.add_task(task, TaskType.Replica)
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
        period_str = format_array(t.period, max_crit_levels)
        deadline_str = format_array(t.deadline, max_crit_levels)
        wcet_str = format_array(t.wcet, max_crit_levels)
        task_def = (
            f"    {{\n"
            f"        .id = {t.id},\n"
            f"        .period = {period_str},\n"
            f"        .deadline = {deadline_str},\n"
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
                map_entries.append(
                    f"    {{ .task_id = {task.id}, .task_type = Primary , .proc_id = {proc.id}, .core_id = {core.id}}}"
                )
            for task in core.assigned_replicas:
                map_entries.append(
                    f"    {{ .task_id = {task.id}, .task_type = Replica , .proc_id = {proc.id}, .core_id = {core.id}}}"
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

    print("--- Generation Complete ---")


if __name__ == "__main__":
    main()
