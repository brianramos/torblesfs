#!/usr/bin/env bash
# torblesFS test suite. Every answer of tfs / tfsd is compared with the live filesystem or the original tool:
# GNU find (the index itself, find, locate, ls, du), stat(1), GNU wc, git hash-object, GNU grep, and a brute-force
# duplicate finder. Tests that need FUSE, git, GNU wc or a prepared btrfs disk are skipped when those are missing.
#   tests/run.sh [TEST...]        (default: all; names below, e.g. tests/run.sh find content)
unset -f find grep rg wc 2>/dev/null
export LC_ALL=C.UTF-8 TZ=UTC
HERE=$(cd "$(dirname "$0")/.." && pwd); TFS=$HERE/native/tfs; TFSD=$HERE/native/tfsd
FIND=/usr/bin/find; GREP=/usr/bin/grep
make -s -C "$HERE" native >/dev/null || { echo "build failed"; exit 1; }
PASS=0; FAIL=0; SKIP=0; CUR=""
ok()   { PASS=$((PASS + 1)); }
bad()  { FAIL=$((FAIL + 1)); echo "  FAIL [$CUR] $*"; }
skip() { SKIP=$((SKIP + 1)); echo "  skip [$CUR] $*"; }
eq()   { if [[ "$2" == "$3" ]]; then ok; else bad "$1"; diff <(printf '%s\n' "$3") <(printf '%s\n' "$2") | head -8 | sed 's/^/      /'; fi; }
same_file() { if cmp -s "$2" "$3"; then ok; else bad "$1"; diff "$3" "$2" | head -8 | sed 's/^/      /'; fi; }
gnu_wc() { for c in gnuwc wc; do command -v $c >/dev/null && $c --version 2>/dev/null | $GREP -q "GNU coreutils" && { echo $c; return; }; done; }

