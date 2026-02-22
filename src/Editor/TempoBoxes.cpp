#include <Editor/TempoBoxes.h>

#include <Core/Draw.h>
#include <Core/Gui.h>
#include <Core/QuadBatch.h>
#include <Core/StringUtils.h>
#include <Core/Text.h>
#include <Core/Texture.h>
#include <Core/Utils.h>
#include <Core/Vector.h>
#include <Core/Xmr.h>

#include <Simfile/SegmentGroup.h>
#include <Simfile/TimingData.h>

#include <Managers/ChartMan.h>
#include <Managers/SimfileMan.h>
#include <Managers/TempoMan.h>

#include <Editor/Common.h>
#include <Editor/FindOnsets.h>
#include <Editor/History.h>
#include <Editor/Menubar.h>
#include <Editor/Music.h>
#include <Editor/Notefield.h>
#include <Editor/Selection.h>
#include <Editor/View.h>
#include <Editor/Waveform.h>

#include <System/Debug.h>
#include <System/System.h>

#include <algorithm>

namespace Vortex {

// ================================================================================================
// TempoBoxesImpl :: member data.

static const int MAX_WIDTH = 400;

static int SnapToPhase(int row, int phase, int rowsPerMeasure, int minRow) {
    int aligned = row - phase;
    int half = rowsPerMeasure / 2;
    int snapped = ((aligned + half) / rowsPerMeasure) * rowsPerMeasure + phase;
    if (snapped <= minRow) snapped += rowsPerMeasure;
    return snapped;
}

struct BpmAnchor {
    BpmChange seg;
    double time;
    double bpmOnlyCorrection;
    int rpm;
    int phase;
    bool dragging;
    double bpmTime() const { return time - bpmOnlyCorrection; }
};

struct DragState {
    bool active;
    bool hasMoved;
    bool altHeld;
    int startX, startY;
    int dragIndex;
    double cursorTime;
    double originalOffset;
    double offset;
    Vector<BpmAnchor> anchors;
    Vector<int> altRows;

