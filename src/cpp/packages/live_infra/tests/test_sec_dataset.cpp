#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "market_data/sec_dataset.h"
#include "market_data/sec_store.h"

using MarketData::FormSet;
using MarketData::SecDataset;
using MarketData::SecFiling;
using MarketData::SecStore;
namespace Lat = MarketData::SecLatency;

namespace {

SecFiling make(const std::string& form, long long filed_ms) {
    SecFiling f;
    f.cik = "0000000001";
    f.accn = form + "-" + std::to_string(filed_ms);
    f.form = form;
    f.filed_ts_ms = filed_ms;
    f.observable_ts_ms = filed_ms + Lat::pad_for_form(form);
    return f;
}

constexpr long long kDay = Lat::kDayMs;
const FormSet kDilutive = {"S-1", "S-3", "424B5"};

}  // namespace

TEST_CASE("SEC latency pads match the python PIT profile", "[sec_dataset]") {
    REQUIRE(Lat::pad_for_form("S-3") == Lat::kRegistrationMs);
    REQUIRE(Lat::pad_for_form("S-1/A") == Lat::kRegistrationMs);
    REQUIRE(Lat::pad_for_form("424B5") == Lat::kRegistrationMs);
    REQUIRE(Lat::pad_for_form("F-3") == Lat::kRegistrationMs);
    REQUIRE(Lat::pad_for_form("8-K") == Lat::kMaterialEvents8kMs);
    REQUIRE(Lat::pad_for_form("4") == Lat::kInsiderForm4Ms);
    REQUIRE(Lat::pad_for_form("4/A") == Lat::kInsiderForm4Ms);
    REQUIRE(Lat::pad_for_form("144") == Lat::kProposedSales144Ms);
    REQUIRE(Lat::pad_for_form("SC 13D") == Lat::kBeneficial13dMs);
    REQUIRE(Lat::pad_for_form("SCHEDULE 13G") == Lat::kBeneficial13gMs);

    SECTION("an unclassified form is never more visible than a known one") {
        const long long unknown = Lat::pad_for_form("WHAT-IS-THIS");
        REQUIRE(unknown >= Lat::kRegistrationMs);
        REQUIRE(unknown >= Lat::kInsiderForm4Ms);
        REQUIRE(unknown >= Lat::kBeneficial13dMs);
        REQUIRE(Lat::pad_for_form("") >= Lat::kRegistrationMs);
    }
}

// The reason this module exists. A strategy must not be able to read a filing
// before it was actionable, and that has to be a property of the accessor
// rather than of whoever calls it -- the research path's worst lookahead came
// from a Python file deciding what was knowable and C++ trusting the answer.
TEST_CASE("a filing is invisible until its observable timestamp", "[sec_dataset]") {
    SecDataset ds("0000000001");
    const long long filed = 1'700'000'000'000LL;
    const auto f = make("S-3", filed);
    ds.add(f);

    REQUIRE(ds.size() == 1);
    REQUIRE(f.observable_ts_ms == filed + Lat::kRegistrationMs);

    SECTION("invisible at filing time, and one ms before observable") {
        REQUIRE(ds.visible_filings(filed).empty());
        REQUIRE(ds.visible_filings(f.observable_ts_ms - 1).empty());
        REQUIRE_FALSE(ds.last_filed_ms(kDilutive, f.observable_ts_ms - 1));
        REQUIRE(ds.count_in_window(kDilutive, f.observable_ts_ms - 1, 90 * kDay) == 0);
    }

    SECTION("visible exactly at, and after, its observable timestamp") {
        REQUIRE(ds.visible_filings(f.observable_ts_ms).size() == 1);
        REQUIRE(ds.last_filed_ms(kDilutive, f.observable_ts_ms) == f.observable_ts_ms);
        REQUIRE(ds.count_in_window(kDilutive, f.observable_ts_ms, 90 * kDay) == 1);
    }

    SECTION("forms with longer pads stay hidden longer") {
        SecDataset d2("0000000001");
        d2.add(make("4", filed));        // 3-day pad
        d2.add(make("SC 13D", filed));   // 12-day pad
        REQUIRE(d2.visible_filings(filed + 2 * kDay).empty());
        REQUIRE(d2.visible_filings(filed + 3 * kDay).size() == 1);
        REQUIRE(d2.visible_filings(filed + 11 * kDay).size() == 1);
        REQUIRE(d2.visible_filings(filed + 12 * kDay).size() == 2);
    }
}

