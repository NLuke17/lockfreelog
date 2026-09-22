// L6 drill — Compile-time format parsing, FROM SCRATCH.
// Compile: clang++ -std=c++20 -O2 -Wall -Wextra L6_compile_time_format.cpp -o L6 && ./L6
//
// Goal: rebuild LOG<"x={} y={}">(buf, cap, 42, 7) yourself, end to end. You write
// EVERY implementation. The harness (main) and the signatures are given.
//
// Remember the split:
//   COMPILE TIME  = the format string (a literal at the call site) is parsed &
//                   validated. Wrong arity => compile error.
//   RUNTIME       = the arguments (runtime values) get spliced into the literal.
//
// The five pieces you'll build:
//   1. FixedString<N>  : wrap a string literal so it can be a template argument (NTTP)
//   2. placeholders()  : consteval count of "{}"
//   3. emit_arg<T>     : convert ONE arg into the buffer (if constexpr dispatch)
//   4. format_impl     : variadic recursion — literal chunk, arg, recurse (+ base case)
//   5. LOG<Fmt>(...)   : static_assert arity, then run format_impl

#include <cstddef>
#include <cstring>
#include <cstdio>
#include <charconv>
#include <string_view>
#include <type_traits>

// ============================================================================
// 1. FixedString<N> — a structural type usable as a non-type template parameter.
//    C++20 lets a class be an NTTP if it's "structural" (public members, etc.).
//    This is what lets us write LOG<"literal">.
// ============================================================================
template <std::size_t N>
struct FixedString {
    char data[N]{};

    // TASK 1a: consteval constructor that copies the N chars of the literal `s`
    // into data[]. Signature takes a reference to a char array: const char (&s)[N].
    // TODO
    consteval FixedString(const char (&s)[N]) {
        // TODO: copy s[0..N) into data
    }

    // TASK 1b: return a string_view over the literal WITHOUT the trailing '\0'.
    // (the literal "ab" is stored as {'a','b','\0'}, so its length is N-1)
    // TODO
    constexpr std::string_view view() const {
        return {}; // TODO
    }

    // TASK 2: consteval — count the number of "{}" placeholders in data[].
    // Only "{}" counts; don't worry about escapes. Return the count.
    // TODO
    consteval std::size_t placeholders() const {
        return 0; // TODO
    }
};

// ============================================================================
// 3. emit_arg<T> — write ONE argument into buf at p; return the new write pointer.
//    Use if constexpr to pick the path at COMPILE time per type:
//      - integral T  -> std::to_chars(p, end, arg); return result.ptr
//      - otherwise   -> treat as const char*: strlen + memcpy; return p + n
//    Why if constexpr (not if): the untaken branch is DISCARDED, so to_chars is
//    never compiled for a string and the char* path is never compiled for an int.
// ============================================================================
template <class T>
static char* emit_arg(char* p, char* end, const T& arg) {
    // TODO
    (void)end; (void)arg;
    return p;
}

// ============================================================================
// 4. format_impl — walk the format, splice args. TWO overloads:
//
//    (base case) no args left: copy the remainder of fmt verbatim into p, return p+n.
//
//    (recursive) at least one arg: find the next "{}", copy the literal chunk before
//    it, emit_arg the first argument, then RECURSE on fmt after the "{}" with the
//    remaining args.
//
//    Hint: std::string_view::find("{}") returns the index; substr(pos+2) skips "{}".
// ============================================================================

// base case
static char* format_impl(char* p, char* end, std::string_view fmt) {
    // TODO: copy the rest of fmt into p (there are no more placeholders to fill)
    (void)end;
    return p;
}

// recursive case
template <class T, class... Rest>
static char* format_impl(char* p, char* end, std::string_view fmt,
                         const T& arg, const Rest&... rest) {
    // TODO:
    //  - pos = fmt.find("{}")
    //  - memcpy the [0, pos) literal chunk into p, advance p
    //  - p = emit_arg(p, end, arg)
    //  - return format_impl(p, end, fmt.substr(pos + 2), rest...)
    (void)end; (void)arg;
    return p;
}

// ============================================================================
// 5. LOG<Fmt>(buf, cap, args...) — the public entry point.
//    Fmt is a COMPILE-TIME template parameter (a FixedString).
// ============================================================================
template <FixedString Fmt, class... Args>
std::size_t LOG(char* buf, std::size_t cap, const Args&... args) {
    // TASK 5: static_assert that the number of "{}" in Fmt equals the number of args.
    // This is what makes a wrong-arity LOG a COMPILE error.
    // TODO

    char* p   = buf;
    char* end = buf + cap;
    p = format_impl(p, end, Fmt.view(), args...);
    return static_cast<std::size_t>(p - buf);
}

// ------------------------------- tests -------------------------------
int main() {
    char buf[256];

    std::size_t n1 = LOG<"x={} y={}">(buf, sizeof buf, 42, 7);
    std::printf("[%.*s]  (expect [x=42 y=7])\n", (int)n1, buf);

    std::size_t n2 = LOG<"user {} from {}">(buf, sizeof buf, "alice", "10.0.0.1");
    std::printf("[%.*s]  (expect [user alice from 10.0.0.1])\n", (int)n2, buf);

    std::size_t n3 = LOG<"no placeholders">(buf, sizeof buf);
    std::printf("[%.*s]  (expect [no placeholders])\n", (int)n3, buf);

    std::size_t n4 = LOG<"{} + {} = {}">(buf, sizeof buf, 2, 3, 5);
    std::printf("[%.*s]  (expect [2 + 3 = 5])\n", (int)n4, buf);

    // After it all works, PROVE compile-time validation: uncomment, must NOT compile.
    // LOG<"x={} y={}">(buf, sizeof buf, 42);       // 2 placeholders, 1 arg
    // LOG<"just {}">(buf, sizeof buf, 1, 2);       // 1 placeholder, 2 args

    std::printf("\nAll four printed correctly? Then uncomment a bad LOG and confirm it won't compile.\n");
    return 0;
}
