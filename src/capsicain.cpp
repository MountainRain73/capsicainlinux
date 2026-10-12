#include "pch.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <algorithm>
#include <string>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>

#include "capsicain.h"
#include "constants.h"
#include "modifiers.h"
#include "scancodes.h"

using namespace std;

string PRETTY_VK_LABELS[MAX_VCODES];

vector<string> sanitizedIniContent;

struct Globals
{
    string iniVersion = "unnamed version - add 'iniVersion xyz123' to capsicain.ini";
    int activeConfigOnStartup = DEFAULT_ACTIVE_CONFIG;
    bool startMinimized = false;
    bool startInTraybar = false;
    bool startAHK = false;
    int capsicainOnOffKey = -1;
    bool protectConsole = true;
    bool translateMessyKeys = true;
    bool deactivateWinkeyStartmenu = false;
} globals;
static const struct Globals defaultGlobals;

struct Options
{
    bool debug = false;
    int delayForKeySequenceMS = DEFAULT_DELAY_FOR_KEY_SEQUENCE_MS;
    bool flipZy = false;
    bool flipAltWinOnAppleKeyboards = false;
    bool LControlLWinBlocksAlphaMapping = false;
    bool processOnlyFirstKeyboard = false;
} options;
static const struct Options defaultOptions;

struct ModifierCombo
{
    int vkey = SC_NOP;
    unsigned char deadkey = 0;
    unsigned short modAnd = 0;
    unsigned short modOr = 0;
    unsigned short modNot = 0;
    unsigned short modTap = 0;
    vector<VKeyEvent> keyEventSequence;
};

struct AllMaps
{
    int rewiremap[REWIRE_ROWS][REWIRE_COLS] = { };

    vector<ModifierCombo> modCombos;

    int alphamap[MAX_VCODES] = { };
} allMaps;

struct LinuxInputState
{
    VKeyEvent currentKeyEvent = { SC_NOP, 0 };
    VKeyEvent previousKeyEvent1 = { SC_NOP, 0 };
    VKeyEvent previousKeyEvent2 = { SC_NOP, 0 };
    string deviceIdKeyboard = "";
} linuxInputState;

struct GlobalState
{
    bool capsicainOn = true;

    int  activeConfig = 0;
    string activeConfigName = DEFAULT_ACTIVE_CONFIG_NAME;
    int previousConfig = 1;

    bool realEscapeIsDown = false;

    string deviceIdKeyboard = "";
    string includeDeviceId = "";
    string excludeDeviceId = "";

    bool deviceIsAppleKeyboard = false;

    int keysDownSentCounter = 0;
    bool keysDownSent[256] = { false };
    bool keysDownTempReleased[256] = { false };
    VKeyEvent lastSentKeyEvent = { SC_NOP, 0 };

    bool secretSequenceRecording = false;
    bool secretSequencePlayback = false;
    int recordingMacro = -1;
    vector<VKeyEvent> recordedMacros[MAX_NUM_MACROS];

    bool altTabPassthrough = false;
    unsigned char altTabExitScancode = 0;

    bool capsLockLedState = false;
    bool numLockLedState = false;
    bool scrollLockLedState = false;
} globalState;
static const struct GlobalState defaultGlobalState;

struct ModifierState
{
    unsigned char activeDeadkey = 0;
    unsigned short modifierDown = 0;
    unsigned short modifierTapped = 0;
    vector<VKeyEvent> modsTempAltered;
    int tapAndHoldKey = -1;
} modifierState;
static const struct ModifierState defaultModifierState;

struct LoopState
{
    unsigned char scancode = SC_NOP;
    int vcode = -1;
    bool isDownstroke = false;
    bool isModifier = false;
    bool tapped = false;
    bool tappedSlow = false;
    bool tapHoldMake = false;

    vector<VKeyEvent> resultingVKeyEventSequence;

} loopState;
static const struct LoopState defaultLoopState;

struct ProfilingTimer
{
    chrono::steady_clock::time_point timepointStopwatch;
    chrono::steady_clock::time_point timepointPreviousKeyEvent;
    chrono::steady_clock::time_point timepointLoopStart = std::chrono::steady_clock::now();

    int countIncoming = 0;
    int countOutgoing = 0;
    unsigned long totalMappingTimeUS = 0;
    unsigned long totalSendingTimeUS = 0;
    unsigned long worstMappingTimeUS = 0;
    unsigned long worstSendingTimeUS = 0;

    chrono::steady_clock::time_point getTimepointNow()
    {
        return std::chrono::steady_clock::now();
    }
    unsigned long stopwatchRestart()
    {
        unsigned long dura = stopwatchReadUS();
        timepointStopwatch = std::chrono::steady_clock::now();
        return dura;
    }
    unsigned long stopwatchReadUS()
    {
        return (unsigned long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - timepointStopwatch).count();
    }
} profiler;
static const struct ProfilingTimer defaultProfiler;

string errorLog = "";
void error(string txt)
{
    cout << endl << "ERROR: " << txt << endl;
    errorLog += "\r\n" + txt;
}

