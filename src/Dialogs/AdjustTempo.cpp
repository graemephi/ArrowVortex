#include <Dialogs/AdjustTempo.h>

#include <Core/Widgets.h>
#include <Core/WidgetsLayout.h>

#include <Managers/TempoMan.h>
#include <Managers/SimfileMan.h>
#include <Simfile/SegmentGroup.h>
#include <System/Debug.h>
#include <System/System.h>

#include <Editor/Selection.h>
#include <Editor/View.h>
#include <Editor/History.h>
#include <Editor/Common.h>
#include <Editor/Editing.h>
#include <Editor/FindTempo.h>
#include <Editor/AutoSync.h>
#include <Editor/Music.h>
#include <Editor/Waveform.h>
#include <Editor/TempoBoxes.h>
#include <Editor/FindOnsets.h>

#include <algorithm>

namespace Vortex {

static SelectionRegion GetSyncRegion(SelectionRegion region,
                                     bool stopAtNextBpm = true) {
    // use tempo box and note selection if we don't have a region
    if (region.beginRow == region.endRow) {
        if (!gTempoBoxes->noneSelected()) {
            const Vector<TempoBox>& boxes = gTempoBoxes->getBoxes();
            int minRow = INT32_MAX;
            int maxRow = INT32_MIN;
            bool prevSelected = false;
            for (const TempoBox& box : boxes) {
                if (box.isSelected) {
                    minRow = std::min(minRow, box.row);
                }
                if (prevSelected) {
                    maxRow = std::max(maxRow, box.row);
                }
                prevSelected = box.isSelected;
            }
            if (prevSelected) {
                maxRow = std::max(maxRow, boxes.back().row);
            }
            if (minRow <= maxRow) {
                region.beginRow = minRow;
                region.endRow = maxRow;
            }
        } else if (!gNotes->noneSelected()) {
            int minRow = INT32_MAX;
            int maxRow = INT32_MIN;
            for (auto& note : *gNotes) {
                if (note.isSelected) {
                    minRow = std::min(minRow, note.row);
                    maxRow = std::max(maxRow, note.row);
                }
            }
            if (minRow <= maxRow) {
                region.beginRow = minRow;
                region.endRow = maxRow;
            }
        }
    }

    // Extend single-row regions forward to next BPM or end of audio
    bool singleRowRegion =
        (region.beginRow > 0) && (region.beginRow == region.endRow);
    if (singleRowRegion) {
        int nextBpmRow = -1;
        if (stopAtNextBpm) {
            auto segments = gTempo->getSegments();
            if (segments) {
                for (const BpmChange* it = segments->begin<BpmChange>();
                     it != segments->end<BpmChange>(); ++it) {
                    if (it->row > region.beginRow) {
                        nextBpmRow = it->row;
                        break;
                    }
                }
            }
        }

        if (nextBpmRow >= 0) {
            region.endRow = nextBpmRow;
        } else {
            auto& music = gMusic->getSamples();
            if (music.isCompleted()) {
                double endOfAudioTime =
                    (double)music.getNumFrames() / music.getFrequency();
                int endOfAudioRow = gTempo->timeToRow(endOfAudioTime);
                int rowsPerMeasure = ROWS_PER_BEAT * 4;
                region.endRow =
                    ((endOfAudioRow + rowsPerMeasure - 1) / rowsPerMeasure) *
                    rowsPerMeasure;
            }
        }
    }

    return region;
}

static SelectionRegion ScaleBpms(double scale, SelectionRegion region,
                                 bool regionSelected) {
    auto segments = gTempo->getSegments();
    if (!segments) return {};

    int cursorRow = gView->getCursorRow();
    double cursorTime = gTempo->rowToTime(cursorRow);

    int newRegionEnd =
        (int)(region.beginRow + (region.endRow - region.beginRow) * scale);
    int rowDelta = newRegionEnd - region.endRow;
    if (!regionSelected) region = {0, INT32_MAX};

    // Scale BPM changes
    SegmentEdit bpmEdit;
    for (const BpmChange* it = segments->begin<BpmChange>();
         it != segments->end<BpmChange>(); ++it) {
        auto segment = *it;
        if (!regionSelected) {
            bpmEdit.rem.append(segment);
            segment.row *= scale;
            segment.bpm *= scale;
            bpmEdit.add.append(segment);
        } else if (it->row >= region.beginRow && it->row < region.endRow) {
            bpmEdit.rem.append(segment);
            segment.row =
                (int)(region.beginRow + (it->row - region.beginRow) * scale);
            segment.bpm *= scale;
            bpmEdit.add.append(segment);
        } else if (it->row >= region.endRow) {
            bpmEdit.rem.append(segment);
            segment.row += rowDelta;
            bpmEdit.add.append(segment);
        }
    }

    // Scale everything else
    auto scaleSegmentRows = [&](const auto begin, const auto end) {
        for (const auto* it = begin; it != end; ++it) {
            auto segment = *it;
            if (!regionSelected) {
                bpmEdit.rem.append(segment);
                segment.row *= scale;
                bpmEdit.add.append(segment);
            } else if (it->row >= region.beginRow && it->row < region.endRow) {
                bpmEdit.rem.append(segment);
                segment.row *= (int)(region.beginRow +
                                     (it->row - region.beginRow) * scale);
                bpmEdit.add.append(segment);
            } else if (it->row >= region.endRow) {
                bpmEdit.rem.append(segment);
                segment.row += rowDelta;
                bpmEdit.add.append(segment);
            }
        }
    };
    scaleSegmentRows(segments->begin<Stop>(), segments->end<Stop>());
    scaleSegmentRows(segments->begin<Delay>(), segments->end<Delay>());
    scaleSegmentRows(segments->begin<Warp>(), segments->end<Warp>());
    scaleSegmentRows(segments->begin<TimeSignature>(),
                     segments->end<TimeSignature>());
    scaleSegmentRows(segments->begin<TickCount>(), segments->end<TickCount>());
    scaleSegmentRows(segments->begin<Combo>(), segments->end<Combo>());
    scaleSegmentRows(segments->begin<Speed>(), segments->end<Speed>());
    scaleSegmentRows(segments->begin<Scroll>(), segments->end<Scroll>());
    scaleSegmentRows(segments->begin<Fake>(), segments->end<Fake>());
    scaleSegmentRows(segments->begin<Label>(), segments->end<Label>());

    // If we have a region, ensure there's a BPM at the region start and end
    if (regionSelected) {
        double bpmAtStart = segments->getRecent<BpmChange>(region.beginRow).bpm;
        bool hasBpmAtStart = false;
        for (const BpmChange* it = bpmEdit.add.begin<BpmChange>();
             it != bpmEdit.add.end<BpmChange>(); ++it) {
            if (it->row == region.beginRow) {
                hasBpmAtStart = true;
                break;
            }
        }
        if (!hasBpmAtStart) {
            bpmEdit.add.insert(BpmChange(region.beginRow, bpmAtStart * scale));
        }

        double bpmAtOldEnd = segments->getRecent<BpmChange>(region.endRow).bpm;
        bool hasBpmAtEnd = false;
        for (const BpmChange* it = bpmEdit.add.begin<BpmChange>();
             it != bpmEdit.add.end<BpmChange>(); ++it) {
            if (it->row == newRegionEnd) {
                hasBpmAtEnd = true;
                break;
            }
        }
        if (!hasBpmAtEnd) {
            bpmEdit.add.insert(BpmChange(newRegionEnd, bpmAtOldEnd));
        }
    }

    // Scale notes
    static const NoteList emptyNotes;
    const NoteList& notes =
        gChart->isOpen() ? gChart->get()->notes : emptyNotes;
    NoteEdit noteEdit;
    for (const Note& note : notes) {
        if (!regionSelected) {
            noteEdit.rem.append(note);
            Note scaled = note;
            scaled.row = (int)(note.row * scale);
            scaled.endrow = (int)(note.endrow * scale);
            noteEdit.add.append(scaled);
        } else if (note.row >= region.beginRow && note.row <= region.endRow) {
            noteEdit.rem.append(note);
            Note scaled = note;
            scaled.row =
                (int)(region.beginRow + (note.row - region.beginRow) * scale);
            scaled.endrow = (int)(region.beginRow +
                                  (note.endrow - region.beginRow) * scale);
            noteEdit.add.append(scaled);
        } else if (note.row > region.endRow) {
            noteEdit.rem.append(note);
            Note shifted = note;
            shifted.row = note.row + rowDelta;
            shifted.endrow = note.endrow + rowDelta;
            noteEdit.add.append(shifted);
        }
    }

    // Abort if notes would collide after scaling
    if (noteEdit.add.size() > 1) {
        std::stable_sort(noteEdit.add.begin(), noteEdit.add.end(),
                         [](const Note& a, const Note& b) {
                             if (a.row != b.row) return a.row < b.row;
                             return a.col < b.col;
                         });

        for (const Note* it = noteEdit.add.begin() + 1;
             it != noteEdit.add.end(); ++it) {
            if (it->row == (it - 1)->row && it->col == (it - 1)->col) return {};
        }
    }

    gTempo->modify(bpmEdit);

    if (!noteEdit.add.empty()) {
        gNotes->modify(noteEdit, true);
    }

    // Update region selection to match the new scaled bounds
    auto currentlySelectedRegion = gSelection->getSelectedRegion();
    if (currentlySelectedRegion.beginRow != currentlySelectedRegion.endRow) {
        gSelection->selectRegion(region.beginRow, newRegionEnd);
    }

    // Restore cursor to same time position
    if (gMusic->isPaused()) {
        int newCursorRow = cursorRow;
        if (cursorRow >= region.beginRow && cursorRow < region.endRow)
            newCursorRow =
                (int)(region.beginRow + (cursorRow - region.beginRow) * scale);
        else if (cursorRow >= region.endRow)
            newCursorRow = cursorRow + rowDelta;
        gView->setCursorRow(newCursorRow);
    } else {
        int newCursorRow = gTempo->timeToRow(cursorTime);
        gView->setCursorRow(newCursorRow);
    }

    return {region.beginRow, newRegionEnd};
}

enum Actions {
    ACT_BPM_SET,
    ACT_BPM_HALVE,
    ACT_BPM_DOUBLE,
    ACT_BPM_TWEAK,

