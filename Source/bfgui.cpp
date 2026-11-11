// bfgui.cpp - Brainfuck IDLE (FLTK 1.3, tkinter 와 비슷한 가벼운 툴킷)
//
// 기존 bfint.cpp 는 수정 없이 #include 합니다 (원본 main 은 bf_cli_main 으로 이름만 바뀜).
// std::cout / std::cerr / std::cin 의 streambuf 를 GUI 용으로 교체해서
// 원본 ManualInterpreter / OptimizedInterpreter 의 출력·입력·Traceback 이 그대로 셸에 연결됩니다.
// 실행은 별도 스레드, 중지는 원본과 같은 g_interrupted 플래그를 사용합니다.

#define main bf_cli_main
#include "bfint.cpp"
#undef main

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Tile.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

// ============================================================================
// 1. 표준 스트림 연결 (스레드 안전)
// ============================================================================
struct OutQueue {
    std::mutex m;
    std::vector<std::pair<bool, std::string>> chunks; // (is_stderr, bytes)
    void push(bool err, const char* s, size_t n) {
        std::lock_guard<std::mutex> l(m);
        if (!chunks.empty() && chunks.back().first == err) chunks.back().second.append(s, n);
        else chunks.emplace_back(err, std::string(s, n));
    }
};
static OutQueue g_outq;

class SinkBuf : public std::streambuf {
public:
    explicit SinkBuf(bool err) : err_(err) {}
protected:
    int_type overflow(int_type c) override {
        if (c != traits_type::eof()) {
            char ch = traits_type::to_char_type(c);
            g_outq.push(err_, &ch, 1);
        }
        return traits_type::not_eof(c);
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        g_outq.push(err_, s, static_cast<size_t>(n));
        return n;
    }
private:
    bool err_;
};

// std::cin 이 읽는 버퍼: 셸에서 입력한 줄이 올 때까지 인터프리터 스레드를 대기시킨다.
class InBuf : public std::streambuf {
public:
    void push(const std::string& s) {
        std::lock_guard<std::mutex> l(m_);
        data_ += s;
        cv_.notify_all();
    }
    void interrupt() {
        std::lock_guard<std::mutex> l(m_);
        intr_ = true;
        cv_.notify_all();
    }
    void send_eof() {
        std::lock_guard<std::mutex> l(m_);
        eof_ = true;
        cv_.notify_all();
    }
    void reset() { // 인터프리터 스레드가 없을 때 호출
        std::lock_guard<std::mutex> l(m_);
        data_.clear();
        cur_.clear();
        intr_ = eof_ = false;
        setg(nullptr, nullptr, nullptr);
    }
protected:
    int_type underflow() override {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return !data_.empty() || intr_ || eof_; });
        if (data_.empty()) {
            eof_ = false; // Ctrl+D 는 한 번만 EOF
            return traits_type::eof();
        }
        cur_.swap(data_);
        data_.clear();
        setg(&cur_[0], &cur_[0], &cur_[0] + cur_.size());
        return traits_type::to_int_type(cur_[0]);
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    std::string data_, cur_;
    bool intr_ = false, eof_ = false;
};
static InBuf g_inbuf;

