#pragma once

#include <Core/Vector.h>

namespace Vortex {

struct BpmChange;
struct Onset;

enum class SyncMode {
    SingleBpm,  // No threshold on single-BPM detection - always use one BPM if
                // found
    Clean,      // Default behavior
    Beats,      // Snap onto nearby beats if it doesn't change the bpm too much
    SnapEverything  // Snap onto nearby rows
};

struct PreserveOptions {
    enum {
        None = 0,
        Preserve = 1 << 0,
        Region = 1 << 1,
    };

    int flags;
    int regionStartRow;
    int regionEndRow;
    double regionStartBpm;
    double regionEndBpm;
};

struct AutoSyncResult {
    Vector<BpmChange> bpmChanges;
    double offset;
    int regionEndRowDelta;
};

// Syncs teh file
AutoSyncResult AutoSync(const Vector<Onset>& onsets, int musicFrameCount,
                        int musicSampleRate,
                        const Vector<BpmChange>& bpmChanges, double offset,
                        const PreserveOptions preserveOptions, SyncMode mode);

// Refines onset positions to match visual transients using edge detection
void RefineOnsets(Vector<struct Onset>& onsets, const float* samples,
                  int samplerate, int numFrames);

};  // namespace Vortex
