#include "engine/controls/loopingcontrol.h"

#include <QtDebug>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "engine/controls/bpmcontrol.h"
#include "engine/controls/enginecontrol.h"
#include "engine/controls/ratecontrol.h"
#include "engine/enginebuffer.h"
#include "moc_loopingcontrol.cpp"
#include "preferences/usersettings.h"
#include "track/track.h"
#include "util/compatibility/qatomic.h"
#include "util/make_const_iterator.h"
#include "util/math.h"

namespace {

constexpr std::array kBeatSizes = {0.03125,
        0.0625,
        0.125,
        0.25,
        0.5,
        1.0,
        2.0,
        4.0,
        8.0,
        16.0,
        32.0,
        64.0,
        128.0,
        256.0,
        512.0};

constexpr mixxx::audio::FrameDiff_t kMinimumAudibleLoopSizeFrames = 150;

// Snap position to the nearest beat, with directional preference depending on
// whether the user is actively adjusting a loop point.
//  - LoopIn:  prefer prevBeat (loop start should be at or before current pos)
//  - LoopOut: prefer nextBeat (loop end should be at or after current pos)
//  - None:    snap to closest beat
// Returns kInvalidFramePos if beats can't be found.
mixxx::audio::FramePos quantizeToNearestBeat(
        const mixxx::BeatsPointer& pBeats,
        mixxx::audio::FramePos position,
        mixxx::audio::FramePos trackEndPosition,
        LoopAdjustTarget adjustTarget) {
    mixxx::audio::FramePos prevBeat;
    mixxx::audio::FramePos nextBeat;
    if (!pBeats->findPrevNextBeats(position, &prevBeat, &nextBeat, false)) {
        return mixxx::audio::kInvalidFramePos;
    }

    const auto distanceToPrev = position - prevBeat;
    const auto distanceToNext = nextBeat - position;
    const auto closestBeat = distanceToNext > distanceToPrev ? prevBeat : nextBeat;

    if (closestBeat == position) {
        return closestBeat;
    }

    switch (adjustTarget) {
    case LoopAdjustTarget::LoopIn:
        return prevBeat;
    case LoopAdjustTarget::LoopOut:
        return (nextBeat > trackEndPosition) ? prevBeat : nextBeat;
    case LoopAdjustTarget::None:
        return (closestBeat > trackEndPosition) ? prevBeat : closestBeat;
    }
    // unreachable
    return mixxx::audio::kInvalidFramePos;
}

// returns true if a is valid and is fairly close to target (within +/- 1 frame).
bool positionNear(mixxx::audio::FramePos a, mixxx::audio::FramePos target) {
    return a.isValid() && a > target - 1 && a < target + 1;
}

bool nearlySameLoop(Loop const& first, Loop const& second) {
    return positionNear(first.startPosition, second.startPosition) &&
            positionNear(first.endPosition, second.endPosition);
}

mixxx::audio::FramePos findQuantizedBeatloopStart(
        const mixxx::BeatsPointer& pBeats,
        mixxx::audio::FramePos currentPosition,
        double beats) {
    // The closest beat might be ahead of play position and will cause a catching loop.
    mixxx::audio::FramePos prevBeatPosition;
    mixxx::audio::FramePos nextBeatPosition;
    if (!pBeats->findPrevNextBeats(currentPosition, &prevBeatPosition, &nextBeatPosition, false)) {
        return currentPosition;
    }

    const mixxx::audio::FrameDiff_t beatLength = nextBeatPosition - prevBeatPosition;
    double loopLength = beatLength * beats;
    if (beats >= 1.0) {
        const mixxx::audio::FramePos closestBeatPosition =
                (nextBeatPosition - currentPosition >
                        currentPosition - prevBeatPosition)
                ? prevBeatPosition
                : nextBeatPosition;
        return closestBeatPosition;
    }

    // In case of beat length less then 1 beat:
    // (| - beats, ^ - current track's position):
    //
    // ...|...................^........|...
    //
    // If we press 1/2 beatloop we want loop from 50% to 100%,
    // If I press 1/4 beatloop, we want loop from 50% to 75% etc
    const mixxx::audio::FrameDiff_t framesSinceLastBeat =
            currentPosition - prevBeatPosition;
    // find the previous beat fraction and check if the current position is
    // closer to this or the next one place the new loop start to the closer one
    const mixxx::audio::FramePos previousFractionBeatPosition =
            prevBeatPosition + floor(framesSinceLastBeat / loopLength) * loopLength;
    double framesSinceLastFractionBeatPosition = currentPosition - previousFractionBeatPosition;
    if (framesSinceLastFractionBeatPosition <= (loopLength / 2.0)) {
        return previousFractionBeatPosition;
    }
    return previousFractionBeatPosition + loopLength;
}

// When a loop changes size such that the playposition is outside of the loop,
// figure out the best place in the new loop to seek to maintain the beat.
// Keeps multi-bar phrasing correct with 4/4 tracks.
mixxx::audio::FramePos adjustedPositionInsideAdjustedLoop(
        mixxx::audio::FramePos currentPosition,
        bool reverse,
        Loop const& oldLoop,
        Loop const& newLoop) {
    if (reverse) {
        if (newLoop.containsReverse(currentPosition)) {
            // playposition already is inside the loop
            return mixxx::audio::kInvalidFramePos;
        }
        if (oldLoop.endPosition.isValid() &&
                currentPosition > oldLoop.endPosition &&
                currentPosition > newLoop.startPosition) {
            // Playposition was after a catching loop and is still
            // a catching loop. nothing to do
            return mixxx::audio::kInvalidFramePos;
        }
        if (currentPosition == newLoop.startPosition) {
            // wrap around since the "end" is considered outside the loop
            return newLoop.endPosition;
        }
    } else {
        if (newLoop.containsForward(currentPosition)) {
            return mixxx::audio::kInvalidFramePos;
        }
        if (oldLoop.startPosition.isValid() &&
                currentPosition < oldLoop.startPosition &&
                currentPosition < newLoop.endPosition) {
            return mixxx::audio::kInvalidFramePos;
        }
        if (currentPosition == newLoop.endPosition) {
            return newLoop.startPosition;
        }
    }

    const mixxx::audio::FrameDiff_t newLoopSize = newLoop.length();
    DEBUG_ASSERT(newLoopSize > 0);
    mixxx::audio::FramePos adjustedPosition = currentPosition;
    if (adjustedPosition > newLoop.endPosition) {
        // In case play head has already passed the new out position, seek in whole
        // loop size steps back, as if playback has been looped within the boundaries
        double adjustSteps =
                ceil((adjustedPosition.value() - newLoop.endPosition.value()) /
                        newLoopSize);
        adjustedPosition -= adjustSteps * newLoopSize;
        DEBUG_ASSERT(adjustedPosition < newLoop.endPosition);
        VERIFY_OR_DEBUG_ASSERT(adjustedPosition >= newLoop.startPosition) {
            // This can happen when offset calculation above has double precision
            // issues (noticed around 0.00) which shifts the pos beyond loop in
            qWarning()
                    << "SHOULDN'T HAPPEN: adjustedPositionInsideAdjustedLoop "
                       "set new position to before in point --"
                    << " seeking to in point";
            adjustedPosition = newLoop.startPosition;
        }
    } else if (adjustedPosition < newLoop.startPosition) {
        // In case play head has already been looped back to the old loop in position,
        // seek in whole loop size steps forward until we are in the new loop boundaries
        double adjustSteps =
                ceil((newLoop.startPosition.value() - adjustedPosition.value()) /
                        newLoopSize);
        adjustedPosition += adjustSteps * newLoopSize;
        DEBUG_ASSERT(adjustedPosition >= newLoop.startPosition);
        VERIFY_OR_DEBUG_ASSERT(adjustedPosition < newLoop.endPosition) {
            // This can happen when offset calculation above has double precision
            // issues (noticed around 0.00) which shifts the pos beyond loop out
            qWarning()
                    << "SHOULDN'T HAPPEN: adjustedPositionInsideAdjustedLoop "
                       "set new position to out point or later--"
                    << " seeking to in point";
            adjustedPosition = newLoop.startPosition;
        }
    }
    if (adjustedPosition != currentPosition) {
        return adjustedPosition;
    } else {
        return mixxx::audio::kInvalidFramePos;
    }
}

// Used to generate the beatloop_%SIZE, beatjump_%SIZE, and loop_move_%SIZE CO
// ConfigKeys.
ConfigKey keyForControl(const QString& group, const QString& ctrlName, double num) {
    ConfigKey key;
    key.group = group;
    key.item = ctrlName.arg(num);
    return key;
}

} // namespace

// static
QList<double> LoopingControl::getBeatSizes() {
    return QList<double>(kBeatSizes.begin(), kBeatSizes.end());
}

