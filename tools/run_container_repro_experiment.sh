#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
DEFAULT_REPROBUILD_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd -P)
DEFAULT_C_BUILD_ROOT="/home/sprooc/c_build"

PROJECT_ROOT=""
RESULT_DIR=""
REPROBUILD_ROOT="${DEFAULT_REPROBUILD_ROOT}"
C_BUILD_ROOT="${DEFAULT_C_BUILD_ROOT}"
C_BUILD_OUTPUT=""
CONTAINER_NAME=""
CLEAN_CMD="make clean"
RUN_CLEAN=1
SKIP_TRACK=0
SKIP_DEBUG=0
SKIP_RENDER=0
RUN_RENDERED=0
BUILD_CMD=()

usage() {
  cat <<'EOF'
Usage: run_container_repro_experiment.sh --project=PATH [options] [-- build command...]

Default build command: make

Options:
  --project=PATH           Benchmark project root to evaluate
  --result-dir=PATH        Result directory
  --reprobuild-root=PATH   Path to this reprobuild repository
  --c-build-root=PATH      Path to the c_build repository
  --c-build-output=PATH    Output directory for c_build generated files
  --container-name=NAME    Override c_build container name
  --clean-cmd='CMD'        Command run before tracked build, default: make clean
  --no-clean               Skip host-side clean step
  --skip-track             Reuse existing build_record.yaml/build_graph.yaml
  --skip-debug             Skip c_build debug-mode container verification
  --skip-render            Skip c_build render-only Dockerfile/build.sh generation
  --run-rendered           Run generated build.sh after render step
  -h, --help               Show this help message

Examples:
  tools/run_container_repro_experiment.sh --project=/path/to/proj
  tools/run_container_repro_experiment.sh --project=/path/to/proj -- make -j8
  tools/run_container_repro_experiment.sh --project=/path/to/proj -- bash -lc 'cmake -S . -B build && cmake --build build'
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

info() {
  echo "==> $*"
}

warn() {
  echo "WARN: $*" >&2
}

canonicalize_dir() {
  local dir="$1"
  [[ -d "${dir}" ]] || die "Directory does not exist: ${dir}"
  (cd "${dir}" && pwd -P)
}

detect_container_name() {
  local docker_config="${C_BUILD_ROOT}/internal/config/docker.go"
  if [[ ! -f "${docker_config}" ]]; then
    return 1
  fi

  sed -n 's/^[[:space:]]*var ContainerName string = "\(.*\)".*/\1/p' \
    "${docker_config}" | head -n1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --project=*)
      PROJECT_ROOT="${1#*=}"
      ;;
    --result-dir=*)
      RESULT_DIR="${1#*=}"
      ;;
    --reprobuild-root=*)
      REPROBUILD_ROOT="${1#*=}"
      ;;
    --c-build-root=*)
      C_BUILD_ROOT="${1#*=}"
      ;;
    --c-build-output=*)
      C_BUILD_OUTPUT="${1#*=}"
      ;;
    --container-name=*)
      CONTAINER_NAME="${1#*=}"
      ;;
    --clean-cmd=*)
      CLEAN_CMD="${1#*=}"
      ;;
    --no-clean)
      RUN_CLEAN=0
      ;;
    --skip-track)
      SKIP_TRACK=1
      ;;
    --skip-debug)
      SKIP_DEBUG=1
      ;;
    --skip-render)
      SKIP_RENDER=1
      ;;
    --run-rendered)
      RUN_RENDERED=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      BUILD_CMD=("$@")
      break
      ;;
    *)
      die "Unknown argument: $1"
      ;;
  esac
  shift
done

[[ -n "${PROJECT_ROOT}" ]] || die "--project is required"

PROJECT_ROOT=$(canonicalize_dir "${PROJECT_ROOT}")
REPROBUILD_ROOT=$(canonicalize_dir "${REPROBUILD_ROOT}")
C_BUILD_ROOT=$(canonicalize_dir "${C_BUILD_ROOT}")

