#include <iostream>
#include <fstream>
#include <cstdlib>
#include <string>
#include <vector>
#include <cstring>
#include <format>
#include <map>
#include <queue>
#include <mutex>
#include <thread>
#include <filesystem>
#include <condition_variable>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <zlib.h>

// ===== CONCURRENCY PURPOSES =====
std::queue<int> client_queue;
std::mutex queue_mutex;
std::condition_variable cv;

// ===== TOKENISE GIVEN STRING FOR PARSING =====
std::vector<std::string> tokenise(const std::string& str, const std::string delim) {
  std::vector<std::string> tokens;
    size_t start = 0;
    size_t pos;

    while ((pos = str.find_first_of(delim, start)) != std::string::npos) {
        if (pos > start) { 
            tokens.push_back(str.substr(start, pos - start));
        }
        start = pos + 1;
    }

    if (start < str.size()) {
        tokens.push_back(str.substr(start));
    }

    return tokens;
}

// ===== BUILD A DICTIONARY BASED ON THE MESSAGE TO SIMPLIFY HANDLING =====
std::map<std::string, std::string> mapping_components(std::string raw_input) {
  std::map<std::string, std::string> components_map;
  std::vector<std::string> components = tokenise(raw_input, "\r\n"); 
  std::vector<std::string> header = tokenise(components.at(0), " ");

  components_map["Method"] = header.at(0);
  components_map["Endpoint"] = header.at(1);
  components_map["Version"] = header.at(2);
  components_map["Body"] = components.back();

  for (auto it = components.begin() + 1; it != components.end() - 1; ++it) {
    std::vector<std::string> temp_item = tokenise(*it, ":");
    if (temp_item.size() > 1) {
      components_map[temp_item.at(0)] = temp_item.at(1).substr(1);
    }
  }
  for (auto& [key, value] : components_map) {
    std::cout << key << " > " << value << "\n";
  }
  
  return components_map;
}

// ===== GZIP COMPRESSION METHOD =====
std::string compress_gzip(const std::string& data) {
  z_stream zs{};
  if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    throw std::runtime_error("deflateInit2 failed");
  }

  zs.next_in = (Bytef*)data.data();
  zs.avail_in = data.size();

  std::string out;
  char buffer[4096];

  int ret;
  do {
    zs.next_out = reinterpret_cast<Bytef*>(buffer);
    zs.avail_out = sizeof(buffer);

    ret = deflate(&zs, Z_FINISH);
    out.append(buffer, sizeof(buffer) - zs.avail_out);
  } while (ret == Z_OK);

  deflateEnd(&zs);

  if (ret != Z_STREAM_END) {
    throw std::runtime_error("gzip compression failed");
  }

  return out;
}

// ===== LIST OF EXISTING COMPRESSION =====
std::vector<std::string> encoding_exists(std::string list) {
  std::vector<std::string> word_list = tokenise(list, " ,");
  std::vector<std::string> accept_list = {"gzip"};
  std::vector<std::string> return_list;

  for (auto &s : word_list) {
    for (auto &t : accept_list) {
      if (t == s) {
        return_list.push_back(s);
      }
    }
  }
  return return_list;
}