// ============================================================================
// 2. 파일 실행 (원본 main 의 Optimize / Manual 파일 모드와 같은 흐름)
// ============================================================================
static int run_file_gui(const std::string& file_path, RunMode mode) {
    std::string active_source = file_path;
    try {
        if (mode == RunMode::Manual) {
            std::string code = read_file_text(file_path);
            std::cerr << "[Manual] Running '" << file_path << "'\n";
            ManualInterpreter vm;
            vm.run(code);
            return 0;
        }

        const std::string bfc_path = make_bfc_path(file_path);
        active_source = bfc_path;
        OptimizedInterpreter vm;
        std::vector<Op> ops;

        BfcFile cached;
        bool have_cache = false;
        const bool bfc_exists = file_exists(bfc_path);
        if (bfc_exists) {
            try {
                cached = OptimizedInterpreter::load_bfc(bfc_path);
                have_cache = true;
            } catch (const std::exception&) {}
        }

        const std::string source_text = read_file_binary(file_path);
        const std::string hash = Sha256::hex_digest(source_text);
        if (have_cache && cached.hash == hash) {
            ops = std::move(cached.ops);
            std::cerr << "[Optimize] Loaded '" << bfc_path << "' (" << ops.size()
                      << " ops, source hash verified)\n";
        } else {
            std::string reason = !bfc_exists ? "no cache" : have_cache ? "source hash mismatch" : "invalid cache";
            active_source = file_path;
            ops = OptimizedInterpreter::compile(source_text);
            active_source = bfc_path;
            std::cerr << "[Optimize] Compiled '" << file_path << "' -> '" << bfc_path
                      << "' (" << ops.size() << " ops, " << reason << ")\n";
            try {
                OptimizedInterpreter::save_bfc(bfc_path, base_name(file_path), hash, ops);
            } catch (const std::exception& e) {
                std::cerr << "[Optimize] Warning: " << e.what() << " (running without cache)\n";
            }
        }
        vm.run_ops(ops);
    } catch (const KeyboardInterrupt&) {
        std::cerr << "\n";
        print_traceback(active_source, "KeyboardInterrupt");
        return 130;
    } catch (const BrainfuckError& e) {
        std::cerr << "\n";
        print_traceback(active_source, e.type(), e.what(), e.instr(), e.has_pos());
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "bfint: " << e.what() << "\n";
        return 2;
    }
    return 0;
}

// ============================================================================
// 3. 셸 위젯 (IDLE 처럼 프롬프트 뒤에서 직접 입력)
// ============================================================================
// 스타일 문자: A=일반, B=오류(빨강), C=안내(회색), D=프롬프트(파랑), E=RESTART(청록)
static const Fl_Text_Display::Style_Table_Entry kShellStyles[] = {
    {FL_BLACK, FL_COURIER, 14},
    {fl_rgb_color(192, 38, 45), FL_COURIER, 14},
    {fl_rgb_color(120, 120, 120), FL_COURIER, 14},
    {fl_rgb_color(39, 72, 168), FL_COURIER_BOLD, 14},
    {fl_rgb_color(14, 124, 134), FL_COURIER_BOLD, 14},
};

class Shell : public Fl_Text_Editor {
public:
    std::function<void(const std::string&)> on_line;
    std::function<void()> on_interrupt, on_eof;

    Shell(int x, int y, int w, int h) : Fl_Text_Editor(x, y, w, h) {
        buffer(&tb_);
        tb_.add_modify_callback(modify_cb, this);
        highlight_data(&sb_, kShellStyles, 5, 'A', nullptr, nullptr);
        textfont(FL_COURIER);
        textsize(14);
        wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    }

    void print(const std::string& t, char style = 'A') {
        if (t.empty()) return;
        const int end = tb_.length();
        char* p = tb_.text_range(input_start_, end);
        std::string pending(p ? p : "");
        free(p);
        tb_.remove(input_start_, end);
        cur_ = style;
        tb_.insert(input_start_, t.c_str());
        cur_ = 'A';
        input_start_ += static_cast<int>(t.size());
        if (!pending.empty()) tb_.insert(input_start_, pending.c_str());
        insert_position(tb_.length());
        show_insert_position();
    }

    void prompt(const std::string& p) {
        const int n = tb_.length();
        if (n > 0 && tb_.byte_at(n - 1) != '\n') print("\n");
        print(p, 'D');
    }

    void clear_all() {
        tb_.text("");
        input_start_ = 0;
    }

