#pragma once
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <cstdlib>
#include <mutex>
#include <atomic>
#include <string>
#include "ur_modern_driver/tcp_socket.h"

class URServer : private TCPSocket
{
private:
  int port_;
  SocketState state_;
  TCPSocket client_;

protected:
  virtual bool open(int socket_fd, struct sockaddr *address, size_t address_len)
  {
    return ::bind(socket_fd, address, address_len) == 0;
  }

public:
  URServer(int port);
  std::string getIP();
  bool bind();
  bool accept();
  bool write(const uint8_t* buf, size_t buf_len, size_t &written);
};