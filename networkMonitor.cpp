#include <errno.h>
#include <iostream>
#include <vector>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_FILE "/tmp/assignment1"

using namespace std;

bool is_running;
bool has_forked;

const int n_intfs = 3;
const string intf_name[n_intfs] = { "lo", "eth0", "eth1" };
const string intf_mon = "./intfMonitor";
const int buffer_size = 512;

void sig_handler(int sig) {
  switch(sig) {
    case SIGINT:
      cout << "networkMonitor: SIGINT received – Shutting down gracefully..." << endl;
      is_running = false;
      break;
  }
}

void fail() {
  cout << "networkMonitor: Process " << getpid() << " has failed" << endl;
  perror(strerror(errno));
  exit(1);
}

int main() {
  // child procs
  vector<pid_t> child_pids;
  int child_status = -1;
  pid_t child_pid = 0;

  // signal handler setup
  struct sigaction action;
  action.sa_handler = sig_handler;
  // clear sigaction mask
  sigemptyset(&action.sa_mask);
  // no flags
  action.sa_flags = 0;
  // register SIGINT handler
  sigaction(SIGINT, &action, NULL);

  // I/O buffers
  char read_buffer[buffer_size];

  // socket vars
  int master_sock_fd;
  struct sockaddr_un serv_addr;
  vector<int> client_conns;
  fd_set client_fds;

  memset(&serv_addr, 0, sizeof(serv_addr));
  master_sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (master_sock_fd < 0) {
    cout << "networkMonitor: Failed to create socket – Terminating..." << endl;
    fail();
  }

  serv_addr.sun_family = AF_UNIX;
  strncpy(serv_addr.sun_path, SOCKET_FILE, strlen(SOCKET_FILE) + 1);
  unlink(SOCKET_FILE);

  // use :: prefix to avoid namespace conflict, not std::bind added in C++11
  // https://stackoverflow.com/questions/10035294/compiling-code-that-uses-socket-function-bind-with-libcxx-fails
  if (::bind(master_sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
    cout << "networkMonitor: Failed to bind socket – Terminating..." << endl;
    close(master_sock_fd);
    fail();
  }

  if (listen(master_sock_fd, n_intfs) < 0) {
    cout << "networkMonitor: Failed to listen on socket – Terminating..." << endl;
    unlink(SOCKET_FILE);
    close(master_sock_fd);
    fail();
  }

  FD_ZERO(&client_fds);

  // child proc cmds
  const string ready_cmd = "Ready";
  const string done_cmd = "Done";
  const string mon_cmd = "Monitor";
  const string link_up_cmd = "Set Link Up";
  const string mon_res = "Monitoring";
  const string link_down_res = "Link Down";

  is_running = true;
  has_forked = false;
  while (is_running) {
      cout << "Number of client connections" << client_conns.size() << endl;
      // stop running when all client conns are closed and we have forked
      if (has_forked && client_conns.size() == 0) {
        cout << "All client connections dropped – Shutting down..." << endl;
        is_running = false;
      }

      for (int i = 0; i < n_intfs && !has_forked; i++) {
        // store child pid in parent scope
        child_pids.push_back(fork());
        if (child_pids[i] == 0) {
          execlp(intf_mon.c_str(), intf_mon.c_str(), intf_name[i].c_str(), NULL);
        }

        client_conns.push_back(accept(master_sock_fd, NULL, NULL));
        if (client_conns[i] < 0) {
          cout << "networkMonitor: Failed to accept client connection from child process – Terminating..." << endl;
          fail();
        }

        FD_SET(client_conns[i], &client_fds);
      }

      // make sure we only fork 3 procs once
      has_forked = true;

      for (int i = 0; i < client_conns.size(); i++) {
        if (FD_ISSET(client_conns[i], &client_fds)) {
        cout << "Client with pid '" << child_pids[i] << "' is connected to fd '" << client_conns[i] << "'" << endl;
          bzero(read_buffer, sizeof(read_buffer));
          if (read(client_conns[i], read_buffer, buffer_size - 1) < 0) {
            cout << "networkMonitor: Failed to read from child process – Terminating..." << endl;
            fail();
          }

          // close the connection when client sends Done or an empty buffer
          if (strcmp(read_buffer, done_cmd.c_str()) == 0 || strlen(read_buffer) == 0) {
            cout << "Closing connection to child with pid '" << child_pids[i] << "' and fd '" << client_conns[i] << "'" << endl;
            FD_CLR(client_conns[i], &client_fds);
            close(client_conns[i]);
            client_conns.erase(client_conns.begin() + i);
            child_pids.erase(child_pids.begin() + i);
          }

          if (strcmp(read_buffer, link_down_res.c_str()) == 0) {
            cout << "Client with pid '" << child_pids[i] << "' and fd '" << client_conns[i] << "' is down" << endl;
            if (write(client_conns[i], link_up_cmd.c_str(), strlen(link_up_cmd.c_str())) < strlen(link_up_cmd.c_str())) {
              cout << "networkMonitor: Failed to send Set Link Up command to child process – Terminating..." << endl;
              fail();
            }
          }

          cout << read_buffer << endl << endl;

          if (strcmp(read_buffer, ready_cmd.c_str()) == 0) {
            if (write(client_conns[i], mon_cmd.c_str(), strlen(mon_cmd.c_str())) < strlen(mon_cmd.c_str())) {
              cout << "networkMonitor: Failed to send Monitor command to child process – Terminating..." << endl;
              fail();
            }

            bzero(read_buffer, sizeof(read_buffer));
            if (read(client_conns[i], read_buffer, buffer_size - 1) < 0) {
              cout << "networkMonitor: Failed to read child process response to Monitor command – Terminating..." << endl;
              fail();
            }
          }
        }
      }
    sleep(1);
  }


  for (int i = 0; i < client_conns.size(); i++) {
    cout << "networkMonitor: Killing client with pid '" << child_pids[i] << "' and fd '" << client_conns[i] << "'" << endl;
    kill(child_pids[i], SIGINT);
    sleep(1);
    FD_CLR(client_conns[i], &client_fds);
    close(client_conns[i]);
  }

  while (child_pid >= 0) {
    child_pid = wait(&child_status);
  }

  close(master_sock_fd);
  unlink(SOCKET_FILE);
  return 0;
}