    int handle(int e) override {
        if (e == FL_KEYBOARD) {
            const int k = Fl::event_key();
            const bool ctrl = Fl::event_state(FL_CTRL) != 0;
            const bool alt = Fl::event_state(FL_ALT) != 0;
            const bool shift = Fl::event_state(FL_SHIFT) != 0;

            if (ctrl && (k == 'c') && !tb_.selected()) { if (on_interrupt) on_interrupt(); return 1; }
            if (ctrl && k == 'd') { if (on_eof) on_eof(); return 1; }
            if (k == FL_Enter || k == FL_KP_Enter) { submit(); return 1; }
            if (k == FL_Up && !shift) { navigate(-1); return 1; }
            if (k == FL_Down && !shift) { navigate(+1); return 1; }
            if (k == FL_Home) { insert_position(input_start_); show_insert_position(); return 1; }
            if ((k == FL_Left || k == FL_BackSpace) && !tb_.selected() && insert_position() <= input_start_) return 1;

            const bool editing = k == FL_BackSpace || k == FL_Delete ||
                                 (Fl::event_length() > 0 && !ctrl && !alt) ||
                                 (ctrl && (k == 'v' || k == 'x'));
            if (editing) {
                int ss = 0, se = 0;
                const bool sel = tb_.selection_position(&ss, &se) != 0;
                const int s = sel ? ss : insert_position();
                if (s < input_start_) {
                    if (k == FL_BackSpace || k == FL_Delete || (ctrl && k == 'x')) return 1;
                    tb_.unselect();
                    insert_position(tb_.length());
                }
            }
        }
        return Fl_Text_Editor::handle(e);
    }

private:
    static void modify_cb(int pos, int n_ins, int n_del, int, const char*, void* v) {
        Shell* s = static_cast<Shell*>(v);
        if (n_ins == 0 && n_del == 0) { s->sb_.unselect(); return; }
        if (n_ins > 0) {
            std::string st(static_cast<size_t>(n_ins), s->cur_);
            s->sb_.replace(pos, pos + n_del, st.c_str());
        } else {
            s->sb_.remove(pos, pos + n_del);
        }
    }

    void submit() {
        char* p = tb_.text_range(input_start_, tb_.length());
        std::string line(p ? p : "");
        free(p);
        for (char& c : line) if (c == '\n' || c == '\r') c = ' ';
        tb_.append("\n");
        input_start_ = tb_.length();
        insert_position(input_start_);
        show_insert_position();
        if (!line.empty() && line.find_first_not_of(' ') != std::string::npos) hist_.push_back(line);
        hist_pos_ = static_cast<int>(hist_.size());
        if (on_line) on_line(line);
    }

    void navigate(int d) {
        if (hist_.empty()) return;
        int np = hist_pos_ + d;
        if (np < 0) np = 0;
        if (np > static_cast<int>(hist_.size())) np = static_cast<int>(hist_.size());
        hist_pos_ = np;
        tb_.remove(input_start_, tb_.length());
        if (np < static_cast<int>(hist_.size())) tb_.insert(input_start_, hist_[np].c_str());
        insert_position(tb_.length());
        show_insert_position();
    }

    Fl_Text_Buffer tb_, sb_;
    char cur_ = 'A';
    int input_start_ = 0;
    std::vector<std::string> hist_;
    int hist_pos_ = 0;
};

// ============================================================================
// 4. 앱 상태와 콜백
// ============================================================================
// 편집기 스타일: A=주석, B=+ -, C=< >, D=[ ], E=. ,
static const Fl_Text_Display::Style_Table_Entry kEditStyles[] = {
    {fl_rgb_color(110, 110, 110), FL_COURIER, 14},
    {fl_rgb_color(30, 140, 75), FL_COURIER_BOLD, 14},
    {fl_rgb_color(40, 100, 215), FL_COURIER_BOLD, 14},
    {fl_rgb_color(215, 125, 10), FL_COURIER_BOLD, 14},
    {fl_rgb_color(190, 60, 160), FL_COURIER_BOLD, 14},
};

namespace app {
Fl_Double_Window* win = nullptr;
Fl_Text_Editor* editor = nullptr;
Shell* shell = nullptr;
Fl_Box* status = nullptr;
Fl_Text_Buffer edbuf, edstyle;
std::string path;
bool modified = false, loading = false;
RunMode mode = RunMode::Optimize;
ManualInterpreter manual_vm;
OptimizedInterpreter opt_vm;
std::thread worker;
std::atomic<bool> done{false};
bool running = false;
int depth = 0;
std::string cbuf;
std::string carry_out, carry_err; // UTF-8 이 중간에 잘린 바이트
bool err_line_start = true, err_dim = false;
} // namespace app

