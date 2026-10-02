#include "CWallpaper.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Render/Wallpapers/CVideo.h"
#include "WallpaperEngine/Render/Wallpapers/CWeb.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

using namespace WallpaperEngine::Render;

CWallpaper::CWallpaper (
    const Wallpaper& wallpaperData, RenderContext& context, AudioContext& audioContext,
    const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
) :
    ContextAware (context), FBOProvider (nullptr), m_wallpaperData (wallpaperData), m_audioContext (audioContext),
    m_state (scalingMode, clampMode) {
    // generate the VAO to stop opengl from complaining
    glGenVertexArrays (1, &this->m_vaoBuffer);
    glBindVertexArray (this->m_vaoBuffer);

    this->setupShaders ();

    constexpr GLfloat texCoords[] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };

    // inverted positions so the final texture is rendered properly
    constexpr GLfloat position[] = { -1.0f, 1.0f,  0.0f, 1.0,  1.0f, 0.0f, -1.0f, -1.0f, 0.0f,
				     -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f,  -1.0f, 0.0f };

    glGenBuffers (1, &this->m_texCoordBuffer);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texCoordBuffer);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texCoords), texCoords, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_positionBuffer);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_positionBuffer);
    glBufferData (GL_ARRAY_BUFFER, sizeof (position), position, GL_STATIC_DRAW);
}

CWallpaper::~CWallpaper () {
    // destroy shader programs
    GLuint attachedShaders[2];
    GLsizei attachedCount = 0;

    // destroy shaders (we only attach 2 to each program)
    glGetAttachedShaders (this->m_shader, 2, &attachedCount, attachedShaders);

    for (auto i = 0; i < attachedCount; i++) {
	glDeleteShader (attachedShaders[i]);
    }

    glDeleteProgram (this->m_shader);
    if (m_hdrShader != GL_NONE) glDeleteProgram (m_hdrShader);
    if (m_hdrPyramidShader != GL_NONE) glDeleteProgram (m_hdrPyramidShader);

    // destroy used buffers
    glDeleteBuffers (1, &this->m_texCoordBuffer);
    glDeleteBuffers (1, &this->m_positionBuffer);
    if (m_hdrPositionBuffer != GL_NONE) glDeleteBuffers (1, &m_hdrPositionBuffer);
    glDeleteVertexArrays (1, &this->m_vaoBuffer);
}

const AssetLocator& CWallpaper::getAssetLocator () const { return *this->m_wallpaperData.project.assetLocator; }

const Wallpaper& CWallpaper::getWallpaperData () const { return this->m_wallpaperData; }

GLuint CWallpaper::getWallpaperFramebuffer () const {
    return (m_hdrOutput ? m_hdrOutput : m_sceneFBO)->getFramebuffer ();
}

GLuint CWallpaper::getWallpaperTexture () const {
    return (m_hdrOutput ? m_hdrOutput : m_sceneFBO)->getTextureID (0);
}

