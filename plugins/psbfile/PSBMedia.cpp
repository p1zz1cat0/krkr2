//
// Created by LiDon on 2025/9/11.
//

#include <spdlog/spdlog.h>

#include "PSBMedia.h"
#include "PSBMediaRegistry.h"

#include "MsgIntf.h"
#include "UtilStreams.h"

namespace PSB {
#define LOGGER spdlog::get("plugin")

    // Scripts build "psb://" + storageName.toLowerCase() + "/...".
    // Registration and lookup both come through here, so use one case.
    void PSBMedia::NormalizeDomainName(ttstr &name) {
        name = name.AsLowerCase();
    }

    void PSBMedia::NormalizePathName(ttstr &name) {
        // province(_p), mask(_m)
    }

    bool PSBMedia::CheckExistentStorage(const ttstr &name) {
        if(_resources.count(name.AsStdString()))
            return true;
        loadContainerOnce(name);
        return _resources.count(name.AsStdString()) != 0;
    }

    // A save can restore "psb://conf.pimg/N.tlg" before any script loads
    // conf.pimg. Find the container by its domain name and register it.
    // Try each domain one time only.
    void PSBMedia::loadContainerOnce(const ttstr &name) {
        tjs_int slash = name.IndexOf(TJS_W('/'));
        if(slash <= 0)
            return;
        ttstr domain = name.SubString(0, slash);
        if(!_triedContainers.insert(domain.AsStdString()).second)
            return;
        try {
            if(!TVPIsExistentStorage(domain))
                return;
            PSBFile file;
            if(!file.loadPSBFile(domain) || file.getObjects() == nullptr) {
                LOGGER->error("PSBMedia: cannot load container {}",
                              domain.AsStdString());
                return;
            }
            registerRootResources(domain, file);
            LOGGER->info("PSBMedia: loaded container {} on demand",
                         domain.AsStdString());
        } catch(const std::exception &error) {
            LOGGER->error("PSBMedia: container {} failed: {}",
                          domain.AsStdString(), error.what());
        } catch(...) {
            LOGGER->error("PSBMedia: container {} failed",
                          domain.AsStdString());
        }
    }

    tTJSBinaryStream *PSBMedia::Open(const ttstr &name, tjs_uint32 flags) {
        if(!CheckExistentStorage(name))
            TVPThrowExceptionMessage(TVPCannotOpenStorage, name);
        auto memoryStream = new tTVPMemoryStream();
        auto res = _resources[name.AsStdString()];
        memoryStream->WriteBuffer(res.data.data(), res.data.size());
        memoryStream->Seek(0, TJS_BS_SEEK_SET);
        return memoryStream;
    }

    void PSBMedia::GetListAt(const ttstr &name, iTVPStorageLister *lister) {
        LOGGER->error("TODO: PSBMedia GetListAt");
    }

    void PSBMedia::GetLocallyAccessibleName(ttstr &name) {
        LOGGER->error("can't get GetLocallyAccessibleName from {}!",
                      name.AsStdString());
    }

    void PSBMedia::add(const std::string &name,
                       const std::shared_ptr<PSBResource> &resource) {
        this->_resources[name] = *resource;
    }
} // namespace PSB