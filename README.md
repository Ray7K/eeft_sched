# EEFT-Sched: A Mixed-Criticality Scheduler Simulator

EEFT-Sched is a multi-core, multi-processor mixed-criticality scheduling
simulator designed for research and thesis work. It implements power management,
job migration, task replication, quality of service, and includes Python tooling
for configuration, task generation, offline allocation, and log analysis.

## Features

- **Mixed-Criticality Scheduling** with mode changes
- **Multi-Processor, Multi-Core Simulation**
- **Distributed Execution Model:** Each processor is simulated as a separate OS
  process, with each core implemented as a thread within that process
- **Dynamic Power Management (DPM)**: idle cores enter sleep states
- **Dynamic Voltage and Frequency Scaling (DVFS)** based on available slack
- **Intra-Processor Job Migration** for load consolidation and longer DPM intervals
- **Quality of Service (QoS)** for low-criticality jobs
- **Active Task Replication** for fault tolerance
- **Distributed Inter-Processor Communication** for propagating completion events
  and criticality changes between processors using multicast
- **Offline Tooling**:
  - system configuration generation
  - taskset generation
  - offline task allocation
  - log parsing and visualization
- **Custom Test Framework**

## Requirements

### C Build Requirements

- **Clang** (default) or GCC with C11
- **Make**
- **POSIX environment** (Linux/macOS)

### Python Tooling Requirements

- **Python ≥ 3.9**
- **uv** (recommended) or pip

## Python Environment Setup

It is recommended to use **uv** for managing the Python environment and
installing dependencies.

### Using uv (recommended)

Create the environment and install all dependencies declared in `pyproject.toml`:

```bash
uv sync
```

**Optional:** activate the environment manually:

```bash
source .venv/bin/activate
```

Run tools through uv:

```bash
uv run python tools/system_config_generator.py
```

### Using pip (alternative)

If you prefer a traditional virtual environment:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install .
```

Both workflows are supported, but uv is recommended for reproducibility and speed.

## Setup and Configuration

The simulator configuration is generated using the Python tools in `tools/`.

### 1. Generate System Configuration

```bash
python3 tools/system_config_generator.py
```

Generates: `config/system_config.yaml`.

### 2. Generate Taskset

```bash
python3 tools/taskset_generator.py
```

Generates: `config/tasks.yaml`.

### 3. Offline Task Allocation

```bash
python3 tools/task_allocator.py
```

Generates:

- `include/sys_config.h`
- `src/task_alloc.c`
- allocation reports in `target/reports/`

These C files are produced automatically and should not be edited manually.

## Building

The Makefile defines three build profiles: **debug**, **release**, **profile**.

Build all:

```bash
make
```

Specific profile:

```bash
make debug
make release
make profile
```

Clean:

```bash
make clean
```

### Build Options

- `V=1` — verbose build
- `ASAN=1`, `TSAN=1`, `UBSAN=1` — enable specific sanitizers
- `STRICT=1` — enable `-Werror`, `-pedantic`, `-Wconversion`
- `TICKS=5000` — override simulation length, default is 1000 ticks

Example:

```bash
make debug ASAN=1 TICKS=20000
```

## Running the Simulator

Debug:

```bash
make run
```

Release:

```bash
make run-release
```

Profile:

```bash
make run-profile
```

Logs are written to `target/logs/`.

## Testing

Tests are compiled into standalone binaries for each build profile.

Run tests in debug mode:

```bash
make test
```

Release tests:

```bash
make test-release
```

Profile tests:

```bash
make test-profile
```

### Test Filtering

The test runner accepts an optional filter string. Only tests whose names contain
the filter substring will be executed.

Pass the filter using the `ARGS` variable:

```bash
make test ARGS="alloc"
```

Tests execute through the built-in lightweight test runner.

## Log Analysis

After running the simulator, analyze results with:

```bash
python3 tools/log_parser.py
```

This produces plots and reports in `target/reports/`, including:

- core utilization timelines
- DVFS / DPM behavior
- task event traces
- migration summaries
- system-level power proxies

## License

This project is licensed under the MIT License.
See `LICENSE` for details.