void CWallpaper::setupShaders () {
    // reserve shaders in OpenGL
    const GLuint vertexShaderID = glCreateShader (GL_VERTEX_SHADER);

    // give shader's source code to OpenGL to be compiled
    const char* sourcePointer = "#version 330\n"
				"precision highp float;\n"
				"in vec3 a_Position;\n"
				"in vec2 a_TexCoord;\n"
				"out vec2 v_TexCoord;\n"
				"void main () {\n"
				"gl_Position = vec4 (a_Position, 1.0);\n"
				"v_TexCoord = a_TexCoord;\n"
				"}";

    glShaderSource (vertexShaderID, 1, &sourcePointer, nullptr);
    glCompileShader (vertexShaderID);

    GLint result = GL_FALSE;
    int infoLogLength = 0;

    // ensure the vertex shader was correctly compiled
    glGetShaderiv (vertexShaderID, GL_COMPILE_STATUS, &result);
    glGetShaderiv (vertexShaderID, GL_INFO_LOG_LENGTH, &infoLogLength);

    if (infoLogLength > 0) {
	const auto logBuffer = new char[infoLogLength + 1];
	// ensure logBuffer ends with a \0
	memset (logBuffer, 0, infoLogLength + 1);
	// get information about the error
	glGetShaderInfoLog (vertexShaderID, infoLogLength, nullptr, logBuffer);
	// throw an exception about the issue
	const std::string message = logBuffer;
	// free the buffer
	delete[] logBuffer;
	// throw an exception
	sLog.exception (message);
    }

    // reserve shaders in OpenGL
    const GLuint fragmentShaderID = glCreateShader (GL_FRAGMENT_SHADER);

    // give shader's source code to OpenGL to be compiled
    sourcePointer = "#version 330\n"
		    "precision highp float;\n"
		    "uniform sampler2D g_Texture0;\n"
		    "in vec2 v_TexCoord;\n"
		    "out vec4 out_FragColor;\n"
		    "void main () {\n"
		    "out_FragColor = texture (g_Texture0, v_TexCoord);\n"
		    "}";

    glShaderSource (fragmentShaderID, 1, &sourcePointer, nullptr);
    glCompileShader (fragmentShaderID);

    result = GL_FALSE;
    infoLogLength = 0;

    // ensure the vertex shader was correctly compiled
    glGetShaderiv (fragmentShaderID, GL_COMPILE_STATUS, &result);
    glGetShaderiv (fragmentShaderID, GL_INFO_LOG_LENGTH, &infoLogLength);

    if (infoLogLength > 0) {
	const auto logBuffer = new char[infoLogLength + 1];
	// ensure logBuffer ends with a \0
	memset (logBuffer, 0, infoLogLength + 1);
	// get information about the error
	glGetShaderInfoLog (fragmentShaderID, infoLogLength, nullptr, logBuffer);
	// throw an exception about the issue
	const std::string message = logBuffer;
	// free the buffer
	delete[] logBuffer;
	// throw an exception
	sLog.exception (message);
    }

    // create the final program
    this->m_shader = glCreateProgram ();
    // link the shaders together
    glAttachShader (this->m_shader, vertexShaderID);
    glAttachShader (this->m_shader, fragmentShaderID);
    glLinkProgram (this->m_shader);
    // check that the shader was properly linked
    result = GL_FALSE;
    infoLogLength = 0;

    glGetProgramiv (this->m_shader, GL_LINK_STATUS, &result);
    glGetProgramiv (this->m_shader, GL_INFO_LOG_LENGTH, &infoLogLength);

    if (infoLogLength > 0) {
	const auto logBuffer = new char[infoLogLength + 1];
	// ensure logBuffer ends with a \0
	memset (logBuffer, 0, infoLogLength + 1);
	// get information about the error
	glGetProgramInfoLog (this->m_shader, infoLogLength, nullptr, logBuffer);
	// throw an exception about the issue
	const std::string message = logBuffer;
	// free the buffer
	delete[] logBuffer;
	// throw an exception
	sLog.exception (message);
    }

    // after being liked shaders can be dettached and deleted
    glDetachShader (this->m_shader, vertexShaderID);
    glDetachShader (this->m_shader, fragmentShaderID);

    // Native ultra's final combine_hdr shader adds the separate bloom target
    // before converting SDR output through its sRGB-to-linear branch. Keep
    // this program independent of the ordinary direct-copy presentation.
    const GLuint hdrFragment = glCreateShader (GL_FRAGMENT_SHADER);
    const char* hdrSource = R"GLSL(#version 330
precision highp float;
uniform sampler2D g_Texture0;
uniform sampler2D g_Texture1;
uniform vec2 g_TexelSize;
in vec2 v_TexCoord;
out vec4 out_FragColor;
vec3 lin(vec3 v) {
    vec3 c = step(vec3(0.04045), v);
    return c * pow((v + vec3(0.055)) / 1.055, vec3(2.4))
        + (vec3(1.0) - c) * (v / 12.92);
}
void main() {
    vec3 albedo = texture(g_Texture0, v_TexCoord).rgb;
    vec2 t = g_TexelSize;
    vec3 bloom = (texture(g_Texture1, v_TexCoord + t).rgb
        + texture(g_Texture1, v_TexCoord - t).rgb
        + texture(g_Texture1, v_TexCoord + vec2(t.x, -t.y)).rgb
        + texture(g_Texture1, v_TexCoord + vec2(-t.x, t.y)).rgb) * 0.25;
    out_FragColor = vec4(clamp(lin(albedo + bloom), 0.0, 1.0), 1.0);
}
)GLSL";
    glShaderSource (hdrFragment, 1, &hdrSource, nullptr);
    glCompileShader (hdrFragment);
    glGetShaderiv (hdrFragment, GL_COMPILE_STATUS, &result);
    if (result != GL_TRUE) {
        GLint length = 0;
        glGetShaderiv (hdrFragment, GL_INFO_LOG_LENGTH, &length);
        std::string log (std::max (1, length), '\0');
        glGetShaderInfoLog (hdrFragment, length, nullptr, log.data ());
        sLog.exception ("HDR presentation shader: ", log);
    }
    m_hdrShader = glCreateProgram ();
    glAttachShader (m_hdrShader, vertexShaderID);
    glAttachShader (m_hdrShader, hdrFragment);
    glLinkProgram (m_hdrShader);
    glGetProgramiv (m_hdrShader, GL_LINK_STATUS, &result);
    if (result != GL_TRUE) {
        GLint length = 0;
        glGetProgramiv (m_hdrShader, GL_INFO_LOG_LENGTH, &length);
        std::string log (std::max (1, length), '\0');
        glGetProgramInfoLog (m_hdrShader, length, nullptr, log.data ());
        sLog.exception ("HDR presentation program: ", log);
    }
    glDetachShader (m_hdrShader, vertexShaderID);
    glDetachShader (m_hdrShader, hdrFragment);
    glDeleteShader (hdrFragment);

    glDeleteShader (vertexShaderID);
    glDeleteShader (fragmentShaderID);

    // get textures
    this->g_Texture0 = glGetUniformLocation (this->m_shader, "g_Texture0");
    this->a_Position = glGetAttribLocation (this->m_shader, "a_Position");
    this->a_TexCoord = glGetAttribLocation (this->m_shader, "a_TexCoord");
    m_hdrTexture0 = glGetUniformLocation (m_hdrShader, "g_Texture0");
    m_hdrTexture1 = glGetUniformLocation (m_hdrShader, "g_Texture1");
    m_hdrTexelSize = glGetUniformLocation (m_hdrShader, "g_TexelSize");
    m_hdrPosition = glGetAttribLocation (m_hdrShader, "a_Position");
    m_hdrTexCoord = glGetAttribLocation (m_hdrShader, "a_TexCoord");
}

