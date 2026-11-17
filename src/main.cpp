// minimo-genie: macOS-like genie for Hyprland 0.57.2 (Minimo OS).
//   open            the window flows out of its dock icon      (automatic)
//   close           it flows into its icon                      (automatic)
//   hyprctl genie out <0xaddress>   flows into the dock; call it right before the window is moved
//                                    to a special workspace (minimise). Moved back to a normal
//                                    workspace later, it flows out again (restore, automatic).
// Icon positions: $XDG_RUNTIME_DIR/minimo-genie, lines "<0xaddress|class> <x> <y>" (global logical px),
// written by Minimo's dock; missing → the bottom centre of the monitor.

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowEffectsController.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/render/transformer/TransformerList.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/ipc/s1/S1.hpp>
#include <hyprland/src/helpers/DeformableMesh.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>

#include "genie_math.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

using namespace Render;
using Clock = std::chrono::steady_clock;

static HANDLE                 PHANDLE = nullptr;
static constexpr double       DURATION_OUT = 0.50, DURATION_IN = 0.42; // seconds
static constexpr double       ICON_W       = 42.0;                      // logical px, the dock's icon
static constexpr size_t       COLS = 8, ROWS = 40;

struct SAnim {
    PHLWINDOWREF        window;
    PHLMONITORREF       monitor;
    bool                out = true;
    Clock::time_point   start;
    genie::Pt           target; // global logical
    // out only: the last picture of the window and where it was (global logical)
    SP<IFramebuffer>    snapshot;
    CBox                box;
    double progress() const {
        const double T = std::chrono::duration<double>(Clock::now() - start).count() / (out ? DURATION_OUT : DURATION_IN);
        return std::clamp(T, 0.0, 1.0);
    }
    // 0 = window as is, 1 = in the icon
    double s() const {
        const double t = genie::smoothstep(0, 1, progress());
        return out ? t : 1.0 - t;
    }
};

static std::vector<SAnim>                     g_outs;     // close / minimise, drawn from a snapshot
static std::unordered_map<void*, SAnim>       g_ins;      // open / restore, drawn by the window's transformer
static std::vector<PHLWINDOWREF>              g_minimised; // put away by `genie out`, to flow back out
static std::vector<CHyprSignalListener>       g_listeners;
static SP<IPC::Socket1::SCommand>             g_cmd;

static std::string addressOf(PHLWINDOW w) {
    return std::format("0x{:x}", rc<uintptr_t>(w.get()));
}

static genie::Pt targetFor(PHLWINDOW w, PHLMONITOR mon) {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    std::string cls = w->metadata().appID();
    std::ranges::transform(cls, cls.begin(), ::tolower);
    std::optional<genie::Pt> byClass;
    if (rt) {
        std::ifstream f(std::string{rt} + "/minimo-genie");
        std::string   line;
        const auto    ADDR = addressOf(w);
        while (std::getline(f, line)) {
            std::istringstream is(line);
            std::string        key;
            double             x = 0, y = 0;
            if (!(is >> key >> x >> y))
                continue;
            if (key == ADDR)
                return {x, y};
            if (key == cls && !byClass)
                byClass = genie::Pt{x, y};
        }
    }
    if (byClass)
        return *byClass;
    // shortcut: no dock data → the bottom centre, where Minimo's dock sits (its icons' centre is 49 px up)
    return {mon->m_position.x + mon->m_size.x / 2.0, mon->m_position.y + mon->m_size.y - 49.0};
}

// The mesh, in pixels of `space` (a box the vertices are normalised to), texture coords from `texOrigin`/`texSize`.
static std::vector<SMeshRenderVertex> genieMesh(const CBox& winPx, genie::Pt targetPx, double iconPx, double s, const CBox& space, const Vector2D& texOrigin,
                                                const Vector2D& texSize) {
    std::vector<SMeshRenderVertex> v;
    v.reserve(COLS * ROWS * 6);
    const auto VERT = [&](size_t c, size_t r) -> SMeshRenderVertex {
        const double U = sc<double>(c) / COLS, V = sc<double>(r) / ROWS;
        const auto   P = genie::point(winPx.x, winPx.y, winPx.w, winPx.h, targetPx, iconPx, s, U, V);
        return {
            .x = sc<float>((P.x - space.x) / space.w),
            .y = sc<float>((P.y - space.y) / space.h),
            .u = sc<float>((winPx.x + U * winPx.w - texOrigin.x) / texSize.x),
            .v = sc<float>((winPx.y + V * winPx.h - texOrigin.y) / texSize.y),
        };
    };
    for (size_t r = 0; r < ROWS; ++r)
        for (size_t c = 0; c < COLS; ++c) {
            v.push_back(VERT(c, r));
            v.push_back(VERT(c, r + 1));
            v.push_back(VERT(c + 1, r));
            v.push_back(VERT(c + 1, r));
            v.push_back(VERT(c, r + 1));
            v.push_back(VERT(c + 1, r + 1));
        }
    return v;
}

