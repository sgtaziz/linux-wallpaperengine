#pragma once

#include "NativeSpectrumMapping.h"
#include "kiss_fft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace WallpaperEngine::Audio::Drivers::Recorders {

/** Native capture prefix with a silent FFT tail, analyzed independently per channel. */
class StereoSpectrum {
public:
    static constexpr size_t Samples = 1920;
    static constexpr size_t WindowBytes = Samples * 2 * sizeof (float);
    struct Bands {
        std::array<std::array<float, 16>, 2> audio16 {};
        std::array<std::array<float, 32>, 2> audio32 {};
        std::array<std::array<float, 64>, 2> audio64 {};
        std::array<float, 16> combined16 {};
        std::array<float, 32> combined32 {};
        std::array<float, 64> combined64 {};
    };

    // wallpaper64.exe v2.8.42 FUN_140110630: form the third 64-band block
    // before taking adjacent maxima to derive the 32- and 16-band blocks.
    [[nodiscard]] static Bands from64 (const std::array<float, 64>& left,
                                       const std::array<float, 64>& right) {
        Bands bands;
        bands.audio64[0] = left;
        bands.audio64[1] = right;
        for (size_t i = 0; i < 64; ++i) {
            bands.combined64[i] = (left[i] + right[i]) * 0.5f;
        }
        for (size_t i = 0; i < 32; ++i) {
            for (size_t channel = 0; channel < 2; ++channel) {
                bands.audio32[channel][i] = std::max (bands.audio64[channel][2 * i],
                                                      bands.audio64[channel][2 * i + 1]);
            }
            bands.combined32[i] = std::max (bands.combined64[2 * i], bands.combined64[2 * i + 1]);
        }
        for (size_t i = 0; i < 16; ++i) {
            for (size_t channel = 0; channel < 2; ++channel) {
                bands.audio16[channel][i] = std::max (bands.audio32[channel][2 * i],
                                                      bands.audio32[channel][2 * i + 1]);
            }
            bands.combined16[i] = std::max (bands.combined32[2 * i], bands.combined32[2 * i + 1]);
        }
        return bands;
    }

    // The host scene smooths each 64-band channel before producing its
    // 32/16-band views. maxDelta is the current Linux interpolation policy;
    // native temporal coefficients are still being recovered.
    static void advancePublished (Bands& published, const Bands& destination, float maxDelta) {
        std::array<float, 64> left = published.audio64[0];
        std::array<float, 64> right = published.audio64[1];
        for (size_t i = 0; i < 64; ++i) {
            const float leftDelta = destination.audio64[0][i] - left[i];
            const float rightDelta = destination.audio64[1][i] - right[i];
            left[i] += std::clamp (leftDelta, -maxDelta, maxDelta);
            right[i] += std::clamp (rightDelta, -maxDelta, maxDelta);
        }
        published = from64 (left, right);
    }

    StereoSpectrum () : StereoSpectrum (44100) {}
    explicit StereoSpectrum (unsigned sampleRate) {
        configureSampleRate (sampleRate);
        m_ready = false;
    }
    ~StereoSpectrum () {
        std::free (m_fft);
        std::free (m_workForward);
        std::free (m_workInverse);
    }
    StereoSpectrum (const StereoSpectrum&) = delete;
    StereoSpectrum& operator= (const StereoSpectrum&) = delete;

    [[nodiscard]] unsigned sampleRate () const { return m_sampleRate; }
    [[nodiscard]] size_t samples () const { return m_samples; }
    [[nodiscard]] size_t windowBytes () const { return m_windowBytes; }
    [[nodiscard]] size_t captureSamples () const { return m_captureSamples; }
    [[nodiscard]] size_t captureBytes () const { return m_captureSamples * 2 * sizeof (float); }

