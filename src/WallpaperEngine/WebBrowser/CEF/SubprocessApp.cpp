#include "SubprocessApp.h"
#include "WPSchemeHandlerFactory.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Logging/Log.h"
#include "include/cef_v8.h"

using namespace WallpaperEngine::WebBrowser::CEF;

namespace {
constexpr auto audioCallbackKey = "__wallpaperEngineAudioCallback";
constexpr auto audioErrorKey = "__wallpaperEngineAudioError";

class AudioListenerRegistration final : public CefV8Handler {
public:
    bool Execute (const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments,
                  CefRefPtr<CefV8Value>& result, CefString& exception) override {
        if (arguments.size () != 1 || !arguments[0]->IsFunction ()) {
            exception = "wallpaperRegisterAudioListener expects a callback function";
            return true;
        }
        // Keep the callback on its own context. Re-registering replaces it,
        // and context destruction releases it without cross-thread V8 state.
        CefV8Context::GetCurrentContext ()->GetGlobal ()->SetValue (
            audioCallbackKey, arguments[0], V8_PROPERTY_ATTRIBUTE_DONTENUM);
        CefV8Context::GetCurrentContext ()->GetGlobal ()->DeleteValue (audioErrorKey);
        result = CefV8Value::CreateUndefined ();
        return true;
    }
    IMPLEMENT_REFCOUNTING (AudioListenerRegistration);
};
}

SubprocessApp::SubprocessApp (WallpaperEngine::Application::WallpaperApplication& application) :
    m_application (application) {
    for (const auto& info : this->m_application.getBackgrounds () | std::views::values) {
	this->m_handlerFactories[info->workshopId] = new WPSchemeHandlerFactory (*info);
    }
}

CefRefPtr<CefRenderProcessHandler> SubprocessApp::GetRenderProcessHandler () { return this; }

void SubprocessApp::OnContextCreated (CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                                     CefRefPtr<CefV8Context> context) {
    if (!frame->IsMain ()) return;
    context->GetGlobal ()->SetValue ("wallpaperRegisterAudioListener",
        CefV8Value::CreateFunction ("wallpaperRegisterAudioListener", new AudioListenerRegistration),
        V8_PROPERTY_ATTRIBUTE_NONE);
}

bool SubprocessApp::OnProcessMessageReceived (CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                                             CefProcessId source, CefRefPtr<CefProcessMessage> message) {
    if (source != PID_BROWSER || message->GetName () != "wallpaperAudioFrame") return false;
    if (!frame->IsMain ()) return true;
    const auto samples = message->GetArgumentList ();
    if (samples->GetSize () != 128) return true;
    const auto context = frame->GetV8Context ();
    if (!context || !context->Enter ()) return true;
    const auto global = context->GetGlobal ();
    const auto callback = global->GetValue (audioCallbackKey);
    if (callback && callback->IsFunction ()) {
        const auto values = CefV8Value::CreateArray (128);
        for (int index = 0; index < 128; ++index)
            values->SetValue (index, CefV8Value::CreateDouble (samples->GetDouble (index)));
        callback->ExecuteFunction (global, {values});
        if (callback->HasException ()) {
            const auto error = callback->GetException ();
            const auto signature = error->GetScriptResourceName ().ToString () + ":"
                + std::to_string (error->GetLineNumber ()) + ": " + error->GetMessage ().ToString ();
            const auto previous = global->GetValue (audioErrorKey);
            if (!previous || !previous->IsString () || previous->GetStringValue () != signature) {
                sLog.error ("Web audio listener ", signature);
                global->SetValue (audioErrorKey, CefV8Value::CreateString (signature), V8_PROPERTY_ATTRIBUTE_DONTENUM);
            }
            callback->ClearException ();
        }
    }
    context->Exit ();
    return true;
}

void SubprocessApp::OnRegisterCustomSchemes (CefRawPtr<CefSchemeRegistrar> registrar) {
    // register all the needed schemes, "wp" + the background id is going to be our scheme
    for (const auto& workshopId : this->m_handlerFactories | std::views::keys) {
	registrar->AddCustomScheme (
	    WPSchemeHandlerFactory::generateSchemeName (workshopId),
	    // Native wallpaper documents load local file properties. Give this
	    // project asset origin file URL rules, rather than a remote-page origin.
	    CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_LOCAL | CEF_SCHEME_OPTION_SECURE
                | CEF_SCHEME_OPTION_FETCH_ENABLED
	);
    }
}

const WallpaperEngine::Application::WallpaperApplication& SubprocessApp::getApplication () const {
    return this->m_application;
}

const std::map<std::string, WPSchemeHandlerFactory*>& SubprocessApp::getHandlerFactories () const {
    return this->m_handlerFactories;
}
