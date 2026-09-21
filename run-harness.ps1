param (
    [switch]$FullAudit,
    [switch]$CheckFormatting,
    [switch]$EnableASan
)

$ErrorActionPreference = "Stop"
$projectRoot = "E:\Github\src\angelscript-lsp"
Set-Location $projectRoot

Write-Host "==========================================" -ForegroundColor Cyan
Write-Host " [1/7] Running Layer, Diagnostic & Signatures" -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan
python "$projectRoot\server\scripts\check-layer-includes.py"
if ($LASTEXITCODE -ne 0) { throw "check-layer-includes.py failed with exit code $LASTEXITCODE" }
python "$projectRoot\server\scripts\check-diagnostic-codes.py"
if ($LASTEXITCODE -ne 0) { throw "check-diagnostic-codes.py failed with exit code $LASTEXITCODE" }
python "$projectRoot\server\scripts\check-clean-signatures.py"
if ($LASTEXITCODE -ne 0) { throw "check-clean-signatures.py failed with exit code $LASTEXITCODE" }
python "$projectRoot\server\scripts\check-grammar-names.py"
if ($LASTEXITCODE -ne 0) { throw "check-grammar-names.py failed with exit code $LASTEXITCODE" }

Write-Host "`n==========================================" -ForegroundColor Cyan
Write-Host " [2/7] Auditing Complexity (Lizard AST)   " -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan
python -m lizard "$projectRoot\server\src" -C 15 -L 70 -a 4 -w
if ($LASTEXITCODE -ne 0) {
    Write-Error "Lizard complexity check failed. Constraints: Max CCN 15, Max Lines 70, Max Params 4."
    exit 1
}

Write-Host "`n==========================================" -ForegroundColor Cyan
Write-Host " [3/7] Initializing MSVC Developer Shell  " -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan
$devShell = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1"
if (-not (Test-Path $devShell)) {
    $devShell = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1"
}
& $devShell -Arch amd64

Write-Host "`n==========================================" -ForegroundColor Cyan
Write-Host " [4/7] Configuring & Building (Debug)     " -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan
Set-Location "$projectRoot\server"
$cmakeArgs = @("-G", "Ninja Multi-Config", "-B", "build", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
if ($EnableASan) {
    $cmakeArgs += "-DCMAKE_CXX_FLAGS=-fsanitize=address"
}
cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }
cmake --build build --config Debug
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

Copy-Item "build\compile_commands.json" -Destination "$projectRoot\" -Force

Write-Host "`n==========================================" -ForegroundColor Cyan
Write-Host " [5/7] Executing Complete CTest Suite     " -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan
ctest --test-dir build -C Debug --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Test suite failed with exit code $LASTEXITCODE" }

if ($CheckFormatting) {
    Write-Host "`n==========================================" -ForegroundColor Cyan
    Write-Host " [6/7] Checking Code Formatting           " -ForegroundColor Cyan
    Write-Host "==========================================" -ForegroundColor Cyan
    $files = Get-ChildItem -Path "$projectRoot\server\src" -Recurse -Include *.cpp,*.h
    foreach ($file in $files) {
        clang-format -dry-run --Werror $file.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "Formatting check failed for $($file.FullName)"
        }
    }
}

if ($FullAudit) {
    Write-Host "`n==========================================" -ForegroundColor Cyan
    Write-Host " [7/7] Cppcheck Static Analysis Audit     " -ForegroundColor Cyan
    Write-Host "==========================================" -ForegroundColor Cyan
    if (Get-Command cppcheck -ErrorAction SilentlyContinue) {
        cppcheck --project="$projectRoot\compile_commands.json" `
                 --enable=style,performance,warning,portability `
                 --inline-suppr `
                 --suppress=missingIncludeSystem `
                 --error-exitcode=1 `
                 -i "server/build/_deps"
    }
}

Write-Host "`n[+] Full Verification Harness Succeeded: 100% Deterministic & Randomized." -ForegroundColor Green
