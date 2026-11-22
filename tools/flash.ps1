# Rekadeck firmware installer for Windows. Double-click flash.bat in the main folder, or run it with options from a
# terminal opened in that folder:
#
#   .\flash.bat                install or update the firmware on a Rekadeck plugged in over USB-C. An update keeps
#                              the board's Wi-Fi, its link to your account and any reviews it hasn't synced yet.
#   .\flash.bat --erase        wipe the board completely first, for a fresh start
#   .\flash.bat --port COM5    use this serial port instead of looking for the board
#   .\flash.bat --check        get everything ready (check the firmware files, download esptool) and stop there
#
# The first run downloads Espressif's esptool, the program that writes the firmware, into .tools\ in the main folder
# (about 50 MB, only once). Nothing is installed anywhere else. Set ESPTOOL to the path of your own esptool to use it.
#
# Written for Windows PowerShell 5.1, the one built into Windows 10 and 11.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Windows PowerShell's progress bar slows downloads to a crawl

# esptool's standalone build from https://github.com/espressif/esptool/releases, checked against this hash
$EsptoolVersion = 'v4.13.0'
$EsptoolAsset = 'windows-amd64'
$EsptoolSha256 = 'd48f3f0111cb67c53d5f760fc7f61860abfb5306629aa6d5c2c0cdcb91d54bbe'

$EspUsbVendor = '303A'   # Espressif: the ESP32-S3's own USB port, which is how the Rekadeck's board connects
$WaitSeconds = 300       # how long to wait for the board to show up

$Root = Split-Path -Parent $PSScriptRoot
$Fw = Join-Path $Root 'firmware\prebuilt'
$Tools = Join-Path $Root '.tools'

function Say([string]$Text = '') { Write-Host $Text }

function Fail([string]$Text) {
  Write-Host ''
  Write-Host "Error: $Text" -ForegroundColor Red
  exit 1
}

function Show-Usage {
  $lines = @(Get-Content -LiteralPath $PSCommandPath)
  for ($i = 0; $i -lt $lines.Count -and $lines[$i].StartsWith('#'); $i++) {
    Write-Host ($lines[$i] -replace '^# ?', '')
  }
}

$Erase = $false
$Check = $false
$PortGiven = ''
$i = 0
while ($i -lt $args.Count) {
  $a = [string]$args[$i]
  if ($a -eq '--erase' -or $a -eq '-erase') { $Erase = $true }
  elseif ($a -eq '--check' -or $a -eq '-check') { $Check = $true }
  elseif ($a -eq '--port' -or $a -eq '-port') {
    $i++
    if ($i -ge $args.Count) { Fail '--port needs a port name, like COM5' }
    $PortGiven = [string]$args[$i]
  }
  elseif ($a -like '--port=*') { $PortGiven = $a.Substring(7) }
  elseif ($a -eq '--help' -or $a -eq '-h' -or $a -eq '/?') { Show-Usage; exit 0 }
  else { Fail "unknown option $a (.\flash.bat --help lists them)" }
  $i++
}
$Port = $PortGiven

