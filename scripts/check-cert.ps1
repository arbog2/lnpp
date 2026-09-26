#Requires -Version 5.1
<#
.SYNOPSIS
  检查 <lnpp 根目录>\ssl\arbog.top.pem 到期时间，14 天内到期则告警并写日志。
  可由 Windows 任务计划每天触发，或手动运行。
  使用: powershell -NoProfile -ExecutionPolicy Bypass -File <lnpp>\scripts\check-cert.ps1
  Dry-run: .\check-cert.ps1 -WhatIf  (只打印不写日志)

.DESCRIPTION
  路径全部从脚本自身位置推导，不再硬编码 D:\code\lnpp —— 硬编码的 bin\nginx\1.30
  在只装了 1.30.4 的机器上不存在，那条 nginx -t 分支于是永远静默跳过，日志里的
  nginxTest=True 是默认值而不是检测结果。nginx 路径现在从 data\settings.ini 的
  ver.nginx 读取，取不到时自动挑选 bin\nginx 下最新的版本目录；找不到任何 nginx
  时会明确报告 nginxTest=NOTFOUND，而不是假装通过。
#>
param([switch]$WhatIf)

$ErrorActionPreference = 'Stop'

# <repo>\scripts\ -> <repo>\
$root    = Split-Path -Parent $PSScriptRoot
$certPath = Join-Path $root 'ssl\arbog.top.pem'
$logDir   = Join-Path $root 'logs'
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

# Resolve the nginx version the manager actually uses (data\settings.ini
# ver.nginx), so this keeps working across version switches.
$nginxVer = $null
$settings = Join-Path $root 'data\settings.ini'
if (Test-Path $settings) {
  $m = Select-String -Path $settings -Pattern '^\s*ver\.nginx\s*=\s*(.+?)\s*$' -ErrorAction SilentlyContinue
  if ($m) { $nginxVer = $m.Matches[0].Groups[1].Value }
}
if (-not $nginxVer) {
  $binNginx = Join-Path $root 'bin\nginx'
  if (Test-Path $binNginx) {
    $dirs = @(Get-ChildItem $binNginx -Directory | Sort-Object Name -Descending)
    if ($dirs.Count -gt 0) { $nginxVer = $dirs[0].Name }
  }
}

$nginxExe    = if ($nginxVer) { Join-Path $root "bin\nginx\$nginxVer\nginx.exe" } else { $null }
$nginxPrefix = if ($nginxVer) { Join-Path $root "data\nginx\$nginxVer" } else { $null }

# also check that nginx can still load the config that references the cert
$nginxState = 'NOTFOUND'
if ($nginxExe -and (Test-Path $nginxExe)) {
  # nginx writes "syntax is ok" to stderr, which under $ErrorActionPreference =
  # 'Stop' turns into a terminating NativeCommandError. Start-Process keeps the
  # message out of the PowerShell error stream; only the exit code matters here.
  $proc = Start-Process -FilePath $nginxExe -ArgumentList @('-t', '-p', "`"$nginxPrefix`"", '-c', 'conf\nginx.conf') `
                        -NoNewWindow -Wait -PassThru
  $nginxState = if ($proc.ExitCode -ne 0) { 'FAIL' } else { 'OK' }
}

$msg = "cert=$domain end=$endStr daysLeft=$daysLeft status=$status nginxVer=$nginxVer nginxTest=$nginxState"
Write-Log $msg

# Balloon/toast when expiring: also write a sentinel file so lnpp.exe (or any
# monitor) can surface it. NOTE: lnpp.exe does not read cert-alert.txt yet — it
# only shows up if you run this by hand or read the log.
if ($status -in @('EXPIRED','EXPIRING_SOON') -or $nginxState -eq 'FAIL') {
  $alertFile = Join-Path $logDir 'cert-alert.txt'
  if (-not $WhatIf) {
    Set-Content -Path $alertFile -Value $msg -Encoding utf8
    Write-Host "[ALERT] $msg" -ForegroundColor Red
  } else {
    Write-Host "[ALERT] $msg" -ForegroundColor Yellow
  }
  if ($status -eq 'EXPIRED') { exit 2 }
  if ($nginxState -eq 'FAIL') { exit 3 }
  exit 1
}

Write-Log "cert OK, no action needed"
exit 0