LoopingControl::LoopingControl(const QString& group,
        UserSettingsPointer pConfig)
        : EngineControl(group, pConfig),
          m_bLoopingEnabled(false),
          m_bLoopRollActive(false),
          m_bLoopWasEnabledBeforeSlipEnable(false),
          m_bLoopOutPressedWhileLoopDisabled(false),
          m_prevLoopSize(-1),
          m_trueTrackBeats(false) {
    m_currentPosition.setValue(mixxx::audio::kStartFramePos);
    m_pActiveBeatLoop = nullptr;
    m_pRateControl = nullptr;
    //Create loop-in, loop-out, loop-exit, and reloop/exit ControlObjects
    m_pLoopInButton = std::make_unique<ControlPushButton>(ConfigKey(group, "loop_in"));
    connect(m_pLoopInButton.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotLoopIn,
            Qt::DirectConnection);
    m_pLoopInButton->set(0);

    m_pLoopInGotoButton = std::make_unique<ControlPushButton>(ConfigKey(group, "loop_in_goto"));
    connect(m_pLoopInGotoButton.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotLoopInGoto);

    m_pLoopOutButton = std::make_unique<ControlPushButton>(ConfigKey(group, "loop_out"));
    connect(m_pLoopOutButton.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotLoopOut,
            Qt::DirectConnection);
    m_pLoopOutButton->set(0);

    m_pLoopOutGotoButton = std::make_unique<ControlPushButton>(ConfigKey(group, "loop_out_goto"));
    connect(m_pLoopOutGotoButton.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotLoopOutGoto);

    m_pLoopExitButton = std::make_unique<ControlPushButton>(ConfigKey(group, "loop_exit"));
    connect(m_pLoopExitButton.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotLoopExit,
            Qt::DirectConnection);
    m_pLoopExitButton->set(0);

    m_pReloopToggleButton = std::make_unique<ControlPushButton>(ConfigKey(group, "reloop_toggle"));
    connect(m_pReloopToggleButton.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotReloopToggle,
            Qt::DirectConnection);
    m_pReloopToggleButton->set(0);
    // The old reloop_exit name was confusing. This CO does both entering and exiting.
    m_pReloopToggleButton->addAlias(ConfigKey(group, QStringLiteral("reloop_exit")));

    m_pReloopAndStopButton = std::make_unique<ControlPushButton>(
            ConfigKey(group, "reloop_andstop"));
    connect(m_pReloopAndStopButton.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotReloopAndStop,
            Qt::DirectConnection);

    m_pCOLoopEnabled = std::make_unique<ControlObject>(ConfigKey(group, "loop_enabled"));
    m_pCOLoopEnabled->set(0.0);
    m_pCOLoopEnabled->connectValueChangeRequest(this,
            &LoopingControl::slotLoopEnabledValueChangeRequest,
            Qt::DirectConnection);

    m_pCOLoopStartPosition =
            std::make_unique<ControlObject>(ConfigKey(group, "loop_start_position"));
    m_pCOLoopStartPosition->set(kNoTrigger);
    connect(m_pCOLoopStartPosition.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotLoopStartPos,
            Qt::DirectConnection);

    m_pCOLoopEndPosition =
            std::make_unique<ControlObject>(ConfigKey(group, "loop_end_position"));
    m_pCOLoopEndPosition->set(kNoTrigger);
    connect(m_pCOLoopEndPosition.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotLoopEndPos,
            Qt::DirectConnection);

    m_pQuantizeEnabled = ControlObject::getControl(ConfigKey(group, "quantize"));
    m_pSlipEnabled = ControlObject::getControl(ConfigKey(group, "slip_enabled"));

    // DEPRECATED: Use beatloop_size and beatloop_set instead.
    // Activates a beatloop of a specified number of beats.
    m_pCOBeatLoop = std::make_unique<ControlObject>(ConfigKey(group, "beatloop"), false);
    connect(
            m_pCOBeatLoop.get(),
            &ControlObject::valueChanged,
            this,
            [this](double value) { slotBeatLoop(value); },
            Qt::DirectConnection);
    m_pCOLoopAnchor = std::make_unique<ControlPushButton>(ConfigKey(group, "loop_anchor"),
            true,
            static_cast<double>(LoopAnchorPoint::Start));
    m_pCOLoopAnchor->setButtonMode(mixxx::control::ButtonMode::Toggle);

    m_pCOBeatLoopSize = std::make_unique<ControlObject>(ConfigKey(group, "beatloop_size"),
            true,
            false,
            false,
            4.0);
    m_pCOBeatLoopSize->connectValueChangeRequest(this,
            &LoopingControl::slotBeatLoopSizeChangeRequest, Qt::DirectConnection);
    m_pCOBeatLoopActivate = std::make_unique<ControlPushButton>(
            ConfigKey(group, "beatloop_activate"));
    connect(m_pCOBeatLoopActivate.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotBeatLoopToggle);
    m_pCOBeatLoopRollActivate = std::make_unique<ControlPushButton>(
            ConfigKey(group, "beatlooproll_activate"));
    connect(m_pCOBeatLoopRollActivate.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotBeatLoopRollActivate);

    // Here we create corresponding beatloop_(SIZE) CO's which all call the same
    // BeatControl, but with a set value.
    for (auto beatSize : kBeatSizes) {
        auto pBeatLoop = std::make_unique<BeatLoopingControl>(group, beatSize);
        connect(pBeatLoop.get(),
                &BeatLoopingControl::activateBeatLoop,
                this,
                &LoopingControl::slotBeatLoopActivate,
                Qt::DirectConnection);
        connect(pBeatLoop.get(),
                &BeatLoopingControl::activateBeatLoopRoll,
                this,
                &LoopingControl::slotBeatLoopActivateRoll,
                Qt::DirectConnection);
        connect(pBeatLoop.get(),
                &BeatLoopingControl::deactivateBeatLoop,
                this,
                &LoopingControl::slotBeatLoopDeactivate,
                Qt::DirectConnection);
        connect(pBeatLoop.get(),
                &BeatLoopingControl::deactivateBeatLoopRoll,
                this,
                &LoopingControl::slotBeatLoopDeactivateRoll,
                Qt::DirectConnection);
        m_beatLoops.push_back(std::move(pBeatLoop));
    }

    m_pCOBeatJump = std::make_unique<ControlObject>(ConfigKey(group, "beatjump"), false);
    connect(m_pCOBeatJump.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotBeatJump,
            Qt::DirectConnection);
    m_pCOBeatJumpSize = std::make_unique<ControlObject>(ConfigKey(group, "beatjump_size"),
            true,
            false,
            false,
            4.0);
    m_pCOBeatJumpSize->connectValueChangeRequest(this,
            &LoopingControl::slotBeatJumpSizeChangeRequest,
            Qt::DirectConnection);

    m_pCOBeatJumpSizeHalve = std::make_unique<ControlPushButton>(
            ConfigKey(group, "beatjump_size_halve"));
    m_pCOBeatJumpSizeHalve->setKbdRepeatable(true);
    connect(m_pCOBeatJumpSizeHalve.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotBeatJumpSizeHalve);
    m_pCOBeatJumpSizeDouble = std::make_unique<ControlPushButton>(
            ConfigKey(group, "beatjump_size_double"));
    m_pCOBeatJumpSizeDouble->setKbdRepeatable(true);
    connect(m_pCOBeatJumpSizeDouble.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotBeatJumpSizeDouble);

    m_pCOBeatJumpForward = std::make_unique<ControlPushButton>(
            ConfigKey(group, "beatjump_forward"));
    m_pCOBeatJumpForward->setKbdRepeatable(true);
    connect(m_pCOBeatJumpForward.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotBeatJumpForward);
    m_pCOBeatJumpBackward = std::make_unique<ControlPushButton>(
            ConfigKey(group, "beatjump_backward"));
    m_pCOBeatJumpBackward->setKbdRepeatable(true);
    connect(m_pCOBeatJumpBackward.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotBeatJumpBackward);

    // Create beatjump_(SIZE) CO's which all call beatjump, but with a set
    // value.
    for (auto beatSize : kBeatSizes) {
        auto pBeatJump = std::make_unique<BeatJumpControl>(group, beatSize);
        connect(pBeatJump.get(),
                &BeatJumpControl::beatJump,
                this,
                &LoopingControl::slotBeatJump,
                Qt::DirectConnection);
        m_beatJumps.push_back(std::move(pBeatJump));
    }

    m_pCOLoopMove = std::make_unique<ControlObject>(ConfigKey(group, "loop_move"), false);
    connect(m_pCOLoopMove.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotLoopMove,
            Qt::DirectConnection);

    // Create loop_move_(SIZE) CO's which all call loop_move, but with a set
    // value.
    for (auto beatSize : kBeatSizes) {
        auto pLoopMove = std::make_unique<LoopMoveControl>(group, beatSize);
        connect(pLoopMove.get(),
                &LoopMoveControl::loopMove,
                this,
                &LoopingControl::slotLoopMove,
                Qt::DirectConnection);
        m_loopMoves.push_back(std::move(pLoopMove));
    }

    m_pCOLoopScale = std::make_unique<ControlObject>(ConfigKey(group, "loop_scale"), false);
    connect(m_pCOLoopScale.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotLoopScale);
    m_pLoopHalveButton = std::make_unique<ControlPushButton>(ConfigKey(group, "loop_halve"));
    m_pLoopHalveButton->setKbdRepeatable(true);
    connect(m_pLoopHalveButton.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotLoopHalve);
    m_pLoopDoubleButton = std::make_unique<ControlPushButton>(ConfigKey(group, "loop_double"));
    m_pLoopDoubleButton->setKbdRepeatable(true);
    connect(m_pLoopDoubleButton.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotLoopDouble);

    m_pLoopRemoveButton = std::make_unique<ControlPushButton>(ConfigKey(group, "loop_remove"));
    m_pLoopRemoveButton->setButtonMode(mixxx::control::ButtonMode::Trigger);
    connect(m_pLoopRemoveButton.get(),
            &ControlObject::valueChanged,
            this,
            &LoopingControl::slotLoopRemove);

    m_pPlayButton = ControlObject::getControl(ConfigKey(group, "play"));

    m_pRepeatButton = ControlObject::getControl(ConfigKey(group, "repeat"));
}

LoopingControl::~LoopingControl() = default;

void LoopingControl::slotLoopScale(double scaleFactor) {
    LoopInfo loopInfo = m_loopInfo.getValue();
    if (!loopInfo.loop.isValid()) {
        return;
    }

    const mixxx::audio::FrameDiff_t loopLength =
            loopInfo.loop.length() * scaleFactor;
    const FrameInfo info = frameInfo();
    const auto trackEndPosition = info.trackEndPosition;
    if (!trackEndPosition.isValid()) {
        return;
    }

    // Abandon loops that are too short of extend beyond the end of the file.
    if (loopLength < kMinimumAudibleLoopSizeFrames ||
            loopInfo.loop.startPosition + loopLength > trackEndPosition) {
        return;
    }

    loopInfo.loop.endPosition = loopInfo.loop.startPosition + loopLength;

    // TODO(XXX) we could be smarter about taking the active beatloop, scaling
    // it by the desired amount and trying to find another beatloop that matches
    // it, but for now we just clear the active beat loop if somebody scales.
    clearActiveBeatLoop();

    // Don't allow 0 samples loop, so one can still manipulate it
    if (loopInfo.loop.endPosition == loopInfo.loop.startPosition) {
        if ((loopInfo.loop.endPosition + 1) >= trackEndPosition) {
            loopInfo.loop.startPosition -= 1;
        } else {
            loopInfo.loop.endPosition += 1;
        }
    }
    // Do not allow loops to go past the end of the song
    else if (loopInfo.loop.endPosition > trackEndPosition) {
        loopInfo.loop.endPosition = trackEndPosition;
    }

    // Reseek if the loop shrank out from under the playposition.
    loopInfo.seekMode = (m_bLoopingEnabled && scaleFactor < 1.0)
            ? LoopSeekMode::Changed
            : LoopSeekMode::MovedOut;

    m_loopInfo.setValue(loopInfo);
    emit loopUpdated(loopInfo.loop.startPosition, loopInfo.loop.endPosition);

    // Update CO for loop end marker
    m_pCOLoopEndPosition->set(loopInfo.loop.endPosition.toEngineSamplePos());
}

