
#include "framework.test.h"

#include <boost/core/demangle.hpp>
#include <boost/range/algorithm/equal.hpp>
#include <cstdint>
#include <type_traits>
#include "wire/traits.h"
#include "wire/json/base.h"
#include "wire/json/read.h"
#include "wire/json/write.h"
#include "wire/vector.h"

#include "wire/base.test.h"

namespace
{
  constexpr const char basic_string[] = u8"my_string_data";
  constexpr const char basic_json[] =
    u8"{\"utf8\":\"my_string_data\",\"vec\":[0,127],\"data\":\"00ff2211\",\"choice\":true}";

  template<typename T>
  struct basic_object
  {
    std::string utf8;
    std::vector<T> vec;
    lws_test::small_blob data;
    bool choice;
  };

  template<typename F, typename T>
  void basic_object_map(F& format, T& self)
  {
    wire::object(format, WIRE_FIELD(utf8), WIRE_FIELD(vec), WIRE_FIELD(data), WIRE_FIELD(choice));
  }

  template<typename T>
  void read_bytes(wire::json_reader& source, basic_object<T>& dest)
  { basic_object_map(source, dest); }

  template<typename T>
  void write_bytes(wire::json_writer& dest, const basic_object<T>& source)
  { basic_object_map(dest, source); }

  template<typename T>
  void test_basic_reading(lest::env& lest_env)
  {
    SETUP("Basic values with " + boost::core::demangle(typeid(T).name()) + " integers")
    {
      basic_object<T> result{};
      EXPECT(!wire::json::from_bytes(std::string{basic_json}, result));
      EXPECT(result.utf8 == basic_string);
      {
        const std::vector<T> expected{0, 127};
        EXPECT(result.vec == expected);
      }
      EXPECT(result.data == lws_test::blob_test1);
      EXPECT(result.choice);
    }
  }

  template<typename T>
  void test_basic_writing(lest::env& lest_env)
  {
    SETUP("Basic values with " + boost::core::demangle(typeid(T).name()) + " integers")
    {
      const basic_object<T> val{basic_string, std::vector<T>{0, 127}, lws_test::blob_test1, true};
      epee::byte_slice result{};
      EXPECT(!wire::json::to_bytes(result, val));
      EXPECT(boost::range::equal(result, std::string{basic_json}));
    }
  }
}

LWS_CASE("wire::json_reader")
{
  using i64_limit = std::numeric_limits<std::int64_t>;
  static constexpr const char negative_number[] = "-1";

  test_basic_reading<std::int16_t>(lest_env);
  test_basic_reading<std::int32_t>(lest_env);
  test_basic_reading<std::int64_t>(lest_env);
  test_basic_reading<std::intmax_t>(lest_env);
  test_basic_reading<std::uint16_t>(lest_env);
  test_basic_reading<std::uint32_t>(lest_env);
  test_basic_reading<std::uint64_t>(lest_env);
  test_basic_reading<std::uintmax_t>(lest_env);

  static_assert(0 < i64_limit::max(), "expected 0 < int64_t::max");
  static_assert(
    i64_limit::max() <= std::numeric_limits<std::uintmax_t>::max(),
    "expected int64_t::max <= uintmax_t::max"
  );
  std::uint64_t one = 0;
  std::int64_t two = 0;
  std::string big_number = std::to_string(std::uintmax_t(i64_limit::max()) + 1);
  EXPECT(wire::json::from_bytes(negative_number, one) == wire::error::schema::larger_integer);
  EXPECT(wire::json::from_bytes(std::move(big_number), two) == wire::error::schema::smaller_integer);
}

LWS_CASE("wire::json_writer")
{
  test_basic_writing<std::int16_t>(lest_env);
  test_basic_writing<std::int32_t>(lest_env);
  test_basic_writing<std::int64_t>(lest_env);
  test_basic_writing<std::intmax_t>(lest_env);
  test_basic_writing<std::uint16_t>(lest_env);
  test_basic_writing<std::uint32_t>(lest_env);
  test_basic_writing<std::uint64_t>(lest_env);
  test_basic_writing<std::uintmax_t>(lest_env);
}
