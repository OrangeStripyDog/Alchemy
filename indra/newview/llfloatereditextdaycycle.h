/** 
 * @file llfloatereditextdaycycle.h
 * @brief Floater to create or edit a day cycle
 *
 * $LicenseInfo:firstyear=2011&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2011, Linden Research, Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLFLOATEREDITEXTDAYCYCLE_H
#define LL_LLFLOATEREDITEXTDAYCYCLE_H

#include "llfloater.h"
#include "llsettingsdaycycle.h"
#include <boost/signals2.hpp>

#include "llenvironment.h"

class LLCheckBoxCtrl;
class LLComboBox;
class LLFlyoutComboBtnCtrl;
class LLLineEditor;
class LLMultiSliderCtrl;
class LLTextBox;
class LLTimeCtrl;
class LLTabContainer;

class LLInventoryItem;

typedef std::shared_ptr<LLSettingsBase> LLSettingsBasePtr_t;

/**
 * Floater for creating or editing a day cycle.
 */
class LLFloaterEditExtDayCycle : public LLFloater
{
	LOG_CLASS(LLFloaterEditExtDayCycle);

public:
    // **RIDER**
    static const std::string    KEY_INVENTORY_ID;
    static const std::string    KEY_LIVE_ENVIRONMENT;
    static const std::string    KEY_DAY_LENGTH;
    // **RIDER**

    typedef boost::signals2::signal<void(LLSettingsDay::ptr_t)> edit_commit_signal_t;
    typedef boost::signals2::connection connection_t;

                                LLFloaterEditExtDayCycle(const LLSD &key);
    virtual                     ~LLFloaterEditExtDayCycle();

    virtual BOOL                postBuild() override;
    virtual void                onOpen(const LLSD& key) override;
    virtual void                onClose(bool app_quitting) override;
    virtual void                onFocusReceived() override;
    virtual void                onFocusLost() override;
    virtual void                onVisibilityChange(BOOL new_visibility) override;

    connection_t                setEditCommitSignal(edit_commit_signal_t::slot_type cb);

    virtual void                refresh() override;

private:

	// flyout response/click
	void                        onButtonApply(LLUICtrl *ctrl, const LLSD &data);
	void                        onBtnCancel();
    void                        onButtonImport();
    void                        onButtonLoadFrame();
    void                        onAddTrack();
	void                        onRemoveTrack();
	void                        onCommitName(class LLLineEditor* caller, void* user_data);
	void                        onTrackSelectionCallback(const LLSD& user_data);
	void                        onPlayActionCallback(const LLSD& user_data);
	// time slider moved
	void                        onTimeSliderMoved();
	// a frame moved or frame selection changed
	void                        onFrameSliderCallback(const LLSD &);
    void                        onFrameSliderDoubleClick(S32 x, S32 y, MASK mask);
    void                        onFrameSliderMouseDown(S32 x, S32 y, MASK mask);
    void                        onFrameSliderMouseUp(S32 x, S32 y, MASK mask);

	void                        selectTrack(U32 track_index, bool force = false);
	void                        selectFrame(F32 frame);
	void                        clearTabs();
	void                        updateTabs();
	void                        updateWaterTabs(const LLSettingsWaterPtr_t &p_water);
	void                        updateSkyTabs(const LLSettingsSkyPtr_t &p_sky);
	void                        setWaterTabsEnabled(BOOL enable);
	void                        setSkyTabsEnabled(BOOL enable);
	void                        updateButtons();
	void                        updateSlider(); //track to slider
	void                        updateTimeAndLabel();
	void                        addSliderFrame(const F32 frame, LLSettingsBase::ptr_t &setting, bool update_ui = true);
	void                        removeCurrentSliderFrame();

    void                        loadInventoryItem(const LLUUID  &inventoryId);
    void                        onAssetLoaded(LLUUID asset_id, LLSettingsBase::ptr_t settings, S32 status);
    void                        loadLiveEnvironment(LLEnvironment::EnvSelection_t env);

    void                        doImportFromDisk();
    void                        doApplyCreateNewInventory();
    void                        doApplyUpdateInventory();
    void                        doApplyEnvironment(const std::string &where);
    void                        onInventoryCreated(LLUUID asset_id, LLUUID inventory_id, LLSD results);
    void                        onInventoryUpdated(LLUUID asset_id, LLUUID inventory_id, LLSD results);

    void                        doOpenInventoryFloater(LLSettingsType::type_e type);
    void                        onPickerCommitSetting(LLUUID asset_id);

    bool                        canUseInventory() const;
    bool                        canApplyRegion() const;
    bool                        canApplyParcel() const;

    void                        updateEditEnvironment();
    void                        syncronizeTabs();
    void                        reblendSettings();

    void                        setTabsData(LLTabContainer * tabcontainer, const LLSettingsBase::ptr_t &settings, bool editable);

    // play functions
    void                        startPlay();
    void                        stopPlay();
    static void                 onIdlePlay(void *);

    LLSettingsDay::ptr_t        mEditDay; // edited copy
    LLSettingsDay::Seconds      mDayLength;
    U32                         mCurrentTrack;
    std::string                 mLastFrameSlider;

    LLButton*                   mAddFrameButton;
    LLButton*                   mDeleteFrameButton;
    LLButton*                   mImportButton;
    LLButton*                   mLoadFrame;
    LLMultiSliderCtrl*	        mTimeSlider;
    LLMultiSliderCtrl*          mFramesSlider;
    LLView*                     mSkyTabLayoutContainer;
    LLView*                     mWaterTabLayoutContainer;
    LLTextBox*                  mCurrentTimeLabel;
    LLUUID                      mInventoryId;
    LLInventoryItem *           mInventoryItem;
    LLFlyoutComboBtnCtrl *      mFlyoutControl;

    LLHandle<LLFloater>         mInventoryFloater;

    LLTrackBlenderLoopingManual::ptr_t  mSkyBlender;
    LLTrackBlenderLoopingManual::ptr_t  mWaterBlender;
    LLSettingsSky::ptr_t        mScratchSky;
    LLSettingsWater::ptr_t      mScratchWater;

    LLFrameTimer                mPlayTimer;
    F32                         mPlayStartFrame; // an env frame
    bool                        mIsPlaying;

    edit_commit_signal_t        mCommitSignal;

    // For map of sliders to parameters
    class FrameData
    {
    public:
        FrameData() : mFrame(0) {};
        FrameData(F32 frame, LLSettingsBase::ptr_t settings) : mFrame(frame), pSettings(settings) {};
        F32 mFrame;
        LLSettingsBase::ptr_t pSettings;
    };
    typedef std::map<std::string, FrameData> keymap_t;
    keymap_t mSliderKeyMap; //slider's keys vs old_frames&settings, shadows mFramesSlider
};

#endif // LL_LLFloaterEditExtDayCycle_H