void LoopingControl::slotLoopHalve(double pressed) {
    if (pressed <= 0.0) {
        return;
    }

    m_pCOBeatLoopSize->set(m_pCOBeatLoopSize->get() / 2.0);
}

void LoopingControl::slotLoopDouble(double pressed) {
    if (pressed <= 0.0) {
        return;
    }

    m_pCOBeatLoopSize->set(m_pCOBeatLoopSize->get() * 2.0);
}

void LoopingControl::process(const double rate,
        mixxx::audio::FramePos currentPosition,
        const std::size_t bufferSize) {
    Q_UNUSED(bufferSize);

    const auto previousPosition = m_currentPosition.getValue();

    if (previousPosition != currentPosition) {
        m_currentPosition.setValue(currentPosition);
    } else {
        // no transport, so we have to do scheduled seeks here
        LoopInfo loopInfo = m_loopInfo.getValue();
        if (m_bLoopingEnabled &&
                m_loopAdjustTarget == LoopAdjustTarget::None &&
                loopInfo.loop.isValid()) {
            if (loopInfo.loop != m_oldLoop) {
                // bool seek is only valid after the loop has changed
                if (loopInfo.seekMode == LoopSeekMode::Changed) {
                    // here the loop has changed and the play position
                    // should be moved with it
                    const auto targetPosition =
                            adjustedPositionInsideAdjustedLoop(currentPosition,
                                    rate < 0, // reverse
                                    m_oldLoop,
                                    loopInfo.loop);
                    if (targetPosition.isValid()) {
                        // jump immediately
                        seekAbs(targetPosition);
                    }
                }
                m_oldLoop = loopInfo.loop;
            }
        }
    }

    if (m_loopAdjustTarget == LoopAdjustTarget::LoopIn) {
        setLoopInToCurrentPosition();
    } else if (m_loopAdjustTarget == LoopAdjustTarget::LoopOut) {
        setLoopOutToCurrentPosition();
    }
}

mixxx::audio::FramePos LoopingControl::nextTrigger(bool reverse,
        mixxx::audio::FramePos currentPosition,
        mixxx::audio::FramePos* pTargetPosition) {
    *pTargetPosition = mixxx::audio::kInvalidFramePos;

    LoopInfo loopInfo = m_loopInfo.getValue();

    if (m_loopAdjustTargetOld != m_loopAdjustTarget) {
        auto oldTarget = std::exchange(m_loopAdjustTargetOld, m_loopAdjustTarget);

        // When a loop point button is released, jump to the opposite end to
        // avoid falling out and disabling the loop. Not needed in quantized
        // mode: the newly set point is always ahead of the playhead, so the
        // regular trigger logic below handles it.
        if (!quantizeEnabledAndHasTrueTrackBeats()) {
            if (oldTarget == LoopAdjustTarget::LoopIn && reverse) {
                m_oldLoop = loopInfo.loop;
                *pTargetPosition = loopInfo.loop.endPosition;
                return currentPosition;
            }
            if (oldTarget == LoopAdjustTarget::LoopOut && !reverse) {
                m_oldLoop = loopInfo.loop;
                *pTargetPosition = loopInfo.loop.startPosition;
                return currentPosition;
            }
        }
    }

    if (m_bLoopingEnabled && loopInfo.loop.isValid()) {
        if (m_loopAdjustTarget == LoopAdjustTarget::None) {
            if (loopInfo.loop != m_oldLoop) {
                // bool seek is only valid after the loop has changed
                switch (loopInfo.seekMode) {
                case LoopSeekMode::Changed:
                    // here the loop has changed and the play position
                    // should be moved with it
                    *pTargetPosition = adjustedPositionInsideAdjustedLoop(currentPosition,
                            reverse,
                            m_oldLoop,
                            loopInfo.loop);
                    break;
                case LoopSeekMode::MovedOut: {
                    const bool movedOutForward = !reverse &&
                            loopInfo.loop.endPosition < currentPosition;
                    const bool movedOutReverse =
                            reverse && loopInfo.loop.startPosition > currentPosition;

                    // Check if we have moved out of the loop before we could enable it
                    if (movedOutForward || movedOutReverse) {
                        *pTargetPosition = adjustedPositionInsideAdjustedLoop(currentPosition,
                                reverse,
                                loopInfo.loop,
                                loopInfo.loop);
                    }
                    break;
                }
                case LoopSeekMode::None:
                    // Nothing to do here. This is used for enabling saved loops
                    // which we want to do without jumping to the loop start
                    // position.
                    break;
                }
                m_oldLoop = loopInfo.loop;
                if (pTargetPosition->isValid()) {
                    // jump immediately
                    return currentPosition;
                }
            }
            if (reverse) {
                *pTargetPosition = loopInfo.loop.endPosition;
                return loopInfo.loop.startPosition;
            } else {
                *pTargetPosition = loopInfo.loop.startPosition;
                return loopInfo.loop.endPosition;
            }
        } else {
            // LOOP in or out button is pressed for adjusting.
            // Jump back to loop start, when reaching the track end this
            // prevents that the track stops outside the adjusted loop.
            if (!reverse) {
                if (m_loopAdjustTarget == LoopAdjustTarget::LoopIn) {
                    // Just in case the user does not release loop-in in time.
                    *pTargetPosition = m_oldLoop.startPosition;
                    return loopInfo.loop.endPosition;
                }
                const FrameInfo info = frameInfo();
                *pTargetPosition = loopInfo.loop.startPosition;
                return info.trackEndPosition;
            } else {
                if (m_loopAdjustTarget == LoopAdjustTarget::LoopOut) {
                    // Just in case the user does not release loop-out in time.
                    *pTargetPosition = m_oldLoop.endPosition;
                    return loopInfo.loop.startPosition;
                }
            }
        }
    }

    // Return trigger if repeat is enabled
    if (m_pRepeatButton->toBool()) {
        const FrameInfo info = frameInfo();
        if (reverse) {
            *pTargetPosition = info.trackEndPosition;
            return mixxx::audio::kStartFramePos;
        } else {
            *pTargetPosition = mixxx::audio::kStartFramePos;
            return info.trackEndPosition;
        }
    }

    return mixxx::audio::kInvalidFramePos;
}

mixxx::audio::FramePos LoopingControl::getTrackFrame() const {
    const FrameInfo info = frameInfo();
    return info.trackEndPosition;
}

mixxx::BeatsPointer LoopingControl::getFake60BpmBeats() const {
    return mixxx::Beats::fromConstTempo(
            frameInfo().sampleRate,
            mixxx::audio::kStartFramePos,
            mixxx::Bpm(60.0));
}

void LoopingControl::hintReader(gsl::not_null<HintVector*> pHintList) {
    const auto loop = m_loopInfo.getValue().loop;
    Hint loop_hint;
    // If the loop is enabled, then this is high priority because we will loop
    // sometime potentially very soon! The current audio itself is priority 1,
    // but we will issue ourselves at priority 2.
    if (m_bLoopingEnabled) {
        // If we're looping, hint the loop in and loop out, in case we reverse
        // into it. We could save information from process to tell which
        // direction we're going in, but that this is much simpler, and hints
        // aren't that bad to make anyway.
        if (loop.startPosition.isValid()) {
            loop_hint.type = Hint::Type::LoopStartEnabled;
            loop_hint.frame = static_cast<SINT>(
                    loop.startPosition.toLowerFrameBoundary().value());
            loop_hint.frameCount = Hint::kFrameCountForward;
            pHintList->append(loop_hint);
        }
        if (loop.endPosition.isValid()) {
            loop_hint.type = Hint::Type::LoopEndEnabled;
            loop_hint.frame = static_cast<SINT>(
                    loop.endPosition.toUpperFrameBoundary().value());
            loop_hint.frameCount = Hint::kFrameCountBackward;
            pHintList->append(loop_hint);
        }
    } else {
        if (loop.startPosition.isValid()) {
            loop_hint.type = Hint::Type::LoopStart;
            loop_hint.frame = static_cast<SINT>(
                    loop.startPosition.toLowerFrameBoundary().value());
            loop_hint.frameCount = Hint::kFrameCountForward;
            pHintList->append(loop_hint);
        }
        // We anticipate a potential loop being set from its end point
        mixxx::BeatsPointer pBeats = m_pBeats;
        if (!pBeats) {
            return;
        }
        double beats = m_pCOBeatLoopSize->get();
        bool quantize = m_pQuantizeEnabled->toBool();
        auto currentPosition = !quantize
                ? m_currentPosition.getValue()
                : findQuantizedBeatloopStart(
                          pBeats, m_currentPosition.getValue(), beats);
        loop_hint.type = Hint::Type::LoopStart;
        loop_hint.frame = static_cast<SINT>(
                pBeats->findNBeatsFromPosition(currentPosition, -beats)
                        .toLowerFrameBoundary()
                        .value());
        loop_hint.frameCount = Hint::kFrameCountForward;
    }
}

