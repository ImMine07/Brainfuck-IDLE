#include <algorithm>
#include <csignal>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t g_interrupted = 0;

void handle_sigint(int) {
    g_interrupted = 1;
    std::signal(SIGINT, handle_sigint);
}

enum class RunMode {
    Optimize, // 기본값: 단순화 후 .bfc 캐시 생성/사용
    Manual    // -manual: 원본 명령어 1:1 실행
};

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

int norm_byte_delta(int d) {
    d %= 256;
    if (d > 128) d -= 256;
    if (d <= -128) d += 256;
    return d;
}

std::string make_bfc_path(const std::string& bf_path) {
    size_t sep = bf_path.find_last_of("/\\");
    size_t dot = bf_path.find_last_of('.');
    if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) {
        return bf_path.substr(0, dot) + ".bfc";
    }
    return bf_path + ".bfc";
}

bool has_bfc_extension(const std::string& path) {
    return path.size() >= 4 && path.substr(path.size() - 4) == ".bfc";
}

bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

std::string read_file_text(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("can't open file '" + path + "'");
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

class KeyboardInterrupt : public std::runtime_error {
public:
    KeyboardInterrupt() : std::runtime_error("KeyboardInterrupt") {}
};

class BrainfuckError : public std::runtime_error {
public:
    BrainfuckError(std::string type, std::string msg, size_t instr = 0, bool has_pos = false)
        : std::runtime_error(std::move(msg)),
          type_(std::move(type)),
          instr_(instr),
          has_pos_(has_pos) {}

    const std::string& type() const { return type_; }
    size_t instr() const { return instr_; }
    bool has_pos() const { return has_pos_; }

private:
    std::string type_;
    size_t instr_;
    bool has_pos_;
};

void print_traceback(const std::string& source_name, const std::string& err_type,
                     const std::string& detail = "", size_t instr = 0, bool has_pos = false) {
    std::cerr << "Traceback (most recent call last):\n";
    std::cerr << "  File \"" << source_name << "\"";
    if (has_pos) {
        std::cerr << ", at instruction " << instr;
    }
    std::cerr << "\n" << err_type;
    if (!detail.empty()) {
        std::cerr << ": " << detail;
    }
    std::cerr << "\n";
}

} // namespace

// ============================================================================
// 1. 기존 방식 인터프리터 (Manual VM: 원본 8개 명령어를 1:1로 실행)
// ============================================================================
class ManualInterpreter {
public:
    explicit ManualInterpreter(size_t tape_size = 32768)
        : tape_(tape_size, 0), ptr_(0) {}

    void run(const std::string& raw_code, bool repl_mode = false) {
        std::string code;
        code.reserve(raw_code.size());
        for (char c : raw_code) {
            if (c == '>' || c == '<' || c == '+' || c == '-' ||
                c == '.' || c == ',' || c == '[' || c == ']') {
                code.push_back(c);
            }
        }

        std::vector<size_t> jump_table(code.size(), 0);
        std::stack<size_t> bracket_stack;

        for (size_t i = 0; i < code.size(); ++i) {
            if (code[i] == '[') {
                bracket_stack.push(i);
            } else if (code[i] == ']') {
                if (bracket_stack.empty()) {
                    throw BrainfuckError("SyntaxError", "unmatched ']'", i, true);
                }
                size_t open_idx = bracket_stack.top();
                bracket_stack.pop();
                jump_table[open_idx] = i;
                jump_table[i] = open_idx;
            }
        }

        if (!bracket_stack.empty()) {
            throw BrainfuckError("SyntaxError", "'[' was never closed", bracket_stack.top(), true);
        }

        g_interrupted = 0;
        bool used_input = false;
        int last_input = '\n';
        bool printed = false;
        bool last_out_nl = true;

        for (size_t pc = 0; pc < code.size(); ++pc) {
            if (g_interrupted) {
                g_interrupted = 0;
                if (repl_mode && printed && !last_out_nl) std::cout.put('\n');
                throw KeyboardInterrupt();
            }

            switch (code[pc]) {
                case '>':
                    if (ptr_ + 1 >= tape_.size()) {
                        throw BrainfuckError("MemoryError", "tape pointer overflow", pc, true);
                    }
                    ++ptr_;
                    break;
                case '<':
                    if (ptr_ == 0) {
                        throw BrainfuckError("MemoryError", "tape pointer underflow (ptr < 0)", pc, true);
                    }
                    --ptr_;
                    break;
                case '+':
                    ++tape_[ptr_];
                    break;
                case '-':
                    --tape_[ptr_];
                    break;
                case '.': {
                    char out = static_cast<char>(tape_[ptr_]);
                    std::cout.put(out);
                    std::cout.flush();
                    printed = true;
                    last_out_nl = (out == '\n');
                    break;
                }
                case ',': {
                    used_input = true;
                    int ch = std::cin.get();
                    last_input = ch;
                    tape_[ptr_] = (ch != std::char_traits<char>::eof()) ? static_cast<uint8_t>(ch) : 0;
                    break;
                }
                case '[':
                    if (tape_[ptr_] == 0) pc = jump_table[pc];
                    break;
                case ']':
                    if (tape_[ptr_] != 0) pc = jump_table[pc];
                    break;
            }
        }

        finalize_repl(repl_mode, printed, last_out_nl, used_input, last_input);
    }