    SegmentEdit edit;
    bool editValid;
};

static void Drag(DragState& drag, double dragTime, bool shift, bool alt) {
    auto& anchors = drag.anchors;
    BpmAnchor dragged = anchors[drag.dragIndex];
    double dragDelta = dragTime - dragged.time;

    // In the easy case we have 3 bpms, A -> B -> C. To move B, we need to
    // adjust A. To fix C in place, we need to adjust B.
    // In the annoying case the BPM is dragged past A or C.
    // Default: Inserts/removes measures to avoid warping the beat grid too
    // much.
    // Shift: Ripple. Moves all later BPMs in time as well. Dragging backwards
    // overwrites earlier BPMs.
    // Alt: Warp. Does not change the row number the BPMs lie on at all. Can
    // result in very low or high BPMs.

    // First move all the bpms to their new positions in time
    auto working = anchors;

    if (shift && dragDelta < 0.0) {
        for (auto& anchor : working) {
            if (anchor.seg.row == dragged.seg.row) break;
            if (anchor.time >= dragTime) anchor.time = INFINITY;
        }
    }

    bool pastDrag = false;
    for (auto& anchor : working) {
        if (shift && pastDrag) {
            anchor.time += dragDelta;
        } else if (anchor.dragging) {
            anchor.time += dragDelta;
            pastDrag = true;
        }
    }

    std::sort(working.begin(), working.end(),
              [](auto& a, auto& b) { return a.time < b.time; });

    // Now update every BPM change's row to one that makes sense given its new
    // time. The way we do this is by letting them move forward or backwards in
    // row space in increments of a measure, by however many measures it takes
    // to best preserve the current duration of a beat
    int rowOffset = 0;
    int count = working.size();
    for (int i = 1; i < count; ++i) {
        BpmAnchor& anchor = working[i];
        if (anchor.time == INFINITY) {
            count = i;
            break;
        }

        if (drag.altHeld) {
            anchor.seg.row = drag.altRows[i];
            continue;
        }

        anchor.seg.row += rowOffset;
        if (!anchor.dragging) continue;

        BpmAnchor prev = working[i - 1];
        bool nextDrags = (i < count - 1) && working[i + 1].dragging;

        double localBpm = prev.seg.bpm;
        if (!shift && i < count - 1 && !nextDrags) {
            int nextRow = working[i + 1].seg.row + rowOffset;
            double timeDelta = working[i + 1].bpmTime() - prev.bpmTime();
            double beatDelta = double(nextRow - prev.seg.row) / ROWS_PER_BEAT;
            localBpm = beatDelta * 60.0 / timeDelta;
        }

        double rowsPerSec = (localBpm / 60.0) * ROWS_PER_BEAT;
        double alignDelta = anchor.bpmTime() - prev.bpmTime();
        int rawRow = prev.seg.row + int(round(alignDelta * rowsPerSec));
        int snapped =
            SnapToPhase(rawRow, anchor.phase, anchor.rpm, prev.seg.row);

        if (i < count - 1 && !nextDrags) {
            BpmAnchor& next = working[i + 1];
            int nextRow = next.seg.row + rowOffset;
            double nextDelta = next.bpmTime() - anchor.bpmTime();
            int nextRaw = snapped + int(round(nextDelta * rowsPerSec));
            int nextSnapped =
                SnapToPhase(nextRaw, next.phase, next.rpm, snapped);
            rowOffset += nextSnapped - nextRow;
        }

        anchor.seg.row = snapped;
    }

    working.resize(count);

    // If drag moved the first anchor, rebase to row 0
    drag.offset = -working[0].time;
    bool offsetMoved =
        working[0].seg.row != 0 || working[0].time != anchors[0].time;
    if (offsetMoved) {
        int rowShift = working[0].seg.row;
        if (rowShift != 0) {
            for (int i = 0; i < count; ++i) working[i].seg.row -= rowShift;
        }
        // Recompute row structure from scratch
        for (int i = 1; i < count; ++i) {
            BpmAnchor& prev = working[i - 1];
            double rowsPerSec = (prev.seg.bpm / 60.0) * ROWS_PER_BEAT;
            double timeDelta = working[i].bpmTime() - prev.bpmTime();
            int rawRow = prev.seg.row + int(round(timeDelta * rowsPerSec));
            working[i].seg.row = SnapToPhase(rawRow, working[i].phase,
                                             working[i].rpm, prev.seg.row);
        }
    }

    // Snapshot rows on first alt frame
    if (alt && !drag.altHeld) {
        drag.altRows.resize(count);
        for (int i = 0; i < count; ++i) drag.altRows[i] = working[i].seg.row;
    }

    // Recompute BPMs from row/time pairs
    for (int i = 0; i < count - 1; ++i) {
        double rowDelta = working[i + 1].seg.row - working[i].seg.row;
        double beatDelta = rowDelta / ROWS_PER_BEAT;
        double timeDelta = working[i + 1].bpmTime() - working[i].bpmTime();
        working[i].seg.bpm = beatDelta * 60.0 / timeDelta;
    }

    // Build the edit
    SegmentEdit edit;
    for (auto& anchor : anchors) edit.rem.append(anchor.seg);
    for (int i = 0; i < count; ++i) edit.add.append(working[i].seg);

    drag.altHeld = alt;
    drag.edit = edit;
    drag.editValid = true;
    gTempo->setTweakEdit(edit, drag.offset);
}

static double SnapToOnset(double dragTime) {
    // One 16th at 120 BPM
    constexpr double SnapDistance = 0.125;

    auto onsets = gWaveform->getCurrentOnsetsInRange(dragTime - SnapDistance,
                                                     dragTime + SnapDistance);
    auto& music = gMusic->getSamples();
    if (!music.isCompleted()) return dragTime;

    double sampleRate = music.getFrequency();
    double bestDist = SnapDistance;
    for (auto& onset : onsets) {
        double onsetTime = onset.pos / sampleRate;
        double dist = fabs(onsetTime - dragTime);
        if (dist < bestDist) {
            bestDist = dist;
            dragTime = onsetTime;
        }
    }

    return dragTime;
}

struct TempoBoxesImpl : public TempoBoxes {
    TextStyle textStyle;
    Vector<TempoBox> myBoxes;
    int myMouseOverBox;
    TileRect myBoxBar;
    TileRect myBoxHl;

