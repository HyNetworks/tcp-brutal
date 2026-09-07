#!/usr/bin/env bash
#
# install_dkms.sh - tcp-brutal dkms module install script
# Try `install_dkms.sh --help` for usage.
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2023 The Hysteria Project
#

set -e


###
# SCRIPT CONFIGURATION
###

# Command line arguments of this script
SCRIPT_ARGS=("$@")

# Initial URL & command of one-click script (for usage & logging)
# TODO: change the link to real
SCRIPT_INITIATOR_URL="https://tcp.hy2.sh"
SCRIPT_INITIATOR_COMMAND="bash <(curl -fsSL $SCRIPT_INITIATOR_URL)"

# URL of GitHub
REPO_URL="https://github.com/HyNetworks/tcp-brutal"

# URL of Hysteria 2 API
HY2_API_BASE_URL="https://api.hy2.io/v1"

# curl command line flags.
# To using a proxy, please specify ALL_PROXY in the environ variable, such like:
# export ALL_PROXY=socks5h://192.0.2.1:1080
CURL_FLAGS=(-L -f -q --retry 5 --retry-delay 10 --retry-max-time 60)

DKMS_MODULE_NAME="tcp-brutal"
KERNEL_MODULE_NAME="brutal"

# The rule management tool, built from the DKMS source tree at install time
BRUTALCTL_PATH="/usr/local/bin/brutalctl"

# tcp-brutal v2.0.0 and later require this kernel version or later
V2_MIN_KERNEL_VERSION="5.10"
# Last version that supports older kernels
V1_LAST_VERSION="v1.0.3"


###
# AUTO DETECTED GLOBAL VARIABLE
###

# Package manager
PACKAGE_MANAGEMENT_INSTALL="${PACKAGE_MANAGEMENT_INSTALL:-}"


###
# COMMAND REPLACEMENT & UTILITIES
###

has_command() {
  local _command=$1

  type -P "$_command" > /dev/null 2>&1
}

curl() {
  command curl "${CURL_FLAGS[@]}" "$@"
}

mktemp() {
  command mktemp "$@" "/tmp/brutalinst.XXXXXXXXXX"
}

# Colors are optional: without a terminal (no $TERM, or TERM=dumb) tput fails,
# which would kill the script under set -e.
tput() {
  if has_command tput && [[ -n "$TERM" ]]; then
    command tput "$@" 2>/dev/null || true
  fi
}

tred="$(tput setaf 1)"
tgreen="$(tput setaf 2)"
tyellow="$(tput setaf 3)"
tblue="$(tput setaf 4)"
taoi="$(tput setaf 6)"
tbold="$(tput bold)"
treset="$(tput sgr0)"

is_run_from_fd() {
  has_prefix "$0" "/dev/" || has_prefix "$0" "/proc/"
}

script_name() {
  local _keep_dirname="$1"

  if is_run_from_fd; then
    echo "$SCRIPT_INITIATOR_COMMAND"
    return
  fi

  if ! has_prefix "$0" "." && [[ -z "$_keep_dirname" ]]; then
    basename "$0"
    return
  fi
  echo "$0"
}

note() {
  local _msg="$1"

  echo -e "$(script_name): ${tbold}note: $_msg${treset}"
}

warning() {
  local _msg="$1"

  echo -e "$(script_name): ${tyellow}warning: $_msg${treset}"
}

error() {
  local _msg="$1"

  echo -e "$(script_name): ${tred}error: $_msg${treset}"
}

has_prefix() {
    local _s="$1"
    local _prefix="$2"

    if [[ -z "$_prefix" ]]; then
        return 0
    fi

    if [[ -z "$_s" ]]; then
        return 1
    fi

    [[ "x$_s" != "x${_s#"$_prefix"}" ]]
}

show_argument_error_and_exit() {
  local _error_msg="$1"

  error "$_error_msg"
  echo "Try \"$(script_name) --help\" for usage." >&2
  exit 22
}

exec_sudo() {
  # exec sudo with configurable environ preserved.
  local _saved_ifs="$IFS"
  IFS=$'\n'
  local _preserved_env=(
    $(env | grep "^PACKAGE_MANAGEMENT_INSTALL=" || true)
    $(env | grep "^FORCE_\w*=" || true)
  )
  IFS="$_saved_ifs"

  exec sudo env \
    "${_preserved_env[@]}" \
    "$@"
}