string getTimestamp()
{
    auto start = std::chrono::system_clock::now();
    auto legacyStart = std::chrono::system_clock::to_time_t(start);
    char tmBuff[30];
    ctime_r(&legacyStart, tmBuff);
    string s(tmBuff);
    if (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

string getPrettyVKLabelPadded(int vcode, int resultLength)
{
    string label = PRETTY_VK_LABELS[vcode];
    if (resultLength > (int)label.size())
        label.insert(0, resultLength - label.size(), ' ');
    return label;
}
string getPrettyVKLabel(int vcode)
{
    return PRETTY_VK_LABELS[vcode];
}

void InterceptionSendCurrentKeystroke()
{
    linux_input_send(linuxInputState.currentKeyEvent.vcode, linuxInputState.currentKeyEvent.isDownstroke);
}

bool linux_get_key_state(int ps2_scancode)
{
    switch (ps2_scancode) {
        case SC_CAPS: return globalState.capsLockLedState;
        case SC_NUMLOCK: return globalState.numLockLedState;
        case SC_SCRLOCK: return globalState.scrollLockLedState;
        default: return false;
    }
}

static volatile sig_atomic_t g_running = 1;
static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static int g_pid_fd = -1;

static bool create_pid_file() {
    g_pid_fd = open("/tmp/capsicain.pid", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_pid_fd < 0) return false;
    return true;
}

static void write_pid() {
    if (g_pid_fd >= 0) {
        string pid = to_string(getpid()) + "\n";
        (void)!write(g_pid_fd, pid.c_str(), pid.length());
    }
}

static void remove_pid_file() {
    if (g_pid_fd >= 0) {
        close(g_pid_fd);
        g_pid_fd = -1;
    }
    unlink("/tmp/capsicain.pid");
}

static bool already_running() {
    int fd = open("/tmp/capsicain.pid", O_RDONLY);
    if (fd < 0) return false;
    char buf[32] = {0};
    (void)!read(fd, buf, sizeof(buf) - 1);
    close(fd);
    int pid = atoi(buf);
    if (pid > 0 && kill(pid, 0) == 0) return true;
    return false;
}

std::string g_config_path;

int main(int argc, char* argv[])
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    g_config_path = "";
    bool daemon_mode = false;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "-d" || arg == "--daemon") {
            daemon_mode = true;
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                g_config_path = argv[++i];
            } else {
                cout << "Error: -c/--config requires a file path" << endl;
                return 1;
            }
        } else if (arg == "--kill") {
            int fd = open("/tmp/capsicain.pid", O_RDONLY);
            if (fd >= 0) {
                char buf[32] = {0};
                (void)!read(fd, buf, sizeof(buf) - 1);
                close(fd);
                int pid = atoi(buf);
                if (pid > 0 && kill(pid, SIGTERM) == 0) {
                    cout << "Sent SIGTERM to capsicain (PID " << pid << ")" << endl;
                } else {
                    cout << "Capsicain is not running (stale PID file)" << endl;
                    unlink("/tmp/capsicain.pid");
                }
            } else {
                cout << "Capsicain is not running (no PID file)" << endl;
            }
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            cout << "Usage: capsicain [-d|--daemon] [-c|--config PATH] [--kill] [--help]" << endl;
            cout << "  -d, --daemon    Run in background as a daemon" << endl;
            cout << "  -c, --config    Specify path to capsicain.ini (default: ./capsicain.ini)" << endl;
            cout << "  --kill          Stop a running capsicain daemon" << endl;
            cout << "  -h, --help      Show this help" << endl;
            return 0;
        } else {
            cout << "Unknown option: " << arg << endl;
            cout << "Usage: capsicain [-d|--daemon] [--kill] [--help]" << endl;
            return 1;
        }
    }

    if (already_running()) {
        cout << endl << "Capsicain already running - exiting..." << endl;
        cout << "Use 'capsicain --kill' to stop the running instance." << endl;
        linux_sleep_ms(5000);
        return 0;
    }

    if (daemon_mode) {
        cout << "Daemonizing capsicain..." << endl;
        if (linux_daemonize() < 0) {
            cerr << "Failed to daemonize!" << endl;
            return 1;
        }
    }

    if (!create_pid_file()) {
        if (!daemon_mode) {
            cout << "WARNING: Cannot create PID file, but continuing..." << endl;
        }
    }
    write_pid();

    IFPROF profiler.stopwatchRestart();

    printHelloHeader();

    defineAllPrettyVKLabels(PRETTY_VK_LABELS);

    if (!readSanitizeIniFile(sanitizedIniContent))
    {
        cout << endl << "No capsicain.ini - exiting..." << endl;
        remove_pid_file();
        return 0;
    }

    parseIniGlobals();
    switchConfig(globals.activeConfigOnStartup, true);

    cout << endl << endl << "[ESC] + [X] to stop." << endl << "[ESC] + [H] for Help";
    cout << endl << endl << "capsicain running.... " << endl;

    IFPROF cout << endl << endl << "Profiling enabled in this build" << endl << "Startup time: " << profiler.stopwatchReadUS() / 1000 << " ms" << endl;

    if (!linux_input_init())
    {
        cout << endl << "Failed to initialize Linux input subsystem. Exiting..." << endl;
        cout << "Make sure you have root privileges and uinput module is loaded." << endl;
        cout << "Try: sudo modprobe uinput" << endl;
        linux_sleep_ms(5000);
        remove_pid_file();
        return 1;
    }

    cout << endl << "Capsicain is now intercepting keyboard input." << endl;
    cout << "Press ESC+X to exit." << endl;

    while (g_running)
    {
        linuxInputState.previousKeyEvent2 = linuxInputState.previousKeyEvent1;
        linuxInputState.previousKeyEvent1 = linuxInputState.currentKeyEvent;

        linuxInputState.currentKeyEvent = linux_input_receive();

        IFPROF
        {
            profiler.timepointPreviousKeyEvent = profiler.timepointLoopStart;
            profiler.timepointLoopStart = profiler.getTimepointNow();
            profiler.stopwatchRestart();
            profiler.countIncoming++;
        }

        IFTRACE cout << endl << "IKS: " << hex << linuxInputState.currentKeyEvent.vcode
             << " = " << getPrettyVKLabel(linuxInputState.currentKeyEvent.vcode)
             << (linuxInputState.currentKeyEvent.isDownstroke ? "v" : "^");

        loopState = defaultLoopState;

        VKeyEvent originalVKeyEvent = linuxInputState.currentKeyEvent;
        loopState.scancode = originalVKeyEvent.vcode;
        loopState.vcode = loopState.scancode;
        loopState.isDownstroke = originalVKeyEvent.isDownstroke;

        if (globals.capsicainOnOffKey != -1)
        {
            if (processOnOffKey())
                continue;
        }

        if (!globalState.capsicainOn)
        {
            InterceptionSendCurrentKeystroke();
            continue;
        }

        if (globalState.altTabPassthrough)
        {
            if (loopState.scancode == globalState.altTabExitScancode && !loopState.isDownstroke)
            {
                InterceptionSendCurrentKeystroke();
                globalState.altTabPassthrough = false;
                globalState.altTabExitScancode = 0;
                reset();
                IFDEBUG cout << endl << "(ALTTAB passthrough END)";
                continue;
            }
            InterceptionSendCurrentKeystroke();
            continue;
        }

        IFDEBUG if(globalState.activeConfig == 0) cout << ". ";

        if (linuxInputState.currentKeyEvent.vcode == 0)
        {
            continue;
        }

        if (loopState.scancode == SC_ESCAPE)
        {
            IFDEBUG cout << endl << "(Hard ESC" << (loopState.isDownstroke ? "v " : "^ ") << ")";
            globalState.realEscapeIsDown = loopState.isDownstroke;

            if (globalState.recordingMacro > 0)
            {
                IFDEBUG cout << endl << "Stop recording macro #" << globalState.recordingMacro;
                if (globalState.recordedMacros[globalState.recordingMacro].size() > 0)
                    globalState.secretSequenceRecording = false;
                {
                    globalState.recordedMacros[globalState.recordingMacro].push_back({ VK_CPS_TEMPRESTOREKEYS,true });
                    globalState.recordedMacros[globalState.recordingMacro].insert(globalState.recordedMacros[globalState.recordingMacro].begin(), { VK_CPS_TEMPRELEASEKEYS,true });
                }
                globalState.recordingMacro = -1;
                continue;
            }
        }
        else if (globalState.realEscapeIsDown && loopState.isDownstroke)
        {
            if (processCommand())
                continue;
            else
            {
                break;
            }
        }

        if (globalState.activeConfig == DISABLED_CONFIG_NUMBER)
        {
            InterceptionSendCurrentKeystroke();
            continue;
        }

        if (!processMessyKeys())
            continue;

        detectTapping();
        if (loopState.tappedSlow)
            modifierState.modifierTapped = 0;

        processRewireScancodeToVirtualcode();
        if (loopState.vcode == SC_NOP)
        {
            IFDEBUG cout << " (r2NOP)";
            continue;
        }

        IFDEBUG
        {
            cout << endl;
            IFPROF cout << "(" << setw(5) << dec << timeBetweenTimepointsUS(profiler.timepointPreviousKeyEvent, profiler.timepointLoopStart) / 1000 << " m) ";
            cout
                << " ["
                << hex << setw(2) << linuxInputState.currentKeyEvent.vcode
                << "= " << setw(8) << (loopState.vcode == loopState.scancode ? "" : PRETTY_VK_LABELS[loopState.scancode] + " > ")
                << setw(8) << getPrettyVKLabel(loopState.vcode) << setw(2) << (loopState.isDownstroke ? "v" : "^")
                << "] ";
        }

        processModifierState();

        IFDEBUG
        {
            string mdown = modifierState.modifierDown > 0 ? stringIntToHex(modifierState.modifierDown,0) : "";
            string mtapp = modifierState.modifierTapped > 0 ? stringIntToHex(modifierState.modifierTapped,0) : "";
            cout << "[M:" << setw(4) << mdown
                 << " T:" << setw(4) << mtapp
                 << " D:" << setw(6) << (modifierState.activeDeadkey > 0 ? getPrettyVKLabel(modifierState.activeDeadkey): "")
                 << "] ";
        }

        processCombos();

        processMapAlphaKeys();

        if (!isModifier(loopState.vcode))
            modifierState.modifierTapped = 0;

        IFPROF
        {
            unsigned long mappingtime = profiler.stopwatchRestart();
            profiler.totalMappingTimeUS += mappingtime;
            profiler.countOutgoing++;
            if (mappingtime > profiler.worstMappingTimeUS)
                profiler.worstMappingTimeUS = mappingtime;
            IFDEBUG cout << "  (" << setw(5) << dec << mappingtime << " u)";
        }

        sendResultingKeyOrSequence();
        IFPROF
        {
            unsigned long sendingtime = profiler.stopwatchReadUS();
            profiler.totalSendingTimeUS += sendingtime;
            if (sendingtime > profiler.worstSendingTimeUS)
                profiler.worstSendingTimeUS = sendingtime;
            if (sendingtime > 1000)
                cout << "\t (slow send: " << dec << sendingtime << " u)";
        }

        IFDEBUG
        {
            cout << (loopState.tappedSlow ? " (tap slow)" : "");
            cout << (loopState.tapped ? " (tap)" : "");
            if (modifierState.tapAndHoldKey >= 0)
                cout << " (TapHoldKey: " << hex << modifierState.tapAndHoldKey << ")";
        }
    }

    reset();
    releaseAllSentKeys();
    linux_input_cleanup();
    remove_pid_file();

    cout << endl << "bye" << endl;
    return 0;
}

