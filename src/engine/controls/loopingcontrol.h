#pragma once

#include <QObject>
#include <QStack>
#include <memory>
#include <vector>

#include "control/controlvalue.h"
#include "engine/controls/enginecontrol.h"
#include "preferences/usersettings.h"
#include "track/beats.h"
#include "track/track_decl.h"

class ControlPushButton;
class ControlObject;
class ControlProxy;
class RateControl;
class LoopMoveControl;
class BeatJumpControl;
class BeatLoopingControl;

enum class LoopSeekMode {
    Changed, // force the playposition to be inside the loop after adjusting it.
    MovedOut,
    None,
};

enum class LoopAdjustTarget {
    None,
    LoopIn,
    LoopOut,
};

enum class LoopAnchorPoint {
    Start, // The loop has been defined by its start point. Adjusting the
           // size will move the end point
    End,   // The loop has been defined by its end point. Adjusting the size
           // will move the end point
    None,  // Used to indicate the end of the enum type and the null type
};

struct Loop {
    mixxx::audio::FramePos startPosition = mixxx::audio::kInvalidFramePos;
    mixxx::audio::FramePos endPosition = mixxx::audio::kInvalidFramePos;

    bool isValid() const {
        return startPosition.isValid() && endPosition.isValid();
    }

    mixxx::audio::FrameDiff_t length() const {
        return endPosition - startPosition;
    }

    friend bool operator==(const Loop& lhs, const Loop& rhs) = default;
};

struct LoopInfo {
    Loop loop;
    LoopSeekMode seekMode = LoopSeekMode::None;
};

class LoopingControl : public EngineControl {
    Q_OBJECT
  public:
    static QList<double> getBeatSizes();

    LoopingControl(const QString& group, UserSettingsPointer pConfig);
    ~LoopingControl() override;

    // process() updates the internal state of the LoopingControl to reflect the
    // correct current sample. If a loop should be taken LoopingControl returns
    // the sample that should be seeked to. Otherwise it returns currentPosition.
    void process(const double rate,
            mixxx::audio::FramePos currentPosition,
            const std::size_t bufferSize) override;

    // nextTrigger returns the sample at which the engine will be triggered to
    // take a loop, given the value of currentPosition and the playback direction.
    virtual mixxx::audio::FramePos nextTrigger(bool reverse,
            mixxx::audio::FramePos currentPosition,
            mixxx::audio::FramePos* pTargetPosition);

    // hintReader will add to hintList hints both the loop in and loop out
    // sample, if set.
    void hintReader(gsl::not_null<HintVector*> pHintList) override;
    mixxx::audio::FramePos getSyncPositionInsideLoop(
            mixxx::audio::FramePos requestedPlayPosition,
            mixxx::audio::FramePos syncedPlayPosition);

    void notifySeek(mixxx::audio::FramePos position) override;

    // Wrapper to use adjustedPositionInsideAdjustedLoop() with the current loop.
    // Called from EngineBuffer while slip mode is enabled
    mixxx::audio::FramePos adjustedPositionForCurrentLoop(
            mixxx::audio::FramePos newPosition,
            bool reverse);

    void setBeatLoop(mixxx::audio::FramePos startPosition, bool enabled);
    void setLoop(mixxx::audio::FramePos startPosition,
            mixxx::audio::FramePos endPosition,
            bool enabled);

    LoopInfo getLoopInfo() const {
        return m_loopInfo.getValue();
    }

    void setRateControl(RateControl* rateControl);

    bool isLoopingEnabled() const {
        return m_bLoopingEnabled;
    }
    bool isAdjustLoopInActive() const {
        return m_loopAdjustTarget == LoopAdjustTarget::LoopIn;
    }
    bool isAdjustLoopOutActive() const {
        return m_loopAdjustTarget == LoopAdjustTarget::LoopOut;
    }
    bool isLoopRollActive() const {
        return m_bLoopRollActive;
    }
    bool loopWasEnabledBeforeSlipEnable() const {
        return m_bLoopWasEnabledBeforeSlipEnable;
    }

    void trackLoaded(TrackPointer pNewTrack) override;
    void trackBeatsUpdated(mixxx::BeatsPointer pBeats) override;

    mixxx::audio::FramePos getTrackFrame() const;

  signals:
    void loopReset();
    void loopEnabledChanged(bool enabled);
    void loopUpdated(mixxx::audio::FramePos startPosition, mixxx::audio::FramePos endPosition);

  public slots:
    void slotLoopIn(double pressed);
    void slotLoopInGoto(double);
    void slotLoopOut(double pressed);
    void slotLoopOutGoto(double);
    void slotLoopExit(double);
    void slotReloopToggle(double);
    void slotReloopAndStop(double);
    void slotLoopStartPos(double);
    void slotLoopEndPos(double);

