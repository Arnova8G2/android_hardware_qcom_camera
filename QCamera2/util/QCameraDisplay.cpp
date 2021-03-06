/* Copyright (c) 2015-2017, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/

#define LOG_TAG "QCameraDisplay"

// Camera dependencies
#include <properties.h>
extern "C" {
#include "mm_camera_dbg.h"
}
#include "QCameraDisplay.h"

#define CAMERA_VSYNC_WAIT_MS               33 // Used by vsync thread to wait for vsync timeout.
#define DISPLAY_EVENT_RECEIVER_ARRAY_SIZE  1
#define DISPLAY_DEFAULT_FPS                60

#ifdef USE_DISPLAY_SERVICE
using ::android::frameworks::displayservice::V1_0::IDisplayEventReceiver;
using ::android::frameworks::displayservice::V1_0::IDisplayService;
using ::android::frameworks::displayservice::V1_0::IEventCallback;
using ::android::frameworks::displayservice::V1_0::Status;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::sp;
#else //USE_DISPLAY_SERVICE
#include <utils/Errors.h>
#include <utils/Looper.h>
using ::android::status_t;
using ::android::NO_ERROR;
using ::android::Looper;
#define ALOOPER_EVENT_INPUT android::Looper::EVENT_INPUT
#endif //USE_DISPLAY_SERVICE

namespace qcamera {

#ifndef USE_DISPLAY_SERVICE
/*===========================================================================
 * FUNCTION   : vsyncEventReceiverCamera
 *
 * DESCRIPTION: Computes average vsync interval. Called by display
 *              event handler for every vsync event.
 *
 * PARAMETERS :
 *   @fd      : file descriptor
 *   @events  : events
 *   @data    : pointer to user data provided during call back registration.
 *
 * RETURN     : always returns 1
 *==========================================================================*/
int QCameraDisplay::vsyncEventReceiverCamera(__unused int fd,
                                             __unused int events, void* data) {
    android::DisplayEventReceiver::Event buffer[DISPLAY_EVENT_RECEIVER_ARRAY_SIZE];
    QCameraDisplay* pQCameraDisplay = (QCameraDisplay *) data;
    ssize_t n;

    while ((n = pQCameraDisplay->mDisplayEventReceiver.getEvents(buffer,
            DISPLAY_EVENT_RECEIVER_ARRAY_SIZE)) > 0) {
        for (int i = 0 ; i < n ; i++) {
            if (buffer[i].header.type == android::DisplayEventReceiver::DISPLAY_EVENT_VSYNC) {
                pQCameraDisplay->computeAverageVsyncInterval(buffer[i].header.timestamp);
            }
        }
    }
    return 1;
}

/*===========================================================================
 * FUNCTION   : vsyncThreadCamera
 *
 * DESCRIPTION: Thread registers a call back function for every vsync event
 *              waits on the looper for the next vsync.
 *
 * PARAMETERS :
 *   @data    : receives vsync_info_t structure.
 *
 * RETURN     : NULL.Just to fullfill the type requirement of thread function.
 *==========================================================================*/
void* QCameraDisplay::vsyncThreadCamera(void * data)
{
    QCameraDisplay* pQCameraDisplay = (QCameraDisplay *) data;
    android::sp<Looper> looper;

    looper = new android::Looper(false);
    status_t status = pQCameraDisplay->mDisplayEventReceiver.initCheck();
    if (status != NO_ERROR) {
        LOGE("Initialization of DisplayEventReceiver failed with status: %d", status);
        return NULL;
    }
    looper->addFd(pQCameraDisplay->mDisplayEventReceiver.getFd(), 0, ALOOPER_EVENT_INPUT,
            QCameraDisplay::vsyncEventReceiverCamera, pQCameraDisplay);
    pQCameraDisplay->mDisplayEventReceiver.setVsyncRate(1);
    while(pQCameraDisplay->mThreadExit == 0)
    {
        looper->pollOnce(CAMERA_VSYNC_WAIT_MS);
    }
    return NULL;
}
#else //USER_DISPLAY_SERVICE
//initializing static
QCameraDisplay*
QCameraDisplay::mCameraDisplay = NULL;

/*===========================================================================
 * FUNCTION   : instance
 *
 * DESCRIPTION: create singleton instance of QCameraDisplay
 *
 * PARAMETERS : none
 *
 * RETURN     : pointer to QCameraDisplay
 *==========================================================================*/
QCameraDisplay*
QCameraDisplay::instance()
{
    if(mCameraDisplay == NULL)
    {
        mCameraDisplay = new QCameraDisplay();
    }
    if(!mCameraDisplay->m_bInitDone)
    {
        mCameraDisplay->init();
    }
    return mCameraDisplay;
}

