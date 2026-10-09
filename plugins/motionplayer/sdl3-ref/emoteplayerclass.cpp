#include "ncbind.hpp"
#include "emoteplayerclass.h"
#include "EmoteTVPRenderer.h"
#include "EmoteInternal.h"
#include "EmoteGLBridge.h"
#include "tjsArray.h"
#include "TickCount.h"
#include "LayerBitmapIntf.h"
#include "RenderManager.h"

#include <SDL2/SDL_log.h>

#include <algorithm>
#include <cstring>

namespace emoteplayer
{

iTJSDispatch2* ResourceManager::_kagWindow = nullptr;
static SeparateLayerAdaptor* _motionWorkLayer = nullptr;

ResourceManager::ResourceManager(iTJSDispatch2* kagWindow, tjs_int cacheSize)
{
    tjs_int sWidth = 1280, sHeight = 720;
    if (kagWindow != nullptr)
    {
        tTJSVariant val;
        kagWindow->PropGet(0, TJS_W("width"), NULL, &val, kagWindow);
        sWidth = (tjs_int)val;
        kagWindow->PropGet(0, TJS_W("height"), NULL, &val, kagWindow);
        sHeight = (tjs_int)val;
        _kagWindow = kagWindow;
    }
    if (_motionWorkLayer == nullptr)
    {
        tTJSVariant baseLayer;
        iTJSDispatch2* kag = kagWindow;
        if (TJS_FAILED(kag->PropGet(0, TJS_W("poolLayer"), NULL, &baseLayer, kag)) ||
            baseLayer.Type() != tvtObject)
        {
            SDL_Log("create motionWorkLayer failed");
            return;
        }
        _motionWorkLayer = new SeparateLayerAdaptor(baseLayer.AsObjectThisNoAddRef());
        tTJSVariant val = tTJSVariant(_motionWorkLayer);
        _motionWorkLayer->Release();
        iTJSDispatch2* global = TVPGetScriptDispatch();
        if (global)
        {
            global->PropSet(TJS_MEMBERENSURE, TJS_W("motionWorkLayer"), NULL, &val, global);
            global->Release();
        }
    }
}
ResourceManager::~ResourceManager()
{
    unloadAll();
}
tTJSVariant ResourceManager::load(tTJSString path)
{
    ttstr trimPath;
    if (path.StartsWith(TJS_W("lzfs://./")))
        trimPath = path.SubString(9, path.length() - 9);
    else
        trimPath = path;
    auto rst = cacheData.find(TVPGetPlacedPath(trimPath));
    if (rst != cacheData.end())
    {
        return rst->second->root();
    }
    emotefile* file = new emotefile();
    file->setSeed(_decryptkey);
    file->setFun(_decryptClo);
    file->load(trimPath);

    cacheData.insert(std::pair<ttstr, emotefile*>(TVPGetPlacedPath(trimPath), file));
    return file->root();
}
void ResourceManager::unload(tTJSString path)
{
    ttstr trimPath;
    if (path.StartsWith(TJS_W("lzfs://./")))
        trimPath = path.SubString(9, path.length() - 9);
    else
        trimPath = path;
    auto it = cacheData.find(TVPGetPlacedPath(trimPath));
    if (it != cacheData.end())
    {
        if (it->second != nullptr)
            delete it->second;
        cacheData.erase(it);
    }
}
void ResourceManager::unloadAll()
{
    for (auto item : cacheData)
    {
        if (item.second != nullptr)
            delete item.second;
    }
    cacheData.clear();
}
emotefile* ResourceManager::GetPlayerByName(const tTJSString& name)
{
    auto it = cacheData.find(name);
    if (it != cacheData.end())
    {
        return it->second;
    }
    return nullptr;
}
void ResourceManager::setEmotePSBDecryptSeed(tjs_int decryptkey)
{
    _decryptkey = decryptkey;
}
void ResourceManager::setEmotePSBDecryptFunc(tTJSVariant funclosure)
{
    _decryptClo = funclosure.AsObjectClosure();
}

SeparateLayerAdaptor::SeparateLayerAdaptor(iTJSDispatch2* targetLayer)
{
    _this = new tTJSNI_Layer();
    tTJSVariant kag(ResourceManager::_kagWindow);
    tTJSVariant layer(targetLayer);
    tTJSVariant* params[] = {&kag, &layer};
    if ((TJS_FAILED(_this->Construct(2, params, this)) < 0))
        TVPThrowExceptionMessage(TVPSpecifyLayer);
    tTJSNI_BaseLayer* ths = NULL;
    if (targetLayer->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                                           (iTJSNativeInstance**)&ths) < 0 ||
        ths == NULL)
        TVPThrowExceptionMessage(TVPSpecifyLayer);
    _this->SetSize(ths->GetWidth(), ths->GetHeight());
    _this->SetImageSize(ths->GetWidth(), ths->GetHeight());
    _this->SetVisible(true);
    _this->SetType(ltAlpha);
    _this->SetHitType(htProvince);
}
SeparateLayerAdaptor::~SeparateLayerAdaptor()
{
    clear();
}
void SeparateLayerAdaptor::assign(iTJSDispatch2* anotherAdaptor)
{
    // TODO
}
void SeparateLayerAdaptor::clear()
{
    if (TVPEnsureEmoteGLContext())
    {
        if (fbotexture != 0 && glIsTexture(fbotexture) == GL_TRUE)
            glDeleteTextures(1, &fbotexture);
        if (fbodepthtexture != 0 && glIsTexture(fbodepthtexture) == GL_TRUE)
            glDeleteTextures(1, &fbodepthtexture);
        if (superfbotexture != 0 && glIsTexture(superfbotexture) == GL_TRUE)
            glDeleteTextures(1, &superfbotexture);
        if (superfbodepthtexture != 0 && glIsTexture(superfbodepthtexture) == GL_TRUE)
            glDeleteTextures(1, &superfbodepthtexture);
        if (fbo != 0 && glIsFramebuffer(fbo) == GL_TRUE)
            glDeleteFramebuffers(1, &fbo);
        if (superfbo != 0 && glIsFramebuffer(superfbo) == GL_TRUE)
            glDeleteFramebuffers(1, &superfbo);
        fbotexture = 0;
        fbodepthtexture = 0;
        superfbotexture = 0;
        superfbodepthtexture = 0;
        fbo = 0;
        superfbo = 0;
    }
    if (_this != nullptr)
    {
        _this->Invalidate();
        delete _this;
        _this = nullptr;
    }
}
void SeparateLayerAdaptor::set_absolute(tjs_int v)
{
    if (_this != nullptr)
    {
        _absolute = v;
        _this->SetAbsoluteOrderIndex(v);
    }
}
void SeparateLayerAdaptor::checkDrawArea(tjs_int width, tjs_int height)
{
    if (_this != nullptr && (_width != width || _height != height))
    {
        _this->SetSize(width, height);
        _this->SetImageSize(width, height);
    }
    if (!TVPEnsureEmoteGLContext())
        return;

    if (fbo == 0 || glIsFramebuffer(fbo) != GL_TRUE)
        glGenFramebuffers(1, &fbo);
    if (superfbo == 0 || glIsFramebuffer(superfbo) != GL_TRUE)
        glGenFramebuffers(1, &superfbo);

    if (_width == width && _height == height)
        return;

    _width = width;
    _height = height;

    const GLuint newfbotexture = createEmptyTexture(width, height);
    const GLuint newfbodepthtexture = createEmptyDepthTexture(width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, newfbotexture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, newfbodepthtexture,
                         0);

    const GLuint newsuperfbotexture = createEmptyTexture(width, height);
    const GLuint newsuperfbodepthtexture = createEmptyDepthTexture(width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, superfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, newsuperfbotexture,
                           0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           newsuperfbodepthtexture, 0);

    if (fbotexture != 0 && glIsTexture(fbotexture) == GL_TRUE)
        glDeleteTextures(1, &fbotexture);
    if (fbodepthtexture != 0 && glIsTexture(fbodepthtexture) == GL_TRUE)
        glDeleteTextures(1, &fbodepthtexture);
    if (superfbotexture != 0 && glIsTexture(superfbotexture) == GL_TRUE)
        glDeleteTextures(1, &superfbotexture);
    if (superfbodepthtexture != 0 && glIsTexture(superfbodepthtexture) == GL_TRUE)
        glDeleteTextures(1, &superfbodepthtexture);

    fbotexture = newfbotexture;
    fbodepthtexture = newfbodepthtexture;
    superfbotexture = newsuperfbotexture;
    superfbodepthtexture = newsuperfbodepthtexture;
}