    void configureSampleRate (unsigned sampleRate) {
        if (sampleRate == 0) throw std::invalid_argument ("Capture sample rate must be nonzero");
        const size_t samples = nativeSpectrumFftLength (sampleRate);
        if (samples < 640 || samples > static_cast<size_t> (std::numeric_limits<int>::max ()))
            throw std::invalid_argument ("Unsupported spectrum FFT length");
        if (sampleRate == m_sampleRate && m_fft) { reset (); return; }
        const size_t bytes = samples * 2 * sizeof (float);
        std::vector<uint8_t> pending (bytes), latest (bytes);
        std::vector<kiss_fft_cpx> input (samples), frequency (samples);
        // KissFFT's generic butterfly is quadratic when a long prime factor
        // remains (2,089 at 48 kHz is prime). Native uses a chirp convolution
        // for non-power-of-two lengths; keep the established fast 1,920 path.
        size_t remaining = samples;
        for (size_t factor : {2, 3, 5, 7, 11, 13})
            while (remaining % factor == 0) remaining /= factor;
        const bool useBluestein = remaining > 1;
        kiss_fft_cfg fft = nullptr, workForward = nullptr, workInverse = nullptr;
        size_t workSize = 0;
        std::vector<kiss_fft_cpx> chirp, work, kernelSpectrum, product, convolution;
        if (useBluestein) {
            if (samples > (static_cast<size_t> (std::numeric_limits<int>::max ()) + 1) / 2)
                throw std::invalid_argument ("Unsupported spectrum convolution length");
            workSize = 1;
            while (workSize < 2 * samples - 1) workSize *= 2;
            if (workSize > static_cast<size_t> (std::numeric_limits<int>::max ()))
                throw std::invalid_argument ("Unsupported spectrum convolution length");
            chirp.resize (samples);
            work.resize (workSize);
            kernelSpectrum.resize (workSize);
            product.resize (workSize);
            convolution.resize (workSize);
            workForward = kiss_fft_alloc (static_cast<int> (workSize), 0, nullptr, nullptr);
            workInverse = kiss_fft_alloc (static_cast<int> (workSize), 1, nullptr, nullptr);
            if (!workForward || !workInverse) {
                std::free (workForward);
                std::free (workInverse);
                throw std::bad_alloc ();
            }
            constexpr double pi = 3.14159265358979323846;
            for (size_t index = 0; index < samples; ++index) {
                const double phase = pi * static_cast<double> (index) * index / samples;
                const float real = static_cast<float> (std::cos (phase));
                const float imaginary = static_cast<float> (std::sin (phase));
                chirp[index] = { real, -imaginary };
                work[index] = { real, imaginary };
                if (index) work[workSize - index] = work[index];
            }
            kiss_fft (workForward, work.data (), kernelSpectrum.data ());
            std::fill (work.begin (), work.end (), kiss_fft_cpx {});
        } else {
            fft = kiss_fft_alloc (static_cast<int> (samples), 0, nullptr, nullptr);
            if (!fft) throw std::bad_alloc ();
        }
        std::free (m_fft);
        std::free (m_workForward);
        std::free (m_workInverse);
        m_fft = fft;
        m_workForward = workForward;
        m_workInverse = workInverse;
        m_workSize = workSize;
        m_sampleRate = sampleRate;
        m_samples = samples;
        m_captureSamples = nativeSpectrumCaptureLength (static_cast<unsigned> (samples));
        m_windowBytes = bytes;
        m_pending.swap (pending);
        m_latest.swap (latest);
        m_input.swap (input);
        m_frequency.swap (frequency);
        m_chirp.swap (chirp);
        m_work.swap (work);
        m_kernelSpectrum.swap (kernelSpectrum);
        m_product.swap (product);
        m_convolution.swap (convolution);
        reset ();
    }

    void feed (const uint8_t* bytes, size_t count) {
        if (!bytes || m_captureComplete) return;
        const size_t portion = std::min (count, captureBytes () - m_pendingSize);
        std::copy_n (bytes, portion, m_pending.begin () + m_pendingSize);
        m_pendingSize += portion;
        if (m_pendingSize == captureBytes ()) {
            m_latest = m_pending;
            m_pendingSize = 0;
            m_ready = true;
            m_captureComplete = true;
        }
        // Native releases the whole packet and drains the rest of the queue
        // after reaching its prefix ceiling (0x1400d1559–0x1400d1b38).
        // Ignore that excess until take() finishes this recorder update.
    }

