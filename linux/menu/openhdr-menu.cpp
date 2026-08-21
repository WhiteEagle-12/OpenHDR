#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>

#include "gamescope-action-binding-client-protocol.h"

extern char **environ;

namespace {

volatile std::sig_atomic_t stopRequested = 0;

void requestStop(int) {
    stopRequested = 1;
}

struct Settings {
    bool enabled = true;
    int peak = 1000;
    int paperWhite = 65;
    int contrast = 120;
    int saturation = 110;
    int outputScale = 100;
    int debandStrength = 100;
    int debandRadius = 16;
    double debandThreshold = 3.0;
    bool dithering = true;
    int ditherStrength = 100;
    int blackFloor = 20;
};

struct Slider {
    const char *label;
    double Settings::*floatingMember = nullptr;
    int Settings::*integerMember = nullptr;
    double minimum;
    double maximum;
    double step;
    const char *suffix;
    bool rebuildLook;
};

constexpr int kPanelWidth = 820;
constexpr int kPanelHeight = 650;
constexpr int kColumnWidth = 360;

double envNumber(const char *name, double fallback) {
    const char *value = std::getenv(name);
    if (!value || !*value) return fallback;
    char *end = nullptr;
    const double parsed = std::strtod(value, &end);
    return end != value ? parsed : fallback;
}

std::string envString(const char *name) {
    const char *value = std::getenv(name);
    return value ? value : "";
}

std::string numberString(double value) {
    std::ostringstream stream;
    if (std::abs(value - std::round(value)) < 0.0001)
        stream << static_cast<int>(std::round(value));
    else
        stream << std::fixed << std::setprecision(2) << value;
    return stream.str();
}

int runCommand(const std::vector<std::string> &arguments) {
    if (arguments.empty()) return -1;
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string &argument : arguments)
        argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t child = 0;
    if (posix_spawnp(&child, argv[0], nullptr, nullptr, argv.data(), environ) != 0)
        return -1;
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void roundedRectangle(cairo_t *cr, double x, double y, double width, double height, double radius) {
    const double right = x + width;
    const double bottom = y + height;
    cairo_new_sub_path(cr);
    cairo_arc(cr, right - radius, y + radius, radius, -M_PI_2, 0);
    cairo_arc(cr, right - radius, bottom - radius, radius, 0, M_PI_2);
    cairo_arc(cr, x + radius, bottom - radius, radius, M_PI_2, M_PI);
    cairo_arc(cr, x + radius, y + radius, radius, M_PI, M_PI * 1.5);
    cairo_close_path(cr);
}

void setSourceHex(cairo_t *cr, unsigned color, double alpha = 1.0) {
    cairo_set_source_rgba(cr,
        ((color >> 16) & 0xff) / 255.0,
        ((color >> 8) & 0xff) / 255.0,
        (color & 0xff) / 255.0,
        alpha);
}

void drawText(cairo_t *cr, const std::string &text, double x, double y, double size,
              unsigned color, bool bold = false, PangoAlignment alignment = PANGO_ALIGN_LEFT,
              double width = -1.0) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_new();
    pango_font_description_set_family(font, "Inter, Noto Sans, sans-serif");
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_font_description_set_weight(font, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text.c_str(), -1);
    if (width > 0.0) {
        pango_layout_set_width(layout, static_cast<int>(width * PANGO_SCALE));
        pango_layout_set_alignment(layout, alignment);
    }
    setSourceHex(cr, color);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font);
    g_object_unref(layout);
}

class OpenHdrMenu {
public:
    ~OpenHdrMenu() { shutdown(); }

    bool initialize() {
        loadEnvironment();
        if (!initializeWayland() || !initializeX11()) return false;
        worker = std::thread([this] { workerLoop(); });
        applyShaderSettings();
        if (envNumber("OPENHDR_MENU_TEST_OPEN", 0) != 0)
            setVisible(true);
        return true;
    }

