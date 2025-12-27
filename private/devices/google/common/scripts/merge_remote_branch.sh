#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

FROM_BRANCH=
REMOTE="partner"
FILTER="^private/"
BUG_ID=
ABORT_BRANCH=

readonly REPO_BRANCH="merge-$(date +%s)"

readonly MERGE_SUCCESS=0
readonly MERGE_FAILURE=1
readonly MERGE_NOTHING=2
readonly MERGE_NOSOURCE=3
readonly ABORT_SUCCESS=4
readonly ABORT_FAILURE=5
readonly ABORT_NOTHING=6

function usage() {
  cat <<EOF
usage: $0 [OPTIONS] [REMOTE/]FROM_BRANCH
       $0 [OPTIONS] --abort REPO_BRANCH

Merge a remote branch. If REMOTE is not given, "partner" is used.

OPTIONS:
  -a, --abort REPO_BRANCH       - Abort the in progress merge and abandon the local REPO_BRANCH.
  -b, --bug_id BUG_ID           - Add BUG_ID to the commit message.
  -r, --regex REGEX             - Filter the project list based on regex or wildcard
                                  matching of strings. Default: ^private/
  -h, --help                    - Show this help message and exit

EXAMPLES:
  $0 mirror-android14-gs-pixel-6.1
  $0 partner/mirror-android14-gs-pixel-6.1 -r ^private/devices/google/common$ -b 12345678
  $0 --abort merge-12345678
EOF
}

function error_out() {
  echo "ERROR: $@" >&2
  usage >&2
  exit 1
}

function abort() {
  local proj="$1"
  (
    cd "${proj}"

    if ! git rev-parse --verify -q "${ABORT_BRANCH}" >/dev/null; then
      return "${ABORT_NOTHING}"
    fi

    git merge --abort 2>/dev/null

    if ! repo abandon "${ABORT_BRANCH}" .; then
      return "${ABORT_FAILURE}"
    fi

    return "${ABORT_SUCCESS}"
  )
}

function merge() {
  local proj="$1"
  (
    cd "${proj}"

    if ! git fetch "${REMOTE}" "${FROM_BRANCH}"; then
      echo "${proj}: Cannot fetch ${REMOTE}/${FROM_BRANCH}, skipped." >&2
      return "${MERGE_NOSOURCE}"
    fi

    if git merge-base --is-ancestor "${REMOTE}/${FROM_BRANCH}" HEAD; then
      echo "${proj}: Nothing to merge." >&2
      return "${MERGE_NOTHING}"
    fi

    repo start --head "${REPO_BRANCH}" .

    local msg_file="$(mktemp)"
    local to_branch="$(git rev-parse --abbrev-ref HEAD@{upstream} | cut -d '/' -f2)"

    echo "$(git rev-parse ${REMOTE}/${FROM_BRANCH})"$'\t\t'"${FROM_BRANCH}" \
      | git fmt-merge-msg --log --into-name "${to_branch}" > "${msg_file}"

    sed -i '/^#.*/d' "${msg_file}"

    echo >> "${msg_file}"

    if [[ -n "${BUG_ID}" ]]; then
      echo "Bug: ${BUG_ID}" >> "${msg_file}"
    fi

    if ! git merge --no-ff --signoff --file "${msg_file}" "${REMOTE}/${FROM_BRANCH}"; then
      echo "${proj}: Merge failed." >&2
      return "${MERGE_FAILURE}"
    fi
    return "${MERGE_SUCCESS}"
  )
}

function parse_args() {
  while (( "$#" > 0 )); do
    case "$1" in
      -a|--abort)
        ABORT_BRANCH="$2"
        shift
        ;;
      -b|--bug_id)
        BUG_ID="$2"
        shift
        ;;
      -r|--regex)
        FILTER="$2"
        shift
        ;;
      -h|--help)
        usage
        exit
        ;;
      -*)
        error_out "Unknown option: $1"
        ;;
      *)
        FROM_BRANCH="$1"
        ;;
    esac
    shift
  done

  if [[ -z "${FROM_BRANCH}" && -z "${ABORT_BRANCH}" ]]; then
    error_out "FROM_BRANCH is required"
  fi

  if [[ "${FROM_BRANCH}" == */* ]]; then
    REMOTE="${FROM_BRANCH%%/*}"
    FROM_BRANCH="${FROM_BRANCH#*/}"
  fi
}

function main() {
  parse_args "$@"

  local projects=($(repo list -p -r "${FILTER}"))
  local pids=()

  local proj
  for proj in "${projects[@]}"; do
    if [[ -n "${ABORT_BRANCH}" ]]; then
      abort "${proj}" & pids+=("$!")
    else
      merge "${proj}" & pids+=("$!")
    fi
  done

  local merge_success_projects=()
  local merge_failure_projects=()
  local merge_nothing_projects=()
  local merge_nosource_projects=()
  local abort_success_projects=()
  local abort_failure_projects=()
  local abort_nothing_projects=()

  local i
  for i in "${!projects[@]}"; do
    wait "${pids[i]}"
    case "$?" in
      "${MERGE_SUCCESS}")
        merge_success_projects+=("${projects[i]}")
        ;;
      "${MERGE_FAILURE}")
        merge_failure_projects+=("${projects[i]}")
        ;;
      "${MERGE_NOTHING}")
        merge_nothing_projects+=("${projects[i]}")
        ;;
      "${MERGE_NOSOURCE}")
        merge_nosource_projects+=("${projects[i]}")
        ;;
      "${ABORT_SUCCESS}")
        abort_success_projects+=("${projects[i]}")
        ;;
      "${ABORT_FAILURE}")
        abort_failure_projects+=("${projects[i]}")
        ;;
      "${ABORT_NOTHING}")
        abort_nothing_projects+=("${projects[i]}")
        ;;
    esac
  done

  echo
  echo "Results:"
  for proj in "${merge_nosource_projects[@]}"; do
    echo "SKIPPED (No source branch): ${proj}"
  done
  for proj in "${merge_nothing_projects[@]}"; do
    echo "SKIPPED (Nothing to merge): ${proj}"
  done
  for proj in "${merge_success_projects[@]}"; do
    echo "MERGED: ${proj}"
  done
  for proj in "${merge_failure_projects[@]}"; do
    echo "FAILED: ${proj}"
  done
  for proj in "${abort_nothing_projects[@]}"; do
    echo "SKIPPED (No merge in progress): ${proj}"
  done
  for proj in "${abort_success_projects[@]}"; do
    echo "ABORTED: ${proj}"
  done
  for proj in "${abort_failure_projects[@]}"; do
    echo "FAILED: ${proj}"
  done
  echo
  if [[ -n "${ABORT_BRANCH}" ]]; then
    echo "Abandon branch:"
    echo "${ABORT_BRANCH}"
  else
    echo "Repo branch:"
    echo "${REPO_BRANCH}"
  fi
}

main "$@"
