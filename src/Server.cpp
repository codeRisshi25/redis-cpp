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
#include <unordered_map>
#include <chrono>

// RESP Parser Structure
struct RESPData
{
  enum Type
  {
    SIMPLE_STRING,
    // todo
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

//* Structure for the KeyValueStores
struct ValueData
{
  std::string value;
  int expiry;
  std::chrono::time_point<std::chrono::steady_clock> timestamp;
};

//*Structure for CONFIG
struct Config
{
  std::string dir;
  std::string dbfilename;
  Config(std::string dir,
         std::string dbfilename)
  {
    this->dir = dir;
    this->dbfilename = dbfilename;
  };
};
// Advance declaration for parseRESP
RESPData
parseRESP(const std::string &buffer, size_t &pos);

// Simple String Parser
std::string parseSimpleString(const std::string &buffer, size_t &pos)
{
  size_t end = buffer.find("\r\n", pos);
  std::string result = buffer.substr(pos, end - pos);
  pos = end + 2;
  return result;
};

// Bulk String Parser
std::string parseBulkString(const std::string &buffer, size_t &pos)
{
  size_t end = buffer.find("\r\n", pos);
  int length = std::stoi(buffer.substr(pos, end - pos));
  pos = end + 2;
  std::string result = buffer.substr(pos, length);
  pos += length + 2;
  return result;
};

// Array Parser
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

//* Check for the first byte and determine data type and then call appropriate parsers
RESPData parseRESP(const std::string &buffer, size_t &pos)
{
  RESPData data;
  char type = buffer[pos++]; // get datatype
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

// Parser for Arrays ***imp
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
// Define the map for storing key-value pairs
std::unordered_map<std::string, ValueData> keyValueStore;

// Enumeration for all the available commands
enum CommandType
{
  PING,
  ECHO,
  GET,
  SET,
  CONFIG,
  px,
  UNKNOWN
};

// Function to map string commands to enumeration values
CommandType getCommandType(const std::string &command)
{
  static const std::unordered_map<std::string, CommandType> commandMap = {
      {"PING", PING},
      {"ECHO", ECHO},
      {"GET", GET},
      {"SET", SET},
      {"CONFIG", CONFIG},
      {"px", px},
  };

  auto it = commandMap.find(command);
  if (it != commandMap.end())
  {
    return it->second;
  }
  return UNKNOWN;
}

void clientHandle(int sock, Config config)
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

    //! calls for the RESP Pareser
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
      CommandType commandType = getCommandType(commands[cnt]);
      std::string response;
      std::string key;
      std::string value;
      int exp;
      switch (commandType)
      {
      case PING:
        n = write(sock, "+PONG\r\n", 7);
        cnt++;
        break;
      case ECHO:
        response = "+" + commands[cnt + 1] + "\r\n";
        n = write(sock, response.c_str(), response.length());
        cnt += 2;
        break;
      case SET:
      {
        key = commands[cnt + 1];
        value = commands[cnt + 2];
        if (cnt + 3 < commands.size() && commands[cnt + 3] == "px")
        {
          exp = std::stoi(commands[cnt + 4]);
          keyValueStore[key].expiry = exp;
          keyValueStore[key].timestamp = std::chrono::steady_clock::now();
          cnt += 5;
        }
        else
        {
          keyValueStore[key].expiry = -1; // No expiration time
          cnt += 3;
        }
        keyValueStore[key].value = value;
        response = "+OK\r\n";
        n = write(sock, response.c_str(), response.length());
        break;
      }
      case GET:
      {
        auto end = std::chrono::steady_clock::now();
        key = commands[cnt + 1];

        // Check if the key exists
        if (keyValueStore.count(key) == 0)
        {
          n = write(sock, "$-1\r\n", 5); // Send null bulk string for non-existent key
          cnt += 2;                      // Move past the GET and key commands
          break;
        }

        // Calculate the duration since the key was set
        auto start = keyValueStore[key].timestamp;
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        exp = keyValueStore[key].expiry;

        // Check if the key has expired
        if (exp > 0 && duration.count() > exp)
        {
          keyValueStore.erase(key);      // Remove expired key
          n = write(sock, "$-1\r\n", 5); // Send null bulk string for expired key
          cnt += 2;                      // Move past the GET and key commands
          break;
        }
        // Key is valid, return the value
        value = keyValueStore[key].value;
        response = "+" + value + "\r\n";
        n = write(sock, response.c_str(), response.length());
        cnt += 2; // Move past the GET and key commands
        break;
      }
      case CONFIG:
        if (commands[cnt + 1] == "GET")
          cnt += 2;
        response = commands[cnt] == "dir" ? "*2\r\n$3\r\ndir\r\n$" + std::to_string(config.dir.length()) + "\r\n"+ config.dir + "\r\n" : "*2\r\n$10\r\ndbfilename\r\n$" + std::to_string(config.dbfilename.length()) + "\r\n" + config.dbfilename + "\r\n";
        n = write(sock,response.c_str(),response.length());
        cnt+=2;
        break;
      }
    }
  }
};
// main function to start the server and wait for connections
// spawn threads
int main(int argc, char **argv)
{
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  // You can use print statements as follows for debugging, they'll be visible when running tests.
  std::cout << "Logs from your program will appear here!\n";
  // Uncomment this block to pass the first stage

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

  //! config arguments are below
  Config config(argv[2], argv[4]);
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
    std::thread t(clientHandle, newserver_fd, config); // Pass newserver_fd to the thread
    t.detach();
  }
  close(server_fd);
  return 0;
}