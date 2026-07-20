# NOAH=OK is guest test data, not a reference to the old command name: the
# prebuilt binary under build/ greps its environment for that exact string (see
# env.c), and it cannot be rebuilt without a Linux toolchain. Renaming it here
# would fail the test. $NABI is the runner, and is a separate thing entirely.
NOAH=OK $NABI $TARGET
