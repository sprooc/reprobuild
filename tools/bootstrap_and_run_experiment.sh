#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
REPROBUILD_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd -P)
DEFAULT_C_BUILD_ROOT="${REPROBUILD_ROOT}/c_build"
RESULT_ROOT="${REPROBUILD_ROOT}/results"
C_BUILD_ROOT="${DEFAULT_C_BUILD_ROOT}"
RUN_ID=""
ONLY_PROJECTS=""
SKIP_SETUP=0
SETUP_ARGS=()

usage() {
  cat <<'EOF'
Usage: tools/bootstrap_and_run_experiment.sh [options]

One-shot entrypoint for a fresh machine:
  1. prepare packages/repos/build tools
  2. run the batch experiment runner locally

Options:
  --run-id=ID            Override run id
  --only=a,b,c           Run only selected projects
  --result-root=PATH     Where to save results
  --c-build-root=PATH    Where to clone/use c_build
  --skip-setup           Do not run setup_experiment_env.sh
  --update-existing      Forwarded to setup script
  --skip-packages        Forwarded to setup script
  --skip-bpftrace        Forwarded to setup script
  --skip-build           Forwarded to setup script
  --skip-projects        Forwarded to setup script
  --skip-c-build         Forwarded to setup script
  -h, --help             Show this help
EOF
}

info() {
  echo "==> $*"
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --run-id=*)
      RUN_ID="${1#*=}"
      ;;
    --only=*)
      ONLY_PROJECTS="${1#*=}"
      ;;
    --result-root=*)
      RESULT_ROOT="${1#*=}"
      ;;
    --c-build-root=*)
      C_BUILD_ROOT="${1#*=}"
      SETUP_ARGS+=("$1")
      ;;
    --skip-setup)
      SKIP_SETUP=1
      ;;
    --update-existing|--skip-packages|--skip-bpftrace|--skip-build|--skip-projects|--skip-c-build)
      SETUP_ARGS+=("$1")
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "Unknown argument: $1"
      ;;
  esac
  shift
done

RESULT_ROOT=$(mkdir -p "${RESULT_ROOT}" && cd "${RESULT_ROOT}" && pwd -P)
C_BUILD_ROOT=$(mkdir -p "$(dirname "${C_BUILD_ROOT}")" && cd "$(dirname "${C_BUILD_ROOT}")" && pwd -P)/"$(basename "${C_BUILD_ROOT}")"

if [[ -z "${RUN_ID}" ]]; then
  RUN_ID=$(date -u +eval_%Y%m%dT%H%M%SZ)
fi

LOG_PATH="${RESULT_ROOT}/${RUN_ID}.console.log"

info "reprobuild root: ${REPROBUILD_ROOT}"
info "c_build root: ${C_BUILD_ROOT}"
info "result root: ${RESULT_ROOT}"
info "run id: ${RUN_ID}"
info "console log: ${LOG_PATH}"

if [[ "${SKIP_SETUP}" -ne 1 ]]; then
  info "Preparing experiment environment"
  "${REPROBUILD_ROOT}/tools/setup_experiment_env.sh" "${SETUP_ARGS[@]}"
fi

[[ -f "${REPROBUILD_ROOT}/tools/reprobuild_eval_runner.py" ]] || die "Missing runner: ${REPROBUILD_ROOT}/tools/reprobuild_eval_runner.py"

info "Validating runner syntax"
python3 -m py_compile "${REPROBUILD_ROOT}/tools/reprobuild_eval_runner.py"

RUNNER_ARGS=(
  python3
  "${REPROBUILD_ROOT}/tools/reprobuild_eval_runner.py"
  --root "${REPROBUILD_ROOT}"
  --c-build-root "${C_BUILD_ROOT}"
  --result-root "${RESULT_ROOT}"
  --run-id "${RUN_ID}"
)

if [[ -n "${ONLY_PROJECTS}" ]]; then
  RUNNER_ARGS+=(--only "${ONLY_PROJECTS}")
fi

info "Starting experiment runner"
"${RUNNER_ARGS[@]}" 2>&1 | tee "${LOG_PATH}"

info "Experiment finished"
info "Result directory: ${RESULT_ROOT}/${RUN_ID}"
info "Console log: ${LOG_PATH}"
