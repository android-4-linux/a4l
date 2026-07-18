#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl tooling. Converts the host system's public CA-root bundle into the
# per-file hashed layout AOSP conscrypt's TrustedCertificateStore reads (<subject_hash_old>.<n>),
# using the host openssl. The trust anchors are the machine's own public CA roots — not derived
# from AOSP or the predecessor. No GPL-3.0 sources.
#
# Without a CA trust store, AOSP conscrypt rejects every server certificate
# ("Trust anchor for certification path not found"), so all HTTPS in every app fails. acl points
# ANDROID_ROOT at the host ART tree, and conscrypt reads system CAs from
# $ANDROID_ROOT/etc/security/cacerts — this populates that directory.
#
# Usage:   tools/gen-cacerts.sh <dest-cacerts-dir> [ca-bundle.pem]
#   dest       the cacerts directory to populate (e.g. $ACL_ART_HOME/etc/security/cacerts)
#   ca-bundle  a PEM bundle of CA roots; auto-detected from common host paths when omitted
# Idempotent: does nothing if <dest> already holds certs, unless ACL_CACERTS_FORCE=1.
set -u

DEST="${1:-}"
[ -n "$DEST" ] || { printf 'usage: %s <dest-cacerts-dir> [ca-bundle.pem]\n' "$0" >&2; exit 2; }

# Already provisioned? Skip (cheap re-runs from the harness), unless forced.
if [ -z "${ACL_CACERTS_FORCE:-}" ] && [ -d "$DEST" ] && [ -n "$(ls -A "$DEST" 2>/dev/null)" ]; then
  exit 0
fi

# Locate a host CA bundle.
BUNDLE="${2:-}"
if [ -z "$BUNDLE" ]; then
  for c in /etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem \
           /etc/ssl/certs/ca-certificates.crt \
           /etc/ssl/certs/ca-bundle.crt \
           /etc/pki/tls/certs/ca-bundle.crt; do
    [ -f "$c" ] && { BUNDLE="$c"; break; }
  done
fi
[ -n "$BUNDLE" ] && [ -f "$BUNDLE" ] || { printf 'gen-cacerts: no host CA bundle found\n' >&2; exit 1; }
command -v openssl >/dev/null 2>&1 || { printf 'gen-cacerts: openssl not found\n' >&2; exit 1; }

mkdir -p "$DEST"

# Split the bundle into one PEM per cert (numbered temp files), then name each by its
# subject_hash_old (the OpenSSL pre-1.0 hash Android uses; a colliding subject bumps <n>).
# conscrypt looks up a trust anchor by the issuer's subject_hash_old, so the filename — not just
# the content — has to be right. Two phases (split, then hash+rename) so each openssl call is a
# plain forward pipe (an awk bidirectional coprocess to openssl deadlocks).
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
awk -v tmp="$TMP" '
  /-----BEGIN CERTIFICATE-----/ { inc = 1; n++; f = sprintf("%s/c%05d.pem", tmp, n) }
  inc { print > f }
  /-----END CERTIFICATE-----/ { inc = 0; close(f) }
' "$BUNDLE"

count=0
for pem in "$TMP"/c*.pem; do
  [ -f "$pem" ] || continue
  h=$(openssl x509 -subject_hash_old -noout -in "$pem" 2>/dev/null) || continue
  [ -n "$h" ] || continue
  i=0
  while [ -e "$DEST/$h.$i" ]; do i=$((i + 1)); done
  cp "$pem" "$DEST/$h.$i"
  count=$((count + 1))
done
printf 'gen-cacerts: wrote %d CA certs to %s\n' "$count" "$DEST" >&2
