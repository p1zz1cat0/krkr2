#include <memory>
#include <cstring>
#include <cocos2d.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "environ/cocos2d/AppDelegate.h"
#include "RendererSelfTest.h"
#include "ui/MainFileSelectorForm.h"

USING_NS_CC;

extern "C" void YoghourtApplyPresentation(void);

int main(int argc, char *argv[]) {
    YoghourtApplyPresentation();

    if (argc == 2 && std::strcmp(argv[1], "--renderer-self-test") == 0) {
        return YoghourtRunRendererSelfTest();
    }

    spdlog::set_level(spdlog::level::info);

    static auto core_logger = spdlog::stdout_color_mt("core");
    static auto tjs2_logger = spdlog::stdout_color_mt("tjs2");
    static auto plugin_logger = spdlog::stdout_color_mt("plugin");
    // 将输入的参数也就是输入文件转为wstring
    if(argc > 1) {
        TVPMainFileSelectorForm::filePath = argv[1];
    }
    spdlog::set_default_logger(core_logger);

    static std::unique_ptr<TVPAppDelegate> pAppDelegate =
        std::make_unique<TVPAppDelegate>();
    return pAppDelegate->run();
}
