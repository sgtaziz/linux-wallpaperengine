#include "FBOProvider.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Model;

namespace {
TextureFormat effectFormat (const std::string& format, TextureFormat backbufferFormat) {
    if (format == "rgba8888") return TextureFormat_ARGB8888;
    if (format == "rgb888") return TextureFormat_RGB888;
    if (format == "r8") return TextureFormat_R8;
    if (format == "rg88") return TextureFormat_RG88;
    if (format == "r16f") return TextureFormat_R16f;
    if (format == "rg1616f") return TextureFormat_RG1616f;
    if (format == "rgba16161616f") return TextureFormat_RGBA16161616f;
    if (format == "rgb161616f") return TextureFormat_RGB161616f;
    // Native selects RGBA8 for an SDR backbuffer and RGBA16F for an HDR one.
    // The provider's output format selects storage independently of the
    // literal effect string. Current scene providers default to SDR RGBA8;
    // an HDR output must explicitly pass RGBA16F when it is wired up.
    if (format == "rgba_backbuffer") return backbufferFormat;
    if (format == "rgb_backbuffer")
        return backbufferFormat == TextureFormat_RGBA16161616f ? TextureFormat_RGB161616f : TextureFormat_RGB888;
    throw std::invalid_argument ("Unsupported effect FBO format: " + format);
}

uint32_t targetDimension (float dimension, float scale) {
    if (!std::isfinite (dimension) || dimension <= 0.0f || !std::isfinite (scale) || scale <= 0.0f)
        throw std::invalid_argument ("FBO dimensions and scale must be positive and finite");
    const double scaled = static_cast<double> (dimension) / static_cast<double> (scale);
    if (scaled > std::numeric_limits<GLsizei>::max ())
        throw std::invalid_argument ("FBO dimension exceeds OpenGL size range");
    GLint hardwareLimit = 0;
    glGetIntegerv (GL_MAX_TEXTURE_SIZE, &hardwareLimit);
    if (hardwareLimit > 0 && scaled > hardwareLimit)
        throw std::invalid_argument ("FBO dimension exceeds hardware texture limit");
    // A target smaller than one pixel still needs valid storage. Keep the
    // existing truncation policy for other nonintegral sizes.
    return static_cast<uint32_t> (std::max (1.0, scaled));
}
}

FBOProvider::FBOProvider (FBOProvider* parent, TextureFormat backbufferFormat) :
    m_parent (parent),
    m_backbufferFormat (parent ? parent->m_backbufferFormat : backbufferFormat) {
    if (m_backbufferFormat != TextureFormat_ARGB8888 && m_backbufferFormat != TextureFormat_RGBA16161616f)
        throw std::invalid_argument ("Backbuffer FBO format must be RGBA8 or RGBA16F");
}

void FBOProvider::setBackbufferFormat (TextureFormat format) {
    if (m_parent || !m_fbos.empty () || !m_authoredTargets.empty () || !m_uniqueTargets.empty ())
        throw std::logic_error ("Backbuffer format must be set before target acquisition");
    if (format != TextureFormat_ARGB8888 && format != TextureFormat_RGBA16161616f)
        throw std::invalid_argument ("Backbuffer FBO format must be RGBA8 or RGBA16F");
    m_backbufferFormat = format;
}

FBOProvider* FBOProvider::root () {
    auto* provider = this;
    while (provider->m_parent) provider = provider->m_parent;
    return provider;
}

