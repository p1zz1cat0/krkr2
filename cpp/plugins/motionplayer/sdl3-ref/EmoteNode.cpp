#include "emotefile.h"
#include "EmoteInternal.h"
#include "EmoteGLBridge.h"
#include "EmoteTVPRenderer.h"

#include "LayerIntf.h"
#include "LayerBitmapIntf.h"
#include "RenderManager.h"

#include <SDL2/SDL_log.h>

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

namespace emoteplayer
{
    emotenode::emotenode(emotemotion* rootmotion,
                         emotenode* parent,
                         std::vector<emotenode*>& nodeList,
                         emotefile* filePtr,
                         uint32_t startOffset)
      : _parent(parent),
        _rootmotion(rootmotion),
        _filePtr(filePtr)
    {
        // add to mgn ss
        nodeList.push_back(this);
        // dic
        std::map<std::string, uint32_t> _rootData;
        filePtr->parseObject(_rootData, startOffset);
        // meshInfo
        int64_t tmp = 0;
        auto it = _rootData.find("meshCombine");
        if (it != _rootData.end())
        {
            filePtr->parseNumber(tmp, it->second);
            meshCombine = static_cast<uint8_t>(tmp);
        }
        it = _rootData.find("meshDivision");
        if (it != _rootData.end())
        {
            filePtr->parseNumber(tmp, it->second);
            meshDivision = static_cast<uint8_t>(tmp);
        }
        it = _rootData.find("meshTransform");
        if (it != _rootData.end())
        {
            filePtr->parseNumber(tmp, it->second);
            meshTransform = static_cast<uint8_t>(tmp);
        }
        it = _rootData.find("type");
        if (it != _rootData.end())
            filePtr->parseNumber(tmp, it->second);
        type = static_cast<uint8_t>(tmp);
        // mask
        it = _rootData.find("meshSyncChildMask");
        if (it != _rootData.end())
        {
            filePtr->parseNumber(tmp, it->second);
            meshSyncChildMask = static_cast<uint32_t>(tmp);
        }
        it = _rootData.find("inheritMask");
        if (it != _rootData.end())
        {
            filePtr->parseNumber(tmp, it->second);
            inheritMask = static_cast<uint32_t>(tmp);
        }
        // var
        it = _rootData.find("parameterize");
        if (it != _rootData.end())
        {
            if (filePtr->parseNumber(tmp, it->second))
            {
                isParameterize = true;
                parameterIdx = static_cast<int32_t>(tmp);
            }
        }
        // label
        it = _rootData.find("label");
        if (it != _rootData.end())
            filePtr->parseString(label, it->second);
        // frameList
        std::vector<uint32_t> _tmpList;
        it = _rootData.find("frameList");
        if (it != _rootData.end())
            filePtr->parseList(_tmpList, it->second);
        for (size_t i = 0; i < _tmpList.size(); i++)
        {
            emoteframe* tmp = new emoteframe(filePtr, _tmpList.at(i));
            frameList.push_back(tmp);
        }
        // children
        _tmpList.clear();
        it = _rootData.find("children");
        if (it != _rootData.end())
            filePtr->parseList(_tmpList, it->second);
        for (size_t i = 0; i < _tmpList.size(); i++)
        {
            emotenode* tmp = new emotenode(_rootmotion, this, nodeList, filePtr, _tmpList.at(i));
            children.push_back(tmp);
        }
        // refmask List
        _tmpList.clear();
        it = _rootData.find("stencilCompositeMaskLayerList");
        if (it != _rootData.end())
        {
            filePtr->parseList(_tmpList, it->second);
            for (size_t i = 0; i < _tmpList.size(); i++)
            {
                std::string refName;
                filePtr->parseString(refName, _tmpList.at(i));
                stencilCompositeMaskLayerList.push_back(refName);
            }
        }
    }
    emotenode::~emotenode()
    {
        for (auto frame : frameList)
        {
            if (frame != nullptr)
                delete frame;
        }
        for (auto child : children)
        {
            if (child != nullptr)
                delete child;
        }
        if (isIcon && data != nullptr)
            delete data;
        if (selftexture != 0 && glIsTexture(selftexture) == GL_TRUE)
        {
            glDeleteTextures(1, &selftexture);
        }
        if (_cachedSourceBitmap != nullptr)
        {
            delete _cachedSourceBitmap;
            _cachedSourceBitmap = nullptr;
        }
        _cachedSourceIcon = nullptr;
    }
    iTVPBaseBitmap* emotenode::getSourceBitmap() const
    {
        if (!isIcon || data == nullptr || width <= 0 || height <= 0)
            return nullptr;
        if (_cachedSourceBitmap == nullptr || _cachedSourceWidth != width ||
            _cachedSourceHeight != height)
        {
            if (_cachedSourceBitmap != nullptr)
                delete _cachedSourceBitmap;
            _cachedSourceBitmap = new tTVPBaseTexture(static_cast<tjs_uint>(width),
                                                        static_cast<tjs_uint>(height), 32);
            _cachedSourceWidth = width;
            _cachedSourceHeight = height;
            _cachedSourceIcon = nullptr;
        }
        if (_cachedSourceIcon != ic)
        {
            if (_filePtr->colorType == 0)
            {
                const size_t pxCount = static_cast<size_t>(width) * static_cast<size_t>(height);
                std::vector<uint8_t> rgba(pxCount * 4);
                for (size_t i = 0; i < pxCount; ++i)
                {
                    rgba[4 * i] = data[4 * i + 2];
                    rgba[4 * i + 1] = data[4 * i + 1];
                    rgba[4 * i + 2] = data[4 * i];
                    rgba[4 * i + 3] = data[4 * i + 3];
                }
                _cachedSourceBitmap->Update(rgba.data(), width * 4, 0, 0, width, height);
            }
            else
            {
                _cachedSourceBitmap->Update(data, width * 4, 0, 0, width, height);
            }
            _cachedSourceIcon = ic;
        }
        return _cachedSourceBitmap;
    }
    void emotenode::drawPatchCommon(const emotelimit& lim, iTVPBaseBitmap* sourceBitmap,
                                    tTJSNI_BaseLayer* layer, iTVPBaseBitmap* target,
                                    const tTVPRect& targetClip) const
    {
        const int surfaceCount = static_cast<int>(renderMethod.size());
#if defined(__ANDROID__) || defined(__APPLE__)
        if (surfaceCount > 20)
#else
        if (surfaceCount > 64)
#endif
        {
            SDL_Log("render:%s failed!!!", label.c_str());
            return;
        }

        std::vector<glm::mat4> patchTransforms(static_cast<size_t>(surfaceCount));
        std::vector<const float*> patchControlPoints(static_cast<size_t>(surfaceCount));
        int idxCnt = 0;
        float totalOpa = currOpa;
        bool useBezierMesh = isNeedBp;
        for (int32_t i = surfaceCount - 1; i >= 0; i--)
        {
            totalOpa *= renderMethod.at(i).opa;
            patchTransforms[static_cast<size_t>(idxCnt)] = renderMethod.at(i).matTrans;
            patchControlPoints[static_cast<size_t>(idxCnt)] =
                renderMethod.at(i).type == 1 ? renderMethod.at(i).controlPts : default_control_points;
            if (renderMethod.at(i).type == 1)
                useBezierMesh = true;
            idxCnt++;
        }

        if (layer != nullptr)
        {
            emoteDrawPatchToLayer(layer, lim, idxCnt, glm::value_ptr(patchTransforms[0]),
                                  patchControlPoints.data(), sourceBitmap, width, height, totalOpa,
                                  currbm, meshDivision, useBezierMesh);
        }
        else if (target != nullptr)
        {
            emoteDrawPatchToBitmap(target, targetClip, lim, idxCnt,
                                   glm::value_ptr(patchTransforms[0]), patchControlPoints.data(),
                                   sourceBitmap, width, height, totalOpa, currbm, meshDivision,
                                   useBezierMesh);
        }
    }
    void emotenode::checkDrawStatus(float tick, std::vector<emoteRender>& renderList, emotelimit lim)
    {
        // 不绘制进行节点传递
        if (renderList.size() > 0 && renderList.back().type == 0)
        {
            isNeedDraw = false;
            return;
        }

        // 确定时间轴
        if (frameList.size() == 0)
        {
            isNeedDraw = false;
            return;
        }
        frame = nullptr;
        size_t currFrameIdx = -1;
        for (size_t i = 0; i < frameList.size(); i++)
        {
            if (frameList.at(i)->time <= tick)
            {
                frame = frameList.at(i);
                currFrameIdx = i;
            }
            else
                break;
        }
        if (frame == nullptr || !frame->hasContent)
        {
            isNeedDraw = false;
            return;
        }
        nextframe = nullptr;
        if (currFrameIdx >= 0 && currFrameIdx < frameList.size() - 1)
            nextframe = frameList.at(currFrameIdx + 1);
        if (nextframe != nullptr && !nextframe->hasContent)
            nextframe = nullptr;

        // 节点基础信息获取
        isNeedDraw = true;
        isIcon = false;
        isLayout = false;
        emoteicon* tmpic = _filePtr->findsourceByName(frame->src);
        if (tmpic == nullptr)
            emot = _filePtr->findmotionByName(frame->src);
        else
            emot = nullptr;
        if (tmpic != nullptr)
        {
            isIcon = true;

            if (tmpic != ic) // 直接比对ic
            {
                ic = tmpic;
                width = ic->width;
                height = ic->height;
                resizeMainData();
                resizeOpenGL();
                originX = ic->originX;
                originY = ic->originY;
                _iconGlTextureUploaded = false;
                _cachedSourceIcon = nullptr;
                if (!_filePtr->readIconTobuffer(data, width * height * 4, width * 4, ic))
                {
                    SDL_Log("readIconTobuffer failed: %s", frame->src.c_str());
                    isNeedDraw = false;
                    return;
                }
            }
            // 设置混色
            currbm = frame->bm;
        }
        else if (emot != nullptr || strcmp(frame->src.c_str(), "layout") == 0 ||
                 strcmp(frame->src.c_str(), "clip") == 0)
        {
            isLayout = true;

            // 直接用父类提供的区域
            if (width != lim.width || height != lim.height)
            {
                width = lim.width;
                height = lim.height;
                originX = lim.originX;
                originY = lim.originY;
            }
        }
        else
        {
            std::istringstream iss(frame->src);
            std::string token;
            std::getline(iss, token, '/');
            if (strcmp(token.c_str(), "blank") == 0)
            {
                std::getline(iss, token, ':');
                int32_t w = std::stoi(token);
                std::getline(iss, token, ':');
                int32_t h = std::stoi(token);
                std::getline(iss, token, ':');
                int32_t ox = std::stoi(token);
                std::getline(iss, token, ':');
                int32_t oy = std::stoi(token);

                if (width != w || height != h)
                {
                    width = w;
                    height = h;
                }
                originX = ox;
                originY = oy;
            }
            else if (strcmp(token.c_str(), "shape") == 0)
            {
                // TODO
                isNeedDraw = false;
                return;
            }
            else
            {
                SDL_Log("source unsupported!!!--->%s", frame->src.c_str());
                isNeedDraw = false;
                return;
            }
        }
    }
    void emotenode::progress(float tick, std::vector<emoteRender>& renderList, emotelimit lim)
    {
        // 参数化时可能改变
        currTick = tick;
        // 对于motion，增加终结机制, 即无法越过selfSyncTime
        if (_filePtr->isMotion && currTick > _rootmotion->selfSyncTime)
            currTick = _rootmotion->selfSyncTime;
        // 再来一个非时间戳节点, 采用时间永驻机制
        if (_filePtr->isMotion && frameList.size() == 2 && frameList.at(0)->type == 2 &&
            frameList.at(1)->type == 0)
            currTick = frameList.at(0)->time;
        // 不会处理，先跳过
        if (type == 12)
        {
            checkDrawStatus(currTick, renderList, lim);
            isNeedDraw = true;
            isLayout = true;
            originX = lim.originX;
            originY = lim.originY;
            width = lim.width;
            height = lim.height;
        }
        // 对于参数节点 进行参数反查来定位tick
        else if (isParameterize)
        {
            if (_rootmotion != nullptr)
            {
                float new_tick = _rootmotion->getTickByIdx(parameterIdx);
                if (new_tick >= 0.0)
                {
                    checkDrawStatus(new_tick, renderList, lim);
                    currTick = new_tick;
                }
                else
                    isNeedDraw = false;
            }
            else
                isNeedDraw = false;
        }
        else
        {
            // 时间戳判断 绘制信息检测
            checkDrawStatus(currTick, renderList, lim);
        }

        // 构建渲染方法
        renderMethod.clear();
        renderMethod = renderList;
        if ((!isNeedDraw && (width == 0 || height == 0)) || type == 7)
        {
            originX = lim.originX;
            originY = lim.originY;
            width = lim.width;
            height = lim.height;
        }
        else
        {
            // 基础参数
            if (nextframe != nullptr &&
                ((frame->type != 2) ||
                 (frame->type == 2 && nextframe->type == 2))) // 存在下一帧则对关键帧进行插值
            {
                // 针对nan/inf情形动态完成刷新
                if (std::isnan(frame->coordX)) frame->coordX = -lim.originX;
                if (std::isnan(frame->coordY)) frame->coordY = -lim.originY;
                if (std::isnan(nextframe->coordX)) nextframe->coordX = -lim.originX;
                if (std::isnan(nextframe->coordY)) nextframe->coordY = -lim.originY;
                if (std::isinf(frame->coordX)) frame->coordX = lim.width-lim.originX;
                if (std::isinf(frame->coordY)) frame->coordY = lim.width-lim.originY;
                if (std::isinf(nextframe->coordX)) nextframe->coordX = lim.height-lim.originX;
                if (std::isinf(nextframe->coordY)) nextframe->coordY = lim.height-lim.originY;

                // 坐标
                currCoordx =
                    (frame->coordX + (nextframe->coordX - frame->coordX) /
                                                  (nextframe->time - frame->time) *
                                                  (currTick - frame->time));
                currCoordy =
                    (frame->coordY + (nextframe->coordY - frame->coordY) /
                                                  (nextframe->time - frame->time) *
                                                  (currTick - frame->time));
                currCoordz =
                    (frame->coordZ + (nextframe->coordZ - frame->coordZ) /
                                                  (nextframe->time - frame->time) *
                                                  (currTick - frame->time));
                // 透明度
                currOpa = (frame->opa + (nextframe->opa - frame->opa) / (nextframe->time - frame->time) *
                                      (currTick - frame->time));
                // 变换参数(太sb了，感觉180才是分界点)
                if (nextframe->angle < 180 && 
                    frame->angle > 180) // 从 小360 到大0
                {
                    currAngle = (frame->angle - 360 +
                                 (nextframe->angle + 360 - frame->angle) /
                                                    (nextframe->time - frame->time) * (currTick - frame->time));
                }
                else if (nextframe->angle > 180 && frame->angle < 180) // 从 大0 到 小360
                {
                    currAngle = (frame->angle +
                                 (nextframe->angle - 360 - frame->angle) /
                                     (nextframe->time - frame->time) * (currTick - frame->time));
                }
                else
                {
                    currAngle = (frame->angle + (nextframe->angle - frame->angle) /
                                                    (nextframe->time - frame->time) *
                                                    (currTick - frame->time));
                }
                currSx = (frame->sx + (nextframe->sx - frame->sx) / (nextframe->time - frame->time) *
                                     (currTick - frame->time));
                currSy = (frame->sy + (nextframe->sy - frame->sy) / (nextframe->time - frame->time) *
                                     (currTick - frame->time));
                currZx = (frame->zx + (nextframe->zx - frame->zx) / (nextframe->time - frame->time) *
                                     (currTick - frame->time));
                currZy = (frame->zy + (nextframe->zy - frame->zy) / (nextframe->time - frame->time) *
                                     (currTick - frame->time));
                currOx =
                    (frame->ox + (nextframe->ox - frame->ox) / (nextframe->time - frame->time) *
                                     (currTick - frame->time));
                currOy =
                    (frame->oy + (nextframe->oy - frame->oy) / (nextframe->time - frame->time) *
                                     (currTick - frame->time));
                // 偏移
                currTimeOffset = (frame->timeOffset + (nextframe->timeOffset - frame->timeOffset) /
                                                          (nextframe->time - frame->time) *
                                                          (currTick - frame->time));
                // 网格参数
                if (frame->hasbp || nextframe->hasbp)
                {
                    isNeedBp = true;
                    for (size_t i = 0; i < 32; i++)
                        currbp[i] = (frame->bp[i] + (nextframe->bp[i] - frame->bp[i]) /
                                                        (nextframe->time - frame->time) *
                                                        (currTick - frame->time));
                }
                else
                    isNeedBp = false;
            }
            else
            {
                // 针对nan/inf情形动态完成刷新
                if (std::isnan(frame->coordX)) frame->coordX = -lim.originX;
                if (std::isnan(frame->coordY)) frame->coordY = -lim.originY;
                if (std::isinf(frame->coordX)) frame->coordX = lim.width-lim.originX;
                if (std::isinf(frame->coordY)) frame->coordY = lim.width-lim.originY;

                // 计算坐标
                currCoordx = frame->coordX;
                currCoordy = frame->coordY;
                currCoordz = frame->coordZ;
                // 透明度
                currOpa = frame->opa;
                // 变换参数
                currAngle = frame->angle;
                currSx = frame->sx, currSy = frame->sy;
                currZx = frame->zx, currZy = frame->zy;
                currOx = frame->ox, currOy = frame->oy;
                // 偏移
                currTimeOffset = frame->timeOffset;
                // 网格参数
                if (frame->hasbp)
                {
                    isNeedBp = true;
                    for (size_t i = 0; i < 32; i++)
                        currbp[i] = frame->bp[i];
                }
                else
                    isNeedBp = false;
            }

            // 有深度信息时，穿透到最顶层
            if (renderMethod.size() > 0 && currCoordz != 0.0)
            {
                if (renderMethod.at(0).type == 3)
                {
                    renderMethod.at(0).matTrans =
                        glm::translate(renderMethod.at(0).matTrans, glm::vec3(0.0, 0.0, currCoordz));
                }
                else
                {
                    glm::mat4 proj = glm::ortho(-renderMethod.at(0).originX,
                                                renderMethod.at(0).width - renderMethod.at(0).originX,
                                                renderMethod.at(0).height - renderMethod.at(0).originY,
                                   -renderMethod.at(0).originY, lim.zMax, -lim.zMax);
                    glm::mat4 last = glm::mat4(1.0f);
                    last = glm::translate(last, glm::vec3(0.0, 0.0, currCoordz));
                    last = last * glm::inverse(proj) * renderMethod.at(0).matTrans;
                    renderMethod.at(0).matTrans = proj * last;
                }
                GLM_ASSERT_VALID(renderMethod.at(0).matTrans);
            }

            // 有模板信息时，穿透到最顶层
            if (renderMethod.size() > 0 && type == 12)
            {
                renderMethod.at(0).hasStencil = true;
                for (auto nodeName : stencilCompositeMaskLayerList)
                {
                    // 让父类去找节点
                    emotenode* tmpNode = _rootmotion->getNodeByName(nodeName);
                    if (tmpNode != nullptr)
                    {
                        renderMethod.at(0).layerNode.push_back(tmpNode);
                    }
                }
            }

            // 是否需要同步父节点变形 玩不明白啊，反正视觉效果还行，就算了吧
            // if ((inheritMask >> 25 & 0x1) != 0x1 && renderMethod.size() > 0 &&
            // renderMethod.back().type == 1)
            //{
            //    emoteRender emt = renderMethod.back();
            //    renderMethod.pop_back();
            //    emt.type = 2;
            //    renderMethod.push_back(emt);
            //}

            // 构建变换矩阵 平移 currCoordx/currCoordy → 缩放 zx/zy → 剪切 sx/sy → 旋转 angle
            glm::mat4 model = glm::mat4(1.0f); // 注:复合顺序是反过来的
            model = glm::translate(model, glm::vec3(currCoordx, currCoordy, 0));
            model = glm::rotate(model, glm::radians(currAngle), glm::vec3(0.0f, 0.0f, 1.0f));
            model = glm::mat4(1.0f, currSy, 0.0f, 0.0f, currSx, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                              0.0f, 0.0f, 0.0f, 0.0f, 1.0f) *
                    model;
            model = glm::scale(model, glm::vec3(currZx, currZy, 1.0f));

            if (isLayout) // 不绘制，不处理网格数据，只构建变换矩阵
            {
                if (renderMethod.size() > 0 &&
                    renderMethod.back().type == 3) // 如果上一级是layout的话，则进行合并
                {
                    // 构造渲染方法结构
                    emoteRender emt = renderMethod.back();
                    renderMethod.pop_back();
                    emt.type = 3;

                    // 更新矩阵
                    emt.matTrans = emt.matTrans * model;

                    // fbo信息
                    emt.originX = originX;
                    emt.originY = originY;
                    emt.width = width;
                    emt.height = height;
                    emt.label = label;
                    GLM_ASSERT_VALID(emt.matTrans);
                    renderMethod.push_back(emt);
                }
                else
                {
                    // 构造渲染方法结构
                    emoteRender emt;
                    emt.type = 3;

                    // 如果父类没有网格变形，则进行矩阵解耦并外加合并
                    if (renderMethod.size() > 1 && renderMethod.back().type == 2)
                    {
                        glm::mat4 demuxMat =
                            glm::scale(renderMethod.back().matTrans,
                                       glm::vec3(1 / renderMethod.back().width,
                                                 1 / renderMethod.back().height, 1.0f));
                        demuxMat =
                            glm::translate(demuxMat, glm::vec3(renderMethod.back().originX,
                                                               renderMethod.back().originY, 0.0f));
                        emt.matTrans = demuxMat * model;
                        emt.opa *= renderMethod.back().opa;
                        renderMethod.pop_back();
                        // 绘制层解耦
                        originX = renderMethod.back().originX;
                        originY = renderMethod.back().originY;
                        width = renderMethod.back().width;
                        height = renderMethod.back().height;
                        emt.matTrans =
                            glm::inverse(glm::ortho(-originX, width - originX, height - originY,
                                                    -originY, lim.zMax, -lim.zMax)) *
                            emt.matTrans;
                    }
                    else // 正常更新矩阵
                    {
                        emt.matTrans = model;
                        
                    }

                    // fbo信息
                    emt.originX = originX;
                    emt.originY = originY;
                    emt.width = width;
                    emt.height = height;
                    emt.label = label;
                    GLM_ASSERT_VALID(emt.matTrans);
                    renderMethod.push_back(emt);
                }
            }
            else // 需要绘制，构建绘制矩阵和变形参数
            {
                if (renderMethod.size() > 0 &&
                    renderMethod.back().type == 3) // 如果上一级是layout的话，则进行合并
                {
                    // 构造渲染方法结构
                    emoteRender emt = renderMethod.back();
                    renderMethod.pop_back();
                    emt.type = 2;
                    emt.opa *= currOpa;
                    if (isNeedBp)
                    {
                        for (size_t i = 0; i < 32; i++)
                            emt.controlPts[i] = currbp[i];
                        emt.type = 1;
                    }

                    // 更新矩阵
                    glm::mat4 projection =
                        glm::ortho(-lim.originX, lim.width - lim.originX, lim.height - lim.originY,
                                   -lim.originY, lim.zMax, -lim.zMax);
                    model = glm::translate(model,
                                           glm::vec3(-originX - currOx, -originY - currOy, 0.0f));
                    model = glm::scale(model, glm::vec3(width, height, 1.0f));
                    emt.matTrans = projection * emt.matTrans * model;

                    // fbo信息
                    emt.originX = originX;
                    emt.originY = originY;
                    emt.width = width;
                    emt.height = height;
                    emt.label = label;
                    GLM_ASSERT_VALID(emt.matTrans);
                    renderMethod.push_back(emt);
                }
                else
                {
                    // 构造渲染方法结构
                    emoteRender emt;
                    emt.type = 2;
                    emt.opa = currOpa;
                    if (isNeedBp)
                    {
                        for (size_t i = 0; i < 32; i++)
                            emt.controlPts[i] = currbp[i];
                        emt.type = 1;
                    }

                    // 如果父类没有网格变形，则进行矩阵解耦并外加合并
                    if (renderMethod.size() > 0 && renderMethod.back().type == 2)
                    {
                        glm::mat4 demuxMat =
                            glm::scale(renderMethod.back().matTrans,
                                       glm::vec3(1 / renderMethod.back().width,
                                                 1 / renderMethod.back().height, 1.0f));
                        demuxMat =
                            glm::translate(demuxMat, glm::vec3(renderMethod.back().originX,
                                                               renderMethod.back().originY, 0.0f));
                        model = glm::translate(
                            model, glm::vec3(-originX - currOx, -originY - currOy, 0.0f));
                        model = glm::scale(model, glm::vec3(width, height, 1.0f));
                        emt.matTrans = demuxMat * model;
                        emt.opa *= renderMethod.back().opa;
                        renderMethod.pop_back();
                    }
                    else // 正常更新矩阵
                    {
                        glm::mat4 projection =
                            glm::ortho(-lim.originX, lim.width - lim.originX, lim.height - lim.originY, -lim.originY, lim.zMax, -lim.zMax);
                        model = glm::translate(
                            model, glm::vec3(-originX - currOx, -originY - currOy, 0.0f));
                        model = glm::scale(model, glm::vec3(width, height, 1.0f));
                        emt.matTrans = projection * model;
                    }

                    // fbo信息
                    emt.originX = originX;
                    emt.originY = originY;
                    emt.width = width;
                    emt.height = height;
                    emt.label = label;
                    GLM_ASSERT_VALID(emt.matTrans);
                    renderMethod.push_back(emt);
                }
            }
        }

        // 传递给子类
        for (auto ch : children)
        {
            ch->progress(tick, renderMethod, {originX, originY, width, height, lim.zMax});
        }

        // 处理motion情形
        if (emot != nullptr)
        {
            emot->progress(tick + currTimeOffset, renderMethod,
                           {originX, originY, width, height, lim.zMax});
        }
    }
    void emotenode::draw(GLuint targetFbo, emotelimit lim, GLuint exFbo, GLuint exTex)
    {
        //if (frame != nullptr && !frame->src.empty() && ic != nullptr)
        //{
        //    SDL_Log("%s with depth:%f and bm:%d", frame->src.c_str(), getCurrentRenderZ(), currbm);
        //}
        //if (frame != nullptr && strcmp(frame->src.c_str(), "src/tex/0066") == 0)
        //{
        //    cv::Mat rgba(height, width, CV_8UC4, data);
        //    cv::imshow("org", rgba);
        //}
        if (emot != nullptr) // 对于motion直接穿透绘制
        {
            emot->draw(targetFbo, lim, exFbo, exTex);
            return;
        }
        if (!isNeedDraw || !isIcon || renderMethod.size() < 1 || removed)
            return; // 跳过无需绘制的 和 非icon的 和 无method 的节点

        // 贴图必须在 draw 阶段、当前 GL 上下文中上传。
        if (!_iconGlTextureUploaded)
            _iconGlTextureUploaded = uploadIconGLTexture();
        if (!_iconGlTextureUploaded || selftexture == 0 || glIsTexture(selftexture) != GL_TRUE)
            return;

        //if (frame != nullptr && !frame->src.empty() && ic != nullptr)
        //{
        //    //SDL_Log("%s with depth:%f and bm:%d with tick:%f", frame->src.c_str(), getCurrentRenderZ(), currbm, currTick);
        //    //cv::Mat rgba(height, width, CV_8UC4, data);
        //    //cv::Mat bgra;
        //    //cv::cvtColor(rgba, bgra, cv::COLOR_RGBA2BGRA);
        //    //cv::imshow(frame->src, bgra);
        //}
        // 提前绘制好蒙版texture
        if (renderMethod.at(0).hasStencil && exFbo != 0) // 进行Stencil过滤 不考虑复合蒙版的情况了
        {
            glBindFramebuffer(GL_FRAMEBUFFER, exFbo);
            glBaseSet();
            for (auto maskLayer : renderMethod.at(0).layerNode)
            {
                if (maskLayer != nullptr)
                    maskLayer->draw(exFbo, lim, 0, 0);
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
        glUseProgram(emotenodeprogram);
        if (!emoteUseLegacyGL())
            glBindVertexArray(emotenodeVAO);
        glViewport(0, 0, lim.width, lim.height);
#if defined(__ANDROID__) || defined(__APPLE__)
        if (renderMethod.size() > 20)
#else
        if (renderMethod.size() > 64)
#endif
        {
            SDL_Log("render:%s failed!!!", label.c_str());
            return;
        }

        //static tjs_uint8* enoughData = new tjs_uint8[lim.width * lim.height * 4];
        //static GLfloat* enoughDepthData = new GLfloat[lim.width * lim.height];
        //if (frame != nullptr && strcmp(frame->src.c_str(), "src/tex/0066") == 0)
        //{
        //    glReadPixels(0, 0, lim.width, lim.height, GL_RGBA, GL_UNSIGNED_BYTE, enoughData);
        //    cv::Mat rgba(lim.height, lim.width, CV_8UC4, enoughData);
        //    cv::imshow("before", rgba);
        //
        //    glReadPixels(0, 0, lim.width, lim.height, GL_DEPTH_COMPONENT, GL_FLOAT,
        //                 enoughDepthData);
        //    cv::Mat depthMat(height, width, CV_32FC1, enoughDepthData);
        //    cv::Mat normalized;
        //    cv::normalize(depthMat, normalized, 0.0, 1.0, cv::NORM_MINMAX);
        //    cv::Mat displayMat;
        //    normalized.convertTo(displayMat, CV_8UC1, 255.0);
        //    cv::Mat colorMap;
        //    cv::applyColorMap(displayMat, colorMap, cv::COLORMAP_JET);
        //    cv::imshow("depth", colorMap);
        //}

        // bm — 与 sdl3 一致：分离 alpha 通道并使用 MAX 方程做深度合成，避免重影
        switch (currbm)
        {
            case 0:
            {
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
                glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
                break;
            }
            case 1:
            {
                glBlendFuncSeparate(GL_DST_COLOR, GL_ONE, GL_ZERO, GL_ONE);
                glBlendEquation(GL_FUNC_ADD);
                break;
            }
            case 3:
            {
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
                glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
                break;
            }
            case 4:
            {
                glBlendFuncSeparate(GL_DST_COLOR, GL_ONE, GL_ZERO, GL_ONE);
                glBlendEquation(GL_FUNC_ADD);
                break;
            }
            case 6: // TODO
            {
                return;
            }
            default:
            {
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
                glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
            }
        }
        // renderMethod
        const int surfaceCount = static_cast<int>(renderMethod.size());
        int idxCnt = 0;
        float totalOpa = currOpa;
        std::vector<glm::mat4> patchTransforms(static_cast<size_t>(surfaceCount));
        std::vector<const float*> patchControlPoints(static_cast<size_t>(surfaceCount));
        if (!emoteUseLegacyGL())
            glUniform1i(glGetUniformLocation(emotenodeprogram, "surfaceCount"), surfaceCount);
        for (int32_t i = surfaceCount - 1; i >= 0; i--)
        {
            // opa
            totalOpa *= renderMethod.at(i).opa;
            patchTransforms[static_cast<size_t>(idxCnt)] = renderMethod.at(i).matTrans;
            patchControlPoints[static_cast<size_t>(idxCnt)] =
                renderMethod.at(i).type == 1 ? renderMethod.at(i).controlPts : default_control_points;
            if (!emoteUseLegacyGL())
            {
                // transform
                char uniformName[32];
#if defined(_MSC_VER)
                sprintf_s(uniformName, 32, "transforms[%d]", idxCnt);
#else
                sprintf(uniformName, "transforms[%d]", idxCnt);
#endif
                glUniformMatrix4fv(glGetUniformLocation(emotenodeprogram, uniformName), 1, GL_FALSE,
                                   glm::value_ptr(renderMethod.at(i).matTrans));
                // controlPoints
#if defined(_MSC_VER)
                sprintf_s(uniformName, 32, "controlPoints[%d]", idxCnt);
#else
                sprintf(uniformName, "controlPoints[%d]", idxCnt);
#endif
                glUniform2fv(glGetUniformLocation(emotenodeprogram, uniformName), 16,
                             patchControlPoints[static_cast<size_t>(idxCnt)]);
            }
            idxCnt++;
        }
        // opa
        glUniform1f(glGetUniformLocation(emotenodeprogram, "opa"), totalOpa);
        // texture
        if (renderMethod.at(0).hasStencil && exFbo != 0) // 使用exTex作为蒙版过滤
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, selftexture);
            glUniform1i(glGetUniformLocation(emotenodeprogram, "texture1"), 0);
            glUniform1i(glGetUniformLocation(emotenodeprogram, "enableMask"), true);
            glUniform2f(glGetUniformLocation(emotenodeprogram, "viewportSize"), lim.width, lim.height);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, exTex);
            glUniform1i(glGetUniformLocation(emotenodeprogram, "maskTexture"), 1);
            if (emoteUseLegacyGL())
                emoteDrawPatch(idxCnt, glm::value_ptr(patchTransforms[0]), patchControlPoints.data());
            else
                glDrawArrays(GL_PATCHES, 0, 16);
        }
        else
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, selftexture);
            glUniform1i(glGetUniformLocation(emotenodeprogram, "texture1"), 0);
            glUniform1i(glGetUniformLocation(emotenodeprogram, "enableMask"), false);
            if (emoteUseLegacyGL())
                emoteDrawPatch(idxCnt, glm::value_ptr(patchTransforms[0]), patchControlPoints.data());
            else
                glDrawArrays(GL_PATCHES, 0, 16);
        }

        //if (frame != nullptr && strcmp(frame->src.c_str(), "src/tex/0066") == 0)
        //{
        //  static tjs_uint8* enoughData = new tjs_uint8[lim.width * lim.height * 4];
        //  glReadPixels(0, 0, lim.width, lim.height, GL_RGBA, GL_UNSIGNED_BYTE, enoughData);
        //  cv::Mat rgba(lim.height, lim.width, CV_8UC4, enoughData);
        //  cv::Mat bgra;
        //  cv::cvtColor(rgba, bgra, cv::COLOR_RGBA2BGRA);
        //  cv::imshow(frame->src, bgra);
        //}
    }
    void emotenode::drawToLayer(tTJSNI_BaseLayer* layer, emotelimit lim, tTJSNI_BaseLayer* maskLayer)
    {
        if (emot != nullptr)
        {
            emot->drawToLayer(layer, lim, maskLayer);
            return;
        }
        if (!isNeedDraw || !isIcon || renderMethod.size() < 1 || removed || layer == nullptr || data == nullptr)
            return;

        if (renderMethod.at(0).hasStencil && maskLayer != nullptr)
        {
            for (auto maskNode : renderMethod.at(0).layerNode)
            {
                if (maskNode != nullptr)
                    maskNode->drawToLayer(maskLayer, lim, nullptr);
            }
        }

        iTVPBaseBitmap* sourceBitmap = getSourceBitmap();
        if (sourceBitmap == nullptr)
            return;
        drawPatchCommon(lim, sourceBitmap, layer, nullptr, tTVPRect());
    }
    void emotenode::drawToBitmap(iTVPBaseBitmap* target, const tTVPRect& targetClip, emotelimit lim,
                                 tTJSNI_BaseLayer* maskLayer)
    {
        if (emot != nullptr)
        {
            emot->drawToBitmap(target, targetClip, lim, maskLayer);
            return;
        }
        if (!isNeedDraw || !isIcon || renderMethod.size() < 1 || removed || target == nullptr ||
            data == nullptr)
            return;

        if (renderMethod.at(0).hasStencil && maskLayer != nullptr)
        {
            for (auto maskNode : renderMethod.at(0).layerNode)
            {
                if (maskNode != nullptr)
                    maskNode->drawToBitmap(target, targetClip, lim, nullptr);
            }
        }

        iTVPBaseBitmap* sourceBitmap = getSourceBitmap();
        if (sourceBitmap == nullptr)
            return;
        drawPatchCommon(lim, sourceBitmap, nullptr, target, targetClip);
    }
    float emotenode::getCurrentRenderZ()
    {
        if (renderMethod.size() > 0)
        {
            return renderMethod.at(0).matTrans[3][2];
        }
        return 0;
    }
    void emotenode::resizeMainData()
    {
        if (data != nullptr)
            delete data;
        data = new uint8_t[width * height * 4];
        memset(data, 0, width * height * 4);
    }
    void emotenode::resizeOpenGL()
    {
        if (selftexture != 0 && TVPEnsureEmoteGLContext() && glIsTexture(selftexture) == GL_TRUE)
            glDeleteTextures(1, &selftexture);
        selftexture = 0;
        _iconGlTextureUploaded = false;
    }
    bool emotenode::uploadIconGLTexture()
    {
        if (!isIcon || data == nullptr || width <= 0 || height <= 0 || ic == nullptr)
            return false;
        if (!TVPEnsureEmoteGLContext())
            return false;

        if (selftexture == 0 || glIsTexture(selftexture) != GL_TRUE)
        {
            if (selftexture != 0)
                glDeleteTextures(1, &selftexture);
            selftexture = createEmptyTexture(width, height);
        }
        if (selftexture == 0 || glIsTexture(selftexture) != GL_TRUE)
            return false;

        glBindTexture(GL_TEXTURE_2D, selftexture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
#if defined(_MSC_VER) || defined(__APPLE__)
        if (_filePtr->colorType == 0)
        {
            if (emoteUseLegacyGL())
            {
                const size_t pxCount = static_cast<size_t>(width) * static_cast<size_t>(height);
                std::vector<uint8_t> rgba(pxCount * 4);
                for (size_t i = 0; i < pxCount; ++i)
                {
                    rgba[4 * i] = data[4 * i + 2];
                    rgba[4 * i + 1] = data[4 * i + 1];
                    rgba[4 * i + 2] = data[4 * i];
                    rgba[4 * i + 3] = data[4 * i + 3];
                }
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, rgba.data());
            }
            else
            {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA, width, height, 0, GL_BGRA,
                             GL_UNSIGNED_BYTE, data);
            }
        }
        else if (_filePtr->colorType == 1)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         data);
        }
        else
        {
            SDL_Log("unknow colorType");
            return false;
        }
#else
        if (_filePtr->colorType == 0)
        {
            for (size_t i = 0; i < static_cast<size_t>(width) * height; i++)
            {
                uint8_t tmp = data[4 * i];
                data[4 * i] = data[4 * i + 2];
                data[4 * i + 2] = tmp;
            }
        }
        else if (_filePtr->colorType != 1)
        {
            SDL_Log("unknow colorType");
            return false;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
#endif
        if (!emoteUseLegacyGL())
            glGenerateMipmap(GL_TEXTURE_2D);
        return true;
    }
} // namespace emoteplayer