static void update_title() {
    std::string name = app::path.empty() ? "Untitled" : base_name(app::path);
    std::string t = name + (app::modified ? " *" : "") + " - Brainfuck IDLE";
    app::win->copy_label(t.c_str());
}

static void restyle() {
    char* t = app::edbuf.text();
    std::string s(t ? t : "");
    free(t);
    for (char& c : s) {
        switch (c) {
            case '+': case '-': c = 'B'; break;
            case '>': case '<': c = 'C'; break;
            case '[': case ']': c = 'D'; break;
            case '.': case ',': c = 'E'; break;
            default: c = 'A'; break;
        }
    }
    app::edstyle.text(s.c_str());
}

static void editor_modify_cb(int, int n_ins, int n_del, int, const char*, void*) {
    if (n_ins == 0 && n_del == 0) return;
    restyle();
    if (!app::loading && !app::modified) { app::modified = true; update_title(); }
}

// UTF-8 이 도중에 끊긴 꼬리 바이트를 떼어 다음 번으로 넘긴다.
static std::string take_complete_utf8(std::string& carry, const std::string& in) {
    std::string s = carry + in;
    carry.clear();
    int n = static_cast<int>(s.size()), i = n - 1, back = 0;
    while (i >= 0 && back < 4 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) { --i; ++back; }
    if (i >= 0) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (need > 1 && back + 1 < need) { carry = s.substr(i); s.resize(i); }
    }
    return s;
}

static void drain() {
    std::vector<std::pair<bool, std::string>> v;
    {
        std::lock_guard<std::mutex> l(g_outq.m);
        v.swap(g_outq.chunks);
    }
    for (auto& [err, bytes] : v) {
        std::string t = take_complete_utf8(err ? app::carry_err : app::carry_out, bytes);
        if (!err) { app::shell->print(t, 'A'); continue; }
        // stderr: "[Optimize]..." / "[Manual]..." 안내 줄은 회색, Traceback 은 빨강
        size_t i = 0;
        while (i < t.size()) {
            if (app::err_line_start) app::err_dim = (t[i] == '[');
            size_t j = t.find('\n', i);
            size_t end = (j == std::string::npos) ? t.size() : j + 1;
            app::shell->print(t.substr(i, end - i), app::err_dim ? 'C' : 'B');
            app::err_line_start = (j != std::string::npos);
            i = end;
        }
    }
}

static void show_prompt() {
    drain();
    app::shell->prompt(app::depth > 0 ? "... " : ">>> ");
}

static void on_finished() {
    if (app::worker.joinable()) app::worker.join();
    drain();
    app::running = false;
    app::status->label("");
    app::status->redraw();
    show_prompt();
}

static void tick(void*) {
    drain();
    if (app::running && app::done.load()) on_finished();
    Fl::repeat_timeout(0.015, tick);
}

static void start_worker(std::function<void()> job) {
    g_inbuf.reset();
    std::cin.clear();
    app::running = true;
    app::done = false;
    app::status->label("  Executing... (Ctrl+C: Stop | Type below for input)");
    app::status->redraw();
    app::worker = std::thread([job] {
        job();
        app::done = true;
    });
}

static void stop_worker_blocking() {
    if (!app::running) return;
    g_interrupted = 1;
    g_inbuf.interrupt();
    if (app::worker.joinable()) app::worker.join();
    app::running = false;
    app::done = false;
}

static void do_stop() {
    if (app::running) {
        g_interrupted = 1;
        g_inbuf.interrupt();
    } else {
        app::shell->print("KeyboardInterrupt\n", 'B');
        app::depth = 0;
        app::cbuf.clear();
        show_prompt();
    }
}

