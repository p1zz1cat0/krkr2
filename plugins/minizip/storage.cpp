// minizip.dll: "zip" storage media (Storages.mountZip / unmountZip).
// Ported from wamsoft/minizip storage.cpp (zlib license). See minizip.cpp.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>

#include "UtilStreams.h"

#define NCB_MODULE_NAME TJS_W("minizip.dll")
#include "ncbind.hpp"
#include <map>
#include <vector>

extern "C" {
#include "mz.h"
#include "mz_strm.h"
#include "mz_zip.h"
#include "mz_zip_rw.h"
}

#include "ioapi.h"
#include "narrow.h"


#define CASESENSITIVITY (0)

#define BASENAME TJS_W("zip")

extern void storeFilename(ttstr &name, const char *narrowName, bool utf8);

/**
 * Zip 展開処理クラス
 *
 * mz_zip_reader はエントリ単位での順次読み込みのみ対応しているため、
 * lock/unlock で 1 つの reader を直列化して使う。
 * (krkr2 port: entries are copied to memory in openMemoryStream.)
 */
class UnzipBase {

public:
    UnzipBase() : refCount(1), zr(NULL), zrstream(NULL), zipfile(), utf8(false), entryOpen(false) {
    }

    void AddRef() {
        refCount++;
    };

    void Release() {
        if (refCount == 1) {
            delete this;
        } else {
            refCount--;
        }
    };

    /**
     * ZIPファイルを開く
     * @param filename ファイル名
     */
    bool init(const ttstr &filename) {
        done();
        zipfile = filename;

        if (!openReader()) return false;

        // UTF8 判定 + ディレクトリエントリ収集
        lock();
        int err = mz_zip_reader_goto_first_entry(zr);
        bool first = true;
        while (err == MZ_OK) {
            mz_zip_file *file_info = NULL;
            if (mz_zip_reader_entry_get_info(zr, &file_info) == MZ_OK && file_info) {
                if (first) {
                    utf8 = (file_info->flag & MZ_ZIP_FLAG_UTF8) != 0;
                    first = false;
                }
                ttstr name;
                storeFilename(name, file_info->filename ? file_info->filename : "", utf8);
                entryName(name);
            }
            err = mz_zip_reader_goto_next_entry(zr);
        }
        unlock();
        return true;
    }

    /**
     * Decompress one entry into a memory stream. Return NULL if not found.
     *
     * krkr2 port: the original keeps the shared reader positioned in an open
     * entry for the stream's whole life. Any other lookup on the same zip
     * (for example SQLite's journal check through CheckExistentStorage) then
     * closes that entry and later reads return 0. A memory copy keeps each
     * stream independent, makes seeks cheap, and holds the lock only here.
     */
    tTJSBinaryStream *openMemoryStream(const ttstr &srcname) {
        std::lock_guard<std::recursive_mutex> guard(cs);
        if (!zr) return NULL;
        closeEntry();
        NarrowString nname(srcname, utf8);
        if (mz_zip_reader_locate_entry(zr, (const char*)nname, CASESENSITIVITY ? 0 : 1) != MZ_OK)
            return NULL;
        if (mz_zip_reader_entry_open(zr) != MZ_OK)
            return NULL;
        entryOpen = true;
        // ponytail: whole entry in memory, 4 GiB limit (tTVPMemoryStream size
        // is tjs_uint). Stream from the reader if large entries matter.
        tTVPMemoryStream *mem = new tTVPMemoryStream();
        char buf[16384];
        int32_t r;
        while ((r = mz_zip_reader_entry_read(zr, buf, (int32_t)sizeof buf)) > 0) {
            mem->Write(buf, (tjs_uint)r);
        }
        closeEntry();
        if (r < 0) {
            delete mem;
            return NULL;
        }
        mem->Seek(0, TJS_BS_SEEK_SET);
        return mem;
    }

