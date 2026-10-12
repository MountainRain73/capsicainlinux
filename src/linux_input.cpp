#include "pch.h"
#include "linux_input.h"
#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cerrno>
#include <thread>
#include <chrono>
#include <vector>

using namespace std;

#define MAX_EVDEV_DEVICES 32

static int uinput_fd = -1;
static int epoll_fd = -1;
static vector<int> evdev_fds;
static vector<string> evdev_paths;

// ============================================================
// PS/2 Set 1 scancode <-> Linux evdev keycode mapping tables
// ============================================================

// Maps PS/2 Set 1 scancode -> Linux keycode
// -1 means no mapping (key not supported)
static int ps2_to_linux[256] = {};
static int linux_to_ps2[KEY_MAX + 1] = {};

static void init_scancode_maps()
{
    // Initialize with -1 (no mapping)
    for (int i = 0; i < 256; i++) ps2_to_linux[i] = -1;
    for (int i = 0; i <= KEY_MAX; i++) linux_to_ps2[i] = -1;

    // For many standard keys, PS/2 Set 1 scancode equals Linux keycode.
    // We define explicit mappings for keys where they differ or for clarity.

    #define MAP(ps2, linux_kc) \
        ps2_to_linux[ps2] = linux_kc; \
        if (linux_kc >= 0 && linux_kc <= KEY_MAX) linux_to_ps2[linux_kc] = ps2;

    // Standard keys where PS/2 == Linux (most keys 0x01-0x58)
    MAP(0x01, KEY_ESC);
    MAP(0x02, KEY_1);
    MAP(0x03, KEY_2);
    MAP(0x04, KEY_3);
    MAP(0x05, KEY_4);
    MAP(0x06, KEY_5);
    MAP(0x07, KEY_6);
    MAP(0x08, KEY_7);
    MAP(0x09, KEY_8);
    MAP(0x0A, KEY_9);
    MAP(0x0B, KEY_0);
    MAP(0x0C, KEY_MINUS);
    MAP(0x0D, KEY_EQUAL);
    MAP(0x0E, KEY_BACKSPACE);
    MAP(0x0F, KEY_TAB);
    MAP(0x10, KEY_Q);
    MAP(0x11, KEY_W);
    MAP(0x12, KEY_E);
    MAP(0x13, KEY_R);
    MAP(0x14, KEY_T);
    MAP(0x15, KEY_Y);
    MAP(0x16, KEY_U);
    MAP(0x17, KEY_I);
    MAP(0x18, KEY_O);
    MAP(0x19, KEY_P);
    MAP(0x1A, KEY_LEFTBRACE);
    MAP(0x1B, KEY_RIGHTBRACE);
    MAP(0x1C, KEY_ENTER);
    MAP(0x1D, KEY_LEFTCTRL);
    MAP(0x1E, KEY_A);
    MAP(0x1F, KEY_S);
    MAP(0x20, KEY_D);
    MAP(0x21, KEY_F);
    MAP(0x22, KEY_G);
    MAP(0x23, KEY_H);
    MAP(0x24, KEY_J);
    MAP(0x25, KEY_K);
    MAP(0x26, KEY_L);
    MAP(0x27, KEY_SEMICOLON);
    MAP(0x28, KEY_APOSTROPHE);
    MAP(0x29, KEY_GRAVE);
    MAP(0x2A, KEY_LEFTSHIFT);
    MAP(0x2B, KEY_BACKSLASH);
    MAP(0x2C, KEY_Z);
    MAP(0x2D, KEY_X);
    MAP(0x2E, KEY_C);
    MAP(0x2F, KEY_V);
    MAP(0x30, KEY_B);
    MAP(0x31, KEY_N);
    MAP(0x32, KEY_M);
    MAP(0x33, KEY_COMMA);
    MAP(0x34, KEY_DOT);
    MAP(0x35, KEY_SLASH);
    MAP(0x36, KEY_RIGHTSHIFT);
    MAP(0x37, KEY_KPASTERISK);
    MAP(0x38, KEY_LEFTALT);
    MAP(0x39, KEY_SPACE);
    MAP(0x3A, KEY_CAPSLOCK);
    MAP(0x3B, KEY_F1);
    MAP(0x3C, KEY_F2);
    MAP(0x3D, KEY_F3);
    MAP(0x3E, KEY_F4);
    MAP(0x3F, KEY_F5);
    MAP(0x40, KEY_F6);
    MAP(0x41, KEY_F7);
    MAP(0x42, KEY_F8);
    MAP(0x43, KEY_F9);
    MAP(0x44, KEY_F10);
    MAP(0x45, KEY_NUMLOCK);
    MAP(0x46, KEY_SCROLLLOCK);
    MAP(0x47, KEY_KP7);
    MAP(0x48, KEY_KP8);
    MAP(0x49, KEY_KP9);
    MAP(0x4A, KEY_KPMINUS);
    MAP(0x4B, KEY_KP4);
    MAP(0x4C, KEY_KP5);
    MAP(0x4D, KEY_KP6);
    MAP(0x4E, KEY_KPPLUS);
    MAP(0x4F, KEY_KP1);
    MAP(0x50, KEY_KP2);
    MAP(0x51, KEY_KP3);
    MAP(0x52, KEY_KP0);
    MAP(0x53, KEY_KPDOT);
    MAP(0x56, KEY_102ND);  // Left backslash on UK/Germany keyboards
    MAP(0x57, KEY_F11);
    MAP(0x58, KEY_F12);

    // Extended keys (E0 prefixed) - PS/2 values differ from Linux
    // PS/2 extended keys have bit 7 set (>= 0x80)
    MAP(0x9D, KEY_RIGHTCTRL);       // PS/2 E0 1D -> Linux KEY_RIGHTCTRL
    MAP(0xB8, KEY_RIGHTALT);        // PS/2 E0 38 -> Linux KEY_RIGHTALT
    MAP(0xC7, KEY_HOME);            // PS/2 E0 47 -> Linux KEY_HOME
    MAP(0xC8, KEY_UP);              // PS/2 E0 48 -> Linux KEY_UP
    MAP(0xC9, KEY_PAGEUP);          // PS/2 E0 49 -> Linux KEY_PAGEUP
    MAP(0xCB, KEY_LEFT);            // PS/2 E0 4B -> Linux KEY_LEFT
    MAP(0xCD, KEY_RIGHT);           // PS/2 E0 4D -> Linux KEY_RIGHT
    MAP(0xCF, KEY_END);             // PS/2 E0 4F -> Linux KEY_END
    MAP(0xD0, KEY_DOWN);            // PS/2 E0 50 -> Linux KEY_DOWN
    MAP(0xD1, KEY_PAGEDOWN);        // PS/2 E0 51 -> Linux KEY_PAGEDOWN
    MAP(0xD2, KEY_INSERT);          // PS/2 E0 52 -> Linux KEY_INSERT
    MAP(0xD3, KEY_DELETE);          // PS/2 E0 53 -> Linux KEY_DELETE
    MAP(0xDB, KEY_LEFTMETA);        // PS/2 E0 5B -> Linux KEY_LEFTMETA
    MAP(0xDC, KEY_RIGHTMETA);       // PS/2 E0 5C -> Linux KEY_RIGHTMETA
    MAP(0xDD, KEY_COMPOSE);         // PS/2 E0 5D -> Linux KEY_COMPOSE (Menu/Apps key)

    // Numeric keypad Enter
    MAP(0x9C, KEY_KPENTER);         // PS/2 E0 1C -> Linux KEY_KPENTER

    // Divide key
    MAP(0xB5, KEY_KPSLASH);         // PS/2 E0 35 -> Linux KEY_KPSLASH

    // Print Screen
    MAP(0xB7, KEY_SYSRQ);           // PS/2 E0 37 (or E0 2A E0 37) -> Linux KEY_SYSRQ

    // Pause/Break (not directly mappable, handled specially)
    MAP(0xC6, KEY_PAUSE);           // PS/2 -> Linux KEY_PAUSE

    // Multimedia keys
    MAP(0xA0, KEY_MUTE);
    MAP(0xA1, KEY_CALC);
    MAP(0xA2, KEY_PLAYPAUSE);
    MAP(0xA4, KEY_STOPCD);
    MAP(0xAE, KEY_VOLUMEDOWN);
    MAP(0xB0, KEY_VOLUMEUP);
    MAP(0xB2, KEY_HOMEPAGE);
    MAP(0xE5, KEY_SEARCH);
    MAP(0xE6, KEY_BOOKMARKS);
    MAP(0xE7, KEY_REFRESH);
    MAP(0xE8, KEY_STOP);
    MAP(0xE9, KEY_FORWARD);
    MAP(0xEA, KEY_BACK);
    MAP(0xEB, KEY_COMPUTER);
    MAP(0xEC, KEY_MAIL);
    MAP(0xED, KEY_MEDIA);

    // Power keys
    MAP(0xDE, KEY_POWER);
    MAP(0xDF, KEY_SLEEP);
    MAP(0xE3, KEY_WAKEUP);

    // Japanese/Korean keys
    MAP(0x70, KEY_KATAKANAHIRAGANA);
    MAP(0x71, KEY_HANJA);
    MAP(0x72, KEY_HANGEUL);
    MAP(0x73, KEY_RO);
    MAP(0x79, KEY_HENKAN);
    MAP(0x7B, KEY_MUHENKAN);
    MAP(0x7D, KEY_YEN);
    MAP(0x7E, KEY_KPJPCOMMA);
    MAP(0x94, KEY_HANJA);

    // Track/media keys
    MAP(0x90, KEY_PREVIOUSSONG);
    MAP(0x99, KEY_NEXTSONG);

    // Scan code 0x54 (Alt+Print) - no direct Linux equivalent,
    // but we keep it mapped to itself for internal handling
    MAP(0x54, KEY_SYSRQ);  // Same as print

    #undef MAP
}