function Get-Sha256([string]$Path) {
  (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

# ------------------------------------------------------------------ the firmware in this folder
function Test-Firmware {
  foreach ($f in 'bootloader.bin', 'partitions.bin', 'boot_app0.bin', 'rekadeck.bin', 'SHA256SUMS', 'VERSION') {
    if (-not (Test-Path -LiteralPath (Join-Path $Fw $f) -PathType Leaf)) {
      Fail "firmware\prebuilt\$f is missing. Download this folder again."
    }
  }
  foreach ($line in @(Get-Content -LiteralPath (Join-Path $Fw 'SHA256SUMS'))) {
    $parts = @($line.Trim() -split '\s+', 2)
    if ($parts.Count -lt 2) { continue }
    $sum = $parts[0].ToLowerInvariant()
    $name = $parts[1].TrimStart('*')
    $path = Join-Path $Fw $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
      Fail "firmware\prebuilt\$name is missing. Download this folder again."
    }
    if ((Get-Sha256 $path) -ne $sum) {
      Fail "firmware\prebuilt\$name is damaged (its checksum doesn't match). Download this folder again."
    }
  }
  (Get-Content -LiteralPath (Join-Path $Fw 'VERSION') -Raw).Trim()
}

# ------------------------------------------------------------------ esptool
function Get-Esptool {
  if ($env:ESPTOOL) {
    if (-not (Get-Command $env:ESPTOOL -ErrorAction SilentlyContinue)) {
      Fail "ESPTOOL is set to $($env:ESPTOOL), which isn't a program that runs"
    }
    return $env:ESPTOOL
  }
  if (-not [Environment]::Is64BitOperatingSystem) {
    $own = Get-Command esptool -ErrorAction SilentlyContinue
    if ($own) { return $own.Source }
    Fail "Espressif's ready-made esptool needs 64-bit Windows. Install it with: py -m pip install esptool, then run this again."
  }

  $dir = Join-Path $Tools "esptool-$EsptoolVersion-$EsptoolAsset"
  $exe = Join-Path $dir "esptool-$EsptoolAsset\esptool.exe"
  $done = Join-Path $dir 'unpacked'   # written last, so a half-unpacked folder is never used
  $antivirus = "If your antivirus removed or blocked esptool.exe, allow it (it's Espressif's official, signed build)"
  if (-not ((Test-Path -LiteralPath $done) -and (Test-Path -LiteralPath $exe -PathType Leaf))) {
    Save-Esptool $dir $exe $done $antivirus
  }
  if (-not (Test-Runs $exe)) {
    $own = Get-Command esptool -ErrorAction SilentlyContinue
    if ($own) { return $own.Source }
    Fail ("esptool.exe doesn't run on this computer. $antivirus and run this again. " +
          "Or install esptool with: py -m pip install esptool, then run this again.")
  }
  return $exe
}

function Test-Runs([string]$Exe) {
  $ErrorActionPreference = 'Continue'
  $global:LASTEXITCODE = 1   # stays 1 if the program can't even start
  try { & $Exe version *> $null } catch { return $false }
  return ($global:LASTEXITCODE -eq 0)
}

function Save-Esptool([string]$dir, [string]$exe, [string]$done, [string]$antivirus) {
  New-Item -ItemType Directory -Force -Path $Tools | Out-Null
  $zip = Join-Path $Tools 'esptool-download.zip'
  $url = "https://github.com/espressif/esptool/releases/download/$EsptoolVersion/esptool-$EsptoolVersion-$EsptoolAsset.zip"
  Say "Downloading esptool $EsptoolVersion, the program that writes the firmware (about 50 MB, only this once)..."
  try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
  } catch {
    Fail "couldn't download esptool ($($_.Exception.Message)). Check your internet connection and try again."
  }
  if ((Get-Sha256 $zip) -ne $EsptoolSha256) {
    Remove-Item -LiteralPath $zip -Force
    Fail "the esptool download didn't match its checksum, so it wasn't used. Try again."
  }
  # Unpacked in place rather than into a temporary folder that's renamed afterwards: Windows won't rename a folder
  # while an antivirus scan or OneDrive sync still has one of the new files open.
  try {
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $dir)
  } catch {
    Fail "couldn't unpack esptool ($($_.Exception.Message)). $antivirus and run this again."
  }
  Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
  if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    Fail "esptool.exe is missing after unpacking it. $antivirus and run this again."
  }
  Set-Content -LiteralPath $done -Value $EsptoolVersion
}

# ------------------------------------------------------------------ finding the board
function Get-BoardPorts {
  # Every COM port that belongs to an Espressif USB device. Its name reads like "USB Serial Device (COM5)".
  $filter = "PNPDeviceID LIKE 'USB\\VID_$EspUsbVendor%'"
  foreach ($d in @(Get-CimInstance -ClassName Win32_PnPEntity -Filter $filter -ErrorAction SilentlyContinue)) {
    if ($d.Name -match '\((COM\d+)\)') { $Matches[1] }
  }
}

function Stop-NoBoard {
  $others = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object -Unique)
  if ($PortGiven) {
    if ($others.Count -gt 0) { Fail ("$PortGiven isn't there. These serial ports are: " + ($others -join ', ')) }
    Fail "$PortGiven isn't there, and no serial ports are."
  }
  if ($others.Count -gt 0) {
    Fail ("no Rekadeck showed up. These serial ports are here: " + ($others -join ', ') +
          ". If one of them is the Rekadeck, run: .\flash.bat --port <that port>")
  }
  Fail "no Rekadeck showed up. Try another USB-C cable (some only charge) or another USB port, then run this again."
}