    bool myShowBoxes;
    bool myShowHelp;

    DragState myDrag;

    int myGhostRow;
    int myGhostY;

    // ================================================================================================
    // TempoBoxesImpl :: constructor and destructor.

    ~TempoBoxesImpl() = default;

    TempoBoxesImpl() {
        textStyle.textFlags |= Text::WRAP_LINE;

        myBoxBar.texture = myBoxHl.texture =
            Texture("assets/icons tempo.png", false);
        myBoxBar.border = myBoxHl.border = 8;
        myBoxBar.uvs = {0, 0, 1, 0.5f};
        myBoxHl.uvs = {0, 0.5f, 1, 1};

        myShowBoxes = true;
        myShowHelp = true;
        myDrag.active = false;
        myGhostRow = -1;
        myGhostY = 0;
    }

    // ================================================================================================
    // TempoBoxesImpl :: load / save settings.

    void loadSettings(XmrNode& settings) {
        XmrNode* view = settings.child("view");
        if (view) {
            view->get("showTempoHelp", &myShowHelp);
        }
    }

    void saveSettings(XmrNode& settings) override {
        XmrNode* view = settings.child("view");
        if (!view) view = settings.addChild("view");

        view->addAttrib("showTempoHelp", myShowHelp);
    }

    // ================================================================================================
    // TempoBoxesImpl :: update.

    void update() {
        myBoxes.clear();

        if (gSimfile->isClosed()) return;

        // Create a box for every segment.
        auto segments = gTempo->getSegments();
        for (const auto& segment : *segments) {
            auto type = segment.type();
            auto meta = Segment::meta[type];
            for (auto seg = segment.begin(), segEnd = segment.end();
                 seg != segEnd; ++seg) {
                std::string desc = meta->getDescription(seg.ptr);
                myBoxes.push_back(TempoBox{desc, seg->row, type, 0, 0, 0});
            }
        }

        // Sort the list of boxes by row.
        std::stable_sort(myBoxes.begin(), myBoxes.end(),
                         [](const TempoBox& a, const TempoBox& b) {
                             return (a.row < b.row);
                         });

        // Precalculate the x-position and width of each box.
        int previousRow = -1;
        int stacks[2] = {0, 0};
        for (TempoBox& box : myBoxes) {
            vec2i bounds =
                Text::arrange(Text::MC, textStyle, MAX_WIDTH, box.str.c_str());
            int width = max(32, bounds.x + 24);
            box.width = width;
            box.height = bounds.y + 16;

            int side = Segment::meta[box.type]->side;
            int offset = width * (side * 2 - 1);

            box.x = (side - 1) * width;
            if (box.row == previousRow) {
                box.x += stacks[side];
                stacks[side] += offset;
            } else {
                stacks[side] = offset;
                stacks[1 - side] = 0;
                previousRow = box.row;
            }
        }
    }

    void onChanges(int changes) override {
        if (changes & VCM_TEMPO_CHANGED) {
            update();
        }
    }

    // ================================================================================================
    // TempoBoxesImpl :: toggle visuals.

    void toggleShowBoxes() override {
        myShowBoxes = !myShowBoxes;
        gMenubar->update(Menubar::SHOW_TEMPO_BOXES);
    }