void CWallpaper::setHdrPresentation (const HdrBloomSettings& settings) {
    m_hdrBloomSettings = settings;
    m_hdrOutput = create ("_rt_HDRPresented", TextureFormat_ARGB8888, TextureFlags_ClampUVs,
                          1.0f, {getWidth (), getHeight ()}, {getWidth (), getHeight ()});
    int width = getWidth ();
    int height = getHeight ();
    for (int index = 0; index < 8 && std::min (width, height) >= 2; ++index) {
        width = std::max (1, width / 2);
        height = std::max (1, height / 2);
        m_hdrPyramid.push_back (create (
            "_rt_HDRBloomLevel" + std::to_string (index), TextureFormat_RGBA16161616f,
            TextureFlags_ClampUVs, 1.0f, {width, height}, {width, height}));
    }
    if (m_hdrPyramid.empty ()) {
        m_hdrPyramid.push_back (create ("_rt_HDRBloomLevel0", TextureFormat_RGBA16161616f,
            TextureFlags_ClampUVs, 1.0f, {1, 1}, {1, 1}));
    }
    m_hdrBloom = m_hdrPyramid.front ();
    updateHdrBloomSettings (settings);

    // Native HDR framebuffer textures pass sampler-factory param3=1 via
    // 1400eb440 -> 140099980, which selects max anisotropy one. CFBO's
    // general default is eight; retain that for ordinary targets and scope
    // the native sampler choice to this HDR graph.
    m_sceneFBO->setMaxAnisotropy (1.0f);
    m_hdrOutput->setMaxAnisotropy (1.0f);
    for (const auto& target : m_hdrPyramid) target->setMaxAnisotropy (1.0f);

    const GLuint vertex = glCreateShader (GL_VERTEX_SHADER);
    const char* vertexSource = R"GLSL(#version 330
out vec2 v_uv;
void main() {
    vec2 p[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    gl_Position = vec4(p[gl_VertexID], 0, 1);
    v_uv = (p[gl_VertexID] + 1.0) * 0.5;
}

)GLSL";
    glShaderSource (vertex, 1, &vertexSource, nullptr);
    glCompileShader (vertex);
    const GLuint fragment = glCreateShader (GL_FRAGMENT_SHADER);
    const char* fragmentSource = R"GLSL(#version 330
uniform sampler2D u_source;
uniform vec2 u_texel;
uniform float u_strength;
uniform vec4 u_blend;
uniform vec3 u_tint;
uniform float u_scatter;
uniform bool u_bloom;
uniform bool u_upsample;
uniform bool u_bicubic;
in vec2 v_uv;
out vec4 out_color;
vec4 cubic(float v) {
    vec4 n = vec4(1.0, 2.0, 3.0, 4.0) - v;
    vec4 s = n*n*n;
    float x=s.x, y=s.y-4.0*s.x, z=s.z-4.0*s.y+6.0*s.x;
    return vec4(x,y,z,6.0-x-y-z)*(1.0/6.0);
}
vec3 bicubic(vec2 uv) {
    vec2 texSize = 0.5/u_texel;
    vec2 invSize = u_texel/0.5;
    uv = uv*texSize-0.5;
    vec2 fxy=fract(uv);
    uv-=fxy;
    vec4 xc=cubic(fxy.x), yc=cubic(fxy.y);
    vec4 c=uv.xxyy+vec4(-0.5,1.5,-0.5,1.5);
    vec4 s=vec4(xc.xz+xc.yw,yc.xz+yc.yw);
    vec4 offset=(c+vec4(xc.yw,yc.yw)/s)*invSize.xxyy;
    vec3 a=texture(u_source,offset.xz).rgb;
    vec3 b=texture(u_source,offset.yz).rgb;
    vec3 d=texture(u_source,offset.xw).rgb;
    vec3 e=texture(u_source,offset.yw).rgb;
    return mix(mix(e,d,s.x/(s.x+s.y)),mix(b,a,s.x/(s.x+s.y)),s.z/(s.z+s.w));
}
vec3 sampleAt(vec2 uv) {
    return u_bicubic ? bicubic(uv) : texture(u_source,uv).rgb;
}
void main() {
    vec2 t=u_texel;
    vec3 color=(sampleAt(v_uv+t)+sampleAt(v_uv-t)
        +sampleAt(v_uv+vec2(t.x,-t.y))+sampleAt(v_uv+vec2(-t.x,t.y)))*0.25;
    if(u_upsample) color*=u_scatter;
    if(u_bloom) {
        color=max(color,vec3(0));
        float brightness=max(color.r,max(color.g,color.b));
        float soft=clamp(brightness-u_blend.y,0.0,u_blend.z);
        soft=soft*soft*u_blend.w;
        float contribution=max(soft,brightness-u_blend.x)/max(brightness,0.00001);
        color*=contribution*u_strength*u_tint;
    }
    out_color=vec4(color,1);
}
)GLSL";
    glShaderSource (fragment, 1, &fragmentSource, nullptr);
    glCompileShader (fragment);
    GLint compiled = GL_FALSE;
    glGetShaderiv (vertex, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) sLog.exception ("HDR bloom vertex shader failed to compile");
    glGetShaderiv (fragment, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        GLint length = 0;
        glGetShaderiv (fragment, GL_INFO_LOG_LENGTH, &length);
        std::string log (std::max (1, length), '\0');
        glGetShaderInfoLog (fragment, length, nullptr, log.data ());
        sLog.exception ("HDR bloom fragment shader: ", log);
    }
    m_hdrPyramidShader = glCreateProgram ();
    glAttachShader (m_hdrPyramidShader, vertex);
    glAttachShader (m_hdrPyramidShader, fragment);
    glLinkProgram (m_hdrPyramidShader);
    glGetProgramiv (m_hdrPyramidShader, GL_LINK_STATUS, &compiled);
    if (compiled != GL_TRUE) sLog.exception ("HDR bloom program failed to link");
    glDetachShader (m_hdrPyramidShader, vertex);
    glDetachShader (m_hdrPyramidShader, fragment);
    glDeleteShader (vertex);
    glDeleteShader (fragment);
    constexpr GLfloat normalPosition[] = {
        -1.0f, -1.0f, 0.0f,  1.0f, -1.0f, 0.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f,  1.0f, -1.0f, 0.0f,  1.0f, 1.0f, 0.0f,
    };
    glGenBuffers (1, &m_hdrPositionBuffer);
    glBindBuffer (GL_ARRAY_BUFFER, m_hdrPositionBuffer);
    glBufferData (GL_ARRAY_BUFFER, sizeof (normalPosition), normalPosition, GL_STATIC_DRAW);
}

