param(
    [string]$Compiler = '/home/sky/insty-build/insty',
    [string]$OutputDir = 'C:\Users\sky\AppData\Local\Temp\opencode',
    [ValidateSet('x86_64_windows', 'x86_64_linux')]
    [string]$Target = 'x86_64_windows',
    [switch]$IncludeBootstrapRepros
)

$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $OutputDir -PathType Container)) {
    throw "Output directory must already exist: $OutputDir"
}
$Repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
function WslPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path).Replace('\', '/')
    return '/mnt/' + $full.Substring(0, 1).ToLowerInvariant() + $full.Substring(2)
}
$ObjectDir = (WslPath $OutputDir) + '/array_new_contract_objects_' + $Target

function Build-Run([string]$Source, [string]$Name, [int]$Expected = 0) {
    $exe = Join-Path $OutputDir ($Name + '_' + $Target + '.exe')
    & wsl -e $Compiler --target $Target --allocator runtime --bounds-check `
        --objects-dir $ObjectDir -L (WslPath (Join-Path $Repo 'compiler-v2\src')) `
        (WslPath $Source) -o (WslPath $exe)
    if ($LASTEXITCODE -ne 0) { throw "$Name compile exit=$LASTEXITCODE" }
    if ($Target -eq 'x86_64_windows') { & $exe }
    else { & wsl -e (WslPath $exe) }
    $status = $LASTEXITCODE
    Write-Host "$Name [$Target] exit=$status (expected $Expected)"
    if ($status -ne $Expected) { throw "$Name failed: exit=$status" }
}

Build-Run (Join-Path $PSScriptRoot 'fixtures\array_new_contract.ins') 'array_new_contract'
Build-Run (Join-Path $PSScriptRoot 'fixtures\new_runtime_count.ins') 'array_new_legacy_count' 9
Build-Run (Join-Path $PSScriptRoot 'fixtures\new_ctor.ins') 'array_new_legacy_ctor' 39

# Negative contracts are generated only in the caller-supplied output directory.
$negative = @{
    mismatch = 'fun main() -> i32 { i64* p = new i32[2] return 0 }'
    implicit_decay = 'fun main() -> i32 { i32[] s = new i32[2] i32* p = s return 0 }'
    pointer_assignment = 'fun main() -> i32 { i32* p = 0 p = new i32[2] return 0 }'
    scalar_slice = 'fun main() -> i32 { i32[] s = new i32 return 0 }'
    delete_scalar = 'fun main() -> i32 { delete 1 return 0 }'
    delete_array = 'fun main() -> i32 { i32[2] a delete a return 0 }'
    bool_count = 'fun main() -> i32 { auto s = new i32[true] return 0 }'
    wide_count = 'fun main() -> i32 { i128 n = 2 auto s = new i32[n] return 0 }'
}
foreach ($name in $negative.Keys) {
    $source = Join-Path $OutputDir ('array_new_reject_' + $name + '.ins')
    [IO.File]::WriteAllText($source, "module main`n" + $negative[$name] + "`n")
    # Windows PowerShell 5.1 wraps redirected native stderr in ErrorRecords.
    # Semantic errors are expected here; inspect the native exit and diagnostic.
    $ErrorActionPreference = 'Continue'
    $diagnostics = & wsl -e $Compiler --check --allocator runtime (WslPath $source) 2>&1
    $status = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($status -eq 0 -or "$diagnostics" -notmatch 'E200[45]') {
        throw "Negative case $name did not produce its semantic error: exit=$status $diagnostics"
    }
    Write-Host "array_new_reject_$name exit=$status (expected semantic rejection)"
}

if ($IncludeBootstrapRepros) {
    Build-Run (Join-Path $Repo 'compiler-v2\tests\allocator_heap_repro.ins') 'array_new_heap_repro'
    Build-Run (Join-Path $Repo 'compiler-v2\tests\allocator_aggregate_return_repro.ins') 'array_new_aggregate_return_repro'
}
exit 0