// ===== HANDLE ALL METHODS =====
void endpoint_handling(const int client_socket) {
  bool keep_alive = true;
  while (keep_alive) {
    // Read the http request
    std::string buffer;
    constexpr int BUFFER_SIZE = 30727;
    buffer.resize(BUFFER_SIZE);
    ssize_t bytes_received = read(client_socket, buffer.data(), BUFFER_SIZE);

    if (bytes_received == 0) {break;}

    std::map<std::string, std::string> parsed_request = mapping_components(buffer);
    std::string response_text;

    // ===== DEFAULT =====
    if (parsed_request["Endpoint"] == "/") {
      response_text = "HTTP/1.1 200 OK\r\n\r\n";
    // ===== USER-AGENT =====
    } else if (parsed_request["Endpoint"] == "/user-agent" && parsed_request["Method"] == "GET") {
      response_text = std::format("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: {}\r\n\r\n{}", parsed_request["User-Agent"].length(), parsed_request["User-Agent"]);
    } else {
    
      std::vector<std::string> detailed_endpoint = tokenise(parsed_request["Endpoint"], "/");
      
      // ===== ECHO - GET =====
      if (detailed_endpoint.at(0) == "echo" && parsed_request["Method"] == "GET") {
        std::vector<std::string> words_in = encoding_exists(parsed_request["Accept-Encoding"].data());

        if (words_in.size() > 0) {
          std::string body_text = compress_gzip(detailed_endpoint.at(1));
          response_text = std::format("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Encoding: gzip\r\nContent-Length: {}\r\n\r\n{}", body_text.size(), body_text);
        } else {
          response_text = std::format("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: {}\r\n\r\n{}", detailed_endpoint.at(1).length(), detailed_endpoint.at(1));
        }
      // ===== FILES - GET =====
      } else if (detailed_endpoint.at(0) == "files" && parsed_request["Method"] == "GET") {
        std::string file_name = detailed_endpoint.at(1);
        file_name.insert(0, "/tmp/data/codecrafters.io/http-server-tester/");

        std::ifstream file(file_name.data());
        
        // file not found / failure to open
        if (!file) {
          std::cerr << "Fail to open\n";
          send(client_socket, "HTTP/1.1 404 Not Found\r\n\r\n", 27, 0);
          close(client_socket);
          return;
        }
        std::stringstream buffer_file;
        buffer_file << file.rdbuf();
        std::string content = buffer_file.str();
        file.close();

        response_text = std::format("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: {}\r\n\r\n{}", content.length(), content);
      // ===== FILES - POST =====
      } else if (detailed_endpoint.at(0) == "files" && parsed_request["Method"] == "POST") {
        std::string file_name = detailed_endpoint.at(1);
        file_name.insert(0, "/tmp/data/codecrafters.io/http-server-tester/");
        
        std::string body_content = parsed_request["Body"];
        int content_length = std::stoi(parsed_request["Content-Length"]);

        std::ofstream file(file_name, std::ios::binary);

        // file not found / failure to open
        if (!file.is_open()) {
          std::cerr << "Fail to open";
          send(client_socket, "HTTP/1.1 404 Not Found\r\n\r\n", 27, 0);
          close(client_socket);
          return;
        }
        file.write(body_content.data(), content_length);
        file.close();
        
        response_text = "HTTP/1.1 201 Created\r\n\r\n";
      } else {
        response_text = "HTTP/1.1 404 Not Found\r\n\r\n";
      }
    }

    if (parsed_request["Connection"] == "close") {
      keep_alive = false;
      size_t header_end = response_text.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        response_text.insert(header_end, "\r\nConnection: close");
      }
    }
    send(client_socket, response_text.data(), response_text.size(), 0);
  }
  close(client_socket);
  return;
}

// ===== CONCURRENT WORKER TO HANDLE MULTIPLE CLIENTTS =====
void worker() {
  while(true) {
    int client_socket;
    {
      std::unique_lock<std::mutex> lock(queue_mutex);
      cv.wait(lock, []{return !client_queue.empty(); });
      client_socket = client_queue.front();
      client_queue.pop();
    }
    endpoint_handling(client_socket);
  }
}

int main(int argc, char **argv) {
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  // Worker threads for concurrent processes
  constexpr int NUM_THREADS = 5;
  for (int i = NUM_THREADS; i >= 0; i--) {
    std::thread(worker).detach();
  }
  
  // Client socket
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::cerr << "Failed to create server socket\n";
    return 1;
  }

  // Ensures that we don't run into 'Address already in use' errors
  int reuse = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    std::cerr << "setsockopt failed\n";
    return 1;
  }

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(4221);

  if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) != 0) {
    std::cerr << "Failed to bind to port 4221\n";
    return 1;
  }

  int connection_backlog = 5;
  if (listen(server_fd, connection_backlog) != 0) {
    std::cerr << "listen failed\n";
    return 1;
  }

  struct sockaddr_in client_addr;
  int client_addr_len = sizeof(client_addr);

  std::cout << "Waiting for a client to connect...\n";

  // ===== REAL LOOP - PUT CLIENTS IN QUEUE, WILL BE HANDLED BY WORKER THREADS =====
  while (true) {
    int client_socket = accept(server_fd, (struct sockaddr *) &client_addr, (socklen_t *) &client_addr_len);

    if (client_socket < 0) {
      std::cerr << "Failed to accept connection\n";
      continue;
    }
    std::cout << "Client connected\n";
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      client_queue.push(client_socket);
    }
    cv.notify_one();

  }

  close(server_fd);

  return 0;
}
