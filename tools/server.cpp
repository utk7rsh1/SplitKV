#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif

#include "splitkv/db.h"
#include "splitkv/options.h"

using namespace splitkv;

// A very basic single-threaded event loop for demonstration. 
// For production, this should use a thread pool or async IO.

void handle_client(int client_fd, DB* db) {
    char buffer[4096];
    std::string request;

    while (true) {
#ifdef _WIN32
        int bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);
#else
        int bytes_read = read(client_fd, buffer, sizeof(buffer));
#endif
        if (bytes_read <= 0) break;

        request.append(buffer, bytes_read);

        // Process line by line
        size_t pos;
        while ((pos = request.find("\r\n")) != std::string::npos || (pos = request.find("\n")) != std::string::npos) {
            std::string line = request.substr(0, pos);
            request.erase(0, pos + (request[pos] == '\r' ? 2 : 1));

            std::istringstream iss(line);
            std::string cmd;
            iss >> cmd;

            std::string response;
            if (cmd == "PING") {
                response = "+PONG\r\n";
            } else if (cmd == "PUT") {
                std::string key, value;
                iss >> key >> value;
                Status s = db->Put(key, value);
                if (s.ok()) response = "+OK\r\n";
                else response = "-ERR " + s.ToString() + "\r\n";
            } else if (cmd == "GET") {
                std::string key;
                iss >> key;
                std::string value;
                Status s = db->Get(key, &value);
                if (s.ok()) {
                    response = "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
                } else if (s.IsNotFound()) {
                    response = "$-1\r\n";
                } else {
                    response = "-ERR " + s.ToString() + "\r\n";
                }
            } else if (cmd == "DEL") {
                std::string key;
                iss >> key;
                Status s = db->Delete(key);
                if (s.ok()) response = "+OK\r\n";
                else response = "-ERR " + s.ToString() + "\r\n";
            } else if (cmd == "STATS") {
                std::string stats = db->GetStats();
                response = "$" + std::to_string(stats.size()) + "\r\n" + stats + "\r\n";
            } else {
                response = "-ERR unknown command\r\n";
            }

#ifdef _WIN32
            send(client_fd, response.c_str(), response.size(), 0);
#else
            write(client_fd, response.c_str(), response.size());
#endif
        }
    }

#ifdef _WIN32
    closesocket(client_fd);
#else
    close(client_fd);
#endif
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <db_path> [port]\n";
        return 1;
    }

    std::string db_path = argv[1];
    int port = (argc >= 3) ? std::stoi(argv[2]) : 7777;

    Options options;
    DB* db = nullptr;
    Status s = DB::Open(options, db_path, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << "\n";
        return 1;
    }

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    SOCKET server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
#endif

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

#ifdef _WIN32
    bind(server_fd, (sockaddr*)&address, sizeof(address));
    listen(server_fd, SOMAXCONN);
#else
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 10);
#endif

    std::cout << "SplitKV Server listening on port " << port << "...\n";

    while (true) {
        sockaddr_in client_addr;
#ifdef _WIN32
        int addrlen = sizeof(client_addr);
        SOCKET client_fd = accept(server_fd, (sockaddr*)&client_addr, &addrlen);
        if (client_fd != INVALID_SOCKET) {
            std::thread(handle_client, client_fd, db).detach();
        }
#else
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (client_fd >= 0) {
            std::thread(handle_client, client_fd, db).detach();
        }
#endif
    }

    delete db;
#ifdef _WIN32
    closesocket(server_fd);
    WSACleanup();
#else
    close(server_fd);
#endif
    return 0;
}
