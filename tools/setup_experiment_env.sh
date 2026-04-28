#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
REPROBUILD_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd -P)
DEFAULT_C_BUILD_ROOT="${REPROBUILD_ROOT}/c_build"
PROJS_DIR="${REPROBUILD_ROOT}/projs"
C_BUILD_ROOT="${DEFAULT_C_BUILD_ROOT}"
INSTALL_PACKAGES=1
INSTALL_BPFTRACE=1
BUILD_REPROBUILD=1
SYNC_PROJECTS=1
SYNC_C_BUILD=1
UPDATE_EXISTING=0

PROJECT_SPECS=(
  "tiny-AES-c|https://github.com/kokke/tiny-AES-c.git"
  "zlib|https://github.com/madler/zlib.git"
  "lz4|https://github.com/lz4/lz4.git"
  "lua|https://github.com/lua/lua.git"
  "libtommath|https://github.com/libtom/libtommath.git"
  "busybox|https://github.com/mirror/busybox"
  "e2fsprogs|https://git.kernel.org/pub/scm/fs/ext2/e2fsprogs.git"
  "openssl|https://github.com/openssl/openssl.git"
)

APT_PACKAGES=(
  build-essential
  libgtest-dev
  libyaml-cpp-dev
  make
  perl
  pkg-config
  python3
)

usage() {
  cat <<'EOF'
Usage: tools/setup_experiment_env.sh [options]

Prepare a fresh machine for reprobuild experiments:
  1. install system packages
  2. clone/update benchmark projects under projs/
  3. clone/update c_build inside this repository
  4. build reprobuild and install /usr/bin/bpftrace0.24

Options:
  --c-build-root=PATH   Where to clone/use c_build
  --projs-dir=PATH      Where to clone/use benchmark projects
  --skip-packages       Skip apt dependency installation
  --skip-bpftrace       Skip 'make install' for /usr/bin/bpftrace0.24
  --skip-build          Skip building reprobuild
  --skip-projects       Skip syncing benchmark projects
  --skip-c-build        Skip syncing c_build
  --update-existing     git pull existing repos when clean
  -h, --help            Show this help
EOF
}

info() {
  echo "==> $*"
}

warn() {
  echo "WARN: $*" >&2
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

canonicalize_parent() {
  local path="$1"
  mkdir -p "$(dirname "${path}")"
  if [[ -e "${path}" ]]; then
    (cd "${path}" && pwd -P)
  else
    local parent
    parent=$(cd "$(dirname "${path}")" && pwd -P)
    printf '%s/%s\n' "${parent}" "$(basename "${path}")"
  fi
}

backup_existing_path() {
  local path="$1"
  local stamp backup_dir
  stamp=$(date -u +%Y%m%dT%H%M%SZ)
  backup_dir="$(dirname "${path}")/.bootstrap_backups/${stamp}"
  mkdir -p "${backup_dir}"
  info "Backing up existing path $(basename "${path}") to ${backup_dir}"
  mv "${path}" "${backup_dir}/"
}

clone_repo() {
  local url="$1"
  local dest="$2"
  info "Cloning ${url} -> ${dest}"
  git clone "${url}" "${dest}"
}

update_repo_if_safe() {
  local dest="$1"
  if [[ "${UPDATE_EXISTING}" -ne 1 ]]; then
    info "Keeping existing repo: ${dest}"
    return 0
  fi

  local status branch
  status=$(git -C "${dest}" status --short)
  if [[ -n "${status}" ]]; then
    warn "Skipping update for dirty repo: ${dest}"
    return 0
  fi

  branch=$(git -C "${dest}" symbolic-ref --quiet --short HEAD || true)
  if [[ -z "${branch}" ]]; then
    warn "Skipping update for detached HEAD repo: ${dest}"
    return 0
  fi

  info "Updating ${dest} on branch ${branch}"
  git -C "${dest}" pull --ff-only
}

ensure_repo() {
  local _name="$1"
  local url="$2"
  local dest="$3"
  local origin=""

  if [[ -d "${dest}/.git" ]]; then
    origin=$(git -C "${dest}" remote get-url origin || true)
    if [[ -n "${origin}" && "${origin}" != "${url}" ]]; then
      warn "Repo ${dest} has unexpected origin: ${origin}"
      return 1
    fi
    update_repo_if_safe "${dest}"
    return 0
  fi

  if [[ -e "${dest}" ]]; then
    if [[ -d "${dest}" ]]; then
      backup_existing_path "${dest}"
    else
      die "Path exists and is not a directory: ${dest}"
    fi
  fi

  clone_repo "${url}" "${dest}"
}

prepare_busybox() {
  local dest="$1"
  if [[ ! -f "${dest}/.config" ]]; then
    info "Initializing BusyBox default config"
    make -C "${dest}" defconfig
  fi
}

prepare_e2fsprogs() {
  local dest="$1"
  if [[ ! -f "${dest}/build/Makefile" ]]; then
    info "Configuring e2fsprogs out-of-tree build directory"
    mkdir -p "${dest}/build"
    (
      cd "${dest}/build"
      ../configure
    )
  fi
}

prepare_project_checkout() {
  local name="$1"
  local dest="$2"
  case "${name}" in
    busybox)
      prepare_busybox "${dest}"
      ;;
    e2fsprogs)
      prepare_e2fsprogs "${dest}"
      ;;
  esac
}

