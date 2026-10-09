#include "emotefile.h"

#include "UtilStreams.h"
#include "tjsArray.h"
#include "tjsDictionary.h"

#include "xp3filter.h"
#include "psbfile/PSBValue.h"

#include <spdlog/spdlog.h>
#include <opencv2/opencv.hpp>

#include <sstream>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <cmath>
#include <cassert>

#ifdef _MSC_VER
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

#define GLM_ASSERT_VALID(matrix) \
    do \
    { \
        const glm::mat4& m = (matrix); \
        for (int i = 0; i < 4; ++i) \
        { \
            for (int j = 0; j < 4; ++j) \
            { \
                assert(!std::isnan(m[i][j]) && "矩阵包含NaN值"); \
                assert(!std::isinf(m[i][j]) && "矩阵包含无穷大值"); \
            } \
        } \
    } while (0)

using namespace PSB;

#define EMOTE_LOG(...) spdlog::get("plugin")->warn(__VA_ARGS__)

namespace emoteplayer
{
    emotefile::emotefile() = default;

    emotefile::~emotefile()
    {
        ClearAniTree();
    }

    void emotefile::setSeed(tjs_int seed)
    {
        _psbFile.setSeed(seed);
    }

    void emotefile::setFun(tTJSVariantClosure decryptClo)
    {
        _psbFile.setDecryptCallback(decryptClo);
    }

    bool emotefile::load(const ttstr& filePath)
    {
        if(!_psbFile.loadPSBFile(filePath)) {
            return false;
        }
        return GenerateAniTree();
    }

    tTJSVariant emotefile::root()
    {
        return readAllObjs("root", _psbFile.getPSBHeader().offsetEntries);
    }

    tTJSVariant emotefile::readAllObjs(const ttstr& key, tjs_uint32 objOffset)
    {
        return _psbFile.readAllObjs(key, objOffset);
    }

    uint32_t emotefile::readListInfo(std::vector<uint32_t>* target)
    {
        return _psbFile.readListInfo(target);
    }

    void emotefile::refreshListInfo(std::vector<uint32_t>* target1,
                                    std::vector<uint32_t>* target2)
    {
        _psbFile.refreshListInfo(target1, target2);
    }

    bool emotefile::parseObject(std::map<std::string, uint32_t>& output,
                                uint32_t objOffset)
    {
        return _psbFile.parseObject(output, objOffset);
    }

    bool emotefile::parseNumber(int64_t& output, uint32_t objOffset)
    {
        return _psbFile.parseNumber(output, objOffset);
    }

    bool emotefile::parseReal(double& output, uint32_t objOffset)
    {
        return _psbFile.parseReal(output, objOffset);
    }

    bool emotefile::parseString(std::string& output, uint32_t objOffset)
    {
        return _psbFile.parseString(output, objOffset);
    }

    bool emotefile::parseList(std::vector<uint32_t>& output, uint32_t objOffset)
    {
        return _psbFile.parseList(output, objOffset);
    }

    bool emotefile::parseResource(int32_t& id, uint32_t objOffset)
    {
        return _psbFile.parseResource(id, objOffset);
    }

    uint64_t emotefile::GetSize()
    {
        return _psbFile.getDataSize();
    }

    void emotefile::ReadAllData(uint8_t* output, uint32_t outputlen)
    {
        _psbFile.readAllData(output, outputlen);
    }