    bool CheckExistentStorage(const ttstr &name) {
        bool ret = false;
        if (zr) {
            lock();
            closeEntry();
            NarrowString nname(name, utf8);
            ret = mz_zip_reader_locate_entry(zr, (const char*)nname, CASESENSITIVITY ? 0 : 1) == MZ_OK;
            unlock();
        }
        return ret;
    }

    void GetListAt(const ttstr &name, iTVPStorageLister *lister) {
        ttstr fname = TJS_W("/");
        fname += name;
        std::map<ttstr, FileNameList>::const_iterator it = dirEntryTable.find(fname);
        if (it != dirEntryTable.end()) {
            std::vector<ttstr>::const_iterator fit = it->second.begin();
            while (fit != it->second.end()) {
                lister->Add(*fit);
                fit++;
            }
        }
    }

    bool isUtf8() const { return utf8; }

protected:

    virtual ~UnzipBase() {
        done();
    }

    bool openReader() {
        tTJSBinaryStream *bs = NULL;
        try {
            bs = TVPCreateStream(zipfile, TJS_BS_READ);
        } catch(...) {
            bs = NULL;
        }
        if (!bs) return false;

        zrstream = mz_stream_tvp_create();
        mz_stream_tvp_attach(zrstream, bs, 1);

        zr = mz_zip_reader_create();
        if (mz_zip_reader_open(zr, zrstream) != MZ_OK) {
            mz_zip_reader_delete(&zr);
            mz_stream_tvp_delete(&zrstream);
            return false;
        }
        return true;
    }

    void done() {
        if (zr) {
            mz_zip_reader_close(zr);
            mz_zip_reader_delete(&zr);
            zr = NULL;
        }
        if (zrstream) {
            mz_stream_close(zrstream);
            mz_stream_tvp_delete(&zrstream);
            zrstream = NULL;
        }
    }

    void closeEntry() {
        if (zr && entryOpen) {
            mz_zip_reader_entry_close(zr);
            entryOpen = false;
        }
    }

    // krkr2 port: the Windows original uses a CRITICAL_SECTION and is a no-op
    // on other platforms. Async loader threads read storages too, so lock on
    // every platform. The mutex is recursive, as a CRITICAL_SECTION is.
    void lock() {
        cs.lock();
    }

    void unlock() {
        cs.unlock();
    }

    static const tjs_char *lastSlash(const tjs_char *p) {
        const tjs_char *q = NULL;
        for (; *p; ++p) {
            if (*p == TJS_W('/')) q = p;
        }
        return q;
    }

    void entryName(const ttstr &name) {
        ttstr dname = TJS_W("/");
        ttstr fname;
        const tjs_char *p = name.c_str();
        const tjs_char *q;
        if ((q = lastSlash(p))) {
            dname += ttstr(p, q-p+1);
            fname = ttstr(q+1);
        } else {
            fname = name;
        }
        dirEntryTable[dname].push_back(fname);
    }

private:
    int refCount;
    void *zr;
    void *zrstream;
    ttstr zipfile;
    bool utf8;
    bool entryOpen;
    std::recursive_mutex cs;

    typedef std::vector<ttstr> FileNameList;
    std::map<ttstr, FileNameList> dirEntryTable;

};

/**
 * ZIPストレージ
 */
class ZipStorage : public iTVPStorageMedia
{

public:
    ZipStorage() : refCount(1) {
    }

    virtual ~ZipStorage() {
        std::map<ttstr, UnzipBase*>::iterator it = unzipTable.begin();
        while (it != unzipTable.end()) {
            it->second->Release();
            it = unzipTable.erase(it);
        }
    }

public:
    // -----------------------------------
    // iTVPStorageMedia Interfaces
    // -----------------------------------

    virtual void AddRef() {
        refCount++;
    };

    virtual void Release() {
        if (refCount == 1) {
            delete this;
        } else {
            refCount--;
        }
    };

    virtual void GetName(ttstr &name) {
        name = BASENAME;
    }

    virtual void NormalizeDomainName(ttstr &name) {
    }

