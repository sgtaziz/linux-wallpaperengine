#include "SceneCameraFade.h"

#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <string>

using namespace WallpaperEngine::Render::Wallpapers;

namespace {
GLuint compile (GLenum type, const char* source) {
    const GLuint shader = glCreateShader (type);
    glShaderSource (shader, 1, &source, nullptr);
    glCompileShader (shader);
    GLint status = GL_FALSE;
    glGetShaderiv (shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
	GLint length = 0;
	glGetShaderiv (shader, GL_INFO_LOG_LENGTH, &length);
	std::string message (std::max (length, 1), '\0');
	glGetShaderInfoLog (shader, length, nullptr, message.data ());
	glDeleteShader (shader);
	sLog.exception ("Camera fade shader: ", message);
    }
    return shader;
}
} // namespace

SceneCameraFade::~SceneCameraFade () {
    if (m_program) glDeleteProgram (m_program);
    if (m_positions) glDeleteBuffers (1, &m_positions);
    if (m_vertexArray) glDeleteVertexArrays (1, &m_vertexArray);
}

void SceneCameraFade::create () {
    // shaders/fade.vert passes a clip-space quad through; shaders/fade.frag
    // writes vec4(color * 0.7, g_Alpha).
    const GLuint vertex = compile (GL_VERTEX_SHADER, "#version 330\n"
	"in vec3 a_Position;\n"
	"void main () { gl_Position = vec4 (a_Position, 1.0); }\n");
    const GLuint fragment = compile (GL_FRAGMENT_SHADER, "#version 330\n"
	"uniform float g_Alpha;\n"
	"uniform vec3 color;\n"
	"out vec4 out_FragColor;\n"
	"void main () { out_FragColor = vec4 (color * 0.7, g_Alpha); }\n");
    m_program = glCreateProgram ();
    glAttachShader (m_program, vertex);
    glAttachShader (m_program, fragment);
    glBindAttribLocation (m_program, 0, "a_Position");
    glLinkProgram (m_program);
    glDeleteShader (vertex);
    glDeleteShader (fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv (m_program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) sLog.exception ("Camera fade program failed to link");
    m_color = glGetUniformLocation (m_program, "color");
    m_alpha = glGetUniformLocation (m_program, "g_Alpha");
    static constexpr GLfloat positions[] = {
	-1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, -1.0f, 1.0f, 0.0f,
	-1.0f, 1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f,
    };
    glGenVertexArrays (1, &m_vertexArray);
    glBindVertexArray (m_vertexArray);
    glGenBuffers (1, &m_positions);
    glBindBuffer (GL_ARRAY_BUFFER, m_positions);
    glBufferData (GL_ARRAY_BUFFER, sizeof (positions), positions, GL_STATIC_DRAW);
    glEnableVertexAttribArray (0);
    glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
}

void SceneCameraFade::draw (GLuint framebuffer, int width, int height, const glm::vec3& schemeColor, float alpha) {
    if (!(alpha > 0.0f) || width <= 0 || height <= 0) return;
    if (!m_program) create ();
    GLint previousArray = 0;
    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &previousArray);
    glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
    glViewport (0, 0, width, height);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_CULL_FACE);
    // The fade material is translucent with depth test and write disabled.
    glEnable (GL_BLEND);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram (m_program);
    glUniform3f (m_color, schemeColor.r, schemeColor.g, schemeColor.b);
    // A fixed-point target saturates the fragment alpha.
    glUniform1f (m_alpha, std::min (alpha, 1.0f));
    glBindVertexArray (m_vertexArray);
    glDrawArrays (GL_TRIANGLES, 0, 6);
    glBindVertexArray (previousArray);
    glDisable (GL_BLEND);
}
