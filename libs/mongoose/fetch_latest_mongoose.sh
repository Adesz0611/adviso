#!/usr/bin/env bash

set -e

cd "$(dirname "$0")"

curl -LsSf https://raw.githubusercontent.com/cesanta/mongoose/refs/heads/master/mongoose.h -o mongoose.h
curl -LsSf https://raw.githubusercontent.com/cesanta/mongoose/refs/heads/master/mongoose.c -o mongoose.c

echo "Mongoose updated in $(pwd)!"
