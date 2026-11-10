#include <catch2/catch_test_macros.hpp>
#include <state/config.hpp>

using namespace state;

TEST_CASE("Video packet size rules", "[packet_size]") {
  const std::vector<wolf::config::VideoPacketSizeRule> rules = {
      {.network = "172.26.0.0/16", .max_packet_size = 1376},
      {.network = "192.168.31.0/24", .max_packet_size = 1392},
      {.network = "172.26.31.0/24", .max_packet_size = 1200},
  };

  SECTION("clients are matched by network") {
    REQUIRE(match_video_packet_size_rule(rules, "172.26.0.5") == std::optional<int>(1376));
    REQUIRE(match_video_packet_size_rule(rules, "172.27.0.5") == std::optional<int>(1376));
    REQUIRE(match_video_packet_size_rule(rules, "192.168.31.100") == std::optional<int>(1392));
  }

  SECTION("the most specific network wins") {
    REQUIRE(match_video_packet_size_rule(rules, "172.26.31.9") == std::optional<int>(1200));
  }

  SECTION("clients without a matching rule are left alone") {
    REQUIRE_FALSE(match_video_packet_size_rule(rules, "10.0.0.5").has_value());
    REQUIRE_FALSE(match_video_packet_size_rule(rules, "192.168.32.5").has_value());
  }

  SECTION("garbage in, nothing out") {
    REQUIRE_FALSE(match_video_packet_size_rule(rules, "не адрес").has_value());
    REQUIRE_FALSE(match_video_packet_size_rule(rules, "").has_value());

    const std::vector<wolf::config::VideoPacketSizeRule> broken = {{.network = "не сеть", .max_packet_size = 1}};
    REQUIRE_FALSE(match_video_packet_size_rule(broken, "172.26.0.5").has_value());
  }

  SECTION("IPv6 clients are matched too") {
    const std::vector<wolf::config::VideoPacketSizeRule> v6_rules = {{.network = "fd00::/8", .max_packet_size = 1200}};
    REQUIRE(match_video_packet_size_rule(v6_rules, "fd11::5") == std::optional<int>(1200));
    REQUIRE_FALSE(match_video_packet_size_rule(v6_rules, "fe80::1").has_value());
  }
}
