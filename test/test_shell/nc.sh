#!/bin/bash

$NABI $TARGET -l 8082 <<<"hello" & 
RECEIVE="`$NABI $TARGET localhost 8082`"
test "hello" = "$RECEIVE" || exit 1
