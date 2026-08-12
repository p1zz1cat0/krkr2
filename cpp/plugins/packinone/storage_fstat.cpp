// PackinOne.dll compatible StoragesFstat class.
//
// fstat.dll attaches a broader API to the global Storages class.  PackinOne
// additionally exports an instantiable StoragesFstat class with a different
// method surface; games are allowed to depend on both, so capability aliases
// on Storages are not sufficient here.

#include "packinone.h"

#include "Platform.h"
#include "StorageIntf.h"

#include <chrono>
#include <filesystem>
#include <system_error>

#if defined(__APPLE__)
#include <sys/attr.h>
#include <sys/stat.h>
#endif

#include <spdlog/spdlog.h>

#define NCB_MODULE_NAME TJS_W("packinone.dll")

extern "C" void TVPPackinOneStorageFstatAnchor() {}

namespace {
namespace fs = std::filesystem;

void LogFileError(const char *operation, const fs::path &path,
                  const std::error_code &error) {
    if(auto logger = spdlog::get("plugin")) {
        logger->error("[packinone] StoragesFstat.{} failed for {}: {}",
                      operation, path.string(), error.message());
    }
}

fs::path ResolveLocalPath(const ttstr &storageName) {
    return PackinOneLocalPath(storageName);
}

ttstr ParentStorageName(const ttstr &storageName) {
    const tjs_char *characters = storageName.c_str();
    const tjs_int length = storageName.GetLen();
    for(tjs_int index = length - 1; index >= 0; --index) {
        const tjs_char character = characters[index];
        if(character == TJS_W('/') || character == TJS_W('\\') ||
           character == TVPArchiveDelimiter) {
            return ttstr(characters, static_cast<size_t>(index + 1));
        }
    }
    return ttstr();
}

tjs_int64 FileTimeToMilliseconds(fs::file_time_type value) {
    const auto systemTime = std::chrono::time_point_cast<
        std::chrono::system_clock::duration>(
        value - fs::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               systemTime.time_since_epoch())
        .count();
}

#if defined(__APPLE__)
tjs_int64 TimespecToMilliseconds(const timespec &value) {
    return static_cast<tjs_int64>(value.tv_sec) * 1000 +
           static_cast<tjs_int64>(value.tv_nsec) / 1000000;
}
#endif

class PackinOneStoragesFstat {
    ttstr CurrentDirectory;

    static bool IsReadOnlyImpl(const fs::path &path, std::error_code &error) {
        const fs::perms permissions = fs::status(path, error).permissions();
        if(error)
            return false;
        return (permissions & (fs::perms::owner_write |
                               fs::perms::group_write |
                               fs::perms::others_write)) == fs::perms::none;
    }

    static tjs_int64 VariantTimeMilliseconds(const tTJSVariant &value) {
        if(value.Type() != tvtObject)
            return static_cast<tjs_int64>(value.AsInteger());

        iTJSDispatch2 *object = value.AsObjectNoAddRef();
        if(!object)
            return 0;
        tTJSVariant result;
        if(TJS_FAILED(object->FuncCall(0, TJS_W("getTime"), nullptr,
                                       &result, 0, nullptr, object)))
            return 0;
        return static_cast<tjs_int64>(result.AsInteger());
    }

public:
    PackinOneStoragesFstat() = default;

    tjs_int64 getFileSize(ttstr path) {
        std::error_code error;
        const fs::path local = ResolveLocalPath(path);
        const std::uintmax_t size = fs::file_size(local, error);
        if(error) {
            LogFileError("getFileSize", local, error);
            return 0;
        }
        return static_cast<tjs_int64>(size);
    }

    bool isExistent(ttstr path) {
        std::error_code error;
        const fs::path local = ResolveLocalPath(path);
        const bool exists = fs::exists(local, error);
        if(error)
            LogFileError("isExistent", local, error);
        return exists && !error;
    }

    bool isDirectory(ttstr path) {
        std::error_code error;
        const fs::path local = ResolveLocalPath(path);
        const bool directory = fs::is_directory(local, error);
        if(error)
            LogFileError("isDirectory", local, error);
        return directory && !error;
    }

    static tjs_error IsReadOnly(tTJSVariant *result, tjs_int numparams,
                                tTJSVariant **param, iTJSDispatch2 *) {
        if(numparams < 1)
            return TJS_E_BADPARAMCOUNT;

        const fs::path local = ResolveLocalPath(ttstr(*param[0]));
        std::error_code error;
        if(numparams >= 2 && param[1]->Type() != tvtVoid) {
            const bool readOnly = param[1]->operator bool();
            const fs::perms writeBits = fs::perms::owner_write |
                                        fs::perms::group_write |
                                        fs::perms::others_write;
            fs::permissions(local, writeBits,
                            readOnly ? fs::perm_options::remove
                                     : fs::perm_options::add,
                            error);
            if(error)
                LogFileError("isReadOnly(set)", local, error);
        }

        const bool readOnly = IsReadOnlyImpl(local, error);
        if(error)
            LogFileError("isReadOnly(get)", local, error);
        if(result)
            *result = static_cast<tjs_int>(readOnly && !error);
        return TJS_S_OK;
    }

