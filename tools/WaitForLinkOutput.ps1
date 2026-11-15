# Wait for transient Windows file locks before MSVC attempts to link.
# Never terminates processes or silently deletes a previous executable.
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [int]$TimeoutSeconds = 30
)
$ErrorActionPreference = 'Stop'
if (-not [System.IO.File]::Exists($Executable)) { exit 0 }
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$lastError = ''
do {
    try {
        $stream = [System.IO.File]::Open(
            $Executable,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None
        )
        $stream.Dispose()
        if ($lastError) {
            Write-Host "Another Chance: archivo liberado; continuando con el enlazado."
        }
        exit 0
    } catch {
        $lastError = $_.Exception.Message
        Start-Sleep -Milliseconds 350
    }
} while ([DateTime]::UtcNow -lt $deadline)
$message = ("No se puede reemplazar '{0}' tras {1} s. Windows informa: {2}. " +
    "No se cerraron procesos ni se eliminó el archivo. Verifica permisos, " +
    "antivirus y procesos que tengan abierto el ejecutable.") -f $Executable, $TimeoutSeconds, $lastError
Write-Error $message
exit 1
