param(
    [Parameter(Mandatory = $true)]
    [string]$MsiPath,
    [Parameter(Mandatory = $true)]
    [string]$RebuiltMsiPath,
    [Parameter(Mandatory = $true)]
    [string]$UpgradeMsiPath
)

$ErrorActionPreference = 'Stop'
$MsiPath = (Resolve-Path $MsiPath).Path
$RebuiltMsiPath = (Resolve-Path $RebuiltMsiPath).Path
$UpgradeMsiPath = (Resolve-Path $UpgradeMsiPath).Path
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) "shatter-msi-$([guid]::NewGuid())"
$installRoot = Join-Path $testRoot 'Shatter Install'
$binPath = Join-Path $installRoot 'bin'
New-Item -ItemType Directory -Path $testRoot | Out-Null

function Invoke-Installer(
    [string]$Operation, [string]$LogName, [string]$ExtraArguments = '',
    [string]$PackagePath = $MsiPath, [bool]$ExpectDowngrade = $false
) {
    $logPath = Join-Path $testRoot $LogName
    $arguments = "$Operation `"$PackagePath`" /qn /norestart /L*v `"$logPath`""
    $arguments += " $ExtraArguments"
    $process = Start-Process msiexec.exe -ArgumentList $arguments -Wait -PassThru
    if ($ExpectDowngrade) {
        if ($process.ExitCode -in @(0, 3010) -or
            (Get-Content $logPath -Raw) -notmatch 'A later version of Shatter is already installed') {
            Get-Content $logPath | Write-Host
            throw 'Downgrade was not rejected with the expected version error'
        }
        return
    }
    if ($process.ExitCode -notin @(0, 3010)) {
        Get-Content $logPath | Write-Host
        throw "MSI $Operation failed with exit code $($process.ExitCode)"
    }
    if ($Operation -eq '/i') { $script:installedPackage = $PackagePath }
}

