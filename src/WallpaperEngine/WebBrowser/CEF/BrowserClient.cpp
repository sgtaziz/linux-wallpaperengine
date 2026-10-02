#include "BrowserClient.h"
#include "WallpaperEngine/Logging/Log.h"
#include "include/cef_parser.h"
#include "include/wrapper/cef_stream_resource_handler.h"
#include "MimeTypes.h"

using namespace WallpaperEngine::WebBrowser::CEF;

BrowserClient::BrowserClient (CefRefPtr<CefRenderHandler> ptr, std::string propertyPayload,
                             std::set<std::filesystem::path> fileProperties) :
    m_renderHandler (std::move (ptr)), m_propertyPayload (std::move (propertyPayload)),
    m_fileProperties (std::move (fileProperties)) { }

CefRefPtr<CefRenderHandler> BrowserClient::GetRenderHandler () { return m_renderHandler; }

CefRefPtr<CefLoadHandler> BrowserClient::GetLoadHandler () { return this; }

CefRefPtr<CefDisplayHandler> BrowserClient::GetDisplayHandler () { return this; }

CefRefPtr<CefRequestHandler> BrowserClient::GetRequestHandler () { return this; }

CefRefPtr<CefResourceRequestHandler> BrowserClient::GetResourceRequestHandler (
    CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>, bool, bool, const CefString&, bool&) {
    return this;
}

CefRefPtr<CefResourceHandler> BrowserClient::GetResourceHandler (
    CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request) {
    CefURLParts parts;
    if (!CefParseURL (request->GetURL (), parts) || CefString (&parts.scheme) != "file") return nullptr;
    const auto host = CefString (&parts.host).ToString ();
    if (!host.empty () && host != "localhost") return nullptr;
    const std::filesystem::path path = CefURIDecode (CefString (&parts.path), true,
        static_cast<cef_uri_unescape_rule_t> (UU_NORMAL | UU_SPACES | UU_URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS))
            .ToString ();
    std::error_code error;
    const auto canonical = std::filesystem::canonical (path, error);
    if (error || !m_fileProperties.contains (canonical)) return nullptr;
    const auto stream = CefStreamReader::CreateForFile (canonical.string ());
    if (!stream) return nullptr;
    const auto mime = MimeTypes::getType (canonical.string ().c_str ());
    sLog.debug ("Serving selected web file property ", canonical.string ());
    return new CefStreamResourceHandler (mime ? mime : "application/octet-stream", stream);
}

void BrowserClient::OnResourceLoadComplete (
    CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request,
    CefRefPtr<CefResponse> response, URLRequestStatus status, int64_t) {
    if (status != UR_FAILED) return;
    const auto url = request->GetURL ().ToString ();
    if (m_reportedResourceErrors.insert (url).second)
        sLog.error ("Web wallpaper resource failed: ", url, " (", static_cast<int> (response->GetError ()), ")");
}

bool BrowserClient::OnConsoleMessage (CefRefPtr<CefBrowser>, cef_log_severity_t level,
                                      const CefString& message, const CefString& source, int line) {
    if (level >= LOGSEVERITY_ERROR)
        sLog.error ("Web wallpaper ", source.ToString (), ":", line, ": ", message.ToString ());
    else
        sLog.debug ("Web wallpaper ", source.ToString (), ":", line, ": ", message.ToString ());
    return false;
}

void BrowserClient::OnLoadEnd (CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int httpStatusCode) {
    if (!frame->IsMain () || httpStatusCode >= 400) return;
    frame->ExecuteJavaScript (
        "if (window.wallpaperPropertyListener && typeof window.wallpaperPropertyListener.applyUserProperties === 'function') "
        "window.wallpaperPropertyListener.applyUserProperties(" + m_propertyPayload + ");",
        frame->GetURL (), 0);
}
