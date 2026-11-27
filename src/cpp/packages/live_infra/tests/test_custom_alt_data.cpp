#include <catch2/catch_all.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "market_data/alt_data_manager.h"
#include "market_data/alt_data_registry.h"
#include "market_data/custom_series_module.h"

namespace fs = std::filesystem;

namespace {

struct ScopedTempDir {
    fs::path path;
    explicit ScopedTempDir(const std::string& tag) {
        const auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("keronis_" + tag + "_" + std::to_string(ns));
        fs::create_directories(path);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Tier 2 example: typed out-of-tree AltDataModule registered via KERONIS_REGISTER_ALT_DATA.
struct SyntheticEvent {
    long long available_ts_ms = 0;
    std::string label;
};

class TestCustomEventsModule final
    : public MarketData::FileBackedAltDataModule<std::vector<SyntheticEvent>> {
   public:
    MarketData::AltDataSource source() const override {
        return MarketData::AltDataSource::Custom;
    }

    void set_root(std::string root) { root_ = std::move(root); }

    std::optional<std::string> latest_label(const std::string& key,
                                            long long as_of_ms) const {
        const auto entry = snapshot(key);
        if (entry == nullptr) return std::nullopt;
        std::optional<std::string> best;
        for (const auto& ev : entry->payload) {
            if (ev.available_ts_ms <= as_of_ms) best = ev.label;
        }
        return best;
    }

   protected:
    std::string path_for(const std::string& key) const override {
        return root_ + "/" + key + ".txt";
    }

    std::vector<SyntheticEvent> parse(const std::string& path) const override {
        std::ifstream in(path);
        std::vector<SyntheticEvent> out;
        long long ts = 0;
        std::string label;
        while (in >> ts >> label) {
            out.push_back({ts, label});
        }
        return out;
    }

   private:
    std::string root_;
};

KERONIS_REGISTER_ALT_DATA("test_custom_events", TestCustomEventsModule);

}  // namespace

TEST_CASE("CustomSeriesModule enforces point-in-time visibility, max_age, and live reload",
          "[custom_alt_data]") {
    ScopedTempDir tmp("custom_series");
    const fs::path ds_dir = tmp.path / "onchain_flows";
    fs::create_directories(ds_dir);

    // Intentionally unsorted rows to verify sort-on-parse.
    {
        std::ofstream f(ds_dir / "BTCUSDT.jsonl");
        f << R"({"available_ts_ms": 2000, "value": 0.85, "confidence": 0.9})" << "\n";
        f << R"({"available_ts_ms": 1000, "value": 0.40, "confidence": 0.7})" << "\n";
    }

    auto& mgr = MarketData::AltDataManager::instance();
    mgr.clear();

    auto& ds = mgr.custom_series("onchain_flows", tmp.path.string());
    REQUIRE(ds.preload({"BTCUSDT", "MISSING"}) == 1);
    REQUIRE(ds.keys_without_data() == std::vector<std::string>{"MISSING"});

    SECTION("fail closed on unprovisioned keys and pre-history timestamps") {
        REQUIRE_FALSE(ds.latest("MISSING", 5000).has_value());
        REQUIRE_FALSE(ds.latest("NEVER", 5000).has_value());
        REQUIRE_FALSE(ds.latest("BTCUSDT", 999).has_value());
        REQUIRE_FALSE(ds.window("MISSING", 0, 5000).has_value());
    }

    SECTION("as_of_ms binary search prevents lookahead") {
        const auto at_1000 = ds.latest_value("BTCUSDT", 1000);
        REQUIRE(at_1000.has_value());
        REQUIRE(*at_1000 == Catch::Approx(0.40));

        const auto at_1999 = ds.latest_value("BTCUSDT", 1999);
        REQUIRE(at_1999.has_value());
        REQUIRE(*at_1999 == Catch::Approx(0.40));

        const auto at_2000 = ds.latest_value("BTCUSDT", 2000);
        REQUIRE(at_2000.has_value());
        REQUIRE(*at_2000 == Catch::Approx(0.85));

        const auto conf = ds.latest_value("BTCUSDT", 2000, "confidence");
        REQUIRE(conf.has_value());
        REQUIRE(*conf == Catch::Approx(0.9));
    }

    SECTION("max_age_ms rejects stale observations") {
        REQUIRE(ds.latest("BTCUSDT", 2500, 500).has_value());
        REQUIRE_FALSE(ds.latest("BTCUSDT", 2501, 500).has_value());
    }

    SECTION("window uses half-open [from, to) semantics") {
        const auto w = ds.window("BTCUSDT", 1000, 2000);
        REQUIRE(w.has_value());
        REQUIRE(w->size() == 1);
        REQUIRE((*w)[0].available_ts_ms == 1000);
    }

    SECTION("refresh_all_changed picks up appended rows on disk") {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        {
            std::ofstream f(ds_dir / "BTCUSDT.jsonl", std::ios::app);
            f << R"({"available_ts_ms": 3000, "value": 0.99})" << "\n";
        }
        REQUIRE(mgr.refresh_all_changed() >= 1);
        const auto at_3000 = ds.latest_value("BTCUSDT", 3000);
        REQUIRE(at_3000.has_value());
        REQUIRE(*at_3000 == Catch::Approx(0.99));
    }

    mgr.clear();
}

TEST_CASE("CustomSeriesModule rejects rows missing available_ts_ms",
          "[custom_alt_data]") {
    ScopedTempDir tmp("custom_bad");
    const fs::path ds_dir = tmp.path / "bad_ds";
    fs::create_directories(ds_dir);
    {
        std::ofstream f(ds_dir / "ETHUSDT.jsonl");
        f << R"({"value": 1.23})" << "\n";
    }

    MarketData::CustomSeriesModule ds("bad_ds", tmp.path.string());
    REQUIRE_THROWS_AS(ds.preload({"ETHUSDT"}), std::runtime_error);
}

TEST_CASE("KERONIS_REGISTER_ALT_DATA registers typed out-of-tree modules on AltDataManager",
          "[custom_alt_data]") {
    REQUIRE(MarketData::AltDataRegistry::contains("test_custom_events"));

    ScopedTempDir tmp("custom_typed");
    {
        std::ofstream f(tmp.path / "AAPL.txt");
        f << "1000 filing_a\n2000 filing_b\n";
    }

    auto& mgr = MarketData::AltDataManager::instance();
    mgr.clear();

    auto& mod = mgr.custom<TestCustomEventsModule>("test_custom_events");
    mod.set_root(tmp.path.string());
    REQUIRE(mod.preload({"AAPL"}) == 1);

    REQUIRE_FALSE(mod.latest_label("AAPL", 999).has_value());
    REQUIRE(mod.latest_label("AAPL", 1500) == "filing_a");
    REQUIRE(mod.latest_label("AAPL", 2000) == "filing_b");

    mgr.clear();
}