    int run() {
        while (running && !stopRequested) {
            pollfd descriptors[2] = {
                { ConnectionNumber(display), POLLIN, 0 },
                { wl_display_get_fd(waylandDisplay), POLLIN, 0 },
            };
            wl_display_dispatch_pending(waylandDisplay);
            wl_display_flush(waylandDisplay);
            const int result = poll(descriptors, 2, 33);
            if (result < 0) continue;
            if (descriptors[1].revents & POLLIN) {
                if (wl_display_dispatch(waylandDisplay) < 0) break;
            }
            while (XPending(display)) processXEvent();
            const unsigned statusNow = statusGeneration.load();
            if (visible && statusNow != drawnStatusGeneration) {
                drawnStatusGeneration = statusNow;
                draw();
            }
        }
        return 0;
    }

private:
    Display *display = nullptr;
    Window rootWindow = 0;
    Window window = 0;
    cairo_surface_t *surface = nullptr;
    cairo_surface_t *backSurface = nullptr;
    cairo_t *presentCr = nullptr;
    cairo_t *cr = nullptr;
    wl_display *waylandDisplay = nullptr;
    gamescope_action_binding_manager *bindingManager = nullptr;
    gamescope_action_binding *menuBinding = nullptr;
    gamescope_action_binding *toggleBinding = nullptr;
    Atom inputFocusAtom = None;
    Atom opacityAtom = None;
    bool running = true;
    bool visible = false;
    std::chrono::steady_clock::time_point lastToggle{};
    std::chrono::steady_clock::time_point lastEnabledToggle{};
    int screenWidth = 0;
    int screenHeight = 0;
    double scale = 1.0;
    double panelX = 0.0;
    double panelY = 0.0;
    int activeSlider = -1;
    bool activeSliderRebuild = false;
    unsigned drawnStatusGeneration = 0;
    std::atomic<unsigned> statusGeneration{1};
    std::atomic<int> status{0}; // 0 ready, 1 applying, 2 error, 3 disabled
    Settings settings;
    std::vector<Slider> sliders;
    std::string generator;
    std::string lut;
    std::string atlas;
    std::string config;
    std::string profileName;
    std::string liveLook;
    std::thread worker;
    std::mutex workerMutex;
    std::condition_variable workerCondition;
    bool workerStop = false;
    bool lookPending = false;
    unsigned requestedGeneration = 0;
    Settings pendingSettings;

    static void registryGlobal(void *data, wl_registry *registry, uint32_t name,
                               const char *interface, uint32_t version) {
        auto *self = static_cast<OpenHdrMenu *>(data);
        if (std::string(interface) == gamescope_action_binding_manager_interface.name) {
            self->bindingManager = static_cast<gamescope_action_binding_manager *>(
                wl_registry_bind(registry, name, &gamescope_action_binding_manager_interface,
                                 std::min(version, 1u)));
        }
    }

    static void registryRemove(void *, wl_registry *, uint32_t) {}
    static void menuHotkeyTriggered(void *data, gamescope_action_binding *, uint32_t,
                                    uint32_t, uint32_t, uint32_t) {
        static_cast<OpenHdrMenu *>(data)->toggleVisible();
    }

    static void toggleHotkeyTriggered(void *data, gamescope_action_binding *, uint32_t,
                                      uint32_t, uint32_t, uint32_t) {
        static_cast<OpenHdrMenu *>(data)->toggleEnabled();
    }

    bool initializeWayland() {
        const std::string name = envString("GAMESCOPE_WAYLAND_DISPLAY");
        waylandDisplay = wl_display_connect(name.empty() ? "gamescope-0" : name.c_str());
        if (!waylandDisplay) return false;
        wl_registry *registry = wl_display_get_registry(waylandDisplay);
        static const wl_registry_listener registryListener = { registryGlobal, registryRemove };
        wl_registry_add_listener(registry, &registryListener, this);
        wl_display_roundtrip(waylandDisplay);
        wl_display_roundtrip(waylandDisplay);
        wl_registry_destroy(registry);
        if (!bindingManager) return false;

        menuBinding = gamescope_action_binding_manager_create_action_binding(bindingManager);
        toggleBinding = gamescope_action_binding_manager_create_action_binding(bindingManager);
        static const gamescope_action_binding_listener menuListener = { menuHotkeyTriggered };
        static const gamescope_action_binding_listener toggleListener = { toggleHotkeyTriggered };
        gamescope_action_binding_add_listener(menuBinding, &menuListener, this);
        gamescope_action_binding_add_listener(toggleBinding, &toggleListener, this);
        gamescope_action_binding_set_description(menuBinding, "OpenHDR settings (Alt+Z)");
        gamescope_action_binding_set_description(toggleBinding, "Toggle OpenHDR (Alt+X)");

        auto addTrigger = [](gamescope_action_binding *target, uint32_t alt, uint32_t key) {
            wl_array array;
            wl_array_init(&array);
            auto *keys = static_cast<uint32_t *>(wl_array_add(&array, sizeof(uint32_t) * 2));
            keys[0] = alt;
            keys[1] = key;
            gamescope_action_binding_add_keyboard_trigger(target, &array);
            wl_array_release(&array);
        };
        for (const uint32_t alt : { XKB_KEY_Alt_L, XKB_KEY_Alt_R }) {
            addTrigger(toggleBinding, alt, XKB_KEY_X);
            addTrigger(menuBinding, alt, XKB_KEY_Z);
        }
        // Keep the bindings registered for protocol compatibility but disarmed.
        // In nested KWin/Gamescope sessions a physical key can arrive through
        // both this protocol and Xwayland, causing close-then-immediate-reopen.
        // X11 is the single shortcut owner below.
        gamescope_action_binding_disarm(menuBinding);
        gamescope_action_binding_disarm(toggleBinding);
        wl_display_flush(waylandDisplay);
        return true;
    }

    bool initializeX11() {
        display = XOpenDisplay(nullptr);
        if (!display) return false;
        const int screen = DefaultScreen(display);
        Window root = RootWindow(display, screen);
        rootWindow = root;
        XWindowAttributes rootAttributes{};
        XGetWindowAttributes(display, root, &rootAttributes);
        screenWidth = rootAttributes.width;
        screenHeight = rootAttributes.height;

        XVisualInfo visualInfo{};
        if (!XMatchVisualInfo(display, screen, 32, TrueColor, &visualInfo)) return false;
        Colormap colormap = XCreateColormap(display, root, visualInfo.visual, AllocNone);
        XSetWindowAttributes attributes{};
        attributes.colormap = colormap;
        attributes.border_pixel = 0;
        attributes.background_pixel = 0;
        attributes.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
            ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
            StructureNotifyMask | FocusChangeMask;
        window = XCreateWindow(display, root, 0, 0, screenWidth, screenHeight, 0,
            visualInfo.depth, InputOutput, visualInfo.visual,
            CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &attributes);
        if (!window) return false;

        XClassHint classHint{ const_cast<char *>("openhdr-menu"), const_cast<char *>("OpenHDR") };
        XSetClassHint(display, window, &classHint);
        XStoreName(display, window, "OpenHDR Settings");
        XWMHints wmHints{};
        wmHints.flags = InputHint;
        wmHints.input = True;
        XSetWMHints(display, window, &wmHints);
        const KeyCode hotkeys[] = {
            XKeysymToKeycode(display, XK_x),
            XKeysymToKeycode(display, XK_z),
        };
        const unsigned ignoredModifiers[] = {
            0, LockMask, Mod2Mask, LockMask | Mod2Mask,
            Mod3Mask, LockMask | Mod3Mask, Mod2Mask | Mod3Mask,
            LockMask | Mod2Mask | Mod3Mask,
        };
        for (const KeyCode hotkey : hotkeys)
            for (const unsigned ignored : ignoredModifiers)
                XGrabKey(display, hotkey, Mod1Mask | ignored, rootWindow, False,
                         GrabModeAsync, GrabModeAsync);
        setCardinal(XInternAtom(display, "STEAM_OVERLAY", False), 1);
        // Gamescope deliberately excludes overlay windows from normal focus.
        // This property is its compositor-native mechanism for routing pointer
        // and keyboard input to an interactive Steam-style overlay.
        inputFocusAtom = XInternAtom(display, "STEAM_INPUT_FOCUS", False);
        setCardinal(inputFocusAtom, 0);
        opacityAtom = XInternAtom(display, "_NET_WM_WINDOW_OPACITY", False);
        setCardinal(opacityAtom, 0);
        Atom fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
        Atom stateAtom = XInternAtom(display, "_NET_WM_STATE", False);
        XChangeProperty(display, window, stateAtom, XA_ATOM, 32, PropModeReplace,
            reinterpret_cast<unsigned char *>(&fullscreen), 1);
        XFlush(display);

        surface = cairo_xlib_surface_create(display, window, visualInfo.visual, screenWidth, screenHeight);
        presentCr = cairo_create(surface);
        createBackBuffer();
        updateLayout();
        return true;
    }

    void loadEnvironment() {
        settings.enabled = envNumber("OPENHDR_ENABLED", 1) != 0;
        settings.peak = static_cast<int>(envNumber("OPENHDR_PEAK", 1000));
        settings.paperWhite = static_cast<int>(envNumber("OPENHDR_PAPER_WHITE", 65));
        settings.contrast = static_cast<int>(envNumber("OPENHDR_CONTRAST", 120));
        settings.saturation = static_cast<int>(envNumber("OPENHDR_SATURATION", 110));
        settings.outputScale = static_cast<int>(envNumber("OPENHDR_OUTPUT_SCALE", 100));
        settings.debandStrength = static_cast<int>(envNumber("OPENHDR_DEBAND_STRENGTH", 100));
        settings.debandRadius = static_cast<int>(envNumber("OPENHDR_DEBAND_RADIUS", 16));
        settings.debandThreshold = envNumber("OPENHDR_DEBAND_THRESHOLD", 3);
        settings.dithering = envNumber("OPENHDR_DITHERING", 1) != 0;
        settings.ditherStrength = static_cast<int>(envNumber("OPENHDR_DITHER_STRENGTH", 100));
        settings.blackFloor = static_cast<int>(envNumber("OPENHDR_BLACK_FLOOR", 20));
        generator = envString("OPENHDR_GENERATOR");
        lut = envString("OPENHDR_LUT");
        atlas = envString("OPENHDR_ATLAS");
        config = envString("OPENHDR_CONFIG");
        profileName = envString("OPENHDR_PROFILE_NAME");
        std::filesystem::path initialLook(envString("OPENHDR_LOOK"));
        liveLook = envString("OPENHDR_LIVE_LOOK");
        if (liveLook.empty())
            liveLook = ((initialLook.empty() ? std::filesystem::temp_directory_path() : initialLook.parent_path()) / "look-live.cube").string();

        sliders = {
            {"Peak brightness", nullptr, &Settings::peak, 400, 2000, 100, " nits", true},
            {"Paper white", nullptr, &Settings::paperWhite, 10, 100, 5, " nits", true},
            {"Contrast", nullptr, &Settings::contrast, 0, 200, 5, "%", true},
            {"Saturation", nullptr, &Settings::saturation, 0, 200, 5, "%", true},
            {"Output intensity", nullptr, &Settings::outputScale, 25, 200, 1, "%", true},
            {"Deband strength", nullptr, &Settings::debandStrength, 0, 100, 1, "%", false},
            {"Sample radius", nullptr, &Settings::debandRadius, 1, 32, 1, " px", false},
            {"Edge threshold", &Settings::debandThreshold, nullptr, 0, 10, 0.25, " permille", false},
            {"Dither strength", nullptr, &Settings::ditherStrength, 0, 100, 1, "%", false},
            {"Black floor", nullptr, &Settings::blackFloor, 0, 50, 1, " permille", true},
        };
        status = settings.enabled ? 0 : 3;
    }

    void setCardinal(Atom atom, unsigned long value) {
        XChangeProperty(display, window, atom, XA_CARDINAL, 32, PropModeReplace,
            reinterpret_cast<unsigned char *>(&value), 1);
    }

    bool waitForInputFocus() {
        for (int attempt = 0; attempt < 20; ++attempt) {
            Window focused = None;
            int revert = RevertToNone;
            XGetInputFocus(display, &focused, &revert);
            if (focused == window) return true;
            XSync(display, False);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::fprintf(stderr, "OpenHDR menu: Gamescope did not grant overlay input focus\n");
        return false;
    }

    void setVisible(bool show) {
        if (show) {
            updateLayout();
            visible = true;
            // Present a complete frame before mapping so Gamescope never sees
            // a transparent or half-painted fullscreen overlay.
            draw();
            setCardinal(opacityAtom, 0xfffffffful);
            setCardinal(inputFocusAtom, 1);
            XMapRaised(display, window);
            XSync(display, False);
            if (!waitForInputFocus()) {
                setCardinal(inputFocusAtom, 0);
                setCardinal(opacityAtom, 0);
                XUnmapWindow(display, window);
                XFlush(display);
                visible = false;
                return;
            }
            XGrabKey(display, XKeysymToKeycode(display, XK_Escape), AnyModifier,
                     rootWindow, False, GrabModeAsync, GrabModeAsync);
        } else {
            visible = false;
            activeSlider = -1;
            XUngrabKey(display, XKeysymToKeycode(display, XK_Escape), AnyModifier,
                       rootWindow);
            setCardinal(inputFocusAtom, 0);
            // Gamescope can retain the final commit of a Steam overlay after
            // XUnmapWindow. Replace it with one fully transparent commit and
            // force an opacity-driven repaint before the window disappears.
            // The offscreen buffer keeps this a single atomic-looking frame.
            clearBackBuffer();
            presentBackBuffer();
            setCardinal(opacityAtom, 0);
            XSync(display, False);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            XUnmapWindow(display, window);
        }
        XFlush(display);
    }

    void toggleVisible() {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastToggle < std::chrono::milliseconds(180)) return;
        lastToggle = now;
        setVisible(!visible);
    }

