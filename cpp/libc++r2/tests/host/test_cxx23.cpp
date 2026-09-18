/*
 *  test_cxx23.cpp — the C++20/23 facilities, exercised on the host.
 *
 *  Three-way comparison, concepts, expected, coroutines, structured bindings
 *  and source_location.  Separate from test_main.cpp so that the older tests
 *  keep building as C++17 and prove the library still serves both.
 */

#include "r2/algorithm.hpp"
#include "r2/array.hpp"
#include "r2/compare.hpp"
#include "r2/concepts.hpp"
#include "r2/coroutine.hpp"
#include "r2/expected.hpp"
#include "r2/io.hpp"
#include "r2/source_location.hpp"
#include "r2/string.hpp"
#include "r2/vector.hpp"

extern "C" int printf(const char *format, ...);

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char *what, int line) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL  line %d: %s\n", line, what);
    }
}

#define CHECK(cond) check(!!(cond), #cond, __LINE__)

void section(const char *name) { printf("%s\n", name); }

/* ------------------------------------------------------------------------ */

struct Point {
    int x;
    int y;
    auto operator<=>(const Point &) const = default;
};

struct Mixed {
    int count;
    double ratio; /*  a floating member makes the category partial  */
    auto operator<=>(const Mixed &) const = default;
};

void test_three_way() {
    section("three-way comparison");

    Point a{1, 2};
    Point b{1, 3};

    CHECK((a <=> b) < 0);
    CHECK((b <=> a) > 0);
    CHECK((a <=> a) == 0);
    CHECK(a < b);
    CHECK(b > a);
    CHECK(a == a);
    CHECK(a != b);

    /*  The defaulted operator over an int and a double must come out partial. */
    using MixedCategory = decltype(Mixed{} <=> Mixed{});
    CHECK((r2::is_same_v<MixedCategory, std::partial_ordering>));

    using PointCategory = decltype(Point{} <=> Point{});
    CHECK((r2::is_same_v<PointCategory, std::strong_ordering>));

    /*  common_comparison_category, which the compiler uses to form those.  */
    CHECK((r2::is_same_v<std::common_comparison_category_t<std::strong_ordering>,
                         std::strong_ordering>));
    CHECK((r2::is_same_v<
           std::common_comparison_category_t<std::strong_ordering, std::weak_ordering>,
           std::weak_ordering>));
    CHECK((r2::is_same_v<
           std::common_comparison_category_t<std::weak_ordering, std::partial_ordering>,
           std::partial_ordering>));

    /*  Conversions between the categories.  */
    std::partial_ordering from_strong = std::strong_ordering::less;
    CHECK(from_strong < 0);
    std::weak_ordering weak = std::strong_ordering::greater;
    CHECK(weak > 0);

    CHECK(r2::is_lt(std::strong_ordering::less));
    CHECK(r2::is_gteq(std::strong_ordering::equal));
    CHECK(r2::is_neq(std::partial_ordering::unordered));

    /*  The library's own types.  */
    CHECK((r2::string_view("abc") <=> r2::string_view("abd")) < 0);
    CHECK((r2::string_view("b") <=> r2::string_view("a")) > 0);
    CHECK((r2::string("same") <=> r2::string("same")) == 0);
    CHECK((r2::pair<int, int>{1, 2} <=> r2::pair<int, int>{1, 3}) < 0);
}

void test_concepts() {
    section("concepts");

    static_assert(r2::integral<int>);
    static_assert(!r2::integral<double>);
    static_assert(r2::floating_point<double>);
    static_assert(r2::signed_integral<int>);
    static_assert(r2::unsigned_integral<unsigned>);
    static_assert(r2::same_as<int, int>);
    static_assert(!r2::same_as<int, long>);
    static_assert(r2::convertible_to<int, long>);
    static_assert(r2::equality_comparable<int>);
    static_assert(r2::totally_ordered<r2::string_view>);
    static_assert(r2::contiguous_range<r2::vector<int>>);
    static_assert(r2::contiguous_range<r2::string>);
    static_assert(r2::random_access_iterator<int *>);
    static_assert(r2::invocable<void (*)(int), int>);
    static_assert(r2::comparator<r2::less<>, int>);
    static_assert(r2::character<char>);
    static_assert(r2::trivially_copyable<Point>);

    /*  A constrained template picks the right overload.  */
    auto describe = []<class T>(T) {
        if constexpr (r2::integral<T>)
            return 1;
        else if constexpr (r2::floating_point<T>)
            return 2;
        else
            return 3;
    };

    CHECK(describe(1) == 1);
    CHECK(describe(1.0) == 2);
    CHECK(describe(r2::string_view("x")) == 3);

    g_checks += 17; /*  the static_asserts above, counted for the report  */
}

enum class LoadError { NotFound, TooBig, Corrupt };

