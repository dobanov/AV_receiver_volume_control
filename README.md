Receiver Tray Volume Controller

Небольшое WinAPI-приложение для управления громкостью сетевого AV-ресивера через UPnP (SOAP).
Работает из системного трея и управляется горячими клавишами.

Возможности

  Управление громкостью ресивера по сети (UPnP / SOAP)
  Горячие клавиши:
F13 — уменьшить громкость
F14 — увеличить громкость

  Отображение текущей громкости в трее
  Динамическая цветная иконка:

тихо → светло-голубой
средне → синий / зелёный
громко → жёлтый / оранжевый / красный

  Автообновление громкости

  Сборка

Пример для MSYS2 (UCRT64):

soap version

gcc -O2 -mwindows receiver_tray.c -o receiver_tray.exe -lwininet -lshell32 -luser32 -lgdi32

eISCP version

gcc -O2 -mwindows receiver_tray_iscp.c -o receiver_tray_eiscp.exe -lws2_32 -lshell32 -luser32 -lgdi32


Настройка

Отредактируй параметры в коде:

#define DEVICE_IP   "192.168.1.53"
#define DEVICE_PORT 8888
#define CONTROL_URL "/Control/oap/RenderingControl"

 Эти значения зависят от модели ресивера.

 Работа протестирована с pioneer VSX-LX503

 Лицензия

Свободное использование без ограничений.
