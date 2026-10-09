// Headless driver for libSpiralBrowserHost.so. It loads the library with dlopen
// exactly as the Editor will, drives the IBrowserSurface it returns through a
// step script, and prints "BROWSER_SMOKE <event> key=value ..." marker lines
// that Scripts/TestBrowserSurface.sh asserts against. No GLFW, RHI, ImGui, or
// libcef is linked into this executable.
#include "BrowserHostExports.h"
#include "BrowserNavigationPolicy.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <unistd.h>

namespace
{
    using Clock = std::chrono::steady_clock;

    const Clock::time_point g_Start = Clock::now();
    volatile std::sig_atomic_t g_Signal = 0;

    double ElapsedMilliseconds()
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - g_Start).count();
    }

    void Marker(const char* event, const char* format = "", ...)
    {
        std::printf("BROWSER_SMOKE %s", event);
        if (format[0] != '\0')
        {
            std::printf(" ");
            va_list arguments;
            va_start(arguments, format);
            std::vprintf(format, arguments);
            va_end(arguments);
        }
        std::printf("\n");
        std::fflush(stdout);
    }

    bool LibcefMapped()
    {
        std::ifstream maps("/proc/self/maps");
        for (std::string line; std::getline(maps, line);)
        {
            if (line.find("libcef.so") != std::string::npos)
                return true;
        }
        return false;
    }

    std::vector<std::string> Split(const std::string& text, char separator)
    {
        std::vector<std::string> parts;
        std::stringstream stream(text);
        for (std::string part; std::getline(stream, part, separator);)
        {
            if (!part.empty())
                parts.push_back(part);
        }
        return parts;
    }

    struct Options
    {
        std::string Library;
        std::string Profile;
        std::string Staging;
        std::vector<std::string> ProviderHosts;
        std::vector<std::string> TestSwitches;
        std::string Steps;
        std::string DumpFrame;
        Engine::u32 MaxFps = 30;
        Engine::u32 StepTimeoutMilliseconds = 30000;
        bool Software = true;
        bool ReinitProbe = false;
    };

    class SmokeListener final : public Fab::IBrowserSurface::Listener
    {
    public:
        void OnFrame(const Fab::BrowserFrameView& frame) override
        {
            const bool resized = frame.Width != m_Width || frame.Height != m_Height;
            if (resized)
            {
                m_Width = frame.Width;
                m_Height = frame.Height;
                m_Pixels.assign(frame.Bgra, frame.Bgra + static_cast<size_t>(frame.StrideBytes) * frame.Height);
            }
            else
            {
                // Only the declared dirty rectangles are copied, so a frame whose
                // Dirty list understates the change leaves a stale pixel behind.
                for (const Fab::BrowserDirtyRect& rect : frame.Dirty)
                {
                    for (int row = std::max(rect.Y, 0); row < std::min<int>(rect.Y + rect.Height, static_cast<int>(m_Height)); ++row)
                    {
                        const int left = std::max(rect.X, 0);
                        const int right = std::min<int>(rect.X + rect.Width, static_cast<int>(m_Width));
                        if (right <= left)
                            continue;
                        const size_t offset = (static_cast<size_t>(row) * m_Width + static_cast<size_t>(left)) * 4u;
                        std::memcpy(m_Pixels.data() + offset, frame.Bgra + offset, static_cast<size_t>(right - left) * 4u);
                    }
                }
            }
            ++m_Frames;
            if (m_Frames == 1)
            {
                Marker("first_frame", "ms=%.0f width=%u height=%u dirty_rects=%zu", ElapsedMilliseconds(), frame.Width,
                    frame.Height, frame.Dirty.size());
            }
        }

        void OnCursor(Fab::BrowserCursor cursor) override { Marker("cursor", "value=%d", static_cast<int>(cursor)); }
        void OnAddress(std::string_view) override { ++m_AddressChanges; }

        void OnLoadState(bool loading, bool canGoBack, bool canGoForward) override
        {
            m_Loading = loading;
            if (loading)
                m_LoadSeen = true;
            else if (m_LoadSeen)
                m_LoadFinished = true;
            Marker("load_state", "loading=%d back=%d forward=%d", loading ? 1 : 0, canGoBack ? 1 : 0, canGoForward ? 1 : 0);
        }

        void OnDownload(const Fab::BrowserDownloadEvent& event) override
        {
            static const char* const names[] = { "started", "progress", "completed", "failed", "cancelled", "blocked" };
            const char* state = names[static_cast<int>(event.Kind)];
            ++m_DownloadCounts[state];
            if (event.Kind == Fab::BrowserDownloadEvent::State::Progress)
                return;
            Marker("download", "state=%s id=%llu name=%s bytes=%llu total=%llu staged=%s", state,
                static_cast<unsigned long long>(event.Id), event.DisplayName.c_str(),
                static_cast<unsigned long long>(event.Bytes), static_cast<unsigned long long>(event.TotalBytes),
                event.StagedPath.string().c_str());
        }

        void OnNavigationDenied(std::string_view host) override
        {
            ++m_Denials;
            Marker("navigation_denied", "host=%.*s", static_cast<int>(host.size()), host.data());
        }

        void OnFailed(std::string_view reason) override
        {
            ++m_Failures;
            Marker("failed", "reason=%.*s", static_cast<int>(reason.size()), reason.data());
        }

        void OnClosed() override
        {
            m_Closed = true;
            Marker("closed");
        }

        bool HasPixel(int x, int y) const
        {
            return x >= 0 && y >= 0 && static_cast<Engine::u32>(x) < m_Width && static_cast<Engine::u32>(y) < m_Height;
        }

        bool PixelMatches(int x, int y, unsigned rgb, int tolerance) const
        {
            if (!HasPixel(x, y))
                return false;
            const Engine::u8* pixel = m_Pixels.data() + (static_cast<size_t>(y) * m_Width + static_cast<size_t>(x)) * 4u;
            const int blue = pixel[0];
            const int green = pixel[1];
            const int red = pixel[2];
            return std::abs(red - static_cast<int>((rgb >> 16) & 0xFF)) <= tolerance
                && std::abs(green - static_cast<int>((rgb >> 8) & 0xFF)) <= tolerance
                && std::abs(blue - static_cast<int>(rgb & 0xFF)) <= tolerance;
        }

        bool WriteFrameAsPpm(const std::string& path) const
        {
            if (m_Pixels.empty())
                return false;
            std::ofstream file(path, std::ios::binary);
            file << "P6\n" << m_Width << " " << m_Height << "\n255\n";
            std::vector<char> row(static_cast<size_t>(m_Width) * 3u);
            for (Engine::u32 y = 0; y < m_Height; ++y)
            {
                const Engine::u8* source = m_Pixels.data() + static_cast<size_t>(y) * m_Width * 4u;
                for (Engine::u32 x = 0; x < m_Width; ++x)
                {
                    row[x * 3u] = static_cast<char>(source[x * 4u + 2]);
                    row[x * 3u + 1] = static_cast<char>(source[x * 4u + 1]);
                    row[x * 3u + 2] = static_cast<char>(source[x * 4u]);
                }
                file.write(row.data(), static_cast<std::streamsize>(row.size()));
            }
            return static_cast<bool>(file);
        }

        Engine::u32 Width() const { return m_Width; }
        Engine::u32 Height() const { return m_Height; }
        Engine::u64 Frames() const { return m_Frames; }
        size_t Denials() const { return m_Denials; }
        size_t Failures() const { return m_Failures; }
        bool Closed() const { return m_Closed; }
        bool LoadFinished() const { return m_LoadFinished; }
        // A load already in flight (the initial about:blank) is replaced by the
        // new navigation and reports one final loading=0, so it counts as seen.
        void ResetLoad()
        {
            m_LoadSeen = m_Loading;
            m_LoadFinished = false;
        }
        size_t DownloadCount(const std::string& state) const
        {
            const auto found = m_DownloadCounts.find(state);
            return found == m_DownloadCounts.end() ? 0 : found->second;
        }

    private:
        std::vector<Engine::u8> m_Pixels;
        Engine::u32 m_Width = 0;
        Engine::u32 m_Height = 0;
        Engine::u64 m_Frames = 0;
        size_t m_AddressChanges = 0;
        size_t m_Denials = 0;
        size_t m_Failures = 0;
        bool m_Closed = false;
        bool m_Loading = false;
        bool m_LoadSeen = false;
        bool m_LoadFinished = false;
        std::map<std::string, size_t> m_DownloadCounts;
    };

    struct KeyName
    {
        const char* Name;
        int GlfwKey;
        int VirtualKey;
    };

    constexpr KeyName kKeys[] = {
        { "enter", 257, 0x0D }, { "backspace", 259, 0x08 }, { "tab", 258, 0x09 }, { "escape", 256, 0x1B },
        { "left", 263, 0x25 }, { "right", 262, 0x27 }, { "delete", 261, 0x2E },
    };

    // Runs the step script one step at a time from the pump loop.
    class StepRunner
    {
    public:
        StepRunner(Fab::IBrowserSurface& surface, SmokeListener& listener, std::vector<std::string> steps, Engine::u32 timeoutMilliseconds)
            : m_Surface(surface)
            , m_Listener(listener)
            , m_Steps(std::move(steps))
            , m_TimeoutMilliseconds(timeoutMilliseconds)
        {
        }

        bool Finished() const { return m_Index >= m_Steps.size(); }
        bool Failed() const { return m_Failed; }

        void Tick()
        {
            if (Finished() || m_Failed)
                return;
            const std::string& step = m_Steps[m_Index];
            const size_t colon = step.find(':');
            const std::string name = step.substr(0, colon);
            const std::string argument = colon == std::string::npos ? std::string() : step.substr(colon + 1);
            if (!m_Entered)
            {
                m_Entered = true;
                m_StepStart = ElapsedMilliseconds();
                m_DownloadBaseline = 0;
                if (name == "wait-download")
                    m_DownloadBaseline = m_Listener.DownloadCount(Split(argument, ',').front());
            }
            const bool timedOut = ElapsedMilliseconds() - m_StepStart > m_TimeoutMilliseconds;
            bool done = false;
            if (!Execute(name, argument, done) || (!done && timedOut && name != "hold"))
            {
                Marker("step_failed", "step=%s reason=%s", step.c_str(), timedOut && !done ? "timeout" : "rejected");
                m_Failed = true;
                return;
            }
            if (done)
            {
                ++m_Index;
                m_Entered = false;
            }
        }

    private:
        bool Execute(const std::string& name, const std::string& argument, bool& done)
        {
            done = true;
            if (name == "navigate")
            {
                m_Listener.ResetLoad();
                m_Surface.Navigate(argument);
            }
            else if (name == "wait-load")
            {
                done = m_Listener.LoadFinished();
            }
            else if (name == "wait-ms")
            {
                done = ElapsedMilliseconds() - m_StepStart >= std::atof(argument.c_str());
            }
            else if (name == "view")
            {
                unsigned width = 0;
                unsigned height = 0;
                float scale = 1.0f;
                if (std::sscanf(argument.c_str(), "%ux%u@%f", &width, &height, &scale) != 3)
                    return false;
                m_Surface.SetViewSize(Fab::BrowserViewSize { width, height, scale });
            }
            else if (name == "visible")
            {
                m_Surface.SetVisible(argument == "1");
            }
            else if (name == "focus")
            {
                m_Surface.SetFocus(argument == "1");
            }
            else if (name == "expect-pixel")
            {
                int x = 0;
                int y = 0;
                unsigned rgb = 0;
                if (std::sscanf(argument.c_str(), "%d,%d,%x", &x, &y, &rgb) != 3)
                    return false;
                done = m_Listener.PixelMatches(x, y, rgb, 4);
                if (done)
                    Marker("pixel_ok", "x=%d y=%d rgb=%06x", x, y, rgb);
            }
            else if (name == "expect-not-pixel")
            {
                int x = 0;
                int y = 0;
                unsigned rgb = 0;
                if (std::sscanf(argument.c_str(), "%d,%d,%x", &x, &y, &rgb) != 3)
                    return false;
                done = m_Listener.HasPixel(x, y) && !m_Listener.PixelMatches(x, y, rgb, 4);
                if (done)
                    Marker("pixel_differs", "x=%d y=%d rgb=%06x", x, y, rgb);
            }
            else if (name == "expect-size")
            {
                unsigned width = 0;
                unsigned height = 0;
                if (std::sscanf(argument.c_str(), "%ux%u", &width, &height) != 2)
                    return false;
                done = m_Listener.Width() == width && m_Listener.Height() == height;
                if (done)
                    Marker("size_ok", "width=%u height=%u", width, height);
            }
            else if (name == "down" || name == "up")
            {
                float x = 0;
                float y = 0;
                if (std::sscanf(argument.c_str(), "%f,%f", &x, &y) != 2)
                    return false;
                m_Surface.SendMouseButton(Fab::BrowserMouse { x, y, name == "down" ? 0u : Fab::BrowserModifier::LeftButton },
                    Fab::BrowserMouseButton::Left, name == "down", 1);
            }
            else if (name == "click" || name == "move")
            {
                float x = 0;
                float y = 0;
                if (std::sscanf(argument.c_str(), "%f,%f", &x, &y) != 2)
                    return false;
                Fab::BrowserMouse mouse { x, y, 0 };
                m_Surface.SendMouseMove(mouse, false);
                if (name == "click")
                {
                    m_Surface.SendMouseButton(mouse, Fab::BrowserMouseButton::Left, true, 1);
                    mouse.Modifiers = Fab::BrowserModifier::LeftButton;
                    m_Surface.SendMouseButton(mouse, Fab::BrowserMouseButton::Left, false, 1);
                }
            }
            else if (name == "wheel")
            {
                float x = 0;
                float y = 0;
                float dx = 0;
                float dy = 0;
                if (std::sscanf(argument.c_str(), "%f,%f,%f,%f", &x, &y, &dx, &dy) != 4)
                    return false;
                m_Surface.SendMouseWheel(Fab::BrowserMouse { x, y, 0 }, dx, dy);
            }
            else if (name == "type")
            {
                for (const char character : argument)
                {
                    const int upper = std::toupper(static_cast<unsigned char>(character));
                    SendKey(Fab::BrowserKey::Phase::Down, upper, upper, 0, false);
                    Fab::BrowserKey text;
                    text.Kind = Fab::BrowserKey::Phase::Char;
                    text.Codepoint = static_cast<char32_t>(static_cast<unsigned char>(character));
                    m_Surface.SendKey(text);
                    SendKey(Fab::BrowserKey::Phase::Up, upper, upper, 0, false);
                }
            }
            else if (name == "char")
            {
                Fab::BrowserKey text;
                text.Kind = Fab::BrowserKey::Phase::Char;
                text.Codepoint = static_cast<char32_t>(std::strtoul(argument.c_str(), nullptr, 10));
                m_Surface.SendKey(text);
            }
            else if (name == "frames")
            {
                Marker("frames", "name=%s count=%llu ms=%.0f", argument.c_str(),
                    static_cast<unsigned long long>(m_Listener.Frames()), ElapsedMilliseconds());
            }
            else if (name == "press")
            {
                const KeyName* key = FindKey(argument);
                if (key == nullptr)
                    return false;
                SendKey(Fab::BrowserKey::Phase::Down, key->GlfwKey, key->VirtualKey, 0, false);
                SendKey(Fab::BrowserKey::Phase::Up, key->GlfwKey, key->VirtualKey, 0, false);
            }
            else if (name == "chord")
            {
                // "ctrl+a": Control down, the letter down and up with Control held, Control up.
                if (argument.size() != 6 || argument.compare(0, 5, "ctrl+") != 0)
                    return false;
                const int letter = std::toupper(static_cast<unsigned char>(argument[5]));
                SendKey(Fab::BrowserKey::Phase::Down, 341, 0x11, Fab::BrowserModifier::Control, false);
                SendKey(Fab::BrowserKey::Phase::Down, letter, letter, Fab::BrowserModifier::Control, false);
                SendKey(Fab::BrowserKey::Phase::Up, letter, letter, Fab::BrowserModifier::Control, false);
                SendKey(Fab::BrowserKey::Phase::Up, 341, 0x11, 0, false);
            }
            else if (name == "wait-download")
            {
                done = m_Listener.DownloadCount(Split(argument, ',').front()) > m_DownloadBaseline;
            }
            else if (name == "wait-denials")
            {
                done = m_Listener.Denials() >= static_cast<size_t>(std::atoi(argument.c_str()));
            }
            else if (name == "wait-failures")
            {
                done = m_Listener.Failures() >= static_cast<size_t>(std::atoi(argument.c_str()));
            }
            else if (name == "signout")
            {
                m_Surface.ClearBrowsingData();
                Marker("signout_requested");
            }
            else if (name == "marker")
            {
                Marker("marker", "name=%s pid=%d", argument.c_str(), static_cast<int>(getpid()));
            }
            else if (name == "hold")
            {
                done = false;
            }
            else
            {
                return false;
            }
            return true;
        }

        const KeyName* FindKey(const std::string& name) const
        {
            for (const KeyName& key : kKeys)
            {
                if (name == key.Name)
                    return &key;
            }
            return nullptr;
        }

        void SendKey(Fab::BrowserKey::Phase phase, int glfwKey, int virtualKey, Engine::u32 modifiers, bool repeat)
        {
            Fab::BrowserKey key;
            key.Kind = phase;
            key.GlfwKey = glfwKey;
            key.WindowsVirtualKey = virtualKey;
            key.Modifiers = modifiers;
            key.Repeat = repeat;
            m_Surface.SendKey(key);
        }

        Fab::IBrowserSurface& m_Surface;
        SmokeListener& m_Listener;
        std::vector<std::string> m_Steps;
        Engine::u32 m_TimeoutMilliseconds;
        size_t m_Index = 0;
        bool m_Entered = false;
        bool m_Failed = false;
        double m_StepStart = 0.0;
        size_t m_DownloadBaseline = 0;
    };

    bool ParseOptions(int argc, char** argv, Options& options)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string argument = argv[i];
            const size_t equals = argument.find('=');
            const std::string key = argument.substr(0, equals);
            const std::string value = equals == std::string::npos ? std::string() : argument.substr(equals + 1);
            if (key == "--lib")
                options.Library = value;
            else if (key == "--profile")
                options.Profile = value;
            else if (key == "--staging")
                options.Staging = value;
            else if (key == "--provider-host")
                options.ProviderHosts.push_back(value);
            else if (key == "--test-switch")
                options.TestSwitches.push_back(value);
            else if (key == "--steps")
                options.Steps = value;
            else if (key == "--dump-frame")
                options.DumpFrame = value;
            else if (key == "--max-fps")
                options.MaxFps = static_cast<Engine::u32>(std::atoi(value.c_str()));
            else if (key == "--step-timeout-ms")
                options.StepTimeoutMilliseconds = static_cast<Engine::u32>(std::atoi(value.c_str()));
            else if (key == "--hardware-rendering")
                options.Software = false;
            else if (key == "--reinit-probe")
                options.ReinitProbe = true;
            else
                return false;
        }
        return !options.Library.empty() && !options.Profile.empty() && !options.Staging.empty();
    }

    void OnSignal(int)
    {
        g_Signal = 1;
    }
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    Options options;
    if (!ParseOptions(argc, argv, options))
    {
        std::fprintf(stderr,
            "usage: SpiralBrowserSmoke --lib=<libSpiralBrowserHost.so> --profile=<abs dir> --staging=<abs dir> "
            "[--provider-host=<host>]... [--test-switch=<name=value>]... [--steps=<step;step>] [--dump-frame=<ppm>] "
            "[--max-fps=N] [--step-timeout-ms=N] [--hardware-rendering] [--reinit-probe]\n");
        return 2;
    }
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGINT, OnSignal);

    Marker("start", "pid=%d libcef_mapped=%d", static_cast<int>(getpid()), LibcefMapped() ? 1 : 0);
    void* library = dlopen(options.Library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
    {
        Marker("load_failed", "error=%s", dlerror());
        return 3;
    }
    Marker("library_loaded", "libcef_mapped=%d", LibcefMapped() ? 1 : 0);

    Fab::IBrowserSurface* surface = nullptr;
    const auto destroy = reinterpret_cast<Fab::BrowserHostDestroyFunction>(dlsym(library, Fab::kBrowserHostDestroySymbol));
    if (options.TestSwitches.empty())
    {
        const auto create = reinterpret_cast<Fab::BrowserHostCreateFunction>(dlsym(library, Fab::kBrowserHostCreateSymbol));
        if (create != nullptr)
            surface = create(Fab::kBrowserHostAbiVersion, static_cast<Engine::u32>(sizeof(Fab::BrowserSurfaceConfig)));
    }
    else
    {
        std::string switches;
        for (const std::string& entry : options.TestSwitches)
            switches += entry + "\n";
        const auto create =
            reinterpret_cast<Fab::BrowserHostCreateForTestFunction>(dlsym(library, Fab::kBrowserHostCreateForTestSymbol));
        if (create != nullptr)
        {
            surface = create(
                Fab::kBrowserHostAbiVersion, static_cast<Engine::u32>(sizeof(Fab::BrowserSurfaceConfig)), switches.c_str());
        }
    }
    if (surface == nullptr || destroy == nullptr)
    {
        Marker("create_failed", "reason=symbol-or-abi-mismatch");
        return 3;
    }
    // A mismatched config size must be refused by the library, not crash it.
    const auto probeCreate = reinterpret_cast<Fab::BrowserHostCreateFunction>(dlsym(library, Fab::kBrowserHostCreateSymbol));
    Marker("abi_mismatch_refused", "value=%d",
        probeCreate(Fab::kBrowserHostAbiVersion, static_cast<Engine::u32>(sizeof(Fab::BrowserSurfaceConfig)) + 1) == nullptr ? 1 : 0);

    Fab::BrowserSurfaceConfig config;
    config.ProfileDir = options.Profile;
    config.DownloadStagingDir = options.Staging;
    config.MaxFps = options.MaxFps;
    config.SoftwareRendering = options.Software;
    for (const std::string& host : options.ProviderHosts)
    {
        std::string error;
        if (!config.Navigation.AddProviderHost(host, error))
        {
            Marker("usage_error", "reason=%s", error.c_str());
            destroy(surface);
            return 2;
        }
    }

    SmokeListener listener;
    std::string error;
    if (!surface->Initialize(config, listener, error))
    {
        Marker("init_failed", "error=%s libcef_mapped=%d", error.c_str(), LibcefMapped() ? 1 : 0);
        destroy(surface);
        return 3;
    }
    Marker("initialized", "backend=%.*s libcef_mapped=%d", static_cast<int>(surface->BackendName().size()),
        surface->BackendName().data(), LibcefMapped() ? 1 : 0);
    surface->SetViewSize(Fab::BrowserViewSize { 640, 360, 1.0f });

    StepRunner steps(*surface, listener, Split(options.Steps, ';'), options.StepTimeoutMilliseconds);
    while (!steps.Finished() && !steps.Failed() && g_Signal == 0 && !listener.Closed())
    {
        surface->Pump();
        steps.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    if (g_Signal != 0)
        Marker("signal_received");
    if (listener.Closed() && !steps.Finished())
        Marker("closed_early");

    if (!options.DumpFrame.empty())
        Marker("frame_dump", "written=%d", listener.WriteFrameAsPpm(options.DumpFrame) ? 1 : 0);
    Marker("summary", "frames=%llu width=%u height=%u denials=%zu failures=%zu", static_cast<unsigned long long>(listener.Frames()),
        listener.Width(), listener.Height(), listener.Denials(), listener.Failures());

    const double closeStart = ElapsedMilliseconds();
    surface->RequestClose();
    while (!surface->IsClosed() && ElapsedMilliseconds() - closeStart < 15000.0)
    {
        surface->Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    surface->Pump();
    const double shutdownStart = ElapsedMilliseconds();
    surface->Shutdown(5000);
    Marker("shutdown_done", "close_ms=%.0f shutdown_ms=%.0f", shutdownStart - closeStart, ElapsedMilliseconds() - shutdownStart);
    destroy(surface);

    if (options.ReinitProbe)
    {
        // CEF cannot be initialized twice in one process (the second CefInitialize
        // crashes), so a new surface must refuse instead of reaching it.
        const auto create = reinterpret_cast<Fab::BrowserHostCreateFunction>(dlsym(library, Fab::kBrowserHostCreateSymbol));
        Fab::IBrowserSurface* second =
            create(Fab::kBrowserHostAbiVersion, static_cast<Engine::u32>(sizeof(Fab::BrowserSurfaceConfig)));
        std::string secondError;
        SmokeListener secondListener;
        const bool initialized = second != nullptr && second->Initialize(config, secondListener, secondError);
        Marker("second_initialize", "accepted=%d error=%s", initialized ? 1 : 0, secondError.c_str());
        if (second != nullptr)
            destroy(second);
    }
    return steps.Failed() ? 1 : 0;
}