function Test-InstalledPath {
    $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    return @($machinePath -split ';' | ForEach-Object { $_.TrimEnd('\') } | Where-Object { $_ -eq $binPath }).Count
}

function Get-StartupCommand {
    return Get-ItemPropertyValue -Path 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run' `
        -Name Shatter -ErrorAction SilentlyContinue
}

function Get-MsiProperty([string]$PackagePath, [string]$PropertyName) {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $null
    $view = $null
    $record = $null
    try {
        $database = $installer.OpenDatabase($PackagePath, 0)
        $query = 'SELECT `Value` FROM `Property` WHERE `Property` = ''{0}''' -f $PropertyName
        $view = $database.OpenView($query)
        $view.Execute()
        $record = $view.Fetch()
        return $record.StringData(1)
    }
    finally {
        foreach ($comObject in @($record, $view, $database, $installer)) {
            if ($null -ne $comObject) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($comObject) }
        }
    }
}

function Assert-Installation([bool]$Startup, [string]$PackagePath) {
    if (-not (Test-Path $binary)) { throw 'MSI did not preserve the installed executable location' }
    & $binary help | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Installed shatter help failed: $LASTEXITCODE" }
    if ((Test-InstalledPath) -ne 1) { throw 'Shatter must occur exactly once in the machine PATH' }
    $root = Get-ItemPropertyValue -Path 'HKLM:\Software\Shatter\Installer' -Name InstallRoot
    if ($root.TrimEnd('\') -ne $installRoot) { throw "Install location changed: $root" }
    $expectedStartup = if ($Startup) { "`"$binary`" start" } else { $null }
    if ((Get-StartupCommand) -ne $expectedStartup) { throw 'Startup preference changed unexpectedly' }
    $products = @(Get-ItemProperty 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*' |
        Where-Object { $_.DisplayName -eq 'Shatter' })
    if ($products.Count -ne 1) { throw "Expected one installed Shatter product, found $($products.Count)" }
    if ($products[0].PSChildName -ne (Get-MsiProperty $PackagePath 'ProductCode') -or
        $products[0].DisplayVersion -ne (Get-MsiProperty $PackagePath 'ProductVersion')) {
        throw 'The expected MSI product/version is not the registered installation'
    }
    if ((Get-Content $userFile -Raw) -ne $userContent) { throw 'Installation changed an existing user file' }
}

$binary = Join-Path $binPath 'shatter.exe'
if ((Get-MsiProperty $MsiPath 'ProductCode') -eq (Get-MsiProperty $RebuiltMsiPath 'ProductCode')) {
    throw 'Same-version replacement fixture must use a different ProductCode'
}
if ((Get-MsiProperty $MsiPath 'ProductVersion') -ne (Get-MsiProperty $RebuiltMsiPath 'ProductVersion') -or
    [version](Get-MsiProperty $UpgradeMsiPath 'ProductVersion') -le [version](Get-MsiProperty $MsiPath 'ProductVersion')) {
    throw 'MSI fixtures must include a same-version replacement and a newer version'
}
foreach ($startup in @($false, $true)) {
    $installed = $false
    $installOptions = "INSTALL_ROOT=`"$installRoot`""
    if ($startup) { $installOptions += ' ADDLOCAL=ALL' }
    try {
        Invoke-Installer '/i' "install-$startup.log" $installOptions
        $installed = $true
        # An unowned file must survive replacement and uninstall.
        $userFile = Join-Path $installRoot 'user-settings.json'
        $userContent = '{"preserve":"existing settings"}'
        [IO.File]::WriteAllText($userFile, $userContent)
        Assert-Installation $startup $MsiPath
        # Subsequent installs intentionally omit INSTALL_ROOT and feature choices.
        Invoke-Installer '/i' "repeat-$startup.log"
        Assert-Installation $startup $MsiPath
        Invoke-Installer '/i' "repair-$startup.log" 'REINSTALL=ALL REINSTALLMODE=amus'
        Assert-Installation $startup $MsiPath
        Invoke-Installer '/i' "replace-$startup.log" '' $RebuiltMsiPath
        Assert-Installation $startup $RebuiltMsiPath
        Invoke-Installer '/i' "upgrade-$startup.log" '' $UpgradeMsiPath
        Assert-Installation $startup $UpgradeMsiPath
        Invoke-Installer '/i' "repeat-upgrade-$startup.log" '' $UpgradeMsiPath
        Assert-Installation $startup $UpgradeMsiPath
        Invoke-Installer '/i' "downgrade-$startup.log" '' $MsiPath $true
        Assert-Installation $startup $UpgradeMsiPath

        $startupCommand = Get-StartupCommand
        if ($startup) {
            if ($startupCommand -ne "`"$binary`" start") { throw "Incorrect startup command: $startupCommand" }
            # Exercise the same executable and arguments that run at sign-in.
            & $binary start
            if ($LASTEXITCODE -ne 0) { throw 'Installed startup command failed' }
            & $binary stop
            if ($LASTEXITCODE -ne 0) { throw 'Installed engine failed to stop' }
            Invoke-Installer '/i' 'disable-startup.log' 'REMOVE=ShatterAutostart' $UpgradeMsiPath
            if (Get-StartupCommand) { throw 'Changing startup to disabled left the Run entry behind' }
            Invoke-Installer '/i' 'enable-startup.log' 'ADDLOCAL=ShatterAutostart' $UpgradeMsiPath
            if ((Get-StartupCommand) -ne "`"$binary`" start") { throw 'Enabling startup during maintenance failed' }
        }
        elseif ($startupCommand) { throw 'Default installation enabled startup without consent' }
    }
    finally {
        if ($installed) {
            & $binary stop | Out-Null
            Invoke-Installer '/x' "uninstall-$startup.log" '' $script:installedPackage
        }
    }

    if (Test-Path $binary) { throw 'Uninstall left shatter.exe behind' }
    if (Test-InstalledPath) { throw 'Uninstall left Shatter in the machine PATH' }
    if (Get-StartupCommand) { throw 'Uninstall left the startup entry behind' }
    if ((Get-Content $userFile -Raw) -ne $userContent) { throw 'Uninstall removed an existing user file' }
}
Remove-Item -Recurse -Force $testRoot
Write-Host 'MSI repeated install, repair, replacement, upgrade, downgrade rejection, startup, PATH, and uninstall checks passed.'
