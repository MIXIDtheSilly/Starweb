#!/usr/bin/env bash
# Generate a local StarWeb root CA and a localhost server certificate for star://.
# Output goes to certs/ (gitignored). Re-running reuses an existing root CA so the
# trust anchor stays stable, and regenerates the leaf. Pass --force to rebuild the
# root as well.
#
#.  certs/starweb_root.pem.  root CA cert.  -> trusted by the browser/client
#.  certs/starweb_root.key.  root CA key.   -> signs leaves; never share this
#.  certs/localhost.pem.     server cert.   -> served by stwp_server on 8490
#.  certs/localhost.key.     server key
set -euo pipefail

cd "$(dirname "$0")/.."
CERTS=certs
mkdir -p "$CERTS"

ROOT_KEY=$CERTS/starweb_root.key
ROOT_CRT=$CERTS/starweb_root.pem
LEAF_KEY=$CERTS/localhost.key
LEAF_CRT=$CERTS/localhost.pem
LEAF_CSR=$CERTS/localhost.csr

FORCE=0
NEW_KEY=0
for arg in "$@"; do
    case "$arg" in
        --force)   FORCE=1 ;;
        --new-key) FORCE=1; NEW_KEY=1 ;;
    esac
done

# Keeps the CA inside StarWeb: it cannot issue for a public name or address, so
# installing this root can't expose the real web even if the key leaks.
# .star stays permitted alongside .web so leaves issued before the zone rename
# keep verifying against a re-signed root.
NAME_CONSTRAINTS="critical\
,permitted;DNS:localhost\
,permitted;DNS:.local\
,permitted;DNS:.web\
,permitted;DNS:.star\
,permitted;IP:127.0.0.0/255.0.0.0\
,permitted;IP:10.0.0.0/255.0.0.0\
,permitted;IP:172.16.0.0/255.240.0.0\
,permitted;IP:192.168.0.0/255.255.0.0\
,permitted;IP:0:0:0:0:0:0:0:1/ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff\
,permitted;IP:fc00:0:0:0:0:0:0:0/fe00:0:0:0:0:0:0:0\
,permitted;IP:fe80:0:0:0:0:0:0:0/ffc0:0:0:0:0:0:0:0"

if [ "$FORCE" = 1 ] || [ ! -f "$ROOT_KEY" ] || [ ! -f "$ROOT_CRT" ]; then
    # Re-signing with the existing key keeps every already-issued leaf valid:
    # same key and same subject means the old chains still verify, and only the
    # constraints change. --new-key forces a clean break instead.
    if [ "$NEW_KEY" = 1 ] || [ ! -f "$ROOT_KEY" ]; then
        echo "Generating StarWeb root CA (new key)..."
        openssl ecparam -name prime256v1 -genkey -noout -out "$ROOT_KEY"
    else
        echo "Re-signing StarWeb root CA (keeping existing key)..."
        cp "$ROOT_CRT" "$ROOT_CRT.bak" 2>/dev/null || true
    fi
    openssl req -x509 -new -key "$ROOT_KEY" -sha256 -days 3650 \
        -subj "/O=StarWeb/CN=StarWeb Root CA" \
        -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" \
        -addext "subjectKeyIdentifier=hash" \
        -addext "nameConstraints=$NAME_CONSTRAINTS" \
        -out "$ROOT_CRT"
else
    echo "Reusing existing root CA ($ROOT_CRT). Pass --force to regenerate it."
    if ! openssl x509 -in "$ROOT_CRT" -noout -ext nameConstraints 2>/dev/null | grep -q Permitted; then
        echo "  WARNING: this root has no name constraints — it can sign for any name."
        echo "           Re-run with --force to replace it with a constrained root."
    fi
fi

echo "Generating localhost server certificate..."
openssl ecparam -name prime256v1 -genkey -noout -out "$LEAF_KEY"
openssl req -new -key "$LEAF_KEY" -sha256 \
    -subj "/O=StarWeb/CN=localhost" -out "$LEAF_CSR"

EXT=$(mktemp)
cat > "$EXT" <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:0:0:0:0:0:0:0:1
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid,issuer
EOF

openssl x509 -req -in "$LEAF_CSR" -CA "$ROOT_CRT" -CAkey "$ROOT_KEY" \
    -CAcreateserial -days 825 -sha256 -extfile "$EXT" -out "$LEAF_CRT"

rm -f "$EXT" "$LEAF_CSR"

echo
echo "Verifying chain:"
openssl verify -CAfile "$ROOT_CRT" "$LEAF_CRT"

echo
echo "Done. Serve with:  ./stwp_server --tls-port 8490 --cert $LEAF_CRT --key $LEAF_KEY"
