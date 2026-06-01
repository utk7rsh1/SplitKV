#include <iostream>
#include <cassert>
#include <string>
#include <filesystem>
#include "splitkv/db.h"
#include "splitkv/write_batch.h"

void TestBasicPutGetDelete() {
    std::cout << "Running TestBasicPutGetDelete...\n";
    std::filesystem::remove_all("test_db_basic");
    
    splitkv::Options options;
    splitkv::DB* db = nullptr;
    splitkv::Status s = splitkv::DB::Open(options, "test_db_basic", &db);
    assert(s.ok());
    
    s = db->Put("key1", "value1");
    assert(s.ok());
    
    std::string val;
    s = db->Get("key1", &val);
    assert(s.ok());
    assert(val == "value1");
    
    s = db->Delete("key1");
    assert(s.ok());
    
    s = db->Get("key1", &val);
    assert(s.IsNotFound());
    
    delete db;
    std::filesystem::remove_all("test_db_basic");
}

void TestWriteBatch() {
    std::cout << "Running TestWriteBatch...\n";
    std::filesystem::remove_all("test_db_batch");
    
    splitkv::Options options;
    splitkv::DB* db = nullptr;
    splitkv::Status s = splitkv::DB::Open(options, "test_db_batch", &db);
    assert(s.ok());
    
    splitkv::WriteBatch batch;
    batch.Put("k1", "v1");
    batch.Put("k2", "v2");
    batch.Delete("k1");
    batch.Put("k3", "v3");
    
    splitkv::WriteOptions wopts;
    s = db->Write(wopts, &batch);
    assert(s.ok());
    
    std::string val;
    s = db->Get("k1", &val);
    assert(s.IsNotFound());
    
    s = db->Get("k2", &val);
    assert(s.ok());
    assert(val == "v2");
    
    s = db->Get("k3", &val);
    assert(s.ok());
    assert(val == "v3");
    
    delete db;
    std::filesystem::remove_all("test_db_batch");
}

void TestWALRecovery() {
    std::cout << "Running TestWALRecovery...\n";
    std::filesystem::remove_all("test_db_recovery");
    
    splitkv::Options options;
    // Set memtable size large so it doesn't flush, keeping all writes in WAL
    options.memtable_size_limit = 1024 * 1024; 
    
    splitkv::DB* db = nullptr;
    splitkv::Status s = splitkv::DB::Open(options, "test_db_recovery", &db);
    assert(s.ok());
    
    s = db->Put("keyA", "valA");
    assert(s.ok());
    s = db->Put("keyB", "valB");
    assert(s.ok());
    s = db->Delete("keyA");
    assert(s.ok());
    
    // Close database without flushing
    delete db;
    
    // Reopen database
    db = nullptr;
    s = splitkv::DB::Open(options, "test_db_recovery", &db);
    assert(s.ok());
    
    std::string val;
    // keyA should be deleted
    s = db->Get("keyA", &val);
    assert(s.IsNotFound());
    
    // keyB should be recovered
    s = db->Get("keyB", &val);
    assert(s.ok());
    assert(val == "valB");
    
    delete db;
    std::filesystem::remove_all("test_db_recovery");
}

int main() {
    TestBasicPutGetDelete();
    TestWriteBatch();
    TestWALRecovery();
    std::cout << "All basic DB tests PASSED successfully!\n";
    return 0;
}