detect_package_manager() {
  if [[ -n "$PACKAGE_MANAGEMENT_INSTALL" ]]; then
    return 0
  fi

  if has_command apt; then
    apt update
    PACKAGE_MANAGEMENT_INSTALL='apt -y --no-install-recommends install'
    return 0
  fi

  if has_command dnf; then
    PACKAGE_MANAGEMENT_INSTALL='dnf -y install'
    return 0
  fi

  if has_command yum; then
    PACKAGE_MANAGEMENT_INSTALL='yum -y install'
    return 0
  fi

  if has_command zypper; then
    PACKAGE_MANAGEMENT_INSTALL='zypper install -y'
    return 0
  fi

  if has_command pacman; then
    PACKAGE_MANAGEMENT_INSTALL='pacman -Syu --noconfirm'
    return 0
  fi

  return 1
}

install_software() {
  local _package_name="$1"

  if ! detect_package_manager; then
    error "Supported package manager is not detected, please install the following package manually:"
    echo
    echo -e "\t* $_package_name"
    echo
    exit 65
  fi

  echo "Installing missing dependence '$_package_name' with '$PACKAGE_MANAGEMENT_INSTALL' ... "
  if $PACKAGE_MANAGEMENT_INSTALL "$_package_name"; then
    echo "ok"
  else
    error "Cannot install '$_package_name' with detected package manager, please install it manually."
    exit 65
  fi
}

install_linux_headers() {
  local _kernel_ver="$(uname -r)"

  echo "Try to install linux-headers for $_kernel_ver ... "

  if has_command pacman; then
    local _kernel_img="/lib/modules/$_kernel_ver/vmlinuz"
    if [[ ! -f "$_kernel_img" ]]; then
      error "Kernel image does not exist."
      note "If you are using a kernel installed by pacman, this usually caused by system upgrading without reboot."
      note "Please reboot your server and try again."
      return 2
    fi
    local _kernel_pkg=$(pacman -Qoq "$_kernel_img")
    if [[ -z "$_kernel_pkg" ]]; then
      error "Failed to detect kernel package."
      warning "It seems like you are NOT using a kernel that installed by pacman."
      return 2
    fi
    install_software "$_kernel_pkg-headers"
  elif has_command apt; then
    install_software "linux-headers-$_kernel_ver"
  elif has_command dnf || has_command yum; then
    install_software "kernel-devel-$_kernel_ver"
  else
    # unsupported
    error "Automatically linux headers installing is currently not supported on this distribution."
    return 1
  fi
}

rerun_with_sudo() {
  if ! has_command sudo; then
    return 13
  fi

  local _target_script

  if is_run_from_fd; then
    local _tmp_script="$(mktemp)"
    chmod +x "$_tmp_script"

    if has_command curl; then
      curl -o "$_tmp_script" "$SCRIPT_INITIATOR_URL"
    elif has_command wget; then
      wget -O "$_tmp_script" "$SCRIPT_INITIATOR_URL"
    else
      return 127
    fi

    _target_script="$_tmp_script"
  else
    _target_script="$0"
  fi

  note "Re-running this script with sudo."
  exec_sudo "$_target_script" "${SCRIPT_ARGS[@]}"
}

check_permission() {
  if [[ "$UID" -eq '0' ]]; then
    return
  fi

  note "The user running this script is not root."

  if ! rerun_with_sudo; then
    error "Please manually switch to root and run this script again."
    echo
    echo -e "\t${tred}sudo -H bash${treset}"
    echo -e "\t${tred}$(script_name "1")${treset}"
    echo
    exit 13
  fi
}

check_environment_operating_system() {
  if [[ "x$(uname)" == "xLinux" ]]; then
    return
  fi

  error "This script only supports Linux."
  exit 95
}

check_environment_curl() {
  if has_command curl; then
    return
  fi

  install_software curl
}

check_environment_grep() {
  if has_command grep; then
    return
  fi

  install_software grep
}

check_environment_dkms() {
  if has_command dkms; then
    return
  fi

  install_software dkms
}

is_linux_headers_installed() {
  test -d "/lib/modules/$(uname -r)/build"
}

is_archlinux() {
  test -f "/etc/arch-release"
}