function Find-Board {
  $start = Get-Date
  $told = 0
  $wait = $WaitSeconds
  if ($PortGiven) { $wait = 15 }
  while ($true) {
    if ($PortGiven) {
      $ports = @([System.IO.Ports.SerialPort]::GetPortNames() | Where-Object { $_ -eq $PortGiven })
    } else {
      $ports = @(Get-BoardPorts | Sort-Object -Unique)
    }
    if ($ports.Count -eq 1) {
      $script:Port = $ports[0]
      break
    }
    if ($ports.Count -gt 1) {
      Say ''
      Say 'More than one board is plugged in:'
      for ($n = 0; $n -lt $ports.Count; $n++) { Say ("  {0}  {1}" -f ($n + 1), $ports[$n]) }
      $reply = Read-Host 'Which one is the Rekadeck? Type its number'
      $pick = 0
      if (-not [int]::TryParse($reply, [ref]$pick) -or $pick -lt 1 -or $pick -gt $ports.Count) {
        Fail "that isn't one of the numbers"
      }
      $script:Port = $ports[$pick - 1]
      break
    }
    $waited = ((Get-Date) - $start).TotalSeconds
    if ($told -eq 0 -and $PortGiven) {
      Say ''
      Say "Waiting for $PortGiven to show up..."
      $told = 2
    } elseif ($told -eq 0) {
      Say ''
      Say 'Looking for the Rekadeck. Plug it in with a USB-C cable that carries data (some cables only charge).'
      Say "If it's plugged in already, press one of its keys to wake it."
      $told = 1
    } elseif ($told -eq 1 -and $waited -ge 20) {
      Say "Still looking. Hold the board's BOOT button, press and let go of RESET, then let go of BOOT."
      $told = 2
    }
    if ($waited -ge $wait) { Stop-NoBoard }
    Start-Sleep -Milliseconds 300
  }
  Say "Found it on $script:Port."
}

# ------------------------------------------------------------------ writing it
function Invoke-Flash([string]$Esptool) {
  # Windows PowerShell can turn whatever a program writes to stderr into a stopping error (when the output is
  # redirected to a file, say). esptool's exit code is what says whether it worked.
  $ErrorActionPreference = 'Continue'
  for ($try = 1; $try -le 3; $try++) {
    $a = @('--chip', 'esp32s3', '--port', $script:Port, '--baud', '460800',
           '--before', 'default_reset', '--after', 'hard_reset',
           'write_flash', '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '4MB')
    if ($Erase) { $a += '--erase-all' }
    $a += @('0x0', (Join-Path $Fw 'bootloader.bin'), '0x8000', (Join-Path $Fw 'partitions.bin'),
            '0xe000', (Join-Path $Fw 'boot_app0.bin'), '0x10000', (Join-Path $Fw 'rekadeck.bin'))
    Say ''
    & $Esptool @a
    if ($LASTEXITCODE -eq 0) { return }
    if ($try -eq 3) { break }
    Say ''
    Say "That didn't work, trying again. If the board went to sleep, press one of its keys."
    Start-Sleep -Seconds 2
    Find-Board   # it may have restarted and come back under another name
  }
  Fail ("couldn't write the firmware. Close anything else using the board (a serial monitor, the Arduino IDE), " +
        "then hold BOOT, press and let go of RESET, let go of BOOT, and run this again.")
}

# ------------------------------------------------------------------ main
Say 'Rekadeck firmware installer'
$Version = Test-Firmware
Say "Firmware $Version"
$Esptool = Get-Esptool

if ($Check) {
  $ErrorActionPreference = 'Continue'   # as in Invoke-Flash: the exit code decides
  & $Esptool version
  if ($LASTEXITCODE -ne 0) { Fail "esptool didn't run ($Esptool)" }
  Say 'Ready: the firmware files check out and esptool runs.'
  exit 0
}

Find-Board
if ($Erase) {
  Say "Wiping the board and installing firmware $Version. Don't unplug it until this says it's done."
} else {
  Say "Installing firmware $Version. Don't unplug the board until this says it's done."
}
Invoke-Flash $Esptool

Say ''
Say "Done: Rekadeck firmware $Version is installed."
Say ''
Say "The board restarts by itself. If its screen hasn't changed after 15 seconds, press its RESET button."
Say 'A new board shows "Set up Wi-Fi": join the network it names from your phone, pick your home Wi-Fi,'
Say 'then link it to your account at the address it shows. The README has the details.'
exit 0
