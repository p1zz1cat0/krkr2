//
// E-mote source-reference dialects. "dx_" PSBs split a frame's source into
// content["src"] (the bank) and content["icon"] (the item); the plain family
// packs the same reference into one slash-joined string. These cases pin the
// join so a dx_ reference can never again reach the resolver truncated to its
// bank name, and so the plain family stays byte-for-byte untouched.
//
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "motionplayer/RuntimeSupport.h"

namespace {
    std::shared_ptr<PSB::PSBDictionary> dict() {
        return std::make_shared<PSB::PSBDictionary>();
    }

    void put(const std::shared_ptr<PSB::PSBDictionary> &owner,
             const std::string &key,
             const std::shared_ptr<PSB::IPSBValue> &value) {
        owner->emplace(key, value);
    }

    void putString(const std::shared_ptr<PSB::PSBDictionary> &owner,
                   const std::string &key, const std::string &value) {
        put(owner, key, std::make_shared<PSB::PSBString>(value));
    }

    std::string srcOf(const std::shared_ptr<PSB::PSBDictionary> &content) {
        const auto value = std::dynamic_pointer_cast<PSB::PSBString>(
            (*content)["src"]);
        return value ? value->value : std::string{};
    }

    // A root shaped like the real files: one source bank holding icons, one
    // object group holding motions, and "head_parts" deliberately present in
    // both tables — the plain family really does use that name twice.
    std::shared_ptr<PSB::PSBDictionary> makeRoot() {
        auto texIcons = dict();
        put(texIcons, "19", dict());

        auto texBank = dict();
        put(texBank, "icon", texIcons);

        auto headSourceIcons = dict();
        put(headSourceIcons, "輪郭00", dict());
        auto headSourceBank = dict();
        put(headSourceBank, "icon", headSourceIcons);

        auto sources = dict();
        put(sources, "tex#001", texBank);
        put(sources, "head_parts", headSourceBank);

        auto bodyMotions = dict();
        put(bodyMotions, "全身変形基礎", dict());
        auto bodyGroup = dict();
        put(bodyGroup, "motion", bodyMotions);

        auto headMotions = dict();
        put(headMotions, "頭部変形基礎", dict());
        auto headGroup = dict();
        put(headGroup, "motion", headMotions);

        auto objects = dict();
        put(objects, "body_parts", bodyGroup);
        put(objects, "head_parts", headGroup);

        auto root = dict();
        put(root, "source", sources);
        put(root, "object", objects);
        return root;
    }

    // Attach a frame content dict the way the PSB nests it: the content sits
    // under a frameList entry, so normalisation has to descend through a list.
    std::shared_ptr<PSB::PSBDictionary>
    attachContent(const std::shared_ptr<PSB::PSBDictionary> &root,
                  const std::string &layerLabel) {
        auto content = dict();
        auto frame = dict();
        put(frame, "content", content);
        auto frameList = std::make_shared<PSB::PSBList>(1);
        frameList->push_back(frame);
        auto layer = dict();
        putString(layer, "label", layerLabel);
        put(layer, "frameList", frameList);

        auto layers = std::make_shared<PSB::PSBList>(1);
        layers->push_back(layer);
        put(root, "layer", layers);
        return content;
    }
} // namespace

TEST_CASE("dx_ image reference joins into the plain src/<bank>/<icon> form") {
    auto root = makeRoot();
    auto content = attachContent(root, "■輪郭");
    putString(content, "src", "tex#001");
    putString(content, "icon", "19");

    motion::detail::normalizeSplitSourceReferences(root);

    REQUIRE(srcOf(content) == "src/tex#001/19");
}

TEST_CASE("dx_ motion reference joins into the plain motion/<group>/<label> "
          "form") {
    auto root = makeRoot();
    auto content = attachContent(root, "全身変形基礎");
    putString(content, "src", "body_parts");
    putString(content, "icon", "全身変形基礎");

    motion::detail::normalizeSplitSourceReferences(root);

    // The bare bank name is what used to reach resolveMotion as the storage
    // path "motion/body_parts/body_parts" and throw.
    REQUIRE(srcOf(content) == "motion/body_parts/全身変形基礎");
}

TEST_CASE("a bank named in both tables is disambiguated by the item") {
    SECTION("item that exists as a source icon resolves to the source form") {
        auto root = makeRoot();
        auto content = attachContent(root, "■輪郭");
        putString(content, "src", "head_parts");
        putString(content, "icon", "輪郭00");

        motion::detail::normalizeSplitSourceReferences(root);

        REQUIRE(srcOf(content) == "src/head_parts/輪郭00");
    }

    SECTION("item that exists as a group motion resolves to the motion form") {
        auto root = makeRoot();
        auto content = attachContent(root, "頭部変形基礎");
        putString(content, "src", "head_parts");
        putString(content, "icon", "頭部変形基礎");

        motion::detail::normalizeSplitSourceReferences(root);

        REQUIRE(srcOf(content) == "motion/head_parts/頭部変形基礎");
    }
}

TEST_CASE("a placeholder bank with no table entry joins unprefixed") {
    auto root = makeRoot();
    auto content = attachContent(root, "UD");
    putString(content, "src", "blank");
    putString(content, "icon", "331:432:165:216");

    motion::detail::normalizeSplitSourceReferences(root);

    REQUIRE(srcOf(content) == "blank/331:432:165:216");
}

TEST_CASE("plain-family references are left exactly as authored") {
    auto root = makeRoot();

    SECTION("no icon key at all") {
        auto content = attachContent(root, "■輪郭");
        putString(content, "src", "src/head_parts/輪郭00");

        motion::detail::normalizeSplitSourceReferences(root);

        REQUIRE(srcOf(content) == "src/head_parts/輪郭00");
    }

    SECTION("an empty icon does not truncate or rewrite the reference") {
        auto content = attachContent(root, "全身変形基礎");
        putString(content, "src", "motion/body_parts/全身変形基礎");
        putString(content, "icon", "");

        motion::detail::normalizeSplitSourceReferences(root);

        REQUIRE(srcOf(content) == "motion/body_parts/全身変形基礎");
    }
}

TEST_CASE("joining is idempotent across repeated normalisation") {
    auto root = makeRoot();
    auto content = attachContent(root, "全身変形基礎");
    putString(content, "src", "body_parts");
    putString(content, "icon", "全身変形基礎");

    motion::detail::normalizeSplitSourceReferences(root);
    motion::detail::normalizeSplitSourceReferences(root);

    REQUIRE(srcOf(content) == "motion/body_parts/全身変形基礎");
}

TEST_CASE("normalising a root without source or object tables is a no-op") {
    auto root = dict();
    auto content = attachContent(root, "UD");
    putString(content, "src", "blank");
    putString(content, "icon", "20:20:10:10");

    motion::detail::normalizeSplitSourceReferences(root);

    REQUIRE(srcOf(content) == "blank/20:20:10:10");
}

TEST_CASE("a null root is tolerated") {
    REQUIRE_NOTHROW(motion::detail::normalizeSplitSourceReferences(nullptr));
}
