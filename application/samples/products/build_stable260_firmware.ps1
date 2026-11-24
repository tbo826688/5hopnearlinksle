param(
    [string]$OutputDirectory = "",
    [int]$Jobs = 8
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$configPath = Join-Path $repoRoot "build\config\target_config\ws63\menuconfig\acore\ws63_liteos_app.config"
$oldConfigPath = "$configPath.old"
$configTool = Join-Path $repoRoot "build\script\usr_config.py"
$buildTool = Join-Path $repoRoot "build.py"
$packagePath = Join-Path $repoRoot "output\ws63\fwpkg\ws63-liteos-app\ws63-liteos-app_all.fwpkg"
$mconfigPath = Join-Path $repoRoot "output\ws63\acore\ws63-liteos-app\mconfig.h"
$toolchainRuntime = "C:\Program Files\HiSpark Studio\tools\Windows\cc_riscv32_musl_fp_win\bin"

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot "output\Stable260-5Hop-Firmware"
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$logDirectory = Join-Path $OutputDirectory "logs"
$configDirectory = Join-Path $OutputDirectory "configs"
$backupDirectory = Join-Path $env:TEMP ("stable260-config-backup-" + [Guid]::NewGuid().ToString("N"))

$commonAssignments = @(
    "ENABLE_PRODUCTS_SAMPLE=y",
    "ENABLE_ALL_PRODUCTS_SAMPLE=n",
    "SAMPLE_SUPPORT_ROUTE_BRIDGE=n",
    "SLE_TWO_HOP_VALIDATION_MODE=n"
)

$roles = @(
    @{
        FileName = "01_Node1_SLE_Server_Stable260.fwpkg"
        Label = "Node1"
        Assignments = $commonAssignments + @(
            "SAMPLE_SUPPORT_ROUTE=n",
            "SAMPLE_SUPPORT_SLE_UART=y",
            "SAMPLE_SUPPORT_SLE_UART_SERVER=y",
            "SAMPLE_FILE_TRANSFER_MODE=y",
            "SLE_UART_BUS=0",
            "UART_TXD_PIN=17",
            "UART_RXD_PIN=18"
        )
        Required = @(
            "CONFIG_SAMPLE_SUPPORT_SLE_UART=y",
            "CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER=y",
            "CONFIG_SAMPLE_FILE_TRANSFER_MODE=y",
            "CONFIG_SLE_UART_BUS=0"
        )
    },
    @{
        FileName = "02_R1_SLE_Route_ID1_Stable260.fwpkg"
        Label = "R1"
        RouterId = 1
    },
    @{
        FileName = "03_R2_SLE_Route_ID2_Stable260.fwpkg"
        Label = "R2"
        RouterId = 2
    },
    @{
        FileName = "04_R3_SLE_Route_ID3_Stable260.fwpkg"
        Label = "R3"
        RouterId = 3
    },
    @{
        FileName = "05_R4_SLE_Route_ID4_Stable260.fwpkg"
        Label = "R4"
        RouterId = 4
    },
    @{
        FileName = "06_Node6_SLE_Client_Stable260.fwpkg"
        Label = "Node6"
        Assignments = $commonAssignments + @(
            "SAMPLE_SUPPORT_ROUTE=n",
            "SAMPLE_SUPPORT_SLE_UART=y",
            "SAMPLE_SUPPORT_SLE_UART_CLIENT=y",
            "SAMPLE_FILE_TRANSFER_MODE=y",
            "SLE_UART_BUS=0",
            "UART_TXD_PIN=17",
            "UART_RXD_PIN=18"
        )
        Required = @(
            "CONFIG_SAMPLE_SUPPORT_SLE_UART=y",
            "CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT=y",
            "CONFIG_SAMPLE_FILE_TRANSFER_MODE=y",
            "CONFIG_SLE_UART_BUS=0"
        )
    }
)

foreach ($role in $roles) {
    if ($role.ContainsKey("RouterId")) {
        $routerId = [int]$role.RouterId
        $role.Assignments = $commonAssignments + @(
            "SAMPLE_SUPPORT_SLE_UART=n",
            "SAMPLE_SUPPORT_ROUTE=y",
            "SAMPLE_SUPPORT_ROUTE_ALL=y",
            "ROUTE_NODE_ID=$routerId",
            "ROUTE_OUTDOOR_LONG_RANGE=y"
        )
        $role.Required = @(
            "CONFIG_SAMPLE_SUPPORT_ROUTE=y",
            "CONFIG_SAMPLE_SUPPORT_ROUTE_ALL=y",
            "CONFIG_ROUTE_NODE_ID=$routerId",
            "CONFIG_ROUTE_OUTDOOR_LONG_RANGE=y"
        )
    }
}

function Invoke-CheckedCommand {
    param(
        [string]$Label,
        [string]$LogPath,
        [scriptblock]$Command
    )

    Write-Host "[$Label]" -ForegroundColor Cyan
    & $Command *> $LogPath
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[$Label] failed; log tail:" -ForegroundColor Red
        Get-Content -LiteralPath $LogPath -Tail 80
        throw "$Label failed with exit code $LASTEXITCODE"
    }
}

