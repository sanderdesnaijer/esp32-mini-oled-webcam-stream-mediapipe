#!/usr/bin/env bash
#
# Generate a self-signed certificate + key for the ESP32 HTTPS server
# and print them as ready-to-paste C string literals.
#
# Usage:
#   bash scripts/generate_cert.sh
#
# Requires: openssl (preinstalled on macOS and Linux; on Windows use
# Git Bash or WSL).
#
# The output goes to stdout. Copy the two blocks and paste them into
# browser-oled.ino, replacing the placeholder cert_pem[] and key_pem[]
# contents.

set -euo pipefail

if ! command -v openssl >/dev/null 2>&1; then
  echo "ERROR: openssl not found. Install it first." >&2
  exit 1
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout "$TMPDIR/key.pem" \
  -out    "$TMPDIR/cert.pem" \
  -days 3650 \
  -subj "/CN=esp32.local" \
  2>/dev/null

format_pem () {
  # turn a PEM file into "line\n" C string literals
  local file="$1"
  while IFS= read -r line; do
    printf '"%s\\n"\n' "$line"
  done < "$file"
}

echo
echo "// -------- paste into cert_pem[] --------"
echo "static const char cert_pem[] ="
format_pem "$TMPDIR/cert.pem"
echo ";"
echo
echo "// -------- paste into key_pem[] --------"
echo "static const char key_pem[] ="
format_pem "$TMPDIR/key.pem"
echo ";"
echo
echo "// Generated $(date)." >&2
echo "// Valid for 10 years. CN=esp32.local, 2048-bit RSA." >&2