int ps2ToLinuxKeycode(int ps2_scancode)
{
    if (ps2_scancode < 0 || ps2_scancode > 255) return -1;
    return ps2_to_linux[ps2_scancode];
}

int linuxToPs2Keycode(int linux_keycode)
{
    if (linux_keycode < 0 || linux_keycode > KEY_MAX) return -1;
    return linux_to_ps2[linux_keycode];
}

// ============================================================
// Linux evdev / uinput implementation
// ============================================================

static bool is_keyboard_device(int fd)
{
    unsigned long evbit = 0;
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), &evbit) < 0)
        return false;

    // Must have EV_KEY and EV_SYN
    if (!(evbit & (1 << EV_KEY))) return false;
    if (!(evbit & (1 << EV_SYN))) return false;

    // Check if it has typical keyboard keys (KEY_A, KEY_ESC, etc.)
    unsigned long keybit[KEY_CNT / (sizeof(unsigned long) * 8) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0)
        return false;

    auto has_key = [&](int code) -> bool {
        return (keybit[code / (sizeof(unsigned long) * 8)] >>
                (code % (sizeof(unsigned long) * 8))) & 1;
    };

    // A keyboard should have at least KEY_A, KEY_ESC, KEY_ENTER
    int key_count = 0;
    if (has_key(KEY_A)) key_count++;
    if (has_key(KEY_ESC)) key_count++;
    if (has_key(KEY_ENTER)) key_count++;
    if (has_key(KEY_SPACE)) key_count++;

    return key_count >= 3;
}