void CWallpaper::resizeHdrPresentation (uint32_t width, uint32_t height) {
    if (!m_hdrOutput) return;
    m_hdrOutput->resize (width, height, width, height);
    for (const auto& target : m_hdrPyramid) {
        width = std::max (1u, width / 2);
        height = std::max (1u, height / 2);
        target->resize (width, height, width, height);
    }
}

void CWallpaper::updateHdrBloomSettings (const HdrBloomSettings& settings) {
    if (m_hdrPyramid.empty ()) return;
    m_hdrBloomSettings = settings;
    m_hdrActiveLevels = std::min<size_t> (m_hdrPyramid.size (), std::max (1, settings.iterations));
}

void CWallpaper::combineHdrFrame () {
    if (!m_hdrOutput || !m_hdrBloom) return;
    const bool peek = !m_hdrPeekDone
        && getContext ().getApp ().getContext ().settings.render.debug.hdrPeek;
    const auto peekTarget = [&] (const CFBO& target, const char* label) {
        GLint previousRead = 0, previousPackBuffer = 0, alignment = 0;
        GLint rowLength = 0, skipRows = 0, skipPixels = 0, swapBytes = 0;
        glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &previousRead);
        glGetIntegerv (GL_PIXEL_PACK_BUFFER_BINDING, &previousPackBuffer);
        glGetIntegerv (GL_PACK_ALIGNMENT, &alignment);
        glGetIntegerv (GL_PACK_ROW_LENGTH, &rowLength);
        glGetIntegerv (GL_PACK_SKIP_ROWS, &skipRows);
        glGetIntegerv (GL_PACK_SKIP_PIXELS, &skipPixels);
        glGetIntegerv (GL_PACK_SWAP_BYTES, &swapBytes);
        glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei (GL_PACK_ALIGNMENT, 4);
        glPixelStorei (GL_PACK_ROW_LENGTH, 0);
        glPixelStorei (GL_PACK_SKIP_ROWS, 0);
        glPixelStorei (GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei (GL_PACK_SWAP_BYTES, GL_FALSE);
        glBindFramebuffer (GL_READ_FRAMEBUFFER, target.getFramebuffer ());
        for (int index = 0; index < 4; ++index) {
            const int x = std::min (target.getRealWidth () - 1,
                (2 * index + 1) * target.getRealWidth () / 8);
            const int y = target.getRealHeight () / 2;
            float value[4] = {};
            glReadPixels (x, y, 1, 1, GL_RGBA, GL_FLOAT, value);
            sLog.debug ("HDR_PEEK ", label, " x=", x, " y=", y,
                        " rgb=", value[0], ",", value[1], ",", value[2]);
        }
        const auto& screenshot = getContext ().getApp ().getContext ().settings.screenshot;
        if (screenshot.take && !screenshot.path.empty ()) {
            const int width = target.getRealWidth ();
            const int height = target.getRealHeight ();
            std::vector<float> rgba (static_cast<size_t> (width) * height * 4);
            glReadPixels (0, 0, width, height, GL_RGBA, GL_FLOAT, rgba.data ());
            const auto path = screenshot.path.parent_path () /
                (std::string ("hdr-") + label + "-f32.rgba");
            std::ofstream stream (path, std::ios::binary);
            stream.write (reinterpret_cast<const char*> (rgba.data ()),
                          static_cast<std::streamsize> (rgba.size () * sizeof (float)));
            if (!stream) sLog.exception ("Cannot write HDR debug readback ", path);
            sLog.debug ("HDR_PEEK ", label, " raw=", path, " size=", width, "x", height);
        }
        glBindFramebuffer (GL_READ_FRAMEBUFFER, previousRead);
        glBindBuffer (GL_PIXEL_PACK_BUFFER, previousPackBuffer);
        glPixelStorei (GL_PACK_ALIGNMENT, alignment);
        glPixelStorei (GL_PACK_ROW_LENGTH, rowLength);
        glPixelStorei (GL_PACK_SKIP_ROWS, skipRows);
        glPixelStorei (GL_PACK_SKIP_PIXELS, skipPixels);
        glPixelStorei (GL_PACK_SWAP_BYTES, swapBytes);
    };
    if (peek) peekTarget (*m_sceneFBO, "scene");
    glBindVertexArray (m_vaoBuffer);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_CULL_FACE);
    glDisable (GL_BLEND);
    glUseProgram (m_hdrPyramidShader);
    glUniform1i (glGetUniformLocation (m_hdrPyramidShader, "u_source"), 0);
    const float threshold = m_hdrBloomSettings.threshold;
    const float knee = threshold * m_hdrBloomSettings.feather;
    const float scatter = m_hdrBloomSettings.scatter;
    const float strength = m_hdrBloomSettings.strength /
        (std::pow (scatter, static_cast<float> (std::max<size_t> (m_hdrActiveLevels, 2) - 2)) + 1.0f);
    glUniform1f (glGetUniformLocation (m_hdrPyramidShader, "u_strength"), strength);
    glUniform4f (glGetUniformLocation (m_hdrPyramidShader, "u_blend"), threshold,
                 threshold - knee, 2.0f * knee, 0.25f / (knee + 0.00001f));
    glUniform3fv (glGetUniformLocation (m_hdrPyramidShader, "u_tint"), 1, &m_hdrBloomSettings.tint[0]);
    glUniform1f (glGetUniformLocation (m_hdrPyramidShader, "u_scatter"), scatter);
    for (size_t index = 0; index < m_hdrActiveLevels; ++index) {
        const auto& source = index == 0 ? m_sceneFBO : m_hdrPyramid[index - 1];
        const auto& target = m_hdrPyramid[index];
        glBindFramebuffer (GL_FRAMEBUFFER, target->getFramebuffer ());
        glViewport (0, 0, target->getRealWidth (), target->getRealHeight ());
        glActiveTexture (GL_TEXTURE0);
        glBindTexture (GL_TEXTURE_2D, source->getTextureID (0));
        glUniform2f (glGetUniformLocation (m_hdrPyramidShader, "u_texel"),
                     std::ldexp (1.0f, static_cast<int> (index)) / m_sceneFBO->getRealWidth (),
                     std::ldexp (1.0f, static_cast<int> (index)) / m_sceneFBO->getRealHeight ());
        glUniform1i (glGetUniformLocation (m_hdrPyramidShader, "u_bloom"), index == 0);
        glUniform1i (glGetUniformLocation (m_hdrPyramidShader, "u_upsample"), 0);
        glUniform1i (glGetUniformLocation (m_hdrPyramidShader, "u_bicubic"), 0);
        glDrawArrays (GL_TRIANGLES, 0, 3);
    }
    if (peek && m_hdrActiveLevels > 1) {
        peekTarget (*m_hdrPyramid[0], "level0-down");
        peekTarget (*m_hdrPyramid[1], "level1-down");
    }
    glEnable (GL_BLEND);
    glBlendFunc (GL_ONE, GL_ONE);
    glUniform1i (glGetUniformLocation (m_hdrPyramidShader, "u_bloom"), 0);
    glUniform1i (glGetUniformLocation (m_hdrPyramidShader, "u_upsample"), 1);
    for (size_t index = m_hdrActiveLevels; index > 1; --index) {
        const auto& source = m_hdrPyramid[index - 1];
        const auto& target = m_hdrPyramid[index - 2];
        glBindFramebuffer (GL_FRAMEBUFFER, target->getFramebuffer ());
        glViewport (0, 0, target->getRealWidth (), target->getRealHeight ());
        glBindTexture (GL_TEXTURE_2D, source->getTextureID (0));
        glUniform2f (glGetUniformLocation (m_hdrPyramidShader, "u_texel"),
                     std::ldexp (1.0f, static_cast<int> (index) - 1) / m_sceneFBO->getRealWidth (),
                     std::ldexp (1.0f, static_cast<int> (index) - 1) / m_sceneFBO->getRealHeight ());
        glUniform1i (glGetUniformLocation (m_hdrPyramidShader, "u_bicubic"),
                     index >= m_hdrActiveLevels - 1);
        glDrawArrays (GL_TRIANGLES, 0, 3);
    }
    glDisable (GL_BLEND);
    if (peek) {
        peekTarget (*m_hdrBloom, "bloom");
        m_hdrPeekDone = true;
    }
    glBindFramebuffer (GL_FRAMEBUFFER, m_hdrOutput->getFramebuffer ());
    glViewport (0, 0, m_hdrOutput->getRealWidth (), m_hdrOutput->getRealHeight ());
    glBindVertexArray (m_vaoBuffer);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_CULL_FACE);
    glUseProgram (m_hdrShader);
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, m_sceneFBO->getTextureID (0));
    glActiveTexture (GL_TEXTURE1);
    glBindTexture (GL_TEXTURE_2D, m_hdrBloom->getTextureID (0));
    glActiveTexture (GL_TEXTURE0);
    glUniform1i (m_hdrTexture0, 0);
    glUniform1i (m_hdrTexture1, 1);
    // Native g_TexelSize (builtin ID 7) uses full scene dimensions even
    // though g_Texture1 samples the half-resolution bloom pyramid.
    glUniform2f (m_hdrTexelSize, 1.0f / m_sceneFBO->getRealWidth (),
                 1.0f / m_sceneFBO->getRealHeight ());
    constexpr GLfloat texCoords[] = {0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1};
    glEnableVertexAttribArray (m_hdrTexCoord);
    glBindBuffer (GL_ARRAY_BUFFER, m_texCoordBuffer);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texCoords), texCoords, GL_STATIC_DRAW);
    glVertexAttribPointer (m_hdrTexCoord, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray (m_hdrPosition);
    glBindBuffer (GL_ARRAY_BUFFER, m_hdrPositionBuffer);
    glVertexAttribPointer (m_hdrPosition, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays (GL_TRIANGLES, 0, 6);
}

void CWallpaper::setDestinationFramebuffer (GLuint framebuffer) { this->m_destFramebuffer = framebuffer; }

void CWallpaper::setSpanInfo (const SpanInfo& spanInfo) { this->m_spanInfo = spanInfo; }

const CWallpaper::SpanInfo* CWallpaper::getSpanInfo () const {
    return this->m_spanInfo.has_value () ? &this->m_spanInfo.value () : nullptr;
}

void CWallpaper::updateUVs (const glm::ivec4& viewport, const bool vflip) {
    // update UVs if something has changed, otherwise use old values
    if (this->m_state.hasChanged (viewport, vflip, this->getWidth (), this->getHeight ())) {
	// Update wallpaper state
	this->m_state.updateState (viewport, vflip, this->getWidth (), this->getHeight ());
    }
}

void CWallpaper::render (
    const glm::ivec4& viewport, const bool vflip, const glm::ivec2& globalPosition, const glm::ivec2& logicalSize
) {
    // Get current frame counter from the driver to avoid redundant scene renders
    const uint32_t currentFrame = this->getContext ().getDriver ().getFrameCounter ();
    const bool needsSceneRender = (currentFrame != this->m_lastRenderedFrame);
    const glm::ivec4 sceneViewport = this->m_spanInfo.has_value ()
	? glm::ivec4 { 0, 0, this->m_spanInfo->totalBounds.z, this->m_spanInfo->totalBounds.w }
	: viewport;

#if !NDEBUG
    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, "Rendering scene");
#endif /* !NDEBUG */
    if (needsSceneRender) {
	this->renderFrame (sceneViewport);
	this->combineHdrFrame ();
	this->m_lastRenderedFrame = currentFrame;
    }
#if !NDEBUG
    glPopDebugGroup ();
    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, "Rendering scene to output");