bool processOnOffKey()
{
    bool pauseKeyTriggeredOnOff = false;

    if (loopState.scancode == globals.capsicainOnOffKey || pauseKeyTriggeredOnOff)
    {
        if (loopState.isDownstroke)
        {
            globalState.capsicainOn = !globalState.capsicainOn;
            if (globalState.capsicainOn)
            {
                reset();
                cout << endl << endl << "[" << getPrettyVKLabel(globals.capsicainOnOffKey) << "] -> Capsicain ON";
                cout << endl << "active config: " << globalState.activeConfig << " = " << globalState.activeConfigName;
            }
            else
                cout << endl << endl << "[" << getPrettyVKLabel(globals.capsicainOnOffKey) << "] -> Capsicain OFF";
        }
        return true;
    }

    return false;
}

bool processMessyKeys()
{
    if (loopState.vcode == SC_ALTPRINT)
    {
        IFTRACE cout << endl << "ALTPRINT";
        if (globals.translateMessyKeys)
            loopState.vcode = SC_PRINT;
    }

    return true;
}

void detectTapping()
{
    loopState.tapped =
        !loopState.isDownstroke
        && (linuxInputState.currentKeyEvent.vcode == linuxInputState.previousKeyEvent1.vcode)
        && (linuxInputState.previousKeyEvent1.isDownstroke);

    loopState.tappedSlow =
        loopState.tapped
        && (linuxInputState.previousKeyEvent2.vcode == linuxInputState.currentKeyEvent.vcode)
        && (linuxInputState.previousKeyEvent2.isDownstroke);

    if (loopState.tappedSlow)
        loopState.tapped = false;

    if (linuxInputState.previousKeyEvent1.vcode == linuxInputState.currentKeyEvent.vcode
        && linuxInputState.previousKeyEvent2.vcode == linuxInputState.currentKeyEvent.vcode
        && (linuxInputState.currentKeyEvent.isDownstroke)
        && (!linuxInputState.previousKeyEvent1.isDownstroke)
        && (linuxInputState.previousKeyEvent2.isDownstroke)
        )
    {
        loopState.tapHoldMake = true;
    }
}

void processModifierState()
{
    unsigned short modBitmask = getModifierBitmaskForVcode(loopState.vcode);

    if (loopState.isDownstroke)
        modifierState.modifierDown |= modBitmask;
    else
        modifierState.modifierDown &= ~modBitmask;

    if (loopState.tapped)
        modifierState.modifierTapped |= modBitmask;
}

void processRewireScancodeToVirtualcode()
{
    if (loopState.scancode == modifierState.tapAndHoldKey && loopState.isDownstroke)
    {
        loopState.vcode = SC_NOP;
        return;
    }

    int rewoutkey = allMaps.rewiremap[loopState.vcode][REWIRE_OUT];
    if (rewoutkey >= 0)
    {
        loopState.vcode = rewoutkey;

        int rewtapkey = allMaps.rewiremap[loopState.scancode][REWIRE_TAP];
        if (loopState.tapped && rewtapkey >= 0)
        {
            modifierState.modifierTapped = 0;

            loopState.resultingVKeyEventSequence.push_back({ rewoutkey, false });
            if (isModifier(loopState.vcode))
            {
                unsigned short modBitmask = getModifierBitmaskForVcode(loopState.vcode);
                if (modBitmask != 0)
                    modifierState.modifierDown &= ~modBitmask;
            }
            loopState.vcode = rewtapkey;
            loopState.resultingVKeyEventSequence.push_back({ rewtapkey, true });
            loopState.resultingVKeyEventSequence.push_back({ rewtapkey, false });
        }

        if (loopState.tapHoldMake)
        {
            int rewtapholdkey = allMaps.rewiremap[loopState.scancode][REWIRE_TAPHOLD];
            if (rewtapholdkey >= 0)
            {
                if (modifierState.tapAndHoldKey < 0)
                {
                    modifierState.tapAndHoldKey = loopState.scancode;
                    if(rewtapholdkey <= 255)
                        loopState.resultingVKeyEventSequence.push_back({ rewtapholdkey, true });
                    loopState.vcode = rewtapholdkey;

                    int rewtappedkey = allMaps.rewiremap[loopState.scancode][REWIRE_TAP];
                    unsigned short modBitmask1 = getModifierBitmaskForVcode(rewoutkey);
                    if (modBitmask1 != 0)
                        modifierState.modifierTapped &= ~modBitmask1;
                    unsigned short modBitmask2 = getModifierBitmaskForVcode(rewtappedkey);
                    if (modBitmask2 != 0)
                        modifierState.modifierTapped &= ~modBitmask2;

                    IFTRACE cout << endl << "Make taphold rewired: " << hex << rewtapholdkey;
                }
                else
                    error("Ignoring second tap-and-hold event; only one can be active.");
            }
        }
        if (!loopState.isDownstroke && loopState.scancode == modifierState.tapAndHoldKey)
        {
            int rewtapholdkey = allMaps.rewiremap[loopState.scancode][REWIRE_TAPHOLD];
            if (rewtapholdkey >= 0)
            {
                modifierState.tapAndHoldKey = -1;
                if (rewtapholdkey < 255)
                    loopState.resultingVKeyEventSequence.push_back({ rewtapholdkey, false });
                else
                    loopState.vcode = SC_NOP;
                loopState.vcode = rewtapholdkey;
                IFTRACE cout << endl << "Break taphold rewired: " << hex << rewtapholdkey;
            }
        }
    }

    loopState.isModifier = isModifier(loopState.vcode) ? true : false;
}