TEST_CASE("window and recency queries respect the form set", "[sec_dataset]") {
    SecDataset ds("0000000001");
    const long long t0 = 1'700'000'000'000LL;
    ds.add(make("S-3", t0));
    ds.add(make("S-1", t0 + 30 * kDay));
    ds.add(make("8-K", t0 + 40 * kDay));   // not in kDilutive
    ds.add(make("424B5", t0 + 200 * kDay));
    const long long now = t0 + 400 * kDay;

    // Observable times are filed + pad: S-3 at +1d, S-1 at +31d, 8-K at +41d,
    // 424B5 at +201d. A 365d window ending at +400d opens at +35d, so only the
    // 424B5 is inside it -- the first two are older than a year by then.
    REQUIRE(ds.count_in_window(kDilutive, now, 365 * kDay) == 1);
    REQUIRE(ds.count_in_window(kDilutive, now, 90 * kDay) == 0);
    REQUIRE(ds.count_in_window(kDilutive, t0 + 100 * kDay, 90 * kDay) == 1);
    REQUIRE(ds.count_in_window(kDilutive, t0 + 100 * kDay, 100 * kDay) == 2);
    REQUIRE(ds.count_in_window({}, now, 500 * kDay) == 4);  // empty set = any

    SECTION("days_since is nullopt when nothing matches, never a large number") {
        REQUIRE_FALSE(ds.days_since({"10-K"}, now));
        const auto since = ds.days_since(kDilutive, now);
        REQUIRE(since);
        REQUIRE(*since == 199);  // 424B5 at +200d, 1-day pad
    }

    SECTION("out-of-order inserts keep the observable ordering") {
        SecDataset d2("0000000001");
        d2.add(make("S-3", t0 + 100 * kDay));
        d2.add(make("S-3", t0));
        d2.add(make("S-3", t0 + 50 * kDay));
        const auto v = d2.visible_filings(t0 + 500 * kDay);
        REQUIRE(v.size() == 3);
        REQUIRE(v[0].observable_ts_ms <= v[1].observable_ts_ms);
        REQUIRE(v[1].observable_ts_ms <= v[2].observable_ts_ms);
    }
}

TEST_CASE("loads a real issuer from the filing store", "[sec_dataset][.disk]") {
    const char* root = std::getenv("DATA_DIR_ROOT");
    if (!root) return;
    const std::string filings = std::string(root) + "/alt/filings/sec";
    if (!std::filesystem::exists(filings + "/registrations/0000320193")) return;

    SecDataset ds("0000320193");  // Apple
    ds.load(filings, 0);
    REQUIRE(ds.loaded());
    REQUIRE(ds.size() > 0);

    const long long now = 1'780'000'000'000LL;
    const auto all = ds.visible_filings(now);
    REQUIRE(!all.empty());

    // String CONTENT, not just row counts. The first version of the loader
    // cast large_string columns to StringArray and produced garbage here while
    // every count and timestamp assertion still passed.
    for (const auto& f : all) {
        REQUIRE(f.cik == "0000320193");
        REQUIRE_FALSE(f.form.empty());
        REQUIRE(f.form.size() < 32);
        REQUIRE(f.accn.size() == 20);  // 0000000000-00-000000
    }
    // Every family must actually load; insider filings were being dropped
    // wholesale by a caught exception while the suite reported success.
    bool saw_insider = false, saw_registration = false;
    for (const auto& f : all) {
        if (f.form == "4" || f.form == "4/A") saw_insider = true;
        if (f.form.rfind("S-", 0) == 0 || f.form.rfind("424", 0) == 0)
            saw_registration = true;
    }
    REQUIRE(saw_insider);
    REQUIRE(saw_registration);

    // Ordering and the invariant must hold on real data, not just synthetic.
    for (std::size_t i = 1; i < all.size(); ++i)
        REQUIRE(all[i - 1].observable_ts_ms <= all[i].observable_ts_ms);
    for (const auto& f : all) {
        REQUIRE(f.observable_ts_ms > f.filed_ts_ms);
        REQUIRE(f.observable_ts_ms <= now);
    }
    // Every filing is hidden strictly before its own observable timestamp.
    const auto& probe = all[all.size() / 2];
    for (const auto& f : ds.visible_filings(probe.observable_ts_ms - 1))
        REQUIRE(f.observable_ts_ms < probe.observable_ts_ms);
}


TEST_CASE("symbol resolution refuses ambiguity", "[sec_dataset][.disk]") {
    const char* root = std::getenv("DATA_DIR_ROOT");
    if (!root) return;
    const std::string filings = std::string(root) + "/alt/filings/sec";
    if (!std::filesystem::exists(filings + "/symbol_cik_map/data.parquet")) return;

    auto& store = SecStore::instance();
    store.clear();
    store.load_symbol_map(filings);
    REQUIRE(store.symbol_map_loaded());
    REQUIRE(store.symbol_map_size() > 5000);

    const long long now = 1'780'000'000'000LL;
    REQUIRE(store.cik_for("AAPL", now) == "0000320193");
    REQUIRE_FALSE(store.cik_for("__NOT_A_TICKER__", now));

    SECTION("provisioning is explicit; an unprovisioned symbol is nullopt") {
        REQUIRE_FALSE(store.dataset_for("AAPL", now));  // not preloaded yet
        const std::size_t made =
            store.preload(filings, {"AAPL", "MSFT", "__NOT_A_TICKER__"}, now, 0);
        REQUIRE(made >= 1);
        REQUIRE(store.dataset_for("AAPL", now));
        // Unresolvable symbols are reported, not silently skipped: the unmapped
        // set is concentrated in delisted names, so its size measures bias.
        const auto missing = store.unresolved_symbols();
        REQUIRE(std::find(missing.begin(), missing.end(), "__NOT_A_TICKER__") !=
                missing.end());
    }

    SECTION("the PIT invariant survives the symbol layer") {
        store.preload(filings, {"AAPL"}, now, 0);
        auto ds = store.dataset_for("AAPL", now);
        REQUIRE(ds);
        const auto all = (*ds)->visible_filings(now);
        REQUIRE(!all.empty());
        const auto& probe = all[all.size() / 2];
        for (const auto& f : (*ds)->visible_filings(probe.observable_ts_ms - 1))
            REQUIRE(f.observable_ts_ms < probe.observable_ts_ms);
    }
    store.clear();
}