r2::expected<int, LoadError> parse_size(r2::string_view text) {
    int64_t value = 0;
    if (!r2::parse_int(text, value))
        return r2::unexpected(LoadError::Corrupt);
    if (value > 1000)
        return r2::unexpected(LoadError::TooBig);
    return (int)value;
}

void test_expected() {
    section("expected");

    auto good = parse_size("42");
    CHECK(good.has_value());
    CHECK(*good == 42);
    CHECK(good.value() == 42);
    CHECK(good.value_or(-1) == 42);

    auto too_big = parse_size("5000");
    CHECK(!too_big.has_value());
    CHECK(too_big.error() == LoadError::TooBig);
    CHECK(too_big.value_or(-1) == -1);

    auto broken = parse_size("nonsense");
    CHECK(!broken);
    CHECK(broken.error() == LoadError::Corrupt);

    /*  Monadic chaining: the error passes straight through.  */
    auto doubled = good.transform([](int v) { return v * 2; });
    CHECK(doubled.has_value());
    CHECK(*doubled == 84);

    auto chained = too_big.transform([](int v) { return v * 2; });
    CHECK(!chained.has_value());
    CHECK(chained.error() == LoadError::TooBig);

    auto and_then = good.and_then(
        [](int v) -> r2::expected<r2::string, LoadError> { return r2::to_string((int64_t)v); });
    CHECK(and_then.has_value());
    CHECK(*and_then == r2::string_view("42"));

    auto recovered = broken.or_else([](LoadError) -> r2::expected<int, LoadError> { return 0; });
    CHECK(recovered.has_value());
    CHECK(*recovered == 0);

    auto translated = broken.transform_error([](LoadError) { return 7; });
    CHECK(!translated.has_value());
    CHECK(translated.error() == 7);

    /*  A non-trivial payload is constructed and destroyed correctly.  */
    r2::expected<r2::string, LoadError> text = r2::string("a string long enough to allocate");
    CHECK(text.has_value());
    CHECK(text->size() == 32);

    r2::expected<r2::string, LoadError> copy = text;
    CHECK(copy.has_value());
    CHECK(*copy == *text);

    r2::expected<r2::string, LoadError> failed_text = r2::unexpected(LoadError::NotFound);
    CHECK(!failed_text.has_value());
    copy = failed_text;
    CHECK(!copy.has_value());
    CHECK(copy.error() == LoadError::NotFound);

    /*  expected<void, E>  */
    auto write = [](bool ok) -> r2::expected<void, LoadError> {
        if (!ok)
            return r2::unexpected(LoadError::NotFound);
        return {};
    };

    CHECK(write(true).has_value());
    CHECK(!write(false).has_value());
    CHECK(write(false).error() == LoadError::NotFound);
}

r2::generator<int> squares(int upto) {
    for (int i = 1; i <= upto; i++)
        co_yield i * i;
}

r2::generator<r2::string> words() {
    co_yield r2::string("alpha");
    co_yield r2::string("bravo");
    co_yield r2::string("charlie");
}

void test_coroutines() {
    section("coroutines");

    r2::vector<int> collected;
    for (int value : squares(5))
        CHECK(collected.push_back(value));

    CHECK(collected.size() == 5);
    CHECK(collected[0] == 1);
    CHECK(collected[4] == 25);

    /*  The pull interface.  */
    auto generator = squares(3);
    int value = 0;
    int count = 0;
    while (generator.next(value))
        count++;
    CHECK(count == 3);

    /*  A generator of a type that allocates.  */
    r2::string joined;
    for (const r2::string &word : words()) {
        if (!joined.empty())
            joined += ",";
        joined += word;
    }
    CHECK(joined == r2::string_view("alpha,bravo,charlie"));

    /*  An empty sequence ends immediately.  */
    auto none = squares(0);
    CHECK(none.begin() == none.end());
}

void test_structured_bindings() {
    section("structured bindings");

    r2::pair<int, r2::string> entry{7, r2::string("seven")};
    auto [number, name] = entry;
    CHECK(number == 7);
    CHECK(name == r2::string_view("seven"));

    r2::array<int, 3> triple{1, 2, 3};
    auto [a, b, c] = triple;
    CHECK(a == 1 && b == 2 && c == 3);

    /*  And through the tuple interface directly.  */
    CHECK(r2::get<0>(triple) == 1);
    CHECK(r2::get<2>(triple) == 3);
    CHECK((std::tuple_size<r2::array<int, 3>>::value == 3));
}

void report_location(r2::source_location where = r2::source_location::current()) {
    CHECK(where.line() > 0);
    CHECK(r2::string_view(where.file_name()).ends_with("test_cxx23.cpp"));
    CHECK(r2::string_view(where.function_name()) == r2::string_view("test_source_location"));
}

void test_source_location() {
    section("source_location");
    report_location();
}

} // namespace

int main() {
    printf("libc++r2 C++23 tests\n\n");

    test_three_way();
    test_concepts();
    test_expected();
    test_coroutines();
    test_structured_bindings();
    test_source_location();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