void processCombos()
{
    VKeyEvent break_tapped_modifier = { SC_NOP, 0 };
    if (!loopState.isDownstroke)
    {
        if (loopState.tapped && loopState.resultingVKeyEventSequence.size() > 0)
        {
            break_tapped_modifier = loopState.resultingVKeyEventSequence.at(0);
        }
        else
            return;
    }

    for (ModifierCombo modcombo : allMaps.modCombos)
    {
        if (modcombo.vkey == loopState.vcode)
        {
            if (
                (modifierState.activeDeadkey == modcombo.deadkey) &&
                (modifierState.modifierDown & modcombo.modAnd) == modcombo.modAnd &&
                (modcombo.modOr == 0 || (modifierState.modifierDown & modcombo.modOr) > 0) &&
                (modifierState.modifierDown & modcombo.modNot) == 0 &&
                ((modifierState.modifierTapped & modcombo.modTap) == modcombo.modTap)
                )
            {
                loopState.resultingVKeyEventSequence = modcombo.keyEventSequence;
                if (break_tapped_modifier.vcode != SC_NOP)
                {
                    loopState.resultingVKeyEventSequence.push_back(break_tapped_modifier);
                }
                modifierState.modifierTapped = 0;
                break;
            }
        }
    }

    if(!loopState.isModifier)
        modifierState.activeDeadkey = 0;
}

void processMapAlphaKeys()
{
    if (loopState.isModifier ||
        (options.LControlLWinBlocksAlphaMapping && (IS_LCTRL_DOWN || IS_LWIN_DOWN)))
    {
        return;
    }

    loopState.vcode = allMaps.alphamap[loopState.vcode];

    if (options.flipZy)
    {
        switch (loopState.vcode)
        {
        case SC_Y:      loopState.vcode = SC_Z;     break;
        case SC_Z:      loopState.vcode = SC_Y;     break;
        }
    }
}

bool processCommand()
{
    bool continueLooping = true;
    cout << endl << endl << "::";

    switch (loopState.scancode)
    {
    case SC_X:
        cout << endl << endl << "ESC+X :: EXIT";
        return false;
    case SC_0:
        cout << endl << "CONFIG CHANGE: " << DISABLED_CONFIG_NUMBER;
        switchConfig(DISABLED_CONFIG_NUMBER, false);
        break;
    case SC_1:
    case SC_2:
    case SC_3:
    case SC_4:
    case SC_5:
    case SC_6:
    case SC_7:
    case SC_8:
    case SC_9:
    {
        int config = loopState.scancode - 1;
        cout << endl << "CONFIG CHANGE: " << config;
        switchConfig(config, false);
        break;
    }
    case SC_BACK:
        cout << endl << endl << "::RESET STATE";
        reset();
        resetCapsNumScrollLock();
        break;
    case SC_W:
        options.flipAltWinOnAppleKeyboards = !options.flipAltWinOnAppleKeyboards;
        cout << "Flip ALT<>WIN for Apple boards: " << (options.flipAltWinOnAppleKeyboards ? "ON" : "OFF") << endl;
        break;
    case SC_E:
        cout << "ERROR LOG: " << endl << errorLog << endl;
        break;
    case SC_R:
        cout << "RELOAD INI";
        reload();
        cout << endl << (globalState.deviceIsAppleKeyboard ? "APPLE keyboard (flipping Win<>Alt)" : "PC keyboard");
        break;
    case SC_I:
    {
        cout << "INI filtered for config " << globalState.activeConfigName;
        vector<string> tmpAssembledConfig = assembleConfig(globalState.activeConfig);
        for (string line : tmpAssembledConfig)
            cout << endl << line;
        break;
    }
    case SC_S:
        printStatus();
        break;
    case SC_D:
        options.debug = !options.debug;
        cout << "DEBUG mode: " << (options.debug ? "ON" : "OFF");
        break;
    case SC_H:
        printHelp();
        break;
    case SC_J:
        cout << "MACRO 0 START RECORDING";
        globalState.recordingMacro = 0;
        globalState.recordedMacros[0].clear();
        break;
    case SC_K:
        if (globalState.recordingMacro == 0)
        {
            while (globalState.recordedMacros[0].size() > 0 && globalState.recordedMacros[0].back().isDownstroke)
                globalState.recordedMacros[0].pop_back();
            while (globalState.recordedMacros[0].size()>0 && !globalState.recordedMacros[0].front().isDownstroke)
                globalState.recordedMacros[0].erase(globalState.recordedMacros[0].begin());
            cout << "MACRO 0 STOP RECORDING (" << globalState.recordedMacros[0].size() << ")";
        }
        else
            cout << "MACRO 0 RECORDING ALREADY STOPPED";

        globalState.recordingMacro = -1;
        break;
    case SC_L:
        cout << "MACRO 0 PLAYBACK";
        playKeyEventSequence(globalState.recordedMacros[0]);
        break;
    case SC_SEMI:
    {
        cout << "COPY MACRO 0 TO CLIPBOARD";
        string macro = "";
        for (VKeyEvent key : globalState.recordedMacros[0])
        {
            if (macro.size() > 0)
                macro += "_";
            if (key.isDownstroke)
                macro += "&";
            else
                macro += "^";
            macro += PRETTY_VK_LABELS[key.vcode];
        }
        cout << endl << "Macro definition (manual copy):" << endl << macro;
        break;
    }
    case SC_Z:
        options.flipZy = !options.flipZy;
        cout << "Flip Z<>Y mode: " << (options.flipZy ? "ON" : "OFF");
        break;
    case SC_C:
        cout << "List of all Key Labels for scancodes" << endl
             << "------------------------------------" << endl;
        printKeylabels();
        break;
    case SC_COMMA:
        if (options.delayForKeySequenceMS >= 1)
            options.delayForKeySequenceMS -= 1;
        cout << "delay between characters in key sequences (ms): " << dec << options.delayForKeySequenceMS;
        break;
    case SC_DOT:
        if (options.delayForKeySequenceMS <= 100)
            options.delayForKeySequenceMS += 1;
        cout << "delay between characters in key sequences (ms): " << dec << options.delayForKeySequenceMS;
        break;
    default:
    {
        cout << "Unknown command";
        break;
    }
    }

    return continueLooping;
}

void getHardwareId()
{
    // Simplified for Linux - just set a generic ID
    globalState.deviceIdKeyboard = "linux_keyboard";
    globalState.deviceIsAppleKeyboard = false;
}

void parseIniGlobals()
{
    vector<string> sectLines = getTaggedLinesFromIni(INI_TAG_GLOBAL, sanitizedIniContent);

    for (string line : sectLines)
    {
        string token = stringCopyFirstToken(line);
        if (token == "debugonstartup")
            options.debug = true;
        else if (token == "capsicainonoffkey")
        {
            string s = stringGetRestBehindFirstToken(line);
            int key = getVcode(s, PRETTY_VK_LABELS);
            if (key < 0)
                cout << "ERROR: unknown key label: " << line << endl;
            else if (key > 255 && key != VK_CPS_PAUSE)
                cout << "ERROR: virtual key makes no sense: " << line << endl;
            else
                globals.capsicainOnOffKey = key;
        }
        else if (token == "iniversion")
            globals.iniVersion = stringGetRestBehindFirstToken(line);
        else if (token == "startminimized")
            globals.startMinimized = true;
        else if (token == "startintraybar")
            globals.startInTraybar = true;
        else if (token == "startahk")
            globals.startAHK = true;
        else if (token == "donttranslatemessykeys")
            globals.translateMessyKeys = false;
        else if (token == "dontprotectconsole")
            globals.protectConsole = false;
        else if (token == "deactivatewinkeystartmenu")
            globals.deactivateWinkeyStartmenu = true;
        else if ((token == "activeconfigonstartup") || (token == "activelayeronstartup"))
            cout << endl;
        else
            cout << endl << "WARNING: unknown GLOBAL " << token;
    }

    if (!getIntValueForTaggedKey(INI_TAG_GLOBAL, "ActiveConfigOnStartup", globals.activeConfigOnStartup, sanitizedIniContent))
    {
        if (getIntValueForTaggedKey(INI_TAG_GLOBAL, "ActiveLayerOnStartup", globals.activeConfigOnStartup, sanitizedIniContent))
        {
            cout << endl << "INFO: Use 'GLOBAL activeConfigOnStartup' instead of 'GLOBAL activeLayerOnStartup'";
        }
        else
        {
            cout << endl << "No ini setting for 'GLOBAL activeConfigOnStartup'. Setting default config " << globals.activeConfigOnStartup;
        }
    }
}

