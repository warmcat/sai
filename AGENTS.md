# Sai AGENTS.md

## Overview

Please make high quality, not lazy, implementation decisions, bearing in mind
maintainability.

Our work should follow the existing usage of apis in the project as much as possible.

## Coding

We avoid things like `scanf` for carefully parsing with code, eg with `lws_tokenize` or similar.

We avoid `FILE *` and use apis like open(), read().

We avoid casual linked-lists and use `lws_dll2_t`.

We consider using lwsac instead of discrete allocations, if the pattern of allocations will benefit from it.

We consider using lws_struct to convert between sqlite storage <-> structs <-> JSON

## Security

Please bear in mind what parts of the system are secrets and look after the security of them.

In particular, all web pieces are made available on the internet with a strict CSP.  That means
no inline styles or scripts.  You can find the web pieces (JS, HTML, css) in ./assets/

## Churn management

When you produced fixes, if possible (main branch, target is within 8 patches back,
no intervening non-sai- tag) it's preferable to --amend apply the fixes directly to
the patch that originated the problem, essentially editing the history, even if
it means just doing that and not adding any fix patch or explanation.  Similarly, unless
asked to produce a new patch or the goal is an explicit phased series, if it's on the
main branch and we are iterating on the same work, and HEAD patch is yours from the
last iteration, it's preferable to directly use --amend on it to commit.

If the changes are for mixed purposes, if you initiate new core library changes or fixes,
these should be broken out into their own patch, even if the rest relates to recent
changes and is squashed in with those.


## Build testing

Please don't worry about build-testing, just push patches when you are confident they are complete and have considered all affected code (ie, not half-assed) and ready and I will try them and report back with grounded information.

If you really want to build-test:

Sai dependencies:

 - libwebsockets from main branch
    - libssl-dev
    - libsqlite3
    - Build lws:  `git init && git fetch https://github.com/warmcat/libwebsockets +main:main && cd libwebsockets && mkdir -p build && cd build && cmake .. -DLWS_UNIX_SOCK=1 -DLWS_WITH_STRUCT_JSON=1 -DLWS_WITH_JOSE=1 -DLWS_WITH_STRUCT_SQLITE3=1 -DLWS_WITH_GENCRYPTO=1 -DLWS_WITH_SPAWN=1 -DLWS_WITH_SECURE_STREAMS=1 && make && sudo make install && sudo ldconfig`

 - sai (main branch shown)
    - Build sai:  `git init && git fetch https://github.com/warmcat/sai +main:m && cd libwebsockets && mkdir -p build && cd build && cmake .. && make && sudo make install && sudo ldconfig`