mixxx::audio::FramePos LoopingControl::getSyncPositionInsideLoop(
        mixxx::audio::FramePos requestedPlayPosition,
        mixxx::audio::FramePos syncedPlayPosition) {
    // no loop, no adjustment
    if (!m_bLoopingEnabled) {
        return syncedPlayPosition;
    }

    const auto loop = m_loopInfo.getValue().loop;

    // if the request itself is outside loop do nothing
    // loop will be disabled later by notifySeek(...) as is was explicitly requested by the user
    // if the requested position is the exact end of a loop it should also be disabled later by notifySeek(...)
    if (!loop.containsForward(requestedPlayPosition)) {
        return syncedPlayPosition;
    }

    // the requested position is inside the loop (e.g hotcue at start)

    // the synced position is in front of the loop
    // adjust the synced position to same amount in front of the loop end
    if (syncedPlayPosition < loop.startPosition) {
        mixxx::audio::FrameDiff_t adjustment = loop.startPosition - syncedPlayPosition;

        // prevents jumping in front of the loop if loop is smaller than adjustment
        adjustment = fmod(adjustment, loop.length());

        // if the synced position is exactly the start of the loop we would end up at the exact end
        // as this would disable the loop in notifySeek() replace it with the start of the loop
        if (adjustment == 0) {
            return loop.startPosition;
        }
        return loop.endPosition - adjustment;
    }

    // the synced position is behind the loop
    // adjust the synced position to same amount behind the loop start
    if (syncedPlayPosition >= loop.endPosition) {
        mixxx::audio::FrameDiff_t adjustment = syncedPlayPosition - loop.endPosition;

        // prevents jumping behind the loop if loop is smaller than adjustment
        adjustment = fmod(adjustment, loop.length());

        return loop.startPosition + adjustment;
    }

    // both, requested and synced position are inside the loop -> do nothing
    return syncedPlayPosition;
}

void LoopingControl::setBeatLoop(mixxx::audio::FramePos startPosition, bool enabled) {
    VERIFY_OR_DEBUG_ASSERT(startPosition.isValid()) {
        return;
    }

    mixxx::BeatsPointer pBeats = m_pBeats;
    if (!pBeats) {
        return;
    }

    double beatloopSize = m_pCOBeatLoopSize->get();

    // TODO(XXX): This is not realtime safe. See this Zulip discussion for details:
    // https://mixxx.zulipchat.com/#narrow/stream/109171-development/topic/getting.20locks.20out.20of.20Beats
    const auto endPosition = pBeats->findNBeatsFromPosition(startPosition, beatloopSize);
    if (endPosition.isValid()) {
        setLoop(startPosition, endPosition, enabled);
    }
}

void LoopingControl::setLoop(mixxx::audio::FramePos startPosition,
        mixxx::audio::FramePos endPosition,
        bool enabled) {
    VERIFY_OR_DEBUG_ASSERT(startPosition.isValid() && endPosition.isValid() &&
            startPosition < endPosition) {
        return;
    }

    LoopInfo loopInfo = m_loopInfo.getValue();
    if (loopInfo.loop.startPosition != startPosition || loopInfo.loop.endPosition != endPosition) {
        // Copy saved loop parameters to active loop
        loopInfo.loop.startPosition = startPosition;
        loopInfo.loop.endPosition = endPosition;
        loopInfo.seekMode = LoopSeekMode::None;
        clearActiveBeatLoop();
        m_loopInfo.setValue(loopInfo);
        m_pCOLoopStartPosition->set(loopInfo.loop.startPosition.toEngineSamplePos());
        m_pCOLoopEndPosition->set(loopInfo.loop.endPosition.toEngineSamplePos());
    }
    setLoopingEnabled(enabled);

    // Seek back to loop in position if we're already behind the loop end.
    //
    // TODO(Holzhaus): This needs to be reverted as soon as GUI controls for
    // controlling saved loop behaviour are in place, because this change makes
    // saved loops very risky to use and might potentially mess up your mix.
    // See https://github.com/mixxxdj/mixxx/pull/2194#issuecomment-721847833
    // for details.
    if (enabled && m_currentPosition.getValue() > loopInfo.loop.endPosition) {
        slotLoopInGoto(1);
    }

    // Don't allow loop size widget setting to trigger creation of another loop.
    m_pCOBeatLoopSize->blockSignals(true);
    m_pCOBeatLoopSize->setAndConfirm(findBeatloopSizeForLoop(startPosition, endPosition));
    m_pCOBeatLoopSize->blockSignals(false);
}

void LoopingControl::setLoopInToCurrentPosition() {
    // set loop-in position
    const mixxx::BeatsPointer pBeats = m_pBeats;
    LoopInfo loopInfo = m_loopInfo.getValue();
    mixxx::audio::FramePos quantizedBeatPosition;
    const FrameInfo info = frameInfo();
    // Note: currentPos can be past the end of the track, in the padded
    // silence of the last buffer. This position might be not reachable in
    // a future runs, depending on the buffering.
    mixxx::audio::FramePos position = math_min(info.currentPosition, info.trackEndPosition);
    if (quantizeEnabledAndHasTrueTrackBeats()) {
        quantizedBeatPosition = quantizeToNearestBeat(
                pBeats, position, info.trackEndPosition, m_loopAdjustTarget);
        if (quantizedBeatPosition.isValid()) {
            position = quantizedBeatPosition;
        }
    }

    // Reset the loop out position if it is before the loop in so that loops
    // cannot be inverted.
    if (loopInfo.loop.endPosition.isValid() && loopInfo.loop.endPosition <= position) {
        loopInfo.loop.endPosition = mixxx::audio::kInvalidFramePos;
        m_pCOLoopEndPosition->set(loopInfo.loop.endPosition.toEngineSamplePosMaybeInvalid());
        if (m_bLoopingEnabled) {
            setLoopingEnabled(false);
        }
    }

    // If we're looping and the loop-in and out points are now so close
    //  that the loop would be inaudible, set the in point to the smallest
    //  pre-defined beatloop size instead (when possible)
    if (loopInfo.loop.endPosition.isValid() &&
            (loopInfo.loop.endPosition - position) < kMinimumAudibleLoopSizeFrames) {
        if (quantizedBeatPosition.isValid() && pBeats) {
            position = pBeats->findNthBeat(quantizedBeatPosition, -2);
            if (!position.isValid() ||
                    (loopInfo.loop.endPosition - position) <
                            kMinimumAudibleLoopSizeFrames) {
                position = loopInfo.loop.endPosition - kMinimumAudibleLoopSizeFrames;
            }
        } else {
            position = loopInfo.loop.endPosition - kMinimumAudibleLoopSizeFrames;
        }
    }

    loopInfo.loop.startPosition = position;

    m_pCOLoopStartPosition->set(loopInfo.loop.startPosition.toEngineSamplePosMaybeInvalid());

    // start looping
    if (loopInfo.loop.isValid()) {
        setLoopingEnabled(true);
        loopInfo.seekMode = LoopSeekMode::Changed;
    } else {
        loopInfo.seekMode = LoopSeekMode::MovedOut;
    }

    if (quantizeEnabledAndHasTrueTrackBeats() &&
            loopInfo.loop.isValid() &&
            loopInfo.loop.startPosition < loopInfo.loop.endPosition) {
        m_pCOBeatLoopSize->setAndConfirm(pBeats->numBeatsInRange(
                loopInfo.loop.startPosition, loopInfo.loop.endPosition));
        updateBeatLoopingControls();
    } else {
        clearActiveBeatLoop();
    }

    m_loopInfo.setValue(loopInfo);
    // qDebug() << "set loop_in to " << loopInfo.loop.startPosition;
}

// Clear the last active loop while saved loop (cue + info) remains untouched
void LoopingControl::slotLoopRemove() {
    setLoopingEnabled(false);
    clearLoopInfoAndControls();
    // The loop cue is stored by BaseTrackPlayerImpl::unloadTrack()
    // if the loop is valid, else it is removed.
    // We remove it here right away so the loop is not restored
    // when the track is loaded to another player in the meantime.
    auto pLoadedTrack = getEngineBuffer()->getLoadedTrack();
    if (!pLoadedTrack) {
        return;
    }
    const QList<CuePointer> cuePoints = pLoadedTrack->getCuePoints();
    for (const auto& pCue : cuePoints) {
        if (pCue->getType() == mixxx::CueType::Loop && pCue->getHotCue() == Cue::kNoHotCue) {
            pLoadedTrack->removeCue(pCue);
            return;
        }
    }
}

void LoopingControl::clearLoopInfoAndControls() {
    LoopInfo loopInfo;
    m_loopInfo.setValue(loopInfo);
    m_oldLoop = loopInfo.loop;
    m_pCOLoopStartPosition->set(loopInfo.loop.startPosition.toEngineSamplePosMaybeInvalid());
    m_pCOLoopEndPosition->set(loopInfo.loop.endPosition.toEngineSamplePosMaybeInvalid());
}

void LoopingControl::slotLoopIn(double pressed) {
    if (!m_pTrack) {
        return;
    }

    // If loop is enabled, suspend looping and set the loop in point
    // when this button is released.
    if (m_bLoopingEnabled) {
        if (pressed > 0.0) {
            m_loopAdjustTarget = LoopAdjustTarget::LoopIn;
        } else {
            setLoopInToCurrentPosition();
            m_loopAdjustTarget = LoopAdjustTarget::None;
            const auto loop = m_loopInfo.getValue().loop;
            if (loop.startPosition < loop.endPosition) {
                emit loopUpdated(loop.startPosition, loop.endPosition);
            } else {
                emit loopReset();
            }
        }
    } else {
        emit loopReset();
        if (pressed > 0.0) {
            setLoopInToCurrentPosition();
        }
        m_loopAdjustTarget = LoopAdjustTarget::None;
    }
}

void LoopingControl::slotLoopInGoto(double pressed) {
    if (pressed == 0.0) {
        return;
    }

    const auto loopInPosition = m_loopInfo.getValue().loop.startPosition;
    if (loopInPosition.isValid()) {
        seekAbs(loopInPosition);
    }
}

