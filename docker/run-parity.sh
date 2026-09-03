#!/usr/bin/env bash
set -euo pipefail

TARGET_ARCH="${TARGET_ARCH:-x64}"
SERVER_DIR="/workspace/server"
BUILD_DIR="${SERVER_DIR}/build-docker-${TARGET_ARCH}"

TEST_SUITE_LOG="$(mktemp)"
PARITY_LOG="$(mktemp)"
trap 'rm -f "${TEST_SUITE_LOG:-}" "${PARITY_LOG:-}"' EXIT

# Block 1: Architecture and toolchain version identification
# Record toolchain versions and architecture upfront to guarantee deterministic environments
# and simplify triage when debugging architecture-specific ABI discrepancies or compiler differences.
echo "=== Parity Check | Target Architecture: ${TARGET_ARCH} ==="
g++ --version | head -n 1
cmake --version | head -n 1

# Block 2: Static sanity and architectural layer verification
# Execute static metadata checks first because fast syntax and layer boundary validation
# catches invalid diagnostic registrations and architectural violations before initiating a heavy C++ build.
cd "${SERVER_DIR}"
python3 scripts/check-diagnostic-codes.py
python3 scripts/check-layer-includes.py

# Block 3: Architecture-isolated CMake configuration
# When mounted from a Windows host, server/build often contains MSVC-generated artifacts and cache entries.
# Overwriting those directories from a Linux container corrupts the host build environment. Isolating the
# build directory by TARGET_ARCH guarantees clean artifact segregation between host, container, and target architectures.
CMAKE_ARGS=(
    -B "${BUILD_DIR}"
    -S "${SERVER_DIR}"
    -DCMAKE_BUILD_TYPE=Release
    -DANGELLSP_BUILD_ORACLE=ON
)

if [ "${TARGET_ARCH}" = "x86" ]; then
    CMAKE_ARGS+=(
        -DCMAKE_CXX_FLAGS="-m32"
        -DCMAKE_C_FLAGS="-m32"
    )
fi

cmake "${CMAKE_ARGS[@]}"

# Block 4: Selective compilation of audit targets
# Compiling only angelscript_oracle and angel_lsp_tests avoids building unnecessary binaries,
# keeping build times short while producing the exact executables required for the parity audit.
cmake --build "${BUILD_DIR}" --config Release -j "$(nproc)" --target angelscript_oracle angel_lsp_tests

# Block 5: Full test suite execution
# All baseline unit and integration tests must succeed before parity evaluation to isolate foundational
# regressions from oracle harness and AngelScript runtime behavior.
"${BUILD_DIR}/angel_lsp_tests" 2>&1 | tee "${TEST_SUITE_LOG}"

# Checked from the output rather than trusted to the exit code. The first run of this container
# reported success on a suite that printed "Status: FAILURE!" and had two failing cases - a green
# gate over a red suite, which is worse than no gate. Grepping the summary cannot be fooled by
# whatever the runner decides to return.
if grep -Fq "Status: FAILURE!" "${TEST_SUITE_LOG}"; then
    echo "ERROR: the test suite reported failures. See the output above." >&2
    exit 1
fi

# Block 6: Parity audit with strict corpus verification
# The test runner silently succeeds with exit code 0 when parity fixtures or harness paths are missing;
# explicitly parsing the summary line and verifying a non-zero script count prevents false-positive CI passes.
export ASHARNESS_EXE="${BUILD_DIR}/angelscript_oracle"
export PARITY_PREDEFINED="${SERVER_DIR}/tests/fixtures/sdk-addons.as.predefined"
export PARITY_SCRIPT_DIR="${SERVER_DIR}/tests/parity"

"${BUILD_DIR}/angel_lsp_tests" --no-skip --test-case="*Parity*" 2>&1 | tee "${PARITY_LOG}"

if ! grep -Fq "AngelScript parity audit (" "${PARITY_LOG}"; then
    echo "ERROR: Parity audit summary line 'AngelScript parity audit (' was not found in test output." >&2
    echo "The parity audit may not have executed or crashed unexpectedly." >&2
    exit 1
fi

if grep -Fq "(0 scripts)" "${PARITY_LOG}"; then
    echo "ERROR: Parity audit executed against 0 scripts. Test corpus directory is missing or empty." >&2
    echo "Silent skips with exit code 0 are treated as hard failures." >&2
    exit 1
fi

# Block 7: Consolidated one-line summary
# A concise single-line summary gives developers and automated telemetry immediate, unambiguous
# confirmation of total test passes and audited corpus volume.
TEST_COUNT=$(grep -E '(test cases:|tests passed|All tests passed)' "${TEST_SUITE_LOG}" | tail -n 1 | sed 's/^[ \t]*//' || true)
if [ -z "${TEST_COUNT}" ]; then
    TEST_COUNT="All unit tests passed"
fi

PARITY_LINE=$(grep -F "AngelScript parity audit (" "${PARITY_LOG}" | head -n 1 | sed 's/^[ \t]*//' || true)
SCRIPT_COUNT=$(echo "${PARITY_LINE}" | grep -oE '[0-9]+ scripts' | head -n 1 || true)
if [ -z "${SCRIPT_COUNT}" ]; then
    SCRIPT_COUNT="${PARITY_LINE}"
fi

echo "=== Summary: ${TEST_COUNT} | Audited: ${SCRIPT_COUNT} (${TARGET_ARCH}) ==="
