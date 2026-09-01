#Requires -Version 5.1
<#
.SYNOPSIS
  检查 D:\code\lnpp\ssl\arbog.top.pem 到期时间，14 天内到期则告警并写日志。
  可由 Windows 任务计划每天触发，或手动运行。
  使用: powershell -NoProfile -ExecutionPolicy Bypass -File D:\code\lnpp\scripts\check-cert.ps1
  Dry-run: .\check-cert.ps1 -WhatIf  (只打印不写日志)
#>
param([switch]$WhatIf)

$ErrorActionPreference = 'Stop'
$certPath = 'D:\code\lnpp\ssl\arbog.top.pem'
$logDir   = 'D:\code\lnpp\logs'
$logFile  = Join-Path $logDir 'cert-check.log'
$warnDays = 14
$domain   = 'jw.arbog.top'

function Write-Log($msg) {
  $line = "[$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')] $msg"
  if (-not $WhatIf) {
    if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force $logDir | Out-Null }
    Add-Content -Path $logFile -Value $line -Encoding utf8
  }
  Write-Host $line
}

if (-not (Test-Path $certPath)) {
  $m = "FAIL cert file missing: $certPath"
  Write-Log $m; exit 1
}

# prefer openssl if available, fallback to .NET X509Certificate2
$endDate = $null
$openssl = Get-Command openssl -ErrorAction SilentlyContinue
if ($openssl) {
  $raw = & openssl x509 -in $certPath -noout -enddate 2>$null
  if ($raw -match 'notAfter=(.+)') {
    $s = $Matches[1].Trim()
    try { $endDate = [DateTime]::Parse($s, [Globalization.CultureInfo]::InvariantCulture) } catch { $endDate = $null }
  }
}
if (-not $endDate) {
  try {
    $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($certPath)
    $endDate = $cert.NotAfter.ToUniversalTime()
  } catch {
    Write-Log "FAIL cannot parse cert: $_"; exit 1
  }
}

$utcNow = (Get-Date).ToUniversalTime()
$daysLeft = [Math]::Floor(($endDate - $utcNow).TotalDays)
$endStr  = $endDate.ToString('yyyy-MM-dd HH:mm:ss UTC')
$status  = if ($daysLeft -lt 0) { 'EXPIRED' } elseif ($daysLeft -le $warnDays) { 'EXPIRING_SOON' } else { 'OK' }

# also check nginx reload after cert replace is possible (nginx -t)
$nginxOk = $true
$nginxExe = 'D:\code\lnpp\bin\nginx\1.30\nginx.exe'
$nginxPrefix = 'D:\code\lnpp\data\nginx\1.30'
if (Test-Path $nginxExe) {
  $t = & $nginxExe -t -p $nginxPrefix 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { $nginxOk = $false; $status = 'NGINX_CONFIG_FAIL' }
}

$msg = "cert=$domain end=$endStr daysLeft=$daysLeft status=$status nginxTest=$nginxOk"
Write-Log $msg

# balloon/ElMessage-style notification when expiring: write a sentinel file that lnpp or any monitor can surface
if ($status -in @('EXPIRED','EXPIRING_SOON','NGINX_CONFIG_FAIL')) {
  $alertFile = Join-Path $logDir 'cert-alert.txt'
  if (-not $WhatIf) {
    Set-Content -Path $alertFile -Value $msg -Encoding utf8
    # optional: toast via BurntToast if installed; otherwise just log
    try {
      Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
      # non-blocking notification is intentionally not using MessageBox (blocks task)
      Write-Host "[ALERT] $msg — please renew $certPath and run: nginx -s reload -p $nginxPrefix" -ForegroundColor Red
    } catch {}
  } else {
    Write-Host "[ALERT] $msg" -ForegroundColor Yellow
  }
  if ($status -eq 'EXPIRED') { exit 2 }
  if ($status -eq 'NGINX_CONFIG_FAIL') { exit 3 }
  exit 1
}

Write-Log "cert OK, no action needed"
exit 0
