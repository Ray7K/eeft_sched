# pyright: basic

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path

import yaml


def load_crit_map(arg):
    # allow inline JSON or file path
    try:
        if Path(arg).exists():
            with open(arg, "r") as f:
                data = yaml.safe_load(f)
        else:
            data = json.loads(arg)
        if not isinstance(data, dict):
            raise ValueError("Criticality map must be a JSON object")
        return data
    except Exception as e:
        raise argparse.ArgumentTypeError(f"Invalid criticality map: {e}")


def generate_system_config(
    num_processors, num_cores, max_tasks, crit_levels, crit_map, output
):
    config = {
        "system": {
            "num_processors": num_processors,
            "num_cores_per_processor": num_cores,
            "max_tasks": max_tasks,
        },
        "criticality_levels": {
            "max_levels": crit_levels,
            "levels": crit_map,
        },
    }

    header = [
        f"# Generated: {datetime.now(timezone.utc).isoformat()}Z",
        f"# generator: system_config_generator.py",
        "",
    ]

    with open(output, "w") as f:
        for line in header:
            f.write(line + "\n")
        yaml.safe_dump(config, f, sort_keys=False)

    print(f"System config written to: {output}")


def main():
    parser = argparse.ArgumentParser(
        description="Generate a standardized system_config.yaml for mixed-criticality schedulers"
    )
    parser.add_argument(
        "--num-processors",
        type=int,
        default=5,
        help="Number of processors in the system",
    )
    parser.add_argument(
        "--num-cores-per-processor",
        type=int,
        default=4,
        help="Number of cores per processor",
    )
    parser.add_argument(
        "--max-tasks",
        type=int,
        default=50,
        help="Maximum number of unique tasks allowed",
    )
    parser.add_argument(
        "--criticality-levels",
        type=int,
        default=5,
        help="Maximum number of criticality levels",
    )
    parser.add_argument(
        "--criticality-map",
        type=load_crit_map,
        default='{"ASIL_D":4,"ASIL_C":3,"ASIL_B":2,"ASIL_A":1,"QM":0}',
        help="JSON string or path to YAML defining criticality-level mapping",
    )
    parser.add_argument(
        "--output",
        type=str,
        default="config/system_config.yaml",
        help="Output file path",
    )

    args = parser.parse_args()

    generate_system_config(
        num_processors=args.num_processors,
        num_cores=args.num_cores_per_processor,
        max_tasks=args.max_tasks,
        crit_levels=args.criticality_levels,
        crit_map=args.criticality_map,
        output=args.output,
    )


if __name__ == "__main__":
    main()