std::shared_ptr<CFBO> FBOProvider::create (const FBO& base, uint32_t flags, const glm::vec2 size) {
    if (base.uvs.has_value ()) {
        if (*base.uvs == "repeat") flags &= ~(TextureFlags_ClampUVs | TextureFlags_ClampUVsBorder);
        else if (*base.uvs == "clamp") {
            flags &= ~TextureFlags_ClampUVsBorder;
            flags |= TextureFlags_ClampUVs;
        } else throw std::invalid_argument ("Unsupported FBO UV mode: " + *base.uvs);
    }
    if (!std::isfinite (base.scale) || base.scale <= 0.0f)
        throw std::invalid_argument ("FBO scale must be positive and finite");
    // Native descriptor fields up to 4096 override each source axis. A fit
    // bound is applied to the longer side before the separate scale divisor.
    double width = base.width && *base.width <= 4096 ? *base.width : size.x;
    double height = base.height && *base.height <= 4096 ? *base.height : size.y;
    if (!std::isfinite (width) || width <= 0 || !std::isfinite (height) || height <= 0)
        throw std::invalid_argument ("FBO source dimensions must be positive and finite");
    if (base.fit && *base.fit <= 4096) {
        if (*base.fit == 0) throw std::invalid_argument ("FBO fit must be positive");
        if (width < height) {
            const auto fitted = std::min (height, static_cast<double> (*base.fit));
            width = std::max (1u, static_cast<uint32_t> (static_cast<float> (width) / static_cast<float> (height)
                                                        * static_cast<float> (fitted)));
            height = fitted;
        } else {
            const auto fitted = std::min (width, static_cast<double> (*base.fit));
            height = std::max (1u, static_cast<uint32_t> (static_cast<float> (height) / static_cast<float> (width)
                                                         * static_cast<float> (fitted)));
            width = fitted;
        }
    }
    const auto realWidth = targetDimension (static_cast<float> (width), base.scale);
    const auto realHeight = targetDimension (static_cast<float> (height), base.scale);
    const auto textureWidth = std::max (2u, realWidth);
    const auto textureHeight = std::max (2u, realHeight);
    const auto format = effectFormat (base.format, m_backbufferFormat);
    // Native effect descriptors first acquire by a scene-scoped resource
    // name. A cache hit reuses the target as-is; only a later setup of the
    // *same descriptor* resizes its retained target. Keep this acquisition
    // separate from logical mappings, which command passes can swap.
    if (const auto retained = m_authoredTargets.find (&base);
        retained != m_authoredTargets.end () && retained->second->getName () == base.name) {
        retained->second->resize (realWidth, realHeight, textureWidth, textureHeight);
        if (base.clear) retained->second->clear (*base.clear);
        // A command may have swapped the logical name since acquisition.
        // Resizing the descriptor's physical target must not reverse it.
        return retained->second;
    }

    std::shared_ptr<CFBO> target;
    if (base.unique) {
        auto& cache = root ()->m_uniqueTargets;
        if (const auto found = cache.find (base.name); found != cache.end ()) target = found->second.lock ();
    }
    if (target) {
        if (base.clear) target->clear (*base.clear);
    } else {
        target = std::make_shared<CFBO> (
            base.name, format, flags,
            base.scale, realWidth, realHeight, textureWidth, textureHeight, base.clear
        );
        if (base.unique) root ()->m_uniqueTargets[base.name] = target;
    }
    m_authoredTargets[&base] = target;
    return m_fbos[base.name] = target;
}

std::shared_ptr<CFBO> FBOProvider::create (
    const std::string& name, TextureFormat format, uint32_t flags, float scale, glm::vec2 realSize,
    glm::vec2 textureSize
) {
    const auto realWidth = targetDimension (realSize.x, 1.0f);
    const auto realHeight = targetDimension (realSize.y, 1.0f);
    const auto textureWidth = targetDimension (textureSize.x, 1.0f);
    const auto textureHeight = targetDimension (textureSize.y, 1.0f);
    if (!std::isfinite (scale) || scale <= 0.0f)
        throw std::invalid_argument ("FBO scale must be positive and finite");
    return this->m_fbos[name] = std::make_shared<CFBO> (
           name, format, flags, scale, realWidth, realHeight, textureWidth, textureHeight
       );
}

std::shared_ptr<CFBO> FBOProvider::alias (const std::string& newName, const std::string& original) {
    const auto source = find (original);
    if (!source) throw std::invalid_argument ("Cannot alias missing FBO: " + original);
    return this->m_fbos[newName] = source;
}

void FBOProvider::swap (const std::string& first, const std::string& second) {
    const auto firstTarget = find (first);
    const auto secondTarget = find (second);
    if (!firstTarget || !secondTarget)
        throw std::invalid_argument ("Cannot swap missing FBO: " + first + " or " + second);
    // Name lookup changes while the textures and their pixel contents stay put.
    // A child provider shadows inherited names; its parent's mappings stay intact.
    this->m_fbos[first] = secondTarget;
    this->m_fbos[second] = firstTarget;
}

std::shared_ptr<CFBO> FBOProvider::find (const std::string& name) const {
    if (const auto it = this->m_fbos.find (name); it != this->m_fbos.end ()) {
	return it->second;
    }

    if (this->m_parent == nullptr) {
	return nullptr;
    }

    return this->m_parent->find (name);
}

bool FBOProvider::eraseIfMappedTo (const std::string& name, const std::shared_ptr<CFBO>& expected) {
    if (!expected) return false;
    const auto it = m_fbos.find (name);
    if (it == m_fbos.end () || it->second != expected) return false;
    m_fbos.erase (it);
    return true;
}
