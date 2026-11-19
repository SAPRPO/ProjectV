#pragma once

// Минимальный тестовый фреймворк без внешних зависимостей:
//   TCP_TEST_CASE(имя) { ... }
//   TCP_CHECK(условие);
//   TCP_CHECK_EQ(a, b);
//   TCP_REQUIRE(условие);   // при провале прекращает тест

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testfw {

struct AssertionFailure : std::exception {
    std::string message;
    explicit AssertionFailure(std::string text) : message(std::move(text)) {}
    const char* what() const noexcept override { return message.c_str(); }
};

struct TestCase {
    std::string name;
    void (*function)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& current_failures() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, void (*function)()) {
        registry().push_back(TestCase{name, function});
    }
};

template <typename T>
std::string to_text(const T& value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

inline std::string to_text(bool value) {
    return value ? "true" : "false";
}

inline void report_failure(const char* file, int line, const std::string& what) {
    ++current_failures();
    std::cerr << "    " << file << ":" << line << ": ПРОВАЛ: " << what << "\n";
}

inline void report_failure(const char* what) {
    ++current_failures();
    std::cerr << "    ПРОВАЛ: " << what << "\n";
}

inline int run_all() {
    int failed_tests = 0;

    for (const auto& test : registry()) {
        current_failures() = 0;
        std::cout << "[ RUN  ] " << test.name << std::endl;
        try {
            test.function();
        } catch (const AssertionFailure& failure) {
            report_failure(failure.what());
        } catch (const std::exception& ex) {
            report_failure((std::string("исключение: ") + ex.what()).c_str());
        } catch (...) {
            report_failure("неизвестное исключение");
        }

        if (current_failures() == 0) {
            std::cout << "[  OK  ] " << test.name << std::endl;
        } else {
            ++failed_tests;
            std::cout << "[ FAIL ] " << test.name << " (провалено проверок: "
                      << current_failures() << ")" << std::endl;
        }
    }

    std::cout << "\nТестов: " << registry().size() << ", провалено: " << failed_tests << std::endl;
    return failed_tests == 0 ? 0 : 1;
}

}  // namespace testfw

#define TCP_TEST_CASE(name)                                              \
    static void name();                                                  \
    static ::testfw::Registrar tcp_test_registrar_##name(#name, &name);  \
    static void name()

#define TCP_CHECK(condition)                                          \
    do {                                                              \
        if (!(condition)) {                                           \
            ::testfw::report_failure(__FILE__, __LINE__, #condition); \
        }                                                             \
    } while (false)

#define TCP_CHECK_EQ(lhs, rhs)                                                        \
    do {                                                                              \
        const auto& tcp_lhs = (lhs);                                                  \
        const auto& tcp_rhs = (rhs);                                                  \
        if (!(tcp_lhs == tcp_rhs)) {                                                  \
            ::testfw::report_failure(                                                 \
                __FILE__, __LINE__,                                                   \
                std::string("CHECK_EQ(" #lhs ", " #rhs ") -> ") +                     \
                    ::testfw::to_text(tcp_lhs) + " != " + ::testfw::to_text(tcp_rhs)); \
        }                                                                             \
    } while (false)

#define TCP_REQUIRE(condition)                                                     \
    do {                                                                           \
        if (!(condition)) {                                                        \
            throw ::testfw::AssertionFailure(std::string("REQUIRE(" #condition ")")); \
        }                                                                          \
    } while (false)
