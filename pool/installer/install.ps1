# Puzzle #135 Pool Universal Installer
# Windows — irm https://starnetlive.space/install-135.ps1 | iex

$RED    = "`e[31m"
$GREEN  = "`e[32m"
$YELLOW = "`e[33m"
$BLUE   = "`e[34m"
$CYAN   = "`e[36m"
$NC     = "`e[0m"

function Banner {
    Write-Host ""
    Write-Host "$CYAN=============================================== $NC"
    Write-Host "$CYAN  Puzzle #135 Pool — Universal Installer      $NC"
    Write-Host "$CYAN  Pollard's Kangaroo  |  ~13.5 BTC Prize      $NC"
    Write-Host "$CYAN=============================================== $NC"
    Write-Host ""
}

function Check-Python {
    $py = Get-Command python3 -ErrorAction SilentlyContinue
    if (-not $py) { $py = Get-Command python -ErrorAction SilentlyContinue }
    if (-not $py) {
        Write-Host "$RED[!] Python 3 is required but not found.$NC"
        Write-Host "$YELLOW    Please install Python 3.8+ from python.org and re-run.$NC"
        exit 1
    }
    $ver = & $py.Source --version 2>&1
    Write-Host "$GREEN[+] Python found: $ver$NC"
    return $py.Source
}

function Download-Installer {
    $url = "https://raw.githubusercontent.com/Soumya001/puzzle-135-pool/main/installer/install.py"
    $tmp = [System.IO.Path]::GetTempFileName() + ".py"
    Write-Host "$BLUE[*] Downloading installer...$NC"
    try {
        Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing
    } catch {
        Write-Host "$RED[!] Download failed: $_$NC"
        exit 1
    }
    Write-Host "$GREEN[+] Installer downloaded.$NC"
    Write-Host ""
    & $PYTHON $tmp
    Remove-Item -Force $tmp -ErrorAction SilentlyContinue
}

Banner
$PYTHON = Check-Python
Download-Installer
