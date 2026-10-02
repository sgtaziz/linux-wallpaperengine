// This code is a modification of the original projects that can be found at
// https://github.com/if1live/cef-gl-example
// https://github.com/andmcgregor/cefgui
#include "CWeb.h"
#include "WallpaperEngine/Render/Drivers/VideoDriver.h"
#include "WallpaperEngine/WebBrowser/CEF/WPSchemeHandlerFactory.h"
#include "WallpaperEngine/WebBrowser/WebPropertyPayload.h"
#include "WallpaperEngine/WebBrowser/WebAudioFrame.h"
#include "include/cef_process_message.h"
#include <stdexcept>

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Wallpapers;

using namespace WallpaperEngine::WebBrowser;
using namespace WallpaperEngine::WebBrowser::CEF;

CWeb::CWeb (
    const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext, WebBrowserContext& browserContext,
    const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
) : CWallpaper (wallpaper, context, audioContext, scalingMode, clampMode), m_browserContext (browserContext) {
    // setup framebuffers
    this->setupFramebuffers ();

    this->m_renderHandler = new WebBrowser::CEF::RenderHandler (this);

    const auto propertyPayload = WebBrowser::webPropertyPayload (this->getWeb ().project);
    this->m_client = new WebBrowser::CEF::BrowserClient (m_renderHandler, propertyPayload,
        WebBrowser::webFilePropertyPaths (this->getWeb ().project, propertyPayload));
}

void CWeb::createBrowser () {
    // Pages commonly snapshot the viewport at script startup. Create the
    // browser only after renderFrame supplies the actual output dimensions.
    CefWindowInfo window_info;
    window_info.SetAsWindowless (0);
    window_info.bounds = CefRect (0, 0, this->getWidth (), this->getHeight ());
    CefBrowserSettings browserSettings;
    browserSettings.windowless_frame_rate = std::max (60,
        this->getContext ().getApp ().getContext ().settings.render.maximumFPS);
    // use the custom scheme for the wallpaper's files
    const std::string htmlURL = WPSchemeHandlerFactory::generateSchemeName (this->getWeb ().project.workshopId)
	+ "://root/" + this->getWeb ().filename;
    this->m_browser
	= CefBrowserHost::CreateBrowserSync (window_info, this->m_client, htmlURL, browserSettings, nullptr, nullptr);
    if (!this->m_browser) throw std::runtime_error ("Cannot create web wallpaper browser");
}

void CWeb::setSize (const int width, const int height) {
    if (width <= 0 || height <= 0) return;
    this->m_sceneFBO->resize (width, height, width, height);
    this->m_width = width;
    this->m_height = height;

    // Notify cef that it was resized(maybe it's not even needed)
    if (this->m_browser) this->m_browser->GetHost ()->WasResized ();
}

void CWeb::renderFrame (const glm::ivec4& viewport) {
    if (viewport.z <= 0 || viewport.w <= 0) return;
    glm::ivec2 size (viewport.z, viewport.w);
    if (!this->m_browser) {
        const auto mode = this->getContext ().getApp ().getContext ().settings.render.mode;
        if (mode == Application::ApplicationContext::NORMAL_WINDOW
            || mode == Application::ApplicationContext::EXPLICIT_WINDOW) {
            auto& driver = this->getContext ().getDriver ();
            // The first surface presentation and its event poll deliver the
            // compositor's initial window configuration. Do not start page
            // scripts with the provisional dimensions before that happens.
            if (driver.getFrameCounter () == 0) return;
            // Output viewports refresh before the event poll, so their cached
            // size can lag the newly configured framebuffer by one frame.
            size = driver.getFramebufferSize ();
            if (size.x <= 0 || size.y <= 0) return;
        }
    }
    // ensure the viewport matches the window size, and resize if needed
    if (size.x != this->getWidth () || size.y != this->getHeight ()) {
	this->setSize (size.x, size.y);
    }
    if (!this->m_browser) this->createBrowser ();

    // ensure the virtual mouse position is up to date
    this->updateMouse (viewport);
    // use the scene's framebuffer by default
    glBindFramebuffer (GL_FRAMEBUFFER, this->getWallpaperFramebuffer ());
    // ensure we render over the whole framebuffer
    glViewport (0, 0, this->getWidth (), this->getHeight ());

    // Cef processes all messages, including OnPaint, which renders frame
    // If there is no OnPaint in message loop, we will not update(render) frame
    //  This means some frames will not have OnPaint call in cef messageLoop
    //  Because of that glClear will result in flickering on higher fps
    //  Do not use glClear until some method to control rendering with cef is supported
    // We might actually try to use cef to execute javascript, and not using off-screen rendering at all
    // But for now let it be like this
    //  glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    CefDoMessageLoopWork ();

    const auto now = std::chrono::steady_clock::now ();
    if (now >= m_nextAudioFrame) {
        const auto& recorder = this->getAudioContext ().getRecorder ();
        const auto values = WebBrowser::webAudioFrame (recorder.audio64Left, recorder.audio64Right);
        const auto message = CefProcessMessage::Create ("wallpaperAudioFrame");
        const auto arguments = message->GetArgumentList ();
        arguments->SetSize (values.size ());
        for (size_t index = 0; index < values.size (); ++index)
            arguments->SetDouble (index, values[index]);
        this->m_browser->GetMainFrame ()->SendProcessMessage (PID_RENDERER, message);
        const auto interval = std::chrono::microseconds (33333);
        m_nextAudioFrame += interval;
        if (m_nextAudioFrame < now - interval) m_nextAudioFrame = now + interval;
    }
}

void CWeb::updateMouse (const glm::ivec4& viewport) {
    // update virtual mouse position first
    auto& input = this->getContext ().getInputContext ().getMouseInput ();

    const glm::dvec2 position = input.position ();
    const auto leftClick = input.leftClick ();
    const auto rightClick = input.rightClick ();

    CefMouseEvent evt;
    // Set mouse current position. Maybe clamps are not needed
    evt.x = std::clamp (static_cast<int> (position.x - viewport.x), 0, viewport.z);
    // Convert from OpenGL coordinates (Y=0 at bottom) to CEF coordinates (Y=0 at top)
    evt.y = viewport.w - std::clamp (static_cast<int> (position.y - viewport.y), 0, viewport.w);
    // Send mouse position to cef
    this->m_browser->GetHost ()->SendMouseMoveEvent (evt, false);

    // TODO: ANY OTHER MOUSE EVENTS TO SEND?
    if (leftClick != this->m_leftClick) {
	this->m_browser->GetHost ()->SendMouseClickEvent (
	    evt, CefBrowserHost::MouseButtonType::MBT_LEFT,
	    leftClick == WallpaperEngine::Input::MouseClickStatus::Released, 1
	);
    }

    if (rightClick != this->m_rightClick) {
	this->m_browser->GetHost ()->SendMouseClickEvent (
	    evt, CefBrowserHost::MouseButtonType::MBT_RIGHT,
	    rightClick == WallpaperEngine::Input::MouseClickStatus::Released, 1
	);
    }

    this->m_leftClick = leftClick;
    this->m_rightClick = rightClick;
}

CWeb::~CWeb () {
    // Closing can leave queued CEF callbacks while the browser releases its
    // client. Keep the ref-counted handler alive, without a destroyed owner.
    this->m_renderHandler->detach ();
    if (this->m_browser) this->m_browser->GetHost ()->CloseBrowser (true);
}
