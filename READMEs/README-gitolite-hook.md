# gitolite hook

```
#!/bin/sh

if [ $(git rev-parse --is-bare-repository) = true ]
then
    RN=$(basename "$PWD")
else
    RN=$(basename $(readlink -nf "$PWD"/..))
fi

RN=${RN%.git}

echo sai update hook $RN...
rm -f .sai.json
git show $3 -- .sai.json | patch -p1 --merge
cat .sai.json

TF=`mktemp`

echo "{\"schema\":\"com-warmcat-sai-notification\"," > $TF
echo " \"action\":\"repo-update\"," >> $TF
echo " \"repository\":{" >> $TF
echo "   \"name\":\"$RN\"" >> $TF
echo " }," >> $TF
echo " \"nonce\":\"`dd if=/dev/urandom bs=32 count=1 | sha256sum | cut -d' ' -f1`\"," >> $TF
echo " \"ref\":\"$1\"," >> $TF
echo " \"hash\":\"$3\"" >> $TF
echo -n " \"saifile\":\"" >> $TF
# disallow any nested JSON monkey business by base64-encoding it
cat .sai.json | base64 >> $TF
echo "\"" >> $TF
echo "}" >> $TF

HM=`cat $TF | sha256hmac -k /etc/sai/private/lws-sai-notification-token | cut -d' ' -f1`

cat $TF
echo $HM

curl --header "authorization: sai sha256=$HM" \
    -F"file=@$TF;type=application/json" \
    -A "sai-notifier" \
     https://warmcat.com/sai/update-hook
#curl  --header "X-Sai-Signature: sha256=$HM" -F"file=@$TF;name=notification;type=application/json" -A "sai-notifier"  http://127.0.0.1:4444

rm -f $TF

exit 0
```