check_linux_headers() {
  echo -n "Checking linux-headers ... "
  if is_linux_headers_installed; then
    echo "ok"
  else
    echo "not installed"
    if ! install_linux_headers; then
      warning "Kernel headers is missing for current running kernel."
      warning "The DKMS kernel module will not be compiled."
    fi
  fi
}

# A clang-built kernel (CONFIG_CC_IS_CLANG) needs clang, lld and llvm to build modules
is_clang_kernel() {
  local _build="/lib/modules/$(uname -r)/build"
  local _config

  for _config in "$_build/include/config/auto.conf" "$_build/.config" "/boot/config-$(uname -r)"; do
    if [[ -f "$_config" ]]; then
      if grep -q '^CONFIG_CC_IS_CLANG=y' "$_config"; then
        return 0
      fi
      return 1
    fi
  done
  return 1
}

check_llvm_toolchain() {
  if ! is_clang_kernel; then
    return
  fi
  echo -n "Checking LLVM toolchain (kernel built with clang) ... "
  if has_command clang && has_command ld.lld && has_command llvm-objcopy; then
    echo "ok"
    return
  fi
  echo "not installed"
  install_software clang
  install_software lld
  install_software llvm
}

check_environment() {
  check_environment_operating_system
  check_environment_curl
  check_environment_grep
  check_environment_dkms
  check_linux_headers
}

vercmp_segment() {
  local _lhs="$1"
  local _rhs="$2"

  if [[ "x$_lhs" == "x$_rhs" ]]; then
    echo 0
    return
  fi
  if [[ -z "$_lhs" ]]; then
    echo -1
    return
  fi
  if [[ -z "$_rhs" ]]; then
    echo 1
    return
  fi

  local _lhs_num="${_lhs//[A-Za-z]*/}"
  local _rhs_num="${_rhs//[A-Za-z]*/}"

  if [[ "x$_lhs_num" == "x$_rhs_num" ]]; then
    echo 0
    return
  fi
  if [[ -z "$_lhs_num" ]]; then
    echo -1
    return
  fi
  if [[ -z "$_rhs_num" ]]; then
    echo 1
    return
  fi
  local _numcmp=$(($_lhs_num - $_rhs_num))
  if [[ "$_numcmp" -ne 0 ]]; then
    echo "$_numcmp"
    return
  fi

  local _lhs_suffix="${_lhs#"$_lhs_num"}"
  local _rhs_suffix="${_rhs#"$_rhs_num"}"

  if [[ "x$_lhs_suffix" == "x$_rhs_suffix" ]]; then
    echo 0
    return
  fi
  if [[ -z "$_lhs_suffix" ]]; then
    echo 1
    return
  fi
  if [[ -z "$_rhs_suffix" ]]; then
    echo -1
    return
  fi
  if [[ "$_lhs_suffix" < "$_rhs_suffix" ]]; then
    echo -1
    return
  fi
  echo 1
}