void LoopingControl::setLoopOutToCurrentPosition() {
    mixxx::BeatsPointer pBeats = m_pBeats;
    LoopInfo loopInfo = m_loopInfo.getValue();
    mixxx::audio::FramePos quantizedBeatPosition;
    FrameInfo info = frameInfo();
    // Note: currentPos can be past the end of the track, in the padded
    // silence of the last buffer. This position might be not reachable in
    // a future runs, depending on the buffering.
    mixxx::audio::FramePos position = math_min(info.currentPosition, info.trackEndPosition);
    if (quantizeEnabledAndHasTrueTrackBeats()) {
        quantizedBeatPosition = quantizeToNearestBeat(
                pBeats, position, info.trackEndPosition, m_loopAdjustTarget);
        if (quantizedBeatPosition.isValid()) {
            // Note: with quantize enabled and playpos AFTER an inactive loop,
            // the new loop_out might snap to the exact the same position as before.
            // Then m_oldLoop would be unchanged and process() would not seek back
            // inside the loop, so we would (re)create and activate a loop
            // we'd never reach (when playing forward).
            // Invalidate the old loop end so adjustedPositionInsideAdjustedLoop()
            // will return a position inside the new/old loop.
            if (position > quantizedBeatPosition &&
                    quantizedBeatPosition == m_oldLoop.endPosition) {
                m_oldLoop.endPosition = mixxx::audio::kInvalidFramePos;
            }
            position = quantizedBeatPosition;
        }
    }

    // If the user is trying to set a loop-out before the loop in or without
    // having a loop-in, then ignore it.
    if (!loopInfo.loop.startPosition.isValid() || position <= loopInfo.loop.startPosition) {
        return;
    }

    // If the loop-in and out points are set so close that the loop would be
    // inaudible (which can happen easily with quantize-to-beat enabled,)
    // use the smallest pre-defined beatloop instead (when possible)
    if ((position - loopInfo.loop.startPosition) < kMinimumAudibleLoopSizeFrames) {
        if (quantizedBeatPosition.isValid() && pBeats) {
            position = pBeats->findNthBeat(quantizedBeatPosition, 2);
            if (!position.isValid() ||
                    (position - loopInfo.loop.startPosition) <
                            kMinimumAudibleLoopSizeFrames) {
                position = loopInfo.loop.startPosition + kMinimumAudibleLoopSizeFrames;
            }
        } else {
            position = loopInfo.loop.startPosition + kMinimumAudibleLoopSizeFrames;
        }
    }

    // set loop out position
    loopInfo.loop.endPosition = position;

    m_pCOLoopEndPosition->set(loopInfo.loop.endPosition.toEngineSamplePosMaybeInvalid());

    // start looping
    if (loopInfo.loop.isValid()) {
        setLoopingEnabled(true);
        loopInfo.seekMode = LoopSeekMode::Changed;
    } else {
        loopInfo.seekMode = LoopSeekMode::MovedOut;
    }

    if (quantizeEnabledAndHasTrueTrackBeats()) {
        m_pCOBeatLoopSize->setAndConfirm(pBeats->numBeatsInRange(
                loopInfo.loop.startPosition, loopInfo.loop.endPosition));
        updateBeatLoopingControls();
    } else {
        clearActiveBeatLoop();
    }
    // qDebug() << "set loop_out to " << loopInfo.loop.endPosition;

    m_loopInfo.setValue(loopInfo);
}

void LoopingControl::setRateControl(RateControl* rateControl) {
    m_pRateControl = rateControl;
}

void LoopingControl::slotLoopOut(double pressed) {
    if (m_pTrack == nullptr) {
        return;
    }

    // If loop is enabled, suspend looping and set the loop out point
    // when this button is released.
    if (m_bLoopingEnabled) {
        if (pressed > 0.0) {
            m_loopAdjustTarget = LoopAdjustTarget::LoopOut;
        } else {
            // If this button was pressed to set the loop out point when loop
            // was disabled, that will enable looping, so avoid moving the
            // loop out point when the button is released.
            if (!m_bLoopOutPressedWhileLoopDisabled) {
                setLoopOutToCurrentPosition();
                const auto loop = m_loopInfo.getValue().loop;
                if (loop.startPosition < loop.endPosition) {
                    emit loopUpdated(loop.startPosition, loop.endPosition);
                } else {
                    emit loopReset();
                }
                m_loopAdjustTarget = LoopAdjustTarget::None;
            } else {
                m_bLoopOutPressedWhileLoopDisabled = false;
            }
        }
    } else {
        emit loopReset();
        if (pressed > 0.0) {
            setLoopOutToCurrentPosition();
            m_bLoopOutPressedWhileLoopDisabled = true;
        }
        m_loopAdjustTarget = LoopAdjustTarget::None;
    }
}

void LoopingControl::slotLoopOutGoto(double pressed) {
    if (pressed == 0.0) {
        return;
    }

    const auto loopOutPosition = m_loopInfo.getValue().loop.endPosition;
    if (loopOutPosition.isValid()) {
        seekAbs(loopOutPosition);
    }
}

void LoopingControl::slotLoopExit(double val) {
    if (!m_pTrack || val <= 0.0) {
        return;
    }

    // If we're looping, stop looping
    if (m_bLoopingEnabled) {
        setLoopingEnabled(false);
    }
}

void LoopingControl::slotLoopEnabledValueChangeRequest(double value) {
    if (!m_pTrack) {
        return;
    }

    if (value > 0.0) {
        // Requested to set loop_enabled to 1
        if (m_bLoopingEnabled) {
            VERIFY_OR_DEBUG_ASSERT(m_pCOLoopEnabled->toBool()) {
                m_pCOLoopEnabled->setAndConfirm(1.0);
            }
        } else {
            // Looping is currently disabled, try to enable the loop. In
            // contrast to the reloop_toggle CO, we jump in no case.
            const auto loop = m_loopInfo.getValue().loop;
            if (loop.isValid() && loop.startPosition <= loop.endPosition) {
                // setAndConfirm is called by setLoopingEnabled
                setLoopingEnabled(true);
            }
        }
    } else {
        // Requested to set loop_enabled to 0
        if (m_bLoopingEnabled) {
            // Looping is currently enabled, disable the loop. If loop roll
            // was active, also disable slip.
            if (m_bLoopRollActive) {
                m_pSlipEnabled->set(0);
                m_bLoopRollActive = false;
                m_activeLoopRolls.clear();
            }
            // setAndConfirm is called by setLoopingEnabled
            setLoopingEnabled(false);
        } else {
            VERIFY_OR_DEBUG_ASSERT(!m_pCOLoopEnabled->toBool()) {
                m_pCOLoopEnabled->setAndConfirm(0.0);
            }
        }
    }
}

void LoopingControl::slotReloopToggle(double val) {
    if (!m_pTrack || val <= 0.0) {
        return;
    }

    // If we're looping, stop looping
    if (m_bLoopingEnabled) {
        // If loop roll was active, also disable slip.
        if (m_bLoopRollActive) {
            m_pSlipEnabled->set(0);
            m_bLoopRollActive = false;
            m_activeLoopRolls.clear();
        }
        setLoopingEnabled(false);
        //qDebug() << "reloop_toggle looping off";
    } else {
        // If we're not looping, enable the loop. If the loop is ahead of the
        // current play position, do not jump to it.
        const auto loop = m_loopInfo.getValue().loop;
        if (loop.isValid() && loop.startPosition <= loop.endPosition) {
            setLoopingEnabled(true);
            if (m_currentPosition.getValue() > loop.endPosition) {
                slotLoopInGoto(1);
            }
        }
        //qDebug() << "reloop_toggle looping on";
    }
}

void LoopingControl::slotReloopAndStop(double pressed) {
    if (pressed == 0.0) {
        return;
    }

    m_pPlayButton->set(0.0);

    const auto loopInPosition = m_loopInfo.getValue().loop.startPosition;
    if (loopInPosition.isValid()) {
        seekAbs(loopInPosition);
    }
    setLoopingEnabled(true);
}

void LoopingControl::slotLoopStartPos(double positionSamples) {
    // This slot is called before trackLoaded() for a new Track

    LoopInfo loopInfo = m_loopInfo.getValue();

    {
        const auto position =
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(
                        positionSamples);
        if (loopInfo.loop.startPosition == position) {
            // Nothing to do
            return;
        }
        loopInfo.loop.startPosition = position;
    }

    loopInfo.seekMode = LoopSeekMode::MovedOut;

    clearActiveBeatLoop();

    if (!loopInfo.loop.startPosition.isValid()) {
        emit loopReset();
        setLoopingEnabled(false);
    } else if (loopInfo.loop.endPosition.isValid() &&
            loopInfo.loop.endPosition <= loopInfo.loop.startPosition) {
        emit loopReset();
        loopInfo.loop.endPosition = mixxx::audio::kInvalidFramePos;
        m_pCOLoopEndPosition->set(kNoTrigger);
        setLoopingEnabled(false);
    }

    m_pCOLoopStartPosition->set(loopInfo.loop.startPosition.toEngineSamplePosMaybeInvalid());
    m_loopInfo.setValue(loopInfo);
}

void LoopingControl::slotLoopEndPos(double positionSamples) {
    // This slot is called before trackLoaded() for a new Track
    const auto position = mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(positionSamples);

    LoopInfo loopInfo = m_loopInfo.getValue();
    if (position.isValid() && loopInfo.loop.endPosition == position) {
        //nothing to do
        return;
    }

    // Reject if the loop-in is not set, or if the new position is before the
    // start point (but not -1).
    if (position.isValid() &&
            (!loopInfo.loop.startPosition.isValid() || position <= loopInfo.loop.startPosition)) {
        m_pCOLoopEndPosition->set(loopInfo.loop.endPosition.toEngineSamplePosMaybeInvalid());
        return;
    }

    clearActiveBeatLoop();

    if (!position.isValid()) {
        emit loopReset();
        setLoopingEnabled(false);
    }
    loopInfo.loop.endPosition = position;
    loopInfo.seekMode = LoopSeekMode::MovedOut;
    m_pCOLoopEndPosition->set(position.toEngineSamplePosMaybeInvalid());
    m_loopInfo.setValue(loopInfo);
}