    void toggleShowHelp() override {
        myShowHelp = !myShowHelp;
        gMenubar->update(Menubar::SHOW_TEMPO_HELP);
    }

    bool hasShowBoxes() override { return myShowBoxes; }

    bool hasShowHelp() override { return myShowHelp; }

    // ================================================================================================
    // TempoBoxesImpl :: selection.

    void deselectAll() override {
        for (auto& box : myBoxes) {
            box.isSelected = 0;
        }
    }

    int selectAll() override {
        for (auto& box : myBoxes) {
            box.isSelected = 1;
        }
        return myBoxes.size();
    }

    int selectType(Segment::Type type) override {
        for (auto& box : myBoxes) {
            box.isSelected = (box.type == type ? 1 : 0);
        }
        return myBoxes.size();
    }

    int selectSegments(const Tempo* tempo) override {
        int numSelected = 0;
        auto boxEnd = myBoxes.end();
        auto segments = gTempo->getSegments();
        for (const auto& segment : *segments) {
            auto type = segment.type();
            auto box = myBoxes.begin();
            for (auto seg = segment.begin(), segEnd = segment.end();
                 seg != segEnd; ++seg) {
                while (box != boxEnd &&
                       (box->type != type || box->row < seg->row)) {
                    ++box;
                }
                bool select = (box != boxEnd && box->row == seg->row);
                box->isSelected = select;
                numSelected += select;
            }
        }
        return numSelected;
    }

    template <typename Predicate>
    int performSelection(SelectModifier mod, Predicate pred) {
        int numSelected = 0;
        auto box = myBoxes.begin();
        auto end = myBoxes.end();
        if (mod == SELECT_SET) {
            for (; box != end; ++box) {
                uint32_t set = pred(box);
                numSelected += set;
                box->isSelected = set;
            }
        } else if (mod == SELECT_ADD) {
            for (; box != end; ++box) {
                uint32_t set = pred(box);
                numSelected += set & (box->isSelected ^ 1);
                box->isSelected |= set;
            }
        } else if (mod == SELECT_SUB) {
            for (; box != end; ++box) {
                uint32_t set = pred(box);
                numSelected += set & box->isSelected;
                box->isSelected &= set ^ 1;
            }
        }
        return numSelected;
    }

    int selectRows(SelectModifier mod, int begin, int end, int xl,
                   int xr) override {
        auto coords = gView->getNotefieldCoords();
        const int baseX[2] = {coords.xl, coords.xr};
        return performSelection(mod, [&](const TempoBox* box) {
            int side = Segment::meta[box->type]->side;
            int x1 = baseX[side] + box->x + 8;
            int x2 = x1 + box->width - 16, row = box->row;
            return (x2 >= xl && x1 <= xr && row >= begin && row <= end);
        });
    }

    int selectTime(SelectModifier mod, double begin, double end, int xl,
                   int xr) override {
        auto coords = gView->getNotefieldCoords();
        const int baseX[2] = {coords.xl, coords.xr};

        TempoTimeTracker tracker;
        return performSelection(mod, [&](const TempoBox* box) {
            int side = Segment::meta[box->type]->side;
            int x1 = baseX[side] + box->x + 8;
            int x2 = x1 + box->width - 16;
            double time = tracker.advance(box->row);
            return (x2 >= xl && x1 <= xr && time >= begin && time <= end);
        });
    }

    bool noneSelected() const override {
        for (auto& box : myBoxes) {
            if (box.isSelected) return false;
        }
        return true;
    }

    // ================================================================================================
    // TempoBoxesImpl :: dragging.

    bool isDragging() const override { return myDrag.active; }

