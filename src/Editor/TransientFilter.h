#pragma once

namespace Vortex {

// Reveals transients in src
void TransientsFilter(const short* src, short* dst, int numFrames,
                      double samplerate, double strength);

// Reveals transients in src using guide as a guide signal. Don't worry about it
void TransientsGuidedFilter(const short* src, short* dst, const short* guide,
                            int numFrames, double samplerate, double strength);

};  // namespace Vortex