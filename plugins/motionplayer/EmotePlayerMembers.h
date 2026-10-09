// Script members shared by every D3DEmotePlayer registration: motionplayer's
// ResourceManager-constructed class and DrawDeviceD3D's D3DLayer-constructed
// one. Each registration adds its own constructor, load, clone and progress,
// which differ between the two (storage vs module, frames vs milliseconds).
#pragma once

#include "EmotePlayer.h"

namespace motion {
    // ncbind looks up a raw callback's native instance through the adaptor
    // of its last parameter type. A subclass registered under its own class
    // ID therefore needs a trampoline typed on the registered class.
    template <class Registered,
              tjs_error (*Callback)(tTJSVariant *, tjs_int, tTJSVariant **,
                                    EmotePlayer *)>
    tjs_error emoteRawCallback(tTJSVariant *result, tjs_int numparams,
                               tTJSVariant **param, Registered *self) {
        return Callback(result, numparams, param, self);
    }
} // namespace motion

#define EMOTE_RAW(callback) \
    (&motion::emoteRawCallback<Class, &motion::EmotePlayer::callback>)

#define EMOTE_PLAYER_SHARED_MEMBERS \
    NCB_PROPERTY_RO(module, getModule); \
    NCB_PROPERTY(completionType, getCompletionType, setCompletionType); \
    NCB_PROPERTY(chara, getChara, setChara); \
    NCB_PROPERTY(motion, getMotion, setMotion); \
    NCB_PROPERTY(motionKey, getMotionKey, setMotionKey); \
    NCB_PROPERTY(maskMode, getMaskMode, setMaskMode); \
    NCB_PROPERTY(outline, getOutline, setOutline); \
    NCB_PROPERTY(priorDraw, getPriorDraw, setPriorDraw); \
    NCB_PROPERTY(frameLastTime, getFrameLastTime, setFrameLastTime); \
    NCB_PROPERTY(frameLoopTime, getFrameLoopTime, setFrameLoopTime); \
    NCB_PROPERTY(loopTime, getLoopTime, setLoopTime); \
    NCB_PROPERTY(processedMeshVerticesNum, getProcessedMeshVerticesNum, \
                 setProcessedMeshVerticesNum); \
    NCB_PROPERTY(visible, getVisible, setVisible); \
    NCB_PROPERTY(smoothing, getSmoothing, setSmoothing); \
    NCB_PROPERTY(meshDivisionRatio, getMeshDivisionRatio, setMeshDivisionRatio); \
    NCB_PROPERTY(queing, getQueuing, setQueuing); \
    NCB_PROPERTY(hairScale, getHairScale, setHairScale); \
    NCB_PROPERTY(partsScale, getPartsScale, setPartsScale); \
    NCB_PROPERTY(bustScale, getBustScale, setBustScale); \
    NCB_PROPERTY(bodyScale, getBodyScale, setBodyScale); \
    NCB_PROPERTY(useD3D, getUseD3D, setUseD3D); \
    NCB_PROPERTY(modified, getModified, setModified); \
    NCB_PROPERTY(drawvisible, getDrawVisible, setDrawVisible); \
    NCB_PROPERTY(drawOpacity, getDrawOpacity, setDrawOpacity); \
    NCB_PROPERTY(opengl, getOpengl, setOpengl); \
    NCB_PROPERTY_RO(animating, getAnimating); \
    NCB_PROPERTY_RO(playCallback, getPlayCallback); \
    NCB_PROPERTY_RO(playing, getPlaying); \
    NCB_PROPERTY_RO(allplaying, getAllplaying); \
    NCB_PROPERTY(tickCount, getTickCount, setTickCount); \
    NCB_PROPERTY(speed, getSpeedRatio, setSpeedRatio); \
    NCB_PROPERTY_RO(variableKeys, getVariableKeys); \
    NCB_METHOD(create); \
    NCB_METHOD(serialize); \
    NCB_METHOD(unserialize); \
    NCB_METHOD(show); \
    NCB_METHOD(hide); \
    NCB_METHOD(assignState); \
    NCB_METHOD(initPhysics); \
    NCB_METHOD_RAW_CALLBACK(setRot, EMOTE_RAW(setRotCompat), 0); \
    NCB_METHOD_RAW_CALLBACK(setRotate, EMOTE_RAW(setRotCompat), 0); \
    NCB_METHOD(getRot); \
    NCB_METHOD_RAW_CALLBACK(setCoord, EMOTE_RAW(setCoordCompat), 0); \
    NCB_METHOD_RAW_CALLBACK(setScale, EMOTE_RAW(setScaleCompat), 0); \
    NCB_METHOD(getScale); \
    NCB_METHOD(setMirror); \
    NCB_METHOD_RAW_CALLBACK(setColor, EMOTE_RAW(setColorCompat), 0); \
    NCB_METHOD(getColor); \
    NCB_METHOD(countVariables); \
    NCB_METHOD(getVariableLabelAt); \
    NCB_METHOD(countVariableFrameAt); \
    NCB_METHOD(getVariableFrameLabelAt); \
    NCB_METHOD(getVariableFrameValueAt); \
    NCB_METHOD_RAW_CALLBACK(setVariable, EMOTE_RAW(setVariableCompat), 0); \
    NCB_METHOD(getVariable); \
    NCB_METHOD(getVariableFrameList); \
    NCB_METHOD_RAW_CALLBACK(startWind, EMOTE_RAW(startWindCompat), 0); \
    NCB_METHOD_RAW_CALLBACK(stopWind, EMOTE_RAW(stopWindCompat), 0); \
    NCB_METHOD(countMainTimelines); \
    NCB_METHOD(getMainTimelineLabelAt); \
    NCB_METHOD(countDiffTimelines); \
    NCB_METHOD(getDiffTimelineLabelAt); \
    NCB_METHOD(getDiffTimelineLabelList); \
    NCB_METHOD(countPlayingTimelines); \
    NCB_METHOD(getPlayingTimelineLabelAt); \
    NCB_METHOD(getPlayingTimelineFlagsAt); \
    NCB_METHOD(isLoopTimeline); \
    NCB_METHOD(getLoopTimeline); \
    NCB_METHOD(getTimelineTotalFrameCount); \
    NCB_METHOD(play); \
    NCB_METHOD(playTimeline); \
    NCB_METHOD(isTimelinePlaying); \
    NCB_METHOD(getTimelinePlaying); \
    NCB_METHOD(stopTimeline); \
    NCB_METHOD(setTimeline); \
    NCB_METHOD_RAW_CALLBACK(setTimelineBlendRatio, \
                            EMOTE_RAW(setTimelineBlendRatioCompat), 0); \
    NCB_METHOD(getTimelineBlendRatio); \
    NCB_METHOD(fadeInTimeline); \
    NCB_METHOD(fadeOutTimeline); \
    NCB_METHOD(getPlayingTimelineInfoList); \
    NCB_METHOD(skip); \
    NCB_METHOD(skipToSync); \
    NCB_METHOD(addPlayCallback); \
    NCB_METHOD(pass); \
    NCB_METHOD_DETAIL(draw, Class, void, Class::draw, (tTJSVariant)); \
    NCB_METHOD_RAW_CALLBACK(setDrawAffineTranslateMatrix, \
                            EMOTE_RAW(setDrawAffineTranslateMatrixCompat), \
                            0); \
    NCB_METHOD_RAW_CALLBACK(setOuterForce, EMOTE_RAW(setOuterForceCompat), \
                            0); \
    NCB_METHOD(getOuterForce); \
    NCB_METHOD_RAW_CALLBACK(contains, EMOTE_RAW(containsCompat), 0); \
    NCB_METHOD(clear); \
    NCB_METHOD(assign); \
    NCB_METHOD(setCameraOffset);
