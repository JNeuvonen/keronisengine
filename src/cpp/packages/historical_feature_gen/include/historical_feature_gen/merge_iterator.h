#pragma once

#include "historical_feature_gen/historical_feature_gen.h"
#include "market_data/market_event.h"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>

#include <filesystem>
#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace HistoricalFeatureGen {

// Convert an Arrow table to MarketEvents based on dataset type and schema.
std::vector<MarketData::MarketEvent> convert_table_to_events(
    const std::shared_ptr<arrow::Table>& table,
    const DatasetConfig& config,
    long long start_ts_ms, long long end_ts_ms);

// Find parquet files in a directory, sorted by partition path.
std::vector<fs::path> find_parquet_files_sorted(
    const std::string& dir, long long start_ts_ms, long long end_ts_ms);

// Reads one parquet file, yields MarketEvents one row group at a time.
class ParquetEventStream {
   public:
    ParquetEventStream(const fs::path& path, const DatasetConfig& config,
                       long long start_ts_ms, long long end_ts_ms);

    bool has_next() const;
    const MarketData::MarketEvent& peek() const;
    MarketData::MarketEvent pop();

   private:
    void load_next_row_group();
    void advance_to_valid();

    DatasetConfig config_;
    std::unique_ptr<parquet::arrow::FileReader> reader_;
    std::shared_ptr<arrow::Schema> schema_;
    std::vector<int> col_indices_;
    int current_row_group_ = 0;
    int total_row_groups_ = 0;
    // A row group is converted a slice at a time: MarketEvents are large, and
    // a replay holds one buffer per dataset.
    static constexpr int64_t kSliceRows = 8192;
    std::shared_ptr<arrow::Table> table_;
    int64_t table_offset_ = 0;
    std::vector<MarketData::MarketEvent> buffer_;
    size_t cursor_ = 0;
    long long start_ts_ms_;
    long long end_ts_ms_;
};

// Wraps an ordered sequence of ParquetEventStreams for one dataset.
class DatasetStreamSequence {
   public:
    DatasetStreamSequence(const DatasetConfig& config, int dataset_index,
                          long long start_ts_ms, long long end_ts_ms);

    bool has_next() const;
    const MarketData::MarketEvent& peek() const;
    MarketData::MarketEvent pop();
    int dataset_index() const { return dataset_index_; }

   private:
    void open_next_file();

    DatasetConfig config_;
    std::vector<fs::path> files_;
    size_t current_file_index_ = 0;
    std::unique_ptr<ParquetEventStream> stream_;
    int dataset_index_;
    long long start_ts_ms_;
    long long end_ts_ms_;
};

// K-way merge iterator over multiple dataset streams.
class MergeIterator {
   public:
    MergeIterator(const std::vector<DatasetConfig>& datasets,
                  long long start_ts_ms, long long end_ts_ms);

    bool has_next() const;
    MarketData::MarketEvent next();
    size_t total_events_consumed() const { return total_consumed_; }

   private:
    struct HeapEntry {
        DatasetStreamSequence* stream;
        long long timestamp;
        int dataset_index;

        bool operator>(const HeapEntry& other) const {
            if (timestamp != other.timestamp) return timestamp > other.timestamp;
            return dataset_index > other.dataset_index;
        }
    };

    std::vector<std::unique_ptr<DatasetStreamSequence>> streams_;
    std::priority_queue<HeapEntry, std::vector<HeapEntry>,
                        std::greater<HeapEntry>> heap_;
    size_t total_consumed_ = 0;
};

}  // namespace HistoricalFeatureGen