static float alphaFor(double s) {
    return sc<float>(std::clamp((1.0 - s) / 0.12, 0.0, 1.0)); // fades only in the last bit, inside the icon
}

// ── open / restore: a transformer on the live window ──
class CGenieTransformer : public IWindowTransformer {
  public:
    explicit CGenieTransformer(PHLWINDOWREF w) : m_window(w) {}

    const SAnim* anim() const {
        const auto it = g_ins.find(m_window.get());
        return it == g_ins.end() ? nullptr : &it->second;
    }

    bool active() const override {
        return anim() != nullptr;
    }
    int priority() const override {
        return 20;
    }
    bool blocksDirectScanout() const override {
        return active();
    }

    // monitor-local logical
    CBox transformedExtents(const CBox& currentBox) const override {
        const auto A   = anim();
        const auto MON = A ? A->monitor.lock() : nullptr;
        if (!A || !MON)
            return currentBox;
        const double tx = A->target.x - MON->m_position.x, ty = A->target.y - MON->m_position.y;
        const double x1 = std::min(currentBox.x, tx - ICON_W), y1 = std::min(currentBox.y, ty - ICON_W);
        const double x2 = std::max(currentBox.x + currentBox.w, tx + ICON_W), y2 = std::max(currentBox.y + currentBox.h, ty + ICON_W);
        return {x1, y1, x2 - x1, y2 - y1};
    }
    CBox sourceBoxForOutput(const CBox&, const CBox& inputBox) const override {
        return inputBox; // every part of the window can land anywhere
    }

    SWindowTransformBuffer transform(CRenderContext& ctx, const SWindowTransformBuffer& in, const SWindowTransformContext& context) override {
        const auto A = anim();
        if (!A || context.standalone || context.renderingSnapshot || !context.monitor || !in.framebuffer || !in.framebuffer->getTexture())
            return in;

        const auto   MON    = context.monitor.lock();
        const double SCALE  = MON->m_scale;
        const auto   CANVAS = pixelBoxForLogical(context.outputBox.copy().expand(1.0 / SCALE), SCALE);
        if (CANVAS.empty())
            return in;
        const auto OUT = g_pHyprRenderer->getWorkBuffer(ctx, CANVAS.size());
        if (!OUT)
            return {.framebuffer = in.framebuffer, .box = in.box, .success = false};

        const CBox      WIN   = context.currentBox.copy().scale(SCALE);
        const CBox      SPACE = transformedExtents(context.currentBox).scale(SCALE);
        const genie::Pt TGT{(A->target.x - MON->m_position.x) * SCALE, (A->target.y - MON->m_position.y) * SCALE};
        const auto      VERTS = genieMesh(WIN, TGT, ICON_W * SCALE, A->s(), SPACE, in.box.pos(), in.framebuffer->getTexture()->m_size);

        GL::CFramebufferBindingGuard bindings{g_pHyprRenderer->glBackend()};
        auto                         state = ctx.saveDrawState();
        auto&                        rd    = ctx.m_data;
        g_pHyprRenderer->bindFB(ctx, OUT);
        rd.damage = CRegion{0, 0, sc<int>(CANVAS.w), sc<int>(CANVAS.h)};
        rd.fbSize = CANVAS.size();
        g_pHyprRenderer->setProjectionType(ctx, RPT_EXPORT);
        g_pHyprRenderer->draw(ctx, CClearPassElement::SClearData{CHyprColor(0, 0, 0, 0)});
        GL::g_pHyprOpenGL->renderTextureMesh(ctx, in.framebuffer->getTexture(), SPACE.copy().translate(-CANVAS.pos()), VERTS,
                                             GL::CHyprOpenGLImpl::STextureRenderData{.damage = &rd.damage, .a = alphaFor(A->s()), .allowCustomUV = true});
        return {.framebuffer = OUT, .box = CANVAS};
    }

  private:
    PHLWINDOWREF m_window;
};

// ── close / minimise: the snapshot, drawn over the windows ──
class CGenieOutElement : public IPassElement {
  public:
    explicit CGenieOutElement(const SAnim& a) : m_anim(a) {}

