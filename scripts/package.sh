#!/usr/bin/env bash
# package.sh - build the submission archive. It holds the source, the
# executable, the documents, the workloads and the tests. It holds none of
# the intermediate build files.
set -u

cd "$(dirname "$0")/.." || exit 1
PROJECT=$(basename "$PWD")
STAMP=$(date +%Y%m%d)
OUT="../tsim-submission-$STAMP.zip"

echo
echo "building a clean submission archive"
echo

# --- build from the start, and stop if the build or the tests fail -------
echo "  rebuilding from scratch..."
if ! make clean >/dev/null 2>&1 || ! make >/dev/null 2>&1; then
    echo "  BUILD FAILED - fix it before packaging"
    make
    exit 1
fi
echo "  build ok"

echo "  running the tests..."
if ! ./tests/run_tests.sh >/tmp/tsim-pkg-test.log 2>&1; then
    echo "  TESTS FAILED - the script does not archive a project that fails"
    tail -20 /tmp/tsim-pkg-test.log
    exit 1
fi
echo "  $(grep -c PASS /tmp/tsim-pkg-test.log) regression tests passed"

# --- check that each required file is present ----------------------------
missing=0
for f in README.md Makefile tsim \
         docs/HOWTO.md docs/DESIGN.md docs/INSTALL_WSL.md \
         docs/VIDEO_SCRIPT.md docs/SUBMISSION.md; do
    [ -e "$f" ] || { echo "  MISSING: $f"; missing=1; }
done
[ "$missing" = 0 ] || exit 1

if [ ! -e VIDEO.txt ] && ! ls ./*.mp4 >/dev/null 2>&1; then
    echo
    echo "  NOTE: no video file and no VIDEO.txt in the project."
    echo "        Add the video, or put the OneDrive link in VIDEO.txt,"
    echo "        before you upload this archive."
fi

# --- write the archive ---------------------------------------------------
# Binusmaya accepts a zip file, so use zip when it is installed. Otherwise
# write a tar.gz file, which needs no extra package.
if command -v zip >/dev/null 2>&1; then
    rm -f "$OUT"
    ( cd .. && zip -r -q "$(basename "$OUT")" "$PROJECT" \
        -x "$PROJECT/build/*" \
        -x "$PROJECT/.git/*" \
        -x "$PROJECT/*.csv" \
        -x "$PROJECT/*.o" \
        -x "$PROJECT/.*" ) || { echo "  zip failed"; exit 1; }
    LIST="unzip -l"
else
    echo
    echo "  zip is not installed, writing a .tar.gz instead."
    echo "  For a .zip:  sudo apt install zip   and run this again."
    OUT="${OUT%.zip}.tar.gz"
    rm -f "$OUT"
    ( cd .. && tar -czf "$(basename "$OUT")" \
        --exclude="$PROJECT/build" \
        --exclude="$PROJECT/.git" \
        --exclude="$PROJECT/*.csv" \
        --exclude="$PROJECT/.*" \
        "$PROJECT" ) || { echo "  tar failed"; exit 1; }
    LIST="tar -tzf"
fi

echo
echo "  wrote $(cd .. && pwd)/$(basename "$OUT")"
echo "  size  $(cd .. && du -h "$(basename "$OUT")" | cut -f1)"
echo
echo "  it contains $(cd .. && $LIST "$(basename "$OUT")" | grep -cv '/$') file(s):"
(cd .. && $LIST "$(basename "$OUT")" 2>/dev/null \
    | grep -oE "$PROJECT/[^ ]*[^/ ]$" | sort | head -45 | sed 's/^/    /')
echo
echo "  Check docs/SUBMISSION.md for the final checklist."
echo