if [[ ${#BUILD_CMD[@]} -eq 0 ]]; then
  BUILD_CMD=(make)
fi

if [[ -z "${RESULT_DIR}" ]]; then
  RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)
  RESULT_DIR="${PROJECT_ROOT}/.repro_experiments/${RUN_ID}"
fi
mkdir -p "${RESULT_DIR}"
RESULT_DIR=$(canonicalize_dir "${RESULT_DIR}")

if [[ -z "${C_BUILD_OUTPUT}" ]]; then
  C_BUILD_OUTPUT="${RESULT_DIR}/c_build"
fi

TRACKER_LOG_DIR="${RESULT_DIR}/tracker_logs"
HOST_SNAPSHOT_DIR="${RESULT_DIR}/host"
LOG_DIR="${RESULT_DIR}/logs"
mkdir -p "${TRACKER_LOG_DIR}" "${HOST_SNAPSHOT_DIR}" "${LOG_DIR}" "${C_BUILD_OUTPUT}"

if [[ -z "${CONTAINER_NAME}" ]]; then
  CONTAINER_NAME=$(detect_container_name || true)
fi
[[ -n "${CONTAINER_NAME}" ]] || die "Failed to detect c_build container name"

REPROBUILD_BIN="${REPROBUILD_ROOT}/build/reprobuild"
PROJECT_RECORD_PATH="${PROJECT_ROOT}/build_record.yaml"
PROJECT_GRAPH_PATH="${PROJECT_ROOT}/build_graph.yaml"
SNAPSHOT_RECORD_PATH="${HOST_SNAPSHOT_DIR}/build_record.yaml"
SNAPSHOT_GRAPH_PATH="${HOST_SNAPSHOT_DIR}/build_graph.yaml"
C_BUILD_RECORD_PATH="${C_BUILD_OUTPUT}/build_record.yaml"
C_BUILD_GRAPH_PATH="${C_BUILD_OUTPUT}/build_graph.yaml"

[[ -d "${PROJECT_ROOT}" ]] || die "Project root does not exist: ${PROJECT_ROOT}"
[[ -f "${C_BUILD_ROOT}/cmd/c_build/main.go" ]] || die "Invalid c_build root: ${C_BUILD_ROOT}"

command -v docker >/dev/null 2>&1 || die "docker is required"
command -v go >/dev/null 2>&1 || die "go is required"
[[ -x /usr/bin/bpftrace0.24 ]] || die "Missing /usr/bin/bpftrace0.24. Run 'make install' in reprobuild first."

if [[ ! -x "${REPROBUILD_BIN}" ]]; then
  info "Building reprobuild binary"
  make -C "${REPROBUILD_ROOT}"
fi
[[ -x "${REPROBUILD_BIN}" ]] || die "Missing reprobuild binary: ${REPROBUILD_BIN}"

if [[ " ${BUILD_CMD[*]} " != *" make "* ]]; then
  warn "c_build currently runs 'make clean' internally. Non-make projects may need extra manual adaptation."
fi

ORIG_DIR="${RESULT_DIR}/original_project_files"
mkdir -p "${ORIG_DIR}"
ORIG_RECORD_EXISTS=0
ORIG_GRAPH_EXISTS=0
TOUCHED_PROJECT_FILES=0

write_summary() {
  local exit_status="${1:-0}"
  cat > "${RESULT_DIR}/summary.txt" <<EOF
project_root=${PROJECT_ROOT}
build_command=${BUILD_CMD[*]}
reprobuild_root=${REPROBUILD_ROOT}
c_build_root=${C_BUILD_ROOT}
container_name=${CONTAINER_NAME}
result_dir=${RESULT_DIR}
host_build_record_snapshot=${SNAPSHOT_RECORD_PATH}
host_build_graph_snapshot=${SNAPSHOT_GRAPH_PATH}
c_build_output=${C_BUILD_OUTPUT}
c_build_build_record_snapshot=${C_BUILD_RECORD_PATH}
c_build_build_graph_snapshot=${C_BUILD_GRAPH_PATH}
logs_dir=${LOG_DIR}
rendered_build_sh=${C_BUILD_OUTPUT}/build.sh
rendered_dockerfile=${C_BUILD_OUTPUT}/Dockerfile
exit_status=${exit_status}
EOF
}

cleanup() {
  if [[ "${TOUCHED_PROJECT_FILES}" -eq 1 ]]; then
    if [[ "${ORIG_RECORD_EXISTS}" -eq 1 ]]; then
      cp -a "${ORIG_DIR}/build_record.yaml" "${PROJECT_RECORD_PATH}"
    fi

    if [[ "${ORIG_GRAPH_EXISTS}" -eq 1 ]]; then
      cp -a "${ORIG_DIR}/build_graph.yaml" "${PROJECT_GRAPH_PATH}"
    fi
  fi
}
trap 'status=$?; write_summary "${status}"; cleanup' EXIT

if [[ "${SKIP_TRACK}" -eq 0 ]]; then
  if [[ -e "${PROJECT_RECORD_PATH}" ]]; then
    cp -a "${PROJECT_RECORD_PATH}" "${ORIG_DIR}/build_record.yaml"
    ORIG_RECORD_EXISTS=1
  fi
  if [[ -e "${PROJECT_GRAPH_PATH}" ]]; then
    cp -a "${PROJECT_GRAPH_PATH}" "${ORIG_DIR}/build_graph.yaml"
    ORIG_GRAPH_EXISTS=1
  fi
  TOUCHED_PROJECT_FILES=1
fi

if [[ "${SKIP_TRACK}" -eq 0 ]]; then
  if [[ "${RUN_CLEAN}" -eq 1 ]]; then
    info "Cleaning project before tracked build"
    (
      cd "${PROJECT_ROOT}"
      bash -lc "${CLEAN_CMD}"
    ) 2>&1 | tee "${LOG_DIR}/00_clean_host.log"
  fi

  info "Running reprobuild tracked build"
  (
    cd "${PROJECT_ROOT}"
    "${REPROBUILD_BIN}" \
      --no-upload \
      --logdir "${TRACKER_LOG_DIR}" \
      --output "${PROJECT_RECORD_PATH}" \
      --graph="${PROJECT_GRAPH_PATH}" \
      -- \
      "${BUILD_CMD[@]}"
  ) 2>&1 | tee "${LOG_DIR}/01_reprobuild_track.log"

  cp -a "${PROJECT_RECORD_PATH}" "${SNAPSHOT_RECORD_PATH}"
  cp -a "${PROJECT_GRAPH_PATH}" "${SNAPSHOT_GRAPH_PATH}"
else
  [[ -f "${PROJECT_RECORD_PATH}" ]] || die "Missing ${PROJECT_RECORD_PATH}; cannot --skip-track"
  [[ -f "${PROJECT_GRAPH_PATH}" ]] || die "Missing ${PROJECT_GRAPH_PATH}; cannot --skip-track"
  cp -a "${PROJECT_RECORD_PATH}" "${SNAPSHOT_RECORD_PATH}"
  cp -a "${PROJECT_GRAPH_PATH}" "${SNAPSHOT_GRAPH_PATH}"
fi

if grep -q 'origin: custom' "${PROJECT_RECORD_PATH}"; then
  warn "Detected custom dependencies in build_record.yaml. Make sure the benchmark project exposes them to c_build."
fi

if [[ "${SKIP_DEBUG}" -eq 0 ]]; then
  info "Running c_build debug-mode verification"
  docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true

  set +e
  (
    cd "${C_BUILD_ROOT}"
    go run cmd/c_build/*.go \
      -c \
      -d \
      "--input=${PROJECT_RECORD_PATH}" \
      "--output=${C_BUILD_OUTPUT}" \
      "--ld_preload=${REPROBUILD_ROOT}"
  ) 2>&1 | tee "${LOG_DIR}/02_c_build_debug.log"
  debug_status=${PIPESTATUS[0]}
  set -e

  if [[ -f "${PROJECT_RECORD_PATH}" ]]; then
    cp -a "${PROJECT_RECORD_PATH}" "${C_BUILD_RECORD_PATH}"
  else
    warn "c_build debug run did not leave ${PROJECT_RECORD_PATH}; skipping c_build build_record snapshot"
  fi

  if [[ -f "${PROJECT_GRAPH_PATH}" ]]; then
    cp -a "${PROJECT_GRAPH_PATH}" "${C_BUILD_GRAPH_PATH}"
  else
    warn "c_build debug run did not leave ${PROJECT_GRAPH_PATH}; skipping c_build build_graph snapshot"
  fi

  if [[ "${debug_status}" -ne 0 ]]; then
    warn "c_build debug-mode verification failed with exit code ${debug_status}; snapshots were copied before exit"
    exit "${debug_status}"
  fi
fi

if [[ "${SKIP_RENDER}" -eq 0 ]]; then
  if [[ ! -f "${C_BUILD_OUTPUT}/digest.yaml" ]]; then
    die "Missing ${C_BUILD_OUTPUT}/digest.yaml. Run debug mode first or point --c-build-output to an existing c_build output directory."
  fi

  info "Rendering Dockerfile and build.sh with c_build"
  (
    cd "${C_BUILD_ROOT}"
    go run cmd/c_build/*.go \
      "--input=${PROJECT_RECORD_PATH}" \
      "--output=${C_BUILD_OUTPUT}" \
      "--ld_preload=${REPROBUILD_ROOT}"
  ) 2>&1 | tee "${LOG_DIR}/03_c_build_render.log"

  if [[ -f "${C_BUILD_OUTPUT}/build.sh" ]]; then
    chmod +x "${C_BUILD_OUTPUT}/build.sh"
  fi
fi

if [[ "${RUN_RENDERED}" -eq 1 ]]; then
  [[ -x "${C_BUILD_OUTPUT}/build.sh" ]] || die "Missing executable build.sh in ${C_BUILD_OUTPUT}"
  info "Running rendered build.sh"
  (
    cd "${C_BUILD_OUTPUT}"
    ./build.sh \
      "--proj_root=${PROJECT_ROOT}" \
      "--reprobuild_path=${REPROBUILD_ROOT}"
  ) 2>&1 | tee "${LOG_DIR}/04_rendered_build.log"
fi

info "Experiment finished"
info "Summary: ${RESULT_DIR}/summary.txt"