    std::vector<UP<IPassElement>> draw(CRenderContext& ctx) override {
        const auto MON = ctx.m_data.pMonitor.lock();
        const auto TEX = m_anim.snapshot ? m_anim.snapshot->getTexture() : nullptr;
        if (!MON || !TEX)
            return {};
        const double    S = MON->m_scale;
        const CBox      WIN{(m_anim.box.x - MON->m_position.x) * S, (m_anim.box.y - MON->m_position.y) * S, m_anim.box.w * S, m_anim.box.h * S};
        const CBox      SPACE{0, 0, MON->m_transformedSize.x, MON->m_transformedSize.y};
        const genie::Pt TGT{(m_anim.target.x - MON->m_position.x) * S, (m_anim.target.y - MON->m_position.y) * S};
        const auto      VERTS = genieMesh(WIN, TGT, ICON_W * S, m_anim.s(), SPACE, {0, 0}, TEX->m_size);
        GL::g_pHyprOpenGL->renderTextureMesh(ctx, TEX, SPACE, VERTS,
                                             GL::CHyprOpenGLImpl::STextureRenderData{.damage = &ctx.m_data.damage, .a = alphaFor(m_anim.s()), .allowCustomUV = true});
        return {};
    }
    bool needsLiveBlur(CRenderContext&) override {
        return false;
    }
    bool needsPrecomputeBlur(CRenderContext&) override {
        return false;
    }
    bool disableSimplification(CRenderContext&) override {
        return true;
    }
    const char* passName() override {
        return "CGenieOutElement";
    }
    ePassElementType type() override {
        return EK_CUSTOM;
    }

  private:
    SAnim m_anim;
};

static void kick(PHLMONITOR mon) {
    if (mon) {
        g_pHyprRenderer->damageMonitor(mon);
        mon->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_ANIMATION);
    }
    if (Animation::mgr())
        Animation::mgr()->requestTick();
}

static void startIn(PHLWINDOW w) {
    const auto MON = w->m_monitor.lock();
    if (!MON || w->backend().traits().overrideRedirect)
        return;
    if (!w->effects().transformers()->get<CGenieTransformer>())
        w->effects().transformers()->emplace<CGenieTransformer>(w);
    g_ins[w.get()] = SAnim{.window = w, .monitor = MON, .out = false, .start = Clock::now(), .target = targetFor(w, MON)};
    kick(MON);
}

// A window asked to close (Super+Q, a title bar's button, the dock): its picture is taken now, while
// its content is still there; when the client goes away, its buffer is often already gone at unmap.
struct SShot {
    SP<IFramebuffer>  fb;
    CBox              box;
    Clock::time_point at;
};
static std::unordered_map<void*, SShot> g_closing;
static CFunctionHook*                  g_sendCloseHook = nullptr;
using FSendClose                                       = void (*)(Desktop::View::CWindow*);

static void hkSendClose(Desktop::View::CWindow* self) {
    if (self->mapped() && !self->isHidden())
        if (auto fb = g_pHyprRenderer->makeSnapshotFB(self->m_self.lock()))
            g_closing[self] = {fb, self->getFullWindowBoundingBox(), Clock::now()};
    ((FSendClose)g_sendCloseHook->m_original)(self);
}

// Takes the picture now (or uses the one taken at the close request): the window must still be on screen.
static bool startOut(PHLWINDOW w) {
    const auto MON = w->m_monitor.lock();
    if (!MON || !w->mapped() || w->backend().traits().overrideRedirect)
        return false;
    SShot shot;
    if (const auto it = g_closing.find(w.get()); it != g_closing.end()) {
        shot = it->second;
        g_closing.erase(it);
    }
    if (!shot.fb || Clock::now() - shot.at > std::chrono::seconds(3))
        shot = {g_pHyprRenderer->makeSnapshotFB(w), w->getFullWindowBoundingBox(), Clock::now()};
    if (!shot.fb || !shot.fb->getTexture())
        return false;
    g_ins.erase(w.get());
    g_outs.push_back(SAnim{.window = w, .monitor = MON, .out = true, .start = Clock::now(), .target = targetFor(w, MON), .snapshot = shot.fb, .box = shot.box});
    kick(MON);
    return true;
}

