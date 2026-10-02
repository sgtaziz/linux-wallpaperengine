#include "RenderHandler.h"

using namespace WallpaperEngine::WebBrowser::CEF;

RenderHandler::RenderHandler (WallpaperEngine::Render::Wallpapers::CWeb* webdata) : m_webdata (webdata) {
    if (m_webdata) m_viewRect = CefRect (0, 0, m_webdata->getWidth (), m_webdata->getHeight ());
}

void RenderHandler::detach () {
    if (m_webdata) m_viewRect = CefRect (0, 0, m_webdata->getWidth (), m_webdata->getHeight ());
    m_webdata = nullptr;
}

// Required by CEF
void RenderHandler::GetViewRect (CefRefPtr<CefBrowser> browser, CefRect& rect) {
    if (m_webdata) m_viewRect = CefRect (0, 0, m_webdata->getWidth (), m_webdata->getHeight ());
    rect = m_viewRect;
}

// Will be executed in CEF message loop
void RenderHandler::OnPaint (
    CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer,
    const int width, const int height
) {
    if (!m_webdata || type != PET_VIEW || width != getWidth () || height != getHeight ()) return;
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, this->texture ());
    glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, buffer);
    glBindTexture (GL_TEXTURE_2D, 0);
}

int RenderHandler::getWidth () const { return this->m_webdata->getWidth (); }

int RenderHandler::getHeight () const { return this->m_webdata->getHeight (); }

// Must be the color texture attached to the wallpaper FBO — not the FBO name
// itself. Binding the FBO id as GL_TEXTURE_2D leaves the wallpaper black.
GLuint RenderHandler::texture () const { return this->m_webdata->getWallpaperTexture (); }
