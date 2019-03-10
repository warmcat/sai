#!/bin/sh

DEST=/usr/local/share/sai
if [ ! -z "$1" ] ; then
	DEST=$1
fi

sudo openssl ecparam -name secp256r1 -genkey -out $DEST/sai-selfsigned-key.pem
echo -e "\n\n\n\n\n\n" | \
sudo openssl req -new -x509 \
	-sha256 \
	-subj "/CN=localhost" \
	-days 10000 \
	-key $DEST/sai-selfsigned-key.pem \
	-out $DEST/sai-selfsigned-cert.pem
echo