    virtual void NormalizePathName(ttstr &name) {
    }

    virtual bool CheckExistentStorage(const ttstr &name) {
        ttstr fname;
        UnzipBase *unzip = getUnzip(name, fname);
        return unzip ? unzip->CheckExistentStorage(fname) : false;
    }

    virtual tTJSBinaryStream * Open(const ttstr & name, tjs_uint32 flags) {
        if (flags == TJS_BS_READ) {
            ttstr fname;
            UnzipBase *unzip = getUnzip(name, fname);
            if (unzip) {
                tTJSBinaryStream *stream = unzip->openMemoryStream(fname);
                if (stream) return stream;
            }
        }
        TVPThrowExceptionMessage(TJS_W("%1:cannot open zipfile"), name);
        return NULL;
    }

    virtual void GetListAt(const ttstr &name, iTVPStorageLister * lister) {
        ttstr fname;
        UnzipBase *unzip = getUnzip(name, fname);
        if (unzip) {
            unzip->GetListAt(fname, lister);
        }
    }

    virtual void GetLocallyAccessibleName(ttstr &name) {
        name = "";
    }

public:

    /**
     * zipファイルをファイルシステムとして mount します
     * zip://ドメイン名/ファイル名 でアクセス可能になります。読み込み専用になります。
     * @param name ドメイン名
     * @param zipfile マウントするZIPファイル名
     * @return マウントに成功したら true
     */
    bool mount(const ttstr &name, const ttstr &zipfile) {
        unmount(name);
        UnzipBase *newUnzip = new UnzipBase();
        if (newUnzip) {
            if (newUnzip->init(zipfile)) {
                unzipTable[name] = newUnzip;
                return true;
            } else {
                newUnzip->Release();
            }
        }
        return false;
    }

    /**
     * zipファイルを unmount します
     * @param name ドメイン名
     * @return アンマウントに成功したら true
     */
    bool unmount(const ttstr &name) {
        std::map<ttstr, UnzipBase*>::iterator it = unzipTable.find(name);
        if (it != unzipTable.end()) {
            it->second->Release();
            unzipTable.erase(it);
            return true;
        }
        return false;
    }

protected:

    UnzipBase *getUnzip(const ttstr &name, ttstr &fname) {
        ttstr dname;
        const tjs_char *p = name.c_str();
        const tjs_char *q;
        if ((q = TJS_strchr(p, TJS_W('/')))) {
            dname = ttstr(p, q-p);
            fname = ttstr(q+1);
        } else {
            TVPThrowExceptionMessage(TJS_W("invalid path:%1"), name);
        }
        std::map<ttstr, UnzipBase*>::const_iterator it = unzipTable.find(dname);
        if (it != unzipTable.end()) {
            return it->second;
        }
        return NULL;
    }

private:
    tjs_uint refCount;
    std::map<ttstr, UnzipBase*> unzipTable;
};


/**
 * メソッド追加用
 */
class StoragesZip {

public:

    static void init() {
        if (zip == NULL) {
            zip = new ZipStorage();
            TVPRegisterStorageMedia(zip);
        }
    }

    static void done() {
        if (zip != NULL) {
            TVPUnregisterStorageMedia(zip);
            zip->Release();
            zip = NULL;
        }
    }

    static bool mountZip(const tjs_char *name, const tjs_char *zipfile) {
        if (zip) {
            return zip->mount(ttstr(name), ttstr(zipfile));
        }
        return false;
    }

    static bool unmountZip(const tjs_char *name) {
        if (zip) {
            return zip->unmount(ttstr(name));
        }
        return false;
    }

protected:
    static ZipStorage *zip;
};

ZipStorage *StoragesZip::zip = NULL;

NCB_ATTACH_CLASS(StoragesZip, Storages) {
    NCB_METHOD(mountZip);
    NCB_METHOD(unmountZip);
};

void initZipStorage()
{
    StoragesZip::init();
}

void doneZipStorage()
{
    StoragesZip::done();
}
