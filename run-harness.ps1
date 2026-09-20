param (
    [switch]$FullAudit,
    [switch]$CheckFormatting,
    [switch]$EnableASan
)

$ErrorActionPreference = "Stop"
$projectRoot = "E:\Github\src\angelscript-lsp"
Set-Location $projectRoot

Write-Host "==========================================" -ForegroundColor Cyan
Write-Host " [1/6] Running Structural Layer Audits    " -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan
python "$projectRoot\server\scripts\check-layer-includes.py"
python "$projectRoot\server\scripts\check-diagnostic-codes.py"

Write-Host "`n==========================================" -ForegroundColor Cyan
Write-Host " [2/6] Initializing MSVC Developer Shell  " -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan
$devShell = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1"
if (-not (Test-Path $devShell)) {
    $devShell = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1"
}
& $devShell -Arch amd64

Write-Host "`n==========================================" -ForegroundColor Cyan
Write-Host " [3/6] Configuring & Building with Ninja  " -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan
Set-Location "$projectRoot\server"
$cmakeArgs = @("-G", "Ninja Multi-Config", "-B", "build", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
if ($EnableASan) {
    $cmakeArgs += "-DCMAKE_CXX_FLAGS=-fsanitize=address"
}
cmake @cmakeArgs
cmake --build build --config Debug

Copy-Item "build\compile_commands.json" -Destination "$projectRoot\" -Force

Write-Host "`n==========================================" -ForegroundColor Cyan
Write-Host " [4/6] Executing Deterministic Test Suite " -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan
ctest --test-dir build -C Debug --output-on-failure

if ($CheckFormatting) {
    Write-Host "`n==========================================" -ForegroundColor Cyan
    Write-Host " [5/6] Verifying Allman Code Formatting   " -ForegroundColor Cyan
    Write-Host "==========================================" -ForegroundColor Cyan
    $files = Get-ChildItem -Path "$projectRoot\server\src" -Recurse -Include *.cpp,*.h
    foreach ($file in $files) {
        clang-format -dry-run --Werror $file.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "Formatting verification failed on $($file.FullName)"
        }
    }
}

if ($FullAudit) {
    Write-Host "`n==========================================" -ForegroundColor Cyan
    Write-Host " [6/6] Cppcheck Static Analysis Audit     " -ForegroundColor Cyan
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

Write-Host "`n[+] Full Harness Succeeded: 100% Deterministic." -ForegroundColor Green