# ------------------------------------------------------------------ fixtures
NAMES=(a a-b a.b a0 b é 日本 "with space" -dash .hidden "$(printf 'z%.0s' {1..200})" $'x\xff')
SIZES=(0 1 7 100 4096 70000)
make_tree() {            # ROOT SEED [FILES] [DIRS]: names around '/', unicode, undecodable bytes, links, empty dirs
    local root=$1 nf=${3:-400} nd=${4:-60} i d nm p; RANDOM=$2; local dirs=("$root")
    for ((i = 0; i < nd; i++)); do
        d=${dirs[RANDOM % ${#dirs[@]}]}; nm=${NAMES[RANDOM % ${#NAMES[@]}]}; (( i % 3 )) && nm+=$((i % 3))
        p=$d/$nm; [[ -e $p ]] || { mkdir -- "$p" 2>/dev/null && dirs+=("$p"); }
    done
    for ((i = 0; i < nf; i++)); do
        d=${dirs[RANDOM % ${#dirs[@]}]}; p=$d/${NAMES[RANDOM % ${#NAMES[@]}]}_$i
        head -c "${SIZES[RANDOM % ${#SIZES[@]}]}" /dev/urandom > "$p" 2>/dev/null || continue
        touch -d "@$(( (RANDOM << 15 | RANDOM) + 100000 )).$(printf '%09d' $(( (RANDOM << 15 | RANDOM) % 1000000000 )))" -- "$p"
    done
    for ((i = 0; i < 10; i++)); do ln -s /nonexistent/target "${dirs[RANDOM % ${#dirs[@]}]}/link$i" 2>/dev/null; done
    mkdir -p "$root/empty"
}
churn() {                # ROOT SEED: deletes, attribute changes, a directory move, new nested names, a hard link, rmtree
    local root=$1 i p; RANDOM=$2; local files dirs
    mapfile -d '' files < <($FIND "$root" -mindepth 1 ! -type d -print0 | sort -z)
    mapfile -d '' dirs < <($FIND "$root" -mindepth 1 -type d -print0 | sort -z)
    for ((i = 0; i < 15; i++)); do rm -f -- "${files[RANDOM % ${#files[@]}]}"; done
    for ((i = 0; i < 10; i++)); do p=${files[RANDOM % ${#files[@]}]}; [[ -f $p && ! -L $p ]] && chmod 640 -- "$p" && touch -d "@$((1700000000 + i))" -- "$p"; done
    p=${dirs[RANDOM % ${#dirs[@]}]}; [[ -d $p ]] && mv -- "$p" "${p}_moved"
    mkdir -p "$root/fresh/deeper"; echo -n xxxxxxxxx > "$root/fresh/deeper/f$2"$'\xfe'
    for p in "${files[@]}"; do [[ -f $p && ! -L $p ]] && { ln -- "$p" "$root/hard link $2"; echo changed > "$p"; break; }; done
    for p in "${dirs[@]}"; do [[ -d $p ]] && { rm -rf -- "$p"; break; }; done
}
oracle_dump() {          # the tree as `tfs dump` prints it, from GNU find
    $FIND "$@" -xdev -printf '%p\t%y %s %b %T@ %m %U %G %n %i %D\n' | LC_ALL=C awk -F'\t' '{
        split($2, f, " "); split(f[4], q, "."); ns = q[1] substr(q[2] "000000000", 1, 9); sub(/^0+/, "", ns); if (ns == "") ns = "0"
        printf "%s\t%s %s %s %s %s %s %s %s %s %s\n", $1, f[1], f[2], f[3], ns, f[5], f[6], f[7], f[8], f[9], f[10] }' | LC_ALL=C sort
}
check_dump() { local o; o=$(mktemp); "$TFS" -i "$2" dump > "$o.a"; oracle_dump "${@:3}" > "$o.b"; same_file "$1" "$o.a" "$o.b"; rm -f "$o" "$o.a" "$o.b"; }
fresh() { WORK=$(mktemp -d /tmp/tfs_test.XXXXXX); ROOT=$WORK/root; mkdir "$ROOT"; IDX=$WORK/t.tfsn; }
done_() { rm -rf "$WORK"; }

# ------------------------------------------------------------------ index: build, refresh, compact, watch
test_index() {
    fresh; make_tree "$ROOT" 31
    "$TFS" build -o "$IDX" "$ROOT" >/dev/null 2>&1; check_dump "build" "$IDX" "$ROOT"
    for s in 1 2; do churn "$ROOT" $s; "$TFS" -i "$IDX" refresh >/dev/null; check_dump "refresh $s" "$IDX" "$ROOT"; done
    "$TFS" -i "$IDX" compact >/dev/null; [[ -e $IDX.side ]] && bad "sidecar left after compact" || ok
    check_dump "compact" "$IDX" "$ROOT"
    done_
}
test_watch() {
    fresh; make_tree "$ROOT" 31; "$TFS" build -o "$IDX" "$ROOT" >/dev/null 2>&1
    "$TFS" -i "$IDX" watch --save-every 0.1 2>/dev/null & local pid=$!
    sleep 0.5; churn "$ROOT" 5; for i in $(seq 300); do echo b > "$ROOT/fresh/burst$i"; done
    sleep 1.5; check_dump "watch" "$IDX" "$ROOT"
    kill $pid; wait $pid 2>/dev/null; done_
}

# ------------------------------------------------------------------ queries: find, locate, stat, ls, du
FIND_EXPRS=()
find_setup() {
    fresh; make_tree "$ROOT" 31; mkdir -p "$ROOT/.hidden_dir/deep"; echo -n x > "$ROOT/.hidden_dir/deep/x_1"; mkdir -m 700 "$ROOT/emptydir"
    REF=$($FIND "$ROOT" -type f -print -quit)
    "$TFS" build -o "$IDX" "$ROOT" >/dev/null 2>&1
    echo delta > "$ROOT/made later_1"; rm -f -- "$($FIND "$ROOT" -maxdepth 1 -type f -print -quit)"   # a delta entry and a tombstone
    "$TFS" -i "$IDX" refresh >/dev/null
}
sum_sizes() { xargs -0 -r stat -c %s -- | awk '{s += $1} END {print s + 0}'; }
test_find() {
    find_setup
    local -a E=("" "-name *_1*" "-iname A*" "-type f -size +100c" "-type f -size -2k" "-type f -size 5000c" "-type d -empty" "-empty"
        "-path *a0* -type f" "! -name *_1* -type f" "( -name a* -o -name b* ) -type d" "-maxdepth 1" "-mindepth 2 -maxdepth 3 -type f"
        "-name .hidden* -prune -o -type f -print0" "-name .hidden* -prune" "-perm -644 -type f" "-perm /022" "-newer $REF"
        "-links +1 -type f" "-user $(id -un) -type f" "-nouser" "-regex .*_1[0-9]" "-iregex .*SPACE.*" "-type l" "-type f,l"
        "-not -type d -a -iname *SPACE*" "-path $ROOT/a0 -prune -o -type f -print" "-path $ROOT/a*" "-path $REF" "-path $(dirname "$REF")/*"
        "-mtime -100000" "-mmin +5")
    local e
    for e in "${E[@]}"; do
        local -a a; set -f; read -r -a a <<< "$e"; set +f
        local got want sep='\n'
        [[ " $e " == *" -print0 "* ]] && sep='\0'
        got=$("$TFS" -i "$IDX" find "$ROOT" "${a[@]}" | if [[ $sep == '\0' ]]; then cat; else tr '\n' '\0'; fi | sort -z | od -An -c)
        if [[ " $e " == *" -print"* ]]; then want=$($FIND "$ROOT" "${a[@]}" | if [[ $sep == '\0' ]]; then cat; else tr '\n' '\0'; fi | sort -z | od -An -c)
        else want=$($FIND "$ROOT" "${a[@]}" -print0 | sort -z | od -An -c); fi
        eq "find $e" "$got" "$want"
        local n b
        n=$("$TFS" -i "$IDX" find "$ROOT" "${a[@]}" --count)
        if [[ " $e " == *" -print"* ]]; then b=$($FIND "$ROOT" "${a[@]}" | if [[ $sep == '\0' ]]; then cat; else tr '\n' '\0'; fi | tee >(tr -cd '\0' | wc -c > "$WORK/n") | sum_sizes)
        else b=$($FIND "$ROOT" "${a[@]}" -print0 | tee >(tr -cd '\0' | wc -c > "$WORK/n") | sum_sizes); fi
        sleep 0.05; eq "find $e --count" "$n" "$(cat "$WORK/n" | tr -d ' ')"
        eq "find $e --bytes" "$("$TFS" -i "$IDX" find "$ROOT" "${a[@]}" --bytes)" "$b"
    done
    done_
}
locate_oracle() {        # findutils' locate rules, through GNU find: substring without wildcards, else fnmatch on the path
    local base=0 icase=0 all=0 o; local -a pats=() ex=()
    for o in "$@"; do case $o in -b) base=1;; -i) icase=1;; -A) all=1;; *) pats+=("$o");; esac; done
    local t=-path; ((base)) && t=-name; ((icase)) && t=-i${t#-}
    for o in "${pats[@]}"; do
        [[ $o == *[\*\?\[\]\\]* ]] || o="*$o*"
        ((${#ex[@]})) && { ((all)) && ex+=(-a) || ex+=(-o); }
        ex+=("$t" "$o")
    done
    $FIND "$ROOT" \( "${ex[@]}" \) -print0 | sort -z | od -An -c
}
test_locate() {
    find_setup
    local -a C=("_1" "/a0/" "a0/sp" "*.hl1" "*_1?" "-i SPACE" "日本" "-b *本*" "-b x" "-A _1 a-b" "_1 a-b" "-b [ab]*" ".hidden_dir/deep")
    local c
    for c in "${C[@]}"; do
        local -a a; set -f; read -r -a a <<< "$c"; set +f
        eq "locate $c" "$("$TFS" -i "$IDX" locate "${a[@]}" | tr '\n' '\0' | sort -z | od -An -c)" "$(locate_oracle "${a[@]}")"
        eq "locate -c $c" "$("$TFS" -i "$IDX" locate -c "${a[@]}")" "$(locate_oracle "${a[@]}" | tr -d ' \n' | $GREP -o '\\0' | wc -l)"
    done
    done_
}
commas() { LC_ALL=C awk '{ s = $1; o = ""; while (length(s) > 3) { o = "," substr(s, length(s) - 2) o; s = substr(s, 1, length(s) - 3) } print s o }'; }
test_stat_ls_du() {
    find_setup
    local p d
    while IFS= read -r -d '' p; do
        local t; t=$($FIND "$p" -maxdepth 0 -printf %y)
        local want; want=$(printf '%s %04o %s:%s %14s %s %s' "$t" "0$(stat -c %a -- "$p")" "$(stat -c %u -- "$p")" "$(stat -c %g -- "$p")" \
            "$(stat -c %s -- "$p" | commas)" "$(date -d "@$(stat -c %Y -- "$p")" '+%F %T')" "$p")
        eq "stat $p" "$("$TFS" -i "$IDX" stat "$p")" "$want"
    done < <($FIND "$ROOT" -print0 | head -z -n 60)
    while IFS= read -r -d '' d; do
        eq "ls $d" "$("$TFS" -i "$IDX" ls "$d" | od -An -c)" "$($FIND "$d" -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort | od -An -c)"
        local c b; read -r c b < <($FIND "$d" -mindepth 1 -xdev -printf '%s\n' | awk '{n++; s += $1} END {print n + 0, s + 0}')
        eq "du $d" "$("$TFS" -i "$IDX" du "$d")" "$(echo "$b" | commas) bytes in $(echo "$c" | commas) entries below $d"
    done < <($FIND "$ROOT" -type d -print0 | head -z -n 25)
    done_
}

# ------------------------------------------------------------------ content: wc, hash, grep, refresh
content_check() {        # LABEL ROOT [store options]: every answer against GNU wc, git and GNU grep
    local label=$1 r=$2; shift 2; local wc; wc=$(gnu_wc)
    local files; files=$($FIND "$r" -type f -print0 | LC_ALL=C sort -z | od -An -c)
    eq "$label: file set" "$("$TFS" -i "$IDX" content "$@" --no-refresh hash "$r" | cut -c43- | tr '\n' '\0' | od -An -c)" "$files"
    if [[ -n $wc ]]; then
        same_file "$label: wc -lwcL" <("$TFS" -i "$IDX" content --no-refresh wc "$r" | head -n -1 | cut -d' ' -f1-4) \
            <($FIND "$r" -type f -print0 | LC_ALL=C sort -z | LC_ALL=C $wc -lwcL --files0-from=- | head -n -1 | awk '{print $1, $2, $3, $4}')
    else skip "GNU wc not installed"; fi
    if command -v git >/dev/null; then
        same_file "$label: git blob ids" <("$TFS" -i "$IDX" content --no-refresh hash "$r" | cut -c1-40) \
            <($FIND "$r" -type f -print0 | LC_ALL=C sort -z | xargs -0 -r git hash-object --no-filters --)
    else skip "git not installed"; fi
    local pat
    for pat in alpha $'gamma \xa0d' zz _1 nope-not-here "more words" ab; do
        eq "$label: grep '$pat'" "$("$TFS" -i "$IDX" content --no-refresh grep "$pat" "$r" | LC_ALL=C sort)" "$($GREP -rlF -- "$pat" "$r" | LC_ALL=C sort)"
    done
}
test_content() {
    fresh; local other=$WORK/other; mkdir "$other"; make_tree "$ROOT" 12; make_tree "$other" 13 50 5
    for i in $(seq 0 19); do for ((j = 0; j < i; j++)); do printf 'alpha beta\n\tgamma \xa0delta\r'; done > "$ROOT/t$i.txt"; done
    "$TFS" build -o "$IDX" "$ROOT" "$other" >/dev/null 2>&1
    local st; st=$("$TFS" -i "$IDX" content refresh "$ROOT"); "$TFS" -i "$IDX" content refresh "$other" >/dev/null
    content_check "first" "$ROOT"; content_check "other root" "$other"
    eq "binary files unindexed by default" "$("$TFS" -i "$IDX" content info | $GREP -c '"binary_unindexed": 0')" "0"
    echo "more words here" >> "$ROOT/t3.txt"; rm "$ROOT/t4.txt"; mv "$ROOT/t5.txt" "$ROOT/t5-renamed.txt"; echo "zz top" > "$ROOT/new.txt"
    "$TFS" -i "$IDX" refresh >/dev/null; st=$("$TFS" -i "$IDX" content refresh "$ROOT")
    eq "refresh reads only changed files" "$(echo "$st" | $GREP -o '"read": [0-9]*, "bytes_read": [0-9]*, "dropped": [0-9]*' | sed 's/"bytes_read": [0-9]*, //')" '"read": 3, "dropped": 3'
    content_check "after changes" "$ROOT"; content_check "other root kept" "$other"
    eq "unchanged refresh reads nothing" "$("$TFS" -i "$IDX" content refresh "$ROOT" | $GREP -o '"read": [0-9]*')" '"read": 0'
    "$TFS" -i "$IDX" content compact >/dev/null; content_check "after compact" "$ROOT"
    eq "wc total" "$("$TFS" -i "$IDX" content wc -s "$ROOT/t3.txt")" "$(LC_ALL=C $(gnu_wc || echo wc) -lwcL < "$ROOT/t3.txt" | awk '{print $1, $2, $3, $4}') total"
    rm -f "$IDX.content"; "$TFS" -i "$IDX" content --index-binary refresh "$ROOT" >/dev/null
    eq "--index-binary" "$("$TFS" -i "$IDX" content info | $GREP -o '"binary_unindexed": [0-9]*')" '"binary_unindexed": 0'
    content_check "binary indexed" "$ROOT"
    done_
}

# ------------------------------------------------------------------ dupes: against a brute-force grouping by content
dupes_oracle() {         # [-H] [-n] [-A] [-r] DIR: sets of identical files, one name per inode unless -H; normalized
    local H=0 n=0 A=0 r=0 o; for o in "$@"; do case $o in -H) H=1;; -n) n=1;; -A) A=1;; -r) r=1;; *) d=$o;; esac; done
    local -a ex=(); ((r)) || ex+=(-maxdepth 1); ((n)) && ex+=(! -empty); ((A)) && ex+=(! -path "$d/.*" ! -path "$d/*/.*")
    $FIND "$d" -mindepth 1 "${ex[@]}" -type f -printf '%D:%i\t%p\0' | LC_ALL=C sort -z -t$'\t' -k2 |
        if ((H)); then cat; else LC_ALL=C awk -v RS='\0' -v ORS='\0' -F'\t' '!seen[$1]++'; fi |
        LC_ALL=C awk -v RS='\0' -F'\t' '{ p = substr($0, index($0, "\t") + 1); printf "%s\0", p }' |
        while IFS= read -r -d '' p; do printf '%s %s\0' "$(sha256sum < "$p" | cut -c1-64)" "$p"; done |
        LC_ALL=C awk -v RS='\0' '{ h = substr($0, 1, 64); p = substr($0, 66); g[h] = g[h] "\n" p; c[h]++ }
            END { for (h in g) if (c[h] >= 2) print g[h] "\x01" }' | normalize_sets
}
normalize_sets() { LC_ALL=C awk -v RS='\x01' 'NF { n = split($0, a, "\n"); m = 0; for (i = 1; i <= n; i++) if (a[i] != "") b[++m] = a[i]
        for (i = 1; i <= m; i++) for (j = i + 1; j <= m; j++) if (b[j] < b[i]) { t = b[i]; b[i] = b[j]; b[j] = t }
        s = b[1]; for (i = 2; i <= m; i++) s = s "|" b[i]; print s; delete b }' | LC_ALL=C sort; }
tfs_sets() { "$TFS" -i "$IDX" dupes "$@" | LC_ALL=C awk 'BEGIN { RS = ""; ORS = "\x01" } { print }' | normalize_sets; }
test_dupes() {
    fresh; local r=$ROOT i
    mkdir -p "$r/a/b" "$r/.hid" "$r/c"
    for i in 1 2 3; do echo same > "$r/a/s$i"; done; cp "$r/a/s1" "$r/a/b/s4"; echo same > "$r/.hid/s5"; echo same > "$r/c/.s6"
    head -c 9000 /dev/urandom > "$r/a/big"; cp "$r/a/big" "$r/c/big2"; cp "$r/a/big" "$r/a/b/big3"
    { head -c 4096 "$r/a/big"; head -c 4904 /dev/urandom; } > "$r/c/same-partial"          # same size and first 4096 bytes
    head -c 9000 /dev/urandom > "$r/c/same-size"
    : > "$r/e1"; : > "$r/e2"; ln "$r/a/s1" "$r/a/hardlink"; echo unique > "$r/u"; echo "with space" > "$r/a/x y"; echo "with space" > "$r/c/x y"
    make_tree "$r/c" 7 40 4; cp -r "$r/c" "$r/copy of c"
    "$TFS" build -o "$IDX" "$r" >/dev/null 2>&1
    local o
    for o in "-r" "-r -H" "-r -n" "-r -A" "-r -n -A -H" ""; do
        local -a a; read -r -a a <<< "$o"
        eq "dupes $o" "$(tfs_sets "${a[@]}" "$r")" "$(dupes_oracle "${a[@]}" "$r")"
    done
    local first; first=$("$TFS" -i "$IDX" dupes -r --store "$WORK/cache" "$r")
    eq "dupes cached rerun" "$("$TFS" -i "$IDX" dupes -r --store "$WORK/cache" "$r")" "$first"
    eq "dupes -Q (cached digests)" "$("$TFS" -i "$IDX" dupes -r -Q --store "$WORK/cache" "$r")" "$first"
    local sets files bytes; sets=$(tfs_sets -r "$r" | wc -l)
    files=$(tfs_sets -r "$r" | awk -F'|' '{s += NF - 1} END {print s}')
    eq "dupes -1 escapes spaces" "$("$TFS" -i "$IDX" dupes -r -1 "$r" | $GREP -c 'x\\ y')" "1"
    eq "dupes -S" "$("$TFS" -i "$IDX" dupes -r -S "$r" | $GREP -c 'bytes\? each:$')" "$sets"
    eq "dupes -m" "$("$TFS" -i "$IDX" dupes -r -m "$r" | cut -d' ' -f1-5)" "$files duplicate files (in $sets"
    eq "dupes -f omits one per set" "$("$TFS" -i "$IDX" dupes -r -f "$r" | $GREP -c .)" "$files"
    done_
}

# ------------------------------------------------------------------ the mount (tfsd)
fuse_ok() { [[ -e /dev/fuse ]] && command -v fusermount3 >/dev/null; }
mount_start() {          # tfsd OPTIONS...: mounts $ROOT's index at $MNT
    MNT=$WORK/mnt; mkdir -p "$MNT"; "$TFS" build -o "$IDX" "$ROOT" >/dev/null 2>&1
    "$TFSD" -i "$IDX" --save-every 0.2 "$@" "$MNT" 2>"$WORK/tfsd.err" & TFSD_PID=$!
    for i in $(seq 100); do mountpoint -q "$MNT" && return 0; sleep 0.05; done
    bad "mount failed: $(cat "$WORK/tfsd.err")"; return 1
}
mount_stop() { fusermount3 -u "$MNT" 2>/dev/null || fusermount3 -u -z "$MNT"; wait $TFSD_PID 2>/dev/null; }
view_of() { $FIND "$1" -mindepth 1 \( -path "$1/.q" -prune \) -o -printf '%P\t%y %s %i %T@ %n %m %l\n' | LC_ALL=C sort; }
data_of() { (cd "$1" && $FIND . \( -path ./.q -prune \) -o -type f -print0 | LC_ALL=C sort -z | xargs -0 -r sha256sum --); }
same_view() { same_file "$1: metadata" <(view_of "$MNT") <(view_of "$ROOT"); same_file "$1: data" <(data_of "$MNT") <(data_of "$ROOT"); }
test_mount() {
    fuse_ok || { skip "FUSE not available"; return; }
    fresh; make_tree "$ROOT" 4; mount_start --watch || { done_; return; }
    same_view "mounted"
    local m=$MNT
    head -c 300000 /dev/urandom > "$m/new file.bin"; printf patch | dd of="$m/new file.bin" bs=1 seek=1000 conv=notrunc 2>/dev/null; truncate -s 200000 "$m/new file.bin"
    mkdir -p "$m/d1/d2"; echo x > "$m/d1/d2/x"; mv "$m/d1" "$m/d1 renamed"
    ln -s "new file.bin" "$m/sl"; ln "$m/new file.bin" "$m/hl"; chmod 600 "$m/hl"; touch -h -d @123456789 "$m/sl"
    $FIND "$m" \( -path "$m/.q" -prune \) -o -type f -print0 | sort -z | head -z -n 10 | xargs -0 rm -f --; rmdir "$m/empty"
    sleep 1.2; same_view "writes through the mount"
    echo outside > "$ROOT/made outside"; rm "$ROOT/hl"
    sleep 1.5; same_view "external changes"
    mount_stop; check_dump "saved index after unmount" "$IDX" "$ROOT"
    done_
}
uri_preds() {            # a URI condition as GNU find predicates
    case $1 in @type=f) echo "-type f";; @type=fl) echo "( -type f -o -type l )";; @name=*_1*) echo "-name *_1*";;
        @name=a*) echo "-name a*";; @size\>99) echo "-size +99c";; @max_size=5000) echo "-size -5000c";; esac
}
uri_trials() {           # 25 random URIs: @count, @bytes, @list and a walk of the view against GNU find
    local links=$1 trial; local -a dirs terms=("@type=f" "@type=fl" "@name=*_1*" "@name=a*" "@size>99" "@max_size=5000")
    mapfile -d '' dirs < <($FIND "$ROOT" -type d -print0 | LC_ALL=C sort -z); RANDOM=13
    for ((trial = 0; trial < 25; trial++)); do
        local sub=${dirs[RANDOM % ${#dirs[@]}]} k=$((1 + RANDOM % 3)) i; local -a conj=() pr=() segs=() rel=()
        local -a pool=("${terms[@]}"); for ((i = 0; i < k; i++)); do local j=$((RANDOM % ${#pool[@]})); conj+=("${pool[j]}"); pool=("${pool[@]:0:j}" "${pool[@]:j+1}"); done
        for i in "${conj[@]}"; do set -f; pr+=($(uri_preds "$i")); set +f; done
        [[ $sub != "$ROOT" ]] && IFS=/ read -r -a rel <<< "${sub#"$ROOT"/}"
        local at=$((RANDOM % (${#rel[@]} + 1))); segs=("${rel[@]:0:at}" "${conj[@]}" "${rel[@]:at}")
        local base=$MNT; for i in "${segs[@]}"; do base+=/$i; done
        local want; want=$($FIND "$sub" "${pr[@]}" -print0 | LC_ALL=C sort -z | od -An -c)
        eq "uri ${segs[*]} @count" "$(cat "$base/@count")" "$($FIND "$sub" "${pr[@]}" -printf x | wc -c)"
        eq "uri ${segs[*]} @bytes" "$(cat "$base/@bytes")" "$($FIND "$sub" "${pr[@]}" -printf '%s\n' | awk '{s += $1} END {print s + 0}')"
        eq "uri ${segs[*]} @list" "$(sed "s|^$MNT|$ROOT|" "$base/@list" | tr '\n' '\0' | LC_ALL=C sort -z | od -An -c)" "$want"
        local seen; seen=$($FIND "$base" -mindepth 1 -printf '%P\t%y\n' | while IFS=$'\t' read -r p t; do
            if [[ $t == d ]]; then [[ -n $($FIND "$sub/$p" -maxdepth 0 "${pr[@]}" -print) ]] && printf '%s\0' "$sub/$p"
            else printf '%s\0' "$sub/$p"; ((links)) && [[ $(readlink "$base/$p") != "$sub/$p" ]] && echo "BADLINK $p"; fi; done | LC_ALL=C sort -z | od -An -c)
        eq "uri ${segs[*]} walk" "$seen" "$($FIND "$sub" -mindepth 1 "${pr[@]}" -print0 | LC_ALL=C sort -z | od -An -c)"
    done
}
test_uris() {
    fuse_ok || { skip "FUSE not available"; return; }
    local links
    for links in 0 1; do
        fresh; make_tree "$ROOT" 6; echo -n "a real file named @count" > "$ROOT/@count"
        if ((links)); then mount_start --watch --links || { done_; continue; }; else mount_start --watch || { done_; continue; }; fi
        CUR="uris links=$links"; uri_trials $links
        eq "@@ escape" "$(cat "$MNT/@@count")" "a real file named @count"
        echo late > "$ROOT/late_1.py"; sleep 1.5
        eq "live via the watcher" "$($GREP -cx "$MNT/late_1.py" "$MNT/@name=*_1*/@list")" "1"
        if ((links)); then mkdir "$MNT/newdir" 2>/dev/null && bad "sidecar accepted a write" || ok
            eq "sidecar .q empty" "$(ls -A "$MNT/.q")" ""; fi
        mount_stop; done_
    done
}
test_query_views() {
    fuse_ok || { skip "FUSE not available"; return; }
    fresh; make_tree "$ROOT" 4; mount_start --watch || { done_; return; }
    local q=$MNT/.q sub; sub=$($FIND "$ROOT" -mindepth 1 -type d ! -empty -print | LC_ALL=C sort | head -1)
    local -A EX=([py]="type=f name='*_1*'" [big]="type=f min_size=1000" [links]="type=l" [dirs]="type=d name='a*'" [all]="" [sub]="path='${sub#"$ROOT"/}' type=f")
    local -A PR=([py]="-type f -name *_1*" [big]="-type f -size +1000c" [links]="-type l" [dirs]="-type d -name a*" [all]="" [sub]="-type f")
    local n
    for n in "${!EX[@]}"; do mkdir "$q/$n"; printf '%s' "${EX[$n]}" > "$q/$n/.expression"; done
    check_views() {
        for n in "${!EX[@]}"; do
            local start=$ROOT; [[ $n == sub ]] && start=$sub; local -a pr; set -f; pr=(${PR[$n]}); set +f
            [[ -d $q/$n ]] || continue
            eq ".q/$n .count ($1)" "$(cat "$q/$n/.count")" "$($FIND "$start" "${pr[@]}" -printf x | wc -c)"
            eq ".q/$n .bytes ($1)" "$(cat "$q/$n/.bytes")" "$($FIND "$start" "${pr[@]}" -printf '%s\n' | awk '{s += $1} END {print s + 0}')"
        done
    }
    check_views "created"
    head -c 2000 /dev/zero > "$MNT/fresh_1.bin"; head -c 1500 /dev/zero > "$ROOT/outside_1.txt"
    rm -f -- "$($FIND "$ROOT" -type f -name '*_1*' -print -quit)"; sleep 1.5; check_views "after changes"
    mv "$q/py" "$q/python"; EX[python]=${EX[py]}; PR[python]=${PR[py]}; unset 'EX[py]' 'PR[py]'
    rmdir "$q/links"; unset 'EX[links]' 'PR[links]'
    check_views "renamed"
    eq ".q listing" "$(ls "$q" | LC_ALL=C sort)" "$(printf '%s\n' "${!EX[@]}" | LC_ALL=C sort)"
    { echo x > "$q/python/nope"; } 2>/dev/null && bad "write into a view accepted" || ok
    mount_stop
    eq "queries saved" "$(cut -f1 "$IDX.queries" | LC_ALL=C sort)" "$(printf '%s\n' "${!EX[@]}" | LC_ALL=C sort)"
    done_
}

# ------------------------------------------------------------------ btrfs catch-up (only on a prepared disk)
test_btrfs() {
    local data=${TFS_BTRFS_DATA:-/mnt/tfs/data} snaps=${TFS_BTRFS_SNAPS:-/mnt/tfs/.snaps}
    if ! { command -v btrfs >/dev/null && [[ -w $data && -d $snaps ]] && { sudo -n /usr/local/sbin/tfs-btrfs-send >/dev/null 2>&1; [[ $? == 2 ]]; }; }; then
        skip "no prepared btrfs disk (TFS_BTRFS_DATA, TFS_BTRFS_SNAPS, tfs-btrfs-send in sudoers)"; return; fi
    fresh; local w; w=$(mktemp -d "$data/tfs_test_XXXXXX"); make_tree "$w" 41
    "$TFS" -i "$IDX" btrfs init "$data" "$snaps" >/dev/null
    for s in 1 2; do churn "$w" $s
        eq "catch-up $s mode" "$("$TFS" -i "$IDX" btrfs catchup | $GREP -o '"mode": "[a-z]*"')" '"mode": "apply"'
        check_dump "catch-up $s" "$IDX" "$data"; done
    rm -rf "$w"; local cur; cur=$(sed -n 's/.*"current": "\([^"]*\)".*/\1/p' "$IDX.btrfs.json")
    [[ -n $cur ]] && { btrfs property set -f -ts "$snaps/$cur" ro false 2>/dev/null; btrfs subvolume delete "$snaps/$cur" >/dev/null 2>&1; }
    done_
}

ALL=(index watch find locate stat_ls_du content dupes mount uris query_views btrfs)
for t in "${@:-${ALL[@]}}"; do
    CUR=$t; before=$FAIL; start=$(date +%s.%N); "test_$t"
    printf '%-12s %s (%.1f s)\n' "$t" "$([[ $FAIL == "$before" ]] && echo ok || echo FAILED)" "$(echo "$(date +%s.%N) - $start" | bc)"
done
echo "passed $PASS checks, failed $FAIL, skipped $SKIP"
[[ $FAIL == 0 ]]
