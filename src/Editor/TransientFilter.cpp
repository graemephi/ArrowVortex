#include <Editor/TransientFilter.h>

#include <Core/Utils.h>
#include <Core/Vector.h>
#include <System/Debug.h>

namespace Vortex {

static uint32_t RoundUpToPowerOfTwo(uint32_t x) {
    x--;
    x |= (x >> 1);
    x |= (x >> 2);
    x |= (x >> 4);
    x |= (x >> 8);
    x |= (x >> 16);
    x++;
    return x;
}

void TransientsFilter(const short* src, short* dst, int numFrames,
                      double samplerate, double strength) {
    TransientsGuidedFilter(src, dst, src, numFrames, samplerate, strength);
}

void TransientsGuidedFilter(const short* src, short* dst, const short* guide,
                            int numFrames, double samplerate, double strength) {
    // Default filter strength is 0.25 so define default values there and
    // extrapolate for other values in [0,1] Threshold is a ratio: activity must
    // be this many times follower to trigger
    constexpr double ThresholdAt025 = 1.5;
    constexpr double ThresholdAt1 = 4.0;
    constexpr double DecayMsAt025 = 125.0;
    constexpr double DecayMsAt1 = 300.0;

    double t = (strength - 0.25) / 0.75;
    double transientThreshold =
        ThresholdAt025 + t * (ThresholdAt1 - ThresholdAt025);
    double decayMs = DecayMsAt025 + t * (DecayMsAt1 - DecayMsAt025);

    constexpr double ActivityHalfLifeMs = 2.5;
    constexpr double FollowerHalfLifeMs = 25;

    double activityDecay =
        pow(2.0, -1.0 / (samplerate * ActivityHalfLifeMs / 1000.0));
    double followerDecay =
        pow(2.0, -1.0 / (samplerate * FollowerHalfLifeMs / 1000.0));
    double envelopeDecay = pow(2.0, -15.0 / (samplerate * decayMs / 1000.0));

    constexpr double LookbackMs = 5.0;
    uint32_t lookbackSamples =
        RoundUpToPowerOfTwo(uint32_t((samplerate * LookbackMs) / 1000.));
    uint32_t lookbackMask = lookbackSamples - 1u;
    Vector<double> transientLookback(lookbackSamples, 0.0);

    double activity = 0.0;
    double follower = 0.0;
    double guidePrev = 0.0;
    bool prevTriggered = false;
    Vector<double> triggers(numFrames, 0);
    Vector<double> followerHistory(numFrames, 0.0);

    for (uint32_t i = 0; i < uint32_t(numFrames); ++i) {
        double guideSample = double(guide[i]) / 32767.0;

        double diff = fabs(guideSample - guidePrev);
        activity = activity * activityDecay + diff;
        follower = lerp(activity, follower, followerDecay);
        bool triggered = false;

        if (follower > 0) {
            double transient = activity / follower;
            triggered = transient > transientThreshold;
            bool onset = triggered && !prevTriggered;

            transientLookback[i & lookbackMask] = transient;

            // On onset the threshold has been reached but it took time
            if (onset) {
                double lookbackThreshold = (1.0 + transient) * 0.25;
                uint32_t j = i;
                uint32_t maxLookback = min(i, lookbackSamples);
                for (uint32_t lookbackIndex = 0; lookbackIndex < maxLookback;
                     ++lookbackIndex) {
                    if (transientLookback[j & lookbackMask] <
                        lookbackThreshold) {
                        break;
                    }

                    j--;
                }

                triggers[j] = follower / activity;
            }
        }

        followerHistory[i] = follower ? follower / activity : 0.0;
        prevTriggered = triggered;
        guidePrev = guideSample;
    }

    double env = 0.0;
    for (int i = 0; i < numFrames; ++i) {
        env = max(env, double(triggers[i]));

        // Adhoc signal mangling so the output isn't just an exp decay on each
        // trigger
        double sample = double(src[i]) / 32767.0;
        double absGuide = abs(double(guide[i]) / 32767.0);
        double absSample = abs(sample);
        double scaled =
            8.0 * absGuide * (0.125 + 0.875 * env) / (1.0 + followerHistory[i]);
        double result = copysign(min(absSample, scaled), sample);
        dst[i] = short(clamp(result * 32767.0, -32767.0, 32767.0));

        // Decay n discard below 16 bits
        env *= envelopeDecay;
        env = (env >= 0x1p-16) ? env : 0.0;
    }
}

};  // namespace Vortex