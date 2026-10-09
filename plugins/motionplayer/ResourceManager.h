//
// Created by LiDon on 2025/9/15.
//
#pragma once
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "tjs.h"

namespace motion {

    class ResourceManager {
    public:
        ResourceManager();

        explicit ResourceManager(iTJSDispatch2 *kag, tjs_int cacheSize);

        ~ResourceManager();

        tTJSVariant load(ttstr path) const;
        tTJSVariant loadSource(ttstr path) const;
        void unload(ttstr path) const;
        void clearCache() const;
        tTJSVariant getLastLoadedModule() const;
        tTJSVariant findLoaded(ttstr path) const;
        // 参考 sdl3/ GetPlayerByName（不编译）: placed/raw/lzf 路径
        tTJSVariant findLoadedModule(ttstr path) const;
        tTJSVariant findSource(ttstr path) const;
        [[nodiscard]] std::size_t uniqueCachedModuleCount() const;
        struct CachedModuleEntry {
            std::string key;
            tTJSVariant module;
            std::uint64_t loadGeneration = 0;
        };
        [[nodiscard]] std::vector<CachedModuleEntry>
        uniqueCachedModules() const;
        tjs_int requireLayerId();
        tjs_int requireLayerIdForName(ttstr name);
        void releaseLayerId(tjs_int id);
        [[nodiscard]] static tjs_int getEmotePSBDecryptSeed();
        [[nodiscard]] static std::uint64_t getEmotePSBDecryptGeneration();

        static tjs_error setEmotePSBDecryptSeed(tTJSVariant *r, tjs_int count,
                                                tTJSVariant **p,
                                                iTJSDispatch2 *obj);

        static tjs_error setEmotePSBDecryptFunc(tTJSVariant *r, tjs_int n,
                                                tTJSVariant **p,
                                                iTJSDispatch2 *obj);

        // Install the current decrypt callback on PSBFile before loading.
        [[nodiscard]] static tTJSVariantClosure
        getEmotePSBDecryptFunc() {
            return _decryptFunc;
        }

        // unloadAll releases all entries cached by this State. The shared snapshot
        // leaves the global registry when its last State owner is released.
        void unloadAll() const { clearCache(); }

    private:
        struct State {
            std::unordered_map<std::string, tTJSVariant> loadedModules;
            // One ownership token per shared State, not per ResourceManager
            // value. Player/NCB wrappers copy this State intentionally.
            std::unordered_set<iTJSDispatch2 *> ownedSnapshotModules;
            std::unordered_map<iTJSDispatch2 *, std::uint64_t>
                moduleLoadGenerations;
            std::uint64_t nextLoadGeneration = 0;
            std::uint64_t decryptConfigGeneration = 0;
            std::string lastLoadedPath;
            tTJSVariant lastLoadedModule;
            std::unordered_map<std::string, tjs_int> layerIdsByName;
            std::unordered_map<tjs_int, std::string> layerNamesById;
            std::unordered_set<tjs_int> usedLayerIds;
            tjs_int nextLayerId = 1;
            tjs_int cacheSize = 20 * 1024 * 1024;
        };

        std::shared_ptr<State> _state;
        inline static int _decryptSeed = 0;
        inline static std::uint64_t _decryptConfigGeneration = 0;
        inline static tTJSVariantClosure _decryptFunc{};
    };
} // namespace motion
