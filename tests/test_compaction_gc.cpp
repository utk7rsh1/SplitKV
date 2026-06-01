#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>
#include <filesystem>
#include "splitkv/db.h"
#include "splitkv/options.h"

int main() {
    std::cout << "Starting Compaction and GC integration tests...\n";

    // Clean up any old database directory first
    std::filesystem::remove_all("test_db_compaction");
    std::string db_path = "test_db_compaction";
    
    splitkv::Options options;
    options.memtable_size_limit = 2 * 1024;    // 2 KB (very small to trigger flushes easily)
    options.vlog_file_size_limit = 4 * 1024;  // 4 KB (very small to trigger rotations easily)
    options.vlog_gc_threshold = 0.3;          // 30% garbage triggers GC
    options.max_sstable_levels = 5;

    splitkv::DB* db = nullptr;
    splitkv::Status s = splitkv::DB::Open(options, db_path, &db);
    assert(s.ok() && "Failed to open database");
    std::unique_ptr<splitkv::DB> db_guard(db);

    std::cout << "Database opened successfully.\n";

    // Test 1: Write enough data to trigger memtable flushes to Level 0
    std::cout << "Test 1: Writing data to trigger flushes...\n";
    // We will write 100 keys, each with a value of ~50 bytes
    for (int i = 0; i < 100; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string val = "value_payload_data_for_key_" + std::to_string(i);
        s = db->Put(key, val);
        assert(s.ok() && "Put failed");
    }

    // Give background flush thread a tiny bit of time to catch up if needed
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::string stats = db->GetStats();
    std::cout << "Stats after initial writes:\n" << stats << std::endl;
    std::cout << std::flush;
    // Assert that we have some SSTables in the database (either Level 0 or Level 1+)
    bool has_any_sstables = false;
    for (int l = 0; l < 5; ++l) {
        std::string search_str = "Level " + std::to_string(l) + " SSTables: 0";
        if (stats.find(search_str) == std::string::npos) {
            has_any_sstables = true;
            break;
        }
    }
    assert(has_any_sstables && "Database should have flushed some SSTables");

    // Test 2: Verify Put and Get consistency
    std::cout << "Test 2: Verifying Get correctness...\n";
    for (int i = 0; i < 100; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string expected_val = "value_payload_data_for_key_" + std::to_string(i);
        std::string val;
        s = db->Get(key, &val);
        assert(s.ok() && "Get failed");
        assert(val == expected_val && "Value mismatch");
    }
    std::cout << "Get check passed.\n";

    // Test 3: Run Compaction and verify that Level 0 files are merged to Level 1
    std::cout << "Test 3: Running CompactRange...\n";
    s = db->CompactRange();
    assert(s.ok() && "CompactRange failed");

    stats = db->GetStats();
    std::cout << "Stats after CompactRange:\n" << stats << std::endl;
    std::cout << std::flush;
    
    // After compaction, Level 0 should be empty (0)
    assert(stats.find("Level 0 SSTables: 0") != std::string::npos && "Level 0 SSTables should be 0 after compaction");
    
    // Check that there is at least one SSTable in Level 1+
    bool has_level1_plus = false;
    for (int l = 1; l < 5; ++l) {
        std::string search_str = "Level " + std::to_string(l) + " SSTables: 0";
        if (stats.find(search_str) == std::string::npos) {
            has_level1_plus = true;
            break;
        }
    }
    assert(has_level1_plus && "Level 1+ should have SSTables after compaction");

    // Verify data remains correct after compaction
    for (int i = 0; i < 100; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string expected_val = "value_payload_data_for_key_" + std::to_string(i);
        std::string val;
        s = db->Get(key, &val);
        assert(s.ok() && "Get failed after compaction");
        assert(val == expected_val && "Value mismatch after compaction");
    }
    std::cout << "Compaction verification passed.\n";

    // Test 4: Delete keys, compact, and verify they are deleted
    std::cout << "Test 4: Deleting keys and compacting...\n";
    for (int i = 0; i < 50; ++i) {
        std::string key = "key_" + std::to_string(i);
        s = db->Delete(key);
        assert(s.ok() && "Delete failed");
    }
    
    // Give background flush thread time
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    s = db->CompactRange();
    assert(s.ok() && "CompactRange failed after deletes");

    // Verify deleted keys return NotFound and remaining keys are still there
    for (int i = 0; i < 100; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string val;
        s = db->Get(key, &val);
        if (i < 50) {
            assert(s.IsNotFound() && "Deleted key should return NotFound");
        } else {
            std::string expected_val = "value_payload_data_for_key_" + std::to_string(i);
            assert(s.ok() && "Remaining key should be found");
            assert(val == expected_val && "Value mismatch for remaining key");
        }
    }
    std::cout << "Delete and compaction verification passed.\n";

    // Test 5: Garbage Collection
    std::cout << "Test 5: Overwriting remaining keys to create vlog garbage...\n";
    // We overwrite keys 50 to 99 multiple times to fill up and rotate vlog files
    for (int run = 0; run < 15; ++run) {
        for (int i = 50; i < 100; ++i) {
            std::string key = "key_" + std::to_string(i);
            std::string val = "new_payload_run_" + std::to_string(run) + "_for_key_" + std::to_string(i);
            s = db->Put(key, val);
            assert(s.ok());
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    stats = db->GetStats();
    std::cout << "Stats before GC:\n" << stats << std::endl;
    std::cout << std::flush;

    std::cout << "Running GC...\n" << std::endl;
    s = db->RunGC();
    assert(s.ok() && "RunGC failed");

    stats = db->GetStats();
    std::cout << "Stats after GC:\n" << stats << std::endl;
    std::cout << std::flush;

    // Verify that the remaining keys are still fully readable with correct latest values
    for (int i = 50; i < 100; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string expected_val = "new_payload_run_14_for_key_" + std::to_string(i);
        std::string val;
        s = db->Get(key, &val);
        if (!s.ok()) {
            std::cerr << "Get failed for key: " << key << ", error: " << s.ToString() << std::endl;
        } else if (val != expected_val) {
            std::cerr << "Mismatch for key: " << key << "\nExpected: " << expected_val << "\nGot:      " << val << std::endl;
        }
        assert(s.ok() && "Get failed after GC");
        assert(val == expected_val && "Value mismatch after GC");
    }

    std::cout << "All Compaction and GC integration tests PASSED successfully!\n";
    return 0;
}