/*===========================================================================
 * FUNCTION   : serviceDied
 *
 * DESCRIPTION: Display service death recipient. Clear all variables.
 *
 * PARAMETERS : uint64_t & IBase refrence of dead serivce (unused).
 *
 * RETURN     : none
 *==========================================================================*/
void
QCameraDisplay::DeathRecipient::serviceDied(uint64_t /*cookie*/,
                                const wp<android::hidl::base::V1_0::IBase>& /*who*/)
{
    if(mCameraDisplay && mCameraDisplay->m_bInitDone)
    {
        mCameraDisplay->m_bInitDone = false;
        mCameraDisplay->m_bSyncing = false;
        mCameraDisplay->mDisplayEventReceiver.clear();
        mCameraDisplay->mDisplayService.clear();
        mCameraDisplay->mDisplayEventCallback.clear();
        mCameraDisplay->mDeathRecipient.clear();
        mCameraDisplay->mDeathRecipient = nullptr;
        mCameraDisplay->mDisplayEventCallback = nullptr;
        mCameraDisplay->mDisplayEventReceiver = nullptr;
        mCameraDisplay->mDisplayService = nullptr;
    }
}
#endif //USE_DISPLAY_SERVICE

/*===========================================================================
 * FUNCTION   : QCameraDisplay
 *
 * DESCRIPTION: constructor of QCameraDisplay
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
QCameraDisplay::QCameraDisplay()
    : mVsyncTimeStamp(0),
      mAvgVsyncInterval(0),
      mOldTimeStamp(0),
      mVsyncHistoryIndex(0),
      mAdditionalVsyncOffsetForWiggle(0),
      mNum_vsync_from_vfe_isr_to_presentation_timestamp(0),
      mSet_timestamp_num_ns_prior_to_vsync(0),
      mVfe_and_mdp_freq_wiggle_filter_max_ns(0),
      mVfe_and_mdp_freq_wiggle_filter_min_ns(0),
#ifndef USE_DISPLAY_SERVICE
      mThreadExit(0)
#else //USE_DISPLAY_SERVICE
      m_bInitDone(false),
      m_bSyncing(false)
#endif //USE_DISPLAY_SERVICE
{
#ifdef USE_DISPLAY_SERVICE
    mDisplayService = nullptr;
    mDisplayEventReceiver = nullptr;
    mDeathRecipient = nullptr;
    mDisplayEventCallback = nullptr;
#else //USE_DISPLAY_SERVICE
    int rc = NO_ERROR;

    memset(&mVsyncIntervalHistory, 0, sizeof(mVsyncIntervalHistory));
    rc = pthread_create(&mVsyncThreadCameraHandle, NULL, vsyncThreadCamera, (void *)this);
    if (rc == NO_ERROR) {
        pthread_setname_np(mVsyncThreadCameraHandle, "CAM_Vsync");
#endif //USE_DISPLAY_SERVICE
        char  value[PROPERTY_VALUE_MAX];
        nsecs_t default_vsync_interval;
        // Read a list of properties used for tuning
        property_get("persist.camera.disp.num_vsync", value, "4");
        mNum_vsync_from_vfe_isr_to_presentation_timestamp = atoi(value);
        property_get("persist.camera.disp.ms_to_vsync", value, "2");
        mSet_timestamp_num_ns_prior_to_vsync = atoi(value) * NSEC_PER_MSEC;
        property_get("persist.camera.disp.filter_max", value, "2");
        mVfe_and_mdp_freq_wiggle_filter_max_ns = atoi(value) * NSEC_PER_MSEC;
        property_get("persist.camera.disp.filter_min", value, "4");
        mVfe_and_mdp_freq_wiggle_filter_min_ns = atoi(value) * NSEC_PER_MSEC;
        property_get("persist.camera.disp.fps", value, "60");
        if (atoi(value) > 0) {
            default_vsync_interval= s2ns(1) / atoi(value);
        } else {
            default_vsync_interval= s2ns(1) / DISPLAY_DEFAULT_FPS;
        }
        for (int i=0; i < CAMERA_NUM_VSYNC_INTERVAL_HISTORY; i++) {
            mVsyncIntervalHistory[i] = default_vsync_interval;
        }
        LOGD("display jitter num_vsync_from_vfe_isr_to_presentation_timestamp %u \
                set_timestamp_num_ns_prior_to_vsync %llu",
                mNum_vsync_from_vfe_isr_to_presentation_timestamp,
                mSet_timestamp_num_ns_prior_to_vsync);
        LOGD("display jitter vfe_and_mdp_freq_wiggle_filter_max_ns %llu \
                vfe_and_mdp_freq_wiggle_filter_min_ns %llu",
                mVfe_and_mdp_freq_wiggle_filter_max_ns,
                mVfe_and_mdp_freq_wiggle_filter_min_ns);
#ifndef USE_DISPLAY_SERVICE
      } else {
          mVsyncThreadCameraHandle = 0;
      }
#else
    init();
#endif //USE_DISPLAY_SERVICE
}

/*===========================================================================
 * FUNCTION   : ~QCameraDisplay
 *
 * DESCRIPTION: destructor of QCameraDisplay
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
QCameraDisplay::~QCameraDisplay()
{
#ifdef USE_DISPLAY_SERVICE
    if(mDisplayEventReceiver != nullptr)
    {
        mDisplayEventReceiver->close();
    }
    mDisplayEventCallback.clear();
    mDeathRecipient.clear();
    mDisplayEventReceiver.clear();
    mDisplayService.clear();
#else
    mThreadExit = 1;
    if (mVsyncThreadCameraHandle != 0) {
        pthread_join(mVsyncThreadCameraHandle, NULL);
    }
#endif //USE_DISPLAY_SERVICE
}

#ifdef USE_DISPLAY_SERVICE
/*===========================================================================
 * FUNCTION   : init
 *
 * DESCRIPTION: Get the display service and register for the callback. OnVsync
 *              and onHotPlug callback will we called based on setVsyncRate
 *              parameter. Check isInitDone to see if init is success or not.
 *
 * PARAMETERS : none
 *
 * RETURN     : none.
 *==========================================================================*/
