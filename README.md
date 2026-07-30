# Pioneer AVR Tray Controller

A lightweight Windows tray application for controlling a Pioneer AV receiver over the network using the **eISCP (Ethernet Integra Serial Control Protocol)**.

The application lives in the system tray, automatically connects to the receiver, displays the current volume, and provides quick access to power and input controls.

## Features

* Automatic connection and reconnection to the AVR
* Real-time volume display in the system tray icon
* Color-coded tray icon indicating receiver state
* Volume popup with mouse drag and wheel support
* Global hotkeys (F13/F14) for volume control
* Power On / Standby control
* Input source selection
* Lightweight native Win32 application
* No external dependencies

## Receiver States

| State      | Description                                    |
| ---------- | ---------------------------------------------- |
| Offline    | Receiver is unreachable                        |
| Connecting | Connection established or waiting for status   |
| Standby    | Receiver is powered off                        |
| Online     | Receiver is powered on and volume is displayed |

## Controls

### Tray Icon

* **Left Click** – Open volume popup
* **Right Click** – Open context menu

### Volume Popup

* Drag the slider
* Mouse wheel adjusts volume
* Arrow keys adjust volume
* **Esc** closes the popup

### Global Hotkeys

| Key | Action      |
| --- | ----------- |
| F13 | Volume Down |
| F14 | Volume Up   |

## Configuration

Edit the following constants in the source code:

```c
#define DEVICE_IP   "192.168.1.53"
#define DEVICE_PORT 60128
```

The default eISCP port is **60128**.

## Building

The project is intended to be built with **MSYS2 (UCRT64)** using the MinGW-w64 GCC toolchain.

### Prerequisites

Install the UCRT64 environment and required packages:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc
```

Open the **MSYS2 UCRT64** shell and compile:

```sh
gcc -Os -mwindows receiver_tray_iscp.c -o receiver_tray_iscp.exe ^
    -lws2_32 -lshell32 -luser32 -lgdi32 -lcomctl32 ^
    -s -fno-exceptions -fno-unwind-tables ^
    -fno-asynchronous-unwind-tables -fomit-frame-pointer ^
    -falign-functions=1 -falign-jumps=1 ^
    -falign-loops=1 -falign-labels=1
```

Or as a single command:

```sh
gcc -Os -mwindows receiver_tray_iscp.c -o receiver_tray_iscp.exe -lws2_32 -lshell32 -luser32 -lgdi32 -lcomctl32 -s -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -fomit-frame-pointer -falign-functions=1 -falign-jumps=1 -falign-loops=1 -falign-labels=1
```

The resulting executable is a small native Win32 application with no external runtime dependencies beyond the standard Windows system libraries.


## Implementation Notes

* Uses native Win32 API only.
* Uses Winsock2 for TCP communication.
* Implements the Pioneer eISCP protocol.
* Dedicated worker thread for network I/O.
* Handles partial TCP packets and stream resynchronization.
* Automatic reconnection after connection loss.
* Graceful shutdown using an event and socket shutdown.
* Thread-safe command queue.

## License

Released under the MIT License.