static int create_uinput_device()
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
        if (fd < 0) {
            cerr << "ERROR: Cannot open /dev/uinput. Make sure uinput module is loaded (modprobe uinput)" << endl;
            return -1;
        }
    }

    // Enable key event types
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_MSC);
    ioctl(fd, UI_SET_MSCBIT, MSC_SCAN);

    // Enable all possible key codes
    for (int i = 0; i <= KEY_MAX; i++) {
        ioctl(fd, UI_SET_KEYBIT, i);
    }

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5678;
    usetup.id.version = 1;
    strncpy(usetup.name, "Capsicain Virtual Keyboard", UINPUT_MAX_NAME_SIZE);

    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
        cerr << "ERROR: uinput UI_DEV_SETUP failed: " << strerror(errno) << endl;
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        cerr << "ERROR: uinput UI_DEV_CREATE failed: " << strerror(errno) << endl;
        close(fd);
        return -1;
    }

    cout << "uinput virtual keyboard created successfully" << endl;
    return fd;
}

static bool grab_device(int fd)
{
    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        cerr << "WARNING: Cannot grab device (fd=" << fd << "): " << strerror(errno) << endl;
        return false;
    }
    return true;
}

static bool wait_for_uinput_device()
{
    for (int attempt = 0; attempt < 20; attempt++) {
        DIR *dir = opendir("/dev/input");
        if (!dir) return false;

        struct dirent *entry;
        bool found = false;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            string path = string("/dev/input/") + entry->d_name;
            int fd = open(path.c_str(), O_RDONLY);
            if (fd < 0) continue;

            char name[256] = {0};
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            close(fd);

            if (strstr(name, "Capsicain Virtual Keyboard") != nullptr) {
                found = true;
                cout << "Virtual keyboard detected as: " << path << " (" << name << ")" << endl;
                break;
            }
        }
        closedir(dir);

        if (found) return true;
        linux_sleep_ms(100);  // Wait 100ms between checks
    }
    return false;
}