static void onTick() {
    std::erase_if(g_closing, [](const auto& p) { return Clock::now() - p.second.at > std::chrono::seconds(3); }); // it did not close
    if (g_outs.empty() && g_ins.empty())
        return;
    std::vector<PHLMONITOR> mons;
    for (auto& a : g_outs)
        mons.push_back(a.monitor.lock());
    for (auto& [_, a] : g_ins)
        mons.push_back(a.monitor.lock());
    std::erase_if(g_outs, [](const SAnim& a) { return a.progress() >= 1.0; });
    std::erase_if(g_ins, [](const auto& p) { return p.second.progress() >= 1.0 || !p.second.window; });
    for (auto& m : mons)
        kick(m); // also once after the last frame, to wipe it
}

static void onRenderStage(const Event::SRenderStageEvent& ev) {
    if (ev.stage != RENDER_POST_WINDOWS || !ev.context || !ev.monitor)
        return;
    for (const auto& a : g_outs)
        if (a.monitor == ev.monitor)
            IHyprRenderer::addPassElement(ev.context->get(), makeUnique<CGenieOutElement>(a));
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    if (std::string{__hyprland_api_get_hash()} != __hyprland_api_get_client_hash()) {
        HyprlandAPI::addNotification(PHANDLE, "[minimo-genie] built for another Hyprland: rebuild it (make)", CHyprColor{1.0, 0.3, 0.3, 1.0}, 8000);
        throw std::runtime_error("[minimo-genie] version mismatch");
    }

    for (const auto& f : HyprlandAPI::findFunctionsByName(PHANDLE, "sendClose"))
        if (f.demangled.contains("CWindow::sendClose")) {
            g_sendCloseHook = HyprlandAPI::createFunctionHook(PHANDLE, f.address, (void*)&hkSendClose);
            break;
        }
    if (!g_sendCloseHook || !g_sendCloseHook->hook())
        HyprlandAPI::addNotification(PHANDLE, "[minimo-genie] no close hook: closing windows may not flow", CHyprColor{1.0, 0.7, 0.3, 1.0}, 5000);

    auto& E = Event::bus()->m_events;
    g_listeners.push_back(E.tick.listen([] { onTick(); }));
    g_listeners.push_back(E.render.stage.listen([](const Event::SRenderStageEvent& ev) { onRenderStage(ev); }));
    g_listeners.push_back(E.window.open.listen([](PHLWINDOW w) { startIn(w); }));
    g_listeners.push_back(E.window.close.listen([](PHLWINDOW w) {
        // ours replaces Hyprland's own fade-out of this window
        if (startOut(w))
            w->m_ruleApplicator->noAnim().matchOptional(true, Desktop::Types::PRIORITY_SET_PROP);
    }));
    g_listeners.push_back(E.window.moveToWorkspace.listen([](PHLWINDOW w, PHLWORKSPACE ws) {
        if (!ws)
            return;
        if (ws->addressableName().starts_with("special:")) {
            // put away by us: the genie already shows it leaving, so Hyprland's fade-out is skipped
            if (std::ranges::any_of(g_minimised, [&](const PHLWINDOWREF& r) { return r.get() == w.get(); }))
                w->alpha()[Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE]->warp();
            return;
        }
        const auto BEFORE = g_minimised.size();
        std::erase_if(g_minimised, [&](const PHLWINDOWREF& r) { return !r || r.get() == w.get(); });
        if (g_minimised.size() < BEFORE)
            startIn(w);
    }));

    g_cmd = HyprlandAPI::registerHyprCtlCommand(PHANDLE,
                                                IPC::Socket1::SCommand{
                                                    .name    = "genie",
                                                    .match   = IPC::Socket1::COMMAND_MATCH_PREFIX,
                                                    .handler = [](const IPC::Socket1::SRequest& req) -> IPC::Socket1::SResponse {
                                                        std::istringstream is(req.command);
                                                        std::string        cmd, verb, addr;
                                                        is >> cmd >> verb >> addr;
                                                        if (verb != "out")
                                                            return "usage: hyprctl genie out <0xaddress>";
                                                        for (const auto& w : Desktop::windowState()->windows()) {
                                                            if (!w || addressOf(w) != addr)
                                                                continue;
                                                            if (!startOut(w))
                                                                return "not on screen";
                                                            g_minimised.push_back(w);
                                                            return "ok";
                                                        }
                                                        return "no such window";
                                                    },
                                                });

    return {"minimo-genie", "macOS-like genie into Minimo's dock", "minimo", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_listeners.clear();
    if (g_cmd)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_cmd);
    g_outs.clear();
    g_ins.clear();
    g_closing.clear();
    // our transformers are now inactive: drop them before our code is unloaded
    for (const auto& w : Desktop::windowState()->windows())
        if (w)
            w->effects().transformers()->removeInactive();
}