void
QCameraDisplay::init()
{
    //get the display service and register for Event receiver.
    mDisplayService = android::frameworks::displayservice::V1_0::IDisplayService::getService();
    if(mDisplayService == nullptr)
    {
        LOGE("Camera failed to get Displayservice for vsync.");
        return;
    }

    Return<sp<IDisplayEventReceiver>> ret = mDisplayService->getEventReceiver();
    mDisplayEventReceiver = ret;
    if(!ret.isOk() || (mDisplayEventReceiver == nullptr))
    {
      LOGE("Failed to get display event receiver");
      return;
    }

    if(mDisplayEventCallback == nullptr)
    {
        mDisplayEventCallback = new DisplayEventCallback();
    }

    /*setting callbacks*/
    Return<Status> retVal = mDisplayEventReceiver->init(mDisplayEventCallback);
    if(!retVal.isOk() || (Status::SUCCESS != static_cast<Status>(retVal)) )
    {
        LOGE("Failed to register display vsync callback");
        return;
    }

    if(mDeathRecipient == nullptr)
    {
        mDeathRecipient = new DeathRecipient();
    }

    mDisplayService->linkToDeath(mDeathRecipient,0);
    m_bInitDone = true;

}

/*===========================================================================
 * FUNCTION   : startVsync
 *
 * DESCRIPTION: Start or stop the onVsync or onHotPlug callback.
 *
 * PARAMETERS : true to start callback or false to stop callback
 *
 * RETURN     : true in success, false in error case.
 *==========================================================================*/
bool
QCameraDisplay::startVsync(bool bStart)
{
    if(!m_bInitDone || mDisplayEventReceiver == nullptr)
    {
        LOGE("ERROR: Display event callbacks is not registered");
        return false;
    }

    if(bStart)
    {
        /*send callback for each vsync event*/
        Return<Status> retVal = mDisplayEventReceiver->setVsyncRate(1);
        if(!retVal.isOk() || (Status::SUCCESS != static_cast<Status>(retVal)) )
        {
            LOGE("Failed to start vsync events");
            return false;
        }
    }
    else
    {
        /*disabling sending callback for vsync event*/
        Return<Status> retVal = mDisplayEventReceiver->setVsyncRate(0);
        if(!retVal.isOk() || (Status::SUCCESS != static_cast<Status>(retVal)) )
        {
            LOGE("Failed to stop vsync events");
            return false;
        }
    }
    LOGI("Display sync event %s", (bStart)?"started":"stopped");

    m_bSyncing = (bStart)?true:false;
    return true; //sync rate is set
}
#endif //USE_DISPLAY_SERVICE

