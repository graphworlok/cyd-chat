#!/usr/bin/env bash
# chat-cert.sh - issue / renew a Let's Encrypt cert for $DOMAIN (HTTP-01 via webroot, no root).
#
# Usage: DOMAIN=chat.example.com [WEBROOT=/var/www/acme] chat-cert.sh <command>
#
# Needs (one-time): a public A record for $DOMAIN -> this host, a web server vhost for $DOMAIN
# serving $WEBROOT on port 80, and write access to $WEBROOT/.well-known/acme-challenge.
#
#   chat-cert.sh check      pre-flight: DNS, webroot write, HTTP fetch
#   chat-cert.sh staging    issue a TEST cert from LE staging (not browser-trusted; proves the pipeline)
#   chat-cert.sh issue      issue the real cert (ECDSA P-256)
#   chat-cert.sh renew      renew if <30 days left (safe to run from cron)
#   chat-cert.sh export     copy cert+key to ~/chat-cert/out/ and print the scp command
#   chat-cert.sh status     show cert expiry
#
# Optional: EMAIL=you@example.com chat-cert.sh issue   (expiry notices; otherwise registered without email)
set -euo pipefail

DOMAIN="${DOMAIN:?set DOMAIN=chat.example.com}"
WEBROOT="${WEBROOT:-/var/www/acme}"
BASE="$HOME/chat-cert"
OUT="$BASE/out"
cmd="${1:-help}"

if [ "$cmd" = staging ]; then LE="$BASE/le-staging"; EXTRA=(--test-cert); else LE="$BASE/le"; EXTRA=(); fi
CB=(certbot --config-dir "$LE" --work-dir "$BASE/work" --logs-dir "$BASE/logs")
mkdir -p "$BASE"/{work,logs} "$OUT"; chmod 700 "$BASE"

contact() { if [ -n "${EMAIL:-}" ]; then echo -m "$EMAIL"; else echo --register-unsafely-without-email; fi; }

check() {
  local ip pub tok="$WEBROOT/.well-known/acme-challenge/.preflight-$$"
  ip=$(dig +short @127.0.0.1 "$DOMAIN" A | head -1)
  pub=$(dig +short @1.1.1.1 "$DOMAIN" A | head -1)
  echo "authoritative: ${ip:-NONE}   public(1.1.1.1): ${pub:-NONE}"
  [ -n "$pub" ] || { echo "FAIL: $DOMAIN does not resolve publicly"; return 1; }
  echo ok > "$tok" || { echo "FAIL: cannot write $WEBROOT/.well-known/acme-challenge"; return 1; }
  local got; got=$(curl -fsS --max-time 10 --resolve "$DOMAIN:80:$pub" "http://$DOMAIN/.well-known/acme-challenge/$(basename "$tok")" || true)
  rm -f "$tok"
  [ "$got" = ok ] && echo "OK: webroot write + HTTP fetch works" || { echo "FAIL: HTTP fetch of challenge file did not return the test token"; return 1; }
}

issue() {
  check
  # shellcheck disable=SC2046
  "${CB[@]}" certonly --webroot -w "$WEBROOT" -d "$DOMAIN" --cert-name "$DOMAIN" \
    --key-type ecdsa --elliptic-curve secp256r1 --non-interactive --agree-tos $(contact) "${EXTRA[@]}"
}

case "$cmd" in
  check)   check ;;
  staging) issue ;;
  issue)   issue ;;
  renew)   "${CB[@]}" renew --non-interactive ;;
  status)  for d in le le-staging; do f="$BASE/$d/live/$DOMAIN/fullchain.pem"
             [ -f "$f" ] && { echo "$d: $(openssl x509 -in "$f" -noout -enddate -issuer | tr '\n' ' ')"; }; done ;;
  export)  src="$BASE/le/live/$DOMAIN"
           [ -f "$src/fullchain.pem" ] || { echo "no production cert yet (run: $0 issue)"; exit 1; }
           install -m 644 "$src/fullchain.pem" "$OUT/$DOMAIN.fullchain.pem"
           install -m 600 "$src/privkey.pem"   "$OUT/$DOMAIN.key.pem"
           echo "exported to $OUT"
           echo "pull with:  scp $(whoami)@$(hostname -f):$OUT/'$DOMAIN.*.pem' ." ;;
  *)       sed -n '2,15p' "$0" ;;
esac