function Assert-Config {
    param(
        [string]$Text,
        [string[]]$RequiredLines,
        [string]$RoleLabel
    )

    foreach ($requiredLine in $RequiredLines) {
        if ($Text -notmatch "(?m)^$([regex]::Escape($requiredLine))\r?$") {
            throw "$RoleLabel configuration is missing: $requiredLine"
        }
    }
    if ($Text -match "(?m)^CONFIG_SAMPLE_SUPPORT_ROUTE_BRIDGE=y\r?$") {
        throw "$RoleLabel still has the dual-board route bridge enabled"
    }
}

function Move-GeneratedArtifactAside {
    param(
        [string]$Path,
        [string]$Label
    )

    if (!(Test-Path -LiteralPath $Path)) {
        return
    }

    $generatedRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot "output\ws63"))
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $requiredPrefix = $generatedRoot.TrimEnd('\') + '\'
    $isGeneratedRoot = $fullPath.Equals($generatedRoot, [System.StringComparison]::OrdinalIgnoreCase)
    if (!$isGeneratedRoot -and !$fullPath.StartsWith($requiredPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to move a path outside the generated ws63 output: $fullPath"
    }

    $artifactBackupDirectory = Join-Path $backupDirectory "generated-artifacts"
    New-Item -ItemType Directory -Force -Path $artifactBackupDirectory | Out-Null
    $destination = Join-Path $artifactBackupDirectory ("$Label-" + [Guid]::NewGuid().ToString("N"))

    for ($attempt = 1; $attempt -le 10; $attempt++) {
        try {
            Move-Item -Force -LiteralPath $fullPath -Destination $destination
            return
        } catch {
            if ($attempt -eq 10) {
                throw
            }
            Start-Sleep -Milliseconds (100 * $attempt)
        }
    }
}

New-Item -ItemType Directory -Force -Path $OutputDirectory, $logDirectory, $configDirectory, $backupDirectory | Out-Null
Copy-Item -LiteralPath $configPath -Destination (Join-Path $backupDirectory "ws63_liteos_app.config")
$hadOldConfig = Test-Path -LiteralPath $oldConfigPath
if ($hadOldConfig) {
    Copy-Item -LiteralPath $oldConfigPath -Destination (Join-Path $backupDirectory "ws63_liteos_app.config.old")
}
$originalConfigHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $configPath).Hash

if (Test-Path -LiteralPath $toolchainRuntime) {
    $env:PATH = "$toolchainRuntime;$env:PATH"
}

$builtPackages = New-Object System.Collections.Generic.List[object]

