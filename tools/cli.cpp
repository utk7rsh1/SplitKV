#include <iostream>
#include <string>
#include <vector>
#include <sstream>

#include "splitkv/db.h"
#include "splitkv/options.h"

using namespace splitkv;

void print_help() {
    std::cout << "Commands:\n"
              << "  put <key> <value>  - Insert key-value pair\n"
              << "  get <key>          - Retrieve value\n"
              << "  delete <key>       - Remove key\n"
              << "  stats              - Show database statistics\n"
              << "  compact            - Trigger manual compaction\n"
              << "  gc                 - Trigger garbage collection\n"
              << "  help               - Print this help\n"
              << "  exit               - Exit\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <db_path>\n";
        return 1;
    }

    std::string db_path = argv[1];
    Options options;
    options.sync_writes = true;
    DB* db = nullptr;
    
    Status s = DB::Open(options, db_path, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << "\n";
        return 1;
    }

    std::cout << "Opened database at " << db_path << "\n";
    print_help();

    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "exit" || cmd == "quit") {
            break;
        } else if (cmd == "help") {
            print_help();
        } else if (cmd == "put") {
            std::string key, value;
            iss >> key >> value;
            if (key.empty()) {
                std::cout << "Error: put requires key and value\n";
                continue;
            }
            s = db->Put(key, value);
            if (s.ok()) std::cout << "OK\n";
            else std::cout << "Error: " << s.ToString() << "\n";
        } else if (cmd == "get") {
            std::string key;
            iss >> key;
            if (key.empty()) {
                std::cout << "Error: get requires key\n";
                continue;
            }
            std::string value;
            s = db->Get(key, &value);
            if (s.ok()) std::cout << value << "\n";
            else if (s.IsNotFound()) std::cout << "(nil)\n";
            else std::cout << "Error: " << s.ToString() << "\n";
        } else if (cmd == "delete") {
            std::string key;
            iss >> key;
            if (key.empty()) {
                std::cout << "Error: delete requires key\n";
                continue;
            }
            s = db->Delete(key);
            if (s.ok()) std::cout << "OK\n";
            else std::cout << "Error: " << s.ToString() << "\n";
        } else if (cmd == "stats") {
            std::cout << db->GetStats() << "\n";
        } else if (cmd == "compact") {
            s = db->CompactRange();
            if (s.ok()) std::cout << "OK\n";
            else std::cout << "Error: " << s.ToString() << "\n";
        } else if (cmd == "gc") {
            s = db->RunGC();
            if (s.ok()) std::cout << "OK\n";
            else std::cout << "Error: " << s.ToString() << "\n";
        } else {
            std::cout << "Unknown command: " << cmd << "\n";
        }
    }

    delete db;
    return 0;
}