    void cancelDrag(bool remove = false) {
        if (myDrag.hasMoved) gTempo->stopTweaking(false);
        myDrag.active = false;

        if (remove) {
            auto& anchors = myDrag.anchors;
            int count = (int)anchors.size();
            SegmentEdit edit;

            for (int i = 1; i < count; ++i) {
                if (!anchors[i].dragging) continue;

                int groupEnd = i;
                while (groupEnd + 1 < count && anchors[groupEnd + 1].dragging)
                    ++groupEnd;

                for (int j = i; j <= groupEnd; ++j)
                    edit.rem.append(anchors[j].seg);

                if (groupEnd + 1 < count) {
                    BpmAnchor& prev = anchors[i - 1];
                    BpmAnchor& next = anchors[groupEnd + 1];
                    double beatDelta =
                        double(next.seg.row - prev.seg.row) / ROWS_PER_BEAT;
                    double timeDelta = next.time - prev.time;
                    double newBpm = beatDelta * 60.0 / timeDelta;

                    edit.rem.append(prev.seg);
                    BpmChange updated = prev.seg;
                    updated.bpm = newBpm;
                    edit.add.append(updated);
                }

                i = groupEnd;
            }

            if (edit.rem.numSegments() > 0) {
                gHistory->startChain();
                gTempo->modify(edit);
                gHistory->finishChain((edit.rem.numSegments() > 2)
                                          ? "Removed BPMs"
                                          : "Removed BPM");
            }
        }

        int newCursorRow = gTempo->timeToRow(myDrag.cursorTime);
        gView->setCursorRow(newCursorRow);
    }

    void onMousePress(MousePress& evt) override {
        if (myDrag.active && evt.button == Mouse::RMB && evt.unhandled()) {
            cancelDrag();
            evt.setHandled();
            return;
        }

        if (evt.button != Mouse::LMB || !evt.unhandled()) return;
        if (myMouseOverBox < 0 && myGhostRow < 0) return;
        if (!gView->isTimeBased()) return;
        if (!gMusic->isPaused()) return;
        if (gTempo->getTweakMode() != TempoMan::TWEAK_NONE) return;

        int targetRow;
        bool draggingGhost;
        if (myMouseOverBox >= 0) {
            auto& box = myBoxes[myMouseOverBox];
            if (box.type != Segment::BPM) return;
            targetRow = box.row;
            draggingGhost = false;
        } else {
            targetRow = myGhostRow;
            draggingGhost = true;
            myGhostRow = -1;
        }

        auto segments = gTempo->getSegments();

        myDrag.active = true;
        myDrag.hasMoved = false;
        myDrag.altHeld = false;
        myDrag.startX = evt.x;
        myDrag.startY = evt.y;
        myDrag.dragIndex = -1;
        myDrag.cursorTime = gTempo->rowToTime(gView->getCursorRow());
        myDrag.originalOffset = myDrag.offset = gTempo->getOffset();
        myDrag.anchors.clear();
        myDrag.altRows.clear();
        myDrag.editValid = false;

        // Build of a list of anchors from bpm segments. Delta from
        // true time, as compared to computing from bpm times alone, is tracked
        // so we don't have to care about stops, etc.
        auto it = segments->begin<BpmChange>();
        auto end = segments->end<BpmChange>();
        auto itBox = myBoxes.begin();
        double bpmOnlyTime = -gTempo->getOffset();
        bool ghostInserted = !draggingGhost;
        for (; it != end || !ghostInserted;) {
            // Splice in the ghost at the target row
            bool insertGhost =
                !ghostInserted && (it == end || it->row > targetRow);
            if (insertGhost) ghostInserted = true;

            BpmChange ghost = {};
            if (insertGhost) {
                ghost = segments->getRecent<BpmChange>(targetRow);
                ghost.row = targetRow;
            }
            const BpmChange& seg = insertGhost ? ghost : *it;

            if (seg.row == targetRow) myDrag.dragIndex = myDrag.anchors.size();

            BpmAnchor anchor = {};
            anchor.seg = seg;
            anchor.time = gTempo->rowToTime(seg.row);
            anchor.rpm =
                segments->getRecent<TimeSignature>(seg.row - 1).rowsPerMeasure;
            anchor.phase = seg.row % anchor.rpm;
            anchor.dragging = insertGhost || (seg.row == targetRow);
            while (itBox->row < it->row) ++itBox;
            if (itBox->isSelected && itBox->row == it->row)
                anchor.dragging = true;

            if (!myDrag.anchors.empty()) {
                auto& prev = myDrag.anchors.back();
                double rowDelta = seg.row - prev.seg.row;
                double rowsPerSec = (prev.seg.bpm / 60.0) * ROWS_PER_BEAT;
                bpmOnlyTime += rowDelta / rowsPerSec;
            }
            anchor.bpmOnlyCorrection = anchor.time - bpmOnlyTime;

            myDrag.anchors.push_back(anchor);

            if (!insertGhost) ++it;
        }

        VortexAssert(myDrag.dragIndex >= 0);
        evt.setHandled();
    }