Push-Location $repoRoot
try {
    foreach ($role in $roles) {
        $label = [string]$role.Label
        $configLog = Join-Path $logDirectory ("$label-config.log")
        $buildLog = Join-Path $logDirectory ("$label-build.log")

        Invoke-CheckedCommand -Label "$label configure" -LogPath $configLog -Command {
            & py -3.11 $configTool --command setconfig --chip ws63 --core acore --target ws63-liteos-app @($role.Assignments)
        }

        $configText = Get-Content -Raw -LiteralPath $configPath
        Assert-Config -Text $configText -RequiredLines $role.Required -RoleLabel $label

        Move-GeneratedArtifactAside `
            -Path (Join-Path $repoRoot "output\ws63") `
            -Label "$label-ws63-output"

        Invoke-CheckedCommand -Label "$label build" -LogPath $buildLog -Command {
            & py -3.11 $buildTool ws63-liteos-app "-j$Jobs"
        }

        if (!(Test-Path -LiteralPath $packagePath)) {
            throw "$label build completed but package was not found: $packagePath"
        }
        if (!(Test-Path -LiteralPath $mconfigPath)) {
            throw "$label build completed but mconfig.h was not found: $mconfigPath"
        }

        $destination = Join-Path $OutputDirectory $role.FileName
        Copy-Item -Force -LiteralPath $packagePath -Destination $destination
        Copy-Item -Force -LiteralPath $mconfigPath -Destination (Join-Path $configDirectory ("$label-mconfig.h"))
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
        $size = (Get-Item -LiteralPath $destination).Length
        $builtPackages.Add([pscustomobject]@{
            Role = $label
            File = $role.FileName
            Size = $size
            SHA256 = $hash
        })
        Write-Host "[$label] package ready: $($role.FileName)" -ForegroundColor Green
    }

    $uniqueHashes = @($builtPackages | Select-Object -ExpandProperty SHA256 -Unique)
    if ($uniqueHashes.Count -ne $builtPackages.Count) {
        throw "Package verification failed: at least two board roles produced identical images"
    }

    $manifestPath = Join-Path $OutputDirectory "SHA256SUMS.txt"
    $manifestLines = foreach ($package in $builtPackages) {
        "$($package.SHA256)  $($package.File)"
    }
    Set-Content -Encoding ASCII -LiteralPath $manifestPath -Value $manifestLines

    $readmePath = Join-Path $OutputDirectory "烧录说明.txt"
    $readme = @(
        "NearLink SLE 主从一体五跳 Stable260 烧录包",
        "",
        "烧录顺序与文件：",
        "1. Node1 -> 01_Node1_SLE_Server_Stable260.fwpkg",
        "2. R1    -> 02_R1_SLE_Route_ID1_Stable260.fwpkg",
        "3. R2    -> 03_R2_SLE_Route_ID2_Stable260.fwpkg",
        "4. R3    -> 04_R3_SLE_Route_ID3_Stable260.fwpkg",
        "5. R4    -> 05_R4_SLE_Route_ID4_Stable260.fwpkg",
        "6. Node6 -> 06_Node6_SLE_Client_Stable260.fwpkg",
        "",
        "拓扑：Node1 -> R1 -> R2 -> R3 -> R4 -> Node6",
        "Node1 和 Node6 的 USB 串口参数：921600 8N1",
        "完整五跳模式已关闭两跳验证开关；R1-R4 使用户外长监督超时。",
        "上位机：application\tools\sle_transfer_gui\dist\SLE-5Hop-Transfer-Stable260.exe",
        "",
        "烧录后必须以 Node6 文件长度及 SHA-256 校验结果作为最终成功依据。"
    )
    Set-Content -Encoding UTF8 -LiteralPath $readmePath -Value $readme
} finally {
    Pop-Location
    Copy-Item -Force -LiteralPath (Join-Path $backupDirectory "ws63_liteos_app.config") -Destination $configPath
    if ($hadOldConfig) {
        Copy-Item -Force -LiteralPath (Join-Path $backupDirectory "ws63_liteos_app.config.old") -Destination $oldConfigPath
    } elseif (Test-Path -LiteralPath $oldConfigPath) {
        Remove-Item -Force -LiteralPath $oldConfigPath
    }
    try {
        Remove-Item -Force -Recurse -LiteralPath $backupDirectory
    } catch {
        Write-Warning "Temporary build backup could not be removed and was left at: $backupDirectory"
    }
}

$restoredConfigHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $configPath).Hash
if ($restoredConfigHash -ne $originalConfigHash) {
    throw "The original ws63_liteos_app.config was not restored exactly"
}

Write-Host "All six packages completed." -ForegroundColor Green
Write-Host "Output: $OutputDirectory"
$builtPackages | Format-Table -AutoSize
