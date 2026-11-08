#!/usr/bin/env bash
# One-shot questions answered by tfs against the usual tools, best of N runs (warm caches), as a Markdown table.
#   bench/bench.sh [ROOT] [CORPUS]
#     ROOT    tree for the metadata questions (default /usr)
#     CORPUS  tree for the content and duplicate questions (default ROOT; "-" to skip); it is read once to build the store
# Every pair of answers is compared; a differing pair is marked in the last column. On a tree that changes while the
# benchmark runs, set WATCH=1 so that tfs watch keeps the index current (answers can still differ by in-flight writes).
unset -f find grep rg wc 2>/dev/null
export LC_ALL=C
HERE=$(cd "$(dirname "$0")/.." && pwd); TFS=$HERE/native/tfs; make -s -C "$HERE" native >/dev/null || exit 1
ROOT=$(realpath "${1:-/usr}"); CORPUS=$(realpath "${2:-$ROOT}"); N=${N:-5}
FIND=/usr/bin/find; GREP=/usr/bin/grep
pick() { for c in "$@"; do command -v "$c" >/dev/null && "$c" --version 2>/dev/null | $GREP -q "GNU coreutils" && { echo "$c"; return; }; done; echo "$1"; }
WC=$(pick gnuwc wc)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT; I=$W/i.tfsn; J=$W/c.tfsn
best() {                 # best wall time of N runs, in ms; the last run's output goes to $W/out
    local b=999999999999999 t0 t1 i
    for ((i = 0; i < N; i++)); do t0=$(date +%s%N); bash -c "$1" > "$W/out" 2>/dev/null; t1=$(date +%s%N); (( (t1 - t0) < b )) && b=$((t1 - t0)); done
    awk -v n="$b" 'BEGIN { v = n / 1e6; printf (v < 10 ? "%.1f" : "%.0f"), v }'
}
row() {                  # QUESTION TFS_CMD BASELINE_NAME BASELINE_CMD [NORMALIZE]
    local a b oa ob norm=${5:-cat}
    a=$(best "$2"); oa=$(eval "$norm" < "$W/out"); b=$(best "$4"); ob=$(eval "$norm" < "$W/out")
    printf '| %s | **%s ms** | %s %s ms | %s |\n' "$1" "$a" "$3" "$b" "$([[ $oa == "$ob" ]] && echo same || echo DIFFERENT)"
}
nent=$($FIND "$ROOT" -xdev | wc -l)
t0=$(date +%s%N); "$TFS" build -o "$I" "$ROOT" >/dev/null 2>&1; t1=$(date +%s%N)
if [[ -n ${WATCH:-} ]]; then "$TFS" -i "$I" watch --save-every 0.2 2>/dev/null & WPID=$!; trap 'kill $WPID 2>/dev/null; rm -rf "$W"' EXIT; sleep 1; fi
echo "## $ROOT ($nent entries): build $(( (t1 - t0) / 1000000 )) ms, index $(du -k "$I" | cut -f1) KB"
echo; echo "| question | tfs | usual way | answers |"; echo "|---|---|---|---|"
row "bytes of everything below (apparent)" "$TFS -i $I du $ROOT | cut -d' ' -f1 | tr -d ," "find -printf \| awk" \
    "$FIND $ROOT -xdev -mindepth 1 -printf '%s\n' | awk '{s += \$1} END {print s + 0}'"
row "count \`.py\` files" "$TFS -i $I find $ROOT -type f -name '*.py' --count" "find \| wc -l" "$FIND $ROOT -xdev -type f -name '*.py' | wc -l"
row "total bytes of \`*.so*\` files" "$TFS -i $I find $ROOT -type f -name '*.so*' --bytes" "find -printf \| awk" \
    "$FIND $ROOT -xdev -type f -name '*.so*' -printf '%s\n' | awk '{s += \$1} END {print s + 0}'"
row "files over 1 MB" "$TFS -i $I find $ROOT -type f -size +1M --count" "find \| wc -l" "$FIND $ROOT -xdev -type f -size +1M | wc -l"
row "newest file" "$TFS -i $I find $ROOT -type f --newest" "find -printf \| sort \| tail" \
    "$FIND $ROOT -xdev -type f -printf '%T@ %p\n' | sort -n | tail -1 | cut -d' ' -f2-"
row "empty directories" "$TFS -i $I find $ROOT -type d -empty --count" "find \| wc -l" "$FIND $ROOT -xdev -type d -empty | wc -l"
row "paths containing \`python3\`" "$TFS -i $I locate -c --root $ROOT python3" "find -path \| wc -l" "$FIND $ROOT -xdev -path '*python3*' | wc -l"
row "files per extension" "$TFS -i $I find $ROOT -type f --group-by ext | wc -l" "find -printf \| awk" \
    "$FIND $ROOT -xdev -type f -printf '%f\n' | awk '{n = \$0; sub(/^\\.+/, \"\", n); i = match(n, /\\.[^.]*\$/); e = i ? substr(n, i) : \"\"; c[e]++} END {print length(c)}'"

[[ ${2:-} == - ]] && exit 0                        # CORPUS "-": metadata only
echo; t0=$(date +%s%N); "$TFS" build -o "$J" "$CORPUS" >/dev/null 2>&1; "$TFS" -i "$J" content refresh "$CORPUS" > "$W/st"; t1=$(date +%s%N)
echo "## content of $CORPUS: store built in $(( (t1 - t0) / 1000000 )) ms ($(du -k "$J.content" | cut -f1) KB; $(cat "$W/st"))"
echo; echo "| question | tfs | usual way | answers |"; echo "|---|---|---|---|"
row "line / word / byte totals" "$TFS -i $J content wc -s $CORPUS | cut -d' ' -f1-3" "find \| wc --files0-from" \
    "$FIND $CORPUS -xdev -type f -print0 | $WC -lwc --files0-from=- | tail -1 | awk '{print \$1, \$2, \$3}'"
row "git blob ids of every file" "$TFS -i $J content hash $CORPUS | cut -c1-40 | sort | md5sum" "git hash-object --stdin-paths" \
    "$FIND $CORPUS -xdev -type f | git hash-object --no-filters --stdin-paths | sort | md5sum"
for p in ${PATTERNS:-SHA1_Init TODO}; do
    row "files containing \`$p\` (preview)" "$TFS -i $J content grep '$p' $CORPUS | sort" "grep -rlF" "$GREP -rlF -- '$p' $CORPUS | sort"
done
if command -v fdupes >/dev/null; then
    "$TFS" -i "$J" dupes -r --store "$W/dc" "$CORPUS" >/dev/null
    row "duplicate files (digests cached)" "$TFS -i $J dupes -r --store $W/dc $CORPUS" "fdupes -r" "fdupes -r -q $CORPUS" \
        "awk 'BEGIN { RS = \"\" } { n = split(\$0, a, \"\\n\"); asort(a); s = \"\"; for (i = 1; i <= n; i++) s = s a[i] \"|\"; print s }' | sort"
fi