    void onMouseRelease(MouseRelease& evt) override {
        if (!myDrag.active || evt.button != Mouse::LMB) return;

        if (myDrag.hasMoved) {
            // Commit whatever the last tick preview computed
            gTempo->stopTweaking(false);
            if (myDrag.editValid) {
                gHistory->startChain();
                if (myDrag.offset != myDrag.originalOffset)
                    gTempo->setOffset(myDrag.offset);
                gTempo->modify(myDrag.edit, false);
                gHistory->finishChain("Dragged BPM");

                int newCursorRow = gTempo->timeToRow(myDrag.cursorTime);
                gView->setCursorRow(newCursorRow);
            }
        }

        myDrag.active = false;
        evt.handled = true;
    }

    void onMouseScroll(MouseScroll& evt) override {
        if (myDrag.active) evt.handled = true;
    }

    void onKeyPress(KeyPress& evt) override {
        if (myDrag.active && !evt.handled) {
            switch (evt.key) {
                case Key::ESCAPE: {
                    cancelDrag();
                    evt.handled = true;
                    break;
                }
                case Key::DELETE:
                case Key::BACKSPACE:
                case Key::D:
                case Key::R: {
                    cancelDrag(true);
                    evt.handled = true;
                    break;
                }
            }
        }
    }

    // ================================================================================================
    // TempoBoxesImpl :: tick.