    ACT_STOP_SET,
    ACT_STOP_CONVERT_REGION_TO_STOP,
    ACT_STOP_CONVERT_REGION_TO_STUTTER,
    ACT_STOP_TWEAK,

    ACT_INSERT_BEATS,
    ACT_REMOVE_BEATS,

    ACT_AUTO_SYNC,

    ACT_SCALE_ALL_HALVE,
    ACT_SCALE_ALL_DOUBLE,
};

DialogAdjustTempo::~DialogAdjustTempo() {}

DialogAdjustTempo::DialogAdjustTempo() {
    setTitle("ADJUST TEMPO");
    myCreateWidgets();
    onChanges(VCM_ALL_CHANGES);
    myBeatsToInsert = 1;
    myInsertTarget = 0;
    mySyncMode = 1;
    myPreserveInputBpms = true;
    mySyncToFilteredWaveform = true;
    pendingSelectBeginRow_ = 0;
    pendingSelectEndRow_ = 0;
}

WgSpinner* DialogAdjustTempo::myCreateWidgetRow(const std::string& label, int y,
                                                double& val, int action) {
    bool isBPM = (action == ACT_BPM_SET);

    const char* tooltips[] = {
        "Halve the current BPM",
        "Double the current BPM",
        "Convert the selected region to a stop",
        "Convert the selected region to a stutter gimmick",
    };
    const char* tooltips2[] = {
        "Stop length at the current beat, in seconds",
        "Music tempo at the current beat, in beats per minute"};

    WgSpinner* spinner = myLayout.add<WgSpinner>(label);
    spinner->value.bind(&val);
    spinner->setPrecision(3, 6);
    spinner->onChange.bind(this, &DialogAdjustTempo::onAction, action + 0);
    spinner->setTooltip(tooltips2[isBPM]);

    WgButton* op1 = myLayout.add<WgButton>();
    op1->text.set(isBPM ? "{g:halve}" : "{g:full selection}");
    op1->onPress.bind(this, &DialogAdjustTempo::onAction, action + 1);
    op1->setTooltip(isBPM ? tooltips[0] : tooltips[2]);

    WgButton* op2 = myLayout.add<WgButton>();
    op2->text.set(isBPM ? "{g:double}" : "{g:half selection}");
    op2->onPress.bind(this, &DialogAdjustTempo::onAction, action + 2);
    op2->setTooltip(isBPM ? tooltips[1] : tooltips[3]);

    WgButton* tweak = myLayout.add<WgButton>();
    tweak->text.set("{g:tweak}");
    tweak->onPress.bind(this, &DialogAdjustTempo::onAction, action + 3);
    tweak->setTooltip(isBPM ? "Tweak the current BPM"
                            : "Tweak the current stop");

    return spinner;
}

void DialogAdjustTempo::myCreateWidgets() {
    myLayout.row().col(38).col(116).col(24).col(24).col(24);

    WgSpinner* bpm = myCreateWidgetRow("BPM", 0, myBPM, ACT_BPM_SET);
    bpm->setRange(VC_MIN_BPM, VC_MAX_BPM);
    bpm->setPrecision(3, 6);
    bpm->setStep(1.0);

    WgSpinner* stop = myCreateWidgetRow("Stop", 28, myStop, ACT_STOP_SET);
    stop->setRange(VC_MIN_STOP, VC_MAX_STOP);
    stop->setPrecision(3, 6);
    stop->setStep(0.001);

    myLayout.row().col(242);
    myLayout.add<WgSeperator>();

    myLayout.row().col(119).col(119);

    WgSpinner* spinner = myLayout.add<WgSpinner>("Offset in beats");
    spinner->setRange(0.0, 100000.0);
    spinner->value.bind(&myBeatsToInsert);
    spinner->setPrecision(3, 6);
    spinner->setTooltip("Number of beats to insert or remove");

    WgCycleButton* cycle = myLayout.add<WgCycleButton>("Apply offset to");
    cycle->addItem("This chart");
    cycle->addItem("All charts");
    cycle->value.bind(&myInsertTarget);
    cycle->setTooltip(
        "Determines which notes and/or tempo changes are targeted");

    WgButton* insert = myLayout.add<WgButton>();
    insert->text.set("Insert beats");
    insert->onPress.bind(this, &DialogAdjustTempo::onAction,
                         static_cast<int>(ACT_INSERT_BEATS));
    insert->setTooltip(
        "Insert the above number of beats at the cursor position\n"
        "All notes and tempo changes after the cursor will be shifted down");

    WgButton* remove = myLayout.add<WgButton>();
    remove->text.set("Delete beats");
    remove->onPress.bind(this, &DialogAdjustTempo::onAction,
                         static_cast<int>(ACT_REMOVE_BEATS));
    remove->setTooltip(
        "Delete the above number of beats at the cursor position\n"
        "All notes and tempo changes after the cursor will be shifted up\n"
        "Notes and tempo changes in the deleted region will be removed");

    myLayout.row().col(242);
    myLayout.add<WgSeperator>();

    myLayout.row().col(242);
    WgButton* autoSync = myLayout.add<WgButton>();
    autoSync->text.set("Auto Sync");
    autoSync->onPress.bind(this, &DialogAdjustTempo::onAction,
                           static_cast<int>(ACT_AUTO_SYNC));
    autoSync->setTooltip(
        "Automatically synchronize BPM changes to detected onsets in the "
        "audio");

    myLayout.row().col(178).col(28).col(28);
    WgCycleButton* syncMode = myLayout.add<WgCycleButton>();
    syncMode->addItem("There is one BPM");
    syncMode->addItem("Hey, not too rough");
    syncMode->addItem("Hurt me plenty");
    syncMode->addItem("Ultra-violence");
    syncMode->value.bind(&mySyncMode);
    syncMode->setTooltip("The toughness of the enemies you'll face");

    WgButton* scaleHalve = myLayout.add<WgButton>();
    scaleHalve->text.set("{g:halve}*");
    scaleHalve->onPress.bind(this, &DialogAdjustTempo::onAction,
                             static_cast<int>(ACT_SCALE_ALL_HALVE));
    scaleHalve->setTooltip(
        "Halve all BPMs\n"
        "Shift for 2/3");

    WgButton* scaleDouble = myLayout.add<WgButton>();
    scaleDouble->text.set("{g:double}*");
    scaleDouble->onPress.bind(this, &DialogAdjustTempo::onAction,
                              static_cast<int>(ACT_SCALE_ALL_DOUBLE));
    scaleDouble->setTooltip(
        "Double all BPMs\n"
        "Shift for 3/2");

    myLayout.row().col(238);
    WgCheckbox* preserveInputBpms = myLayout.add<WgCheckbox>();
    preserveInputBpms->text.set("Preserve BPMs");
    preserveInputBpms->value.bind(&myPreserveInputBpms);
    preserveInputBpms->setTooltip("Keep existing BPM changes");

    myLayout.row().col(238);
    WgCheckbox* syncToFiltered = myLayout.add<WgCheckbox>();
    syncToFiltered->text.set("Sync to active waveform filter");
    syncToFiltered->value.bind(&mySyncToFilteredWaveform);
    syncToFiltered->setTooltip("Use the filtered waveform for onset detection");
}

void DialogAdjustTempo::onChanges(int changes) {
    if (changes & VCM_FILE_CHANGED) {
        myBPM = 0.0;
        myStop = 0.0;
        myOffset = 0.0;
        if (gSimfile->isOpen()) {
            for (auto w : myLayout) w->setEnabled(true);
        } else {
            for (auto w : myLayout) w->setEnabled(false);
        }
    }
}

void DialogAdjustTempo::onTick() {
    if (gSimfile->isOpen()) {
        int row = gView->getCursorRow();
        auto segments = gTempo->getSegments();
        myBPM = segments->getRecent<BpmChange>(row).bpm;
        myStop = segments->getRow<Stop>(row).seconds;
    }
    if (pendingSelectBeginRow_ != pendingSelectEndRow_) {
        gTempoBoxes->selectRows(SELECT_ADD, pendingSelectBeginRow_,
                                pendingSelectEndRow_, INT32_MIN, INT32_MAX);
        pendingSelectBeginRow_ = 0;
        pendingSelectEndRow_ = 0;
    }
    EditorDialog::onTick();
}

void DialogAdjustTempo::onAction(int id) {
    if (gSimfile->isClosed()) return;
    int row = gView->getCursorRow();
    switch (id) {
        case ACT_BPM_SET: {
            if (myBPM != 0.0) {
                gTempo->addSegment(BpmChange(row, myBPM));
            }
        } break;
        case ACT_BPM_HALVE:
        case ACT_BPM_DOUBLE: {
            double bpm =
                gTempo->getBpm(row) * ((id == ACT_BPM_DOUBLE) ? 2 : 0.5);
            gTempo->addSegment(BpmChange(row, bpm));
        } break;
        case ACT_BPM_TWEAK:
            gTempo->startTweakingBpm(row);
            break;
        case ACT_STOP_SET: {
            gTempo->addSegment(Stop(row, myStop));
        } break;
        case ACT_STOP_CONVERT_REGION_TO_STUTTER:
        case ACT_STOP_CONVERT_REGION_TO_STOP: {
            auto region = gSelection->getSelectedRegion();
            double t1 = gTempo->rowToTime(region.beginRow);
            double t2 = gTempo->rowToTime(region.endRow);
            if (t2 > t1) {
                if (id == ACT_STOP_CONVERT_REGION_TO_STOP) {
                    gTempo->addSegment(Stop(region.beginRow, t2 - t1));
                } else if (region.endRow - region.beginRow >= 2) {
                    double halfTime = (t2 - t1) * 0.5;
                    int rows = region.endRow - region.beginRow;

                    SegmentEdit edit;
                    edit.add.append(Stop(region.beginRow, halfTime));

                    double stutterBpm = BeatsPerMin(halfTime / rows);
                    edit.add.append(BpmChange(region.beginRow, stutterBpm));

                    double endBpm = gTempo->getBpm(region.endRow);
                    edit.add.append(BpmChange(region.endRow, endBpm));

                    gTempo->modify(edit);
                }
            }
        } break;
        case ACT_STOP_TWEAK:
            gTempo->startTweakingStop(row);
            break;
        case ACT_INSERT_BEATS:
        case ACT_REMOVE_BEATS: {
            int numRows =
                static_cast<int>(ROWS_PER_BEAT * myBeatsToInsert + 0.5);
            if (id == ACT_REMOVE_BEATS) numRows *= -1;
            gEditing->insertRows(gView->getCursorRow(), numRows,
                                 (myInsertTarget == 0));
        } break;
        case ACT_AUTO_SYNC: {
            SyncMode mode = static_cast<SyncMode>(mySyncMode);

            auto segments = gTempo->getSegments();
            double offset = gTempo->getOffset();
            int cursorRow = gView->getCursorRow();
            double cursorTime = gTempo->rowToTime(cursorRow);
            auto& music = gMusic->getSamples();
            if (!segments) return;
            if (!gWaveform) return;
            if (!music.isCompleted()) {
                HudInfo(
                    "The music is still loading, wait a bit longer before "
                    "using auto sync");
                return;
            }

            Vector<BpmChange> bpmChanges;
            for (const BpmChange* it = segments->begin<BpmChange>();
                 it != segments->end<BpmChange>(); ++it) {
                bpmChanges.push_back(*it);
            }

            auto selectedRegion = gSelection->getSelectedRegion();
            auto regionUnrounded =
                GetSyncRegion(selectedRegion, myPreserveInputBpms);
            bool singleRowRegion =
                (selectedRegion.beginRow > 0) &&
                (selectedRegion.beginRow == selectedRegion.endRow);
            bool validRegion =
                (regionUnrounded.beginRow < regionUnrounded.endRow) ||
                singleRowRegion;
            auto region = regionUnrounded;
            if (!singleRowRegion) {
                region.beginRow =
                    ((region.beginRow + ROWS_PER_BEAT / 2) / ROWS_PER_BEAT) *
                    ROWS_PER_BEAT;
                region.endRow =
                    ((region.endRow + ROWS_PER_BEAT / 2) / ROWS_PER_BEAT) *
                    ROWS_PER_BEAT;
            }

            double regionUnroundedEndTime =
                gTempo->rowToTime(regionUnrounded.endRow);

            Vector<Onset> onsets =
                gWaveform->getOnsets(mySyncToFilteredWaveform);

            // Regions and preserving BPMs are pretty much the same thing
            // implementation-wise
            PreserveOptions preserveOptions = {};
            if (validRegion) preserveOptions.flags |= PreserveOptions::Region;
            if (myPreserveInputBpms)
                preserveOptions.flags |= PreserveOptions::Preserve;
            if (validRegion) {
                double bpmAtStart =
                    segments->getRecent<BpmChange>(region.beginRow).bpm;
                double bpmAtEnd =
                    segments->getRecent<BpmChange>(region.endRow).bpm;

                preserveOptions.regionStartRow = region.beginRow;
                preserveOptions.regionEndRow = region.endRow;
                preserveOptions.regionStartBpm = bpmAtStart;
                preserveOptions.regionEndBpm = bpmAtEnd;
            }

            AutoSyncResult syncResult =
                AutoSync(onsets, music.getNumFrames(), music.getFrequency(),
                         bpmChanges, offset, preserveOptions, mode);

            gHistory->startChain();
            SegmentEdit edit;

            for (const BpmChange* it = segments->begin<BpmChange>();
                 it != segments->end<BpmChange>(); ++it) {
                edit.rem.append(*it);
            }
            for (const auto& bpm : syncResult.bpmChanges) {
                edit.add.append(bpm);
            }

            int rowDelta = syncResult.regionEndRowDelta;
            auto updateSegments = [&](const auto begin, const auto end) {
                if (rowDelta > 0) {
                    for (const auto* it = begin; it != end; ++it) {
                        auto segment = *it;
                        edit.rem.append(segment);
                        if (segment.row > region.endRow)
                            segment.row += rowDelta;
                        edit.add.append(segment);
                    }
                }
            };

            updateSegments(segments->begin<Stop>(), segments->end<Stop>());
            updateSegments(segments->begin<Delay>(), segments->end<Delay>());
            updateSegments(segments->begin<Warp>(), segments->end<Warp>());
            updateSegments(segments->begin<TimeSignature>(),
                           segments->end<TimeSignature>());
            updateSegments(segments->begin<TickCount>(),
                           segments->end<TickCount>());
            updateSegments(segments->begin<Combo>(), segments->end<Combo>());
            updateSegments(segments->begin<Speed>(), segments->end<Speed>());
            updateSegments(segments->begin<Scroll>(), segments->end<Scroll>());
            updateSegments(segments->begin<Fake>(), segments->end<Fake>());
            updateSegments(segments->begin<Label>(), segments->end<Label>());

            gTempo->setOffset(syncResult.offset);
            gTempo->modify(edit);

            gHistory->finishChain(validRegion ? "Auto Sync Region"
                                              : "Auto Sync");

            // Restore cursor to same time position after sync
            int newCursorRow = gTempo->timeToRow(cursorTime);
            gView->setCursorRow(newCursorRow);

            // Clear any region and select the new BPMs in it
            if (validRegion) {
                int newRegionEnd = gTempo->timeToRow(regionUnroundedEndTime);
                if (singleRowRegion) {
                    gSelection->selectRegion();
                } else {
                    gSelection->selectRegion(0, 0);
                }

                pendingSelectBeginRow_ = region.beginRow;
                pendingSelectEndRow_ = newRegionEnd + 1;
            }
        } break;
        case ACT_SCALE_ALL_HALVE:
        case ACT_SCALE_ALL_DOUBLE: {
            bool shiftHeld = gSystem->isKeyDown(Key::SHIFT_L) ||
                             gSystem->isKeyDown(Key::SHIFT_R);
            double scale = 1.0;
            const char* actionName = "";

            if (id == ACT_SCALE_ALL_DOUBLE) {
                if (shiftHeld) {
                    scale = 1.5;
                    actionName = "Scale all BPMs x1.5";
                } else {
                    scale = 2.0;
                    actionName = "Double all BPMs";
                }
            } else {
                if (shiftHeld) {
                    scale = 2.0 / 3.0;
                    actionName = "Scale all BPMs x0.67";
                } else {
                    scale = 0.5;
                    actionName = "Halve all BPMs";
                }
            }

            auto region = GetSyncRegion(gSelection->getSelectedRegion());
            bool regionSelected = (region.beginRow < region.endRow);
            if (regionSelected) {
                region.beginRow =
                    (region.beginRow / ROWS_PER_BEAT) * ROWS_PER_BEAT;
                region.endRow =
                    ((region.endRow + ROWS_PER_BEAT / 2) / ROWS_PER_BEAT) *
                    ROWS_PER_BEAT;
                if (region.beginRow >= region.endRow) regionSelected = false;
            }

            gHistory->startChain();
            auto scaled = ScaleBpms(scale, region, regionSelected);
            gHistory->finishChain(actionName);
            gNotes->deselectAll();

            if (regionSelected) {
                gSelection->selectRegion(0, 0);
                pendingSelectBeginRow_ = scaled.beginRow;
                pendingSelectEndRow_ = scaled.endRow - 1;
            }
        } break;
    };
}

};  // namespace Vortex