bool parseIniOptions(std::vector<std::string> assembledIni)
{
    vector<string> sectLines = getTaggedLinesFromIni(INI_TAG_OPTIONS, assembledIni);
    globalState.activeConfigName = INI_TAG_OPTIONS " configName is undefined";

    for (string line : sectLines)
    {
        string token = stringCopyFirstToken(line);
        if (token == "configname")
        {
            globalState.activeConfigName = stringGetRestBehindFirstToken(line);
        }
        else if (token == "layername")
        {
            globalState.activeConfigName = stringGetRestBehindFirstToken(line);
            cout << endl << "INFO: Option LAYERname is deprecated. Use Option CONFIGname instead.";
        }
        else if (token == "debug")
        {
            options.debug = true;
        }
        else if (token == "flipzy")
        {
            options.flipZy = true;
        }
        else if (token == "altalttoalt")
        {
            cout << endl << INI_TAG_OPTIONS " AltAltToAlt is obsolete. You can do this now with 'REWIRE LALT MOD12 // LALT'";
        }
        else if (token == "flipaltwinonapplekeyboards")
        {
            options.flipAltWinOnAppleKeyboards = true;
        }
        else if (token == "lcontrollwinblocksalphamapping")
        {
            options.LControlLWinBlocksAlphaMapping = true;
        }
        else if (token == "processonlyfirstkeyboard")
        {
            options.processOnlyFirstKeyboard = true;
        }
        else if (token == "includedeviceid")
        {
            globalState.includeDeviceId = stringGetRestBehindFirstToken(line);
            cout << endl << "INFO: this config is active for devices whose ID contains '" << globalState.includeDeviceId << "'";
        }
        else if (token == "excludedeviceid")
        {
            globalState.excludeDeviceId = stringGetRestBehindFirstToken(line);
            cout << endl << "INFO: this config is active for devices whose ID does NOT contain '" << globalState.excludeDeviceId << "'";
        }
        else if (token == "delayforkeysequencems")
        {
            getIntValueForKey("delayForKeySequenceMS", options.delayForKeySequenceMS, sectLines);
        }
        else if (token == "shiftshifttoshiftlock")
        {
            cout << endl << ("WARNING: this is obsolete: OPTION shiftShiftToShiftLock");
            cout << endl << "  Put this into your .ini instead: "
                << endl << "    COMBO  LSHF   [& ....] > key(CAPSOFF)"
                << endl << "    COMBO  RSHF[.&] > key(CAPSON)" << endl;
        }
        else
        {
            cout << endl << "WARNING: ignoring unknown OPTION " << line << endl;
        }
    }

    return true;
}

void parseIniRewires(std::vector<std::string> assembledIni)
{
    vector<string> sectLines = getTaggedLinesFromIni(INI_TAG_REWIRE, assembledIni);

    int tagCounter = 0;
    int keyIn, keyOut, keyTap, keyTapHold;
    for (string line : sectLines)
    {
        keyTap = -1;
        keyTapHold = -1;
        if (parseKeywordRewire(line, keyIn, keyOut, keyTap, keyTapHold, PRETTY_VK_LABELS))
        {
            if (allMaps.rewiremap[keyIn][REWIRE_OUT] >= 0)
            {
                cout << endl << "WARNING: ignoring redefinition of " << INI_TAG_REWIRE << " "
                    << PRETTY_VK_LABELS[keyIn] << " " << PRETTY_VK_LABELS[keyOut] << " " << PRETTY_VK_LABELS[keyTap];
                continue;
            }

            if (!isModifier(keyOut) && keyTap > 0)
                cout << endl << "WARNING: 'If-Tapped' definition only makes sense for modifiers: " << INI_TAG_REWIRE << " " << line;

            tagCounter++;
            allMaps.rewiremap[keyIn][REWIRE_OUT] = keyOut;
            allMaps.rewiremap[keyIn][REWIRE_TAP] = keyTap;
            allMaps.rewiremap[keyIn][REWIRE_TAPHOLD] = keyTapHold;
        }
        else
            error("Bad Rewire / key mapping: " + line);
    }
    IFDEBUG cout << endl << "Rewire Definitions: " << dec << tagCounter;
}

bool parseIniCombos(std::vector<std::string> assembledIni)
{
    allMaps.modCombos.clear();
    vector<string> sectLines = getTaggedLinesFromIni(INI_TAG_COMBOS, assembledIni);
    if (sectLines.size() == 0)
        return false;

    unsigned short mods[5] = { 0 };
    vector<VKeyEvent> keyEventSequence;

    for (string line : sectLines)
    {
        int key;
        if (parseKeywordCombo(line, key, mods, keyEventSequence, PRETTY_VK_LABELS))
        {
            bool isDuplicate = false;
            for (ModifierCombo testcombo : allMaps.modCombos)
            {
                if (key == testcombo.vkey && mods[0] == testcombo.deadkey && mods[1] == testcombo.modAnd
                    && mods[2] == testcombo.modOr && mods[3] == testcombo.modNot && mods[4] == testcombo.modTap)
                {
                    bool redefined = false;
                    if (testcombo.keyEventSequence.size() == keyEventSequence.size())
                    {
                        for (int i = 0; i < (int)keyEventSequence.size(); i++)
                        {
                            if (keyEventSequence[i].vcode != testcombo.keyEventSequence[i].vcode
                                || keyEventSequence[i].isDownstroke != testcombo.keyEventSequence[i].isDownstroke)
                            {
                                redefined = true;
                                break;
                            }
                        }
                    }
                    else
                        redefined = true;

                    if(redefined)
                        cout << endl << "WARNING: Ignoring redefinition of Combo: " << line;

                    isDuplicate = true;
                    break;
                }
            }
            if(!isDuplicate)
                allMaps.modCombos.push_back({ key, (unsigned char) mods[0], mods[1], mods[2], mods[3], mods[4], keyEventSequence });
        }
        else
            error("Cannot parse combo rule: " + line);
    }
    return true;
}

bool parseIniAlphaLayout(std::vector<std::string> assembledIni)
{
    string tagFrom = stringToLower(INI_TAG_ALPHA_FROM);
    string tagEnd = stringToLower(INI_TAG_ALPHA_END);

    string mapFromTo = "";
    bool inMapFromTo = false;
    for (string line : assembledIni)
    {
        string firstToken = stringCopyFirstToken(line);
        if (firstToken == tagFrom)
        {
            if (inMapFromTo)
            {
                error(std::string("Bad ") + INI_TAG_ALPHA_FROM + ".." + INI_TAG_ALPHA_TO + "definition - received second "+ INI_TAG_ALPHA_FROM +". Forgot the "+INI_TAG_ALPHA_END+"?");
                return false;
            }
            inMapFromTo = true;
            mapFromTo = stringGetRestBehindFirstToken(line) + " ";
        }
        else if (firstToken == tagEnd)
        {
            inMapFromTo = false;
            if (!parseKeywordsAlpha_FromTo(mapFromTo, allMaps.alphamap, PRETTY_VK_LABELS))
                error(std::string("Cannot parse the ") + INI_TAG_ALPHA_FROM + ".." + INI_TAG_ALPHA_TO + " alpha definition");
        }
        else if (inMapFromTo)
        {
            mapFromTo += line + " ";
        }
    }
    return true;
}

