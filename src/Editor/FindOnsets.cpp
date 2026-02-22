#include <Editor/FindOnsets.h>

#include <Core/Utils.h>
#include <Core/Vector.h>
#include <Core/AlignedMemory.h>

#include <System/Thread.h>

#include <aubio/aubio.h>
#include <math.h>
#include <mutex>

namespace Vortex {

// ================================================================================================
// Main function.

static int RoundDownToMultiple(int x, int m) { return (x / m) * m; }

void FindOnsets(const float* samples, int samplerate, int numFrames,
                int numThreads, Vector<Onset>& out) {
    static const int windowlen = 256;
    static const int bufsize = windowlen * 4;
    static const char* method = "complex";

    numThreads = max(numThreads, 1);

    struct Chunk {
        Vector<Onset> onsets;
    };
    Vector<Chunk> chunks(numThreads, {});

    ParallelFor(0, numThreads, [&](int item) {
        int framesPerItem = (numFrames + numThreads - 1) / numThreads;
        int beginPos = framesPerItem * item;
        int endPos = min(framesPerItem * (item + 1), numFrames);
        int paddedBegin =
            max(RoundDownToMultiple(beginPos - bufsize, bufsize), 0);
        int paddedEnd = min(endPos + bufsize, numFrames - windowlen + 1);

        auto onset = new_aubio_onset(method, bufsize, windowlen, samplerate);
        aubio_onset_set_threshold(onset, 0.3f);
        aubio_onset_set_minioi_ms(onset, 20.0f);
        aubio_onset_set_delay(onset, (uint_t)(4.3f * windowlen));
        aubio_onset_set_awhitening(onset, 0);
        aubio_onset_set_compression(onset, 0.0f);
        fvec_t *samplevec = new_fvec(windowlen), *beatvec = new_fvec(2);
        for (int i = paddedBegin; i < paddedEnd; i += windowlen) {
            memcpy(samplevec->data, samples + i, sizeof(float) * windowlen);
            aubio_onset_do(onset, samplevec, beatvec);
            if (beatvec->data[0] > 0) {
                int pos = aubio_onset_get_last(onset) + paddedBegin;
                if (pos >= beginPos && pos < endPos) {
                    chunks[item].onsets.push_back(
                        {pos, aubio_onset_get_descriptor(onset)});
                }
            }
        }
        del_fvec(samplevec);
        del_fvec(beatvec);
        del_aubio_onset(onset);
    });

    for (auto& chunk : chunks)
        for (auto& onset : chunk.onsets) out.push_back(onset);
}

};  // namespace Vortex