    void tick() override {
        myGhostRow = -1;
        bool canDrag = gMusic->isPaused() && gView->isTimeBased();
        if (myDrag.active) {
            if (!canDrag || !gSystem->isMouseDown(Mouse::LMB)) {
                cancelDrag();
                return;
            }

            vec2i mpos = gSystem->getMousePos();

            // Initiate dragging when the user starts dragging
            int dx = mpos.x - myDrag.startX;
            int dy = mpos.y - myDrag.startY;
            if (dx * dx + dy * dy > 9) {
                myDrag.hasMoved = true;
                gTempo->startDragTweakingBpm();
            }

            if (myDrag.hasMoved) {
                // We don't use yToOffset here as it introduces a feedback loop
                // when changing the offset
                double anchorTime = myDrag.anchors[myDrag.dragIndex].time;
                double dragDelta =
                    (mpos.y - myDrag.startY) / gView->getPixPerSec();
                double dragTime = anchorTime + dragDelta;
                bool ctrl = gSystem->getKeyFlags() & Keyflag::CTRL;
                bool shift = gSystem->getKeyFlags() & Keyflag::SHIFT;
                bool alt = gSystem->getKeyFlags() & Keyflag::ALT;
                if (ctrl) dragTime = SnapToOnset(dragTime);

                Drag(myDrag, dragTime, shift, alt);

                int newCursorRow = gTempo->timeToRow(myDrag.cursorTime);
                gView->setCursorRow(newCursorRow);
            }

            gSystem->setCursor(Cursor::HIDDEN);
        }

        myMouseOverBox = -1;
        if (!myDrag.active && !GuiMain::isCapturingMouse()) {
            bool timeBased = gView->isTimeBased();
            double oy = gView->offsetToY(0.0);
            double dy = gView->getPixPerOfs();

            TempoTimeTracker tracker;
            auto coords = gView->getNotefieldCoords();
            const int baseX[2] = {coords.xl, coords.xr};
            vec2i mpos = gSystem->getMousePos();
            for (int i = 0; i < myBoxes.size(); ++i) {
                auto& box = myBoxes[i];
                int y = static_cast<int>(
                    oy + dy * (timeBased ? tracker.advance(box.row)
                                         : static_cast<double>(box.row)));
                int side = Segment::meta[box.type]->side;
                int x = baseX[side] + box.x;
                if (IsInside(
                        recti{x, y - (box.height / 2), box.width, box.height},
                        mpos.x, mpos.y)) {
                    myMouseOverBox = i;
                    break;
                }
            }

            // Set up ghost BPM if hovering over the BPM side
            myGhostRow = -1;
            int bpmBaseX = baseX[1];
            bool hoveringX =
                (mpos.x >= bpmBaseX - 20) && (mpos.x <= bpmBaseX + 80);
            ChartOffset mouseOffset = gView->yToOffset(mpos.y);
            if (myMouseOverBox == -1 && canDrag && hoveringX) {
                bool alt = gSystem->getKeyFlags() & Keyflag::ALT;
                int row = gView->offsetToRow(mouseOffset);
                int quantization = gNotefield->hasShowBeatLinesSnap() &&
                                           gView->getSnapType() > ST_4TH
                                       ? 192 / gView->getSnapQuant()
                                       : ROWS_PER_BEAT;
                int snapRow = alt ? row
                                  : ((row + quantization / 2) / quantization *
                                     quantization);
                int snapY = gView->rowToY(snapRow);
                bool hoveringY = abs(snapY - mpos.y) < 12;
                BpmChange existing =
                    gTempo->getSegments()->getRecent<BpmChange>(snapRow);
                if (hoveringY && existing.row != snapRow) {
                    myGhostRow = snapRow;
                    myGhostY = snapY;
                }
            }
        }
    }

    // ================================================================================================
    // TempoBoxesImpl :: draw.