static void restart_shell() {
    stop_worker_blocking();
    app::manual_vm.reset();
    app::opt_vm.reset();
    app::depth = 0;
    app::cbuf.clear();
    app::err_line_start = true;
    app::carry_out.clear();
    app::carry_err.clear();
    { std::lock_guard<std::mutex> l(g_outq.m); g_outq.chunks.clear(); }
    app::shell->clear_all();
    app::shell->print(std::string("Brainfuck 1.0 (C++17 VM, mode=") +
                          (app::mode == RunMode::Manual ? "manual" : "optimize") +
                          ")\nType \"help()\", \"mem()\", \"reset()\" or \"quit()\" for more information.\n",
                      'C');
    show_prompt();
}

// ---------------- 파일 ----------------
static bool write_to(const std::string& p) {
    std::ofstream out(p, std::ios::binary);
    if (!out) { fl_alert("Failed to save: %s", p.c_str()); return false; }
    char* t = app::edbuf.text();
    out << (t ? t : "");
    free(t);
    app::path = p;
    app::modified = false;
    update_title();
    return true;
}

static bool choose_file(bool save, std::string& out) {
    Fl_Native_File_Chooser fc;
    fc.type(save ? Fl_Native_File_Chooser::BROWSE_SAVE_FILE : Fl_Native_File_Chooser::BROWSE_FILE);
    fc.filter("Brainfuck\t*.{bf,b}\nAll\t*");
    if (save) fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
    if (fc.show() != 0) return false;
    out = fc.filename();
    return true;
}

static bool save_as() {
    std::string p;
    if (!choose_file(true, p)) return false;
    if (p.find('.') == std::string::npos || p.find_last_of("/\\") > p.find_last_of('.')) p += ".bf";
    return write_to(p);
}
static bool save_file() { return app::path.empty() ? save_as() : write_to(app::path); }

static bool maybe_save() {
    if (!app::modified) return true;
    int r = fl_choice("Save Changes?", "Cancel", "Save", "Discard");
    if (r == 1) return save_file();
    return r == 2;
}

static void load_text(const std::string& text, const std::string& p) {
    app::loading = true;
    app::edbuf.text(text.c_str());
    app::loading = false;
    app::path = p;
    app::modified = false;
    update_title();
}

static void cb_new(Fl_Widget*, void*) { if (maybe_save()) load_text("", ""); }
static void cb_open(Fl_Widget*, void*) {
    if (!maybe_save()) return;
    std::string p;
    if (!choose_file(false, p)) return;
    try { load_text(read_file_binary(p), p); }
    catch (const std::exception& e) { fl_alert("%s", e.what()); }
}
static void cb_save(Fl_Widget*, void*) { save_file(); }
static void cb_save_as(Fl_Widget*, void*) { save_as(); }

// ---------------- 실행 ----------------
static void cb_run(Fl_Widget*, void*) {
    if (app::running) return;
    if (!save_file()) return;
    const std::string p = app::path;
    const RunMode mode = app::mode;
    app::manual_vm.reset();
    app::opt_vm.reset();
    app::depth = 0;
    app::cbuf.clear();
    app::shell->print("\n=== RESTART: " + p + " ===\n", 'E');
    start_worker([p, mode] { run_file_gui(p, mode); });
}
static void cb_stop(Fl_Widget*, void*) { do_stop(); }
static void cb_restart(Fl_Widget*, void*) { restart_shell(); }
static void cb_mode(Fl_Widget*, void* d) {
    app::mode = (reinterpret_cast<intptr_t>(d) == 1) ? RunMode::Manual : RunMode::Optimize;
    app::shell->print(std::string("mode=") + (app::mode == RunMode::Manual ? "manual" : "optimize") + "\n", 'C');
}

static void cb_close(Fl_Widget*, void*) {
    if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return; // Esc 로 닫히지 않게
    if (!maybe_save()) return;
    stop_worker_blocking();
    app::win->hide();
}

