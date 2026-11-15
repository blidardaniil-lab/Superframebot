#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <fstream>
#include <cstdio>

using namespace geode::prelude;

enum class Mode { Off, Record, Play };

struct Click { int frame; bool down; bool p1; int win; float x, y; };

// Фрейм-окна: подпись и цвет кружка (как на картинке)
static const char* WIN_NAMES[8] = {"9-12", "7-8", "5-6", "4", "3", "2", "1", "X"};
static const ccColor3B WIN_COLORS[8] = {
    {176, 96, 255}, {70, 160, 255}, {140, 255, 140}, {255, 255, 255},
    {255, 230, 0},  {255, 150, 30}, {255, 40, 40},   {100, 100, 100}};

namespace Bot {
    inline Mode mode = Mode::Off;
    inline std::vector<Click> clicks;
    inline size_t idx = 0;
    inline int frame = 0;
    inline int selWin = 3;          // выбранный кружок (по умолчанию "4")
    inline bool injecting = false;
    inline bool wantRender = false; // галочка "Render"
    inline bool rendering = false;
    inline FILE* ff = nullptr;
    inline CCDrawNode* markers = nullptr;
    inline CCLabelBMFont* status = nullptr;
    inline int vw = 0, vh = 0;

    inline std::filesystem::path replayPath(PlayLayer* pl) {
        return Mod::get()->getSaveDir() / fmt::format("{}.fb", pl->m_level->m_levelID.value());
    }
    inline std::filesystem::path videoPath(PlayLayer* pl) {
        return Mod::get()->getSaveDir() / fmt::format("{}.mp4", pl->m_level->m_levelName);
    }

    inline void addMarker(const Click& c) {
        if (!markers || !c.down) return;
        auto col = WIN_COLORS[c.win];
        markers->drawCircle({c.x, c.y}, 14.f, {0, 0, 0, 0}, 3.f,
                            {col.r / 255.f, col.g / 255.f, col.b / 255.f, 1.f}, 32);
    }

    inline void save(PlayLayer* pl) {
        std::ofstream f(replayPath(pl));
        for (auto& c : clicks)
            f << c.frame << ' ' << c.down << ' ' << c.p1 << ' ' << c.win << ' ' << c.x << ' ' << c.y << '\n';
    }
    inline void load(PlayLayer* pl) {
        clicks.clear();
        std::ifstream f(replayPath(pl));
        Click c{}; int d, p;
        while (f >> c.frame >> d >> p >> c.win >> c.x >> c.y) { c.down = d; c.p1 = p; clicks.push_back(c); }
    }
    inline void redrawMarkers() {
        if (!markers) return;
        markers->clear();
        for (auto& c : clicks) addMarker(c);
    }

    inline void updateStatus() {
        if (!status) return;
        const char* m = mode == Mode::Off ? "OFF" : mode == Mode::Record ? "REC" : "PLAY";
        status->setString(fmt::format("{} | окно: {} | video: {}", m, WIN_NAMES[selWin], wantRender ? "ON" : "off").c_str());
    }

    inline void startRender(PlayLayer* pl) {
#ifdef GEODE_IS_WINDOWS
        if (ff || !wantRender) return;
        auto sz = CCDirector::get()->getOpenGLView()->getFrameSize();
        vw = (int)sz.width & ~1; vh = (int)sz.height & ~1;
        auto cmd = fmt::format(
            "ffmpeg -y -f rawvideo -pix_fmt rgb24 -s {}x{} -r 60 -i - -vf vflip "
            "-c:v libx264 -pix_fmt yuv420p -preset fast \"{}\" 2>NUL",
            vw, vh, videoPath(pl).string());
        ff = _popen(cmd.c_str(), "wb");
        rendering = ff != nullptr;
        if (!ff) FLAlertLayer::create("FrameBot", "Не удалось запустить ffmpeg. Установите ffmpeg и добавьте в PATH.", "OK")->show();
#endif
    }
    inline void stopRender() {
#ifdef GEODE_IS_WINDOWS
        if (ff) { _pclose(ff); ff = nullptr; }
#endif
        rendering = false;
    }
    inline void grab() {
        if (!ff) return;
        std::vector<uint8_t> buf((size_t)vw * vh * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, buf.data());
        fwrite(buf.data(), 1, buf.size(), ff);
    }
}