// This is called from the engine thread
void LoopingControl::notifySeek(mixxx::audio::FramePos newPosition) {
    // Leave loop alone if we're in slip mode and if it was turned on
    // by something that was not a rolling beatloop.
    if (m_pSlipEnabled->toBool() && !m_bLoopRollActive) {
        return;
    }

    const auto loop = m_loopInfo.getValue().loop;
    const auto currentPosition = m_currentPosition.getValue();
    VERIFY_OR_DEBUG_ASSERT(m_pRateControl) {
        qWarning() << "LoopingControl: RateControl not set!";
        return;
    }
    bool reverse = m_pRateControl->isReverseButtonPressed();
    if (m_bLoopingEnabled) {
        // Disable loop when we jumping out, or over a catching loop,
        // using hot cues or waveform overview.
        // Jumping to the exact end of a loop is considered jumping out.
        if (loop.containsForward(currentPosition) ||
                loop.containsReverse(currentPosition)) {
            if ((reverse && newPosition > loop.endPosition) ||
                    (!reverse && newPosition < loop.startPosition)) {
                // jumping out of loop in "backwards"
                setLoopingEnabled(false);
            }
        }
        if ((reverse && currentPosition >= loop.startPosition &&
                    newPosition <= loop.startPosition) ||
                (!reverse && currentPosition <= loop.endPosition &&
                        newPosition >= loop.endPosition)) {
            // jumping out or to the exact "end" of a loop or
            // over a catching loop "forward"
            setLoopingEnabled(false);
        }
    }
}

void LoopingControl::setLoopingEnabled(bool enabled) {
    m_bLoopWasEnabledBeforeSlipEnable =
            !m_pSlipEnabled->toBool() && enabled && !m_bLoopRollActive;
    if (m_bLoopingEnabled == enabled) {
        return;
    }

    m_bLoopingEnabled = enabled;
    m_pCOLoopEnabled->setAndConfirm(enabled ? 1.0 : 0.0);
    BeatLoopingControl* pActiveBeatLoop = atomicLoadRelaxed(m_pActiveBeatLoop);
    if (pActiveBeatLoop != nullptr) {
        if (enabled) {
            pActiveBeatLoop->activate();
        } else {
            pActiveBeatLoop->deactivate();
        }
    }

    emit loopEnabledChanged(enabled);
}

void LoopingControl::trackLoaded(TrackPointer pNewTrack) {
    m_pTrack = pNewTrack;
    mixxx::BeatsPointer pBeats;
    if (pNewTrack) {
        pBeats = pNewTrack->getBeats();
    }
    trackBeatsUpdated(pBeats);
}

void LoopingControl::trackBeatsUpdated(mixxx::BeatsPointer pBeats) {
    clearActiveBeatLoop();
    if (pBeats) {
        m_pBeats = pBeats;
        m_trueTrackBeats = true;
    } else if (m_pTrack) {
        // no beats, use fake beats so we can use seconds as beat unit
        m_pBeats = getFake60BpmBeats();
        m_trueTrackBeats = false;
    } else {
        // no track, no beats
        m_pBeats = pBeats;
        m_trueTrackBeats = false;
    }
    const auto loop = m_loopInfo.getValue().loop;
    if (loop.isValid()) {
        double loaded_loop_size = findBeatloopSizeForLoop(
                loop.startPosition, loop.endPosition);
        if (loaded_loop_size != -1) {
            m_pCOBeatLoopSize->setAndConfirm(loaded_loop_size);
        }
    }
}

void LoopingControl::slotBeatLoopActivate(
        BeatLoopingControl* pBeatLoopControl, LoopAnchorPoint forcedAnchor) {
    if (!m_pTrack) {
        return;
    }

    // Maintain the current start point if there is an active loop currently
    // looping. slotBeatLoop will update m_pActiveBeatLoop if applicable. Note,
    // this used to only maintain the current start point if a beatloop was
    // enabled. See Issue #6957.
    slotBeatLoop(pBeatLoopControl->getSize(), m_bLoopingEnabled, true, forcedAnchor);
}

void LoopingControl::slotBeatLoopActivateRoll(
        BeatLoopingControl* pBeatLoopControl, LoopAnchorPoint forcedAnchor) {
    if (!m_pTrack) {
        return;
    }

    storeLoopInfo();

    // Disregard existing loops (except beatlooprolls).
    m_pSlipEnabled->set(1);
    slotBeatLoop(pBeatLoopControl->getSize(), m_bLoopRollActive, true, forcedAnchor);
    m_bLoopRollActive = true;
    m_activeLoopRolls.push(pBeatLoopControl->getSize());
}

void LoopingControl::slotBeatLoopDeactivate(BeatLoopingControl* pBeatLoopControl) {
    Q_UNUSED(pBeatLoopControl);
    setLoopingEnabled(false);
}

void LoopingControl::slotBeatLoopDeactivateRoll(BeatLoopingControl* pBeatLoopControl) {
    pBeatLoopControl->deactivate();
    const double size = pBeatLoopControl->getSize();
    // clang-tidy wants auto to be auto* because QStack inherits from QVector
    // and QVector::iterator is a pointer type in Qt5, but QStack inherits
    // from QList in Qt6 so QStack::iterator is not a pointer type in Qt6.
    // NOLINTNEXTLINE(readability-qualified-auto)
    auto i = m_activeLoopRolls.constBegin();
    while (i != m_activeLoopRolls.constEnd()) {
        if (size == *i) {
            i = constErase(&m_activeLoopRolls, i);
        } else {
            ++i;
        }
    }

    // Make sure slip mode is not turned off if it was turned on
    // by something that was not a rolling beatloop.
    if (m_bLoopRollActive && m_activeLoopRolls.empty()) {
        setLoopingEnabled(false);
        m_pSlipEnabled->set(0);
        m_bLoopRollActive = false;
    }

    // Return to the previous beatlooproll if necessary.
    // Else previous regular beatloop if no rolling loops are active.
    if (!m_activeLoopRolls.empty()) {
        slotBeatLoop(m_activeLoopRolls.top(), m_bLoopRollActive, true);
    } else {
        restoreLoopInfo();
    }
}

void LoopingControl::storeLoopInfo() {
    if (m_bLoopRollActive || !m_activeLoopRolls.empty()) {
        return;
    }

    const auto loop = m_loopInfo.getValue().loop;
    if (loop.isValid()) {
        m_prevLoop.setValue(loop);
    } else {
        // If we don't have a valid loop, yet, we store the current beatloop size.
        // This way this (default) value is available again for `beatloop_activate`
        // after disaling the (last) rolling loop.
        // Explicitly clear the last saved loop.
        m_prevLoop.setValue(Loop{});
        m_prevLoopSize = m_pCOBeatLoopSize->get();
    }
}

void LoopingControl::restoreLoopInfo() {
    if (m_bLoopRollActive || !m_activeLoopRolls.empty()) {
        return;
    }

    const auto prevLoop = m_prevLoop.getValue();
    if (prevLoop.isValid()) {
        setLoop(prevLoop.startPosition, prevLoop.endPosition, false);
        m_prevLoop.setValue(Loop{});
    } else {
        // This may happen when there was no loop set when we activated the
        // rolling loop that triggered storeLoopInfo(). Re-apply the loop size
        // we stored.
        clearLoopInfoAndControls();
        double prevLoopSize = m_prevLoopSize;
        if (prevLoopSize > 0) {
            m_pCOBeatLoopSize->setAndConfirm(m_prevLoopSize);
        }
    }
}

void LoopingControl::clearActiveBeatLoop() {
    BeatLoopingControl* pOldBeatLoop = m_pActiveBeatLoop.fetchAndStoreAcquire(nullptr);
    if (pOldBeatLoop != nullptr) {
        pOldBeatLoop->deactivate();
    }
}

bool LoopingControl::currentLoopMatchesBeatloopSize(const Loop& loop) const {
    const mixxx::BeatsPointer pBeats = m_pBeats;
    if (!pBeats) {
        return false;
    }

    if (!loop.startPosition.isValid()) {
        return false;
    }

    // Calculate where the loop out point would be if it is a beatloop
    const auto loopEndPosition = pBeats->findNBeatsFromPosition(
            loop.startPosition, m_pCOBeatLoopSize->get());

    return positionNear(loop.endPosition, loopEndPosition);
}

bool LoopingControl::quantizeEnabledAndHasTrueTrackBeats() const {
    return m_pQuantizeEnabled->toBool() && m_trueTrackBeats;
}

double LoopingControl::findBeatloopSizeForLoop(
        mixxx::audio::FramePos startPosition,
        mixxx::audio::FramePos endPosition) const {
    const mixxx::BeatsPointer pBeats = m_pBeats;
    if (!pBeats) {
        return -1;
    }

    for (auto beatSize : kBeatSizes) {
        const auto loopEndPosition = pBeats->findNBeatsFromPosition(startPosition, beatSize);
        if (loopEndPosition.isValid()) {
            if (endPosition > (loopEndPosition - 1) && endPosition < (loopEndPosition + 1)) {
                return beatSize;
            }
        }
    }
    return -1;
}

void LoopingControl::updateBeatLoopingControls() {
    // O(n) search, but there are only ~10-ish beatloop controls so this is
    // fine.
    double dBeatloopSize = m_pCOBeatLoopSize->get();
    for (auto const& pBeatLoopControl : std::as_const(m_beatLoops)) {
        if (pBeatLoopControl->getSize() == dBeatloopSize) {
            if (m_bLoopingEnabled) {
                pBeatLoopControl->activate();
            }
            BeatLoopingControl* pOldBeatLoop =
                    m_pActiveBeatLoop.fetchAndStoreRelease(pBeatLoopControl.get());
            if (pOldBeatLoop != nullptr && pOldBeatLoop != pBeatLoopControl.get()) {
                pOldBeatLoop->deactivate();
            }
            return;
        }
    }
    // If the loop did not return from the function yet, dBeatloopSize does
    // not match any of the BeatLoopingControls' sizes.
    clearActiveBeatLoop();
}



