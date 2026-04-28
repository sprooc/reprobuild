# Experiments

## Environment Setup

```bash
tools/setup_experiment_env.sh
```

This script will:

- install required system packages with `apt`
- clone the benchmark projects into `projs/`
- clone `c_build` into `./c_build`
- build `reprobuild`
- install `/usr/bin/bpftrace0.24` if it is missing

Useful variants:

```bash
# Skip package installation
tools/setup_experiment_env.sh --skip-packages

# Try to fast-forward existing repositories
tools/setup_experiment_env.sh --update-existing
```

## Run Experiments

Run the full workflow with:

```bash
tools/bootstrap_and_run_experiment.sh
```

This will prepare the environment if needed, then run the batch experiment runner.

Run only selected projects:

```bash
tools/bootstrap_and_run_experiment.sh --only=tiny-AES-c,zlib,lz4
```

Reuse an already-prepared environment:

```bash
tools/bootstrap_and_run_experiment.sh --skip-setup
```

Run the batch runner directly:

```bash
python3 tools/reprobuild_eval_runner.py \
  --root "$(pwd)" \
  --c-build-root "$(pwd)/c_build" \
  --result-root "$(pwd)/results" \
  --run-id "eval_$(date -u +%Y%m%dT%H%M%SZ)"
```

Results are written under:

```bash
results/<RUN_ID>/
```

The console log is written to:

```bash
results/<RUN_ID>.console.log
```

## Experiment Procedure

The batch experiment runs each selected benchmark with the following steps:

1. clean the project tree
2. run a normal host build and record its timing
3. run a tracked host build with `reprobuild` to generate `build_record.yaml` and `build_graph.yaml`
4. invoke `c_build` to perform the container-side rebuild workflow
5. collect logs, analysis output, and per-project summaries under `results/<RUN_ID>/`

At the end of the run, the batch runner writes:

- `results/<RUN_ID>/summary.json`
- `results/<RUN_ID>/report.md`
