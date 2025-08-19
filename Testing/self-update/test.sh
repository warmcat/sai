#!/bin/bash

set -e

# Path to the .sai.json file
SAI_JSON_PATH="Testing/self-update/.sai.json"
if [ ! -f "$SAI_JSON_PATH" ]; then
    echo "Error: $SAI_JSON_PATH not found."
    exit 1
fi

# Base64 encode the .sai.json file
SAI_JSON_B64=$(base64 -w 0 < "$SAI_JSON_PATH")
SAI_JSON_LEN=${#SAI_JSON_B64}

# Notification details
REPO_NAME="sai-self-update-test"
FETCH_URL="file:///var/tmp/sai-self-update-test"
REF="refs/heads/master"
HASH="deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
# This key must match the one in the sai-server config
NOTIFICATION_KEY="51b3ee2f06ef2a893cfe901972bd13065d7dbae4cf087b396ee38e7bf78f79a6"
SERVER_URL="http://127.0.0.1:4444/update-hook"

# Construct the notification JSON payload
# Using a heredoc for readability
read -r -d '' JSON_PAYLOAD <<EOF
{
    "schema": "com-warmcat-sai-notification",
    "action": "repo-update",
    "repository": {
        "name": "$REPO_NAME",
        "fetchurl": "$FETCH_URL"
    },
    "ref": "$REF",
    "hash": "$HASH",
    "saifile_len": $SAI_JSON_LEN,
    "saifile": "$SAI_JSON_B64"
}
EOF

# The server expects the payload in a temporary file as part of a multipart form.
TMP_JSON_FILE=$(mktemp)
# The server code calculates the HMAC of the raw file content.
printf "%s" "$JSON_PAYLOAD" > "$TMP_JSON_FILE"

# Calculate the HMAC-SHA256 signature of the JSON payload
SIGNATURE=$(openssl dgst -sha256 -hmac "$NOTIFICATION_KEY" < "$TMP_JSON_FILE" | awk '{print $2}')

echo "Generated Signature: $SIGNATURE"

# Send the POST request using curl
echo "Sending notification to $SERVER_URL"
curl -k -i \
     -H "Authorization: sai sha256=$SIGNATURE" \
     -F "notification=@$TMP_JSON_FILE" \
     "$SERVER_URL"

# Clean up the temporary file
rm "$TMP_JSON_FILE"

echo "Notification sent."