/*===========================================================================
 * FUNCTION   : computeAverageVsyncInterval
 *
 * DESCRIPTION: Computes average vsync interval using current and previously
 *              stored vsync data.
 *
 * PARAMETERS : current vsync time stamp
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraDisplay::computeAverageVsyncInterval(nsecs_t currentVsyncTimeStamp)
{
    nsecs_t sum;
    nsecs_t vsyncMaxOutlier;
    nsecs_t vsyncMinOutlier;

    mVsyncTimeStamp = currentVsyncTimeStamp;
    if (mOldTimeStamp) {
        // Compute average vsync interval using current and previously stored vsync data.
        // Leave the max and min vsync interval from history in computing the average.
        mVsyncIntervalHistory[mVsyncHistoryIndex] = currentVsyncTimeStamp - mOldTimeStamp;
        mVsyncHistoryIndex++;
        mVsyncHistoryIndex = mVsyncHistoryIndex % CAMERA_NUM_VSYNC_INTERVAL_HISTORY;
        sum = mVsyncIntervalHistory[0];
        vsyncMaxOutlier = mVsyncIntervalHistory[0];
        vsyncMinOutlier = mVsyncIntervalHistory[0];
        for (int j=1; j<CAMERA_NUM_VSYNC_INTERVAL_HISTORY; j++) {
            sum += mVsyncIntervalHistory[j];
            if (vsyncMaxOutlier < mVsyncIntervalHistory[j]) {
                vsyncMaxOutlier = mVsyncIntervalHistory[j];
            } else if (vsyncMinOutlier > mVsyncIntervalHistory[j]) {
                vsyncMinOutlier = mVsyncIntervalHistory[j];
            }
        }
        sum = sum - vsyncMaxOutlier - vsyncMinOutlier;
        mAvgVsyncInterval = sum / (CAMERA_NUM_VSYNC_INTERVAL_HISTORY - 2);
    }
    mOldTimeStamp = currentVsyncTimeStamp;
}

/*===========================================================================
 * FUNCTION   : computePresentationTimeStamp
 *
 * DESCRIPTION: Computes presentation time stamp using vsync interval
 *              and last vsync time stamp and few other tunable variables
 *              to place the time stamp at the expected future vsync
 *
 * PARAMETERS : current frame time stamp set by VFE when buffer copy done.
 *
 * RETURN     : time stamp in future or 0 in case of failure.
 *==========================================================================*/
nsecs_t QCameraDisplay::computePresentationTimeStamp(nsecs_t frameTimeStamp)
{
    nsecs_t moveToNextVsync;
    nsecs_t keepInCurrentVsync;
    nsecs_t timeDifference        = 0;
    nsecs_t presentationTimeStamp = 0;
    int     expectedVsyncOffset   = 0;
    int     vsyncOffset;
#ifdef USE_DISPLAY_SERVICE
    if(!isSyncing())
    {
       return 0;
    }
#endif //USE_DISPLAY_SERVICE
    if ( (mAvgVsyncInterval != 0) && (mVsyncTimeStamp != 0) ) {
        // Compute presentation time stamp in future as per the following formula
        // future time stamp = vfe time stamp +  N *  average vsync interval
        // Adjust the time stamp so that it is placed few milliseconds before
        // the expected vsync.
        // Adjust the time stamp for the period where vsync time stamp and VFE
        // timstamp cross over due difference in fps.
        presentationTimeStamp = frameTimeStamp +
                (mNum_vsync_from_vfe_isr_to_presentation_timestamp * mAvgVsyncInterval);
        if (presentationTimeStamp > mVsyncTimeStamp) {
            timeDifference      = presentationTimeStamp - mVsyncTimeStamp;
            moveToNextVsync     = mAvgVsyncInterval - mVfe_and_mdp_freq_wiggle_filter_min_ns;
            keepInCurrentVsync  = mAvgVsyncInterval - mVfe_and_mdp_freq_wiggle_filter_max_ns;
            vsyncOffset         = timeDifference % mAvgVsyncInterval;
            expectedVsyncOffset = mAvgVsyncInterval -
                    mSet_timestamp_num_ns_prior_to_vsync - vsyncOffset;
            if (vsyncOffset > moveToNextVsync) {
                mAdditionalVsyncOffsetForWiggle = mAvgVsyncInterval;
            } else if (vsyncOffset < keepInCurrentVsync) {
                mAdditionalVsyncOffsetForWiggle = 0;
            }
            LOGD("vsyncTimeStamp: %llu presentationTimeStamp: %llu expectedVsyncOffset: %d \
                    timeDifference: %llu vsyncffset: %d avgvsync: %llu \
                    additionalvsyncOffsetForWiggle: %llu",
                    mVsyncTimeStamp, presentationTimeStamp, expectedVsyncOffset,
                    timeDifference, vsyncOffset, mAvgVsyncInterval,
                    mAdditionalVsyncOffsetForWiggle);
        }
        presentationTimeStamp = presentationTimeStamp + expectedVsyncOffset +
                mAdditionalVsyncOffsetForWiggle;
    }
    return presentationTimeStamp;
}

}; // namespace qcamera
