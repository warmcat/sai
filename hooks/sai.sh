#!/bin/bash

# this is a hook to run on your gitolite server when you push a branch
# it usually goes in ./local/VREF in your gitohashi config

REPO_URL_BASE="https://libwebsockets.org"
REPO_FETCH_URL_BASE="${REPO_URL_BASE}/repo"
REPO_WEB_URL_BASE="${REPO_URL_BASE}/git"
SAI_SERVER_BASE="https://libwebsockets.org:4444/sai/update-hook"


# json_escape <string>: emit a JSON string literal body with \, ", and control
# bytes escaped.  Git ref names and repository names can legally contain ", \,
# and control characters; interpolating them raw into the notification JSON
# would allow JSON injection (a pushed branch named foo","extra":"... could
# inject fields).  The whole payload is HMAC-signed afterwards, but escaping
# here keeps the structure unambiguous regardless of parser quirks.

json_escape() {
    printf '%s' "$1" | sed \
        -e 's/\\/\\\\/g' \
        -e 's/"/\\"/g' \
        -e 's/	/\\t/g' \
        -e ':a' -e 'N' -e '$!ba' -e 's/\n/\\n/g' \
        -e 's/\r/\\r/g'
}

if [ $(git rev-parse --is-bare-repository) = true ]
then
    RN=$(basename "$PWD")
else
    RN=$(basename $(readlink -nf "$PWD"/..))
fi

RN=${RN%.git}

# Pre-escape attacker-influenced fields once.  RN is derived from the repo
# directory name; $1 is the git ref; $3 is the new commit hash.

RN_E=$(json_escape "$RN")
REF_E=$(json_escape "$1")
HASH_E=$(json_escape "$3")



pwd

echo sai update hook $1 $RN_E...
if [ ! -z "`echo $1 | grep /_`" ] ; then
	echo "Detected temp ref starting with _, not passing to Sai"
	exit 0
fi

rm -f .sai.json .sai.json.b64
git show $3:.sai.json | base64 -w0 > .sai.json.b64
SJL=`stat .sai.json.b64 -c %s`


TF=`mktemp`

echo "{\"schema\":\"com-warmcat-sai-notification\"," > $TF
echo " \"action\":\"repo-update\"," >> $TF
echo " \"repository\":{" >> $TF
echo "   \"name\":\"${RN_E}\"," >> $TF
echo "   \"fetchurl\":\"$REPO_FETCH_URL_BASE/${RN_E}\"," >> $TF
echo "   \"weburl\":\"$REPO_WEB_URL_BASE/${RN_E}\"" >> $TF
echo " }," >> $TF
echo " \"nonce\":\"`dd if=/dev/urandom bs=32 count=1 | sha256sum | cut -d' ' -f1`\"," >> $TF
echo " \"ref\":\"$REF_E\"," >> $TF
echo " \"hash\":\"$HASH_E\"," >> $TF
echo " \"saifile_len\":${SJL}," >> $TF
echo -n " \"saifile\":\"" >> $TF
# disallow any nested JSON monkey business by base64-encoding it
cat .sai.json.b64 >> $TF
echo "\"" >> $TF
echo "}" >> $TF

HM=`cat $TF | openssl dgst -sha256 -hmac $(cat /etc/sai/private/lws-sai-notification-token2) | cut -d' ' -f2`

if [ $SJL = "0" ] ; then
	echo "No saifile"
	exit 0
fi

cat $TF
echo $HM
echo badline: -F"file=@${TF};type=application/json"

curl --header "authorization: sai sha256=$HM" \
    -F"file=@${TF};type=application/json" \
    -A "sai-notifier" -m 30 \
     ${SAI_SERVER_BASE}

rm -f $TF

exit 0


