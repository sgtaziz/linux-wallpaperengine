#pragma once

#include "include/cef_client.h"
#include "include/cef_load_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_resource_request_handler.h"
#include <filesystem>
#include <set>

namespace WallpaperEngine::WebBrowser::CEF {
// *************************************************************************
//! \brief Provide access to browser-instance-specific callbacks. A single
//! CefClient instance can be shared among any number of browsers.
// *************************************************************************
class BrowserClient : public CefClient, public CefLoadHandler, public CefDisplayHandler,
                      public CefRequestHandler, public CefResourceRequestHandler {
public:
    explicit BrowserClient (CefRefPtr<CefRenderHandler> ptr, std::string propertyPayload,
                            std::set<std::filesystem::path> fileProperties);

    [[nodiscard]] CefRefPtr<CefRenderHandler> GetRenderHandler () override;
    [[nodiscard]] CefRefPtr<CefLoadHandler> GetLoadHandler () override;
    [[nodiscard]] CefRefPtr<CefDisplayHandler> GetDisplayHandler () override;
    [[nodiscard]] CefRefPtr<CefRequestHandler> GetRequestHandler () override;
    void OnLoadEnd (CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;
    bool OnConsoleMessage (CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
                           const CefString& message, const CefString& source, int line) override;
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler (
        CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request,
        bool isNavigation, bool isDownload, const CefString& requestInitiator, bool& disableDefaultHandling) override;
    CefRefPtr<CefResourceHandler> GetResourceHandler (
        CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request) override;
    void OnResourceLoadComplete (CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
        CefRefPtr<CefRequest> request, CefRefPtr<CefResponse> response,
        URLRequestStatus status, int64_t receivedContentLength) override;

    CefRefPtr<CefRenderHandler> m_renderHandler = nullptr;
    std::string m_propertyPayload;
    const std::set<std::filesystem::path> m_fileProperties;
    std::set<std::string> m_reportedResourceErrors;

    IMPLEMENT_REFCOUNTING (BrowserClient);
};
} // namespace WallpaperEngine::WebBrowser::CEF