    // Generate a loop of 'beats' length. It can also do fractions for a
    // beatslicing effect.
    void slotBeatLoop(double loopSize,
            bool keepSetPoint = false,
            bool enable = true,
            LoopAnchorPoint forcedAnchor =
                    LoopAnchorPoint::None);
    void slotBeatLoopSizeChangeRequest(double beats);
    void slotBeatLoopToggle(double pressed);
    void slotBeatLoopRollActivate(double pressed);
    void slotBeatLoopActivate(BeatLoopingControl* pBeatLoopControl,
            LoopAnchorPoint forcedAnchor =
                    LoopAnchorPoint::None);
    void slotBeatLoopActivateRoll(BeatLoopingControl* pBeatLoopControl,
            LoopAnchorPoint forcedAnchor =
                    LoopAnchorPoint::None);
    void slotBeatLoopDeactivate(BeatLoopingControl* pBeatLoopControl);
    void slotBeatLoopDeactivateRoll(BeatLoopingControl* pBeatLoopControl);

    // Jump forward or backward by beats.
    void slotBeatJump(double beats);
    void slotBeatJumpSizeChangeRequest(double beats);
    void slotBeatJumpSizeHalve(double pressed);
    void slotBeatJumpSizeDouble(double pressed);
    void slotBeatJumpForward(double pressed);
    void slotBeatJumpBackward(double pressed);

    // Move the loop by beats.
    void slotLoopMove(double beats);

    void slotLoopScale(double scaleFactor);
    void slotLoopDouble(double pressed);
    void slotLoopHalve(double pressed);
    void slotLoopRemove();

  private slots:
    void slotLoopEnabledValueChangeRequest(double enabled);

  private:
    void setLoopingEnabled(bool enabled);
    void setLoopInToCurrentPosition();
    void setLoopOutToCurrentPosition();

    void storeLoopInfo();
    void restoreLoopInfo();

    void clearActiveBeatLoop();
    void clearLoopInfoAndControls();
    void updateBeatLoopingControls();
    bool currentLoopMatchesBeatloopSize(const Loop& loop) const;
    bool quantizeEnabledAndHasTrueTrackBeats() const;

    // Fake beats that allow using looping/beatjump controls with no beats:
    // one 'beat' = one second
    mixxx::BeatsPointer getFake60BpmBeats() const;

    // Given loop in and out points, determine if this is a beatloop of a particular
    // size.
    double findBeatloopSizeForLoop(mixxx::audio::FramePos startPosition,
            mixxx::audio::FramePos endPosition) const;

    std::unique_ptr<ControlPushButton> m_pCOBeatLoopActivate;
    std::unique_ptr<ControlPushButton> m_pCOBeatLoopRollActivate;
    std::unique_ptr<ControlObject> m_pCOLoopStartPosition;
    std::unique_ptr<ControlObject> m_pCOLoopEndPosition;
    std::unique_ptr<ControlObject> m_pCOLoopEnabled;
    std::unique_ptr<ControlPushButton> m_pCOLoopAnchor;
    std::unique_ptr<ControlPushButton> m_pLoopInButton;
    std::unique_ptr<ControlPushButton> m_pLoopInGotoButton;
    std::unique_ptr<ControlPushButton> m_pLoopOutButton;
    std::unique_ptr<ControlPushButton> m_pLoopOutGotoButton;
    std::unique_ptr<ControlPushButton> m_pLoopExitButton;
    std::unique_ptr<ControlPushButton> m_pReloopToggleButton;
    std::unique_ptr<ControlPushButton> m_pReloopAndStopButton;
    std::unique_ptr<ControlObject> m_pCOLoopScale;
    std::unique_ptr<ControlPushButton> m_pLoopHalveButton;
    std::unique_ptr<ControlPushButton> m_pLoopDoubleButton;
    std::unique_ptr<ControlPushButton> m_pLoopRemoveButton;
    ControlObject* m_pSlipEnabled;
    RateControl* m_pRateControl;
    ControlObject* m_pPlayButton;
    ControlObject* m_pRepeatButton;

    bool m_bLoopingEnabled;
    bool m_bLoopRollActive;
    bool m_bLoopWasEnabledBeforeSlipEnable;
    LoopAdjustTarget m_loopAdjustTarget = LoopAdjustTarget::None;
    LoopAdjustTarget m_loopAdjustTargetOld = LoopAdjustTarget::None;
    bool m_bLoopOutPressedWhileLoopDisabled;
    QStack<double> m_activeLoopRolls;
    ControlValueAtomic<LoopInfo> m_loopInfo;
    ControlValueAtomic<LoopInfo> m_prevLoopInfo;
    double m_prevLoopSize;
    Loop m_oldLoop;
    ControlValueAtomic<mixxx::audio::FramePos> m_currentPosition;
    ControlObject* m_pQuantizeEnabled;
    QAtomicPointer<BeatLoopingControl> m_pActiveBeatLoop;