void LoopingControl::slotBeatLoop(double beats,
        bool keepSetPoint,
        bool enable,
        LoopAnchorPoint forcedAnchor) {
    // If this is a "new" loop, stop tracking saved loop changes
    if (!keepSetPoint) {
        emit loopReset();
    }

    // if a seek was queued in the engine buffer move the current sample to its position
    const mixxx::audio::FramePos seekPosition = getEngineBuffer()->queuedSeekPosition();
    if (seekPosition.isValid()) {
        // seek position is already quantized if quantization is enabled
        m_currentPosition.setValue(seekPosition);
    }

    if (beats < 0) {
        // For now we do not handle negative beatloops.
        clearActiveBeatLoop();
        return;
    }
    beats = std::clamp(beats, kBeatSizes.front(), kBeatSizes.back());

    FrameInfo info = frameInfo();
    const auto trackEndPosition = info.trackEndPosition;
    const mixxx::BeatsPointer pBeats = m_pBeats;
    if (!trackEndPosition.isValid() || !pBeats) {
        clearActiveBeatLoop();
        m_pCOBeatLoopSize->setAndConfirm(beats);
        return;
    }

    const LoopAnchorPoint loopAnchor = forcedAnchor == LoopAnchorPoint::None
            ? static_cast<LoopAnchorPoint>(m_pCOLoopAnchor->get())
            : forcedAnchor;
    // Calculate the new loop start and end positions
    const auto loop = m_loopInfo.getValue().loop;
    mixxx::audio::FramePos currentPosition = info.currentPosition;
    const bool anchorAtEnd = loopAnchor == LoopAnchorPoint::End;

    // Determine the anchor position — the fixed point from which the other
    // endpoint is computed.
    mixxx::audio::FramePos anchorPosition;
    if (keepSetPoint) {
        auto existing = anchorAtEnd ? loop.endPosition : loop.startPosition;
        anchorPosition = existing.isValid()
                ? existing
                : math_min(info.currentPosition, trackEndPosition);
    } else {
        // If running reverse, move the loop one loop size to the left.
        // Thus, the loops end will be closest to the current position
        VERIFY_OR_DEBUG_ASSERT(m_pRateControl) {
            qWarning() << "LoopingControl: RateControl not set!";
            return;
        }
        if (m_pRateControl->isReverseButtonPressed()) {
            currentPosition = pBeats->findNBeatsFromPosition(currentPosition, -beats);
        }

        bool quantize = quantizeEnabledAndHasTrueTrackBeats();
        anchorPosition = quantize
                ? findQuantizedBeatloopStart(pBeats, currentPosition, beats)
                : currentPosition;
    }

    // Compute both endpoints from the anchor
    LoopInfo newloopInfo;
    newloopInfo.seekMode = LoopSeekMode::MovedOut;
    if (anchorAtEnd) {
        newloopInfo.loop.endPosition = anchorPosition;
        newloopInfo.loop.startPosition = pBeats->findNBeatsFromPosition(anchorPosition, -beats);
    } else {
        newloopInfo.loop.startPosition = anchorPosition;
        newloopInfo.loop.endPosition = pBeats->findNBeatsFromPosition(anchorPosition, beats);
    }

    if (!newloopInfo.loop.isValid() ||
            newloopInfo.loop.startPosition >=
                    newloopInfo.loop.endPosition // happens when the call above fails
            || (newloopInfo.loop.endPosition > trackEndPosition &&
                       (enable || m_bLoopingEnabled))) { // Do not allow beat
                                                         // loops to go beyond
                                                         // the end of the track
        // If a track is loaded with beatloop_size larger than
        // the distance between the loop in point and
        // the end of the track, let beatloop_size be set to
        // a smaller size, but not get larger.
        const double previousBeatloopSize = m_pCOBeatLoopSize->get();
        const mixxx::audio::FramePos previousLoopEndPosition =
                pBeats->findNBeatsFromPosition(
                        newloopInfo.loop.startPosition, previousBeatloopSize);
        if (previousLoopEndPosition < newloopInfo.loop.startPosition &&
                beats < previousBeatloopSize) {
            m_pCOBeatLoopSize->setAndConfirm(beats);
        }
        return;
    }

    // When loading a new track or after setting a manual loop without quantize,
    // do not resize the existing loop until beatloop_size matches
    // the size of the existing loop.
    // Do not return immediately so beatloop_size can be updated.
    bool omitResize = false;
    if (!currentLoopMatchesBeatloopSize(loop) && !enable) {
        omitResize = true;
    }

    if (m_pCOBeatLoopSize->get() != beats) {
        m_pCOBeatLoopSize->setAndConfirm(beats);
    }

    // This check happens after setting m_pCOBeatLoopSize so
    // beatloop_size can be prepared without having a track loaded.
    if (!newloopInfo.loop.isValid()) {
        return;
    }

    if (omitResize) {
        return;
    }

    bool const loopEnabled = enable || m_bLoopingEnabled;

    switch (loopAnchor) {
    case LoopAnchorPoint::None:
    case LoopAnchorPoint::Start: {
        // If the start point has changed, or the loop is not enabled,
        // or if the endpoints are nearly the same, do not seek forward into the adjusted loop.
        if (keepSetPoint && loopEnabled && !nearlySameLoop(newloopInfo.loop, loop)) {
            newloopInfo.seekMode = LoopSeekMode::Changed;
        } else {
            newloopInfo.seekMode = LoopSeekMode::MovedOut;
        }
        break;
    }
    case LoopAnchorPoint::End:
        // If the end point is behind the current position and the loop is enabled, seek backward .
        if (!loopEnabled || newloopInfo.loop.endPosition > currentPosition) {
            newloopInfo.seekMode = LoopSeekMode::MovedOut;
        } else {
            newloopInfo.seekMode = LoopSeekMode::Changed;
            // If the loop is being enabled, flush the old loop status to force
            // LoopingControl::nextTrigger to evaluate the LoopSeekMode
            if (!m_bLoopingEnabled) {
                m_oldLoop = Loop{};
            }
        }
        break;
    }

    m_loopInfo.setValue(newloopInfo);
    emit loopUpdated(newloopInfo.loop.startPosition, newloopInfo.loop.endPosition);
    m_pCOLoopStartPosition->set(newloopInfo.loop.startPosition.toEngineSamplePos());
    m_pCOLoopEndPosition->set(newloopInfo.loop.endPosition.toEngineSamplePos());

    if (enable) {
        setLoopingEnabled(true);
    }
    updateBeatLoopingControls();
}

void LoopingControl::slotBeatLoopSizeChangeRequest(double beats) {
    // slotBeatLoop will call m_pCOBeatLoopSize->setAndConfirm if
    // new beatloop_size is valid

    double maxBeatLoopSize = kBeatSizes.back();
    double minBeatLoopSize = kBeatSizes.front();
    if ((beats < minBeatLoopSize) || (beats > maxBeatLoopSize)) {
        // Don't clamp the value here to not fall out of a measure
        return;
    }

    slotBeatLoop(beats, true, false);
}

void LoopingControl::slotBeatLoopToggle(double pressed) {
    if (pressed > 0) {
        if (m_bLoopingEnabled) {
            // Deactivate the loop if we're already looping
            setLoopingEnabled(false);
        } else {
            // Create a loop at current position
            slotBeatLoop(m_pCOBeatLoopSize->get());
        }
    }
}

void LoopingControl::slotBeatLoopRollActivate(double pressed) {
    if (pressed > 0.0) {
        if (m_bLoopingEnabled) {
            setLoopingEnabled(false);
            // Make sure slip mode is not turned off if it was turned on
            // by something that was not a rolling beatloop.
            if (m_bLoopRollActive) {
                m_pSlipEnabled->set(0.0);
                m_bLoopRollActive = false;
                m_activeLoopRolls.clear();
            }
        } else {
            storeLoopInfo();
            m_pSlipEnabled->set(1.0);
            slotBeatLoop(m_pCOBeatLoopSize->get());
            m_bLoopRollActive = true;
        }
    } else {
        setLoopingEnabled(false);
        // Make sure slip mode is not turned off if it was turned on
        // by something that was not a rolling beatloop.
        if (m_bLoopRollActive) {
            m_pSlipEnabled->set(0.0);
            m_bLoopRollActive = false;
            m_activeLoopRolls.clear();
        }
        restoreLoopInfo();
    }
}

void LoopingControl::slotBeatJump(double beats) {
    const mixxx::BeatsPointer pBeats = m_pBeats;
    if (!pBeats) {
        return;
    }

    const auto loop = m_loopInfo.getValue().loop;
    const auto currentPosition = m_currentPosition.getValue();

    if (m_bLoopingEnabled && m_loopAdjustTarget == LoopAdjustTarget::None &&
            (loop.containsForward(currentPosition) ||
                    loop.containsReverse(currentPosition))) {
        // If inside an active loop, move loop
        slotLoopMove(beats);
    } else {
        // seekExact bypasses Quantize, because a beat jump is implicit quantized
        const auto seekPosition = pBeats->findNBeatsFromPosition(currentPosition, beats);
        if (seekPosition.isValid()) {
            seekExact(seekPosition);
        }
    }
}

void LoopingControl::slotBeatJumpSizeChangeRequest(double beats) {
    // Use same limits as for beat loop size
    double maxBeatJumpSize = kBeatSizes.back();
    double minBeatJumpSize = kBeatSizes.front();

    if ((beats < minBeatJumpSize) || (beats > maxBeatJumpSize)) {
        // Don't clamp the value here to not fall out of a measure
        return;
    }

    m_pCOBeatJumpSize->setAndConfirm(beats);
}

void LoopingControl::slotBeatJumpSizeHalve(double pressed) {
    if (pressed > 0) {
        m_pCOBeatJumpSize->set(m_pCOBeatJumpSize->get() / 2);
    }
}

void LoopingControl::slotBeatJumpSizeDouble(double pressed) {
    if (pressed > 0) {
        m_pCOBeatJumpSize->set(m_pCOBeatJumpSize->get() * 2);
    }
}

void LoopingControl::slotBeatJumpForward(double pressed) {
    if (pressed > 0) {
        slotBeatJump(m_pCOBeatJumpSize->get());
    }
}

void LoopingControl::slotBeatJumpBackward(double pressed) {
    if (pressed > 0) {
        slotBeatJump(-1.0 * m_pCOBeatJumpSize->get());
    }
}

