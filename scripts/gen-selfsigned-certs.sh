#!/bin/sh

DEST=/usr/local/share/sai
if [ ! -z "$1" ] ; then
	DEST=$1
fi

sudo openssl ecparam -name secp256r1 -genkey -out $DEST/sai-selfsigned-key.pem.1
echo -e "\n\n\n\n\n\n" | \
sudo openssl req -new -x509 \
	-sha256 \
	-subj "/CN=localhost" \
	-days 10000 \
	-key $DEST/sai-selfsigned-key.pem.1  \
	-out $DEST/sai-selfsigned-cert.pem

#
# mbedtls can't handle the extra EC parameters stanza
#
sudo sed '/^-----BEGIN\ EC\ PARAMETERS-----/,/^-----END\ EC\ PARAMETERS-----/{/^#/!{/^\$/!d}}' $DEST/sai-selfsigned-key.pem.1 > sai-selfsigned-key.pem
sudo cp sai-selfsigned-key.pem $DEST
sudo rm sai-selfsigned-key.pem
echo

