#include "fluxnet/server.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string bench_path() {
    const char* env = std::getenv("FLUXNET_BENCH");
    if (env == nullptr || env[0] == '\0') {
        return "fluxnet_bench";
    }
    return env;
}

int run_bench(const std::string& args) {
    const std::string cmd = bench_path() + " " + args;
    return std::system(cmd.c_str());
}

} // namespace

TEST(LoadGen, RefusesRemoteWithoutFlag) {
    const int rc = run_bench("--host 8.8.8.8 --port 1 --duration 0.1 --warmup 0 --connections 1 "
                             "--out /tmp/fluxnet-remote.json");
    EXPECT_NE(rc, 0);
}

TEST(LoadGen, CsvChurnAndSlowRead) {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_threads = 1;
    config.io_backend = fluxnet::IoBackend::Epoll;
    config.idle_timeout = std::chrono::milliseconds(0);
    config.slow_consumer_timeout = std::chrono::milliseconds(0);
    fluxnet::Server server(config);
    server.set_on_message([](fluxnet::Connection& conn, fluxnet::MessageView msg) {
        conn.send(msg.type(), msg.payload(), msg.flags());
    });
    std::thread thread([&] { server.run(); });
    ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));
    const std::string json = "/tmp/fluxnet-loadgen.json";
    const std::string args = "--host 127.0.0.1 --port " + std::to_string(server.bound_port()) +
                             " --connections 4 --payload 64 --threads 1 --warmup 0.1 "
                             "--duration 0.6 --churn-seconds 0.2 --slow-read-bytes 16 --out " +
                             json;
    const int rc = run_bench(args);
    server.stop();
    thread.join();
    EXPECT_EQ(rc, 0) << args;

    std::ifstream csv("/tmp/fluxnet-loadgen.csv");
    ASSERT_TRUE(csv.good());
    std::string header;
    std::getline(csv, header);
    EXPECT_EQ(header, "offered,completed,failures,timed_out,connect_failures,rejected_sends,"
                      "msgs_per_s,bytes_per_s,p50_ns,p95_ns,p99_ns,p999_ns,max_ns");
    std::ifstream in(json);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(body.find("\"connect_failures\""), std::string::npos);
    EXPECT_NE(body.find("\"generator_rss_kb\""), std::string::npos);
    EXPECT_NE(body.find("\"timed_out\""), std::string::npos);
    EXPECT_NE(body.find("\"behind_schedule\""), std::string::npos);
    std::string csv_row;
    std::getline(csv, csv_row);
    EXPECT_FALSE(csv_row.empty());
}

TEST(LoadGen, OpenLoopRecordsBehindSchedule) {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_threads = 1;
    config.idle_timeout = std::chrono::milliseconds(0);
    config.slow_consumer_timeout = std::chrono::milliseconds(0);
    fluxnet::Server server(config);
    server.set_on_message([](fluxnet::Connection& conn, fluxnet::MessageView msg) {
        conn.send(msg.type(), msg.payload(), msg.flags());
    });
    std::thread thread([&] { server.run(); });
    ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));
    const std::string json = "/tmp/fluxnet-openloop.json";
    const std::string args = "--host 127.0.0.1 --port " + std::to_string(server.bound_port()) +
                             " --connections 1 --payload 64 --threads 1 --warmup 0 "
                             "--duration 0.4 --outstanding 1 --rate 1000000 --out " +
                             json;
    const int rc = run_bench(args);
    server.stop();
    thread.join();
    EXPECT_EQ(rc, 0) << args;
    std::ifstream in(json);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(body.find("\"behind_schedule\": "), std::string::npos);
    EXPECT_EQ(body.find("\"behind_schedule\": 0"), std::string::npos) << body;
    EXPECT_NE(body.find("\"failures\": 0"), std::string::npos);
}

TEST(LoadGen, BadPayloadIsAFailure) {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_threads = 1;
    config.idle_timeout = std::chrono::milliseconds(0);
    config.slow_consumer_timeout = std::chrono::milliseconds(0);
    fluxnet::Server server(config);
    server.set_on_message([](fluxnet::Connection& conn, fluxnet::MessageView msg) {
        std::vector<std::byte> flipped(msg.payload().begin(), msg.payload().end());
        if (!flipped.empty()) {
            flipped[0] = std::byte{0xff};
        }
        conn.send(msg.type(), flipped, msg.flags());
    });
    std::thread thread([&] { server.run(); });
    ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));
    const int rc = run_bench("--host 127.0.0.1 --port " + std::to_string(server.bound_port()) +
                             " --connections 1 --payload 16 --threads 1 --warmup 0 --duration 0.3 "
                             "--out /tmp/fluxnet-badpayload.json");
    server.stop();
    thread.join();
    EXPECT_NE(rc, 0);
}

TEST(LoadGen, UnansweredSendCountsTimedOut) {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_threads = 1;
    config.idle_timeout = std::chrono::milliseconds(0);
    config.slow_consumer_timeout = std::chrono::milliseconds(0);
    fluxnet::Server server(config);
    server.set_on_message([](fluxnet::Connection&, fluxnet::MessageView) {});
    std::thread thread([&] { server.run(); });
    ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));
    const std::string json = "/tmp/fluxnet-timeout.json";
    const int rc = run_bench("--host 127.0.0.1 --port " + std::to_string(server.bound_port()) +
                             " --connections 1 --payload 8 --threads 1 --warmup 0 --duration 0.2 "
                             "--outstanding 1 --out " +
                             json);
    server.stop();
    thread.join();
    EXPECT_NE(rc, 0);
    std::ifstream in(json);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(body.find("\"timed_out\": 0"), std::string::npos) << body;
}
