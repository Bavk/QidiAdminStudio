param(
  [ValidatePattern('^[A-Za-z0-9_.-]+$')]
  [string]$Repository = 'QidiAdminStudio'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$gh = 'C:\Program Files\GitHub CLI\gh.exe'
if (-not (Test-Path -LiteralPath $gh)) { $gh = 'gh.exe' }

& $gh auth status 2>$null
if ($LASTEXITCODE -ne 0) {
  throw 'GitHub CLI is not authenticated. Run: gh auth login --web --git-protocol https'
}

Push-Location $root
try {
  $currentBranch = (git branch --show-current).Trim()
  if ([string]::IsNullOrWhiteSpace($currentBranch)) {
    git checkout -b main
    $currentBranch = 'main'
  }
  $name = (git config user.name).Trim()
  $email = (git config user.email).Trim()
  if ([string]::IsNullOrWhiteSpace($name) -or [string]::IsNullOrWhiteSpace($email)) {
    throw 'Set your author identity first: git config --global user.name "Your Name"; git config --global user.email "you@example.com"'
  }
  if (-not (git rev-parse --verify HEAD 2>$null)) {
    git add -A
    git commit -m 'Initial Qidi Admin Studio fork'
  }
  $login = (& $gh api user --jq .login).Trim()
  if ([string]::IsNullOrWhiteSpace($login)) { throw 'Could not determine the GitHub account.' }
  $fullName = "$login/$Repository"
  & $gh repo view $fullName 2>$null
  if ($LASTEXITCODE -ne 0) {
    & $gh repo create $fullName --public --source . --remote origin --push
  } else {
    if (-not (git remote get-url origin 2>$null)) {
      git remote add origin "https://github.com/$fullName.git"
    }
    git push --set-upstream origin $currentBranch
  }
  Write-Host "Published: https://github.com/$fullName"
  Write-Host 'Open Actions and run “Qidi Admin Studio — Windows”.'
} finally {
  Pop-Location
}
