#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include "host/diagnostics.hpp"

using namespace ni::host;

TEST_CASE("settings: maxConcurrentProbes defaults to 0 (unlimited) and is read when present")
{
    const auto p = std::filesystem::temp_directory_path() / "novainsert-settings-test.json";
    { std::ofstream o(p); o << "{\"diagnostics\": true, \"diagnosticsDir\": \"/tmp/x\"}"; }
    auto s = DiagLog::probeSettings(p.string());
    REQUIRE(s.diagnostics); REQUIRE(s.dir == "/tmp/x"); REQUIRE(s.maxConcurrentProbes == 0);
    { std::ofstream o(p); o << "{\"diagnostics\": true,\n \"maxConcurrentProbes\": 4\n}"; }
    s = DiagLog::probeSettings(p.string());
    REQUIRE(s.maxConcurrentProbes == 4);
}

TEST_CASE("diagnostics: a text row reaches the CSV immediately, and is dropped when disabled")
{
    const auto dir = std::filesystem::temp_directory_path() / "novainsert-diag-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto& d = DiagLog::get();
    d.text("BEFORE", 1, "x=%d", 1);   // disabled: no file
    REQUIRE(std::filesystem::is_empty(dir));
    d.enable(dir.string());
    d.text("STATE", 3, "state=%d|reason=%d", 5, 7);
    std::string csv;
    for (const auto& e : std::filesystem::directory_iterator(dir))
    {
        std::ifstream in(e.path()); std::string line;
        while (std::getline(in, line)) csv += line + "\n";
    }
    d.disable();
    REQUIRE(csv.find("t,kind,inst,v0,v1,v2,v3\n") == 0);
    REQUIRE(csv.find(",STATE,3,state=5|reason=7\n") != std::string::npos);
    REQUIRE(csv.find("BEFORE") == std::string::npos);
}
