# Sai web auth

## Authorization Overview

Sai uses signed JWTs

There's no UI at the moment for creating authorized users, everything except
event deletion and task restart works without authorization.

## sai-master configuration for auth

In the `vhosts|ws-protocols|com-warmcat-sai` section of the config JSON, the
following entries define the authorization operation

```
           "jwt-auth-alg":         "ES512",
           "jwt-auth-jwk-path":    "/etc/sai/master/auth.jwk",
           "jwt-iss":              "com.warmcat",
           "jwt-aud":              "https://libwebsockets.org/sai",
```

The `jwt-iss` and `jwt-aud` values go into generated JWTs and are confirmed
to match when receiving a JWT, these define the issuing authority and the
"audience", the receipient the JWT was created to be consumed by... the
audience should be the globally unique site base URI.

## Creating the server JWK

If you build lws with
`-DLWS_WITH_GENCRYPTO=1 -DLWS_WITH_JOSE=1 -DLWS_WITH_MINIMAL_EXAMPLES=1`
it will create a set of JOSE utilities, including one for JWK key generation.

Use this as below to create the server JWK used for signing and validation,
for ES512 algorithm in our case

```
$ sudo ./bin/lws-crypto-jwk -t EC -b512 -vP-521 --alg ES512 > /etc/sai/master/auth.jwk
```

## Defining an authorized user

Sai-master creates a separate auth database and prepares the table schema in
it on startup if not already existing.  So you using sai-master at all already
did most of the work.

To create a user that can login via his browser and see and use the UI for the
additional actions, currently you can create the user by hand on the server:

```
# sqlite3 /home/srv/sai/sai-master-auth.sqlite3
SQLite version 3.32.3 2020-06-18 14:00:33
Enter ".help" for usage hints.
sqlite> .schema
CREATE TABLE auth (_lws_idx integer, name varchar, passphrase varchar, since integer primary key autoincrement, last_updated integer);
CREATE TABLE sqlite_sequence(name,seq);
sqlite> insert into auth (_lws_idx, name, passphrase) values (0, "your@email.com", "somepassword");
```

Afterwards, it should be possible to log in from the web UI using the given
credentials and see the additional UI elements.