// 셸에서 한 줄 입력됨 (원본 main 의 REPL 루프와 같은 규칙)
static void on_line(const std::string& line) {
    if (app::running) { // 실행 중이면 프로그램의 표준 입력(,)으로 전달
        g_inbuf.push(line + "\n");
        return;
    }
    size_t a = line.find_first_not_of(" \t\r\n");
    const std::string cmd = a == std::string::npos ? "" : line.substr(a, line.find_last_not_of(" \t\r\n") - a + 1);

    if (app::depth == 0) {
        if (cmd == "quit()" || cmd == "exit()") { cb_close(nullptr, nullptr); return; }
        if (cmd == "help()") { print_help(); show_prompt(); return; }
        if (cmd == "mem()") {
            if (app::mode == RunMode::Manual) app::manual_vm.dump_memory();
            else app::opt_vm.dump_memory();
            show_prompt();
            return;
        }
        if (cmd == "reset()") {
            if (app::mode == RunMode::Manual) app::manual_vm.reset();
            else app::opt_vm.reset();
            show_prompt();
            return;
        }
    }

    bool unmatched = false;
    for (char c : line) {
        if (c == '[') ++app::depth;
        else if (c == ']' && --app::depth < 0) { unmatched = true; break; }
    }
    if (unmatched) {
        print_traceback("<stdin>", "SyntaxError", "unmatched ']'");
        app::cbuf.clear();
        app::depth = 0;
        show_prompt();
        return;
    }

    app::cbuf += line + "\n";
    if (app::depth > 0) { show_prompt(); return; }

    const std::string code = app::cbuf;
    app::cbuf.clear();
    if (trim(code).empty()) { show_prompt(); return; }

    const bool manual = (app::mode == RunMode::Manual);
    start_worker([code, manual] {
        try {
            if (manual) app::manual_vm.run(code, true);
            else app::opt_vm.run(code, true);
        } catch (const KeyboardInterrupt&) {
            print_traceback("<stdin>", "KeyboardInterrupt");
        } catch (const BrainfuckError& e) {
            print_traceback("<stdin>", e.type(), e.what(), e.instr(), e.has_pos());
        } catch (const std::exception& e) {
            std::cerr << "bfint: " << e.what() << "\n";
        }
    });
}

// ---------------- Help / About IDLE ----------------
static std::string compiler_string() {
#if defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

static const char* platform_string() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "unknown";
#endif
}

