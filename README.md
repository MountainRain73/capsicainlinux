# Capsicain

A keyboard remapping tool for Linux that rewires keys and creates powerful key combinations — without changing how you type.

Originally a Windows tool (using Interception), now fully ported to Linux using the native **evdev + uinput** subsystem.

## Features

- **Key Rewiring** — Remap any key to any other key. Tap vs hold can produce different keys (e.g., CapsLock tap = Esc, hold = Ctrl).
- **Combo System** — Define modifier + key combinations with AND/OR/NOT logic. Support for virtual modifiers (MOD9–MOD15) that don't interfere with real modifier keys.
- **Alt+HJKL Cursor Navigation** — Move cursor with Alt+HJKL (arrows), Alt+Shift+HJKL (select), Alt+CapsLock+HL (word jump), plus Home/End/PgUp/PgDown.
- **Alt+Tab Passthrough** — Native Alt+Tab window switching works through capsicain's modifier proxy.
- **Multiple Config Layers** — Toggle between different key mapping configurations at runtime (ESC+1/2/3...).
- **Daemon Mode** — Run in background with `-d`, stop with `--kill`. Logs to `/tmp/capsicain.log`.
- **Zero Dependencies** — Uses only standard C++17 and Linux kernel headers (`linux/input.h`, `linux/uinput.h`). No external libraries required.

## Quick Start

### Option A: Pre-built Binary

Download the latest `release/capsicain` and `release/capsicain.ini`, then:

```bash
sudo modprobe uinput
chmod +x capsicain
sudo ./capsicain
# Press ESC+X to exit
```

### Option B: Build from Source

```bash
# Prerequisites: cmake, g++, Linux kernel headers (included in most distros)
git clone https://github.com/yourname/capsicain.git
cd capsicain

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

sudo ./capsicain
```

## Command Line Options

| Flag | Description |
|------|-------------|
| (none) | Run in foreground. Press `Ctrl+C` or `ESC+X` to exit. |
| `-d`, `--daemon` | Run as a background daemon. Detaches from terminal. |
| `-c`, `--config PATH` | Use a custom config file path (default: `./capsicain.ini`). |
| `--kill` | Stop a running daemon instance. |
| `-h`, `--help` | Show help. |

```bash
# Typical daemon workflow
sudo ./capsicain -d                     # Start in background
sudo ./capsicain -d -c /etc/capsicain.ini  # With custom config
sudo ./capsicain --kill                 # Stop
cat /tmp/capsicain.log                  # View logs
```

## Configuration

Keyboard mappings are defined in `capsicain.ini`. The default configuration provides:

```
REWIRE CAPS LCTRL ESC     # CapsLock: tap=Esc, hold=LCtrl
REWIRE LALT MOD9 NOP      # LAlt → virtual MOD9 (prevents app shortcuts)
COMBO LSHF [.... ...&] > key(CAPS)       # Shift+Shift = CapsLock toggle
COMBO TAB  [.... ...&] > alttab()        # Alt+Tab passthrough
COMBO H    [.... ...&] > key(LEFT)       # Alt+H = Left Arrow
COMBO J    [.... ...&] > key(DOWN)       # Alt+J = Down Arrow
COMBO K    [.... ...&] > key(UP)         # Alt+K = Up Arrow
COMBO L    [.... ...&] > key(RIGHT)      # Alt+L = Right Arrow
# ... and many more
```

For a complete reference of all mappings, features, and the full configuration syntax, see the **[capsicain-mappings-reference-linux.md](capsicain-mappings-reference-linux.md)**.

## System Requirements

- Linux with `uinput` kernel module (`sudo modprobe uinput`)
- Root privileges (required for `/dev/input/*` and `/dev/uinput` access)
- Any architecture: x86_64, ARM (Raspberry Pi), etc.

## Auto-start on Boot (systemd)

```bash
# Install
sudo cp release/capsicain /usr/local/bin/
sudo mkdir -p /etc/capsicain
sudo cp capsicain.ini /etc/capsicain/

# Ensure uinput loads on boot
echo "uinput" | sudo tee /etc/modules-load.d/capsicain.conf

# Create systemd service
sudo tee /etc/systemd/system/capsicain.service << 'EOF'
[Unit]
Description=Capsicain Keyboard Remapper
After=multi-user.target

[Service]
Type=forking
ExecStart=/usr/local/bin/capsicain -d -c /etc/capsicain/capsicain.ini
PIDFile=/tmp/capsicain.pid
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now capsicain
```

## Project Structure

```
capsicainlinux/
├── src/                          # Source code
│   ├── capsicain.h / .cpp        # Main program logic, config parsing
│   ├── linux_input.h / .cpp      # Linux evdev/uinput abstraction layer
│   ├── configUtils.h / .cpp      # INI config file parser
│   ├── scancodes.h / .cpp        # PS/2 ↔ Linux keycode mapping tables
│   ├── modifiers.h / .cpp        # Modifier bitmask definitions
│   ├── constants.h               # Shared constants
│   └── utils.h / .cpp            # Utility functions
├── CMakeLists.txt                # Build configuration
├── capsicain.ini                 # Default key mapping config
├── capsicain-mappings-reference-linux.md  # Full technical reference
├── release/                      # Pre-built binary for quick use
│   ├── capsicain
│   └── capsicain.ini
├── README.md
├── LICENSE
└── .gitignore
```

## How It Works

1. **Capture** — Uses `EVIOCGRAB` to exclusively grab all physical keyboard devices via evdev.
2. **Process** — Runs the capsicain engine: modifier tracking, key rewiring, COMBO matching.
3. **Send** — Outputs modified key events through a virtual uinput keyboard device.

See the [technical reference](capsicain-mappings-reference-linux.md) for detailed implementation documentation including the PS/2 ↔ Linux keycode mapping tables, daemonization internals, and complete function reference.

## License

MIT License — see [LICENSE](LICENSE) for details.

## Acknowledgments

- Original Windows version inspired by [Interception](https://github.com/oblitum/Interception)
- Ported to Linux using native evdev/uinput (no Interception dependency)