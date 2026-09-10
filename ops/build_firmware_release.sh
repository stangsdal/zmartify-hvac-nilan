#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_DIR="${1:-$REPO_ROOT/build/release}"
EDGE_ROOT="${ZMARTIFY_EDGE_ROOT:-$(cd "$REPO_ROOT/../.." && pwd)/zmartify-edge}"
CATALOG_ROOT="${ZMARTIFY_FIRMWARE_CATALOG_DIR:-$EDGE_ROOT/zmartify-admin/public/firmware}"
PUBLISHER="$EDGE_ROOT/zmartify-admin/scripts/publish-firmware-release.mjs"

version="$(sed -n 's/^CONFIG_APP_PROJECT_VER="\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)"$/\1/p' "$REPO_ROOT/sdkconfig.defaults")"
if [[ -z "$version" ]]; then
  echo "Unable to read CONFIG_APP_PROJECT_VER from sdkconfig.defaults" >&2
  exit 1
fi
if [[ ! -f "$PUBLISHER" ]]; then
  echo "Firmware catalog publisher not found: $PUBLISHER" >&2
  exit 1
fi
if [[ -z "${IDF_PYTHON_ENV_PATH:-}" || ! -x "$IDF_PYTHON_ENV_PATH/bin/python" ]]; then
  echo "ESP-IDF Python environment is unavailable; source the ESP-IDF export.sh first" >&2
  exit 1
fi

cd "$REPO_ROOT"
if [[ "${ZMARTIFY_SKIP_BUILD:-0}" != "1" ]]; then
  idf.py build
fi

built_version="$(sed -n 's/^[[:space:]]*"project_version":[[:space:]]*"\([^"]*\)".*/\1/p' build/project_description.json)"
if [[ "$built_version" != "$version" ]]; then
  echo "Built version $built_version does not match CONFIG_APP_PROJECT_VER $version" >&2
  exit 1
fi

bootloader="$REPO_ROOT/build/bootloader/bootloader.bin"
partition_table="$REPO_ROOT/build/partition_table/partition-table.bin"
ota_data="$REPO_ROOT/build/ota_data_initial.bin"
application="$REPO_ROOT/build/zmartify_hvac_nilan.bin"
for artifact in "$bootloader" "$partition_table" "$ota_data" "$application"; do
  if [[ ! -s "$artifact" ]]; then
    echo "Missing build artifact: $artifact" >&2
    exit 1
  fi
done

mkdir -p "$OUTPUT_DIR"
factory_image="$OUTPUT_DIR/zmartify-hvac-nilan-$version.factory.bin"
"$IDF_PYTHON_ENV_PATH/bin/python" -m esptool --chip esp32s3 merge-bin \
  --output "$factory_image" \
  --flash-mode dio \
  --flash-freq 80m \
  --flash-size 16MB \
  0x0 "$bootloader" \
  0x8000 "$partition_table" \
  0x19000 "$ota_data" \
  0x20000 "$application"

node "$PUBLISHER" \
  --controller nilan \
  --version "$version" \
  --ota "$application" \
  --recovery "$factory_image" \
  --chip-family ESP32-S3 \
  --catalog-root "$CATALOG_ROOT"

(cd "$EDGE_ROOT/zmartify-admin" && npm run firmware:validate)
printf 'NILAN release %s built and published to %s\n' "$version" "$CATALOG_ROOT"