    void draw() override {
        if (myShowBoxes == false || myBoxes.empty() ||
            gView->getScaleLevel() < 2)
            return;

        auto coords = gView->getNotefieldCoords();
        const int baseX[2] = {coords.xl, coords.xr};

        bool timeBased = gView->isTimeBased();
        double oy = gView->offsetToY(0.0);
        double dy = gView->getPixPerOfs();
        int viewTop = gView->getRect().y;
        int viewBtm = viewTop + gView->getHeight();

        Renderer::resetColor();
        Renderer::bindTexture(myBoxHl.texture.handle());
        Renderer::bindShader(Renderer::SH_TEXTURE);

        // First pass, draw the box sprites.
        int previousRow = 0;
        TempoTimeTracker tracker;
        auto batch = Renderer::batchTC();
        for (const TempoBox& box : myBoxes) {
            int y = static_cast<int>(
                oy + dy * (timeBased ? tracker.advance(box.row)
                                     : static_cast<double>(box.row)));
            if (y < viewTop - 16 || y > viewBtm + 16) continue;

            int side = Segment::meta[box.type]->side;
            int x = baseX[side] + box.x;

            int flags = side * TileBar::FLIP_H;
            recti r = {x, y - (box.height / 2), box.width, box.height};

            uint32_t color = Segment::meta[box.type]->color;
            myBoxBar.draw(&batch, r, color, flags);
            if (box.isSelected) myBoxHl.draw(&batch, r, Colors::white, flags);

            previousRow = box.row;
        }
        batch.flush();

        // Second pass, draw the text labels.
        tracker = TempoTimeTracker();
        for (const TempoBox& box : myBoxes) {
            int y = static_cast<int>(
                oy + dy * (timeBased ? tracker.advance(box.row)
                                     : static_cast<double>(box.row)));
            if (y < viewTop - 16 || y > viewBtm + 16) continue;

            int side = Segment::meta[box.type]->side;
            int x = baseX[side] + box.x + side * 4 - 2;

            Text::arrange(Text::MC, textStyle, MAX_WIDTH, box.str.c_str());
            Text::draw(recti{x, y - 17, static_cast<int>(box.width), 32});
        }

        // Transparent ghost BPM box
        if (myGhostRow >= 0) {
            const TempoBox& box = myBoxes[0];
            const SegmentMeta* meta = Segment::meta[Segment::BPM];
            int side = meta->side;
            int x = baseX[side];
            int textX = baseX[side] + box.x + side * 4 - 2;
            int y = myGhostY;

            auto segments = gTempo->getSegments();
            BpmChange bpm = segments->getRecent<BpmChange>(myGhostRow);
            std::string label = meta->getDescription(&bpm);

            int flags = side * TileBar::FLIP_H;
            recti r = {x, y - (box.height / 2), box.width, box.height};

            uint32_t ghostColor = Color32a(meta->color, 167);
            Renderer::resetColor();
            Renderer::bindTexture(myBoxHl.texture.handle());
            Renderer::bindShader(Renderer::SH_TEXTURE);
            auto batch = Renderer::batchTC();
            myBoxBar.draw(&batch, r, ghostColor, flags);
            batch.flush();

            Renderer::setColor(RGBAtoColor32(255, 255, 255, 167));
            Text::arrange(Text::MC, textStyle, MAX_WIDTH, label.c_str());
            Text::draw(recti{textX, y - 17, static_cast<int>(box.width), 32});
            Renderer::resetColor();
        }

        // Display detailed info of the mouse over box.
        if (myMouseOverBox >= 0 && myShowHelp &&
            !gSystem->isMouseDown(Mouse::LMB)) {
            drawBoxHelp(myBoxes[myMouseOverBox]);
        }
    }

    void drawBoxHelp(const TempoBox& box) {
        auto meta = Segment::meta[box.type];

        auto coords = gView->getNotefieldCoords();
        int x = (meta->side ? coords.xr : coords.xl) + box.x + box.width / 2;
        int y = gView->rowToY(box.row) + (box.height / 2) + 8;

        TextStyle style;

        style.fontSize = 12;
        Text::arrange(Text::TC, style, meta->singular);
        vec2i nameSize = Text::getSize();

        style.fontSize = 10;
        style.textColor = RGBAtoColor32(192, 192, 192, 255);
        Text::arrange(Text::TC, style, meta->help);
        vec2i helpSize = Text::getSize();

        int w = max(nameSize.x, helpSize.x) + 12;
        int h = nameSize.y + helpSize.y + 8;
        recti r = recti{x - w / 2, y, w, h};

        Draw::roundedBox(r, RGBAtoColor32(128, 128, 128, 255));
        r = Shrink(r, 1);
        Draw::roundedBox(r, RGBAtoColor32(26, 26, 26, 255));

        Text::draw(vec2i{x, y + nameSize.y + 4});

        style.fontSize = 12;
        style.textColor = Colors::white;
        Text::arrange(Text::MC, style, meta->singular);
        Text::draw(vec2i{x, y + 10});
    }

    const Vector<TempoBox>& getBoxes() override { return myBoxes; }

};  // TempoBoxesImpl

// ================================================================================================
// TempoBoxes API.

TempoBoxes* gTempoBoxes = nullptr;

void TempoBoxes::create(XmrNode& settings) {
    gTempoBoxes = new TempoBoxesImpl;
    static_cast<TempoBoxesImpl*>(gTempoBoxes)->loadSettings(settings);
}

void TempoBoxes::destroy() {
    delete static_cast<TempoBoxesImpl*>(gTempoBoxes);
    gTempoBoxes = nullptr;
}

};  // namespace Vortex