vercmp() {
  local _lhs=${1#v}
  local _rhs=${2#v}

  while [[ -n "$_lhs" && -n "$_rhs" ]]; do
    local _clhs="${_lhs/.*/}"
    local _crhs="${_rhs/.*/}"

    local _segcmp="$(vercmp_segment "$_clhs" "$_crhs")"
    if [[ "$_segcmp" -ne 0 ]]; then
      echo "$_segcmp"
      return
    fi

    _lhs="${_lhs#"$_clhs"}"
    _lhs="${_lhs#.}"
    _rhs="${_rhs#"$_crhs"}"
    _rhs="${_rhs#.}"
  done

  if [[ "x$_lhs" == "x$_rhs" ]]; then
    echo 0
    return
  fi

  if [[ -z "$_lhs" ]]; then
    echo -1
    return
  fi

  if [[ -z "$_rhs" ]]; then
    echo 1
    return
  fi

  return
}

kernel_version() {
  # e.g. 5.10.0-8-amd64 -> 5.10.0
  uname -r | grep -oE '^[0-9]+\.[0-9]+(\.[0-9]+)?'
}

check_kernel_version() {
  local _version="$1"

  if [[ "$(vercmp "$_version" "v2.0.0")" -lt "0" ]]; then
    return
  fi
  if [[ "$(vercmp "$(kernel_version)" "$V2_MIN_KERNEL_VERSION")" -ge "0" ]]; then
    return
  fi

  error "tcp-brutal $_version requires Linux $V2_MIN_KERNEL_VERSION or later, but this system is running $(uname -r)."
  note "Run '$(script_name) --version $V1_LAST_VERSION' to install the last version that supports this kernel."
  exit 95
}


###
# ARGUMENTS PARSER
###

show_usage_and_exit() {
  echo
  echo -e "\t${tbold}$(script_name)${treset} - tcp-brutal dkms install script"
  echo
  echo -e "Usage:"
  echo
  echo -e "${tbold}Install tcp-brutal${treset} (the kernel module, and brutalctl to $BRUTALCTL_PATH)"
  echo -e "\t$(script_name) [install] [ -f | -l <file> | --version <version> ]"
  echo -e "Options:"
  echo -e "\t-f, --force\tForce re-install latest or specified version even if it has been installed."
  echo -e "\t-l, --local <file>\tInstall specified DKMS tarball instead of download it."
  echo -e "\t--version <version>\tInstall specified version instead of the latest."
  echo
  echo -e "${tbold}Uninstall tcp-brutal${treset}"
  echo -e "\t$(script_name) uninstall"
  echo
  echo -e "${tbold}Check for the status & update${treset}"
  echo -e "\t$(script_name) check"
  echo
  echo -e "${tbold}Reload / Unload tcp-brutal kernel module${treset}"
  echo -e "\t$(script_name) [re]load"
  echo -e "\t$(script_name) unload"
  echo
  echo -e "${tbold}Show this help${treset}"
  echo -e "\t$(script_name) help"
  exit 0
}

check_show_usage_and_exit() {
  case "$1" in
    "help")
      show_usage_and_exit
      ;;
  esac

  # if '-h' or '--help' appear in arguments in any position,
  # display help and exit
  while [[ "$#" -gt '0' ]]; do
    case "$1" in
      '--help' | '-h')
        show_usage_and_exit
        ;;
    esac
    shift
  done
}


###
# DKMS
###

dkms_get_installed_versions() {
  local _module="$1"

  local _dkms_moddir="/var/lib/dkms/$_module"

  if [[ ! -d "$_dkms_moddir" ]]; then
    return
  fi

  for file in $(command ls "$_dkms_moddir/"); do
    if [[ -L "$_dkms_moddir/$file" ]]; then
      # ignore kernel-* symlinks
      continue
    fi
    echo "v$file"
  done
}

dkms_remove_modules() {
  local _module="$1"
  local _keep_latest="$2"

  local _versions_to_remove=($(dkms_get_installed_versions "$_module"))
  if [[ -n "$_keep_latest" ]]; then
    local _latest=""
    local _new_versions_to_remove
    _new_versions_to_remove=()
    for version in "${_versions_to_remove[@]}"; do
      local _vercmp="$(vercmp "$version" "$_latest")"
      if [[ "$_vercmp" -gt 0 ]]; then
        if [[ -n "$_latest" ]]; then
          _new_versions_to_remove+=("$_latest")
        fi
        _latest="$version"
      else
        _new_versions_to_remove+=("$version")
      fi
    done
    _versions_to_remove=("${_new_versions_to_remove[@]}")
  fi

  for version in "${_versions_to_remove[@]}"; do
    local _dkms_version="${version#v}"

    echo -n "Removing DKMS module $_module/$_dkms_version ... "
    if dkms remove "$_module/$_dkms_version" --all > /dev/null; then
      echo "ok"
    else
      # suppress dkms remove failed, shall not to be a problem
      continue
    fi
    echo -n "Cleaning DKMS module source /usr/src/$_module-$_dkms_version ... "
    if rm -rf "/usr/src/$_module-$_dkms_version"; then
      echo "ok"
    else
      # also suppress this
      continue
    fi
  done
}

dkms_ldtarball() {
  local _tarball="$1"

  # dkms variables
  local PACKAGE_NAME PACKAGE_VERSION MAKE CLEAN
  local BUILT_MODULE_NAME DEST_MODULE_LOCATION AUTOINSTALL

  local _extractdir="$(mktemp -d)"
  tar xf "$_tarball" -C "$_extractdir"
  source "$_extractdir/dkms_source_tree/dkms.conf"

  if [[ -z "$PACKAGE_NAME" || -z "$PACKAGE_VERSION" ]]; then
    error "Malformed DKMS tarball, PACKAGE_NAME or PACKAGE_VERSION is missing."
    exit 22
  fi

  rm -rf "/usr/src/$PACKAGE_NAME-$PACKAGE_VERSION"
  mkdir -p "/usr/src/$PACKAGE_NAME-$PACKAGE_VERSION"
  cp -a "$_extractdir/dkms_source_tree/." "/usr/src/$PACKAGE_NAME-$PACKAGE_VERSION/"
  rm -rf "$_extractdir"

  dkms add "$PACKAGE_NAME/$PACKAGE_VERSION"
}

