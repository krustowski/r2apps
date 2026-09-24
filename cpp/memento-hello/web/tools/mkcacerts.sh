#!/bin/sh
#
#  mkcacerts.sh --- regenerates ../cacerts.bin, the roots the browser trusts.
#
#  Usage: tools/mkcacerts.sh [bundle.pem] [iso-dir]
#
#  The browser reads the file at run time from /mnt/iso/opt/memento/cacerts.bin,
#  so it lives on the CD rather than in Memento's 2 MiB image.  Give the
#  kernel's ISO tree (r2_main/iso) as the second argument and the file is put
#  in its opt/memento/ as well; r2_main's build_iso copies it from here too.
#
#  The bundle defaults to the system's.  The roots kept are the ones named in
#  ROOTS below, matched on the certificate's CN: the CAs that issue for most
#  of the web --- Let's Encrypt, DigiCert, Google, Amazon, Sectigo, GlobalSign,
#  Microsoft, GoDaddy.  To trust the whole bundle instead, run
#  tests/build/mkcacerts ../cacerts.bin <bundle.pem> directly.
#
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUNDLE=${1:-/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem}
[ -f "$BUNDLE" ] || BUNDLE=/etc/ssl/certs/ca-certificates.crt
ISO=$2

ROOTS='Amazon Root CA 1
Amazon Root CA 2
Amazon Root CA 3
Amazon Root CA 4
Certum Trusted Network CA
AAA Certificate Services
DigiCert Global Root CA
DigiCert Global Root G2
DigiCert Global Root G3
DigiCert High Assurance EV Root CA
DigiCert TLS ECC P384 Root G5
DigiCert TLS RSA4096 Root G5
Entrust Root Certification Authority - G2
GTS Root R1
GTS Root R2
GTS Root R3
GTS Root R4
GlobalSign
GlobalSign Root CA
GlobalSign Root E46
GlobalSign Root R46
Go Daddy Root Certificate Authority - G2
ISRG Root X1
ISRG Root X2
Microsoft ECC Root Certificate Authority 2017
Microsoft RSA Root Certificate Authority 2017
SSL.com Root Certification Authority ECC
SSL.com Root Certification Authority RSA
Sectigo Public Server Authentication Root E46
Sectigo Public Server Authentication Root R46
Starfield Root Certificate Authority - G2
USERTrust ECC Certification Authority
USERTrust RSA Certification Authority'

make -s -C "$HERE/tests" build/mkcacerts

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
awk -v dir="$TMP" '/BEGIN CERT/{n++; f=sprintf("%s/%04d.pem", dir, n)} n{print > f}' "$BUNDLE"
: > "$TMP/selected.pem"
for f in "$TMP"/[0-9]*.pem; do
    cn=$(openssl x509 -in "$f" -noout -subject -nameopt multiline | sed -n 's/^ *commonName *= *//p')
    if printf '%s\n' "$ROOTS" | grep -qxF "$cn"; then
        cat "$f" >> "$TMP/selected.pem"
    fi
done

"$HERE/tests/build/mkcacerts" "$HERE/cacerts.bin" "$TMP/selected.pem"
if [ -n "$ISO" ]; then
    mkdir -p "$ISO/opt/memento"
    cp "$HERE/cacerts.bin" "$ISO/opt/memento/cacerts.bin"
    echo "copied to $ISO/opt/memento/cacerts.bin"
fi
