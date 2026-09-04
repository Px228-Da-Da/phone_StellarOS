# Терминал VELO-OS поверх USB.
#
# Телефон представляется последовательным портом класса CDC-ACM, поэтому
# драйвер в Windows встроенный и ставить ничего не нужно. Порт находим не
# по номеру, а по нашим идентификаторам: номер COM у Windows плавает от
# подключения к подключению.
#
# Кодировку задаём явно: ядро печатает в UTF-8, а порт по умолчанию
# читается как ASCII, и кириллица превращается в вопросительные знаки.

$VID = 'VID_0BC4'

Write-Host "Ищу VELO-OS на USB..." -ForegroundColor DarkGray

while ($true) {
    $dev = Get-PnpDevice -PresentOnly -Class Ports -ErrorAction SilentlyContinue |
           Where-Object { $_.InstanceId -match $VID }

    if ($null -eq $dev) { Start-Sleep -Milliseconds 500; continue }
    if ($dev.FriendlyName -notmatch 'COM(\d+)') { Start-Sleep -Milliseconds 500; continue }

    $com = "COM$($matches[1])"
    Write-Host "Подключаюсь к $com" -ForegroundColor Green

    $p = New-Object System.IO.Ports.SerialPort $com, 115200, None, 8, one
    $p.Encoding = [System.Text.Encoding]::UTF8
    $p.ReadTimeout = 500

    try { $p.Open() }
    catch {
        # Обычное дело: телефон ещё перезагружается или ядро пока не
        # дошло до подъёма консоли. Просто ждём и пробуем снова.
        Start-Sleep -Seconds 1
        continue
    }

    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    Write-Host "--- вывод ядра, Ctrl+C для выхода ---" -ForegroundColor DarkGray

    try {
        while ($p.IsOpen) {
            $s = $p.ReadExisting()
            if ($s.Length) { [Console]::Write($s) }
            Start-Sleep -Milliseconds 30
        }
    } catch {
        Write-Host "`nсвязь потеряна: $($_.Exception.Message)" -ForegroundColor DarkYellow
    } finally {
        if ($p.IsOpen) { $p.Close() }
    }

    Write-Host "Жду телефон снова..." -ForegroundColor DarkGray
    Start-Sleep -Seconds 1
}