void LoopingControl::slotLoopMove(double beats) {
    const mixxx::BeatsPointer pBeats = m_pBeats;
    if (!pBeats || beats == 0) {
        return;
    }
    LoopInfo loopInfo = m_loopInfo.getValue();
    if (!loopInfo.loop.isValid()) {
        return;
    }

    FrameInfo info = frameInfo();
    if (BpmControl::getBeatContext(pBeats,
                info.currentPosition,
                nullptr,
                nullptr,
                nullptr,
                nullptr)) {
        const auto newLoopStartPosition =
                pBeats->findNBeatsFromPosition(loopInfo.loop.startPosition, beats);
        const auto newLoopEndPosition = currentLoopMatchesBeatloopSize(loopInfo.loop)
                ? pBeats->findNBeatsFromPosition(newLoopStartPosition, m_pCOBeatLoopSize->get())
                : pBeats->findNBeatsFromPosition(loopInfo.loop.endPosition, beats);

        // The track would stop as soon as the playhead crosses track end,
        // so we don't allow moving a loop beyond end.
        // https://github.com/mixxxdj/mixxx/issues/9478
        const auto trackEndPosition = info.trackEndPosition;
        if (!trackEndPosition.isValid() || newLoopEndPosition > trackEndPosition) {
            return;
        }
        // If we are looping make sure that the play head does not leave the
        // loop as a result of our adjustment.
        loopInfo.seekMode = m_bLoopingEnabled ? LoopSeekMode::Changed : LoopSeekMode::MovedOut;

        loopInfo.loop.startPosition = newLoopStartPosition;
        loopInfo.loop.endPosition = newLoopEndPosition;
        m_loopInfo.setValue(loopInfo);
        emit loopUpdated(loopInfo.loop.startPosition, loopInfo.loop.endPosition);
        m_pCOLoopStartPosition->set(loopInfo.loop.startPosition.toEngineSamplePosMaybeInvalid());
        m_pCOLoopEndPosition->set(loopInfo.loop.endPosition.toEngineSamplePosMaybeInvalid());
    }
}

// Used to simulate looping while slip mode is enabled
mixxx::audio::FramePos LoopingControl::adjustedPositionForCurrentLoop(
        mixxx::audio::FramePos currentPosition,
        bool reverse) {
    if (!m_bLoopingEnabled) {
        return currentPosition;
    }
    const auto loop = m_loopInfo.getValue().loop;
    const auto targetPosition = adjustedPositionInsideAdjustedLoop(
            currentPosition,
            reverse,
            loop,
            loop);
    if (targetPosition.isValid()) {
        return targetPosition;
    } else {
        return currentPosition;
    }
}



BeatJumpControl::BeatJumpControl(const QString& group, double size)
        : m_dBeatJumpSize(size) {
    m_pJumpForward = std::make_unique<ControlPushButton>(
            keyForControl(group, "beatjump_%1_forward", size));
    m_pJumpForward->setKbdRepeatable(true);
    connect(m_pJumpForward.get(),
            &ControlObject::valueChanged,
            this,
            &BeatJumpControl::slotJumpForward,
            Qt::DirectConnection);
    m_pJumpBackward = std::make_unique<ControlPushButton>(
            keyForControl(group, "beatjump_%1_backward", size));
    m_pJumpBackward->setKbdRepeatable(true);
    connect(m_pJumpBackward.get(),
            &ControlObject::valueChanged,
            this,
            &BeatJumpControl::slotJumpBackward,
            Qt::DirectConnection);
}

BeatJumpControl::~BeatJumpControl() = default;

void BeatJumpControl::slotJumpBackward(double pressed) {
    if (pressed > 0) {
        emit beatJump(-m_dBeatJumpSize);
    }
}

void BeatJumpControl::slotJumpForward(double pressed) {
    if (pressed > 0) {
        emit beatJump(m_dBeatJumpSize);
    }
}

LoopMoveControl::LoopMoveControl(const QString& group, double size)
        : m_dLoopMoveSize(size) {
    m_pMoveForward = std::make_unique<ControlPushButton>(
            keyForControl(group, "loop_move_%1_forward", size));
    connect(m_pMoveForward.get(),
            &ControlObject::valueChanged,
            this,
            &LoopMoveControl::slotMoveForward,
            Qt::DirectConnection);
    m_pMoveBackward = std::make_unique<ControlPushButton>(
            keyForControl(group, "loop_move_%1_backward", size));
    connect(m_pMoveBackward.get(),
            &ControlObject::valueChanged,
            this,
            &LoopMoveControl::slotMoveBackward,
            Qt::DirectConnection);
}

LoopMoveControl::~LoopMoveControl() = default;

void LoopMoveControl::slotMoveBackward(double v) {
    if (v > 0) {
        emit loopMove(-m_dLoopMoveSize);
    }
}

void LoopMoveControl::slotMoveForward(double v) {
    if (v > 0) {
        emit loopMove(m_dLoopMoveSize);
    }
}

BeatLoopingControl::BeatLoopingControl(const QString& group, double size)
        : m_dBeatLoopSize(size),
          m_bActive(false) {
    // This is the original beatloop control which is now deprecated. Its value
    // is the state of the beatloop control (1 for enabled, 0 for disabled).
    m_pLegacy = std::make_unique<ControlPushButton>(
            keyForControl(group, "beatloop_%1", size));
    m_pLegacy->setButtonMode(mixxx::control::ButtonMode::Toggle);
    connect(m_pLegacy.get(),
            &ControlObject::valueChanged,
            this,
            &BeatLoopingControl::slotLegacy,
            Qt::DirectConnection);
    // A push-button which activates the beatloop.
    m_pActivate = std::make_unique<ControlPushButton>(
            keyForControl(group, "beatloop_%1_activate", size));
    connect(
            m_pActivate.get(),
            &ControlObject::valueChanged,
            this,
            [this](double value) {
                slotActivate(value, LoopAnchorPoint::None);
            },
            Qt::DirectConnection);
    // And the same but setting it from the end point instead of starting
    m_pRActivate = std::make_unique<ControlPushButton>(
            keyForControl(group, "beatloop_r%1_activate", size));
    connect(m_pRActivate.get(),
            &ControlObject::valueChanged,
            this,
            &BeatLoopingControl::slotReverseActivate,
            Qt::DirectConnection);
    // A push-button which toggles the beatloop as active or inactive.
    m_pToggle = std::make_unique<ControlPushButton>(
            keyForControl(group, "beatloop_%1_toggle", size));
    connect(
            m_pToggle.get(),
            &ControlObject::valueChanged,
            this,
            [this](double value) {
                slotToggle(value, LoopAnchorPoint::None);
            },
            Qt::DirectConnection);
    // And the same but setting it from the end point instead of starting
    m_pRToggle = std::make_unique<ControlPushButton>(
            keyForControl(group, "beatloop_r%1_toggle", size));
    connect(m_pRToggle.get(),
            &ControlObject::valueChanged,
            this,
            &BeatLoopingControl::slotReverseToggle,
            Qt::DirectConnection);

    // A push-button which activates rolling beatloops
    m_pActivateRoll = std::make_unique<ControlPushButton>(
            keyForControl(group, "beatlooproll_%1_activate", size));
    connect(
            m_pActivateRoll.get(),
            &ControlObject::valueChanged,
            this,
            [this](double value) {
                slotActivateRoll(value, LoopAnchorPoint::None);
            },
            Qt::DirectConnection);
    // And the same but setting it from the end point instead of starting
    m_pRActivateRoll = std::make_unique<ControlPushButton>(
            keyForControl(group, "beatlooproll_r%1_activate", size));
    connect(m_pRActivateRoll.get(),
            &ControlObject::valueChanged,
            this,
            &BeatLoopingControl::slotReverseActivateRoll,
            Qt::DirectConnection);

    // An indicator control which is 1 if the beatloop is enabled and 0 if not.
    m_pEnabled = std::make_unique<ControlObject>(
            keyForControl(group, "beatloop_%1_enabled", size));
    m_pEnabled->setReadOnly();
}

void BeatLoopingControl::deactivate() {
    if (m_bActive) {
        m_bActive = false;
        m_pEnabled->forceSet(0);
        m_pLegacy->set(0);
    }
}

void BeatLoopingControl::activate() {
    if (!m_bActive) {
        m_bActive = true;
        m_pEnabled->forceSet(1);
        m_pLegacy->set(1);
    }
}

void BeatLoopingControl::slotLegacy(double v) {
    //qDebug() << "slotLegacy" << m_dBeatLoopSize << "v" << v;
    if (v > 0) {
        emit activateBeatLoop(this, LoopAnchorPoint::None);
    } else {
        emit deactivateBeatLoop(this);
    }
}

void BeatLoopingControl::slotActivate(double value, LoopAnchorPoint anchor) {
    //qDebug() << "slotActivate" << m_dBeatLoopSize << "value" << value;
    if (value == 0) {
        return;
    }
    emit activateBeatLoop(this, anchor);
}

void BeatLoopingControl::slotActivateRoll(double v, LoopAnchorPoint anchor) {
    //qDebug() << "slotActivateRoll" << m_dBeatLoopSize << "v" << v;
    if (v > 0) {
        emit activateBeatLoopRoll(this, anchor);
    } else {
        emit deactivateBeatLoopRoll(this);
    }
}

void BeatLoopingControl::slotToggle(double value, LoopAnchorPoint anchor) {
    //qDebug() << "slotToggle" << m_dBeatLoopSize << "value" << value;
    if (value == 0) {
        return;
    }
    if (m_bActive) {
        emit deactivateBeatLoop(this);
    } else {
        emit activateBeatLoop(this, anchor);
    }
}

void BeatLoopingControl::slotReverseActivate(double value) {
    slotActivate(value, LoopAnchorPoint::End);
}

void BeatLoopingControl::slotReverseActivateRoll(double v) {
    slotActivateRoll(v, LoopAnchorPoint::End);
}

void BeatLoopingControl::slotReverseToggle(double value) {
    slotToggle(value, LoopAnchorPoint::End);
}
