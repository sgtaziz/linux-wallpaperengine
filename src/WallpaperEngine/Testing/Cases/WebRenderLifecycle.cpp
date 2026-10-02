#include <catch2/catch_test_macros.hpp>
#include "WallpaperEngine/WebBrowser/CEF/BrowserClient.h"
#include "WallpaperEngine/WebBrowser/CEF/RenderHandler.h"

namespace {
class CountedRenderHandler final : public WallpaperEngine::WebBrowser::CEF::RenderHandler {
public:
    explicit CountedRenderHandler (int& destroyed) : RenderHandler (nullptr), m_destroyed (destroyed) { }
    ~CountedRenderHandler () override { ++m_destroyed; }
private:
    int& m_destroyed;
};
}

TEST_CASE ("Web render callbacks survive owner detachment until the client releases them", "[web][lifecycle]") {
    int destroyed = 0;
    CefRefPtr<CountedRenderHandler> owner = new CountedRenderHandler (destroyed);
    CefRefPtr<WallpaperEngine::WebBrowser::CEF::BrowserClient> client
        = new WallpaperEngine::WebBrowser::CEF::BrowserClient (owner, "{}", {});
    auto callback = client->GetRenderHandler ();
    owner->detach ();
    owner = nullptr;
    REQUIRE (destroyed == 0);
    client = nullptr;
    REQUIRE (destroyed == 0);
    CefRect rect;
    callback->GetViewRect (nullptr, rect);
    REQUIRE (rect.width > 0);
    REQUIRE (rect.height > 0);
    // A queued paint after detachment must return without accessing GL or the
    // old wallpaper. No graphics context or paint buffer exists in this test.
    callback->OnPaint (nullptr, PET_VIEW, {}, nullptr, 1, 1);
    callback = nullptr;
    REQUIRE (destroyed == 1);
}
