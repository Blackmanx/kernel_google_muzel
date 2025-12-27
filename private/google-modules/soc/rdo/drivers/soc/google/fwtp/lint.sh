#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Run lint on source files.
#
# This scripts runs various linters such as kernel-doc, checkpatch, and
# clang-format on source files.
#

# Get the parent directory of this script.
SCRIPT_FILE_PATH="$(realpath "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(dirname "${SCRIPT_FILE_PATH}")"

# Define the files to lint.
C_FILES="$(find ${SCRIPT_DIR} -name "*.[c,h]" -printf "%P\n")"
SH_FILES="$(find ${SCRIPT_DIR} -name "*.sh" -printf "%P\n")"
BUILD_FILES="$(find ${SCRIPT_DIR} -name "BUILD*" -printf "%P\n")"

# Define the set of checkpatch violations to ignore.
# Files copied from Pixel FW use some typedefs (e.g., fwtp_error_code_t).
CHECKPATCH_IGNORE="NEW_TYPEDEFS"

# Run lint on C files.
for LINT_FILE in ${C_FILES}; do
  echo Linting "${LINT_FILE}"
  scripts/checkpatch.pl --ignore "${CHECKPATCH_IGNORE}" --show-types -q \
                        --no-tree --file "${SCRIPT_DIR}/${LINT_FILE}"
  clang-format "${SCRIPT_DIR}/${LINT_FILE}" | diff "${SCRIPT_DIR}/${LINT_FILE}" -
  scripts/kernel-doc -Werror -none "${SCRIPT_DIR}/${LINT_FILE}"
done

# Run lint on shell files.
for LINT_FILE in ${SH_FILES}; do
  echo Linting "${LINT_FILE}"
  scripts/checkpatch.pl --ignore "${CHECKPATCH_IGNORE}" -q --no-tree --file \
                        "${SCRIPT_DIR}/${LINT_FILE}"
done

# Run lint on BUILD files.
for LINT_FILE in ${BUILD_FILES}; do
  echo Linting "${LINT_FILE}"
  buildifier -d --diff_command="diff" -lint="warn" "${SCRIPT_DIR}/${LINT_FILE}"
done