    void reset() {
        std::fill(tape_.begin(), tape_.end(), 0);
        ptr_ = 0;
    }

    void dump_memory(size_t window = 8) const {
        print_tape_window(tape_, ptr_, window);
    }

    static void print_tape_window(const std::vector<uint8_t>& tape, size_t ptr, size_t window) {
        size_t start = (ptr > window / 2) ? ptr - window / 2 : 0;
        size_t end = std::min(tape.size(), start + window);

        std::cout << "Tape(ptr=" << ptr << ", size=" << tape.size() << "): ";
        for (size_t i = start; i < end; ++i) {
            if (i == ptr) std::cout << "[";
            std::cout << i << "=" << static_cast<int>(tape[i]);
            if (tape[i] >= 32 && tape[i] <= 126) {
                std::cout << "('" << static_cast<char>(tape[i]) << "')";
            }
            if (i == ptr) std::cout << "]";
            if (i + 1 < end) std::cout << ", ";
        }
        std::cout << "\n";
    }

    static void finalize_repl(bool repl_mode, bool printed, bool last_out_nl,
                              bool used_input, int last_input) {
        if (!repl_mode) return;
        if (printed && !last_out_nl) std::cout.put('\n');
        if (used_input && last_input != '\n' && std::cin.good()) {
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }
        if (used_input && std::cin.eof()) {
            std::cin.clear();
        }
    }

private:
    std::vector<uint8_t> tape_;
    size_t ptr_;
};

// ============================================================================
// 2. 최적화 인터프리터 (Optimize VM: 명령어 압축·루프 단순화 + .bfc 저장/로드)
//
// 설계 원칙: 최적화 전(Manual)과 "관찰 가능한 동작"이 완전히 같아야 한다.
//   - 출력/입력, 테이프 값, 포인터 위치가 같다 (오류로 중단된 경우 포함).
//   - 오류 종류/메시지/"at instruction N"의 N(원본 명령어 인덱스)이 같다.
//   - 이를 위해 이동 명령은 방향이 같은 연속 구간만 압축하고(><는 합치지 않음),
//     포인터를 이동하는 루프 패턴(Mul/Clear)은 Guard 로 "방문하는 모든 셀이
//     테이프 안에 있음"을 확인한 뒤에만 빠른 경로를 쓰고, 아니면 원본 루프를
//     그대로 실행(fallback)한다.
// ============================================================================

// ---------------------------------------------------------------------------
// SHA-256 (소스 파일 변경 감지용)
// ---------------------------------------------------------------------------
class Sha256 {
public:
    static std::string hex_digest(const std::string& data) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

        std::string msg = data;
        const uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8;
        msg.push_back(static_cast<char>(0x80));
        while (msg.size() % 64 != 56) msg.push_back('\0');
        for (int i = 7; i >= 0; --i) {
            msg.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xff));
        }

        for (size_t off = 0; off < msg.size(); off += 64) {
            uint32_t w[64];
            for (int i = 0; i < 16; ++i) {
                w[i] = (static_cast<uint32_t>(static_cast<uint8_t>(msg[off + i * 4])) << 24) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(msg[off + i * 4 + 1])) << 16) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(msg[off + i * 4 + 2])) << 8) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(msg[off + i * 4 + 3])));
            }
            for (int i = 16; i < 64; ++i) {
                uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
            uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
            for (int i = 0; i < 64; ++i) {
                uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                uint32_t ch = (e & f) ^ (~e & g);
                uint32_t t1 = hh + S1 + ch + K[i] + w[i];
                uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                uint32_t t2 = S0 + maj;
                hh = g; g = f; f = e; e = d + t1;
                d = c; c = b; b = a; a = t1 + t2;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d;
            h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
        }

        static const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (uint32_t v : h) {
            for (int i = 7; i >= 0; --i) out.push_back(hex[(v >> (i * 4)) & 0xf]);
        }
        return out;
    }