class $modify(FBBase, GJBaseGameLayer) {
    void processCommands(float dt) {
        if (PlayLayer::get() && Bot::mode == Mode::Play) {
            while (Bot::idx < Bot::clicks.size() && Bot::clicks[Bot::idx].frame <= Bot::frame) {
                auto& c = Bot::clicks[Bot::idx++];
                Bot::injecting = true;
                this->handleButton(c.down, 1, c.p1);
                Bot::injecting = false;
            }
        }
        GJBaseGameLayer::processCommands(dt);
        if (PlayLayer::get()) Bot::frame++;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (PlayLayer::get() && !Bot::injecting) {
            if (Bot::mode == Mode::Play) return; // во время бота ручные клики блокируем
            if (Bot::mode == Mode::Record && button == 1) {
                // клик "ставится" на кадр и позицию, где вы прыгнули
                Click c{Bot::frame, down, isPlayer1, Bot::selWin,
                        m_player1->getPositionX(), m_player1->getPositionY()};
                Bot::clicks.push_back(c);
                Bot::addMarker(c);
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(FBPlay, PlayLayer) {
    bool init(GJGameLevel* level, bool a, bool b) {
        if (!PlayLayer::init(level, a, b)) return false;
        Bot::mode = Mode::Off;
        Bot::stopRender();
        return true;
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        Bot::load(this);

        Bot::markers = CCDrawNode::create();
        m_objectLayer->addChild(Bot::markers, 1000);
        Bot::redrawMarkers();

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        this->addChild(menu, 1000);

        // 8 кружков-окон
        for (int i = 0; i < 8; i++) {
            auto spr = CCSprite::createWithSpriteFrameName("GJ_button_01.png");
            spr->setColor(WIN_COLORS[i]);
            spr->setScale(0.45f);
            auto lbl = CCLabelBMFont::create(WIN_NAMES[i], "bigFont.fnt");
            lbl->setScale(0.7f);
            lbl->setPosition(spr->getContentSize() / 2);
            spr->addChild(lbl);
            auto item = CCMenuItemExt::createSpriteExtra(spr, [i](CCMenuItemSpriteExtra*) {
                Bot::selWin = i; Bot::updateStatus();
            });
            item->setPosition({22.f + (i % 4) * 30.f, 130.f - (i / 4) * 30.f});
            menu->addChild(item);
        }

        auto mkBtn = [&](const char* txt, float x, auto cb) {
            auto s = ButtonSprite::create(txt, "bigFont.fnt", "GJ_button_04.png", 0.5f);
            auto it = CCMenuItemExt::createSpriteExtra(s, cb);
            it->setPosition({x, 60.f});
            menu->addChild(it);
        };
        mkBtn("Rec", 30, [this](CCMenuItemSpriteExtra*) {
            Bot::mode = Mode::Record; Bot::clicks.clear(); Bot::redrawMarkers();
            Bot::updateStatus(); this->resetLevel();
        });
        mkBtn("Play", 85, [this](CCMenuItemSpriteExtra*) {
            Bot::mode = Mode::Play; Bot::updateStatus(); this->resetLevel();
        });
        mkBtn("Off", 135, [this](CCMenuItemSpriteExtra*) {
            Bot::mode = Mode::Off; Bot::stopRender(); Bot::updateStatus();
        });
        mkBtn("Video", 185, [](CCMenuItemSpriteExtra*) {
            Bot::wantRender = !Bot::wantRender; Bot::updateStatus();
        });

        Bot::status = CCLabelBMFont::create("", "chatFont.fnt");
        Bot::status->setAnchorPoint({0, 0.5f});
        Bot::status->setPosition({8, 95});
        Bot::status->setScale(0.7f);
        this->addChild(Bot::status, 1000);
        Bot::updateStatus();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        Bot::frame = 0;
        Bot::idx = 0;
        if (Bot::mode == Mode::Record) { Bot::clicks.clear(); Bot::redrawMarkers(); }
        if (Bot::mode == Mode::Play) Bot::startRender(this);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (Bot::rendering) Bot::grab();
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        if (Bot::mode == Mode::Record) Bot::save(this);
        Bot::stopRender();
    }

    void onQuit() {
        if (Bot::mode == Mode::Record) Bot::save(this);
        Bot::stopRender();
        Bot::markers = nullptr; Bot::status = nullptr;
        PlayLayer::onQuit();
    }
};

// Фиксированный шаг 1/60 при рендере, чтобы видео не лагало
class $modify(FBSched, CCScheduler) {
    void update(float dt) {
        if (Bot::rendering) dt = 1.f / 60.f;
        CCScheduler::update(dt);
    }
};
