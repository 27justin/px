compiler := "pxc"

default: test-all

test-all:
    @find tests -name "*.md" | xargs -n 1 -P 10 just test

build:
    cmake --build build

test file:
    #!/usr/bin/env bash
    set -uo pipefail

    DIR=$(mktemp -d)
    trap 'rm -r "$DIR"' EXIT

    # 1. Extract blocks
    awk -v outdir="$DIR" '
        /^# / { last_h = substr($0, 3) }
        /^## / { last_h = substr($0, 4) }
        /^### / { last_h = substr($0, 5) }
        /^```px/ {
            i++;
            f = "body_" i ".px";
            print (last_h ? last_h : "Test " i) > (outdir "/name_" i ".txt")
            next
        }
        /^```text/ { j++; f = "expected_" j ".txt"; next }
        /^```/ { f = "" }
        f { print > (outdir "/" f) }
    ' "{{file}}"

    TOTAL_TESTS=$(ls "$DIR"/body_*.px 2>/dev/null | wc -l)
    FAILED=0

    echo "==> File: {{file}} ($TOTAL_TESTS cases)"

    for i in $(seq 1 $TOTAL_TESTS); do
        CASE_NAME=$(cat "$DIR/name_$i.txt")
        CASE_DIR="$DIR/case_$i"
        mkdir -p "$CASE_DIR"

        # 2. Robust Split Logic
        # We use sed with explicit patterns to avoid the "no previous regex" error
        if grep -q "// ---" "$DIR/body_$i.px"; then
            sed -n '1,/\/\/ ---/p' "$DIR/body_$i.px" | grep -v "// ---" > "$CASE_DIR/global.px"
            sed -n '/\/\/ ---/,$p' "$DIR/body_$i.px" | grep -v "// ---" > "$CASE_DIR/local.px"
        else
            touch "$CASE_DIR/global.px"
            cat "$DIR/body_$i.px" > "$CASE_DIR/local.px"
        fi

        # 3. Assemble
        {
            echo "import test_util"
            cat "$CASE_DIR/global.px"
            echo "main := fn () -> i32 {"
            sed 's/^/  /' "$CASE_DIR/local.px"
            echo "  0"
            echo "}"
        } > "$CASE_DIR/main.px"

        # 4. Compile
        # Capture the exit code so we can diagnose Segfaults (139) vs Normal Errors
        set +e
        timeout 5s ./build/{{compiler}} "$CASE_DIR/main.px" "tests/test_util.px" -I tests/ -o "$CASE_DIR/bin" 2> "$CASE_DIR/err.txt"
        EXIT_CODE=$?
        set -e

        if [ $EXIT_CODE -ne 0 ]; then
            if [ $EXIT_CODE -eq 139 ]; then
                echo "  [$CASE_NAME] FAILED: COMPILER SEGFAULTED"
            else
                echo "  [$CASE_NAME] FAILED: Compilation Error"
            fi

            # Show the generated code and the error to help debug the crash
            echo "    --- Generated Code ---"
            sed 's/^/    /' "$CASE_DIR/main.px"
            echo "    --- Compiler Stderr ---"
            sed 's/^/    /' "$CASE_DIR/err.txt"
            FAILED=$((FAILED + 1))
            continue
        fi

        # 5. Run
        if ! timeout 5s "$CASE_DIR/bin" > "$CASE_DIR/actual.txt" 2>> "$CASE_DIR/err.txt"; then
            echo "  [$CASE_NAME] FAILED: Runtime Error/Timeout"
            FAILED=$((FAILED + 1))
            continue
        fi

        # 6. Diff
        if ! diff -u -Z "$DIR/expected_$i.txt" "$CASE_DIR/actual.txt" > "$CASE_DIR/diff.txt"; then
            echo "  [$CASE_NAME] FAILED: Output mismatch"
            sed 's/^/    /' "$CASE_DIR/diff.txt"
            FAILED=$((FAILED + 1))
            continue
        fi

        echo "  [$CASE_NAME] PASSED"
    done

    if [ "$FAILED" -ne 0 ]; then
        echo "RESULT: {{file}} failed $FAILED/$TOTAL_TESTS cases."
        exit 1
    fi