    // Base BeatLoop Control Object.
    std::unique_ptr<ControlObject> m_pCOBeatLoop;
    std::unique_ptr<ControlObject> m_pCOBeatLoopSize;
    // Array of BeatLoopingControls, one for each size.
    std::vector<std::unique_ptr<BeatLoopingControl>> m_beatLoops;

    std::unique_ptr<ControlObject> m_pCOBeatJump;
    std::unique_ptr<ControlObject> m_pCOBeatJumpSize;
    std::unique_ptr<ControlPushButton> m_pCOBeatJumpSizeHalve;
    std::unique_ptr<ControlPushButton> m_pCOBeatJumpSizeDouble;
    std::unique_ptr<ControlPushButton> m_pCOBeatJumpForward;
    std::unique_ptr<ControlPushButton> m_pCOBeatJumpBackward;
    std::vector<std::unique_ptr<BeatJumpControl>> m_beatJumps;

    std::unique_ptr<ControlObject> m_pCOLoopMove;
    std::vector<std::unique_ptr<LoopMoveControl>> m_loopMoves;

    // objects below are written from an engine worker thread
    TrackPointer m_pTrack;
    mixxx::BeatsPointer m_pBeats;
    // Flag that allows to act quantized only if we have true track beats.
    // See quantizeEnabledAndHasTrueTrackBeats()
    bool m_trueTrackBeats;

    friend class LoopingControlTest;
};

// Class for handling loop moves of a set size. This allows easy access from
// skins.
class LoopMoveControl : public QObject {
    Q_OBJECT
  public:
    LoopMoveControl(const QString& group, double size);
    ~LoopMoveControl() override;

  signals:
    void loopMove(double beats);

  public slots:
    void slotMoveForward(double value);
    void slotMoveBackward(double value);

  private:
    double m_dLoopMoveSize;
    std::unique_ptr<ControlPushButton> m_pMoveForward;
    std::unique_ptr<ControlPushButton> m_pMoveBackward;
};

// Class for handling beat jumps of a set size. This allows easy access from
// skins.
class BeatJumpControl : public QObject {
    Q_OBJECT
  public:
    BeatJumpControl(const QString& group, double size);
    ~BeatJumpControl() override;

  signals:
    void beatJump(double beats);

  public slots:
    void slotJumpForward(double pressed);
    void slotJumpBackward(double pressed);

  private:
    double m_dBeatJumpSize;
    std::unique_ptr<ControlPushButton> m_pJumpForward;
    std::unique_ptr<ControlPushButton> m_pJumpBackward;
};

// Class for handling beat loops of a set size. This allows easy access from
// skins.
class BeatLoopingControl : public QObject {
    Q_OBJECT
  public:
    BeatLoopingControl(const QString& group, double size);

    void activate();
    void deactivate();
    inline double getSize() {
        return m_dBeatLoopSize;
    }
  public slots:
    void slotLegacy(double value);
    void slotActivate(double value, LoopAnchorPoint forcedAnchor);
    void slotActivateRoll(double value, LoopAnchorPoint forcedAnchor);
    void slotToggle(double value, LoopAnchorPoint forcedAnchor);
  private slots:
    void slotReverseActivate(double value);
    void slotReverseActivateRoll(double value);
    void slotReverseToggle(double value);

  signals:
    void activateBeatLoop(BeatLoopingControl*, LoopAnchorPoint forcedAnchor);
    void deactivateBeatLoop(BeatLoopingControl*);
    void activateBeatLoopRoll(BeatLoopingControl*, LoopAnchorPoint forcedAnchor);
    void deactivateBeatLoopRoll(BeatLoopingControl*);

  private:
    double m_dBeatLoopSize;
    bool m_bActive;
    std::unique_ptr<ControlPushButton> m_pLegacy;
    std::unique_ptr<ControlPushButton> m_pActivate;
    std::unique_ptr<ControlPushButton> m_pRActivate;
    std::unique_ptr<ControlPushButton> m_pActivateRoll;
    std::unique_ptr<ControlPushButton> m_pRActivateRoll;
    std::unique_ptr<ControlPushButton> m_pToggle;
    std::unique_ptr<ControlPushButton> m_pRToggle;
    std::unique_ptr<ControlObject> m_pEnabled;
};
