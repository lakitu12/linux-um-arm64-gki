#!/bin/bash
# apply-stack.sh <kernel-tree> <patches-dir> [report-dir]
#
# Applies the um-arm64 GKI patch stack in order with git am, so every
# patch's author/date survives into the tree's history. Stops at the first
# conflict and writes a readable report instead of leaving a half-applied
# tree: that report is exactly what a monthly GKI drift should produce.
#
# Exit codes: 0 = all applied, 8 = git am failed mid-way, 9 = apply check
# failed (conflict), 2 = bad usage/tree.
set -u
T=${1:?usage: apply-stack.sh <kernel-tree> <patches-dir> [report-dir]}
P=${2:?}
R=${3:-apply-report}

mkdir -p "$R"
cd "$T" || exit 2
git config user.name  "um-ci"
git config user.email "um-ci@um-arm64.invalid"
: > "$R/apply-report.tsv"

n=0
for pf in "$P"/*.patch; do
    n=$((n + 1))
    name=$(basename "$pf")
    if git apply --check --whitespace=nowarn "$pf" 2>"$R/last-error.txt"; then
        if git am --whitespace=nowarn "$pf" >/dev/null 2>>"$R/last-error.txt"; then
            printf '%s\tOK\n' "$name" >>"$R/apply-report.tsv"
        else
            printf '%s\tAM-FAIL\n' "$name" >>"$R/apply-report.tsv"
            {
                echo "git am failed on: $name"
                echo
                tail -20 "$R/last-error.txt"
            } >"$R/CONFLICT.txt"
            cat "$R/CONFLICT.txt"
            exit 8
        fi
    else
        printf '%s\tCONFLICT\n' "$name" >>"$R/apply-report.tsv"
        {
            echo "First conflict: $name"
            echo
            echo "== git apply --check stderr:"
            cat "$R/last-error.txt"
            echo
            echo "== tree state:"
            git status --short | head -30
        } >"$R/CONFLICT.txt"
        cat "$R/CONFLICT.txt"
        exit 9
    fi
done

echo "patch stack applied OK: $n/$n"
git log --oneline -4 >"$R/git-log.txt"
cat "$R/git-log.txt"