std::vector<std::string> assembleConfig(int config)
{
    string sectionName = "config_" + to_string(config);
    vector<string> assembledIni = getSectionFromIni(sectionName, sanitizedIniContent);

    if (assembledIni.size() == 0)
    {
        sectionName = "layer_" + to_string(config);
        assembledIni = getSectionFromIni(sectionName, sanitizedIniContent);

        if (assembledIni.size() > 0)
            cout << endl << "INFO: section [layer_x]  should now be named  [config_x]";
    }

    while (true)
    {
        bool foundInclude = false;
        for (int i = 0; i < (int)assembledIni.size(); i++)
        {
            string line = assembledIni.at(i);
            if (stringStartsWith(line, "include "))
            {
                assembledIni.erase(assembledIni.begin() + i);
                string subSectionName = stringGetRestBehindFirstToken(line);
                vector<string> subsection = getSectionFromIni(subSectionName, sanitizedIniContent);
                if (subsection.size() == 0)
                {
                    error("Subsection [" + subSectionName + "] does not exist or is empty)");
                }
                else
                {
                    IFDEBUG cout << endl << "inserting sub-section: " + subSectionName << " (" << subsection.size() << " lines)";
                    assembledIni.insert(assembledIni.begin() + i, subsection.begin(), subsection.end());
                }
                foundInclude = true;
                break;
            }
        }
        if (!foundInclude)
            break;
    }

    return assembledIni;
}

void initializeAllMaps()
{
    allMaps.modCombos.clear();

    {
        for (int i = 0; i < MAX_VCODES; i++)
            allMaps.alphamap[i] = i;
    }

    {
        for (int r = 0; r < REWIRE_ROWS; r++)
            for (int c = 0; c < REWIRE_COLS; c++)
                allMaps.rewiremap[r][c] = -1;
    }
}

bool parseProcessIniConfig(int config)
{
    initializeAllMaps();

    if (sanitizedIniContent.size() == 0)
    {
        cout << endl << "Capsicain.ini is missing or empty.";
        return false;
    }

    vector<string> assembledConfig = assembleConfig(config);
    if (assembledConfig.size() == 0)
    {
        cout << endl << "No valid configuration for Config " << config;
        return false;
    }

    IFDEBUG cout << endl << "Assembled config #" << config << " : " << dec << assembledConfig.size() << " lines";

    parseIniOptions(assembledConfig);

    parseIniRewires(assembledConfig);

    parseIniCombos(assembledConfig);
    IFDEBUG cout << endl << "Combo  Definitions: " << dec << allMaps.modCombos.size();

    parseIniAlphaLayout(assembledConfig);
    IFDEBUG
    {
        int remapped = 0;
        for (int i = 0; i < MAX_VCODES; i++)
            if (i != allMaps.alphamap[i])
                remapped++;
        cout << endl << "Alpha  Definitions: " << dec << remapped;
    }

    return true;
}

void switchConfig(int config, bool forceReloadSameConfig)
{
    if (!forceReloadSameConfig && config == globalState.activeConfig)
        return;

    int oldConfig = globalState.activeConfig;
    reset();

    if (config == DISABLED_CONFIG_NUMBER)
    {
        globalState.activeConfig = DISABLED_CONFIG_NUMBER;
        globalState.activeConfigName = DISABLED_CONFIG_NAME;
    }
    else if (parseProcessIniConfig(config))
    {
        globalState.activeConfig = config;
        globalState.previousConfig = oldConfig;
        printOptions();
    }
    else if (parseProcessIniConfig(oldConfig))
    {
        cout << endl << endl << "Keeping the current config";
    }
    else
    {
        cout << endl << endl << "ERROR: CANNOT RELOAD CURRENT CONFIG? Switching to config 0";
        globalState.activeConfig = DISABLED_CONFIG_NUMBER;
        globalState.activeConfigName = DISABLED_CONFIG_NAME;
    }

    cout << endl << endl << "ACTIVE CONFIG: " << globalState.activeConfig << " = " << globalState.activeConfigName;
}

void resetCapsNumScrollLock()
{
    vector<VKeyEvent> sequence;

    // Set NumLock on by default, set CapsLock and ScrollLock off
    if (!globalState.numLockLedState)
        keySequenceAppendMakeBreakKey(SC_NUMLOCK, sequence);
    if (globalState.capsLockLedState)
        keySequenceAppendMakeBreakKey(SC_CAPS, sequence);
    if (globalState.scrollLockLedState && globals.capsicainOnOffKey != SC_SCRLOCK)
        keySequenceAppendMakeBreakKey(SC_SCRLOCK, sequence);
    if (sequence.size() != 0)
        playKeyEventSequence(sequence);
}

void reset()
{
    releaseAllSentKeys();

    loopState = defaultLoopState;
    modifierState = defaultModifierState;

    IFPROF
    {
        chrono::steady_clock::time_point tps = profiler.timepointStopwatch;
        chrono::steady_clock::time_point tppk = profiler.timepointPreviousKeyEvent;
        chrono::steady_clock::time_point tpls = profiler.timepointLoopStart;
        profiler = defaultProfiler;
        profiler.timepointStopwatch = tps;
        profiler.timepointPreviousKeyEvent = tppk;
        profiler.timepointLoopStart = tpls;
    }

    GlobalState tmp = globalState;
    globalState = defaultGlobalState;
    globalState.deviceIdKeyboard = tmp.deviceIdKeyboard;
    globalState.activeConfig = tmp.activeConfig;
    globalState.activeConfigName = tmp.activeConfigName;
    globalState.previousConfig = tmp.previousConfig;
    globalState.capsLockLedState = tmp.capsLockLedState;
    globalState.numLockLedState = tmp.numLockLedState;
    globalState.scrollLockLedState = tmp.scrollLockLedState;
    for(int i=0;i<MAX_NUM_MACROS;i++)
        globalState.recordedMacros[i] = tmp.recordedMacros[i];
}

void reload()
{
    initializeAllMaps();
    globals = defaultGlobals;
    options = defaultOptions;

    readSanitizeIniFile(sanitizedIniContent);

    parseIniGlobals();
    switchConfig(globalState.activeConfig, true);
}

void releaseAllSentKeys()
{
    IFDEBUG cout << endl << "Resetting all sent DOWN keys to UP: " << endl;
    for (int i = 0; i < 255; i++)
    {
        if (globalState.keysDownSent[i])
        {
            sendVKeyEvent({ i, false });
        }
    }
}

void printHelloHeader()
{
    string line1 = "Capsicain v" VERSION " (Linux)";
#ifdef NDEBUG
    line1 += " (Release build)";
#else
    line1 += " (DEBUG build)";
#endif
    size_t linelen = line1.length();

    cout << endl;
    for (unsigned int i = 0; i < linelen; i++)
        cout << "-";
    cout << endl << line1 << endl;
    for (unsigned int i = 0; i < linelen; i++)
        cout << "-";
    cout << endl;
}

void printOptions()
{
    cout
        << endl << endl << "OPTIONs"
        << endl << (options.debug ? "ON :" : "off: --") << " debug output for each key event"
        << endl << (options.flipZy ? "ON :" : "off: --") << " Z <-> Y"
        << endl << (options.flipAltWinOnAppleKeyboards ? "ON :" : "off: --") << " Alt <-> Win for Apple keyboards"
        << endl << (options.LControlLWinBlocksAlphaMapping ? "ON :" : "off: --") << " Left Control and Win block alpha key mapping"
        << endl
        ;
}