bool linux_input_init()
{
    init_scancode_maps();

    // ============================================================
    // Step 1: Create uinput device FIRST, BEFORE grabbing keyboards
    // ============================================================
    uinput_fd = create_uinput_device();
    if (uinput_fd < 0)
        return false;

    // ============================================================
    // Step 2: Wait for the system (udev/libinput/X11/Wayland) to
    // detect and initialize the virtual keyboard device.
    // WITHOUT this delay, the system has no input device to read
    // from while physical keyboards are grabbed -> all keys dead!
    // ============================================================
    cout << "Waiting for system to detect virtual keyboard..." << endl;
    if (!wait_for_uinput_device()) {
        cerr << "WARNING: Virtual keyboard not detected by system after 2 seconds." << endl;
        cerr << "         Input may not work. Try running: sudo modprobe uinput" << endl;
        cerr << "         Continuing anyway..." << endl;
    }

    // Extra settling time for libinput/X11/Wayland to fully initialize
    cout << "Allowing system to settle (1s)..." << endl;
    linux_sleep_ms(1000);

    // ============================================================
    // Step 3: NOW grab physical keyboards
    // ============================================================
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        cerr << "ERROR: epoll_create1 failed: " << strerror(errno) << endl;
        return false;
    }

    DIR *dir = opendir("/dev/input");
    if (!dir) {
        cerr << "ERROR: Cannot open /dev/input directory" << endl;
        return false;
    }

    struct dirent *entry;
    int total_keyboards = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        string path = string("/dev/input/") + entry->d_name;
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) continue;

        if (is_keyboard_device(fd)) {
            // Skip our own uinput device (don't grab ourselves!)
            char name[256] = {0};
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            if (strstr(name, "Capsicain Virtual Keyboard") != nullptr) {
                close(fd);
                continue;
            }

            if (grab_device(fd)) {
                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.fd = fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
                    cerr << "WARNING: epoll_ctl failed for " << path << ": " << strerror(errno) << endl;
                    ioctl(fd, EVIOCGRAB, 0);
                    close(fd);
                    continue;
                }
                evdev_fds.push_back(fd);
                evdev_paths.push_back(path);
                total_keyboards++;
                cout << "Keyboard device grabbed: " << path << " (" << name << ")" << endl;
            } else {
                close(fd);
            }
        } else {
            close(fd);
        }
    }
    closedir(dir);

    if (total_keyboards == 0) {
        cerr << "ERROR: No keyboard devices found. Run with sudo or check /dev/input/ permissions." << endl;
        return false;
    }

    cout << "Total keyboards grabbed: " << total_keyboards << endl;
    return true;
}

