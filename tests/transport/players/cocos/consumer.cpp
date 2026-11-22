#include "game_component.hpp"
#include "build_info.hpp"
#include "cocos2d.h"
#include "platform/CCRuntime.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
using namespace netcode;
using namespace std::chrono_literals;
using netcode::cocos::GameComponent;

const Values fields{int32_t(42), int32_t(11), int32_t(3), int32_t(5), int32_t(0), int32_t(7),
                    int32_t(-4), int32_t(16777219)};

class Consumer final : public cocos2d::Application {
public:
    Consumer(int argc, char** argv)
        : args_(argv + 1, argv + argc), exe_(std::filesystem::absolute(argv[0]).parent_path()) {}

    bool applicationDidFinishLaunching() override {
        using namespace cocos2d;
        auto* director = Director::getInstance();
        director->setOpenGLView(GLView::createHeadless("Transport consumer", Size(480, 270)));
        director->setAnimationInterval(1.0 / 60);
        auto* scene = Scene::create();
        SessionOptions options;
        host_ = options.host = has("--host");
        options.extended_join = true;
        options.admission = {2, true, true};
        ticket_ = std::filesystem::u8path(arg("--ticket"));
        resuming_ = has("--resume");
        if (resuming_) {
            std::ifstream input(ticket_, std::ios::binary);
            const Bytes bytes(std::istreambuf_iterator<char>(input), {});
            options.resume = decode_resume_ticket(bytes.data(), bytes.size());
        }
        const ReplicationLayout layout(exe_ / "consumer.cfg");
        const ProviderConfig transport{arg("--provider", "tcp"), std::filesystem::u8path(arg("--module")), {}};
        const auto expected = arg("--expect-error");
        try {
            component_ = GameComponent::create_with_provider(arg("--endpoint"), options, layout, transport);
        } catch (const std::exception& error) {
            if (expected.empty() || std::string_view(error.what()).find(expected) == std::string_view::npos)
                throw;
            std::cout << "OPEN_ERROR " << error.what() << std::endl;
            expected_error_ = true;
            return false;
        }
        if (!component_ || !expected.empty()) throw std::runtime_error("Unexpected open result");
        if (!component_->bind_callable<int32_t>("Proof", 71, 2, Direction::Server, true,
                [this](PlayerId sender, ObjectId object, int32_t phase) { proof(sender, object, phase); }, false, true) ||
            !component_->bind_callable<int32_t>("Reply", 72, 2, Direction::Client, true,
                [this](PlayerId sender, ObjectId object, int32_t value) { reply(sender, object, value); }, false, true))
            throw std::runtime_error("RPC registration");
        component_->on_frame = [this] { tick(); };
        scene->addComponent(component_);
        scene->scheduleUpdate();
        director->runWithScene(scene);
        std::cout << "OPEN sdk=" << core_build_id() << '/' << GameComponent::platform_build_id()
                  << " provider=" << component_->transport_identity().label() << std::endl;
        return true;
    }

    void applicationDidEnterBackground() override {}
    void applicationWillEnterForeground() override {}
    bool expected_error() const { return expected_error_; }

private:
    bool has(std::string_view key) const { return std::find(args_.begin(), args_.end(), key) != args_.end(); }
    std::string arg(std::string_view key, const char* fallback = "") const {
        const auto it = std::find(args_.begin(), args_.end(), key);
        return it != args_.end() && it + 1 != args_.end() ? std::string(*(it + 1)) : fallback;
    }

    void proof(PlayerId sender, ObjectId object, int32_t phase) {
        if (!host_ || sender != 2 || object != 0 || phase != ++reported_)
            throw std::runtime_error("Unexpected proof");
        if (!component_->game().call_method("Reply", 0, {int32_t(42)}, 2))
            throw std::runtime_error("Reply rejected");
        std::cout << "HOST_RPC phase=" << phase << " player=" << sender << std::endl;
        if (phase == 2) quit_after_ = Clock::now() + 500ms;
    }

    void reply(PlayerId sender, ObjectId object, int32_t value) {
        if (host_ || !sent_ || sender != 1 || object != 0 || value != 42)
            throw std::runtime_error("Unexpected reply");
        if (!resuming_) {
            const auto bytes = encode_resume_ticket(component_->game().session().guest()->ticket());
            std::ofstream output(ticket_, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (!output || bytes.empty()) throw std::runtime_error("Cannot save ticket");
        }
        std::cout << "GUEST_PASS fields=8 player=2 resumed=" << resuming_ << std::endl;
        cocos2d::Director::getInstance()->end();
    }

    void tick() {
        auto& script = component_->game();
        auto& game = script.session();
        if (host_) {
            if (!game.round() && game.host()->members().size() == 2 &&
                (!game.start(Clock::now()) || game.world().spawn(1, 1, fields) != 1))
                throw std::runtime_error("Cannot start round");
            if (quit_after_ && Clock::now() >= *quit_after_) {
                std::cout << "HOST_PASS" << std::endl;
                cocos2d::Director::getInstance()->end();
            }
        } else if (!sent_ && game.round() && game.local() == 2) {
            Values values;
            if (script.read(1, false, values) && values == fields)
                sent_ = script.call_method("Proof", 0, {int32_t(resuming_ ? 2 : 1)});
        }
    }

    std::vector<std::string_view> args_;
    std::filesystem::path exe_, ticket_;
    GameComponent* component_ = nullptr;
    bool host_ = false, resuming_ = false, sent_ = false, expected_error_ = false;
    int reported_ = 0;
    std::optional<TimePoint> quit_after_;
};
}

int main(int argc, char** argv) {
    try {
        if (!cocos2d::Runtime::configure(argc, argv)) return 1;
        Consumer application(argc, argv);
        const int result = application.run();
        return application.expected_error() ? 0 : result;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << std::endl;
        return 1;
    }
}