void printStatus()
{
    int numMakeSent = 0;
    for (int i = 0; i < 255; i++)
    {
        if (globalState.keysDownSent[i])
            numMakeSent++;
    }
    cout << "STATUS" << endl << endl
        << "Capsicain version: " << VERSION << " (Linux)" << endl
        << "ini version: " << globals.iniVersion << endl
        << "active config: " << globalState.activeConfig << " = " << globalState.activeConfigName << endl
        << "Capsicain on/off key: [" << (globals.capsicainOnOffKey >= 0 ? getPrettyVKLabel(globals.capsicainOnOffKey) : "(not defined)") << "]" << endl
        << "delay between keys in sequences (ms): " << options.delayForKeySequenceMS << endl
        << "number of keys-down sent: " << dec << numMakeSent << endl
        << (errorLog.length() > 1 ? "ERROR LOG contains entries" : "clean error log") << " (" << dec << errorLog.length() << " chars)"
        ;

    IFPROF cout << endl << endl << "Profiling statistics (microseconds)"
        << endl << "Incoming / Sent out: " << profiler.countIncoming << " / " << profiler.countOutgoing
        << endl << "Average mapping time: " << profiler.totalMappingTimeUS / profiler.countOutgoing
        << endl << "Average sending time: " << profiler.totalSendingTimeUS / profiler.countOutgoing
        << endl << "Worst mapping time: " << profiler.worstMappingTimeUS
        << endl << "Worst sending time: " << profiler.worstSendingTimeUS
        ;

    printOptions();
}

void printKeylabels()
{
    for (int i = 0; i <= 255; i++)
        cout << "sc " << uppercase << hex << i << " = " << PRETTY_VK_LABELS[i] << endl;
}

void printHelp()
{
    cout << "HELP" << endl << endl
        << "Press [ESC] + [key] for core commands" << endl << endl
        << "[H] Help" << endl
        << "[X] Exit" << endl
        << "[0]..[9] switch configs. [0] is the unchangeable empty config" << endl
        << "[W] flip ALT <-> WIN on Apple keyboards" << endl
        << "[Z] flip Y <-> Z keys" << endl
        << "[S] Status" << endl
        << "[D] Debug mode output" << endl
        << "[E] Error log" << endl
        << "[C] Print list of key labels for all scancodes" << endl
        << "[R] Reset and reload the .ini file" << endl
        << "[I] Show processed Ini for the active config" << endl
        << "[J][K][L][;] Macro Recording: Start,Stop,Playback,Copy macro definition" << endl
        << "[,] and [.]: delay between keys in sequences -/+ 1ms " << endl
        ;
}

void sendCapsicainCodeHandler(VKeyEvent keyEvent)
{
    if (!keyEvent.isDownstroke)
        return;

    IFTRACE cout << endl << "(CPS code: " << getPrettyVKLabelPadded(keyEvent.vcode, 0) << ")";

    switch (keyEvent.vcode)
    {
    case VK_CPS_CAPSON:
        if (!globalState.capsLockLedState)
        {
            sendVKeyEvent({ SC_CAPS, true });
            sendVKeyEvent({ SC_CAPS, false });
        }
        break;
    case VK_CPS_CAPSOFF:
        if (globalState.capsLockLedState)
        {
            sendVKeyEvent({ SC_CAPS, true });
            sendVKeyEvent({ SC_CAPS, false });
        }
        break;
    case VK_CPS_CONFIGPREVIOUS:
        switchConfig(globalState.previousConfig, false);
        break;
    case VK_CPS_ALTTAB:
    {
        linux_input_send(SC_LALT, true);
        linux_sleep_ms(10);
        linux_input_send(SC_TAB, true);
        globalState.altTabPassthrough = true;
        globalState.altTabExitScancode = SC_LALT;
        break;
    }
    case VK_CPS_OBFUSCATED_SEQUENCE_START:
    {
        globalState.secretSequencePlayback = true;
        break;
    }
    case VK_CPS_PAUSE:
        IFTRACE cout << endl << "sending the Pause key sequence";
        break;
    }
}

void sendResultingKeyOrSequence()
{
    if (loopState.resultingVKeyEventSequence.size() > 0)
    {
        playKeyEventSequence(loopState.resultingVKeyEventSequence);
    }
    else
    {
        IFDEBUG
        {
            if (loopState.scancode != loopState.vcode)
                cout << "  --  " << PRETTY_VK_LABELS[loopState.vcode] << (loopState.isDownstroke ? "v" : "^");
            else
                cout << "  -->";
        }
        {
            sendVKeyEvent({ loopState.vcode, loopState.isDownstroke });
        }
    }
}