install_packages() {
  command -v apt-get >/dev/null 2>&1 || die "This setup script currently expects apt-get"
  info "Installing system packages"
  sudo apt-get update
  sudo apt-get install -y "${APT_PACKAGES[@]}"
}

install_bpftrace() {
  if [[ -x /usr/bin/bpftrace0.24 ]]; then
    info "Found /usr/bin/bpftrace0.24"
    return 0
  fi
  info "Installing /usr/bin/bpftrace0.24 via 'make install'"
  make -C "${REPROBUILD_ROOT}" install
}

build_reprobuild() {
  info "Building reprobuild"
  make -C "${REPROBUILD_ROOT}"
}

post_checks() {
  if command -v docker >/dev/null 2>&1; then
    if ! docker info >/dev/null 2>&1; then
      warn "docker is installed but current user cannot talk to the daemon yet"
      warn "You may need to start Docker and/or re-login after joining the docker group"
    fi
  else
    warn "docker command not found after setup"
  fi

  if [[ ! -x "${REPROBUILD_ROOT}/build/reprobuild" ]]; then
    warn "reprobuild binary is still missing: ${REPROBUILD_ROOT}/build/reprobuild"
  fi

  if [[ ! -x /usr/bin/bpftrace0.24 ]]; then
    warn "/usr/bin/bpftrace0.24 is still missing"
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --c-build-root=*)
      C_BUILD_ROOT="${1#*=}"
      ;;
    --projs-dir=*)
      PROJS_DIR="${1#*=}"
      ;;
    --skip-packages)
      INSTALL_PACKAGES=0
      ;;
    --skip-bpftrace)
      INSTALL_BPFTRACE=0
      ;;
    --skip-build)
      BUILD_REPROBUILD=0
      ;;
    --skip-projects)
      SYNC_PROJECTS=0
      ;;
    --skip-c-build)
      SYNC_C_BUILD=0
      ;;
    --update-existing)
      UPDATE_EXISTING=1
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

PROJS_DIR=$(canonicalize_parent "${PROJS_DIR}")
C_BUILD_ROOT=$(canonicalize_parent "${C_BUILD_ROOT}")

info "reprobuild root: ${REPROBUILD_ROOT}"
info "benchmark root: ${PROJS_DIR}"
info "c_build root: ${C_BUILD_ROOT}"

mkdir -p "${PROJS_DIR}"

if [[ "${INSTALL_PACKAGES}" -eq 1 ]]; then
  install_packages
fi

if [[ "${SYNC_PROJECTS}" -eq 1 ]]; then
  for spec in "${PROJECT_SPECS[@]}"; do
    name=${spec%%|*}
    url=${spec#*|}
    ensure_repo "${name}" "${url}" "${PROJS_DIR}/${name}"
    prepare_project_checkout "${name}" "${PROJS_DIR}/${name}"
  done
fi

if [[ "${SYNC_C_BUILD}" -eq 1 ]]; then
  ensure_repo "c_build" "https://github.com/sprooc/c_build.git" "${C_BUILD_ROOT}"
fi

if [[ "${BUILD_REPROBUILD}" -eq 1 ]]; then
  build_reprobuild
fi

if [[ "${INSTALL_BPFTRACE}" -eq 1 ]]; then
  install_bpftrace
fi

post_checks

info "Environment setup complete"