VKeyEvent linux_input_receive()
{
    VKeyEvent result = {0, false};
    struct input_event ev;

    while (true) {
        struct epoll_event events[MAX_EVDEV_DEVICES];
        int nfds = epoll_wait(epoll_fd, events, MAX_EVDEV_DEVICES, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            cerr << "ERROR: epoll_wait failed: " << strerror(errno) << endl;
            return result;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            while (true) {
                ssize_t n = read(fd, &ev, sizeof(ev));
                if (n < (ssize_t)sizeof(ev)) {
                    if (n < 0 && errno == EAGAIN) break;
                    break;
                }

                // We only care about EV_KEY events (not EV_SYN, EV_MSC, etc.)
                if (ev.type == EV_KEY) {
                    int linux_kc = ev.code;
                    int ps2_sc = linuxToPs2Keycode(linux_kc);

                    // ev.value: 0=release, 1=press, 2=autorepeat
                    // Treat autorepeat same as press (like Windows Interception does)
                    bool is_down = (ev.value != 0);

                    if (ps2_sc >= 0) {
                        result.vcode = ps2_sc;
                        result.isDownstroke = is_down;
                        return result;
                    }
                    if (linux_kc < 256) {
                        result.vcode = linux_kc;
                        result.isDownstroke = is_down;
                        return result;
                    }
                }
            }
        }
    }
}

void linux_input_send(int ps2_scancode, bool is_down)
{
    if (uinput_fd < 0) return;
    if (ps2_scancode <= 0 || ps2_scancode > 255) return;

    int linux_kc = ps2ToLinuxKeycode(ps2_scancode);
    if (linux_kc < 0) {
        if (ps2_scancode < KEY_MAX) {
            linux_kc = ps2_scancode;
        } else {
            return;
        }
    }

    struct input_event ev;
    memset(&ev, 0, sizeof(ev));

    ev.type = EV_KEY;
    ev.code = linux_kc;
    ev.value = is_down ? 1 : 0;

    // Retry writes in case of EAGAIN (uinput fd is non-blocking)
    for (int retry = 0; retry < 3; retry++) {
        ssize_t n = write(uinput_fd, &ev, sizeof(ev));
        if (n == sizeof(ev)) break;
        if (n < 0 && errno == EAGAIN) {
            linux_sleep_ms(1);
            continue;
        }
        if (n < 0) {
            cerr << "ERROR: uinput write failed: " << strerror(errno) << endl;
            return;
        }
    }

    // Send EV_SYN to flush
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    for (int retry = 0; retry < 3; retry++) {
        ssize_t n = write(uinput_fd, &ev, sizeof(ev));
        if (n == sizeof(ev)) break;
        if (n < 0 && errno == EAGAIN) {
            linux_sleep_ms(1);
            continue;
        }
        if (n < 0) {
            cerr << "ERROR: uinput syn write failed: " << strerror(errno) << endl;
            return;
        }
    }
}

void linux_input_cleanup()
{
    if (uinput_fd >= 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        uinput_fd = -1;
    }

    for (int fd : evdev_fds) {
        ioctl(fd, EVIOCGRAB, 0);  // Release grab
        if (epoll_fd >= 0) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        }
        close(fd);
    }
    evdev_fds.clear();
    evdev_paths.clear();

    if (epoll_fd >= 0) {
        close(epoll_fd);
        epoll_fd = -1;
    }
}

int linux_daemonize()
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    if (setsid() < 0) return -1;

    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    umask(0);

    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        if (null_fd > STDIN_FILENO) close(null_fd);
    }

    int log_fd = open("/tmp/capsicain.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd >= 0) {
        time_t now = time(nullptr);
        string ts = ctime(&now);
        if (!ts.empty() && ts.back() == '\n') ts.pop_back();
        string header = "\n=== capsicain daemon started at " + ts + " (PID " + to_string(getpid()) + ") ===\n";
        (void)!write(log_fd, header.c_str(), header.length());

        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        if (log_fd > STDERR_FILENO) close(log_fd);
    }

    return 0;
}

void linux_sleep_ms(int ms)
{
    this_thread::sleep_for(chrono::milliseconds(ms));
}

bool linux_is_foreground()
{
    // On headless Linux, ESC commands always work
    return true;
}