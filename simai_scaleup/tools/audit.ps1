param(
  [string]$Binary = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$legacyPaths = @(
  ".gitmodules",
  "SimAI-CLEM",
  "SimCCL",
  "aicb",
  "astra-sim-alibabacloud",
  "docs",
  "example",
  "ns-3-alibabacloud",
  "scripts",
  "vidur-alibabacloud"
)
foreach ($relative in $legacyPaths) {
  $path = Join-Path $root $relative
  if (Test-Path -LiteralPath $path) {
    throw "legacy production path remains: $relative"
  }
}

$forbidden = "\b(rdma|roce|infiniband|htsim|garnet|dcqcn|ns-?3)\b|queue[ _-]?pair|completion[ _-]?queue|AstraNetworkAPI"
$productionPaths = @(
  (Join-Path $root "CMakeLists.txt"),
  (Join-Path $root "README.md"),
  (Join-Path $root "native")
)
$matches = & rg -n -i $forbidden @productionPaths
if ($LASTEXITCODE -eq 0) {
  $matches | Write-Output
  throw "forbidden production vocabulary found"
}
if ($LASTEXITCODE -ne 1) {
  throw "production vocabulary scan failed"
}

$cmake = Get-Content -Raw -LiteralPath (Join-Path $root "native\CMakeLists.txt")
$sources = Get-ChildItem -LiteralPath (Join-Path $root "native\src") -Filter "*.cpp"
foreach ($source in $sources) {
  $entry = "src/" + $source.Name
  if (-not $cmake.Contains($entry)) {
    throw "native source is unreachable from CMake: $entry"
  }
}

if ($Binary -ne "") {
  $resolvedBinary = (Resolve-Path -LiteralPath $Binary).Path
  $symbolMatches = & nm -C $resolvedBinary |
      Select-String -Pattern $forbidden -CaseSensitive:$false
  if ($symbolMatches) {
    $symbolMatches | Write-Output
    throw "forbidden binary symbol found"
  }
  $linkageMatches = & objdump -p $resolvedBinary |
      Select-String -Pattern $forbidden -CaseSensitive:$false
  if ($linkageMatches) {
    $linkageMatches | Write-Output
    throw "forbidden binary linkage found"
  }
}

Write-Output "zero-contamination audit passed"