#define setprop_t(d, p, ty) \
    { \
        tTJSVariant v(ty(p)); \
        d->PropSet(TJS_MEMBERENSURE, TJS_W(#p), nullptr, &v, d); \
    }
#define setprop(d, p) setprop_t(d, p, )
#define getprop_t(d, p, ty) \
    { \
        tTJSVariant v; \
        if (TJS_SUCCEEDED(d->PropGet(0, TJS_W(#p), nullptr, &v, d)) && v.Type() != tvtVoid) \
        { \
            p = ty(v); \
        } \
    }
#define getprop(d, p) getprop_t(d, p, )

namespace {

void blitRGBAWithScale(tjs_uint8* dst, tjs_int dstW, tjs_int dstH, tjs_int dstPitch, const tjs_uint8* src,
                       tjs_int srcW, tjs_int srcH)
{
    if (dst == nullptr || src == nullptr || dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0)
        return;

    if (dstW == srcW && dstH == srcH)
    {
        for (tjs_int y = 0; y < dstH; ++y)
            memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(dstPitch), src + static_cast<size_t>(y) * static_cast<size_t>(srcW) * 4u,
                   static_cast<size_t>(srcW) * 4u);
        return;
    }

    for (tjs_int y = 0; y < dstH; ++y)
    {
        const tjs_int srcY = (y * srcH) / dstH;
        const tjs_uint8* srcRow = src + static_cast<size_t>(srcY) * static_cast<size_t>(srcW) * 4u;
        tjs_uint8* dstRow = dst + static_cast<size_t>(y) * static_cast<size_t>(dstPitch);
        for (tjs_int x = 0; x < dstW; ++x)
        {
            const tjs_int srcX = (x * srcW) / dstW;
            memcpy(dstRow + static_cast<size_t>(x) * 4u, srcRow + static_cast<size_t>(srcX) * 4u, 4u);
        }
    }
}

} // namespace

static void fillLayerColor(tTJSNI_BaseLayer* layer, tjs_uint32 color)
{
    if (layer == nullptr)
        return;
    tjs_uint8* buff = static_cast<tjs_uint8*>(layer->GetMainImagePixelBufferForWrite());
    if (buff == nullptr)
        return;
    const tjs_int pitch = layer->GetMainImagePixelBufferPitch();
    const tjs_int h = layer->GetHeight();
    const tjs_int w = layer->GetWidth();
    const tjs_uint8 b = static_cast<tjs_uint8>(color & 0xFF);
    const tjs_uint8 g = static_cast<tjs_uint8>((color >> 8) & 0xFF);
    const tjs_uint8 r = static_cast<tjs_uint8>((color >> 16) & 0xFF);
    const tjs_uint8 a = static_cast<tjs_uint8>((color >> 24) & 0xFF);
    for (tjs_int y = 0; y < h; ++y)
    {
        tjs_uint8* row = buff + y * pitch;
        for (tjs_int x = 0; x < w; ++x)
        {
            tjs_uint8* px = row + x * 4;
            px[0] = b;
            px[1] = g;
            px[2] = r;
            px[3] = a;
        }
    }
    layer->Update();
}

EmotePlayer::~EmotePlayer()
{
    if (TVPEnsureEmoteGLContext())
    {
        if (fbotexture != 0 && glIsTexture(fbotexture) == GL_TRUE)
            glDeleteTextures(1, &fbotexture);
        if (fbodepthtexture != 0 && glIsTexture(fbodepthtexture) == GL_TRUE)
            glDeleteTextures(1, &fbodepthtexture);
        if (superfbotexture != 0 && glIsTexture(superfbotexture) == GL_TRUE)
            glDeleteTextures(1, &superfbotexture);
        if (superfbodepthtexture != 0 && glIsTexture(superfbodepthtexture) == GL_TRUE)
            glDeleteTextures(1, &superfbodepthtexture);
        if (fbo != 0 && glIsFramebuffer(fbo) == GL_TRUE)
            glDeleteFramebuffers(1, &fbo);
        if (superfbo != 0 && glIsFramebuffer(superfbo) == GL_TRUE)
            glDeleteFramebuffers(1, &superfbo);
    }
    if (m_BmpBits != nullptr)
        delete static_cast<tTVPBaseTexture*>(m_BmpBits);
    if (m_bmpData != nullptr)
        delete[] m_bmpData;
}
int32_t EmotePlayer::get_loopTime()
{
    if (_currentfile != nullptr && _currentfile->_objects.size() > 0 &&
        _currentfile->_objects.begin()->second->motion.size() > 0)
    {
        return _currentfile->_objects.begin()->second->motion.begin()->second->loopTime;
    }
    return 0;
}
tTJSVariant EmotePlayer::get_variableKeys()
{
    iTJSDispatch2* array = TJSCreateArrayObject();
    if (_currentfile != nullptr)
    {
        for (auto varItm : _currentfile->_metadata->_varList)
        {
            tTJSVariant tmp(varItm.first);
            tTJSVariant* args[] = {&tmp};
            static tjs_uint addHint = 0;
            array->FuncCall(0, TJS_W("add"), &addHint, nullptr, 1, args, array);
        }
    }
    tTJSVariant result(array, array);
    array->Release();
    return result;
}
tTJSVariant EmotePlayer::serialize()
{
    auto dict = TJSCreateDictionaryObject();

    setprop(dict, currCoordx);
    setprop(dict, currCoordy);
    setprop(dict, currCoordz);
    setprop(dict, currAngle);
    setprop(dict, currZx);
    setprop(dict, currZy);

    auto res = tTJSVariant(dict, dict);
    dict->Release();
    return res;
}
void EmotePlayer::unserialize(tTJSVariant data)
{
    auto dict = data.AsObjectNoAddRef();
    if (!dict)
    {
        return;
    }

    getprop_t(dict, currCoordx, static_cast<tjs_real>);
    getprop_t(dict, currCoordy, static_cast<tjs_real>);
    getprop_t(dict, currCoordz, static_cast<tjs_real>);
    getprop_t(dict, currAngle, static_cast<tjs_real>);
    getprop_t(dict, currZx, static_cast<tjs_real>);
    getprop_t(dict, currZy, static_cast<tjs_real>);
}
void EmotePlayer::play(tTJSString name, int flag)
{
    if (_currentfile != nullptr)
    {
        auto it = _currentfile->_objects.find(_currentfile->_metadata->chara.c_str());
        if (it != _currentfile->_objects.end())
        {
            auto it1 = it->second->motion.find(_currentfile->_metadata->motion.c_str());
            if (it1 != it->second->motion.end())
            {
                _currmotion = it1->second;
            }
        }
        clockPassed = 0.0;
        _motion = name;
        _animating = true;
        _playing = true;
        _allplaying = true;
        isSelfClear = true;
    }
    else if (_resourceManager->cacheData.size() == 1 &&
             _resourceManager->cacheData.begin()->second->isMotion)
    {
        _motion = name;
        _currentfile = _resourceManager->cacheData.begin()->second;
        auto it = _currentfile->_objects.find(_chara.AsStdString().c_str());
        if (it != _currentfile->_objects.end())
        {
            auto it1 = it->second->motion.find(_motion.AsStdString().c_str());
            if (it1 != it->second->motion.end())
            {
                _currmotion = it1->second;
            }
        }
        clockPassed = 0.0;
        _motion = name;
        _animating = true;
        _playing = true;
        _allplaying = true;
        isSelfClear = true;
    }
    else
    {
        for (auto itm : _resourceManager->cacheData)
        {
            auto it = itm.second->_objects.find(itm.second->_metadata->chara.c_str());
            if (it != itm.second->_objects.end())
            {
                auto it1 = it->second->motion.find(itm.second->_metadata->motion.c_str());
                if (it1 != it->second->motion.end())
                {
                    _currentfile = itm.second;
                    _currmotion = it1->second;
                    break;
                }
            }
        }
        for (auto itm1 : _resourceManager->cacheData)
        {
            for (auto itm2 : _resourceManager->cacheData)
            {
                if (itm1.second != itm2.second)
                    itm1.second->addEmoteFile(itm2.second);
            }
        }
        clockPassed = 0.0;
        _motion = name;
        _animating = true;
        _playing = true;
        _allplaying = true;
        isSelfClear = false;
    }
}
void EmotePlayer::initPhysics(tTJSVariant metadata)
{
    SDL_Log("EmotePlayer::initPhysics TODO");
}
void EmotePlayer::clear(iTJSDispatch2* layer, tjs_uint32 neutralColor)
{
    auto* self = ncbInstanceAdaptor<SeparateLayerAdaptor>::GetNativeInstance(layer);
    tTJSNI_BaseLayer* ths = NULL;
    if (self != nullptr)
    {
        ths = self->GetLayer();
    }
    else
    {
        if (layer->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                                         (iTJSNativeInstance**)&ths) < 0)
            return;
        withoutAdaptor = true;
    }
    if (ths == NULL)
        return;

    fillLayerColor(ths, neutralColor);
}
void EmotePlayer::progress(tjs_real mstime)
{
    if (_isStop)
        return;
    if (_currentfile != nullptr && _currmotion != nullptr && clockPassed > -1.0 &&
        _limitArea.width != _limitArea.originX && _limitArea.height != _limitArea.originY)
    {
        if (_playing)
            clockPassed += mstime / speedRatio;
        std::vector<emoteRender> empty;
        empty.push_back(_renderMethod);
        if (_currentfile->isMirror)
        {
            empty.at(0).matTrans = glm::scale(empty.at(0).matTrans, glm::vec3(-1.0f, 1.0f, 1.0f));
        }
        if (_currentfile->_metadata->_varList.size() > 0)
        {
            _currentfile->updateEyeControl(clockPassed, true);
            _currentfile->updateTimelineControl(clockPassed, true);
            _currmotion->progress(0, empty, _limitArea);
        }
        else
        {
            if (!_currentfile->isMotion && clockPassed > _currmotion->lastTime)
            {
                _animating = false;
                _playing = false;
                _allplaying = false;
            }
            if (_currentfile->isMotion && clockPassed > _currmotion->syncTime)
            {
                clockPassed = _currmotion->syncTime;
                _animating = false;
                _playing = false;
                _allplaying = false;
            }
            _currmotion->progress(clockPassed, empty, _limitArea);
        }
    }
}
void EmotePlayer::setOpenGLDrawArea(tjs_int width, tjs_int height)
{
    if (!TVPEnsureEmoteGLContext())
        return;

    if (fbo == 0 || glIsFramebuffer(fbo) != GL_TRUE)
        glGenFramebuffers(1, &fbo);
    if (superfbo == 0 || glIsFramebuffer(superfbo) != GL_TRUE)
        glGenFramebuffers(1, &superfbo);

    const GLuint newfbotexture = createEmptyTexture(width, height);
    const GLuint newfbodepthtexture = createEmptyDepthTexture(width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, newfbotexture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, newfbodepthtexture,
                         0);

    const GLuint newsuperfbotexture = createEmptyTexture(width, height);
    const GLuint newsuperfbodepthtexture = createEmptyDepthTexture(width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, superfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, newsuperfbotexture,
                           0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           newsuperfbodepthtexture, 0);

    if (fbotexture != 0 && glIsTexture(fbotexture) == GL_TRUE)
        glDeleteTextures(1, &fbotexture);
    if (fbodepthtexture != 0 && glIsTexture(fbodepthtexture) == GL_TRUE)
        glDeleteTextures(1, &fbodepthtexture);
    if (superfbotexture != 0 && glIsTexture(superfbotexture) == GL_TRUE)
        glDeleteTextures(1, &superfbotexture);
    if (superfbodepthtexture != 0 && glIsTexture(superfbodepthtexture) == GL_TRUE)
        glDeleteTextures(1, &superfbodepthtexture);

    fbotexture = newfbotexture;
    fbodepthtexture = newfbodepthtexture;
    superfbotexture = newsuperfbotexture;
    superfbodepthtexture = newsuperfbodepthtexture;
}

void EmotePlayer::drawWithTVPFallback(tTJSNI_BaseLayer* layer)
{
    if (layer == nullptr || _currmotion == nullptr)
        return;

    if (isSelfClear)
    {
        tjs_uint8* buff = static_cast<tjs_uint8*>(layer->GetMainImagePixelBufferForWrite());
        if (buff != nullptr)
            memset(buff, 0, layer->GetMainImagePixelBufferPitch() * layer->GetHeight());
    }

    auto* offscreen = static_cast<tTVPBaseTexture*>(m_BmpBits);
    if (offscreen != nullptr)
    {
        const tTVPRect targetClip(0, 0, _width, _height);
        if (isSelfClear)
            offscreen->FillColor(targetClip, 0, 255);
        _currmotion->drawToBitmap(offscreen, targetClip, _limitArea, nullptr);

        tjs_uint8* dstBits = static_cast<tjs_uint8*>(layer->GetMainImagePixelBufferForWrite());
        if (dstBits != nullptr)
        {
            iTVPTexture2D* offscreenTex = offscreen->GetTexture();
            const tjs_uint8* srcBits = offscreenTex != nullptr
                                           ? static_cast<const tjs_uint8*>(offscreenTex->GetPixelData())
                                           : nullptr;
            if (srcBits != nullptr)
            {
                const tjs_int dstPitch = layer->GetMainImagePixelBufferPitch();
                const tjs_int srcPitch = offscreenTex->GetPitch();
                const tjs_int copyWidth =
                    std::min<tjs_int>(_width, static_cast<tjs_int>(layer->GetWidth()));
                const tjs_int copyHeight =
                    std::min<tjs_int>(_height, static_cast<tjs_int>(layer->GetHeight()));
                for (tjs_int y = 0; y < copyHeight; ++y)
                {
                    memcpy(dstBits + y * dstPitch, srcBits + y * srcPitch,
                           static_cast<size_t>(copyWidth) * 4u);
                }
            }
        }
    }
    else
    {
        _currmotion->drawToLayer(layer, _limitArea, nullptr);
    }
}

void EmotePlayer::draw(iTJSDispatch2* objthis)
{
    auto* self = ncbInstanceAdaptor<SeparateLayerAdaptor>::GetNativeInstance(objthis);
    tTJSNI_BaseLayer* ths = NULL;
    if (self != nullptr)
    {
        ths = self->GetLayer();
        if (ths == NULL)
            return;
    }
    else
    {
        if (objthis->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                                           (iTJSNativeInstance**)&ths) < 0)
            return;
        withoutAdaptor = true;
    }

    if (_currentfile == nullptr || _currmotion == nullptr)
        return;

    ResetDrawArea(ths->GetWidth(), ths->GetHeight());

    // Primary path: same GL FBO + tessellation pipeline as sdl3/emoteplayer (verified correct).
    if (TVPEnsureEmoteGLContext())
    {
        GLuint targetFbo = 0;
        GLuint superFbo = 0;
        GLuint superTex = 0;
        if (!withoutAdaptor && self != nullptr)
        {
            self->checkDrawArea(_width, _height);
            targetFbo = self->fbo;
            superFbo = self->superfbo;
            superTex = self->superfbotexture;
        }
        else
        {
            setOpenGLDrawArea(_width, _height);
            targetFbo = fbo;
            superFbo = superfbo;
            superTex = superfbotexture;
        }

        if (targetFbo != 0)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
            glBaseSetWithoutClear();
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            _currmotion->draw(targetFbo, _limitArea, superFbo, superTex);

            const size_t byteCount =
                static_cast<size_t>(_width) * static_cast<size_t>(_height) * 4u;
            if (m_bmpData == nullptr)
                m_bmpData = new tjs_uint8[byteCount];
            glReadPixels(0, 0, _width, _height, GL_RGBA, GL_UNSIGNED_BYTE, m_bmpData);

            tjs_uint8* buff = static_cast<tjs_uint8*>(ths->GetMainImagePixelBufferForWrite());
            if (buff != nullptr)
            {
                const tjs_int layerW = ths->GetWidth();
                const tjs_int layerH = ths->GetHeight();
                const tjs_int layerPitch = ths->GetMainImagePixelBufferPitch();
                blitRGBAWithScale(buff, layerW, layerH, layerPitch, m_bmpData, _width, _height);
            }

            emoteRestoreGLState();
            ths->Update();
            return;
        }
    }

    drawWithTVPFallback(ths);
    ths->Update();
}
void EmotePlayer::assign(iTJSDispatch2* anotherAdaptor)
{
    SDL_Log("EmotePlayer::assign TODO");
}
void EmotePlayer::setCoord(tjs_real x, tjs_real y)
{
    if (_currentfile != nullptr)
    {
        currCoordx = x;
        currCoordy = y;
        updateTransMat();
    }
}
void EmotePlayer::setScale(tjs_real scale)
{
    if (_currentfile != nullptr)
    {
        currZx = scale;
        currZy = scale;
        updateTransMat();
    }
}
void EmotePlayer::setRotate(tjs_real rotate)
{
    if (_currentfile != nullptr)
    {
        currAngle = rotate;
        updateTransMat();
    }
}
void EmotePlayer::setColor(tjs_uint32 color)
{
    SDL_Log("EmotePlayer::setColor TODO");
}
void EmotePlayer::setVariable(tTJSString name, tjs_real value)
{
    if (_currentfile != nullptr)
    {
        std::string tmpName = name.AsNarrowStdString();
        tmpName.append(1, '\0');
        _currentfile->setVariable(tmpName, value, true);
    }
}
tjs_real EmotePlayer::getVariable(tTJSString name)
{
    if (_currentfile != nullptr)
    {
        std::string tmpName = name.AsNarrowStdString();
        tmpName.append(1, '\0');
        auto varPos = _currentfile->_metadata->_varList.find(tmpName);
        if (varPos != _currentfile->_metadata->_varList.end())
        {
            return varPos->second;
        }
    }
    return 0.0;
}
void EmotePlayer::setOuterForce(tTJSString name, tjs_real ofx, tjs_real ofy)
{
    SDL_Log("EmotePlayer::setOuterForce TODO");
}
void EmotePlayer::setDrawAffineTranslateMatrix(
    tjs_real a, tjs_real b, tjs_real c, tjs_real d, tjs_int tx, tjs_int ty)
{
    if (_currentfile != nullptr)
    {
        _affineTrans = glm::mat4(a, -c, 0.0f, 0.0f,
                                 -b, d, 0.0f, 0.0f,
                                 0.0f, 0.0f, 1.0f, 0.0f,
                                 tx, ty, 0.0f, 1.0f);
        updateTransMat();
    }
}
void EmotePlayer::setCameraOffset(tjs_int w, tjs_int h)
{
    currCamX = w;
    currCamY = h;
}
void EmotePlayer::startWind(
    tjs_real start, tjs_real goal, tjs_real speed, tjs_real powMin, tjs_real powMax)
{
    SDL_Log("EmotePlayer::startWind TODO");
}
void EmotePlayer::stopWind()
{
    SDL_Log("EmotePlayer::stopWind TODO");
}
bool EmotePlayer::contains(tTJSString valName, tjs_real x, tjs_real y)
{
    SDL_Log("EmotePlayer::contains TODO");
    return false;
}
void EmotePlayer::skip()
{
    SDL_Log("EmotePlayer::skip TODO");
}
void EmotePlayer::skipToSync()
{
    if (_currmotion != nullptr)
    {
        clockPassed = _currentfile->getSyncTime();
    }
    SDL_Log("EmotePlayer::skipToSync TODO");
}
void EmotePlayer::pass()
{
    SDL_Log("EmotePlayer::pass TODO");
}
void EmotePlayer::stop()
{
    _isStop = true;
    _animating = false;
    _playing = false;
    _allplaying = false;
    SDL_Log("EmotePlayer::stop TODO");
}
void EmotePlayer::playTimeline(tTJSString name, tjs_int flags)
{
    if (_currentfile != nullptr)
    {
        _currentfile->startTimeline(-10000.0f, name.AsStdString(), true);
    }
}
void EmotePlayer::stopTimeline(tTJSString name)
{
    if (_currentfile != nullptr)
    {
        _currentfile->stopTimeline(name.AsStdString(), true);
    }
}
bool EmotePlayer::getTimelinePlaying(tTJSString name)
{
    if (_currentfile != nullptr)
    {
        bool ret = false;
        if (_currentfile->checkTimline(name.AsStdString(), ret, true))
            return ret;
    }
    return false;
}
bool EmotePlayer::getLoopTimeline(tTJSString name)
{
    if (_currentfile != nullptr)
    {
        for (auto itm : _currentfile->_metadata->_timelineControl)
        {
            if (strcmp(itm->label.c_str(), name.AsNarrowStdString().c_str()) == 0)
            {
                return itm->lastTime < 0;
            }
        }
    }
    return false;
}
tjs_real EmotePlayer::getTimelineTotalFrameCount(tTJSString name)
{
    if (_currentfile != nullptr)
    {
        for (auto itm : _currentfile->_metadata->_timelineControl)
        {
            if (strcmp(itm->label.c_str(), name.AsNarrowStdString().c_str()) == 0)
            {
                return itm->loopEnd - itm->loopBegin + 1;
            }
        }
    }
    return 0;
}
tTJSVariant EmotePlayer::getMainTimelineLabelList()
{
    iTJSDispatch2* array = TJSCreateArrayObject();
    if (_currentfile != nullptr)
    {
        for (auto itm : _currentfile->_metadata->_timelineControl)
        {
            if (itm->diff == 0)
            {
                tTJSVariant tmp(ttstr(itm->label));
                tTJSVariant* args[] = {&tmp};
                static tjs_uint addHint = 0;
                array->FuncCall(0, TJS_W("add"), &addHint, nullptr, 1, args, array);
            }
        }
    }
    tTJSVariant result(array, array);
    array->Release();
    return result;
}
tTJSVariant EmotePlayer::getDiffTimelineLabelList()
{
    iTJSDispatch2* array = TJSCreateArrayObject();
    if (_currentfile != nullptr)
    {
        for (auto itm : _currentfile->_metadata->_timelineControl)
        {
            if (itm->diff == 1)
            {
                tTJSVariant tmp(ttstr(itm->label));
                tTJSVariant* args[] = {&tmp};
                static tjs_uint addHint = 0;
                array->FuncCall(0, TJS_W("add"), &addHint, nullptr, 1, args, array);
            }
        }
    }
    tTJSVariant result(array, array);
    array->Release();
    return result;
}
void EmotePlayer::setTimelineBlendRatio(
    tTJSString name, tjs_real ratio, tjs_real time, tjs_real easing)
{
    SDL_Log("EmotePlayer::setTimelineBlendRatio TODO");
}
void EmotePlayer::fadeInTimeline(tTJSString name, tjs_real time, tjs_real easing)
{
    playTimeline(name);
}
void EmotePlayer::fadeOutTimeline(tTJSString name, tjs_real time, tjs_real easing)
{
    stopTimeline(name);
}
tTJSVariant EmotePlayer::getPlayingTimelineInfoList()
{
    iTJSDispatch2* array = TJSCreateArrayObject();
    tTJSVariant result(array, array);
    if (_currentfile != nullptr)
    {
        for (auto playingTimeline : _currentfile->currTimeline)
        {
            iTJSDispatch2* obj = TJSCreateDictionaryObject();
            tTJSVariant val = tTJSVariant(playingTimeline->label);
            obj->PropSet(TJS_MEMBERENSURE, TJS_W("label"), NULL, &val, obj);
            tTJSVariant objItm(obj, obj);
            obj->Release();
            tTJSVariant tmp(objItm);
            tTJSVariant* args[] = {&tmp};
            static tjs_uint addHint = 0;
            array->FuncCall(0, TJS_W("add"), &addHint, nullptr, 1, args, array);
        }
    }
    array->Release();
    return result;
}
tTJSVariant EmotePlayer::getVariableFrameList(tTJSString name)
{
    if (_currentfile != nullptr)
    {
        iTJSDispatch2* root = _currentfile->root().AsObject();
        tTJSVariant itm;
        if (TJS_FAILED(root->PropGet(0, TJS_W("metadata"), NULL, &itm, root)))
        {
            SDL_Log("emotefile donot contain metadata");
            return tTJSVariant();
        }
        root->Release();
        root = itm.AsObject();
        if (TJS_FAILED(root->PropGet(0, TJS_W("variableList"), NULL, &itm, root)))
        {
            SDL_Log("emotefile donot contain variableList");
            return tTJSVariant();
        }
        root->Release();
        root = itm.AsObjectThis();

        tTJSVariant retNeed;
        for (tjs_uint32 i = 0; i < _currentfile->_metadata->_varList.size(); i++)
        {
            tTJSVariant varItem;
            if (TJS_FAILED(root->PropGetByNum(TJS_MEMBERMUSTEXIST, i, &varItem, root)))
                break;
            iTJSDispatch2* rev = varItem.AsObjectThisNoAddRef();
            tTJSVariant labelname;
            if (TJS_FAILED(rev->PropGet(0, TJS_W("label"), NULL, &labelname, rev)))
                continue;
            if (labelname.Type() != tvtString || ttstr(labelname) != name)
                continue;
            if (TJS_FAILED(rev->PropGet(0, TJS_W("frameList"), NULL, &retNeed, rev)))
                continue;
            break;
        }

        root->Release();
        return retNeed;
    }
    else
    {
        iTJSDispatch2* array = TJSCreateArrayObject();
        SDL_Log("EmotePlayer::getVariableFrameList TODO");
        tTJSVariant result(array, array);
        array->Release();
        return result;
    }
}
void EmotePlayer::updateTransMat()
{
    _renderMethod.type = 3;
    _renderMethod.opa = 1.0f;
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, glm::vec3(currCoordx + currCamX, currCoordy + currCamY, 0));
    model = glm::rotate(model, glm::radians(currAngle), glm::vec3(0.0f, 0.0f, 1.0f));
    model = glm::scale(model, glm::vec3(currZx, currZy, 1.0f));
    _renderMethod.matTrans = _affineTrans * model;
}
void EmotePlayer::ResetDrawArea(tjs_int layerWidth, tjs_int layerHeight)
{
    tjs_int renderW = layerWidth;
    tjs_int renderH = layerHeight;
    if (_currentfile != nullptr && _currentfile->_screenSize.width > 0 &&
        _currentfile->_screenSize.height > 0)
    {
        _limitArea.originX = _currentfile->_screenSize.originX;
        _limitArea.originY = _currentfile->_screenSize.originY;
        _limitArea.width = _currentfile->_screenSize.width;
        _limitArea.height = _currentfile->_screenSize.height;
        renderW = _currentfile->_screenSize.width;
        renderH = _currentfile->_screenSize.height;
    }
    else
    {
        _limitArea.originX = 0;
        _limitArea.originY = 0;
        _limitArea.width = layerWidth;
        _limitArea.height = layerHeight;
    }

    if (_width != renderW || _height != renderH)
    {
        _width = renderW;
        _height = renderH;
        if (_currentfile != nullptr)
        {
            _limitArea.zMax = _currentfile->getZMax(true) * 2;
        }
        if (_limitArea.zMax < 30.0f)
            _limitArea.zMax = 30.0f;
        if (m_BmpBits != nullptr)
            delete static_cast<tTVPBaseTexture*>(m_BmpBits);
        m_BmpBits = new tTVPBaseTexture(_width, _height, 32);
        if (m_bmpData != nullptr)
            delete[] m_bmpData;
        m_bmpData = new tjs_uint8[static_cast<size_t>(_width) * static_cast<size_t>(_height) * 4u];
        updateTransMat();
    }
}

} // namespace emoteplayer
