#!/bin/sh

REPO_FETCH_URL_BASE="https://libwebsockets.org/repo"

if [ $(git rev-parse --is-bare-repository) = true ]
then
    RN=$(basename "$PWD")
else
    RN=$(basename $(readlink -nf "$PWD"/..))
fi

RN=${RN%.git}

echo sai update hook $RN...
rm -f .sai.json .sai.json.b64
git show $3 -- .sai.json | patch -p1 --merge
cat .sai.json
cat .sai.json | base64 -w0 > .sai.json.b64
SJL=`stat .sai.json.b64 -c %s`

if [ -z $SJL -o $SJL = "0" ] ; then
	cat /home/sai/.sai.json | base64 -w0 > .sai.json.b64
	SJL=`stat .sai.json.b64 -c %s`
fi



TF=`mktemp`

echo "{\"schema\":\"com-warmcat-sai-notification\"," > $TF
echo " \"action\":\"repo-update\"," >> $TF
echo " \"repository\":{" >> $TF
echo "   \"name\":\"$RN\"" >> $TF
echo "   \"fetchurl\":\"$REPO_FETCH_URL_BASE\"" >> $TF
echo " }," >> $TF
echo " \"nonce\":\"`dd if=/dev/urandom bs=32 count=1 | sha256sum | cut -d' ' -f1`\"," >> $TF
echo " \"ref\":\"$1\"," >> $TF
echo " \"hash\":\"$3\"," >> $TF
echo " \"saifile_len\":$SJL," >> $TF
echo -n " \"saifile\":\"" >> $TF
# disallow any nested JSON monkey business by base64-encoding it
cat .sai.json.b64 >> $TF
echo "\"" >> $TF
echo "}" >> $TF

HM=`cat $TF | sha256hmac -k /etc/sai/private/lws-sai-notification-token | cut -d' ' -f1`

if [ $SJL = "0" ] ; then
	echo "No saifile"
	exit 0
fi

cat $TF
echo $HM

curl --header "authorization: sai sha256=$HM" \
    -F"file=@$TF;type=application/json" \
    -A "sai-notifier" \
     https://warmcat.com/sai/update-hook
#curl  --header "X-Sai-Signature: sha256=$HM" -F"file=@$TF;name=notification;type=application/json" -A "sai-notifier"  http://127.0.0.1:4444

rm -f $TF

exit 0

