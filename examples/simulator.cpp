#include "note4c/application.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace note4c;
struct MemorySettings final : SettingsStore {
    Settings value;
    LoadResult load(Settings& s) override { s=value; return LoadResult::Missing; }
    bool save(const Settings& s) override { value=s; return true; }
};
struct OfflineNetwork final : Network {
    bool connect(std::string_view,std::string_view) override { return false; }
    void stop() override {}
    NetworkState state() const override { return NetworkState::Off; }
};
struct SimDisplay final : DisplayPort {
    Millis now=0, ready_at=0;
    const Frame* job=nullptr;
    bool fail=false;
    Frame visible;
    bool start_full_refresh(const Frame& f) override {
        job=&f; ready_at=now+12000;
        std::cout << "Refresh started at " << now << " ms\n"; return true;
    }
    Transfer poll() override {
        if (now<ready_at) return Transfer::Busy;
        if (fail) { fail=false; return Transfer::Failure; }
        visible=*job;
        std::ofstream out("screen.ppm",std::ios::binary);
        out << "P6\n400 300\n255\n";
        constexpr unsigned char colors[4][3]={{20,20,20},{250,249,242},{245,200,25},{195,35,40}};
        for (int y=0;y<Frame::height;++y) for (int x=0;x<Frame::width;++x)
            out.write(reinterpret_cast<const char*>(colors[static_cast<int>(visible.pixel(x,y))]),3);
        std::cout << "Refresh completed at " << now << " ms; wrote screen.ppm\n";
        return Transfer::Success;
    }
};
int main() {
    MemorySettings settings; OfflineNetwork net; SimDisplay port;
    DisplayScheduler display(port); CounterApp counter; StatusApp status;
    Runtime runtime(settings,net,display);
    runtime.add(counter); runtime.add(status); runtime.begin(0); runtime.tick(0);
    std::cout << "Commands: up, down, next, reset, retry, fail, advance MS, show, quit\n"
                 "Time is simulated; advance 600 then 12000 to see the initial frame.\n";
    std::string line;
    while (std::getline(std::cin,line) && line!="quit") {
        std::istringstream in(line); std::string cmd; in>>cmd;
        if (cmd=="advance") {
            uint64_t ms=0; in>>ms;
            if (!in || ms>3600000) { std::cout << "Use 0..3600000 ms\n"; continue; }
            const auto end=port.now+ms;
            while (port.now<end) { port.now+=std::min<uint64_t>(20,end-port.now); runtime.tick(port.now); }
        } else if (cmd=="up") runtime.button({Button::Up,Press::Short},port.now);
        else if (cmd=="down") runtime.button({Button::Down,Press::Short},port.now);
        else if (cmd=="next") runtime.button({Button::Confirm,Press::Short},port.now);
        else if (cmd=="reset") runtime.button({Button::Confirm,Press::Long},port.now);
        else if (cmd=="retry") display.retry(port.now);
        else if (cmd=="fail") port.fail=true;
        else if (cmd!="show") { std::cout << "Unknown command\n"; continue; }
        runtime.tick(port.now);
        std::cout << port.now << " ms app=" << runtime.active_id()
                  << " busy=" << display.busy() << " pending=" << display.pending()
                  << " fault=" << display.faulted() << " completed=" << display.completed() << '\n';
    }
}