#endif /* !NDEBUG */

    float ustart, uend, vstart, vend;

    if (this->m_spanInfo.has_value ()) {
	// Span mode: treat bounding box as virtual viewport, scale wallpaper using
	// the normal scaling rules (fill/fit/stretch/default), then slice per monitor.
	const auto& span = this->m_spanInfo.value ();
	const float spanW = static_cast<float> (span.totalBounds.z);
	const float spanH = static_cast<float> (span.totalBounds.w);
	const float spanX = static_cast<float> (span.totalBounds.x);
	const float spanY = static_cast<float> (span.totalBounds.y);

	// Compute base UVs for the wallpaper scaled to the bounding box
	this->updateUVs (span.totalBounds, vflip);
	auto [baseUstart, baseUend, baseVstart, baseVend] = this->m_state.getTextureUVs ();
	if (rendersAtOutputSize ()) {
	    baseUstart = 0.0f;
	    baseUend = 1.0f;
	    baseVstart = vflip ? 0.0f : 1.0f;
	    baseVend = vflip ? 1.0f : 0.0f;
	}

	// This viewport's relative position within the bounding box [0..1]
	// Use logicalSize (same coordinate space as globalPosition and totalBounds)
	const float relLeft = (static_cast<float> (globalPosition.x) - spanX) / spanW;
	const float relRight = (static_cast<float> (globalPosition.x + logicalSize.x) - spanX) / spanW;
	const float relTop = (static_cast<float> (globalPosition.y) - spanY) / spanH;
	const float relBottom = (static_cast<float> (globalPosition.y + logicalSize.y) - spanY) / spanH;

	// Interpolate within the base UVs to get this viewport's slice
	const float baseURange = baseUend - baseUstart;
	const float baseVRange = baseVend - baseVstart;

	ustart = baseUstart + relLeft * baseURange;
	uend = baseUstart + relRight * baseURange;
	vstart = baseVstart + relTop * baseVRange;
	vend = baseVstart + relBottom * baseVRange;

	// Log span debug info only on first few frames
	if (this->m_lastRenderedFrame < 5) {
	    sLog.debug (
		"SPAN DEBUG: viewport=", viewport.z, "x", viewport.w, " globalPos=(", globalPosition.x, ",",
		globalPosition.y, ")", " span=(", span.totalBounds.x, ",", span.totalBounds.y, ",", span.totalBounds.z,
		",", span.totalBounds.w, ")", " rel=[", relLeft, ",", relRight, "]x[", relTop, ",", relBottom, "]",
		" baseUV=[", baseUstart, ",", baseUend, "]x[", baseVstart, ",", baseVend, "]", " finalUV=[", ustart,
		",", uend, "]x[", vstart, ",", vend, "]"
	    );
	}
    } else {
	// Normal mode: compute UVs based on viewport dimensions and wallpaper resolution
	updateUVs (viewport, vflip);
	auto uvs = this->m_state.getTextureUVs ();
	ustart = uvs.ustart;
	uend = uvs.uend;
	vstart = uvs.vstart;
	vend = uvs.vend;
        if (rendersAtOutputSize ()) {
            ustart = 0.0f;
            uend = 1.0f;
            vstart = vflip ? 0.0f : 1.0f;
            vend = vflip ? 1.0f : 0.0f;
        }
    }

    const GLfloat texCoords[] = {
	ustart, vstart, uend, vstart, ustart, vend, ustart, vend, uend, vstart, uend, vend,
    };

    glViewport (viewport.x, viewport.y, viewport.z, viewport.w);

    glBindFramebuffer (GL_FRAMEBUFFER, this->m_destFramebuffer);

    glBindVertexArray (this->m_vaoBuffer);

    glDisable (GL_BLEND);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_CULL_FACE);
    // do not use any shader
    glUseProgram (m_shader);
    // activate scene texture
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, this->getWallpaperTexture ());
    // set uniforms and attribs
    const GLint texCoordAttribute = a_TexCoord;
    const GLint positionAttribute = a_Position;
    glEnableVertexAttribArray (texCoordAttribute);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texCoordBuffer);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texCoords), texCoords, GL_STATIC_DRAW);
    glVertexAttribPointer (texCoordAttribute, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glEnableVertexAttribArray (positionAttribute);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_positionBuffer);
    glVertexAttribPointer (positionAttribute, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glUniform1i (this->g_Texture0, 0);
    // write the framebuffer as is to the screen
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texCoordBuffer);
    glDrawArrays (GL_TRIANGLES, 0, 6);

