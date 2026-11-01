#!/usr/bin/env bash
# Builds the overlay's non-UI code for the host and runs it against a mock Spotify API.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=../..
WORK=$(mktemp -d)
trap 'kill $MOCK_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

"${CXX:-g++}" -std=c++23 -O1 -g -Wall -Wextra -Wno-missing-field-initializers -fsanitize=address,undefined \
    -DSPOTIFY_API_BASE='"http://127.0.0.1:18080/v1"' \
    -DSPOTIFY_TOKEN_URL='"http://127.0.0.1:18080/api/token"' \
    -Istub -I$ROOT/source -I$ROOT/lib/libultrahand/common \
    test_main.cpp $ROOT/source/{util,log,config,http,spotify,remote,setup_server}.cpp \
    $ROOT/lib/libultrahand/common/cJSON.c \
    -lcurl -lpthread -o "$WORK/test"

python3 mock_spotify.py 18080 & MOCK_PID=$!
sleep 0.5
(cd "$WORK" && ./test)
