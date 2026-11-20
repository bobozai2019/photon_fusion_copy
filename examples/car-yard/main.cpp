#include "scene.hpp"

#include <iostream>

namespace {

class YardApplication final : public cocos2d::Application {
public:
    explicit YardApplication(yard::Options options) : options_(std::move(options)) {}

    void initGLContextAttrs() override
    {
        GLContextAttrs attrs{8, 8, 8, 8, 24, 8, 0};
        cocos2d::GLView::setGLContextAttrs(attrs);
    }

    bool applicationDidFinishLaunching() override
    {
        using namespace cocos2d;
        auto director = Director::getInstance();
        auto view = GLViewImpl::createWithRect(options_.host ? "Netcode Car Yard - Host" : "Netcode Car Yard - Guest",
                                              Rect(60, 70, 960, 640));
        if (!view) return false;
        if (options_.trace) {
            std::cout << "renderer=" << glGetString(GL_RENDERER) << " version="
                      << glGetString(GL_VERSION) << std::endl;
        }
        director->setOpenGLView(view);
        view->setDesignResolutionSize(960, 640, ResolutionPolicy::SHOW_ALL);
        director->setAnimationInterval(1.0f / 60.0f);
        if (options_.smoke || options_.binding_smoke) {
            auto scene = Scene::create();
            scene->addChild(LayerColor::create(Color4B(29, 39, 46, 255)));
            scene->scheduleOnce([this, scene](float) {
                if (options_.binding_smoke) {
                    const yard::Layout layout(options_.assets / "replicators.cfg");
                    const bool passed = yard::cocos::verify_bindings(layout, *scene);
                    std::cout << "Cocos 2D/3D transform, independent fields, child replacement, deletion "
                              << (passed ? "passed" : "failed") << std::endl;
                    quit(passed ? 0 : 1);
                } else {
                    std::cout << "Cocos2d-x scene rendered" << std::endl;
                    quit(0);
                }
            }, 0.5f, "smoke");
            director->runWithScene(scene);
            return true;
        }
        runner_ = std::make_unique<yard::Runner>(options_);
        scene_ = yard::cocos::YardScene::create(*runner_, [this](int status) { quit(status); });
        if (!scene_) return false;
        director->runWithScene(scene_);
        return true;
    }

    void applicationDidEnterBackground() override { if (scene_) scene_->clear_input(); }
    void applicationWillEnterForeground() override {}

    int status() const { return status_; }

private:
    void quit(int status)
    {
        status_ = status;
        scene_ = nullptr;
        cocos2d::Director::getInstance()->end();
    }

    yard::Options options_;
    std::unique_ptr<yard::Runner> runner_;
    yard::cocos::YardScene* scene_ = nullptr;
    int status_ = 0;
};

}

int main(int argc, char** argv)
{
    try {
        YardApplication app(yard::parse_options(argc, argv));
        const auto error = app.run();
        return error ? error : app.status();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
