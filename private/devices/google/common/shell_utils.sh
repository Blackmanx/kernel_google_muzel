# SPDX-License-Identifier: GPL-2.0-or-later

function gettop
{
    local TOPFILE=private/devices/google/common/shell_utils.sh
    # The ${TOP-} expansion allows this to work even with set -u
    if [ -n "${TOP:-}" -a -f "${TOP:-}/$TOPFILE" ] ; then
        # The following circumlocution ensures we remove symlinks from TOP.
        (cd "$TOP"; PWD= /bin/pwd)
    else
        if [ -f $TOPFILE ] ; then
            # The following circumlocution (repeated below as well) ensures
            # that we record the true directory name and not one that is
            # faked up with symlink names.
            PWD= /bin/pwd
        else
            local HERE=$PWD
            local T=
            while [ \( ! \( -f $TOPFILE \) \) -a \( "$PWD" != "/" \) ]; do
                \cd ..
                T=`PWD= /bin/pwd -P`
            done
            \cd "$HERE"
            if [ -f "$T/$TOPFILE" ]; then
                echo "$T"
            fi
        fi
    fi
}

function setup_cog_env_if_needed() {
  local top=$(gettop)

  # return early if not in a cog workspace
  if [[ ! "$top" =~ ^/google/cog ]]; then
    return 0
  fi

  setup_cog_symlink

  export ANDROID_BUILD_ENVIRONMENT_CONFIG="googler-cog"

  # Running repo command within Cog workspaces is not supported, so override
  # it with this function. If the user is running repo within a Cog workspace,
  # we'll fail with an error, otherwise, we run the original repo command with
  # the given args.
  if ! ORIG_REPO_PATH=`which repo`; then
    return 0
  fi
  function repo {
    if [[ "${PWD}" == /google/cog/* ]]; then
      echo -e "\e[01;31mERROR:\e[0mrepo command is disallowed within Cog workspaces."
      kill -INT $$ # exits the script without exiting the user's shell
    fi
    ${ORIG_REPO_PATH} "$@"
  }
}

# creates a symlink for the out/ dir when inside a cog workspace.
function setup_cog_symlink() {
  local out_dir=$(getoutdir)
  local top=$(gettop)
  local cog_workspace_name="$(basename "$(dirname "${top}")")"

  # return early if out dir is already a symlink.
  if [[ -L "$out_dir" ]]; then
    destination=$(readlink "$out_dir")
    # ensure the destination exists.
    mkdir -p "$destination"
    return 0
  fi

  # return early if out dir is not in the workspace
  if [[ ! "$out_dir" =~ ^$top/ ]]; then
    return 0
  fi

  local link_destination="${HOME}/.cog/partner-android-build-out/${cog_workspace_name}"

  # remove existing out/ dir if it exists
  if [[ -d "$out_dir" ]]; then
    echo "Detected existing out/ directory in the Cog workspace which is not supported. Repairing workspace by removing it and creating the symlink to ~/.cog/android-build-out"
    if ! rm -rf "$out_dir"; then
      echo "Failed to remove existing out/ directory: $out_dir" >&2
      kill -INT $$ # exits the script without exiting the user's shell
    fi
  fi

  # create symlink
  echo "Creating symlink: $out_dir -> $link_destination"
  mkdir -p ${link_destination}
  if ! ln -s "$link_destination" "$out_dir"; then
    echo "Failed to create cog symlink: $out_dir -> $link_destination" >&2
    kill -INT $$ # exits the script without exiting the user's shell
  fi
}

function getoutdir
{
    local top=$(gettop)
    local out_dir="${OUT_DIR:-}"
    if [[ -z "${out_dir}" ]]; then
        if [[ -n "${OUT_DIR_COMMON_BASE:-}" && -n "${top}" ]]; then
            out_dir="${OUT_DIR_COMMON_BASE}/$(basename ${top})"
        else
            out_dir="out"
        fi
    fi
    if [[ "${out_dir}" != /* ]]; then
        out_dir="${top}/${out_dir}"
    fi
    echo "${out_dir}"
}
