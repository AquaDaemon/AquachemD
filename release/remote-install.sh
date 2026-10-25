#!/bin/bash

# Enable strict error checking across environments
set -u
set -o pipefail

# Global Configurations & Ecosystem Constants
BIN="aquachemd"
REPO="https://api.github.com/repos/Aquadaemon/${BIN}"
INSTALLED_BINARY="/usr/local/bin/${BIN}"

# Systemd Journal Message ID (Shared with C code)
SD_MSG_ID="c3b9b418e24440939b4bfae6dfbc1122" 

# Inverse Boolean Setup
TRUE=0
FALSE=1

# Initialize output log destination (overridden by specific logic files)
OUTPUT=""

log_init_session() {
  local msg="$1"
  
  # Print to console
  echo "$msg"

  if command -v systemd-journal-send &>/dev/null && [[ -n "${SD_MSG_ID:-}" ]]; then
    systemd-journal-send \
      MESSAGE_ID="$SD_MSG_ID" \
      SYSLOG_IDENTIFIER="${BIN}" \
      PRIORITY=5 \
      MESSAGE="$msg"
  else
    logger -p "local0.notice" -t ${BIN} -- "$msg"
  fi
}

# Centralized Logger Engine
log_to_journal() {
  local priority="$1"
  shift
  local msg="$*"

  # Map systemd numeric priorities to syslog facilities for the logger fallback
  local syslog_pri="local0.notice"
  if [ "$priority" -eq 3 ]; then
    syslog_pri="local0.err"
  elif [ "$priority" -eq 6 ]; then
    syslog_pri="local0.info"
  fi

  # Output straight to console stdout/stderr
  if [ "$priority" -eq 3 ]; then
    echo "Error: $msg" >&2
  else
    echo "$msg"
  fi

  # Log to systemd journal database if available
  if command -v systemd-journal-send &>/dev/null; then
    systemd-journal-send \
      SYSLOG_IDENTIFIER="${BIN}" \
      PRIORITY="$priority" \
      MESSAGE="$msg"
  else
    logger -p "$syslog_pri" -t ${BIN} "Upgrade/Install: $msg"
  fi

  # Log to file if OUTPUT variable has been populated
  if [[ -n "${OUTPUT:-}" ]]; then
    if [ "$priority" -eq 3 ]; then
      echo "$(date): ERROR: $msg" >> "$OUTPUT" 2>/dev/null || true
    else
      echo "$(date): $msg" >> "$OUTPUT" 2>/dev/null || true
    fi
  fi
}

log() {
  log_to_journal 5 "$*"
}

logerr() {
  log_to_journal 3 "$*"
}

check_tool() {
  local cmd=$1
  if ! command -v "$cmd" &>/dev/null; then
    log "Command '$cmd' could not be found!"
    return "$FALSE"
  fi
  return "$TRUE"
}

check_root_privileges() {
  if [[ $EUID -ne 0 ]]; then
     logerr "This script must be run as root" 
     exit 1
  fi
}
# ==============================================================================
# REMOTE INSTALLATION / UPGRADE LOGIC
# ==============================================================================
CONTEXT="REMOTE"
SELF="remote_install.sh"
OUTPUT="/var/log/${BIN}_upgrade.log"

REQUIRED_SPACE_MB=2
REL_VERSION=""
DEV_VERSION=""
INSTALLED_VERSION=""

TEMP_INSTALL="/tmp/${BIN}"
UNTAR_CMD="tar xz --directory=$TEMP_INSTALL"

FROM_CURL=$FALSE
SYSTEMD_LOG=$FALSE

if command -v "systemd-cat" &>/dev/null; then
  SYSTEMD_LOG=$TRUE
fi

cleanup_on_exit() {
  rm -rf "$TEMP_INSTALL"
}
trap cleanup_on_exit EXIT

latest_release_version() {
  REL_VERSION=$(curl -fsSL "$REPO/releases/latest" | grep -Po '"tag_name": "\K.*?(?=")')
  if [[ "$REL_VERSION" != "" ]]; then return "$TRUE"; else return "$FALSE"; fi
}

latest_development_version() {
  DEV_VERSION=$(curl -fsSL -H "Accept: application/vnd.github.raw" "$REPO/contents/source/version.h" | grep AQUALINKD_VERSION | cut -d '"' -f 2)
  if [[ "$DEV_VERSION" != "" ]]; then return "$TRUE"; else return "$FALSE"; fi
}

installed_version() {
  if [ -f "$INSTALLED_BINARY" ]; then
    if check_tool strings && check_tool grep && check_tool awk && check_tool tr; then
      INSTALLED_VERSION=$(strings "$INSTALLED_BINARY" | grep sw_version | awk -v RS="," -v FS=":" '/sw_version/{print $2;exit;}' | tr -d ' "' )
    fi
  else
    log "AquachemD is not installed"
  fi
  if [[ "$INSTALLED_VERSION" != "" ]]; then return "$TRUE"; else return "$FALSE"; fi
}

check_system_arch() {
  ARCH=$(dpkg --print-architecture)
  case $ARCH in 
    arm64 | armhf) return "$TRUE" ;;
    *)
      logerr "System arch is $ARCH, this is not supported by AquachemD"
      return "$FALSE"
    ;;
  esac
}