dkms_install_tarball() {
  local _tarball="$1"

  echo "Installing DKMS module from tarball file $_tarball ... "
  if ! dkms_ldtarball "$_tarball"; then
    error "Failed to install DKMS tarball, please check above output or try to uninstall first."
    return 1
  fi
}

# brutalctl is a user-space program and needs the C library headers. Unlike
# the kernel module, which only needs linux-headers, and unlike the compiler
# itself, they are not pulled in by dkms (e.g. apt --no-install-recommends).
install_libc_headers() {
  local _cc="$1"
  local _package

  if echo '#include <errno.h>' | "$_cc" -E -x c - >/dev/null 2>&1; then
    return 0
  fi
  if has_command apt; then
    _package="libc6-dev"
  elif has_command dnf || has_command yum || has_command zypper; then
    _package="glibc-devel"
  else
    return 1
  fi
  echo "Installing missing dependence '$_package' ... "
  detect_package_manager && $PACKAGE_MANAGEMENT_INSTALL "$_package"
}

brutalctl_install() {
  local _version="$(dkms_get_installed_versions "$DKMS_MODULE_NAME" | head -1)"
  local _source="/usr/src/$DKMS_MODULE_NAME-${_version#v}/tools/brutalctl.c"
  local _cc

  if [[ ! -f "$_source" ]]; then
    echo "Installing brutalctl to $BRUTALCTL_PATH ... skipped (not part of $_version)"
    return
  fi
  if has_command cc; then
    _cc="cc"
  elif has_command gcc; then
    _cc="gcc"
  else
    echo "Installing brutalctl to $BRUTALCTL_PATH ... skipped (no C compiler)"
    return
  fi
  install_libc_headers "$_cc" || true
  echo -n "Installing brutalctl to $BRUTALCTL_PATH ... "
  if "$_cc" -O2 -Wall -o "$BRUTALCTL_PATH" "$_source"; then
    echo "ok"
  else
    warning "Failed to build brutalctl, the kernel module is not affected."
  fi
}

brutalctl_uninstall() {
  if [[ -f "$BRUTALCTL_PATH" ]]; then
    echo -n "Removing $BRUTALCTL_PATH ... "
    rm -f "$BRUTALCTL_PATH" && echo "ok"
  fi
}


###
# Kernel modules
###

kmod_is_loaded() {
  local _module="$1"

  lsmod | grep -qP '\b'"$_module"'\b'
}

kmod_load_if_unloaded() {
  local _module="$1"

  if ! kmod_is_loaded "$_module"; then
    echo -n "Loading kernel module $_module ... "
    if modprobe "$_module"; then
      echo "ok"
    else
      error "Failed to load kernel module, kernel module might not be installed successfully."
      return 1
    fi
  fi
}

kmod_unload_if_loaded() {
  local _module="$1"

  if kmod_is_loaded "$_module"; then
    echo -n "Unloading kernel module $_module ... "
    if rmmod "$_module"; then
      echo "ok"
    else
      error "Failed to unload kernel module, kernel module might be occupied by other process."
      error "Try to stop all related proxy service, or simply reboot your server and try again."
      return 1
    fi
  fi
}

kmod_setup_autoload() {
  local _module="$1"

  echo -n "Enabling auto load kernel module $_module on system boot ... "
  if echo "$_module" > "/etc/modules-load.d/$_module.conf"; then
    echo "ok"
  else
    warning "Failed to enable auto load $_module on system boot."
  fi
}

kmod_unsetup_autoload() {
  local _module="$1"

  echo -n "Disabling auto load kernel module $_module on system boot ... "
  if rm -f "/etc/modules-load.d/$_module.conf"; then
    echo "ok"
  else
    warning "Failed to disable auto load $_module on system boot."
  fi
}

###
# API
###