static std::string about_page(int page) {
    switch (page) {
    case 0: {
#ifdef NDEBUG
        const char* build_type = "Release (optimized)";
#else
        const char* build_type = "Debug (NOT optimized - expect slow execution)";
#endif
        const bool manual = (app::mode == RunMode::Manual);
        std::string s;
        s += "Brainfuck IDLE 1.0\n\n";
        s += "An IDLE-style editor and interactive shell for Brainfuck,\n";
        s += "built on a C++17 virtual machine with an optimizing compiler.\n\n";
        s += "Build\n";
        s += std::string("  Type        : ") + build_type + "\n";
        s += "  Compiler    : " + compiler_string() + "\n";
        s += "  GUI toolkit : FLTK " + std::to_string(FL_MAJOR_VERSION) + "." +
             std::to_string(FL_MINOR_VERSION) + "." + std::to_string(FL_PATCH_VERSION) + "\n";
        s += std::string("  Built on    : ") + __DATE__ + " " + __TIME__ + "\n";
        s += std::string("  Platform    : ") + platform_string() + "\n\n";
        s += "Current session\n";
        s += std::string("  Mode        : ") + (manual ? "manual (1:1 execution)" : "optimize (.bfc cache)") + "\n";
        s += "  File        : " + (app::path.empty() ? std::string("(untitled)") : app::path) + "\n";
        if (!manual && !app::path.empty()) s += "  Cache file  : " + make_bfc_path(app::path) + "\n";
        s += std::string("  Running     : ") + (app::running ? "yes" : "no") + "\n\n";
        s += "Virtual machine\n";
        s += "  Tape        : 32,768 cells, 8-bit unsigned (wraps 0-255)\n";
        s += "  Pointer     : leaving the tape raises MemoryError\n";
        s += "  Input (,)   : at EOF the cell is set to 0\n";
        s += "  .bfc cache  : format BFC2, validated by the SHA-256 of the source\n";
        return s;
    }
    case 1:
        return
            "Brainfuck commands\n"
            "  >   move the pointer right\n"
            "  <   move the pointer left\n"
            "  +   increment the current cell (wraps at 255)\n"
            "  -   decrement the current cell (wraps at 0)\n"
            "  .   output the current cell as a byte\n"
            "  ,   read one byte into the current cell\n"
            "  [   jump past the matching ] if the cell is 0\n"
            "  ]   jump back to the matching [ if the cell is not 0\n"
            "  Any other character is treated as a comment.\n\n"
            "Shell commands (at the >>> prompt)\n"
            "  help()   show the command list\n"
            "  mem()    show tape values around the pointer\n"
            "  reset()  clear all 32,768 cells and the pointer\n"
            "  quit()   close the window (also exit())\n\n"
            "Errors\n"
            "  SyntaxError       unmatched ']' or a '[' that is never closed\n"
            "  MemoryError       tape pointer overflow / underflow\n"
            "  KeyboardInterrupt execution stopped by the user\n"
            "  The traceback shows the instruction index of the original source.\n\n"
            "Optimizations (optimize mode)\n"
            "  - runs of + / - and same-direction moves are merged\n"
            "  - [-] and [+] become a single clear\n"
            "  - copy / multiply loops such as [->+++<] are folded\n"
            "  - scan loops such as [>] and [<<] are folded\n"
            "  - behavior, errors and tape state stay identical to manual mode\n";
    case 2:
        return
            "Keyboard shortcuts\n\n"
            "File\n"
            "  Ctrl+N          new file\n"
            "  Ctrl+O          open\n"
            "  Ctrl+S          save\n"
            "  Ctrl+Shift+S    save as\n"
            "  Ctrl+Q          quit\n\n"
            "Run\n"
            "  F5              run module (saves first, restarts the shell)\n"
            "  Ctrl+Shift+C    stop (KeyboardInterrupt)\n"
            "  Ctrl+F6         restart shell\n\n"
            "Shell\n"
            "  Enter           run the line (or send it to the program as input)\n"
            "  Up / Down       command history\n"
            "  Home            go to the start of the input\n"
            "  Ctrl+C          stop the running program (copy if text is selected)\n"
            "  Ctrl+D          send EOF to the program (close when idle)\n";
    default:
        return
            "Credits\n"
            "  Prompted by MyongNyang\n\n"
            "Interpreter core\n"
            "  bfint: manual VM, optimizing compiler, .bfc cache (C++17)\n\n"
            "GUI\n"
            "  FLTK - Fast Light Toolkit (www.fltk.org),\n"
            "  licensed under the GNU LGPL v2 with exceptions.\n\n"
            "Design\n"
            "  Layout and behavior follow Python's IDLE: an editor next to an\n"
            "  interactive shell, F5 to run, Ctrl+C to interrupt.\n"
            "  Standard streams are redirected so the interpreter core runs\n"
            "  unmodified and its output and tracebacks appear in the shell.\n\n"
            "Compiled on "__DATE__" at "__TIME__"\n";
    }
}

struct AboutCtx {
    Fl_Text_Buffer* buf;
    Fl_Text_Display* disp;
};

static void about_page_cb(Fl_Widget* b, void* d) {
    AboutCtx* c = static_cast<AboutCtx*>(b->window()->user_data());
    c->buf->text(about_page(static_cast<int>(reinterpret_cast<intptr_t>(d))).c_str());
    c->disp->scroll(0, 0);
}

