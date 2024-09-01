#include <iostream>
#include <cstdlib>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <thread>
#include <vector>

struct RESPData
{
  enum Typeh
  {
    SIMPLE_STRING,
    // ERROR,
    // INTEGER,
    BULK_STRING,
    ARRAY
  } type;
  std::string simpleString;
  // std::string error;
  // int integer;
  std::string bulkString;
  std::vector<RESPData> array;
};

RESPData parseRESP(const std::string &buffer, size_t &pos);

std::string parseSimpleString(const std::string &buffer, size_t &pos)
{
  size_t end = buffer.find("\r\n", pos);
  std::string result = buffer.substr(pos, end - pos);
  pos = end + 2;
  return result;
};

std::string parseBulkString(const std::string &buffer, size_t &pos)
{
  size_t end = buffer.find("\r\n", pos);
  int length = std::stoi(buffer.substr(pos, end - pos));
  pos = end + 2;
  std::string result = buffer.substr(pos, length);
  pos += length + 2;
  return result;
};

std::vector<RESPData> parseArray(const std::string &buffer, size_t pos)
{
  size_t end = buffer.find("\r\n", pos);
  int numElements = std::stoi(buffer.substr(pos, end - pos));
  pos = end + 2;
  std::vector<RESPData> result;
  for (int i = 0; i < numElements; ++i)
  {
    result.push_back(parseRESP(buffer, pos));
  };
  return result;
};

RESPData parseRESP(const std::string &buffer, size_t &pos)
{
  RESPData data;
  char type = buffer[pos++];
  switch (type)
  {
  case '+':
    data.type = RESPData::SIMPLE_STRING;
    data.simpleString = parseSimpleString(buffer, pos);
    break;

  case '$':
    data.type = RESPData::BULK_STRING;
    data.bulkString = parseBulkString(buffer, pos);
    break;
  case '*':
    data.type = RESPData::ARRAY;
    data.array = parseArray(buffer, pos);
    break;
  default:
    throw std::runtime_error("Unkown RESP Datatype");
  }
  return data;
}

std::vector<RESPData> client_parser(const std::string &str)
{
  size_t pos = 0;
  RESPData data = parseRESP(str, pos);
  if (data.type == RESPData::ARRAY)
  {
    return data.array;
  }
  return {};
}

void client_res(int);

int main(int argc, char **argv)
{
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  // You can use print statements as follows for debugging, they'll be visible when running tests.
  std::cout << "Logs from your program will appear here!\n";

  // Uncomment this block to pass the first stage
  //
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  int newserver_fd;
  std::vector<std::thread> threads;
  if (server_fd < 0)
  {
    std::cerr << "Failed to create server socket\n";
    return 1;
  }

  // Since the tester restarts your program quite often, setting SO_REUSEADDR
  // ensures that we don't run into 'Address already in use' errors
  int reuse = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
  {
    std::cerr << "setsockopt failed\n";
    return 1;
  }

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(6379);

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0)
  {
    std::cerr << "Failed to bind to port 6379\n";
    return 1;
  }

  int connection_backlog = 5;
  if (listen(server_fd, connection_backlog) != 0)
  {
    std::cerr << "listen failed\n";
    return 1;
  }

  struct sockaddr_in client_addr;
  int client_addr_len = sizeof(client_addr);

  std::cout << "Waiting for a client to connect...\n";
  while (true)
  {
    newserver_fd = accept(server_fd, (struct sockaddr *)&client_addr, (socklen_t *)&client_addr_len);
    if (newserver_fd < 0)
    {
      std::cerr << "ERROR on accept\n";
      continue;
    }
    std::cout << "Client connected\n";
    std::thread t(client_res, newserver_fd);  // Pass newserver_fd to the thread
    t.detach(); 
  }
  close(server_fd);
  return 0;
}

void client_res(int sock)
{
  char buffer[512];
  while (true)
  {
    bzero(buffer, 512);
    int n = read(sock, buffer, 511);
    if (n < 0)
    {
      std::cerr << "Error reading from socket\n";
      close(sock);
      return;
    }
    if (n == 0)
    {
      std::cout << "Client disconnected\n";
      close(sock);
      return;
    }
    std::string str = buffer;
    std::vector<RESPData> result = client_parser(buffer);
    std::vector<std::string> commands;
    for (const auto &element : result)
    {
      if (element.type == RESPData::BULK_STRING)
      {
        commands.push_back(element.bulkString);
      }
    }
    int cnt = 0;
    while (cnt < commands.size())
    {
      if (commands[cnt] == "PING")
      {
        n = write(sock, "+PONG\r\n", 7);
        cnt++;
      }
      else if (commands[cnt] == "ECHO")
      {
        std::string response = "+" + commands[cnt + 1] + "\r\n";
        n = write(sock, response.c_str(), response.length());
        cnt += 2;
      }
    }
  }
}