void playKeyEventSequence(vector<VKeyEvent> keyEventSequence)
{
    if (keyEventSequence.size() == 0)
    {
        cout << endl << "BUG? keyEventSequence.size == 0" << endl;
        return;
    }

    unsigned int delayBetweenKeyEventsMS = options.delayForKeySequenceMS;
    bool tempReleasedKeys = false;
    int  expectParamForFuncKey = -1;

    IFDEBUG
        if (!globalState.secretSequencePlayback && keyEventSequence.at(0).vcode != VK_CPS_OBFUSCATED_SEQUENCE_START)
             cout << "  --> SEQUENCE (" << dec << keyEventSequence.size() << ")  ";

    for (VKeyEvent keyEvent : keyEventSequence)
    {
        int vc = keyEvent.vcode;
        if (globalState.secretSequencePlayback)
            vc = deObfuscateVKey(vc);

        if (expectParamForFuncKey != -1)
        {
            switch (expectParamForFuncKey)
            {
            case VK_CPS_SLEEP:
                IFTRACE cout << endl << "vk_cps_sleep: " << vc;
                linux_sleep_ms(vc);
                break;
            case VK_CPS_DEADKEY:
                IFTRACE cout << endl << "vk_cps_deadkey: " << getPrettyVKLabelPadded(vc, 0);
                modifierState.activeDeadkey = vc;
                break;
            case VK_CPS_CONFIGSWITCH:
                IFTRACE cout << endl << "vk_cps_configswitch: " << vc;
                switchConfig(vc, false);
                break;
            case VK_CPS_RECORDMACRO:
            case VK_CPS_RECORDSECRETMACRO:
            {
                int macroNum = vc;
                bool isSecret = (expectParamForFuncKey == VK_CPS_RECORDSECRETMACRO);

                if (macroNum < 1 || macroNum >= MAX_NUM_MACROS)
                    cout << endl << "ERROR in .ini: bad number for macro. Must be 1.." << MAX_NUM_MACROS - 1;
                else if (globalState.recordingMacro != -1)
                    cout << endl << "INFO: a macro is already being recorded: #" << globalState.recordingMacro;
                else
                {
                    IFDEBUG cout << endl << "Start recording " << (isSecret ? "secret" : "") << "macro #" << macroNum << endl;
                    globalState.recordingMacro = macroNum;
                    globalState.recordedMacros[macroNum].clear();

                    if (isSecret)
                    {
                        globalState.secretSequenceRecording = true;
                        globalState.recordedMacros[macroNum].push_back({ VK_CPS_OBFUSCATED_SEQUENCE_START, true });
                    }
                }
                break;
            }
            case VK_CPS_PLAYMACRO:
            {
                IFTRACE cout << endl << "vk_cps_playmacro: " << vc;
                int macnum = vc;

                if (macnum < 1 || macnum >= MAX_NUM_MACROS)
                    cout << endl << "ERROR: bad number for macro. Must be 1.." << MAX_NUM_MACROS - 1;
                else
                {
                    if (globalState.recordedMacros[macnum].size() == 0)
                        cout << endl << "INFO macro #" << macnum << " has not been recorded before.";
                    else
                    {
                        playKeyEventSequence(globalState.recordedMacros[macnum]);
                        globalState.secretSequencePlayback = false;
                    }
                }
                break;
            }
            default:
                cout << endl << "BUG? unknown expectParamForFuncKey";
            }

            expectParamForFuncKey = -1;
            continue;
        }

        if (vc == VK_CPS_TEMPRELEASEKEYS)
        {
            tempReleasedKeys = true;
            for (int i = 0; i <= 255; i++)
            {
                globalState.keysDownTempReleased[i] = globalState.keysDownSent[i];
                if (globalState.keysDownSent[i])
                    sendVKeyEvent({ i, false });
            }
            if (globalState.keysDownSentCounter != 0)
                error("BUG: keysDownSentCounter != 0");
        }
        else if (vc == VK_CPS_TEMPRESTOREKEYS)
        {
            tempReleasedKeys = false;
            for (int i = 0; i <= 255; i++)
            {
                if (globalState.keysDownTempReleased[i])
                {
                    sendVKeyEvent({ i, true });
                    globalState.keysDownTempReleased[i] = false;
                }
            }
        }
        else if (vc == VK_CPS_SLEEP
            || vc == VK_CPS_DEADKEY
            || vc == VK_CPS_CONFIGSWITCH
            || vc == VK_CPS_RECORDMACRO
            || vc == VK_CPS_RECORDSECRETMACRO
            || vc == VK_CPS_PLAYMACRO
            )
        {
            expectParamForFuncKey = vc;
        }
        else
        {
            if(globalState.secretSequencePlayback)
                sendVKeyEvent({ deObfuscateVKey(keyEvent.vcode) , keyEvent.isDownstroke });
            else
                sendVKeyEvent(keyEvent);
            if (vc == AHK_HOTKEY1 || vc == AHK_HOTKEY2)
                linux_sleep_ms(DEFAULT_DELAY_FOR_AHK_MS);
            else
                linux_sleep_ms(delayBetweenKeyEventsMS);
        }
    }

    if (tempReleasedKeys)
        error("VK_CPS_TEMPRELEASEKEYS without corresponding VK_CPS_TEMPRESTOREKEYS. Check your config.");
    if (expectParamForFuncKey != -1)
        error("BUG: func key with param: " + getPrettyVKLabel(expectParamForFuncKey) + " is unfinished");
}

void sendVKeyEvent(VKeyEvent keyEvent)
{
    IFTRACE cout << endl << "sendVkeyEvent(" << keyEvent.vcode << ")";
    if (keyEvent.vcode < 0)
    {
        cout << endl << "BUG: vcode<0";
        return;
    }
    if (keyEvent.vcode == 0)
    {
        cout << endl << "{blocked NOP}";
        return;
    }
    if (keyEvent.vcode > 0xFF || keyEvent.vcode == VK_CPS_PAUSE)
    {
        sendCapsicainCodeHandler(keyEvent);
        return;
    }

    unsigned char scancode = (unsigned char) keyEvent.vcode;

    if (!keyEvent.isDownstroke &&  !globalState.keysDownSent[scancode])
    {
        IFDEBUG cout << " {blocked " << PRETTY_VK_LABELS[scancode] << " UP: was not down.}";
        return;
    }

    if (globals.deactivateWinkeyStartmenu
        && scancode == SC_LWIN && !keyEvent.isDownstroke && globalState.keysDownSent[SC_LWIN]
        && (globalState.lastSentKeyEvent.vcode == SC_LWIN)
        )
    {
        IFDEBUG cout << " { test WINKEY NO MENU send shift down up" << "}";
        SendShiftDownUp();
    }

    if (globalState.keysDownSent[scancode] == 0 && keyEvent.isDownstroke)
        globalState.keysDownSentCounter++;
    else if (globalState.keysDownSent[scancode] == 1 && !keyEvent.isDownstroke)
        globalState.keysDownSentCounter--;

    globalState.keysDownSent[scancode] = keyEvent.isDownstroke;

    if (globalState.recordingMacro >= 0)
    {
        if (globalState.recordedMacros[globalState.recordingMacro].size() >= (unsigned int)MAX_MACRO_LENGTH - 2)
        {
            globalState.recordingMacro = -1;
            globalState.secretSequenceRecording = false;
            cout << endl << endl << "Macro Length > " << MAX_MACRO_LENGTH << ". Forgotten Macro?" << "Stop recording macro #" << globalState.recordingMacro << endl << endl;
        }
        else
        {
            if (keyEvent.isDownstroke || globalState.recordedMacros[globalState.recordingMacro].size() > 0)
            {
                VKeyEvent obfusc = keyEvent;
                if (globalState.secretSequenceRecording)
                    obfusc.vcode = obfuscateVKey(obfusc.vcode);
                globalState.recordedMacros[globalState.recordingMacro].push_back(obfusc);
            }
        }
    }

    // Track CapsLock/NumLock/ScrollLock LED state
    if (keyEvent.vcode == SC_CAPS)
        globalState.capsLockLedState = keyEvent.isDownstroke ? !globalState.capsLockLedState : globalState.capsLockLedState;
    if (keyEvent.vcode == SC_NUMLOCK)
        globalState.numLockLedState = keyEvent.isDownstroke ? !globalState.numLockLedState : globalState.numLockLedState;
    if (keyEvent.vcode == SC_SCRLOCK)
        globalState.scrollLockLedState = keyEvent.isDownstroke ? !globalState.scrollLockLedState : globalState.scrollLockLedState;

    IFDEBUG
        if(!globalState.secretSequencePlayback)
            cout << " {" << PRETTY_VK_LABELS[keyEvent.vcode] << (keyEvent.isDownstroke ? "v" : "^") << " #" << globalState.keysDownSentCounter << "}";

    linux_input_send(keyEvent.vcode, keyEvent.isDownstroke);
    globalState.lastSentKeyEvent = keyEvent;
}

void SendShiftDownUp()
{
    IFDEBUG cout << " { LSHFv^ to deactivateWinkeyStartmenu } ";
    linux_input_send(SC_LSHIFT, true);
    linux_input_send(SC_LSHIFT, false);
}

void keySequenceAppendMakeKey(unsigned short scancode, vector<VKeyEvent> &sequence)
{
    sequence.push_back({ (int)scancode, true });
}
void keySequenceAppendBreakKey(unsigned short scancode, vector<VKeyEvent> &sequence)
{
    sequence.push_back({ (int)scancode, false });
}
void keySequenceAppendMakeBreakKey(unsigned short scancode, vector<VKeyEvent> &sequence)
{
    sequence.push_back({ (int)scancode, true });
    sequence.push_back({ (int)scancode, false });
}

string getSymbolForIKStrokeState(unsigned short state)
{
    switch (state)
    {
    case 0b000: return "v";
    case 0b001: return "^";
    case 0b010: return "v*";
    case 0b011: return "^*";
    case 0b100: return "v**";
    case 0b101: return "^**";
    }
    return "???" + to_string(state);
}

int obfuscateVKey(int vk)
{
    return vk ^ 0b0101010101010101;
}
int deObfuscateVKey(int vk)
{
    return vk ^ 0b0101010101010101;
}