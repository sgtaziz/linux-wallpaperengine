#pragma once

#include <GL/glew.h>

namespace WallpaperEngine::Render::Objects {

// Native text draws through the shipped materials/fonts/*.json, which are all
// nocull. Text draws outside CPass, so it must clear face culling itself
// instead of inheriting the cull state a preceding model pass left behind.
// Depth state is deliberately left as the last pass set it.
inline void applyTextDrawState () {
    glDisable (GL_CULL_FACE);
}

// Native text depth contract (native D3/D5/D6/D7 captures, w29 depth-re):
//  - depth TEST: only in a perspective scene whose text depthtest is not "disabled"
//    (orthographic scenes skip it; the property is stored and the gate read);
//  - depth WRITE: never (the *_depth font materials enable it, but a sphere drawn after
//    the text still covers the first glyph completely in the native D3 capture).
// The compare function is not recoverable from the materials; LEQUAL is CPass's default
// for depthtest "enabled" (CPass.cpp:231-232).
inline bool textDepthTestEnabled (bool orthogonal, bool depthTest) {
    return !orthogonal && depthTest;
}

// Sets the depth state for one text draw and restores the caller's state on exit, so
// neither a previous model pass's depthwrite (CPass.cpp:253) nor this draw's state leaks.
class TextDepthState {
  public:
    TextDepthState (bool test) {
	m_test = glIsEnabled (GL_DEPTH_TEST) == GL_TRUE;
	glGetIntegerv (GL_DEPTH_FUNC, &m_func);
	glGetBooleanv (GL_DEPTH_WRITEMASK, &m_mask);
	if (test) {
	    glEnable (GL_DEPTH_TEST);
	    glDepthFunc (GL_LEQUAL);
	} else {
	    glDisable (GL_DEPTH_TEST);
	}
	glDepthMask (GL_FALSE);
    }
    ~TextDepthState () {
	glDepthMask (m_mask);
	glDepthFunc (static_cast<GLenum> (m_func));
	if (m_test) glEnable (GL_DEPTH_TEST); else glDisable (GL_DEPTH_TEST);
    }
    TextDepthState (const TextDepthState&) = delete;
    TextDepthState& operator= (const TextDepthState&) = delete;

  private:
    bool m_test = false;
    GLint m_func = GL_LESS;
    GLboolean m_mask = GL_TRUE;
};

} // namespace WallpaperEngine::Render::Objects