    void feedSilence (size_t count) {
        constexpr std::array<uint8_t, 4096> silence {};
        while (count) {
            const size_t portion = std::min (count, silence.size ());
            feed (silence.data (), portion);
            count -= portion;
        }
    }

    void reset () {
        std::fill (m_pending.begin (), m_pending.end (), 0);
        std::fill (m_latest.begin (), m_latest.end (), 0);
        m_pendingSize = 0;
        m_captureComplete = false;
        m_ready = true;
    }

    // Capture errors and native silent packets clear raw publication directly;
    // they must not enqueue a synthetic baseline FFT on the next empty poll.
    void discard () {
        reset ();
        m_ready = false;
    }

    [[nodiscard]] bool take (Bands& bands) {
        if (!m_ready) return false;
        m_ready = false;
        m_captureComplete = false;
        std::array<std::array<float, 64>, 2> channels {};
        for (size_t channel = 0; channel < 2; ++channel) {
            for (size_t sample = 0; sample < m_samples; ++sample) {
                float value = 0.0f;
                std::memcpy (&value, m_latest.data () + (sample * 2 + channel) * sizeof (float), sizeof (value));
                const auto nativeInput = nativeSpectrumInput (std::isfinite (value) ? value : 0.0f);
                m_input[sample] = { nativeInput.r, nativeInput.i };
            }
            if (m_workForward) {
                std::fill (m_work.begin (), m_work.end (), kiss_fft_cpx {});
                for (size_t index = 0; index < m_samples; ++index)
                    m_work[index] = multiply (m_input[index], m_chirp[index]);
                kiss_fft (m_workForward, m_work.data (), m_product.data ());
                for (size_t index = 0; index < m_workSize; ++index)
                    m_work[index] = multiply (m_product[index], m_kernelSpectrum[index]);
                kiss_fft (m_workInverse, m_work.data (), m_convolution.data ());
                const float scale = 1.0f / static_cast<float> (m_workSize);
                for (size_t index = 0; index < m_samples; ++index) {
                    const auto value = multiply (m_convolution[index], m_chirp[index]);
                    m_frequency[index] = { value.r * scale, value.i * scale };
                }
            } else {
                kiss_fft (m_fft, m_input.data (), m_frequency.data ());
            }
            channels[channel] = nativeSpectrumReduce (std::span<const kiss_fft_cpx> (m_frequency),
                                                      640, static_cast<unsigned> (m_samples));
        }
        bands = from64 (channels[0], channels[1]);
        return true;
    }

private:
    [[nodiscard]] static kiss_fft_cpx multiply (kiss_fft_cpx lhs, kiss_fft_cpx rhs) {
        return {lhs.r * rhs.r - lhs.i * rhs.i, lhs.r * rhs.i + lhs.i * rhs.r};
    }

    kiss_fft_cfg m_fft = nullptr;
    kiss_fft_cfg m_workForward = nullptr;
    kiss_fft_cfg m_workInverse = nullptr;
    size_t m_workSize = 0;
    unsigned m_sampleRate = 0;
    size_t m_samples = 0;
    size_t m_captureSamples = 0;
    size_t m_windowBytes = 0;
    std::vector<uint8_t> m_pending;
    std::vector<uint8_t> m_latest;
    size_t m_pendingSize = 0;
    bool m_ready = false;
    bool m_captureComplete = false;
    std::vector<kiss_fft_cpx> m_input;
    std::vector<kiss_fft_cpx> m_frequency;
    std::vector<kiss_fft_cpx> m_chirp;
    std::vector<kiss_fft_cpx> m_work;
    std::vector<kiss_fft_cpx> m_kernelSpectrum;
    std::vector<kiss_fft_cpx> m_product;
    std::vector<kiss_fft_cpx> m_convolution;
};

} // namespace WallpaperEngine::Audio::Drivers::Recorders
