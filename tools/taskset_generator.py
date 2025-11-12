# pyright: basic

"""
tools/taskset_generator.py

Generate mixed-criticality periodic tasksets and write config/tasks.yaml (or batch files).

Usage example:
  python tools/taskset_generator.py --config config/system_config.yaml \
      --num-tasks 40 --avg-util 0.25 --replica-mean 1.5 --seed 42 \
      --period-min 10 --period-max 200 --period-mode loguniform \
      --crit-dist '{"ASIL_D":0.2,"ASIL_C":0.25,"ASIL_B":0.3,"ASIL_A":0.15,"QM":0.1}' \
      --output config/tasks.yaml
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Dict, List, Tuple

import yaml

DEFAULT_OUTPUT = "config/tasks.yaml"
DEFAULT_CONFIG = "config/system_config.yaml"
DEFAULT_WCET_SCALE = [
    1.0,
    1.2,
    1.5,
    2.0,
    3.0,
]
DEFAULT_PERIODS = [10, 20, 50, 100, 200, 500]
DEFAULT_CRIT_DIST = None
UUNIFAST_DISCARD_MAX_TRIES = 1000


@dataclass
class GenArgs:
    config: str
    num_tasks: int
    avg_util: float
    total_util: float | None
    replica_mean: float
    replica_max: int
    seed: int | None
    output: str
    crit_dist: Dict[str, float] | None
    period_min: int
    period_max: int
    period_mode: str
    period_choices: List[int] | None
    enforce_harmonic: bool
    wcet_scale: List[float]
    util_sampler: str
    util_stddev: float
    num_sets: int
    prefix: str
    mode: str
    summary: bool
    deadline_equal_period: bool


def load_system_config(path: str) -> dict:
    with open(path, "r") as f:
        return yaml.safe_load(f)


def write_yaml_with_metadata(payload: dict, outfile: str, metadata: dict) -> None:
    head_comments = [
        f"# Generated: {datetime.now(timezone.utc).isoformat()}Z",
        f"# generator: taskset_generator.py",
        f"# metadata: {json.dumps(metadata, sort_keys=True)}",
        "",
    ]
    os.makedirs(os.path.dirname(outfile) or ".", exist_ok=True)
    with open(outfile, "w") as f:
        for line in head_comments:
            f.write(line + "\n")
        yaml.safe_dump(payload, f, sort_keys=False)


def safe_int(x) -> int:
    return int(round(float(x)))


def uunifast(n: int, U_total: float) -> List[float]:
    """Plain UUniFast (uncapped)."""
    if n <= 0:
        return []
    sum_u = U_total
    utilizations: List[float] = []
    for i in range(1, n):
        next_sum = sum_u * (random.random() ** (1.0 / (n - i)))
        utilizations.append(sum_u - next_sum)
        sum_u = next_sum
    utilizations.append(sum_u)
    return utilizations


def uunifast_discard(
    n: int, U_total: float, max_tries: int = UUNIFAST_DISCARD_MAX_TRIES
) -> List[float]:
    """UUniFast-Discard: regenerate until all U_i <= 1.0 (or fail after max_tries)."""
    if U_total < 0:
        raise ValueError("U_total must be ≥ 0")
    if n <= 0:
        return []
    for _ in range(max_tries):
        utils = uunifast(n, U_total)
        if all(u <= 1.0 + 1e-12 for u in utils):
            return utils
    raise RuntimeError(
        f"uunifast_discard: failed to generate valid utils after {max_tries} tries"
    )


_chosen_periods: List[int] = []

import math
import random
from math import gcd


def lcm(a, b):
    return abs(a * b) // gcd(a, b)


def is_prime(n: int) -> bool:
    if n < 2:
        return False
    for i in range(2, int(math.sqrt(n)) + 1):
        if n % i == 0:
            return False
    return True


def sample_period(
    period_min: int,
    period_max: int,
    mode: str,
    choices: list[int] | None,
    enforce_harmonic: bool = False,
) -> int:
    global _chosen_periods

    if choices:
        candidate = random.choice(choices)
    elif mode == "uniform":
        candidate = random.randint(period_min, period_max)
    elif mode == "loguniform":
        a, b = math.log(period_min), math.log(period_max)
        candidate = int(round(math.exp(random.random() * (b - a) + a)))
    else:
        candidate = random.randint(period_min, period_max)

    if enforce_harmonic:
        if not _chosen_periods:
            for _ in range(20):
                if not is_prime(candidate) and any(
                    candidate % f == 0 for f in (2, 3, 5)
                ):
                    break
                candidate += random.choice([-1, 1]) * random.randint(1, 3)
            _chosen_periods = [candidate]
        else:
            base = random.choice(_chosen_periods)
            factor = random.randint(1, 8)
            candidate = base // factor if base % factor == 0 else base * factor

            for _ in range(10):
                if all(gcd(candidate, p) != 1 for p in _chosen_periods):
                    break
                candidate += random.randint(-5, 5)

            for _ in range(5):
                if is_prime(candidate):
                    candidate += random.choice([-1, 1]) * random.randint(1, 3)
                else:
                    break

        span = period_max - period_min
        candidate = ((candidate - period_min) % span) + period_min

        for _ in range(5):
            if is_prime(candidate) or all(
                gcd(candidate, p) == 1 for p in _chosen_periods
            ):
                candidate += random.choice([-1, 1]) * random.randint(1, 3)
                candidate = ((candidate - period_min) % span) + period_min
            else:
                break

        _chosen_periods.append(candidate)

    return candidate


def normalize_weights(mapping: Dict[str, float]) -> Dict[str, float]:
    s = sum(mapping.values())
    if s <= 0:
        raise ValueError("crit-dist must have positive sum")
    return {k: float(v) / s for k, v in mapping.items()}


def sample_criticality(crit_dist: Dict[str, float]) -> str:
    keys = list(crit_dist.keys())
    weights = [crit_dist[k] for k in keys]
    return random.choices(keys, weights=weights, k=1)[0]


def sample_replicas(replica_mean: float, replica_max: int) -> int:
    lam = max(0.0, replica_mean)
    if lam < 10:
        L = math.exp(-lam)
        k = 0
        p = 1.0
        while p > L:
            k += 1
            p *= random.random()
        r = max(0, k - 1)
    else:
        r = max(0, int(round(random.gauss(lam, math.sqrt(lam)))))
    return min(r, max(0, replica_max))


def compute_wcet_vector(
    base_wcet: float, max_levels: int, wcet_scale: List[float]
) -> List[int]:
    wcet = []
    for i in range(max_levels):
        factor = wcet_scale[i] if i < len(wcet_scale) else wcet_scale[-1]
        val = max(1, int(math.ceil(base_wcet * factor)))
        wcet.append(val)
    for i in range(1, len(wcet)):
        if wcet[i] < wcet[i - 1]:
            wcet[i] = wcet[i - 1]
    return wcet


def finalize_wcet_against_period(wcet_list: List[int], period: int) -> List[int]:
    fixed = []
    for w in wcet_list:
        if w >= period:
            fixed_w = max(1, period - 1)
        else:
            fixed_w = w
        fixed.append(fixed_w)
    for i in range(1, len(fixed)):
        if fixed[i] < fixed[i - 1]:
            fixed[i] = fixed[i - 1]
    return fixed


def generate_single_task(
    task_id: int,
    util: float,
    crit: str,
    period: int,
    deadline_equal_period: bool,
    crit_levels_map: Dict[str, int],
    max_levels: int,
    wcet_scale: List[float],
    replica_mean: float,
    replica_max: int,
) -> dict:
    crit_level = crit_levels_map[crit]
    base_wcet = max(1.0, util * period)

    float_wcet = [0.0] * max_levels
    float_wcet[crit_level] = base_wcet

    for l in range(crit_level - 1, -1, -1):
        factor = wcet_scale[l + 1] if l + 1 < len(wcet_scale) else wcet_scale[-1]
        float_wcet[l] = float_wcet[l + 1] / factor if factor > 0 else float_wcet[l + 1]

    for l in range(crit_level + 1, max_levels):
        float_wcet[l] = float_wcet[crit_level]

    wcet = [max(1, int(round(x))) for x in float_wcet]
    for l in range(1, max_levels):
        if wcet[l] < wcet[l - 1]:
            wcet[l] = wcet[l - 1]

    replicas = sample_replicas(replica_mean, replica_max)
    replicas = max(0, min(replica_max, int(replicas)))

    if deadline_equal_period:
        deadline = period
    else:
        deadline = max(1, int(round(random.uniform(0.7, 1.0) * period)))

    cap = max(1, min(period, deadline) - 1)  # at least 1 tick margin
    max_w = max(wcet)
    if max_w >= cap:
        scale_down = cap / max_w if max_w > 0 else 1.0
        wcet = [max(1, int(round(w * scale_down))) for w in wcet]
        for l in range(1, max_levels):
            if wcet[l] < wcet[l - 1]:
                wcet[l] = wcet[l - 1]

    return {
        "taskId": task_id,
        "name": f"Task_{task_id}",
        "period": int(period),
        "deadline": int(deadline),
        "criticality": crit,
        "wcet": wcet,
        "replicas": replicas,
    }


def generate_taskset(sys_cfg: dict, args: GenArgs) -> dict:
    crit_levels = list(sys_cfg["criticality_levels"]["levels"].keys())

    if args.num_tasks > sys_cfg["system"]["max_tasks"]:
        print(
            f"Error: requested {args.num_tasks} tasks exceeds system limit "
            f"({sys_cfg['system']['max_tasks']}).\n"
            "Please increase max_tasks in system_config.yaml or regenerate sys_config.h.",
            file=sys.stderr,
        )
        sys.exit(1)

    max_levels = sys_cfg["criticality_levels"]["max_levels"]
    crit_dist = (
        normalize_weights(args.crit_dist)
        if args.crit_dist
        else {k: 1.0 for k in crit_levels}
    )

    prelim_replicas = [
        sample_replicas(args.replica_mean, args.replica_max)
        for _ in range(args.num_tasks)
    ]
    total_instances = sum(1 + r for r in prelim_replicas)

    if args.total_util is not None:
        U_total_all = args.total_util
        U_total_primaries = U_total_all * (args.num_tasks / total_instances)
    else:
        U_total_all = args.avg_util * total_instances
        U_total_primaries = U_total_all * (args.num_tasks / total_instances)

    if args.util_sampler == "uunifast":
        utils = uunifast_discard(args.num_tasks, U_total_primaries)
    elif args.util_sampler == "normal":
        raw = [
            max(0.0001, random.gauss(args.avg_util, args.util_stddev))
            for _ in range(args.num_tasks)
        ]
        s = sum(raw)
        utils = [u / s * U_total_primaries for u in raw]
    else:
        raw = [random.random() * args.avg_util * 2 for _ in range(args.num_tasks)]
        s = sum(raw) or 1.0
        utils = [u / s * U_total_primaries for u in raw]

    tasks: List[dict] = []
    for i, (u, r) in enumerate(zip(utils, prelim_replicas), start=1):
        crit = sample_criticality(crit_dist)
        period = sample_period(
            args.period_min,
            args.period_max,
            args.period_mode,
            args.period_choices,
            args.enforce_harmonic,
        )

        t = generate_single_task(
            task_id=i,
            util=u,
            crit=crit,
            period=period,
            deadline_equal_period=args.deadline_equal_period,
            crit_levels_map=sys_cfg["criticality_levels"]["levels"],
            max_levels=max_levels,
            wcet_scale=args.wcet_scale,
            replica_mean=args.replica_mean,
            replica_max=args.replica_max,
        )
        t["replicas"] = r
        tasks.append(t)

    return {"tasks": tasks}


def sanity_check(taskset: dict, sys_cfg: dict) -> Tuple[bool, str]:
    tasks = taskset["tasks"]
    max_tasks_allowed = sys_cfg["system"].get("max_tasks", len(tasks))
    if len(tasks) > max_tasks_allowed:
        return False, f"Generated {len(tasks)} tasks > max_tasks {max_tasks_allowed}"

    levels = sys_cfg["criticality_levels"]["levels"]
    per_level_util = {lvl: 0.0 for lvl in levels.keys()}
    for t in tasks:
        for lvl, num in levels.items():
            task_crit_num = levels[t["criticality"]]
            if task_crit_num >= num:
                wcet_val = t["wcet"][num] if num < len(t["wcet"]) else t["wcet"][-1]
                per_level_util[lvl] += wcet_val / t["period"]
    num_procs = sys_cfg["system"]["num_processors"]
    cores_per_proc = sys_cfg["system"]["num_cores_per_processor"]
    total_cores = num_procs * cores_per_proc
    for lvl, util in per_level_util.items():
        if util > total_cores + 1e-9:
            return (
                False,
                f"Level {lvl} total utilization {util:.2f} exceeds total cores {total_cores}",
            )
    return True, "OK"


def print_summary(taskset: dict, sys_cfg: dict):
    tasks = taskset["tasks"]
    num_tasks = len(tasks)
    total_instances = sum(1 + t.get("replicas", 0) for t in tasks)
    crit_counts = {}
    util_sum_primary = 0.0
    util_sum_total = 0.0

    for t in tasks:
        crit_counts[t["criticality"]] = crit_counts.get(t["criticality"], 0) + 1
        crit_level_num = sys_cfg["criticality_levels"]["levels"][t["criticality"]]
        wcet = t["wcet"][crit_level_num]
        util_primary = wcet / t["period"]
        util_sum_primary += util_primary
        util_sum_total += util_primary * (1 + t.get("replicas", 0))

    avg_util_primary = util_sum_primary / num_tasks if num_tasks else 0.0
    avg_util_total = util_sum_total / total_instances if total_instances else 0.0

    print(
        f"Generated {num_tasks} tasks, {total_instances} total instances (primaries+replicas)."
    )
    print(f"Avg effective util (primaries only): {avg_util_primary:.3f}")
    print(f"Avg effective util (including replicas): {avg_util_total:.3f}")
    print(f"Total system utilization (primaries only): {util_sum_primary:.3f}")
    print(f"Total system utilization (including replicas): {util_sum_total:.3f}")

    print("Criticality distribution:")
    # Use canonical order from system config
    ordered_levels = list(sys_cfg["criticality_levels"]["levels"].keys())
    for level in ordered_levels:
        if level in crit_counts:
            print(f"  {level}: {crit_counts[level]}")


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Generate mixed-criticality tasksets (YAML)"
    )
    p.add_argument(
        "--config", default=DEFAULT_CONFIG, help="Path to system_config.yaml"
    )
    p.add_argument(
        "--num-tasks", type=int, default=30, help="Number of tasks to generate"
    )
    p.add_argument(
        "--avg-util",
        type=float,
        default=0.2,
        help="Average utilization per task (if total-util unset)",
    )
    p.add_argument(
        "--total-util",
        type=float,
        default=None,
        help="Override: total utilization across tasks",
    )
    p.add_argument(
        "--replica-mean", type=float, default=1.0, help="Mean replicas per task"
    )
    p.add_argument("--replica-max", type=int, default=4, help="Max replicas per task")
    p.add_argument("--seed", type=int, default=None, help="Random seed")
    p.add_argument("--output", default=DEFAULT_OUTPUT, help="Output tasks YAML path")
    p.add_argument(
        "--crit-dist",
        default=None,
        help="Criticality distribution as JSON string or path to JSON. E.g. '{\"ASIL_D\":0.2,...}'",
    )
    p.add_argument("--period-min", type=int, default=10, help="Minimum period (ms)")
    p.add_argument("--period-max", type=int, default=200, help="Maximum period (ms)")
    p.add_argument(
        "--period-mode",
        choices=("uniform", "loguniform"),
        default="loguniform",
        help="How to sample periods",
    )
    p.add_argument(
        "--period-choices", default=None, help="Optional JSON list of period choices"
    )
    p.add_argument(
        "--enforce-harmonic",
        action="store_true",
        help="Force sampled periods to be harmonic or divisor/multiple-related (avoid coprime periods)",
    )
    p.add_argument(
        "--wcet-scale",
        default=None,
        help="JSON list of WCET scale factors low->high. Default: [1.0,1.2,1.5,2.0,3.0]",
    )
    p.add_argument(
        "--util-sampler", choices=("uunifast", "normal", "uniform"), default="uunifast"
    )
    p.add_argument(
        "--util-stddev", type=float, default=0.05, help="Stddev for normal util sampler"
    )
    p.add_argument(
        "--num-sets", type=int, default=1, help="Generate N sets for batch experiments"
    )
    p.add_argument(
        "--prefix", type=str, default="tasks", help="Prefix for batch output files"
    )
    p.add_argument(
        "--mode",
        choices=("balanced", "lowpower", "redundant"),
        default="balanced",
        help="Generation mode hint (affects distribution heuristics)",
    )
    p.add_argument(
        "--summary", action="store_true", help="Print summary after generation"
    )
    p.add_argument(
        "--deadline-equal-period", action="store_true", help="Set deadline == period"
    )
    return p


def parse_crit_dist(arg: str | None) -> Dict[str, float] | None:
    if not arg:
        return None
    try:
        data = json.loads(arg)
        if isinstance(data, dict):
            return {str(k): float(v) for k, v in data.items()}
    except Exception:
        pass
    if os.path.isfile(arg):
        with open(arg, "r") as f:
            data = json.load(f)
            return {str(k): float(v) for k, v in data.items()}
    raise ValueError("crit-dist must be JSON map or path to JSON file")


def main(argv=None):
    argv = argv if argv is not None else sys.argv[1:]
    parser = build_arg_parser()
    ns = parser.parse_args(argv)

    args = (
        GenArgs(
            config=ns.config,
            num_tasks=ns.num_tasks,
            avg_util=ns.avg_util,
            total_util=ns.total_util,
            replica_mean=ns.replica_mean,
            replica_max=ns.replica_max,
            seed=ns.seed,
            output=ns.output,
            crit_dist=parse_crit_dist(ns.crit_dist),
            period_min=ns.period_min,
            period_max=ns.period_max,
            period_mode=ns.period_mode,
            period_choices=json.loads(ns.period_choices) if ns.period_choices else None,
            enforce_harmonic=ns.enforce_harmonic,
            wcet_scale=(
                json.loads(ns.wcet_scale) if ns.wcet_scale else DEFAULT_WCET_SCALE
            ),
            util_sampler=ns.util_sampler,
            util_stddev=ns.util_stddev,
            num_sets=ns.num_sets,
            prefix=ns.prefix,
            mode=ns.mode,
            summary=ns.summary,
            deadline_equal_period=ns.deadline_equal_period,
            output_path=ns.output if hasattr(ns, "output") else DEFAULT_OUTPUT,
        )
        if False
        else None
    )

    args = GenArgs(
        config=ns.config,
        num_tasks=ns.num_tasks,
        avg_util=ns.avg_util,
        total_util=ns.total_util,
        replica_mean=ns.replica_mean,
        replica_max=ns.replica_max,
        seed=ns.seed,
        output=ns.output,
        crit_dist=parse_crit_dist(ns.crit_dist),
        period_min=ns.period_min,
        period_max=ns.period_max,
        period_mode=ns.period_mode,
        period_choices=json.loads(ns.period_choices) if ns.period_choices else None,
        enforce_harmonic=ns.enforce_harmonic,
        wcet_scale=json.loads(ns.wcet_scale) if ns.wcet_scale else DEFAULT_WCET_SCALE,
        util_sampler=ns.util_sampler,
        util_stddev=ns.util_stddev,
        num_sets=ns.num_sets,
        prefix=ns.prefix,
        mode=ns.mode,
        summary=ns.summary,
        deadline_equal_period=ns.deadline_equal_period,
    )

    if args.seed is None:
        args.seed = int(time.time())
    random.seed(args.seed)

    if not os.path.isfile(args.config):
        print(f"System config not found: {args.config}", file=sys.stderr)
        sys.exit(2)
    sys_cfg = load_system_config(args.config)

    if args.crit_dist is None:
        levels = sys_cfg["criticality_levels"]["levels"]
        args.crit_dist = {k: 1.0 / (1 + v) for k, v in levels.items()}

    for set_idx in range(1, args.num_sets + 1):
        taskset = generate_taskset(sys_cfg, args)

        ok, message = sanity_check(taskset, sys_cfg)
        if not ok:
            print(f"Sanity check failed: {message}", file=sys.stderr)
            metadata = {
                "seed": args.seed,
                "num_tasks": args.num_tasks,
                "avg_util": args.avg_util,
                "total_util": args.total_util,
                "warning": message,
            }
        else:
            metadata = {
                "seed": args.seed,
                "num_tasks": args.num_tasks,
                "avg_util": args.avg_util,
                "total_util": args.total_util,
                "mode": args.mode,
            }

        if args.num_sets == 1:
            outpath = args.output
        else:
            base, ext = os.path.splitext(args.output)
            outpath = f"{base}_{args.prefix}_{set_idx:03d}{ext or '.yaml'}"

        write_yaml_with_metadata(taskset, outpath, metadata)

        if args.summary:
            print(f"\nOutput written to: {outpath}")
            print_summary(taskset, sys_cfg)

    print("Generation complete.")


if __name__ == "__main__":
    main()