get_latest_version() {
  if [[ -n "$VERSION" ]]; then
    echo "$VERSION"
    return
  fi

  local _tmpfile=$(mktemp)
  if ! curl -sS "$HY2_API_BASE_URL/update?cver=installscript&arch=generic&plat=linux&chan=tcp-brutal" -o "$_tmpfile"; then
    error "Failed to get the latest version from Hysteria 2 API, please check your network and try again."
    exit 11
  fi

  local _latest_version=$(grep -oP '"lver":\s*\K"v.*?"' "$_tmpfile" | head -1)
  _latest_version=${_latest_version#'"'}
  _latest_version=${_latest_version%'"'}

  if [[ -n "$_latest_version" ]]; then
    echo "$_latest_version"
  fi

  rm -f "$_tmpfile"
}

download_dkms_tarball() {
  local _version="$1"
  local _destination="$2"

  local _download_url="$REPO_URL/releases/download/$_version/tcp-brutal.dkms.tar.gz"
  echo "Downloading DKMS tarball: $_download_url ..."
  if ! curl -R -H 'Cache-Control: no-cache' "$_download_url" -o "$_destination"; then
    error "Download failed, please check your network and try again."
    return 11
  fi
  return 0
}


###
# ENTRY
###

perform_install() {
  local _local_file=""
  local _user_provided_local_file=""
  local _version=""
  local _install_needed=""

  while [[ "$#" -gt '0' ]]; do
    case "$1" in
      '--force' | '-f')
        _install_needed="1"
        ;;
      '--local' | '-l')
        shift
        if [[ "x$1" == "x--" ]]; then
          shift
          _local_file="$1"
        elif has_prefix "$1" "-"; then
          _local_file=""
        else
          _local_file="$1"
        fi
        if [[ -z "$_local_file" ]]; then
          show_argument_error_and_exit "Please specify the local dkms.tar file to install for option '-l' or '--local'."
        fi
        _install_needed="1"
        _user_provided_local_file="1"
        ;;
      '--version')
        shift
        if [[ "x$1" == "x--" ]]; then
          shift
          _version="$1"
        elif has_prefix "$1" "-"; then
          _version=""
        else
          _version="$1"
        fi
        if [[ -z "$_version" ]]; then
          show_argument_error_and_exit "Please specify the version for option '--version'."
        fi
        ;;
      *)
        show_argument_error_and_exit "Unrecognized option '$1' for subcommand 'install'."
        ;;
    esac
    shift
  done

  if [[ -n "$_local_file" && -n "$_version" ]]; then
    show_argument_error_and_exit "'--version' and '--local' cannot be used together."
  fi

  # check installed version
  echo "Cleaning old installations ... "
  dkms_remove_modules "$DKMS_MODULE_NAME" "1"

  echo -n "Checking installed version ... "
  local _installed_version="$(dkms_get_installed_versions "$DKMS_MODULE_NAME" | head -1)"
  if [[ -n "$_installed_version" ]]; then
    echo "$_installed_version"
  else
    echo "not installed"
  fi

  if [[ -z "$_local_file" && -z "$_version" ]]; then
    echo -n "Checking latest version ... "
    local _latest_version=$(get_latest_version)
    if [[ -n "$_latest_version" ]]; then
      echo "$_latest_version"
      _version="$_latest_version"
    fi
  fi

  if [[ -z "$_local_file" && -n "$_version" ]]; then
    check_kernel_version "$_version"

    local _vercmp="$(vercmp "$_installed_version" "$_version")"
    if [[ "$_vercmp" -lt "0" ]]; then
      _install_needed="1"
    fi
    if [[ -n "$_install_needed" ]]; then
      local _download_destination="$(mktemp).tar.gz"
      download_dkms_tarball "$_version" "$_download_destination"
      _local_file="$_download_destination"
    fi
  fi

  if [[ -n "$_install_needed" ]]; then
    # remove all installed version as DKMS not allowed to overwrite a installed module
    dkms_remove_modules "$DKMS_MODULE_NAME" ""
    dkms_install_tarball "$_local_file"
  fi

  if [[ -z "$_user_provided_local_file" && -n "$_local_file" ]]; then
    # clean auto downloaded tarball
    rm -f "$_local_file"
  fi

  check_llvm_toolchain

  echo "Rebuilding DKMS modules as needed ... "
  if ! dkms autoinstall; then
    warning "Error occurred in 'dkms autoinstall', please check above output."
  fi

  brutalctl_install

  kmod_setup_autoload "$KERNEL_MODULE_NAME"

  if [[ -z "$_install_needed" ]]; then
    if ! kmod_load_if_unloaded "$KERNEL_MODULE_NAME"; then
      warning "tcp-brutal is installed but failed to load."
    fi

    echo "${tbold}There is nothing to do today.${treset}"
    exit 0
  fi

  if ! kmod_unload_if_loaded "$KERNEL_MODULE_NAME"; then
    warning "tcp-brutal is successfully update, but occupied by other process, please reboot your server to active the latest change."
    exit 0
  fi

  if ! kmod_load_if_unloaded "$KERNEL_MODULE_NAME"; then
    error "tcp-brutal is successfully installed, but failed to load, this might cause by mismatched linux-headers."
    error "If you update your system recently, reboot the system might solve this."
    exit 2
  fi

  echo
  echo -e "${tbold}Congratulation! tcp-brutal $_version has been successfully installed and loaded on your server.${treset}"
}