    bool rename(ttstr from, ttstr to) {
        const fs::path source = ResolveLocalPath(from);
        const fs::path destination = ResolveLocalPath(to);
        std::error_code error;
        fs::rename(source, destination, error);
        if(error)
            LogFileError("rename", source, error);
        else
            TVPClearStorageCaches();
        return !error;
    }

    bool remove(ttstr path) {
        const fs::path local = ResolveLocalPath(path);
        std::error_code error;
        const bool removed = fs::remove(local, error);
        if(error)
            LogFileError("remove", local, error);
        else if(removed)
            TVPClearStorageCaches();
        return removed && !error;
    }

    tjs_int64 getCreationTime(ttstr path) {
        const fs::path local = ResolveLocalPath(path);
#if defined(__APPLE__)
        struct stat status {};
        if(::stat(local.c_str(), &status) == 0)
            return TimespecToMilliseconds(status.st_birthtimespec);
#endif
        std::error_code error;
        const fs::file_time_type time = fs::last_write_time(local, error);
        if(error) {
            LogFileError("getCreationTime", local, error);
            return 0;
        }
        return FileTimeToMilliseconds(time);
    }

    static tjs_error SetCreationTime(tTJSVariant *result, tjs_int numparams,
                                     tTJSVariant **param, iTJSDispatch2 *) {
        if(numparams < 2)
            return TJS_E_BADPARAMCOUNT;
        const fs::path local = ResolveLocalPath(ttstr(*param[0]));
        const tjs_int64 milliseconds = VariantTimeMilliseconds(*param[1]);
        bool succeeded = false;
#if defined(__APPLE__)
        struct attrlist attributes {};
        attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
        attributes.commonattr = ATTR_CMN_CRTIME;
        struct timespec creationTime {
            static_cast<time_t>(milliseconds / 1000),
            static_cast<long>((milliseconds % 1000) * 1000000)
        };
        succeeded = ::setattrlist(local.c_str(), &attributes, &creationTime,
                                  sizeof(creationTime), 0) == 0;
#else
        (void)local;
        (void)milliseconds;
#endif
        if(!succeeded) {
            if(auto logger = spdlog::get("plugin"))
                logger->error("[packinone] StoragesFstat.setCreationTime failed for {}",
                              local.string());
        }
        if(result)
            *result = static_cast<tjs_int>(succeeded);
        return TJS_S_OK;
    }

    tjs_int64 getLastAccessTime(ttstr path) {
        const fs::path local = ResolveLocalPath(path);
#if defined(__APPLE__)
        struct stat status {};
        if(::stat(local.c_str(), &status) == 0)
            return TimespecToMilliseconds(status.st_atimespec);
#endif
        std::error_code error;
        const fs::file_time_type time = fs::last_write_time(local, error);
        if(error) {
            LogFileError("getLastAccessTime", local, error);
            return 0;
        }
        return FileTimeToMilliseconds(time);
    }

    bool copy(ttstr from, ttstr to, bool overwrite) {
        const fs::path source = ResolveLocalPath(from);
        const fs::path destination = ResolveLocalPath(to);
        std::error_code error;
        const fs::copy_options options = overwrite
            ? fs::copy_options::overwrite_existing
            : fs::copy_options::none;
        const bool copied = fs::copy_file(source, destination, options, error);
        if(error)
            LogFileError("copy", source, error);
        else if(copied)
            TVPClearStorageCaches();
        return copied && !error;
    }

    ttstr getCurrentDirectory() const {
        if(!CurrentDirectory.IsEmpty())
            return CurrentDirectory;
        return ParentStorageName(TVPGetPlacedPath(TJS_W("startup.tjs")));
    }

    void setCurrentDirectory(ttstr path) {
        path = TVPNormalizeStorageName(path);
        TVPSetCurrentDirectory(path);
        CurrentDirectory = path;
    }
};

} // namespace

NCB_REGISTER_CLASS_DIFFER(StoragesFstat, PackinOneStoragesFstat) {
    Constructor();
    NCB_METHOD(getFileSize);
    NCB_METHOD(isExistent);
    NCB_METHOD(isDirectory);
    RawCallback("isReadOnly", &Class::IsReadOnly, 0);
    NCB_METHOD(rename);
    NCB_METHOD(remove);
    NCB_METHOD(getCreationTime);
    RawCallback("setCreationTime", &Class::SetCreationTime, 0);
    NCB_METHOD(getLastAccessTime);
    NCB_METHOD(copy);
    Property(TJS_W("currentDirectory"), &Class::getCurrentDirectory,
             &Class::setCurrentDirectory);
}