static void cb_about(Fl_Widget*, void*) {
    Fl_Text_Buffer buf;
    AboutCtx ctx{&buf, nullptr};

    const int W = 640, H = 500;
    Fl_Window* w = new Fl_Window(W, H, "About IDLE");
    w->user_data(&ctx);

    Fl_Box* title = new Fl_Box(10, 10, W - 20, 34, "Brainfuck IDLE");
    title->labelfont(FL_HELVETICA_BOLD);
    title->labelsize(22);
    title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    Fl_Box* sub = new Fl_Box(10, 44, W - 20, 20, "version 1.0  -  C++17 VM + FLTK");
    sub->labelsize(12);
    sub->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    Fl_Text_Display* disp = new Fl_Text_Display(10, 74, W - 20, H - 74 - 56);
    disp->buffer(&buf);
    disp->textfont(FL_COURIER);
    disp->textsize(13);
    disp->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    ctx.disp = disp;

    const char* names[] = {"About", "Commands", "Shortcuts", "Credits"};
    const int bw = 110, by = H - 44;
    for (int i = 0; i < 4; ++i) {
        Fl_Button* b = new Fl_Button(10 + i * (bw + 8), by, bw, 32, names[i]);
        b->callback(about_page_cb, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    }
    Fl_Button* close = new Fl_Button(W - 10 - bw, by, bw, 32, "Close");
    close->callback([](Fl_Widget* b, void*) { b->window()->hide(); });
    w->end();

    buf.text(about_page(0).c_str());
    w->set_modal();
    w->show();
    while (w->shown()) Fl::wait();
    delete w;
}

int main() {
    static SinkBuf out_buf(false), err_buf(true);
    std::streambuf* old_out = std::cout.rdbuf(&out_buf);
    std::streambuf* old_err = std::cerr.rdbuf(&err_buf);
    std::streambuf* old_in = std::cin.rdbuf(&g_inbuf);

    Fl::scheme("gtk+");
    const int W = 1240, H = 720, MH = 26, SH = 22;
    app::win = new Fl_Double_Window(W, H);
    app::win->callback(cb_close);

    auto* menu = new Fl_Menu_Bar(0, 0, W, MH);
    menu->add("File/New File", FL_CTRL + 'n', cb_new);
    menu->add("File/Open...", FL_CTRL + 'o', cb_open);
    menu->add("File/Save", FL_CTRL + 's', cb_save);
    menu->add("File/Save As...", FL_CTRL + FL_SHIFT + 's', cb_save_as);
    menu->add("File/Quit", FL_CTRL + 'q', [](Fl_Widget*, void*) { cb_close(nullptr, nullptr); });
    menu->add("Run/Run Module", FL_F + 5, cb_run);
    menu->add("Run/Stop (KeyboardInterrupt)", FL_CTRL + FL_SHIFT + 'c', cb_stop);
    menu->add("Run/Restart Shell", FL_CTRL + FL_F + 6, cb_restart);
    menu->add("Options/Optimize Mode (.bfc Cache)", 0, cb_mode, reinterpret_cast<void*>(0), FL_MENU_RADIO | FL_MENU_VALUE);
    menu->add("Options/Manual Mode (Direct Execution)", 0, cb_mode, reinterpret_cast<void*>(1), FL_MENU_RADIO);
    menu->add("Help/About IDLE", 0, cb_about);

    const int th = H - MH - SH;
    auto* tile = new Fl_Tile(0, MH, W, th);
    app::editor = new Fl_Text_Editor(0, MH, 560, th);
    app::editor->buffer(&app::edbuf);
    app::editor->textfont(FL_COURIER);
    app::editor->textsize(14);
    app::editor->highlight_data(&app::edstyle, kEditStyles, 5, 'A', nullptr, nullptr);
    app::edbuf.add_modify_callback(editor_modify_cb, nullptr);

    app::shell = new Shell(560, MH, W - 560, th);
    tile->end();

    app::status = new Fl_Box(0, H - SH, W, SH);
    app::status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    app::status->labelsize(12);

    app::win->end();
    app::win->resizable(tile);

    app::shell->on_line = on_line;
    app::shell->on_interrupt = do_stop;
    app::shell->on_eof = [] {
        if (app::running) g_inbuf.send_eof();
        else cb_close(nullptr, nullptr);
    };

    load_text("","");
    restyle();
    app::win->show();
    Fl::focus(app::shell);
    restart_shell();
    Fl::add_timeout(0.015, tick);
    int rc = Fl::run();

    stop_worker_blocking();
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    std::cin.rdbuf(old_in);
    return rc;
}