    bool emotefile::GenerateAniTree()
    {
        const auto& header = _psbFile.getPSBHeader();
        // baseObj
        std::map<std::string, uint32_t> _rootData;
        parseObject(_rootData, header.offsetEntries);
        // isemote
        std::string typeName;
        parseString(typeName, _rootData["spec"]);
        if (strcmp(typeName.c_str(), "krkr") == 0) // emote
        {
            isKrkr = true;
        }
        else if (strcmp(typeName.c_str(), "win") == 0) // motion
        {
            isKrkr = false;
        }
        else if (strcmp(typeName.c_str(), "common") == 0)
        {
            isKrkr = false;
            colorType = 1;
        }
        else
        {
            EMOTE_LOG("unknown emoteType!!!");
        }
        // metadata
        _metadata = new emotemetadata(this, _rootData["metadata"]);
        // objs
        std::map<std::string, uint32_t> _tmpMap;
        parseObject(_tmpMap, _rootData["object"]);
        for (auto _obj : _tmpMap)
        {
            emoteobject* tmp = new emoteobject(this, _obj.second);
            _objects.insert(std::pair<std::string, emoteobject*>(_obj.first, tmp));
        }
        // screen
        parseObject(_tmpMap, _rootData["screenSize"]);
        int64_t tmp;
        parseNumber(tmp, _tmpMap["originX"]);
        _screenSize.originX = static_cast<int32_t>(tmp);
        parseNumber(tmp, _tmpMap["originY"]);
        _screenSize.originY = static_cast<int32_t>(tmp);
        parseNumber(tmp, _tmpMap["width"]);
        _screenSize.width = static_cast<int32_t>(tmp);
        parseNumber(tmp, _tmpMap["height"]);
        _screenSize.height = static_cast<int32_t>(tmp);
        // source
        parseObject(_tmpMap, _rootData["source"]);
        for (auto _obj : _tmpMap)
        {
            emotesource* tmp = new emotesource(this, _obj.second);
            _source.insert(std::pair<std::string, emotesource*>(_obj.first, tmp));
        }
        // stereovisionProfile
        parseObject(_tmpMap, _rootData["stereovisionProfile"]);
        parseReal(_stereovisionProfile.dist_e2d, _tmpMap["dist_e2d"]);
        parseReal(_stereovisionProfile.dist_eye, _tmpMap["dist_eye"]);
        parseReal(_stereovisionProfile.eye_angle_ltd, _tmpMap["eye_angle_ltd"]);
        parseReal(_stereovisionProfile.f_level, _tmpMap["f_level"]);
        parseReal(_stereovisionProfile.fov, _tmpMap["fov"]);
        parseReal(_stereovisionProfile.len_disp, _tmpMap["len_disp"]);

        // getLastSyncTime
        for (auto _objTmp : _objects)
        {
            for (auto _tmpMtn : _objTmp.second->motion)
            {
                std::vector<emotenode*> _stack(_tmpMtn.second->nodeList.begin(),
                                               _tmpMtn.second->nodeList.end());
                while (!_stack.empty())
                {
                    emotenode* _current = _stack.back();
                    _stack.pop_back();

                    for (auto frameTmp : _current->frameList)
                    {
                        if (frameTmp != nullptr && frameTmp->hasContent)
                        {
                            if (frameTmp->time > _tmpMtn.second->syncTime)
                                _tmpMtn.second->syncTime = frameTmp->time;

                            emotemotion* _emot = findmotionByName(frameTmp->src);
                            if (_emot != nullptr)
                            {
                                for (auto it = _emot->nodeList.begin(); it != _emot->nodeList.end();
                                     ++it)
                                {
                                    _stack.push_back(*it);
                                }
                            }
                        }
                    }
                }
            }
        }
        // mark attrcomp
        for (auto attrItm : _metadata->_attrcomp)
        {
            if (attrItm == nullptr)
                continue;
            for (auto removeDataItm : attrItm->data_remove)
            {
                if (removeDataItm.value > 0)
                    continue;
                auto cha = _objects.find(removeDataItm.chara.c_str());
                if (cha != _objects.end())
                {
                    auto mtn = cha->second->motion.find(removeDataItm.motion.c_str());
                    if (mtn != cha->second->motion.end())
                    {
                        auto lay = mtn->second->getNodeByName(removeDataItm.layer);
                        if (lay != nullptr)
                        {
                            lay->removed = true;
                        }
                    }
                }
            }
        }
        // selectValueSet
        for (auto selcItm : _metadata->_selectorControl)
        {
            selcItm->selectValue(0);
        }

        return true;
    }
    bool emotefile::ClearAniTree()
    {
        if (_metadata != nullptr)
            delete _metadata;
        for (auto obj : _objects)
        {
            if (obj.second != nullptr)
                delete obj.second;
        }
        for (auto src : _source)
        {
            if (src.second != nullptr)
                delete src.second;
        }
        return true;
    }
    void emotefile::updateZMax(float zMax)
    {
        if (abs(zMax) > _zMax)
        {
            _zMax = abs(zMax);
        }
    }
    float emotefile::getZMax(bool isMain)
    {
        float _zMaxTmp = _zMax;
        // 附属
        if (isMain)
        {
            for (auto itm : _attach)
            {
                if (itm->getZMax() > _zMaxTmp)
                    _zMaxTmp = itm->getZMax();
            }
        }
        return _zMaxTmp;
    }
    void emotefile::updateSyncTime(float _syn)
    {
        if (_syn > _syncTime)
        {
            _syncTime = _syn;
        }
    }
    float emotefile::getSyncTime()
    {
        return _syncTime;
    }
    emoteicon* emotefile::findsourceByName(const std::string& name)
    {
        std::istringstream iss(name);
        std::string token;

        // 源头
        std::getline(iss, token, '/');
        if (strcmp(token.c_str(), "src") == 0)
        {
            // 一级路径
            std::getline(iss, token, '/');
            auto tmp = _source.find(token.c_str());
            if (tmp != _source.end())
            {
                emotesource* src = tmp->second;
                // 二级路径
                std::getline(iss, token, '/');
                auto tmp1 = src->icon.find(token.c_str());
                if (tmp1 != src->icon.end())
                {
                    return tmp1->second;
                }
            }
            EMOTE_LOG("src find failed!!!");
        }
        else
        {
            return nullptr;
        }
        return nullptr;
    }
    bool emotefile::readIconTobuffer(uint8_t* buff, uint32_t buffSize, uint32_t pitch, emoteicon* ic)
    {
        const auto& chunkLengths = _psbFile.getChunkLengths().value;
        if (ic->pixel < 0 || ic->pixel >= static_cast<int32_t>(chunkLengths.size())) return false;
        // 生数据读取
        uint8_t* srcbuff = new uint8_t[chunkLengths.at(ic->pixel)];
        if (!_psbFile.readChunkData(ic->pixel, srcbuff, chunkLengths.at(ic->pixel)))
        {
            delete[] srcbuff;
            return false;
        }
        // 解码方案
        if (strcmp(ic->compress.c_str(), "RL") == 0)
        {
            if (ic->pal > -1)
            {
                uint8_t* palbuff = new uint8_t[chunkLengths.at(ic->pal)];
                if (!_psbFile.readChunkData(ic->pal, palbuff, chunkLengths.at(ic->pal)))
                {
                    delete[] srcbuff;
                    delete[] palbuff;
                    return false;
                }

                uint32_t currSize = 0;
                uint8_t* currPtr = srcbuff;
                uint8_t* endPtr = currPtr + chunkLengths.at(ic->pixel);
                uint8_t* currDst = buff;
                while (currPtr < endPtr && currSize < buffSize)
                {
                    int current = *currPtr;
                    currPtr++;
                    int count;
                    if ((current & 0x80) != 0) // Redundant
                    {
                        count = (current ^ 0x80) + 3;
                        uint8_t coloridx = *currPtr;
                        currPtr += 1;
                        for (int i = 0; i < count; i++)
                        {
                            if (currSize + 4 > buffSize)
                                break;
                            memcpy(currDst, palbuff + coloridx, 4);
                            currDst += 4;
                            currSize += 4;
                        }
                    }
                    else // not redundant
                    {
                        count = (current + 1);
                        if (currSize + count * 4 > buffSize)
                            count = (buffSize - currSize) / 4;
                        for (int i = 0; i < count; i++)
                        {
                            memcpy(currDst + i * 4, palbuff + (*(currPtr + i)), 4);
                        }
                        currPtr += count;
                        currDst += count * 4;
                        currSize += count * 4;
                    }
                }
            }
            else
            {
                uint32_t currSize = 0;
                uint8_t* currPtr = srcbuff;
                uint8_t* endPtr = currPtr + chunkLengths.at(ic->pixel);
                uint8_t* currDst = buff;
                while (currPtr < endPtr && currSize < buffSize)
                {
                    int current = *currPtr;
                    currPtr++;
                    int count;
                    if ((current & 0x80) != 0) // Redundant
                    {
                        count = (current ^ 0x80) + 3;
                        uint32_t color = *((uint32_t*)currPtr);
                        currPtr += 4;
                        for (int i = 0; i < count; i++)
                        {
                            if (currSize + 4 > buffSize)
                                break;
                            memcpy(currDst, &color, 4);
                            currDst += 4;
                            currSize += 4;
                        }
                    }
                    else // not redundant
                    {
                        count = (current + 1) * 4;
                        if (currSize + count > buffSize)
                            count = buffSize - currSize;
                        memcpy(currDst, currPtr, count);
                        currPtr += count;
                        currDst += count;
                        currSize += count;
                    }
                }
            }
        }
        else if (strcmp(ic->compress.c_str(), "none") == 0)
        {
            uint32_t cpySize = std::min(chunkLengths.at(ic->pixel), buffSize);
            memcpy(buff, srcbuff, cpySize);
        }
        else
        {
            if (!isKrkr) // 采用裁切贴图
            {
                if (strcmp(ic->type.c_str(), "RGBA8") == 0)
                {
                    uint8_t* start = srcbuff + (int)(ic->top * ic->texWidth + ic->left) * 4;
                    uint32_t cppitch = std::min(
                        (uint32_t)(std::min(ic->left + ic->width, ic->texWidth) - ic->left) * 4,
                        pitch);
                    uint32_t height = std::min(
                        (uint32_t)(std::min(ic->top + ic->height, ic->texHeight) - ic->top),
                        buffSize / pitch);
                    for (uint32_t i = 0; i < height; i++)
                    {
                        memcpy(buff + i * pitch, start + (int)(ic->texWidth * 4 * i), cppitch);
                    }
                }
                else
                {
                    EMOTE_LOG("unknown icon format");
                    delete[] srcbuff;
                    return false;
                }
            }
            else
            {
                EMOTE_LOG("unknown icon format");
                delete[] srcbuff;
                return false;
            }
        }
        delete[] srcbuff;
        return true;
    }
    emotemotion* emotefile::findmotionByName(const std::string& name)
    {
        std::istringstream iss(name);
        std::string token, token1, token2;

        // 源头
        std::getline(iss, token, '/');
        if (strcmp(token.c_str(), "motion") == 0)
        {
            std::getline(iss, token1, '/');
            std::getline(iss, token2, '/');
            // 一级路径
            auto tmp = _objects.find(token1.c_str());
            if (tmp != _objects.end())
            {
                emoteobject* src = tmp->second;
                // 二级路径
                auto tmp1 = src->motion.find(token2.c_str());
                if (tmp1 != src->motion.end())
                {
                    return tmp1->second;
                }
            }
            // 附属
            for (auto itm : _attach)
            {
                // 一级路径
                tmp = itm->_objects.find(token1.c_str());
                if (tmp != itm->_objects.end())
                {
                    emoteobject* src = tmp->second;
                    // 二级路径
                    auto tmp1 = src->motion.find(token2.c_str());
                    if (tmp1 != src->motion.end())
                    {
                        return tmp1->second;
                    }
                }
            }
            EMOTE_LOG("motion find failed!!!");
        }
        else
        {
            return nullptr;
        }
        return nullptr;
    }
    bool emotefile::getTickByName(const std::string& name, float& ret)
    {
        if (_metadata == nullptr) return false;
        auto it = _metadata->_varList.find(name);
        if (it == _metadata->_varList.end()) return false;
        ret = it->second;
        return true;
    }
    void emotefile::updateEyeControl(float tick, bool isMain)
    {
        // 附属
        if (isMain)
        {
            for (auto itm : _attach)
            {
                itm->updateEyeControl(tick);
            }
        }

        std::default_random_engine dre;
        // 眼动控制
        for (auto itm : _metadata->_eyeControl)
        {
            // 初始化tick
            if (!itm->hasStart)
            {
                itm->hasStart = true;
                itm->lastTick = tick;
            }
            // 初始化间隔
            if (itm->currWaitInterval < 0)
            {
                itm->currWaitInterval = itm->uid(dre);
            }
            
            // 播放时 进行参数控制
            if (itm->isBlinking)
            {
                if (tick - itm->lastTick > itm->blinkFrameCount) // 是否结束
                {
                    itm->isBlinking = false;
                    itm->currWaitInterval = -1;
                    itm->lastTick = tick;
                    // 重置初始值
                    auto varPos = _metadata->_varList.find(itm->label);
                    if (varPos != _metadata->_varList.end())
                    {
                        varPos->second = itm->baseVal;
                    }
                }
                else
                {
                    // 计算数值
                    float realVal = itm->beginFrame;
                    if ((tick - itm->lastTick) * 2 < itm->blinkFrameCount ) // 闭眼
                    {
                        realVal += (tick - itm->lastTick) * 2 * (itm->endFrame - itm->beginFrame) / itm->blinkFrameCount;
                    }
                    else // 睁开
                    {
                        realVal += (itm->blinkFrameCount - (tick - itm->lastTick)) * 2 * (itm->endFrame - itm->beginFrame) / itm->blinkFrameCount;
                    }
                    realVal = std::max(realVal, itm->beginFrame);
                    realVal = std::min(realVal, itm->endFrame);
                    // 写入参数
                    auto varPos = _metadata->_varList.find(itm->label);
                    if (varPos != _metadata->_varList.end())
                    {
                        varPos->second = realVal;
                    }
                }
            }
            else // 未播放时进行等待
            {
                if (tick - itm->lastTick > itm->currWaitInterval)
                {
                    itm->isBlinking = true;
                    itm->lastTick = tick;
                    // 获取初始值
                    auto varPos = _metadata->_varList.find(itm->label);
                    if (varPos != _metadata->_varList.end())
                    {
                        itm->baseVal = varPos->second;
                    }
                }
            }
        }
        for (auto itm : _metadata->_eyeControl)
        {
            // nothing to do
        }
    }
    void emotefile::startTimeline(float tick, const std::string& name, bool isMain)
    {
        // 附属
        if (isMain)
        {
            for (auto itm : _attach)
            {
                itm->startTimeline(tick, name);
            }
        }

        for (auto itm : _metadata->_timelineControl)
        {
            if (strcmp(itm->label.c_str(), name.c_str()) == 0)
            {
                currTimeline.push_back(itm);
                currStartTick = tick;
                return;
            }
        }
    }
    void emotefile::stopTimeline(const std::string& name, bool isMain)
    {
        // 附属
        if (isMain)
        {
            for (auto itm : _attach)
            {
                itm->stopTimeline(name);
            }
        }

        for (auto itm : _metadata->_timelineControl)
        {
            if (strcmp(itm->label.c_str(), name.c_str()) == 0)
            {
                currTimeline.erase(std::remove(currTimeline.begin(), currTimeline.end(), itm),
                                   currTimeline.end());
                currStartTick = -1.0f;
                return;
            }
        }
    }
    bool emotefile::checkTimline(const std::string& name, bool& result, bool isMain)
    {
        // 附属
        if (isMain)
        {
            for (auto itm : _attach)
            {
                bool _result = false;
                if (itm->checkTimline(name, _result))
                {
                    result = _result;
                    return true;
                }
            }
        }

        emotetimeline* matchT = nullptr;
        for (auto itm : _metadata->_timelineControl)
        {
            if (strcmp(itm->label.c_str(), name.c_str()) == 0)
            {
                matchT = itm;
                break;
            }
        }
        if (matchT == nullptr)
            return false;
        for (auto itm : currTimeline)
        {
            if (itm == matchT)
            {
                result = true;
                return true;
            }
        }
        result = false;
        return true;
    }
    void emotefile::updateTimelineControl(float tick, bool isMain)
    {
        // 附属
        if (isMain)
        {
            for (auto itm : _attach)
            {
                itm->updateTimelineControl(tick);
            }
        }

        if (currTimeline.size() < 1) return;

        for (auto timelineItm : currTimeline)
        {
            // 考察时间
            if (timelineItm->loopEnd > 0.0f &&
                tick - currStartTick + timelineItm->loopBegin > timelineItm->loopEnd)
            {
                // 重置时间戳
                currStartTick = tick;
            }
            float currRelTime = tick - currStartTick + timelineItm->loopBegin;

            // 遍历每一个变量
            for (auto varItm : timelineItm->variableList)
            {
                // 跳过
                if (varItm->frameList.size() == 0)
                    continue;

                // 对于select变量进行跳过
                bool isfindInSelect = false;
                for (auto sleItm : _metadata->_selectorControl)
                {
                    for (auto _sleItm : sleItm->selectItem)
                    {
                        if (strcmp(_sleItm.label.c_str(), varItm->label.c_str()) == 0)
                        {
                            isfindInSelect = true;
                            break;
                        }
                    }
                    if (isfindInSelect)
                        break;
                }
                if (isfindInSelect)
                    continue;

                // 确定帧位置
                emoteTimeVarFrame* currFrame = nullptr;
                size_t currFrameIdx = -1;
                for (size_t i = 0; i < varItm->frameList.size(); i++)
                {
                    if (varItm->frameList.at(i)->time <= currRelTime)
                    {
                        currFrame = varItm->frameList.at(i);
                        currFrameIdx = i;
                    }
                    else
                        break;
                }
                if (currFrame == nullptr || !currFrame->hasContent)
                    continue;
                emoteTimeVarFrame* nextframe = nullptr;
                if (currFrameIdx >= 0 && currFrameIdx < varItm->frameList.size() - 1)
                    nextframe = varItm->frameList.at(currFrameIdx + 1);
                if (nextframe != nullptr && !nextframe->hasContent)
                    nextframe = nullptr;

                // 插值
                double realVal = 0.0;
                if (nextframe != nullptr &&
                    ((currFrame->type != 2) ||
                     (currFrame->type == 2 &&
                      nextframe->type == 2))) // 存在下一帧则对关键帧进行插值
                {
                    // val
                    realVal = (currFrame->value + (nextframe->value - currFrame->value) /
                                                      (nextframe->time - currFrame->time) *
                                                      (currRelTime - currFrame->time));
                }
                else
                {
                    // 计算坐标
                    realVal = currFrame->value;
                }

                // 赋予
                auto varPos = _metadata->_varList.find(varItm->label);
                if (varPos != _metadata->_varList.end())
                {
                    varPos->second = realVal;
                }
            }
        }
    }
    emoteVar* emotefile::findVarByName(const std::string& name)
    {
        for (auto obj : _objects)
        {
            emoteVar* ret = nullptr;
            ret = obj.second->findVarByName(name);
            if (ret != nullptr)
                return ret;
        }
        return nullptr;
    }
    static bool startswith(const std::string& str, const std::string& prefix)
    {
        return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
    }
    static bool endswith(const std::string& str, const std::string& suffix)
    {
        return str.size() >= suffix.size() &&
               str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    void emotefile::setVariable(const std::string& name, tjs_real value, bool isMain)
    {
        // 附属
        if (isMain)
        {
            for (auto itm : _attach)
            {
                itm->setVariable(name, value);
            }
        }

        // _selectorControl
        for (auto _selItm : _metadata->_selectorControl)
        {
            if (strcmp(_selItm->lable.c_str(), name.c_str()) == 0)
            {
                _selItm->selectValue(value);
                return;
            }
        }
        // _varList
        auto varPos = _metadata->_varList.find(name);
        if (varPos != _metadata->_varList.end())
        {
            varPos->second = value;
        }
    }
    void emotefile::updatePhysics(float tick)
    {
        for (auto itm : _metadata->_bustControl)
        {
            // 寻找各变量
            auto varLR = findVarByName(itm->var_lr);
            if (varLR == nullptr)
                continue;
            auto varUD = findVarByName(itm->var_ud);
            if (varUD == nullptr)
                continue;

            // 获取摆动值
            float sway_value = breast_sim.generate(tick, 0, -30.f, 30.f);
            varLR->currVal = sway_value;
            sway_value = breast_sim.generate(tick, 0, -30.f, 30.f);
            varUD->currVal = sway_value;
        }
        // for (auto itm : _metadata->_clampControl)
        //{
        //     auto varLR = _metadata->_varList.find(itm.var_lr);
        //     if (varLR == _metadata->_varList.end()) continue;
        //     auto varUD = _metadata->_varList.find(itm.var_ud);
        //     if (varUD == _metadata->_varList.end()) continue;
        //     float clamp_sway = oscillator.generate(tick, itm.min, itm.max);
        //     varLR->second = clamp_sway;
        //     varUD->second = clamp_sway;
        // }
        //  刮风
        hair_sim.startWind(glm::vec2(0.0f, 0.0f), glm::vec2(1280.0f, 0.0f), 0.15, 0.2, 0.3);
        for (auto itm : _metadata->_hairControl)
        {
            // 寻找各变量
            auto varLR = findVarByName(itm->var_lr);
            if (varLR == nullptr)
                continue;
            auto varLRM = findVarByName(itm->var_lrm);
            if (varLRM == nullptr)
                continue;
            auto varUD = findVarByName(itm->var_ud);
            if (varUD == nullptr)
                continue;

            // 获取飘动值
            float hair_sway = hair_sim.generate(tick, glm::vec2(640.0f, 0.0f), -30.f, 30.f);
            varLR->currVal = hair_sway;
            hair_sway = hair_sim.generate(tick, glm::vec2(640.0f, 0.0f), -30.f, 30.f);
            varLRM->currVal = hair_sway;
            hair_sway = hair_sim.generate(tick, glm::vec2(640.0f, 0.0f), -30.f, 30.f);
            varUD->currVal = hair_sway;
        }
        for (auto itm : _metadata->_partsControl)
        {
            // 寻找各变量
            auto varLR = findVarByName(itm->var_lr);
            if (varLR == nullptr)
                continue;
            auto varLRM = findVarByName(itm->var_lrm);
            if (varLRM == nullptr)
                continue;
            auto varUD = findVarByName(itm->var_ud);
            if (varUD == nullptr)
                continue;

            // 获取飘动值
            float hair_sway = hair_sim.generate(tick, glm::vec2(640.0f, 0.0f), -30.f, 30.f);
            varLR->currVal = hair_sway;
            hair_sway = hair_sim.generate(tick, glm::vec2(640.0f, 0.0f), -30.f, 30.f);
            varLRM->currVal = hair_sway;
            hair_sway = hair_sim.generate(tick, glm::vec2(640.0f, 0.0f), -30.f, 30.f);
            varUD->currVal = hair_sway;
        }
    }
    void emotefile::addEmoteFile(emotefile* itm)
    {
        _attach.push_back(itm);
    }
} // namespace emoteplayer