perform_uninstall() {
  while [[ "$#" -gt '0' ]]; do
    case "$1" in
      *)
        show_argument_error_and_exit "Unrecognized option '$1' for subcommand 'uninstall'."
        ;;
    esac
    shift
  done

  kmod_unsetup_autoload "$KERNEL_MODULE_NAME"

  dkms_remove_modules "$DKMS_MODULE_NAME" ""

  brutalctl_uninstall

  if ! kmod_unload_if_loaded "$KERNEL_MODULE_NAME"; then
    warning "tcp-brutal is successfully uninstall from your server, but failed to unload from the kernel."
    warning "Please reboot your system to unload it from the kernel."
    exit 0
  fi

  echo
  echo -e "${tbold}Congratulation! tcp-brutal has been successfully uninstalled and unloaded."
}

perform_check() {
  while [[ "$#" -gt '0' ]]; do
    case "$1" in
      *)
        show_argument_error_and_exit "Unrecognized option '$1' for subcommand 'check'."
        ;;
    esac
    shift
  done

  echo -n "Checking kernel module ... "
  if kmod_is_loaded "$KERNEL_MODULE_NAME"; then
    echo "loaded, version $(cat "/sys/module/$KERNEL_MODULE_NAME/version" 2> /dev/null || echo unknown)"
  else
    echo "not loaded"
  fi

  echo -n "Checking installed version ... "
  local _installed_versions=($(dkms_get_installed_versions "$DKMS_MODULE_NAME"))
  if [[ "${#_installed_versions[@]}" -eq "0" ]]; then
    echo "not installed"
  elif [[ "${#_installed_versions[@]}" -eq "1" ]]; then
    echo "${_installed_versions[0]}"
  else
    echo "multiple version installed"
    for version in "${_installed_versions[@]}"; do
      echo -e "\tFound $version"
    done
  fi

  echo -n "Checking latest version ... "
  local _latest_version=$(get_latest_version)
  if [[ -n "$_latest_version" ]]; then
    echo "$_latest_version"
  fi
}

perform_reload() {
  while [[ "$#" -gt '0' ]]; do
    case "$1" in
      *)
        show_argument_error_and_exit "Unrecognized option '$1' for subcommand 'reload'."
        ;;
    esac
    shift
  done

  kmod_unload_if_loaded "$KERNEL_MODULE_NAME"
  kmod_load_if_unloaded "$KERNEL_MODULE_NAME"
}

perform_unload() {
  while [[ "$#" -gt '0' ]]; do
    case "$1" in
      *)
        show_argument_error_and_exit "Unrecognized option '$1' for subcommand 'unload'."
        ;;
    esac
    shift
  done

  kmod_unload_if_loaded "$KERNEL_MODULE_NAME"
}

main() {
  check_show_usage_and_exit "$@"

  check_permission
  check_environment

  case "$1" in
    "install")
      shift
      perform_install "$@"
      ;;
    "uninstall" | "remove")
      shift
      perform_uninstall "$@"
      ;;
    "check" | "status")
      shift
      perform_check "$@"
      ;;
    "load" | "reload")
      shift
      perform_reload "$@"
      ;;
    "unload")
      shift
      perform_unload "$@"
      ;;
    *)
      # default action
      perform_install "$@"
      ;;
  esac
}

main "$@"

# vim:set ft=bash ts=2 sw=2 sts=2 et:
