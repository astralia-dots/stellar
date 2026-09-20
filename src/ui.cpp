#include "ui.h"

#include "binds.h"
#include "text.h"
#include "watch.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <EGL/egl.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <poll.h>

namespace {

// `rgba(20, 20, 30, 0.85)` background (premultiplied by hand), `#f0f0f5` text,
// rows padded `4px 12px`
constexpr float BG_ALPHA = 0.85f;
constexpr float BG_R = 20.f / 255 * BG_ALPHA, BG_G = 20.f / 255 * BG_ALPHA, BG_B = 30.f / 255 * BG_ALPHA;
constexpr float FG_R = 0xf0 / 255.f, FG_G = 0xf0 / 255.f, FG_B = 0xf5 / 255.f;
constexpr int PAD_X = 12, PAD_Y = 4;
constexpr int WIDTH = 720, HEIGHT = 480;     // logical px; the compositor centres the surface
constexpr double SCROLL_FACTOR = 2.0;        // wheel notch is ~15 units, so ~1.3 rows per notch

class Ui {
public:
    int run();

private:
    void init_egl();
    void reload();
    void draw();
    void on_configure(uint32_t serial, int w, int h);
    void set_scale(int scale);

    wl_display* display_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    zwlr_layer_shell_v1* shell_ = nullptr;
    wl_surface* surface_ = nullptr;
    zwlr_layer_surface_v1* layer_ = nullptr;
    wl_egl_window* window_ = nullptr;
    wl_seat* seat_ = nullptr;
    wl_pointer* pointer_ = nullptr;

    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLConfig egl_config_ = nullptr;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    EGLSurface egl_surface_ = EGL_NO_SURFACE;

    std::unique_ptr<Text> text_; // needs a current GL context, so created on first configure
    ConfigWatcher watcher_;
    std::vector<std::string> lines_;
    int width_ = 0, height_ = 0, scale_ = 1; // logical size, integer buffer scale
    double scroll_ = 0;                      // logical px scrolled down; clamped in `draw`
    bool running_ = true, dirty_ = false;
};

void Ui::init_egl() {
    egl_display_ = eglGetDisplay(EGLNativeDisplayType(display_));
    if (egl_display_ == EGL_NO_DISPLAY || !eglInitialize(egl_display_, nullptr, nullptr))
        throw std::runtime_error("EGL initialisation failed");
    eglBindAPI(EGL_OPENGL_ES_API);
    // needs an alpha channel for the translucent background
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                            EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(egl_display_, attrs, &egl_config_, 1, &n) || n < 1)
        throw std::runtime_error("no suitable EGL config");
    const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, ctx_attrs);
    if (egl_context_ == EGL_NO_CONTEXT)
        throw std::runtime_error("failed to create an OpenGL ES 2.0 context");
}

void Ui::reload() {
    lines_.clear();
    auto binds = fetch();
    if (binds) {
        lines_ = format_binds(*binds);
    } else {
        // errors can span lines (`hyprctl` stderr): one row each
        const std::string msg = "stellar: " + binds.error();
        for (size_t i = 0; i < msg.size();) {
            size_t nl = std::min(msg.find('\n', i), msg.size());
            if (nl > i)
                lines_.push_back(msg.substr(i, nl - i));
            i = nl + 1;
        }
    }
    dirty_ = true;
}

void Ui::set_scale(int scale) {
    if (scale == scale_)
        return;
    scale_ = scale;
    wl_surface_set_buffer_scale(surface_, scale_); // takes effect on the next commit, the swap in `draw`
    if (window_) {
        wl_egl_window_resize(window_, width_ * scale_, height_ * scale_, 0, 0);
        text_->set_scale(scale_);
        dirty_ = true;
    }
}

void Ui::on_configure(uint32_t serial, int w, int h) {
    zwlr_layer_surface_v1_ack_configure(layer_, serial);
    if (w <= 0 || h <= 0)
        return;
    width_ = w;
    height_ = h;
    if (!window_) {
        window_ = wl_egl_window_create(surface_, w * scale_, h * scale_);
        egl_surface_ = eglCreateWindowSurface(egl_display_, egl_config_,
                                              EGLNativeWindowType(window_), nullptr);
        if (egl_surface_ == EGL_NO_SURFACE ||
            !eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_))
            throw std::runtime_error("failed to create the EGL window surface");
        // swaps are on demand: never wait for a frame callback
        eglSwapInterval(egl_display_, 0);
        text_ = std::make_unique<Text>();
        text_->set_scale(scale_);
    } else {
        wl_egl_window_resize(window_, w * scale_, h * scale_, 0, 0);
    }
    dirty_ = true;
}

void Ui::draw() {
    const int w = width_ * scale_, h = height_ * scale_;
    glViewport(0, 0, w, h);
    glClearColor(BG_R, BG_G, BG_B, BG_ALPHA);
    glClear(GL_COLOR_BUFFER_BIT);

    text_->begin(w, h);
    const int row = text_->line_height() + 2 * PAD_Y * scale_;
    // clamp here so a reload that shortens the list can't leave the view past the end
    const int content = row * static_cast<int>(lines_.size());
    scroll_ = std::clamp(scroll_, 0.0, static_cast<double>(std::max(0, content - h)) / scale_);
    int y = -static_cast<int>(std::lround(scroll_ * scale_));
    for (const std::string& line : lines_) {
        if (y >= h)
            break; // the rest is below the surface
        if (y + row > 0)
            text_->add(line, static_cast<float>(PAD_X * scale_), static_cast<float>(y + PAD_Y * scale_));
        y += row;
    }
    text_->flush(FG_R, FG_G, FG_B);
    eglSwapBuffers(egl_display_, egl_surface_);
    dirty_ = false;
}

int Ui::run() {
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        std::fprintf(stderr, "stellar: cannot connect to the Wayland display\n");
        return 1;
    }

    // only the wheel matters; libwayland needs every dispatched slot non-null
    static const wl_pointer_listener pointer_listener = {
        .enter = [](void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {},
        .leave = [](void*, wl_pointer*, uint32_t, wl_surface*) {},
        .motion = [](void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {},
        .button = [](void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t) {},
        .axis = [](void* d, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
            if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
                return;
            auto* self = static_cast<Ui*>(d);
            self->scroll_ += wl_fixed_to_double(value) * SCROLL_FACTOR;
            self->dirty_ = true;
        },
        .frame = [](void*, wl_pointer*) {},
        .axis_source = [](void*, wl_pointer*, uint32_t) {},
        .axis_stop = [](void*, wl_pointer*, uint32_t, uint32_t) {},
        .axis_discrete = [](void*, wl_pointer*, uint32_t, int32_t) {},
        // newer than the bound v5, so never sent; listed for -Wmissing-field-initializers
        .axis_value120 = [](void*, wl_pointer*, uint32_t, int32_t) {},
        .axis_relative_direction = [](void*, wl_pointer*, uint32_t, uint32_t) {},
        .warp = [](void*, wl_pointer*, wl_fixed_t, wl_fixed_t) {},
    };
    static const wl_seat_listener seat_listener = {
        .capabilities = [](void* d, wl_seat* seat, uint32_t caps) {
            auto* self = static_cast<Ui*>(d);
            const bool has_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
            if (has_pointer && !self->pointer_) {
                self->pointer_ = wl_seat_get_pointer(seat);
                wl_pointer_add_listener(self->pointer_, &pointer_listener, self);
            } else if (!has_pointer && self->pointer_) {
                wl_pointer_release(self->pointer_);
                self->pointer_ = nullptr;
            }
        },
        .name = [](void*, wl_seat*, const char*) {},
    };
    static const wl_registry_listener registry_listener = {
        .global = [](void* d, wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
            auto* self = static_cast<Ui*>(d);
            if (!std::strcmp(iface, wl_compositor_interface.name)) {
                // v6 adds `preferred_buffer_scale`; older compositors stay at scale 1
                self->compositor_ = static_cast<wl_compositor*>(wl_registry_bind(
                    reg, name, &wl_compositor_interface, std::min(ver, 6u)));
            } else if (!std::strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
                self->shell_ = static_cast<zwlr_layer_shell_v1*>(
                    wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1));
            } else if (!std::strcmp(iface, wl_seat_interface.name) && !self->seat_) {
                // `pointer_listener` is written for v5
                self->seat_ = static_cast<wl_seat*>(
                    wl_registry_bind(reg, name, &wl_seat_interface, std::min(ver, 5u)));
                wl_seat_add_listener(self->seat_, &seat_listener, self);
            }
        },
        .global_remove = [](void*, wl_registry*, uint32_t) {},
    };
    wl_registry_add_listener(wl_display_get_registry(display_), &registry_listener, this);
    wl_display_roundtrip(display_);
    if (!compositor_ || !shell_) {
        std::fprintf(stderr, "stellar: the compositor lacks `wl_compositor` or `zwlr_layer_shell_v1`\n");
        return 1;
    }

    init_egl();
    reload();

    static const wl_surface_listener surface_listener = {
        .enter = [](void*, wl_surface*, wl_output*) {},
        .leave = [](void*, wl_surface*, wl_output*) {},
        .preferred_buffer_scale = [](void* d, wl_surface*, int32_t s) { static_cast<Ui*>(d)->set_scale(s); },
        .preferred_buffer_transform = [](void*, wl_surface*, uint32_t) {},
    };
    static const zwlr_layer_surface_v1_listener layer_listener = {
        .configure = [](void* d, zwlr_layer_surface_v1*, uint32_t serial, uint32_t w, uint32_t h) {
            static_cast<Ui*>(d)->on_configure(serial, static_cast<int>(w), static_cast<int>(h));
        },
        .closed = [](void* d, zwlr_layer_surface_v1*) { static_cast<Ui*>(d)->running_ = false; },
    };

    surface_ = wl_compositor_create_surface(compositor_);
    wl_surface_add_listener(surface_, &surface_listener, this);
    layer_ = zwlr_layer_shell_v1_get_layer_surface(shell_, surface_, nullptr,
                                                   ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "stellar");
    zwlr_layer_surface_v1_add_listener(layer_, &layer_listener, this);
    // no anchors, so the compositor centres it; zone -1 ignores other surfaces' exclusive zones
    zwlr_layer_surface_v1_set_size(layer_, WIDTH, HEIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(surface_); // no buffer yet: requests the first configure

    // no teardown: the process exits right after, and SIGTERM skips destructors anyway
    while (running_) {
        while (wl_display_prepare_read(display_) != 0)
            wl_display_dispatch_pending(display_);
        if (wl_display_flush(display_) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display_);
            return 1;
        }
        pollfd fds[] = {{wl_display_get_fd(display_), POLLIN, 0}, {watcher_.fd(), POLLIN, 0}};
        const int r = poll(fds, 2, -1);
        if (r > 0 && fds[0].revents) {
            if (wl_display_read_events(display_) < 0)
                return 1;
        } else {
            wl_display_cancel_read(display_);
            if (r < 0 && errno != EINTR)
                return 1;
        }
        if (wl_display_dispatch_pending(display_) < 0)
            return 1;
        // one reload per burst of inotify events
        if (r > 0 && fds[1].revents & POLLIN && watcher_.drain())
            reload();
        if (dirty_ && window_)
            draw();
    }
    return 0;
}

} // namespace

int run_ui() {
    try {
        return Ui().run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stellar: %s\n", e.what());
        return 1;
    }
}