check_can_upgrade() {
  local output=""
  if ! command -v curl &>/dev/null; then output+="Command 'curl' not found\n"; fi
  if ! command -v dpkg &>/dev/null; then output+="Command 'dpkg' not found\n"; fi
  if ! command -v systemctl &>/dev/null; then output+="Command 'systemctl' not found\n"; fi
  
  if mount | grep " / " | grep -q "(ro,"; then
    if mount / -o remount,rw &>/dev/null; then mount / -o remount,ro &>/dev/null; else output+="Root filesystem is readonly & failed to remount rw"; fi
  fi

  if ! latest_release_version; then output+="Couldn't find latest version on github"; fi
  if command -v dpkg &>/dev/null && ! check_system_arch; then output+="System Architecture not supported!"; fi

  mkdir -p "$TEMP_INSTALL"
  local free_space_mb=$(df -mP "$TEMP_INSTALL" 2>/dev/null | awk 'NR==2{print $4}' )

  if [ -z "$free_space_mb" ] || ! [[ "$free_space_mb" =~ ^[0-9]+$ ]]; then
    output+="Could not retrieve free space for directory: $TEMP_INSTALL"
  else
    if [ "$free_space_mb" -lt "$REQUIRED_SPACE_MB" ]; then output+="Not enough disk space! (Required $REQUIRED_SPACE_MB MB)"; fi
  fi

  if [[ "$output" == "" ]] && [[ "$REL_VERSION" != "" ]]; then return "$TRUE"; else logerr "$output"; return "$FALSE"; fi
}

download_latest_release() {
  mkdir -p "$TEMP_INSTALL"
  local tar_url=""

  tar_url=$(curl -fsSL "$REPO/releases/latest" | grep -Po '"browser_download_url": "\K.*?(?=")' | grep -i ${BIN}-release.tar.gz)
  
  if [[ "$tar_url" == "" ]]; then return "$FALSE"; fi
  curl -fsSL "$tar_url" | $UNTAR_CMD
  if [ $? -ne 0 ]; then return "$FALSE"; fi
  return "$TRUE"
}

download_latest_development() {
  mkdir -p "$TEMP_INSTALL"
  local tar_url="$REPO/tarball/master"
  curl -fsSL "$tar_url" | tar xz --strip-components=1 --directory="$TEMP_INSTALL"
  if [ $? -ne 0 ]; then return "$FALSE"; fi
  return "$TRUE"
}

download_version() {
  local tar_url=""

  tar_url=$(curl -fsSL "$REPO/releases" | awk 'match(tolower($0),/.*"browser_download_url": "(.*\/'"${BIN}"'-release\.tar\.gz)".*/)' | grep "$1/" | awk -F '"' '{print $4}' )

  if [[ ! -n "$tar_url" ]]; then return "$FALSE"; fi
  mkdir -p "$TEMP_INSTALL"
  curl -fsSL "$tar_url" | $UNTAR_CMD
  if [ $? -ne 0 ]; then return "$FALSE"; fi
  return "$TRUE"
}

get_all_versions() {
  curl -fsSL "$REPO/releases" | awk 'match(tolower($0),/.*"browser_download_url": "(.*\/'"${BIN}"'-release\.tar\.gz)".*/)' | awk -F '/' '{split($(NF-1), a, "\""); print a[1]}'
}

run_install_script() {
  if [ ! -f "$TEMP_INSTALL/release/install.sh" ]; then
    logerr "Can not find install script $TEMP_INSTALL/release/install.sh"
    return "$FALSE"
  fi

  log "Installing AquachemD $1"
  nohup "$TEMP_INSTALL/release/install.sh" "noinitmsg" >> "$OUTPUT" 2>&1
}

remove_install() {
  curl -fsSL -H "Accept: application/vnd.github.raw" "$REPO/contents/release/install.sh" | sudo bash -s -- clean
}

log_init_session "--- Starting AquachemD Installation/Upgrade ---"

main() {
  if ! tty > /dev/null 2>&1; then
    local script=$(basename "${0:-}")
    if [ "$script" == "bash" ] || [ "$script" == "" ]; then FROM_CURL=$TRUE; fi
  fi

  check_root_privileges
  echo "$SELF - $(date) " 2>/dev/null > "$OUTPUT"

  if check_can_upgrade; then
    installed_version
    if [[ "$INSTALLED_VERSION" != "" ]]; then
      log "Current AquachemD installation $INSTALLED_VERSION"
      log "System OK to install AquachemD latest release $REL_VERSION"
    else
      log "System OK to install AquachemD $REL_VERSION"
    fi
  else
    logerr "Can not upgrade, Please fix error(s)!"
    exit $FALSE
  fi

  case ${1:-latest} in
    local)
      # for debug purposes only
      cp -rfp ./release $TEMP_INSTALL
      cp -rfp ./web $TEMP_INSTALL
      run_install_script "local_Version"
    ;;
    #check|checkupgradable) exit $TRUE ;;
    #development)
    #  if ! latest_development_version; then logerr "getting development version"; exit "$FALSE"; fi
    #  if ! download_latest_development; then logerr "downloading latest development"; exit "$FALSE"; fi
    #  run_install_script "$DEV_VERSION"
    #;;
    clean|delete|remove)
      if ! remove_install; then logerr "Removing install"; exit "$FALSE"; fi
      log "Removed install"
    ;;
    list|versions) 
      get_all_versions
    ;;
    v*)
      if ! download_version "$1"; then logerr "downloading version $1"; exit "$FALSE"; fi
      run_install_script "$1"
    ;;
    -h|help|h)
      echo "AquachemD Installation script"
      echo "$SELF               <- download and install latest AquachemD version"
      echo "$SELF latest        <- download and install latest AquachemD version"
#      if [ "$USE_RELEASE_PKG" -eq $FALSE ]; then
#        echo "$SELF development   <- download and install latest AquachemD development version"
#      fi
      echo "$SELF clean         <- Remove AquachemD"
      echo "$SELF list          <- List available versions to install"
      echo "$SELF v1.0.0        <- install AquachemD v1.0.0"
    ;;
    latest|*)
      if ! download_latest_release; then logerr "downloading latest"; exit "$FALSE"; fi
      run_install_script "$REL_VERSION"
    ;;
  esac
}

main "$@"
