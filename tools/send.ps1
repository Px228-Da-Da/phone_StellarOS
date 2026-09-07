# Отправить приложение Hittis в телефон по проводу.
#
#   powershell -File tools\send.ps1 apps\demo.slt
#
# Зачем это здесь, а не в tools/ht.sh: последовательный порт телефона
# виден только из Windows. WSL к COM-портам доступа не имеет вовсе, а
# именно там живёт компилятор. Поэтому сборка идёт в WSL, а отправка —
# отсюда; ht.bat связывает обе половины одной командой.
#
# Устройство посылки описано в kernel/include/appload.h. Коротко:
#
#     !SLT <байт> <сумма>
#     <шестнадцатеричные цифры>
#     !END
#
# Разметка из латиницы, тело — шестнадцатеричное. На проводе не должно
# быть ничего, что зависит от кодировки терминала.

param([Parameter(Mandatory=$true)][string]$Path)

if (-not (Test-Path $Path)) {
    Write-Host "нет файла: $Path" -ForegroundColor Red
    exit 1
}

$bytes = [System.IO.File]::ReadAllBytes($Path)
if ($bytes.Length -eq 0) {
    Write-Host "файл пустой" -ForegroundColor Red
    exit 1
}
if ($bytes.Length -gt 16384) {
    Write-Host "приложение $($bytes.Length) байт — больше 16384 телефон не примет" -ForegroundColor Red
    exit 1
}

# Ищем порт телефона по описанию, а не по номеру: номер меняется от
# перетыкания кабеля, а имя устройства — нет.
$port = $null
try {
    $dev = Get-CimInstance Win32_SerialPort -ErrorAction Stop |
           Where-Object { $_.PNPDeviceID -like "*VID_0BC4*" } |
           Select-Object -First 1
    if ($dev) { $port = $dev.DeviceID }
} catch { }
if (-not $port) { $port = [System.IO.Ports.SerialPort]::GetPortNames() | Select-Object -First 1 }
if (-not $port) {
    Write-Host "телефон не найден: StellarOS не подключён или не загрузился" -ForegroundColor Red
    exit 1
}

# Сумма — обычная сумма байт. Её задача не защитить от злого умысла, а
# поймать обрыв посылки: половина приложения хуже, чем никакого.
$sum = 0
foreach ($b in $bytes) { $sum = ($sum + $b) -band 0xFFFFFFFF }

$hex = New-Object System.Text.StringBuilder
for ($i = 0; $i -lt $bytes.Length; $i++) {
    [void]$hex.Append($bytes[$i].ToString("x2"))
    if ((($i + 1) % 32) -eq 0) { [void]$hex.Append("`n") }
}

$payload = "!SLT $($bytes.Length) $sum`n" + $hex.ToString() + "`n!END`n"

try {
    $sp = New-Object System.IO.Ports.SerialPort($port, 115200, "None", 8, "One")
    $sp.WriteTimeout = 5000
    $sp.Open()
} catch {
    Write-Host "порт $port занят — закрой окно консоли и повтори" -ForegroundColor Yellow
    exit 1
}

try {
    # Небольшими кусками: приёмное кольцо в телефоне четыре килобайта, а
    # вычитывает его обычная задача. Вывалить всё разом значит устроить
    # переполнение на ровном месте.
    $step = 512
    for ($i = 0; $i -lt $payload.Length; $i += $step) {
        $n = [Math]::Min($step, $payload.Length - $i)
        $sp.Write($payload.Substring($i, $n))
        Start-Sleep -Milliseconds 12
    }
    Write-Host "отправлено $($bytes.Length) байт в $port, сумма $sum" -ForegroundColor Green
    Write-Host "телефон должен показать приложение сам"
} finally {
    $sp.Close()
}
