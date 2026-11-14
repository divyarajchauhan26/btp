param(
  [Parameter(Mandatory = $true)]
  [string]$Binary,
  [string]$OutputDirectory = "results/cp14"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$resolvedBinary = (Resolve-Path -LiteralPath $Binary).Path
$output = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$allowed = [IO.Path]::GetFullPath((Join-Path $root "results"))
if (-not $output.StartsWith($allowed + [IO.Path]::DirectorySeparatorChar)) {
  throw "output directory must be below the repository results directory"
}
New-Item -ItemType Directory -Force -Path $output | Out-Null

function Parse-KeyValues([string]$Line, [char]$Separator, [char]$Assignment) {
  $values = @{}
  foreach ($field in $Line.Split($Separator)) {
    $pieces = $field.Split(@($Assignment), 2)
    if ($pieces.Count -eq 2) {
      $values[$pieces[0]] = $pieces[1]
    }
  }
  return $values
}

function Run-Case(
  [string]$Sweep,
  [string]$Value,
  [int]$Endpoints,
  [UInt64]$Payload,
  [int]$Rails,
  [int]$SwitchEnabled,
  [UInt64]$QueueCapacity,
  [UInt64]$Credits,
  [UInt64]$CreditLatency,
  [UInt64]$ErrorPdu,
  [UInt64]$ErrorAttempts,
  [UInt64]$Retries,
  [UInt64]$ReplayLatency
) {
  $safeValue = $Value -replace '[^A-Za-z0-9_.-]', '_'
  $configPath = Join-Path $output ($Sweep + "_" + $safeValue + ".workload")
  $algorithm = if ($Sweep -eq "queue_capacity" -or
      $Sweep -eq "credit_latency") {
    "gather_reduce_broadcast"
  } else {
    "ring"
  }
  $config = @(
    "schema_version=1",
    "deterministic_seed=20261005",
    "endpoint_count=$Endpoints",
    "collective_algorithm=$algorithm",
    "link_bandwidth_gbps=200",
    "propagation_delay_ns=10",
    "transaction_payload_bytes=256",
    "pdu_payload_bytes=4096",
    "pdu_overhead_bytes=0",
    "rail_count=$Rails",
    "switch_enabled=$SwitchEnabled",
    "switch_queue_capacity_pdus=$QueueCapacity",
    "switch_latency_ns=0",
    "switch_credits_per_egress=$Credits",
    "credit_return_latency_ns=$CreditLatency",
    "error_injection_pdu_id=$ErrorPdu",
    "error_injection_attempts=$ErrorAttempts",
    "maximum_retries=$Retries",
    "replay_latency_ns=$ReplayLatency",
    "layer=0,100,allreduce,$Payload",
    "layer=1,50,none,0"
  )
  Set-Content -LiteralPath $configPath -Value $config -Encoding Ascii
  $report = & $resolvedBinary $configPath
  if ($LASTEXITCODE -ne 0) {
    throw "simulation failed for $Sweep/$Value"
  }
  $endLine = $report | Where-Object { $_ -like "end_to_end_ns=*" }
  $layerLine = $report | Where-Object { $_ -like "layer=0,*" }
  $counterLine = $report | Where-Object { $_ -like "counters=*" }
  $layer = Parse-KeyValues $layerLine ',' '='
  $counters = Parse-KeyValues $counterLine.Substring(9) ',' ':'
  [UInt64]$backpressure = 0
  [UInt64]$creditStarvation = 0
  foreach ($line in ($report | Where-Object { $_ -like "switch_egress=*" })) {
    $queue = Parse-KeyValues $line ',' '='
    $backpressure += [UInt64]$queue["backpressure_events"]
    $creditStarvation += [UInt64]$queue["credit_starvation_events"]
  }
  [UInt64]$maximumRailBusy = 0
  foreach ($line in ($report | Where-Object { $_ -like "rail=*" })) {
    $rail = Parse-KeyValues $line ',' '='
    $maximumRailBusy = [Math]::Max(
        $maximumRailBusy, [UInt64]$rail["busy_time_ns"])
  }
  [UInt64]$expectedBytes = 2 * ($Endpoints - 1) * $Payload
  [UInt64]$logicalBytes = $counters["logical_operation_bytes"]
  if ($logicalBytes -ne $expectedBytes) {
    throw "ring byte formula failed for $Sweep/$Value"
  }
  [UInt64]$wireBytes = $counters["transmitted_wire_bytes"]
  [UInt64]$idealSerialization =
      [Math]::Ceiling(($wireBytes * 8.0) / (200.0 * $Rails))
  [UInt64]$exposed = $layer["exposed_communication_ns"]
  if ($exposed -lt $idealSerialization -or
      $exposed -lt $maximumRailBusy) {
    throw "physical serialization lower bound failed for $Sweep/$Value"
  }
  return [PSCustomObject]@{
    sweep = $Sweep
    value = $Value
    algorithm = $algorithm
    endpoints = $Endpoints
    payload_bytes = $Payload
    rails = $Rails
    end_to_end_ns = [UInt64]$endLine.Substring(14)
    exposed_communication_ns = $exposed
    logical_operation_bytes = $logicalBytes
    transmitted_wire_bytes = $wireBytes
    ideal_serialization_ns = $idealSerialization
    maximum_rail_busy_ns = $maximumRailBusy
    pdus = [UInt64]$counters["pdus"]
    retries = [UInt64]$counters["retries"]
    backpressure_events = $backpressure
    credit_starvation_events = $creditStarvation
  }
}

$rows = @()
foreach ($payload in @(256, 1024, 4096)) {
  $rows += Run-Case "payload" "$payload" 8 $payload 1 0 0 0 0 0 0 0 0
}
foreach ($endpoints in @(2, 4, 8, 16)) {
  $rows += Run-Case "endpoints" "$endpoints" $endpoints 4096 1 0 0 0 0 0 0 0 0
}
foreach ($rails in @(1, 2, 4)) {
  $rows += Run-Case "rails" "$rails" 8 4096 $rails 0 0 0 0 0 0 0 0
}
foreach ($capacity in @(1, 4)) {
  $rows += Run-Case "queue_capacity" "$capacity" 8 4096 2 1 $capacity 0 0 0 0 0 0
}
foreach ($latency in @(0, 10, 100)) {
  $rows += Run-Case "credit_latency" "$latency" 8 4096 2 1 4 1 $latency 0 0 0 0
}
foreach ($latency in @(0, 10, 100)) {
  $rows += Run-Case "replay_latency" "$latency" 8 4096 1 0 0 0 0 1 1 1 $latency
}

function Assert-Monotonic([string]$Sweep, [bool]$Increasing) {
  $series = @($rows | Where-Object { $_.sweep -eq $Sweep })
  for ($index = 1; $index -lt $series.Count; ++$index) {
    $prior = $series[$index - 1].end_to_end_ns
    $current = $series[$index].end_to_end_ns
    if (($Increasing -and $current -lt $prior) -or
        (-not $Increasing -and $current -gt $prior)) {
      throw "$Sweep end-to-end trend is not monotonic"
    }
  }
}

Assert-Monotonic "payload" $true
Assert-Monotonic "endpoints" $true
Assert-Monotonic "rails" $false
Assert-Monotonic "credit_latency" $true
Assert-Monotonic "replay_latency" $true

$summary = Join-Path $output "summary.csv"
$rows | Export-Csv -LiteralPath $summary -NoTypeInformation -Encoding Ascii

$traceRows = @()
foreach ($trace in @(
    @{ name = "llm_tp_proxy"; bytes = 114688 },
    @{ name = "moe_ep_proxy"; bytes = 86016 })) {
  $tracePath = Join-Path $root (
      "native/tests/data/" + $trace.name + ".workload")
  $traceReport = & $resolvedBinary $tracePath
  if ($LASTEXITCODE -ne 0) {
    throw "trace execution failed: $($trace.name)"
  }
  $counterLine = $traceReport | Where-Object { $_ -like "counters=*" }
  $counters = Parse-KeyValues $counterLine.Substring(9) ',' ':'
  if ([UInt64]$counters["logical_operation_bytes"] -ne $trace.bytes) {
    throw "trace byte formula failed: $($trace.name)"
  }
  $reportPath = Join-Path $output ($trace.name + ".report")
  Set-Content -LiteralPath $reportPath -Value $traceReport -Encoding Ascii
  $endLine = $traceReport | Where-Object { $_ -like "end_to_end_ns=*" }
  $traceRows += [PSCustomObject]@{
    trace = $trace.name
    end_to_end_ns = [UInt64]$endLine.Substring(14)
    logical_operation_bytes = [UInt64]$counters["logical_operation_bytes"]
    pdus = [UInt64]$counters["pdus"]
    credits_consumed = [UInt64]$counters["credits_consumed"]
  }
}
$traceRows | Export-Csv -LiteralPath (Join-Path $output "trace_summary.csv") `
    -NoTypeInformation -Encoding Ascii
Write-Output "research sweep passed: $summary"
