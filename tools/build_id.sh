#!/bin/sh
# The build's branch and commit for core's log ("<branch> <commit>", "-dirty" after the commit with local changes in the
# tracked files); "unknown" outside a git checkout. Under WSL a Windows git worktree is not readable by the Linux git
# (its .git file holds a Windows path): then git.exe is tried.
for g in git git.exe; do
    b=$($g rev-parse --abbrev-ref HEAD 2>/dev/null | tr -d '\r') || continue
    [ -n "$b" ] || continue
    c=$($g rev-parse --short=7 HEAD 2>/dev/null | tr -d '\r')
    $g diff --quiet HEAD -- 2>/dev/null || c="$c-dirty"
    echo "$b $c"
    exit 0
done
echo unknown
