#include "note4c/application.hpp"
#include "note4c/ssd2683.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

using namespace note4c;
#define CHECK(x) do { if (!(x)) { std::cerr << __func__ << ":" << __LINE__ \
    << " failed: " #x "\n"; std::exit(1); } } while (false)
struct FakeDisplay : DisplayPort {
    const Frame* current = nullptr;
    Frame snapshot;
    Transfer result = Transfer::Busy;
    bool accept = true;
    int starts = 0;
    bool start_full_refresh(const Frame& f) override {
        if (!accept) return false;
        ++starts; current = &f; snapshot = f; result = Transfer::Busy; return true;
    }
    Transfer poll() override {
        CHECK(current && *current == snapshot); // Worker memory must stay immutable.
        return result;
    }
};
struct MemoryStore : SettingsStore {
    Settings value;
    LoadResult result = LoadResult::Missing;
    bool save_ok = true;
    int saves = 0;
    LoadResult load(Settings& out) override { out = value; return result; }
    bool save(const Settings& s) override { ++saves; if (save_ok) value = s; return save_ok; }
};
struct FakeNetwork : Network {
    NetworkState status = NetworkState::Off;
    bool connect(std::string_view, std::string_view) override {
        status = NetworkState::Connecting; return true;
    }
    void stop() override { status = NetworkState::Off; }
    NetworkState state() const override { return status; }
};
void pixels_and_rendering() {
    Frame f;
    CHECK(f.bytes.size() == 30000 && f.bytes.front() == 0x55);
    f.pixel(0,0,Color::Black); f.pixel(1,0,Color::White);
    f.pixel(2,0,Color::Yellow); f.pixel(3,0,Color::Red);
    CHECK(f.bytes[0] == 0x1B);
    CHECK(f.pixel(3,0) == Color::Red);
    auto old = f;
    f.pixel(-1,0,Color::Red); f.pixel(400,300,Color::Red);
    CHECK(f == old);
    f.rect(-3,-3,5,5,Color::Yellow);
    CHECK(f.pixel(1,1) == Color::Yellow && f.pixel(2,2) == Color::White);
    f.rect(std::numeric_limits<int>::max(), 0, 10, 10, Color::Black);
    f.text(std::numeric_limits<int>::min(),0,"A",Color::Black);
    Frame a, b;
    Screen screen{"TEST", {"VALUE: 1"}, Color::Yellow};
    ScreenRenderer renderer;
    renderer.draw(screen, a); renderer.draw(screen, b);
    CHECK(a == b && a.pixel(0,0) == Color::Yellow);
    screen.lines[0] = "VALUE: 2"; renderer.draw(screen, b);
    CHECK(a != b);
}
void scheduling_and_busy_input() {
    FakeDisplay port;
    DisplayScheduler scheduler(port, {600,2000,3000});
    MemoryStore store; FakeNetwork net; CounterApp counter; StatusApp status;
    Runtime runtime(store,net,scheduler);
    CHECK(runtime.add(counter) && runtime.add(status) && !runtime.add(counter));
    CHECK(runtime.begin(0) && !runtime.begin(0));
    CHECK(!runtime.add(status));
    runtime.tick(0); runtime.tick(599); CHECK(port.starts == 0);
    runtime.tick(600); CHECK(port.starts == 1 && scheduler.busy());
    const auto first = port.snapshot;
    // A simulated 12-second refresh must not prevent model input or navigation.
    for (Millis t = 1000; t < 12000; t += 100) {
        runtime.button({Button::Up,Press::Short},t);
        runtime.tick(t);
    }
    CHECK(port.starts == 1 && port.snapshot == first && scheduler.pending());
    runtime.button({Button::Confirm,Press::Short},12000);
    runtime.tick(12000); CHECK(runtime.active_id() == "status");
    runtime.button({Button::Confirm,Press::Short},12100);
    runtime.tick(12100); CHECK(runtime.active_id() == "counter");
    CHECK(runtime.flush_settings() && store.value.counter == 110);
    port.result = Transfer::Success;
    runtime.tick(12600); CHECK(scheduler.completed() == 1 && !scheduler.busy());
    runtime.tick(15599); CHECK(port.starts == 1);
    runtime.tick(15600); CHECK(port.starts == 2 && port.snapshot != first);
    port.result = Transfer::Success;
    runtime.tick(27600); CHECK(scheduler.completed() == 2 && !scheduler.pending());
    runtime.tick(90000); CHECK(port.starts == 2); // Unchanged screen never refreshes.
}
void coalescing_and_return_to_visible() {
    FakeDisplay p; DisplayScheduler s(p, {100,400,0});
    Frame white, red; red.clear(Color::Red);
    s.submit(white,0); s.tick(100); CHECK(p.starts == 1);
    s.submit(red,110); s.submit(white,120);
    p.result = Transfer::Success; s.tick(200);
    CHECK(!s.pending() && p.starts == 1);
    // Continuous changes cannot starve refresh beyond maximum coalescing time.
    for (Millis t=300; t<=700; t+=50) {
        red.pixel(0,0,(t/50)%2 ? Color::Black : Color::Red);
        s.submit(red,t); s.tick(t);
    }
    CHECK(p.starts == 2);
    // A repeated identical request does not extend the quiet window.
    FakeDisplay p2; DisplayScheduler s2(p2,{100,400,0});
    s2.submit(white,0); s2.submit(white,90); s2.tick(100);
    CHECK(p2.starts == 1);
    // Returning to the OLD visible image during a newer in-flight job still
    // requires repainting after that job completes.
    FakeDisplay p3; DisplayScheduler s3(p3,{0,0,0});
    s3.submit(white,0); s3.tick(0); p3.result=Transfer::Success; s3.tick(1);
    s3.submit(red,2); s3.tick(2); s3.submit(white,3);
    p3.result=Transfer::Success; s3.tick(4);
    CHECK(p3.starts==3 && p3.snapshot==white);
}
void display_fault_recovery() {
    FakeDisplay p; DisplayScheduler s(p,{0,0,0}); Frame f;
    s.submit(f,0); s.tick(0); p.result=Transfer::Failure; s.tick(120000);
    CHECK(s.faulted() && s.pending() && !s.busy() && s.completed()==0);
    s.tick(999999); CHECK(p.starts==1);
    s.retry(1000000); s.tick(1000000); CHECK(p.starts==2);
    p.result=Transfer::Success; s.tick(1000010);
    CHECK(!s.faulted() && !s.pending() && s.completed()==1);
    p.accept=false; f.pixel(0,0,Color::Red); s.submit(f,1000020); s.tick(1000020);
    CHECK(s.faulted() && !s.busy() && s.pending());
}
void buttons_and_boot_guard() {
    Buttons b;
    CHECK(!b.sample(Button::Up,false,0));
    CHECK(!b.sample(Button::Up,true,10));
    CHECK(!b.sample(Button::Up,false,15)); // Bounce.
    CHECK(!b.sample(Button::Up,true,20));
    CHECK(!b.sample(Button::Up,true,50) && b.held());
    CHECK(!b.sample(Button::Up,false,100));
    auto e=b.sample(Button::Up,false,130);
    CHECK(e && e->button==Button::Up && e->press==Press::Short && !b.held());
    b.sample(Button::Up,true,200); b.sample(Button::Up,true,230);
    e=b.sample(Button::Up,true,1130); CHECK(e && e->press==Press::Long);
    CHECK(!b.sample(Button::Up,true,2000));
    b.sample(Button::Up,false,2010); CHECK(!b.sample(Button::Up,false,2040));
    Buttons boot;
    boot.sample(Button::Confirm,true,0);
    CHECK(!boot.sample(Button::Confirm,true,2000));
    boot.sample(Button::Confirm,false,2010);
    CHECK(!boot.sample(Button::Confirm,false,2040));
    boot.sample(Button::Confirm,true,2100); boot.sample(Button::Confirm,true,2130);
    boot.sample(Button::Confirm,false,2200);
    CHECK(boot.sample(Button::Confirm,false,2230).has_value());
}
struct LifecycleApp : Application {
    int enters=0, leaves=0, events=0;
    std::string name;
    explicit LifecycleApp(std::string n):name(std::move(n)){}
    std::string_view id() const override { return name; }
    void enter(Services&) override { ++enters; }
    void leave(Services&) override { ++leaves; }
    void button(ButtonEvent,Services&) override { ++events; }
    Screen screen(const Services&) const override { return {name}; }
};
void lifecycle_settings_and_power() {
    for (const int32_t value : {0, -1, 42, std::numeric_limits<int32_t>::min(),
                                std::numeric_limits<int32_t>::max()}) {
        Settings original; original.counter=value;
        auto bytes=SettingsRecord::encode(original);
        Settings restored;
        CHECK(SettingsRecord::decode(bytes.data(),bytes.size(),restored) && restored==original);
        CHECK(!SettingsRecord::decode(bytes.data(),bytes.size()-1,restored));
        bytes[0]=99;
        CHECK(!SettingsRecord::decode(bytes.data(),bytes.size(),restored) && restored==original);
    }
    FakeDisplay p; DisplayScheduler d(p); MemoryStore store; FakeNetwork n;
    LifecycleApp a("A"), b("B"); Runtime r(store,n,d);
    CHECK(!r.begin(0)); r.add(a); r.add(b); CHECK(r.begin(0));
    r.button({Button::Up,Press::Short},1);
    r.button({Button::Confirm,Press::Short},2);
    CHECK(a.enters==1 && a.leaves==1 && a.events==1 && b.enters==1 && b.events==0);
    CHECK(!PowerPolicy::may_sleep(r.activity(true,false),10000,0,1000));
    Activity idle;
    CHECK(PowerPolicy::may_sleep(idle,1000,0,1000));
    CHECK(!PowerPolicy::may_sleep(idle,1000,0,0));
    CHECK(!PowerPolicy::may_sleep(idle,999,0,1000));
    for (int i=0;i<8;++i) {
        Activity blocked;
        switch(i) {
        case 0: blocked.display_busy=true; break;
        case 1: blocked.display_pending=true; break;
        case 2: blocked.display_fault=true; break;
        case 3: blocked.settings_dirty=true; break;
        case 4: blocked.network_active=true; break;
        case 5: blocked.button_held=true; break;
        case 6: blocked.charging=true; break;
        case 7: blocked.app_busy=true; break;
        }
        CHECK(!PowerPolicy::may_sleep(blocked,10000,0,1000));
    }
    CounterApp c; FakeDisplay p2; DisplayScheduler d2(p2);
    MemoryStore s2; Runtime r2(s2,n,d2); r2.add(c); r2.begin(0);
    r2.button({Button::Up,Press::Short},10); r2.tick(10);
    r2.tick(1009); CHECK(s2.saves==0);
    s2.save_ok=false; r2.tick(1010); CHECK(s2.saves==1 && r2.settings_dirty());
    r2.tick(1050); CHECK(s2.saves==1 && r2.storage_error());
    s2.save_ok=true; r2.tick(2010); CHECK(s2.saves==2 && !r2.settings_dirty());
    r2.tick(9000); CHECK(s2.saves==2);
    MemoryStore incompatible; incompatible.result=LoadResult::Incompatible;
    Runtime r3(incompatible,n,d2); r3.add(c); r3.begin(0);
    r3.button({Button::Up,Press::Short},1); r3.tick(5000);
    CHECK(!r3.storage_writable() && r3.storage_error() && !r3.flush_settings());
    CHECK(incompatible.saves==0);
    Settings settings; Services services{settings,n};
    settings.counter=std::numeric_limits<int32_t>::max();
    c.button({Button::Up,Press::Short},services);
    CHECK(settings.counter==std::numeric_limits<int32_t>::max());
}
struct RecordingIo : PanelIo {
    std::vector<uint8_t> commands;
    std::vector<bool> power_states, reset_states;
    size_t pixel_bytes=0;
    int waits=0, fail_wait=0, writes=0, fail_write=0;
    bool power(bool on) override { power_states.push_back(on); return true; }
    bool reset(bool high) override { reset_states.push_back(high); return true; }
    bool write(bool data,const uint8_t* b,size_t len) override {
        if (++writes==fail_write) return false;
        if (!data) commands.push_back(*b);
        if (data && len==Frame::stride) pixel_bytes+=len;
        return true;
    }
    bool wait_ready(uint32_t timeout) override { CHECK(timeout==120000); return ++waits!=fail_wait; }
    void delay_ms(uint32_t) override {}
};
void panel_sequence_and_failures() {
    Frame frame; RecordingIo io; Ssd2683 panel(io);
    CHECK(panel.refresh(frame));
    CHECK(io.commands==std::vector<uint8_t>({0xE9,0x10,0x04,0x12,0x02,0x07}));
    CHECK(io.pixel_bytes==30000 && io.waits==5);
    CHECK(io.reset_states==std::vector<bool>({true,false,true}));
    CHECK(io.power_states==std::vector<bool>({true,false}));
    for (int wait=1;wait<=5;++wait) {
        RecordingIo bad; bad.fail_wait=wait; Ssd2683 driver(bad);
        CHECK(!driver.refresh(frame) && !bad.power_states.back());
        CHECK(bad.waits==wait); // Never continue sending commands after a timeout.
    }
    for (int write=1;write<=io.writes;++write) {
        RecordingIo bad; bad.fail_write=write; Ssd2683 driver(bad);
        CHECK(!driver.refresh(frame) && !bad.power_states.back() && bad.writes==write);
    }
}
int main() {
    pixels_and_rendering(); scheduling_and_busy_input();
    coalescing_and_return_to_visible(); display_fault_recovery();
    buttons_and_boot_guard(); lifecycle_settings_and_power(); panel_sequence_and_failures();
    std::cout << "PASS: 7 suites (including every panel write and BUSY failure point)\n";
}