#if !NDEBUG
    glPopDebugGroup ();
#endif /* !NDEBUG */
}

void CWallpaper::setPause (bool newState) { }

void CWallpaper::setupFramebuffers (TextureFormat format) {
    const uint32_t width = this->getWidth ();
    const uint32_t height = this->getHeight ();
    const uint32_t clamp = this->m_state.getClampingMode ();

    // create framebuffer for the scene
    this->m_sceneFBO = this->create (
	"_rt_FullFrameBuffer", format, clamp, 1.0, { width, height }, { width, height }
    );

}

AudioContext& CWallpaper::getAudioContext () const { return this->m_audioContext; }

const WallpaperState& CWallpaper::getState () const { return this->m_state; }

std::shared_ptr<const CFBO> CWallpaper::findFBO (const std::string& name) const {
    if (name == "_rt_Reflection" || name == "_alias_NullShaderResource") {
        auto& target = name == "_rt_Reflection" ? m_reflectionSceneFBO : m_nullShaderResourceFBO;
        if (!target) {
            GLint draw = 0, read = 0, texture = 0;
            glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw);
            glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read);
            glGetIntegerv (GL_TEXTURE_BINDING_2D, &texture);
            const auto width = name == "_rt_Reflection" ? m_sceneFBO->getTextureWidth (0) : 1u;
            const auto height = name == "_rt_Reflection" ? m_sceneFBO->getTextureHeight (0) : 1u;
            target = std::make_shared<CFBO> (name, TextureFormat_ARGB8888, TextureFlags_ClampUVs,
                                           1.0f, width, height, width, height);
            glBindFramebuffer (GL_DRAW_FRAMEBUFFER, draw);
            glBindFramebuffer (GL_READ_FRAMEBUFFER, read);
            glBindTexture (GL_TEXTURE_2D, texture);
        }
        return target;
    }
    if (name == "_rt_MipMappedFrameBuffer") {
        if (!m_mipMappedSceneFBO) {
            // Native allocates RGB storage independently of the scene RT.
            // Never sample the framebuffer that the current layer is writing.
            GLint draw = 0, read = 0, texture = 0;
            glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw);
            glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read);
            glGetIntegerv (GL_TEXTURE_BINDING_2D, &texture);
            const auto width = std::max (2u, m_sceneFBO->getTextureWidth (0));
            const auto height = std::max (2u, m_sceneFBO->getTextureHeight (0));
            m_mipMappedSceneFBO = std::make_shared<CFBO> (
                name, m_sceneFBO->getFormat () == TextureFormat_RGBA16161616f
                    ? TextureFormat_RGB161616f : TextureFormat_RGB888,
                TextureFlags_ClampUVs, 1.0f, width, height, width, height);
            m_mipMappedSceneFBO->setMaxAnisotropy (1.0f);
            m_mipMappedSceneFBO->enableSceneReflectionMipmaps ();
            glBindFramebuffer (GL_DRAW_FRAMEBUFFER, draw);
            glBindFramebuffer (GL_READ_FRAMEBUFFER, read);
            glBindTexture (GL_TEXTURE_2D, texture);
        }
        return m_mipMappedSceneFBO;
    }
    const auto fbo = this->find (name);

    if (fbo == nullptr) {
	sLog.exception ("Cannot find FBO ", name);
    }

    return fbo;
}

std::shared_ptr<const CFBO> CWallpaper::getFBO () const { return this->m_sceneFBO; }

std::unique_ptr<CWallpaper> CWallpaper::fromWallpaper (
    const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
    WebBrowser::WebBrowserContext* browserContext, const WallpaperState::TextureUVsScaling& scalingMode,
    const uint32_t& clampMode
) {
    if (wallpaper.is<Scene> ()) {
	return std::make_unique<WallpaperEngine::Render::Wallpapers::CScene> (
	    wallpaper, context, audioContext, scalingMode, clampMode
	);
    }

    if (wallpaper.is<Video> ()) {
	return std::make_unique<WallpaperEngine::Render::Wallpapers::CVideo> (
	    wallpaper, context, audioContext, scalingMode, clampMode
	);
    }

    if (wallpaper.is<Web> ()) {
	return std::make_unique<WallpaperEngine::Render::Wallpapers::CWeb> (
	    wallpaper, context, audioContext, *browserContext, scalingMode, clampMode
	);
    }

    sLog.exception ("Unsupported wallpaper type");
}
