#include <catch2/catch_test_macros.hpp>
#include <string>
#include <thread>
#include "host/platform.hpp"
#include "host/reaper_types.hpp"

TEST_CASE("setLowPriorityWorker names the calling thread", "[platform]")
{
    std::string seen;
    std::thread t([&] { ni::host::platform::setLowPriorityWorker("NovaInsert corr"); seen = ni::host::platform::currentThreadName(); });
    t.join();
    REQUIRE(seen == "NovaInsert corr");          // 15 bytes: fits every OS
}

TEST_CASE("setLowPriorityWorker truncates long names", "[platform]")   // Review Focus 3
{
    std::string seen;
    std::thread t([&] { ni::host::platform::setLowPriorityWorker("NovaInsert correlator worker thread"); seen = ni::host::platform::currentThreadName(); });
    t.join();
    REQUIRE(seen.rfind("NovaInsert corr", 0) == 0);   // prefix survives; Linux keeps exactly 15 bytes, macOS/Windows more
    REQUIRE(!seen.empty());
}

TEST_CASE("GUID has REAPER's 16-byte layout on every OS", "[platform]")
{
    // REAPER (Win32 GUID / SWELL) hands out 16-byte GUIDs; a wider Data1 (unsigned long on LP64) over-reads in keyOf().
    REQUIRE(sizeof(GUID) == 16);
    REQUIRE(sizeof(GUID{}.Data1) == 4);
}
