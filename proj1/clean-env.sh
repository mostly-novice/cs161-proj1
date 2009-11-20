#!/bin/sh
F="$1"
shift

DIR=$(cd -P -- "$(dirname -- "$F")" && pwd -P)
FILE=$(basename -- "$F")
exec env - PWD=$DIR SHLVL=0 $DIR/$FILE "$@"