private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
};

std::string read_file_binary(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("can't open file '" + path + "'");
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::string dir_prefix(const std::string& path) {
    size_t sep = path.find_last_of("/\\");
    return sep == std::string::npos ? "" : path.substr(0, sep + 1);
}

std::string base_name(const std::string& path) {
    size_t sep = path.find_last_of("/\\");
    return sep == std::string::npos ? path : path.substr(sep + 1);
}

// ---------------------------------------------------------------------------
// Op 정의
// ---------------------------------------------------------------------------
enum class OpKind {
    Add,   // a: delta (mod 256)
    Move,  // a: offset (방향이 하나인 연속 구간), pos: 원본 첫 명령어 인덱스
    Clear, // tape[ptr] = 0
    Mul,   // a: target offset, b: factor (tape[ptr + a] += tape[ptr] * b)
    Scan,  // a: stride, pos: 원본 이동 구간 인덱스 (while (tape[ptr] != 0) ptr += a)
    Out,
    In,
    Jz,    // target: jump index
    Jnz,   // target: jump index
    Guard, // a: lo, b: hi. [ptr+lo, ptr+hi]가 테이프 안이면 다음 op(빠른 경로),
           //   아니면 target(원본 루프 fallback)으로 점프
    Jmp    // target: 무조건 점프 (빠른 경로 뒤에서 fallback 건너뛰기)
};

struct Op {
    OpKind kind;
    int a = 0;
    int b = 0;
    size_t target = 0;
    size_t pos = 0; // 오류 보고용 원본 명령어 인덱스 (Move/Scan)
};

// .bfc 파일 내용
struct BfcFile {
    std::string source_name; // 원본 파일 이름 (경로 제외)
    std::string hash;        // 원본 파일의 SHA-256 (hex)
    std::vector<Op> ops;
};

class OptimizedInterpreter {
public:
    explicit OptimizedInterpreter(size_t tape_size = 32768)
        : tape_(tape_size, 0), ptr_(0) {}

    // 원본 Brainfuck 코드를 단순화하여 Op 시퀀스로 컴파일
    static std::vector<Op> compile(const std::string& raw_code) {
        std::string code;
        code.reserve(raw_code.size());
        for (char c : raw_code) {
            if (c == '>' || c == '<' || c == '+' || c == '-' ||
                c == '.' || c == ',' || c == '[' || c == ']') {
                code.push_back(c);
            }
        }

        // Pass 1: 연속된 + / - 는 하나로, 같은 방향의 > / < 는 하나로 압축.
        //  - + 와 - 는 경계 검사가 없으므로 섞어서 합쳐도 동일.
        //  - > 와 < 는 방향이 바뀌는 지점마다 경계 검사가 필요하므로
        //    같은 방향 연속 구간만 합친다 ("<>" 를 지우면 underflow 가 사라짐).
        std::vector<Op> pass1;
        std::stack<std::pair<size_t, size_t>> stk1; // (op index, 원본 코드 index)

        for (size_t i = 0; i < code.size();) {
            char c = code[i];
            if (c == '+' || c == '-') {
                int delta = 0;
                while (i < code.size() && (code[i] == '+' || code[i] == '-')) {
                    delta += (code[i] == '+') ? 1 : -1;
                    ++i;
                }
                delta = norm_byte_delta(delta);
                if (delta != 0) {
                    pass1.push_back({OpKind::Add, delta, 0, 0, 0});
                }
            } else if (c == '>' || c == '<') {
                size_t start = i;
                int n = 0;
                while (i < code.size() && code[i] == c) {
                    ++n;
                    ++i;
                }
                pass1.push_back({OpKind::Move, (c == '>') ? n : -n, 0, 0, start});
            } else if (c == '.') {
                pass1.push_back({OpKind::Out});
                ++i;
            } else if (c == ',') {
                pass1.push_back({OpKind::In});
                ++i;
            } else if (c == '[') {
                stk1.push({pass1.size(), i});
                pass1.push_back({OpKind::Jz});
                ++i;
            } else { // ']'
                if (stk1.empty()) {
                    throw BrainfuckError("SyntaxError", "unmatched ']'", i, true);
                }
                size_t open_idx = stk1.top().first;
                stk1.pop();
                size_t close_idx = pass1.size();
                pass1.push_back({OpKind::Jnz, 0, 0, open_idx});
                pass1[open_idx].target = close_idx;
                ++i;
            }
        }

        if (!stk1.empty()) {
            // 원본 코드 기준 인덱스로 보고 (Manual 과 동일)
            throw BrainfuckError("SyntaxError", "'[' was never closed", stk1.top().second, true);
        }

        // Pass 2: 루프 패턴 단순화 (CLEAR, MUL, SCAN)
        std::vector<Op> optimized;
        optimized.reserve(pass1.size());
        for (size_t i = 0; i < pass1.size();) {
            if (pass1[i].kind != OpKind::Jz) {
                optimized.push_back(pass1[i]);
                ++i;
                continue;
            }

            size_t end_i = pass1[i].target;
            bool simple_body = true;
            for (size_t k = i + 1; k < end_i; ++k) {
                if (pass1[k].kind != OpKind::Add && pass1[k].kind != OpKind::Move) {
                    simple_body = false;
                    break;
                }
            }

            if (simple_body) {
                int cur_off = 0, lo = 0, hi = 0;
                std::map<int, int> deltas;
                for (size_t k = i + 1; k < end_i; ++k) {
                    if (pass1[k].kind == OpKind::Move) {
                        cur_off += pass1[k].a;
                        lo = std::min(lo, cur_off);
                        hi = std::max(hi, cur_off);
                    } else {
                        deltas[cur_off] += pass1[k].a;
                    }
                }

                std::vector<Op> fast;
                bool matched = false;
                bool is_scan = false;

                if (cur_off == 0 && deltas.count(0)) {
                    int self_delta = norm_byte_delta(deltas[0]);
                    if (deltas.size() == 1 && (self_delta == -1 || self_delta == 1)) {
                        // [-] 또는 [+] -> CLEAR
                        fast.push_back({OpKind::Clear});
                        matched = true;
                    } else if (self_delta == -1) {
                        // [->+++>+<<] 형태의 복사/곱셈 루프 -> MUL + CLEAR
                        for (const auto& [off, factor] : deltas) {
                            if (off == 0) continue;
                            int nf = norm_byte_delta(factor);
                            if (nf != 0) {
                                fast.push_back({OpKind::Mul, off, nf, 0, 0});
                            }
                        }
                        fast.push_back({OpKind::Clear});
                        matched = true;
                    }
                } else if (end_i == i + 2 && pass1[i + 1].kind == OpKind::Move) {
                    // [>], [<<] 형태 (본문이 이동 한 번뿐) -> SCAN
                    fast.push_back({OpKind::Scan, pass1[i + 1].a, 0, 0, pass1[i + 1].pos});
                    matched = true;
                    is_scan = true;
                }

                if (matched) {
                    if (is_scan || (lo == 0 && hi == 0)) {
                        // 이동이 없거나(Clear) Scan: 경계 검사가 런타임에 정확히 재현되므로 Guard 불필요
                        optimized.insert(optimized.end(), fast.begin(), fast.end());
                    } else {
                        // 루프가 방문하는 모든 셀 [ptr+lo, ptr+hi] 가 테이프 안일 때만 빠른 경로.
                        // 아니면 원본 루프를 그대로 실행해 오류 시점/상태까지 동일하게 만든다.
                        size_t guard_idx = optimized.size();
                        optimized.push_back({OpKind::Guard, lo, hi, 0, 0});
                        optimized.insert(optimized.end(), fast.begin(), fast.end());
                        size_t jmp_idx = optimized.size();
                        optimized.push_back({OpKind::Jmp});
                        optimized[guard_idx].target = optimized.size(); // fallback 시작
                        for (size_t k = i; k <= end_i; ++k) optimized.push_back(pass1[k]);
                        optimized[jmp_idx].target = optimized.size();   // fallback 다음
                    }
                    i = end_i + 1;
                    continue;
                }
            }

            // 위 패턴에 해당하지 않으면 일반 루프로 유지
            optimized.push_back(pass1[i]);
            ++i;
        }

        // Pass 3: 최종 명령어 배열 기준으로 Jz / Jnz 점프 인덱스 재계산
        // (Guard / Jmp 의 target 은 Pass 2 에서 최종 인덱스로 이미 기록됨)
        std::stack<size_t> stk2;
        for (size_t i = 0; i < optimized.size(); ++i) {
            if (optimized[i].kind == OpKind::Jz) {
                stk2.push(i);
            } else if (optimized[i].kind == OpKind::Jnz) {
                size_t open_idx = stk2.top();
                stk2.pop();
                optimized[open_idx].target = i;
                optimized[i].target = open_idx;
            }
        }

        return optimized;
    }

    // 단순화된 명령어를 원본 해시와 함께 .bfc 파일로 저장
    static void save_bfc(const std::string& bfc_path, const std::string& source_name,
                         const std::string& source_hash, const std::vector<Op>& ops) {
        std::ofstream out(bfc_path);
        if (!out) {
            throw std::runtime_error("cannot write optimized file '" + bfc_path + "'");
        }
        out << "# BFC2\n";
        out << "# SRC " << source_name << "\n";
        out << "# SHA256 " << source_hash << "\n";
        for (const auto& op : ops) {
            switch (op.kind) {
                case OpKind::Add:   out << "ADD " << op.a << "\n"; break;
                case OpKind::Move:  out << "MOVE " << op.a << " " << op.pos << "\n"; break;
                case OpKind::Clear: out << "CLEAR\n"; break;
                case OpKind::Mul:   out << "MUL " << op.a << " " << op.b << "\n"; break;
                case OpKind::Scan:  out << "SCAN " << op.a << " " << op.pos << "\n"; break;
                case OpKind::Out:   out << "OUT\n"; break;
                case OpKind::In:    out << "IN\n"; break;
                case OpKind::Jz:    out << "JZ " << op.target << "\n"; break;
                case OpKind::Jnz:   out << "JNZ " << op.target << "\n"; break;
                case OpKind::Guard: out << "GUARD " << op.a << " " << op.b << " " << op.target << "\n"; break;
                case OpKind::Jmp:   out << "JMP " << op.target << "\n"; break;
            }
        }
        out.flush();
        if (!out) {
            throw std::runtime_error("failed while writing optimized file '" + bfc_path + "'");
        }
    }

    // .bfc 파일을 읽어 복원 (헤더/해시가 없거나 형식이 다르면 예외)
    static BfcFile load_bfc(const std::string& bfc_path) {
        std::ifstream in(bfc_path);
        if (!in) {
            throw std::runtime_error("can't open file '" + bfc_path + "'");
        }

        std::string line;
        if (!std::getline(in, line) || trim(line) != "# BFC2") {
            throw BrainfuckError("SyntaxError", "invalid or outdated .bfc header in '" + bfc_path + "'");
        }

        BfcFile file;
        std::vector<Op>& ops = file.ops;
        while (std::getline(in, line)) {
            std::string t = trim(line);
            if (t.empty()) continue;
            if (t[0] == '#') {
                if (t.rfind("# SRC ", 0) == 0) file.source_name = trim(t.substr(6));
                else if (t.rfind("# SHA256 ", 0) == 0) file.hash = trim(t.substr(9));
                continue;
            }

            std::istringstream iss(t);
            std::string mnem;
            iss >> mnem;

            Op op{};
            if (mnem == "ADD") {
                if (!(iss >> op.a)) throw BrainfuckError("SyntaxError", "invalid ADD operand", ops.size(), true);
                op.kind = OpKind::Add;
            } else if (mnem == "MOVE") {
                if (!(iss >> op.a >> op.pos) || op.a == 0) throw BrainfuckError("SyntaxError", "invalid MOVE operand", ops.size(), true);
                op.kind = OpKind::Move;
            } else if (mnem == "CLEAR") {
                op.kind = OpKind::Clear;
            } else if (mnem == "MUL") {
                if (!(iss >> op.a >> op.b)) throw BrainfuckError("SyntaxError", "invalid MUL operands", ops.size(), true);
                op.kind = OpKind::Mul;
            } else if (mnem == "SCAN") {
                if (!(iss >> op.a >> op.pos) || op.a == 0) throw BrainfuckError("SyntaxError", "invalid SCAN operand", ops.size(), true);
                op.kind = OpKind::Scan;
            } else if (mnem == "OUT") {
                op.kind = OpKind::Out;
            } else if (mnem == "IN") {
                op.kind = OpKind::In;
            } else if (mnem == "JZ") {
                if (!(iss >> op.target)) throw BrainfuckError("SyntaxError", "invalid JZ target", ops.size(), true);
                op.kind = OpKind::Jz;
            } else if (mnem == "JNZ") {
                if (!(iss >> op.target)) throw BrainfuckError("SyntaxError", "invalid JNZ target", ops.size(), true);
                op.kind = OpKind::Jnz;
            } else if (mnem == "GUARD") {
                if (!(iss >> op.a >> op.b >> op.target)) throw BrainfuckError("SyntaxError", "invalid GUARD operands", ops.size(), true);
                op.kind = OpKind::Guard;
            } else if (mnem == "JMP") {
                if (!(iss >> op.target)) throw BrainfuckError("SyntaxError", "invalid JMP target", ops.size(), true);
                op.kind = OpKind::Jmp;
            } else {
                throw BrainfuckError("SyntaxError", "unknown opcode '" + mnem + "'", ops.size(), true);
            }
            ops.push_back(op);
        }

        if (file.hash.empty()) {
            throw BrainfuckError("SyntaxError", "missing source hash in '" + bfc_path + "'");
        }

        for (size_t i = 0; i < ops.size(); ++i) {
            const Op& op = ops[i];
            switch (op.kind) {
                case OpKind::Jz:
                case OpKind::Jnz:
                    if (op.target >= ops.size()) throw BrainfuckError("SyntaxError", "jump target out of range", i, true);
                    break;
                case OpKind::Guard:
                    if (op.target <= i || op.target >= ops.size()) throw BrainfuckError("SyntaxError", "jump target out of range", i, true);
                    break;
                case OpKind::Jmp:
                    if (op.target <= i || op.target > ops.size()) throw BrainfuckError("SyntaxError", "jump target out of range", i, true);
                    break;
                default:
                    break;
            }
        }
        return file;
    }

    void run_ops(const std::vector<Op>& ops, bool repl_mode = false) {
        g_interrupted = 0;
        bool used_input = false;
        int last_input = '\n';
        bool printed = false;
        bool last_out_nl = true;

        auto raise_interrupt = [&]() {
            g_interrupted = 0;
            if (repl_mode && printed && !last_out_nl) std::cout.put('\n');
            throw KeyboardInterrupt();
        };

        for (size_t pc = 0; pc < ops.size(); ++pc) {
            if (g_interrupted) raise_interrupt();

            const Op& op = ops[pc];
            switch (op.kind) {
                case OpKind::Add:
                    tape_[ptr_] = static_cast<uint8_t>(static_cast<int>(tape_[ptr_]) + op.a);
                    break;
                case OpKind::Move:
                    move_ptr(op.a, op.pos);
                    break;
                case OpKind::Clear:
                    tape_[ptr_] = 0;
                    break;
                case OpKind::Mul:
                    if (tape_[ptr_] != 0) {
                        long long dst = static_cast<long long>(ptr_) + op.a;
                        if (dst < 0 || dst >= static_cast<long long>(tape_.size())) {
                            // 정상적인 .bfc 에서는 Guard 때문에 도달하지 않음 (손상된 파일 방어)
                            throw BrainfuckError("MemoryError",
                                                 dst < 0 ? "tape pointer underflow (ptr < 0)"
                                                         : "tape pointer overflow",
                                                 pc, true);
                        }
                        int prod = static_cast<int>(tape_[ptr_]) * op.b;
                        tape_[dst] = static_cast<uint8_t>(static_cast<int>(tape_[dst]) + prod);
                    }
                    break;
                case OpKind::Scan:
                    while (tape_[ptr_] != 0) {
                        if (g_interrupted) raise_interrupt();
                        move_ptr(op.a, op.pos);
                    }
                    break;
                case OpKind::Out: {
                    char out = static_cast<char>(tape_[ptr_]);
                    std::cout.put(out);
                    std::cout.flush();
                    printed = true;
                    last_out_nl = (out == '\n');
                    break;
                }
                case OpKind::In: {
                    used_input = true;
                    int ch = std::cin.get();
                    last_input = ch;
                    tape_[ptr_] = (ch != std::char_traits<char>::eof()) ? static_cast<uint8_t>(ch) : 0;
                    break;
                }
                case OpKind::Jz:
                    if (tape_[ptr_] == 0) pc = op.target;
                    break;
                case OpKind::Jnz:
                    if (tape_[ptr_] != 0) pc = op.target;
                    break;
                case OpKind::Guard: {
                    long long p = static_cast<long long>(ptr_);
                    bool in_range = (p + op.a >= 0) &&
                                    (p + op.b < static_cast<long long>(tape_.size()));
                    if (!in_range) pc = op.target - 1; // fallback 원본 루프로
                    break;
                }
                case OpKind::Jmp:
                    pc = op.target - 1;
                    break;
            }
        }

        ManualInterpreter::finalize_repl(repl_mode, printed, last_out_nl, used_input, last_input);
    }

    void run(const std::string& raw_code, bool repl_mode = false) {
        std::vector<Op> ops = compile(raw_code);
        run_ops(ops, repl_mode);
    }

    void reset() {
        std::fill(tape_.begin(), tape_.end(), 0);
        ptr_ = 0;
    }

    void dump_memory(size_t window = 8) const {
        ManualInterpreter::print_tape_window(tape_, ptr_, window);
    }

private:
    // 방향이 하나인 이동 구간(offset)을 수행한다.
    // 경계를 벗어나면 Manual 과 똑같이: 포인터는 경계 셀(size-1 또는 0)까지 간 상태에서,
    // 원본에서 실제로 실패했을 명령어의 인덱스(pos 기준)로 오류를 던진다.
    void move_ptr(int offset, size_t pos) {
        if (offset > 0) {
            size_t step = static_cast<size_t>(offset);
            if (ptr_ + step >= tape_.size()) {
                size_t fail_step = tape_.size() - ptr_; // 1-based, 실패하는 '>' 번호
                size_t err_pos = pos + fail_step - 1;
                ptr_ = tape_.size() - 1;
                throw BrainfuckError("MemoryError", "tape pointer overflow", err_pos, true);
            }
            ptr_ += step;
        } else if (offset < 0) {
            size_t back = static_cast<size_t>(-static_cast<long long>(offset));
            if (back > ptr_) {
                size_t err_pos = pos + ptr_; // ptr_ 번 이동 후 (ptr_+1)번째 '<' 에서 실패
                ptr_ = 0;
                throw BrainfuckError("MemoryError", "tape pointer underflow (ptr < 0)", err_pos, true);
            }
            ptr_ -= back;
        }
    }

    std::vector<uint8_t> tape_;
    size_t ptr_;
};

void print_help() {
    std::cout
        << "Commands:\n"
        << "  mem()    Inspect tape values around the current pointer\n"
        << "  reset()  Reset all 30,000 tape cells to 0 and pointer to 0\n"
        << "  help()   Show this help message\n"
        << "  quit()   Exit the interpreter (or Ctrl+D / Ctrl+Z)\n";
}

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [<file.bf>] [-optimize | -manual]\n"
              << "  -optimize (default) : Load <file>.bfc if present and its recorded source hash matches the\n"
              << "                        source file; otherwise simplify the source and (re)generate <file>.bfc\n"
              << "  -manual             : Execute original Brainfuck instructions step by step\n";
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, handle_sigint);

    RunMode mode = RunMode::Optimize;
    std::string file_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-manual" || arg == "--manual") {
            mode = RunMode::Manual;
        } else if (arg == "-optimize" || arg == "--optimize") {
            mode = RunMode::Optimize;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 2;
        } else if (file_path.empty()) {
            file_path = arg;
        } else {
            std::cerr << "Too many file arguments.\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    // 1. 파일 실행 모드
    if (!file_path.empty()) {
        if (mode == RunMode::Manual) {
            try {
                std::string code = read_file_text(file_path);
                std::cerr << "[Manual] Running '" << file_path << "'\n";
                ManualInterpreter vm;
                vm.run(code);
            } catch (const KeyboardInterrupt&) {
                std::cerr << "\n";
                print_traceback(file_path, "KeyboardInterrupt");
                return 130;
            } catch (const BrainfuckError& e) {
                std::cerr << "\n";
                print_traceback(file_path, e.type(), e.what(), e.instr(), e.has_pos());
                return 1;
            } catch (const std::exception& e) {
                std::cerr << "bfint: " << e.what() << "\n";
                return 2;
            }
            return 0;
        }

        // Optimize 모드 (기본값)
        //  - <file>.bfc 가 있으면 안에 적힌 원본 해시(SHA-256)와 현재 원본 파일의 해시를 비교한다.
        //  - 일치하면 .bfc 를 그대로 사용, 불일치/손상/구버전이면 원본을 다시 컴파일해 .bfc 를 재생성한다.
        const bool direct_bfc = has_bfc_extension(file_path);
        std::string bfc_path = direct_bfc ? file_path : make_bfc_path(file_path);
        std::string src_path = direct_bfc ? "" : file_path; // .bfc 를 직접 실행하면 헤더의 SRC 로 추정
        std::string active_source = bfc_path;

        try {
            OptimizedInterpreter vm;
            std::vector<Op> ops;

            // (1) 기존 .bfc 읽기 시도
            BfcFile cached;
            bool have_cache = false;
            std::exception_ptr cache_error;
            const bool bfc_exists = file_exists(bfc_path);
            if (bfc_exists) {
                try {
                    cached = OptimizedInterpreter::load_bfc(bfc_path);
                    have_cache = true;
                } catch (const std::exception&) {
                    cache_error = std::current_exception();
                }
            }
            if (src_path.empty() && have_cache && !cached.source_name.empty()) {
                src_path = dir_prefix(bfc_path) + cached.source_name;
            }

            // (2) 원본 파일 읽기 + 해시
            bool have_source = false;
            std::string source_text;
            if (!src_path.empty() && file_exists(src_path)) {
                source_text = read_file_binary(src_path);
                have_source = true;
            }

            if (have_source) {
                const std::string hash = Sha256::hex_digest(source_text);
                if (have_cache && cached.hash == hash) {
                    ops = std::move(cached.ops);
                    std::cerr << "[Optimize] Loaded '" << bfc_path << "' (" << ops.size()
                              << " ops, source hash verified)\n";
                } else {
                    std::string reason;
                    if (!bfc_exists)        reason = "no cache";
                    else if (have_cache)    reason = "source hash mismatch";
                    else                    reason = "invalid cache";

                    active_source = src_path;
                    ops = OptimizedInterpreter::compile(source_text);
                    active_source = bfc_path;
                    std::cerr << "[Optimize] Compiled '" << src_path << "' -> '" << bfc_path
                              << "' (" << ops.size() << " ops, " << reason << ")\n";
                    try {
                        OptimizedInterpreter::save_bfc(bfc_path, base_name(src_path), hash, ops);
                    } catch (const std::exception& e) {
                        std::cerr << "[Optimize] Warning: " << e.what() << " (running without cache)\n";
                    }
                }
            } else if (have_cache) {
                // 원본이 없으면 검증할 수 없으므로 .bfc 를 그대로 사용
                ops = std::move(cached.ops);
                std::cerr << "[Optimize] Loaded '" << bfc_path << "' (" << ops.size()
                          << " ops, source not found: hash not verified)\n";
            } else if (cache_error) {
                std::rethrow_exception(cache_error);
            } else {
                throw std::runtime_error("can't open file '" + file_path + "'");
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

    // 2. 인자가 없을 경우 대화형 REPL 진입
    ManualInterpreter manual_vm;
    OptimizedInterpreter opt_vm;

    std::cout << "Brainfuck 1.0 (C++17 VM, mode="
              << (mode == RunMode::Manual ? "manual" : "optimize") << ")\n"
              << "Type \"help()\", \"mem()\", \"reset()\" or \"quit()\" for more information.\n";

    std::string code_buffer;
    int bracket_depth = 0;
    std::string line;

    while (true) {
        std::cout << (bracket_depth > 0 ? "... " : ">>> ") << std::flush;

        if (!std::getline(std::cin, line)) {
            if (g_interrupted) {
                g_interrupted = 0;
                std::cin.clear();
                code_buffer.clear();
                bracket_depth = 0;
                std::cout << "\nKeyboardInterrupt\n";
                continue;
            }
            std::cout << "\n";
            break;
        }

        std::string cmd = trim(line);
        if (bracket_depth == 0) {
            if (cmd == "quit()" || cmd == "exit()") break;
            if (cmd == "help()") { print_help(); continue; }
            if (cmd == "mem()") {
                if (mode == RunMode::Manual) manual_vm.dump_memory();
                else opt_vm.dump_memory();
                continue;
            }
            if (cmd == "reset()") {
                if (mode == RunMode::Manual) manual_vm.reset();
                else opt_vm.reset();
                continue;
            }
        }

        bool unmatched_close = false;
        for (char c : line) {
            if (c == '[') ++bracket_depth;
            else if (c == ']' && --bracket_depth < 0) {
                unmatched_close = true;
                break;
            }
        }

        if (unmatched_close) {
            print_traceback("<stdin>", "SyntaxError", "unmatched ']'");
            code_buffer.clear();
            bracket_depth = 0;
            continue;
        }

        code_buffer += line;
        code_buffer.push_back('\n');
        if (bracket_depth > 0) continue;

        if (!trim(code_buffer).empty()) {
            try {
                if (mode == RunMode::Manual) manual_vm.run(code_buffer, true);
                else opt_vm.run(code_buffer, true);
            } catch (const KeyboardInterrupt&) {
                print_traceback("<stdin>", "KeyboardInterrupt");
            } catch (const BrainfuckError& e) {
                print_traceback("<stdin>", e.type(), e.what(), e.instr(), e.has_pos());
            }
        }
        code_buffer.clear();
    }

    return 0;
}