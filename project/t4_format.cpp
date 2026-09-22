// Tier 4 — Compile-time format-string parsing.
// Compile: clang++ -std=c++20 -O2 -Wall -Wextra -pthread t4_format.cpp -o t4 && ./t4
//
// Goal: LOG<"x={} y={}">(a, b) where the format string is parsed & VALIDATED at
// compile time. Wrong placeholder/arg count => COMPILE ERROR, not a runtime bug.
// No format-string parsing happens in the hot path.
//
// L6 tools used: consteval (must-run-at-compile-time), a C++20 string-literal NTTP
// (carry the literal into the type system), and if constexpr (per-type dispatch).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <charconv>
#include <string_view>
#include <type_traits>

// ---- A structural type usable as a non-type template parameter (C++20). ----
// Wraps a string literal so it can be passed as LOG<"...">.
template <std::size_t N>
struct FixedString
{
    char data[N]{};
    consteval FixedString(const char (&s)[N])
    {
        for (std::size_t i = 0; i < N; ++i)
            data[i] = s[i];
    }
    constexpr std::string_view view() const { return {data, N - 1}; } // drop '\0'

    // ---------- TASK 1: count "{}" placeholders at COMPILE TIME ----------
    // Make this consteval. Walk data[] and count occurrences of the two-char
    // sequence "{}" . Return the count.
    // (Keep it simple: only "{}" is a placeholder; you don't need to handle
    //  escapes like "{{" for this tier.)
    consteval std::size_t placeholders() const
    {
        std::size_t count = 0;
        // TODO: scan the view() for "{}" and count them
        for (std::size_t i = 0; i < N - 1; i++)
        {
            if (data[i] == '{' && data[i + 1] == '}')
            {
                ++count;
            }
        }
        return count;
    }
};

// ---- Convert one argument into buf at p, return new position. ----
// Integers via std::to_chars (no alloc, no locale). Strings via memcpy.
template <class T>
static char *emit_arg(char *p, char *end, const T &arg)
{
    // ---------- TASK 2: dispatch on the argument type with if constexpr ----------
    // - if T is an integral type: use std::to_chars(p, end, arg); advance p to
    //   result.ptr and return it.
    // - else (treat as a C-string / string_view-ish): copy its bytes into p and
    //   advance. For simplicity assume const char* here (use std::strlen).
    // Hint:
    //   if constexpr (std::is_integral_v<T>) { ... std::to_chars ... }
    //   else { const char* s = arg; std::size_t n = std::strlen(s); memcpy(...); }
    // TODO
    if constexpr (std::is_integral_v<T>)
    { // this was introduced in cpp17 right? Still a little confused as to the specifics of it
        auto result = std::to_chars(p, end, arg);
        return result.ptr;
    }
    else
    {
        const char *s = arg;
        std::size_t n = std::strlen(s);
        std::memcpy(p, s, n);
        return p + n;
    }
}

// Base case: no args left — copy the remaining literal.
static char *format_impl(char *p, char *end, std::string_view fmt)
{
    std::size_t pos = fmt.find("{}");
    // no more placeholders: copy the rest verbatim
    std::size_t n = (pos == std::string_view::npos) ? fmt.size() : pos;
    std::memcpy(p, fmt.data(), n);
    return p + n;
}

// Recursive case: copy literal up to the next "{}", emit one arg, recurse.
template <class T, class... Rest>
static char *format_impl(char *p, char *end, std::string_view fmt, const T &arg, const Rest &...rest)
{
    std::size_t pos = fmt.find("{}");
    // copy literal chunk before the placeholder
    std::memcpy(p, fmt.data(), pos);
    p += pos;
    // emit the argument in place of "{}"
    p = emit_arg(p, end, arg);
    // recurse on the remainder after the placeholder
    return format_impl(p, end, fmt.substr(pos + 2), rest...);
}

// ---- The public LOG entry point. Fmt is a compile-time template parameter. ----
template <FixedString Fmt, class... Args>
std::size_t LOG(char *buf, std::size_t cap, const Args &...args)
{
    // ---------- TASK 3: the compile-time check ----------
    // static_assert that the number of "{}" in Fmt equals the number of arguments.
    // This is what makes a wrong-arity LOG a COMPILE error.
    // Hint: Fmt.placeholders() and sizeof...(Args)
    // TODO
    static_assert(sizeof...(args) == Fmt.placeholders(), "LOG: placeholder/argument count mismatch");

    char *p = buf;
    char *end = buf + cap;
    p = format_impl(p, end, Fmt.view(), args...);
    return static_cast<std::size_t>(p - buf); // bytes written
}

int main()
{
    char buf[256];

    std::size_t n1 = LOG<"x={} y={}">(buf, sizeof buf, 42, 7);
    std::printf("[%.*s]  (expect [x=42 y=7])\n", (int)n1, buf);

    std::size_t n2 = LOG<"user {} logged in from {}">(buf, sizeof buf, "alice", "10.0.0.1");
    std::printf("[%.*s]  (expect [user alice logged in from 10.0.0.1])\n", (int)n2, buf);

    std::size_t n3 = LOG<"no placeholders here">(buf, sizeof buf);
    std::printf("[%.*s]  (expect [no placeholders here])\n", (int)n3, buf);

    std::size_t n4 = LOG<"mixed {} and {} and {}">(buf, sizeof buf, 1, "two", 3);
    std::printf("[%.*s]  (expect [mixed 1 and two and 3])\n", (int)n4, buf);

    // ---- Uncomment to PROVE compile-time validation (should NOT compile): ----
    LOG<"x={} y={}">(buf, sizeof buf, 42); // 2 placeholders, 1 arg
    // LOG<"only one {}">(buf, sizeof buf, 1, 2, 3);   // 1 placeholder, 3 args

    std::printf("\nAll ran. Now uncomment the two bad LOG lines and confirm they FAIL to compile.\n");
    return 0;
}