    void toggleEnabled() {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastEnabledToggle < std::chrono::milliseconds(180)) return;
        lastEnabledToggle = now;
        settings.enabled = !settings.enabled;
        applyEnabled();
        saveSettings();
        if (visible) draw();
    }

    void updateLayout() {
        scale = std::clamp(screenHeight / 820.0, 0.78, 1.25);
        panelX = (screenWidth - kPanelWidth * scale) * 0.5;
        panelY = (screenHeight - kPanelHeight * scale) * 0.5;
    }

    void createBackBuffer() {
        if (cr) cairo_destroy(cr);
        if (backSurface) cairo_surface_destroy(backSurface);
        backSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, screenWidth, screenHeight);
        cr = cairo_create(backSurface);
    }

    void clearBackBuffer() {
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    void presentBackBuffer() {
        cairo_surface_flush(backSurface);
        cairo_save(presentCr);
        cairo_set_operator(presentCr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(presentCr, backSurface, 0, 0);
        cairo_paint(presentCr);
        cairo_restore(presentCr);
        cairo_surface_flush(surface);
        XFlush(display);
    }

    double sliderValue(const Slider &slider) const {
        return slider.integerMember ? settings.*(slider.integerMember) : settings.*(slider.floatingMember);
    }

    bool setSliderValue(const Slider &slider, double value) {
        const double previous = sliderValue(slider);
        value = std::clamp(value, slider.minimum, slider.maximum);
        value = std::round((value - slider.minimum) / slider.step) * slider.step + slider.minimum;
        if (slider.integerMember) settings.*(slider.integerMember) = static_cast<int>(std::round(value));
        else settings.*(slider.floatingMember) = value;
        return std::abs(sliderValue(slider) - previous) > 0.0001;
    }

    void drawSlider(const Slider &slider, double x, double y, double width) {
        const double value = sliderValue(slider);
        drawText(cr, slider.label, x, y, 12, 0xd6dde6);
        drawText(cr, numberString(value) + slider.suffix, x, y, 12, 0x73b2ff, true, PANGO_ALIGN_RIGHT, width);
        const double trackY = y + 29;
        roundedRectangle(cr, x, trackY, width, 4, 2);
        setSourceHex(cr, 0x303945);
        cairo_fill(cr);
        const double fraction = (value - slider.minimum) / (slider.maximum - slider.minimum);
        roundedRectangle(cr, x, trackY, width * fraction, 4, 2);
        setSourceHex(cr, 0x479cff);
        cairo_fill(cr);
        cairo_arc(cr, x + width * fraction, trackY + 2, 7, 0, M_PI * 2);
        setSourceHex(cr, 0xeef8ff);
        cairo_fill(cr);
    }

    void drawSwitch(double x, double y, bool enabled, bool small = false) {
        const double width = small ? 32 : 44;
        const double height = small ? 18 : 25;
        roundedRectangle(cr, x, y, width, height, height / 2);
        setSourceHex(cr, enabled ? 0x267bdc : 0x20262e);
        cairo_fill(cr);
        const double radius = small ? 6 : 8;
        const double cx = enabled ? x + width - radius - 3 : x + radius + 3;
        cairo_arc(cr, cx, y + height / 2, radius, 0, M_PI * 2);
        setSourceHex(cr, enabled ? 0xffffff : 0xaeb8c5);
        cairo_fill(cr);
    }

    void drawSection(double x, double y, double width, double height, const std::string &title) {
        roundedRectangle(cr, x, y, width, height, 13);
        setSourceHex(cr, 0x15191f, 0.96);
        cairo_fill_preserve(cr);
        setSourceHex(cr, 0x252b34);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        drawText(cr, title, x + 16, y + 13, 11, 0xaeb8c5, true);
    }

    void draw() {
        clearBackBuffer();
        cairo_save(cr);
        setSourceHex(cr, 0x000000, 0.36);
        cairo_paint(cr);
        cairo_translate(cr, panelX, panelY);
        cairo_scale(cr, scale, scale);

        roundedRectangle(cr, 0, 0, kPanelWidth, kPanelHeight, 18);
        setSourceHex(cr, 0x0b0d10, 0.985);
        cairo_fill_preserve(cr);
        setSourceHex(cr, 0x303945, 0.9);
        cairo_set_line_width(cr, 1.2);
        cairo_stroke(cr);

        drawText(cr, "OPENHDR", 28, 22, 10, 0x66a9ff, true);
        drawText(cr, "Game processing", 28, 39, 22, 0xeef2f6, true);
        drawText(cr, "Alt+X Toggle  |  Alt+Z Menu", 545, 29, 11, 0x778290, false, PANGO_ALIGN_RIGHT, 205);
        drawSwitch(750, 30, settings.enabled);

        roundedRectangle(cr, 28, 78, 764, 38, 10);
        setSourceHex(cr, 0x12161b);
        cairo_fill_preserve(cr);
        setSourceHex(cr, 0x29313a);
        cairo_stroke(cr);
        const int state = status.load();
        unsigned dot = state == 2 ? 0xff6d67 : state == 3 ? 0x788493 : state == 1 ? 0xffbd59 : 0x40d890;
        cairo_arc(cr, 44, 97, 4, 0, M_PI * 2);
        setSourceHex(cr, dot);
        cairo_fill(cr);
        const char *statusText = state == 2 ? "ERROR - the new settings did not reach Gamescope" :
            state == 3 ? "OFF - tone controls have no effect; the game is passing through" :
            state == 1 ? "Rebuilding and uploading the signed 49^3 LUT..." :
                         "OpenHDR is active - linear scRGB HDR";
        drawText(cr, statusText, 57, 88, 11, 0xaeb8c5);
        if (!profileName.empty())
            drawText(cr, "Profile: " + profileName, 540, 88, 10, 0x778290, false,
                     PANGO_ALIGN_RIGHT, 235);

        drawSection(28, 132, 374, 430, "TONE MAPPING");
        drawSection(418, 132, 374, 430, "BANDING AND SHADOWS");
        drawText(cr, "Dither", 690, 145, 11, 0xaeb8c5);
        drawSwitch(747, 144, settings.dithering, true);

        for (int index = 0; index < 5; ++index)
            drawSlider(sliders[index], 46, 171 + index * 72, kColumnWidth - 34);
        for (int index = 5; index < 10; ++index)
            drawSlider(sliders[index], 436, 171 + (index - 5) * 72, kColumnWidth - 34);

        roundedRectangle(cr, 28, 582, 112, 36, 9);
        setSourceHex(cr, 0x171c22);
        cairo_fill_preserve(cr);
        setSourceHex(cr, 0x303945);
        cairo_stroke(cr);
        drawText(cr, "Reset defaults", 43, 591, 11, 0xc9d2dd);
        drawText(cr, "49^3 signed LUT  -  scRGB HDR  -  Gamescope compositor", 405, 594, 10, 0x778290, false, PANGO_ALIGN_RIGHT, 387);

        cairo_restore(cr);
        presentBackBuffer();
    }

    bool logicalPoint(const XEvent &event, double &x, double &y) const {
        const int rawX = event.type == MotionNotify ? event.xmotion.x : event.xbutton.x;
        const int rawY = event.type == MotionNotify ? event.xmotion.y : event.xbutton.y;
        x = (rawX - panelX) / scale;
        y = (rawY - panelY) / scale;
        return x >= 0 && y >= 0 && x <= kPanelWidth && y <= kPanelHeight;
    }

    int sliderAt(double x, double y) const {
        for (int index = 0; index < 5; ++index)
            if (x >= 46 && x <= 372 && y >= 191 + index * 72 && y <= 220 + index * 72) return index;
        for (int index = 5; index < 10; ++index)
            if (x >= 436 && x <= 762 && y >= 191 + (index - 5) * 72 && y <= 220 + (index - 5) * 72) return index;
        return -1;
    }

    void updateSliderFromPointer(int index, double x) {
        const bool left = index < 5;
        const double start = left ? 46 : 436;
        const double width = 326;
        const double fraction = std::clamp((x - start) / width, 0.0, 1.0);
        const Slider &slider = sliders[index];
        if (setSliderValue(slider, slider.minimum + fraction * (slider.maximum - slider.minimum)))
            draw();
    }

    void processXEvent() {
        XEvent event{};
        XNextEvent(display, &event);
        // Keep only the newest consecutive pointer position. A 240 Hz mouse can
        // otherwise queue multiple full overlay frames for every compositor
        // refresh and make the menu appear to strobe while dragging.
        if (event.type == MotionNotify) {
            while (XPending(display)) {
                XEvent next{};
                XPeekEvent(display, &next);
                if (next.type != MotionNotify || next.xmotion.window != window) break;
                XNextEvent(display, &event);
            }
        }
        if (event.type == KeyPress) {
            const KeySym symbol = XLookupKeysym(&event.xkey, 0);
            const bool toggleKey = symbol == XK_x || symbol == XK_X;
            const bool menuKey = symbol == XK_z || symbol == XK_Z;
            if ((toggleKey || menuKey) && (event.xkey.state & Mod1Mask)) {
                if (toggleKey) toggleEnabled();
                else toggleVisible();
                return;
            }
            if (visible && symbol == XK_Escape) setVisible(false);
            return;
        }
        if (event.type == Expose && visible) { draw(); return; }
        if (event.type == ConfigureNotify) {
            if (event.xconfigure.width == screenWidth && event.xconfigure.height == screenHeight)
                return;
            screenWidth = event.xconfigure.width;
            screenHeight = event.xconfigure.height;
            cairo_xlib_surface_set_size(surface, screenWidth, screenHeight);
            createBackBuffer();
            updateLayout();
            if (visible) draw();
            return;
        }
        if (!visible) return;

        double x = 0, y = 0;
        const bool insidePanel = logicalPoint(event, x, y);
        // Always commit a drag on release, even if the pointer left the panel.
        // Previously that path silently saved no LUT, making a moved slider
        // appear functional while the transform remained unchanged.
        if (event.type == ButtonRelease && activeSlider >= 0) {
            updateSliderFromPointer(activeSlider, x);
            if (activeSliderRebuild) scheduleLookRebuild();
            else applyShaderSettings();
            saveSettings();
            activeSlider = -1;
            return;
        }
        if (!insidePanel) {
            // Treat the dimmed backdrop like a normal modal menu: clicking it
            // closes the overlay while still consuming the press, so the game
            // beneath never receives the click.
            if (event.type == ButtonPress) setVisible(false);
            return;
        }
        if (event.type == ButtonPress) {
            if (x >= 740 && x <= 804 && y >= 20 && y <= 65) {
                toggleEnabled();
                return;
            }
            if (x >= 675 && x <= 794 && y >= 135 && y <= 169) {
                settings.dithering = !settings.dithering;
                applyShaderSettings();
                saveSettings();
                draw();
                return;
            }
            if (x >= 28 && x <= 140 && y >= 582 && y <= 618) {
                resetDefaults();
                return;
            }
            activeSlider = sliderAt(x, y);
            if (activeSlider >= 0) {
                activeSliderRebuild = sliders[activeSlider].rebuildLook;
                updateSliderFromPointer(activeSlider, x);
            }
        } else if (event.type == MotionNotify && activeSlider >= 0) {
            updateSliderFromPointer(activeSlider, x);
        }
    }

    void resetDefaults() {
        settings = Settings{};
        applyShaderSettings();
        applyEnabled();
        saveSettings();
        draw();
    }

    void applyEnabled() {
        if (runCommand({"gamescopectl", "openhdr_enabled", settings.enabled ? "1" : "0"}) != 0) {
            status = 2;
            statusGeneration++;
            return;
        }
        if (settings.enabled) {
            if (std::filesystem::exists(liveLook))
                runCommand({"gamescopectl", "set_look", liveLook});
            scheduleLookRebuild();
        } else {
            {
                std::lock_guard lock(workerMutex);
                // Keep generating the selected tone settings while bypassed.
                // This makes the next enable instantaneous and prevents a fast
                // off/on test from canceling a slider change permanently.
                pendingSettings = settings;
                lookPending = true;
                ++requestedGeneration;
            }
            // Gamescope's set_look console command clears all looks when it is
            // called without a LUT path.
            runCommand({"gamescopectl", "set_look"});
            status = 3;
            statusGeneration++;
            workerCondition.notify_one();
        }
    }

    void applyShaderSettings() {
        const double dither = settings.dithering ? settings.ditherStrength / 100.0 : 0.0;
        const bool applied =
            runCommand({"gamescopectl", "openhdr_deband_strength", numberString(settings.debandStrength / 100.0)}) == 0 &&
            runCommand({"gamescopectl", "openhdr_deband_radius", numberString(settings.debandRadius)}) == 0 &&
            runCommand({"gamescopectl", "openhdr_deband_threshold", numberString(settings.debandThreshold / 1000.0)}) == 0 &&
            runCommand({"gamescopectl", "openhdr_dither_strength", numberString(dither)}) == 0;
        if (!applied) {
            status = 2;
            statusGeneration++;
        }
    }

    void saveSettings() const {
        if (config.empty()) return;
        const std::filesystem::path path(config);
        std::filesystem::create_directories(path.parent_path());
        const std::filesystem::path temporary = path.string() + ".tmp";
        std::ofstream output(temporary);
        output << "enabled=" << (settings.enabled ? 1 : 0) << '\n'
               << "peak=" << settings.peak << '\n'
               << "paper_white=" << settings.paperWhite << '\n'
               << "contrast=" << settings.contrast << '\n'
               << "saturation=" << settings.saturation << '\n'
               << "output_scale=" << settings.outputScale << '\n'
               << "deband_strength=" << settings.debandStrength << '\n'
               << "deband_radius=" << settings.debandRadius << '\n'
               << "deband_threshold=" << numberString(settings.debandThreshold) << '\n'
               << "dithering=" << (settings.dithering ? 1 : 0) << '\n'
               << "dither_strength=" << settings.ditherStrength << '\n'
               << "black_floor=" << settings.blackFloor << '\n';
        output.close();
        std::filesystem::rename(temporary, path);
    }

    void scheduleLookRebuild() {
        {
            std::lock_guard lock(workerMutex);
            pendingSettings = settings;
            lookPending = true;
            ++requestedGeneration;
        }
        status = settings.enabled ? 1 : 3;
        statusGeneration++;
        workerCondition.notify_one();
    }

    void workerLoop() {
        for (;;) {
            Settings work;
            unsigned generation = 0;
            {
                std::unique_lock lock(workerMutex);
                workerCondition.wait(lock, [this] { return workerStop || lookPending; });
                if (workerStop) return;
                work = pendingSettings;
                generation = requestedGeneration;
                lookPending = false;
            }
            const int result = runCommand({
                "python3", generator, "--lut", lut, "--atlas", atlas,
                "--output", liveLook,
                "--peak", numberString(work.peak),
                "--paper-white", numberString(work.paperWhite),
                "--contrast", numberString(work.contrast),
                "--saturation", numberString(work.saturation),
                "--output-scale", numberString(work.outputScale),
                "--black-floor", numberString(work.blackFloor),
            });
            bool latest = false;
            {
                std::lock_guard lock(workerMutex);
                latest = generation == requestedGeneration;
            }
            if (!latest) continue;
            if (result == 0 && work.enabled) {
                const int upload = runCommand({"gamescopectl", "set_look", liveLook});
                status = upload == 0 ? 0 : 2;
            } else {
                status = result == 0 ? 3 : 2;
            }
            statusGeneration++;
        }
    }

    void shutdown() {
        {
            std::lock_guard lock(workerMutex);
            workerStop = true;
        }
        workerCondition.notify_all();
        if (worker.joinable()) worker.join();
        if (menuBinding) gamescope_action_binding_destroy(menuBinding);
        if (toggleBinding) gamescope_action_binding_destroy(toggleBinding);
        if (bindingManager) gamescope_action_binding_manager_destroy(bindingManager);
        if (waylandDisplay) wl_display_disconnect(waylandDisplay);
        if (display && rootWindow)
            XUngrabKey(display, AnyKey, AnyModifier, rootWindow);
        if (cr) cairo_destroy(cr);
        if (presentCr) cairo_destroy(presentCr);
        if (backSurface) cairo_surface_destroy(backSurface);
        if (surface) cairo_surface_destroy(surface);
        if (display) XCloseDisplay(display);
    }
};

} // namespace

int main() {
    std::signal(SIGTERM, requestStop);
    std::signal(SIGINT, requestStop);
    OpenHdrMenu menu;
    if (!menu.initialize()) {
        std::fprintf(stderr, "OpenHDR menu: could not connect to Gamescope's X11/Wayland interfaces\n");
        return 1;
    }
    std::fprintf(stderr, "OpenHDR: Alt+Z menu and Alt+X toggle are ready\n");
    return menu.run();
}
