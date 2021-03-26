#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <iostream>
#include <fstream>
#include <net/if.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_FILE "/tmp/assignment1"

using namespace std;

const int buffer_size = 512;
bool is_running;

// I/O buffers
char read_buffer[buffer_size];
char write_buffer[buffer_size];
int buff_len;

const string mon_cmd = "Monitor";
const string link_up_cmd = "Set Link Up";
// cmd responses
const string ready_res = "Ready";
const string mon_res = "Monitoring";
const string done_res = "Done";
const string link_down = "Link Down";

void sig_handler(int sig) {
  switch (sig) {
    case SIGINT:
      cout << "intfMonitor: SIGINT received for '" << getpid() << "' - Shutting down gracefully..." << endl;
      is_running = false;
      break;
  }
}

void report_done(int socket_fd) {
  bzero(write_buffer, sizeof(write_buffer));
  buff_len = sprintf(write_buffer, "%s", done_res.c_str());
  // intfMonitor is no longer running, so tells the network monitor it's Done
  if (write(socket_fd, write_buffer, buff_len) < 0) {
    cout << "intfMonitor: Failed to report Done to network monitor – Terminating..." << endl;
    perror(strerror(errno));
    exit(1);
  }
  close(socket_fd);
}

// single point of failure
void fail() {
  cout << "intfMonitor: Process " << getpid() << " has failed" << endl;
  perror(strerror(errno));
  exit(1);
}

int get_intf_stat(int socket_fd, string pathname, string statname) {
  string stat_path = pathname + statname;
  ifstream stat_file(stat_path);
  if (stat_file.bad()) {
    report_done(socket_fd);
    cout << "intfMonitor: Failed to open interface up count file '" << stat_path << "'" << endl;
    exit(1);
  }

  if (stat_file.is_open()) {
    string line((istreambuf_iterator<char>(stat_file)), (istreambuf_iterator<char>()));
    stat_file.close();
    return stoi(line);
  }

  return 0;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    cout << "Usage error: ./intfMonitor <interface-name>" << endl;
    fail();
  }

  // first argument is the interface_name
  string intf_name = argv[1];

  // signal handling setup
  struct sigaction action;
  action.sa_handler = sig_handler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  sigaction(SIGINT, &action, NULL);

  // socket stuff
  int socket_fd;
  struct sockaddr_un serv_addr;
  socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    cout << "intfMonitor: Failed to open socket – Terminating..." << endl;
    fail();
  }

  serv_addr.sun_family = AF_UNIX;
  strncpy(serv_addr.sun_path, SOCKET_FILE, strlen(SOCKET_FILE) + 1);
  if (connect(socket_fd, (struct sockaddr*)&serv_addr, sizeof(struct sockaddr_un)) < 0) {
    cout << "intfMonitor: Failed to connect to network monitor socket – Terminating..." << endl;
    fail();
  }

  // notify network monitor that interface monitor is Ready
  if (write(socket_fd, ready_res.c_str(), strlen(ready_res.c_str()))  < strlen(ready_res.c_str())) {
    cout << "intfMonitor: Failed to send Ready to network monitor – Terminating..." << endl;
    fail();
  }

  // interface stuff
  const string base_path = "/sys/class/net/";
  // base path + eth0 + null byte
  const string intf_path = base_path + intf_name;
  const string intf_stats_path = intf_path + "/statistics";

  // read Monitor command from network monitor
  bzero(read_buffer, sizeof(read_buffer));
  if (read(socket_fd, read_buffer, buffer_size - 1) < 0) {
    cout << "intfMonitor: Failed to read Monitor command from network monitor – Terminating..." << endl;
    fail();
  }

  if (strcmp(read_buffer, mon_cmd.c_str()) == 0) {
    // respond with Monitoring
    if (write(socket_fd, mon_res.c_str(), strlen(mon_res.c_str())) < strlen(mon_res.c_str())) {
      cout << "intfMonitor: Failed to respond to Monitor command – Terminating..." << endl;
      fail();
    }
  }

  // now the interface monitor can poll stats
  is_running = true;
  while (is_running) {
    struct dirent* intf_dir_entry = NULL;
    DIR* intf_dir = opendir(intf_path.c_str());

    // interface stats
    string state;
    int up_count;
    int down_count;
    int rx_bytes;
    int rx_dropped;
    int rx_errors;
    int rx_packets;
    int tx_bytes;
    int tx_dropped;
    int tx_errors;
    int tx_packets;

    if (intf_dir == NULL) {
      report_done(socket_fd);
      cout << "intfMonitor: Failed to open interface directory '" << intf_path << "'" << endl;
      fail();
    }

    // state, up_count and down_count live 1 directory level above other stats
    while ((intf_dir_entry = readdir(intf_dir)) != NULL) {
      if (strcmp(intf_dir_entry->d_name, "operstate") == 0) {
        string operstate_path = intf_path + "/operstate";

        ifstream operstate_file(operstate_path);
        if (operstate_file.bad()) {
          report_done(socket_fd);
          closedir(intf_dir);
          cout << "intfMonitor: Failed to open interface up count file '" << operstate_path << "'" << endl;
          exit(1);
        }

        if (operstate_file.is_open()) {
          string line((istreambuf_iterator<char>(operstate_file)), (istreambuf_iterator<char>()));
          operstate_file.close();
          state = line.erase(line.find_last_not_of(" \n\r\t") + 1);
        }
      }
      if (strcmp(intf_dir_entry->d_name, "carrier_up_count") == 0) {
        up_count = get_intf_stat(socket_fd, intf_path, "/carrier_up_count");
      }

      if (strcmp(intf_dir_entry->d_name, "carrier_down_count") == 0) {
        down_count = get_intf_stat(socket_fd, intf_path, "/carrier_down_count");
      }
    }

    // open /statistics directory
    DIR* intf_stats_dir = opendir(intf_stats_path.c_str());
    if (intf_stats_dir == NULL) {
      report_done(socket_fd);
      cout << "intfMonitor: Failed to open interface statistics directory '" << intf_stats_path << "'" << endl;
      fail();
    }

    // parse interface stats
    while ((intf_dir_entry = readdir(intf_stats_dir)) != NULL) {
      if (strcmp(intf_dir_entry->d_name, "rx_bytes") == 0) {
        rx_bytes = get_intf_stat(socket_fd, intf_stats_path, "/rx_bytes");
      }

      if (strcmp(intf_dir_entry->d_name, "rx_dropped") == 0) {
        rx_dropped = get_intf_stat(socket_fd, intf_stats_path, "/rx_dropped");
      }

      if (strcmp(intf_dir_entry->d_name, "rx_errors") == 0) {
        rx_errors = get_intf_stat(socket_fd, intf_stats_path, "/rx_errors");
      }

      if (strcmp(intf_dir_entry->d_name, "rx_packets") == 0) {
        rx_packets = get_intf_stat(socket_fd, intf_stats_path, "/rx_packets");
      }

      if (strcmp(intf_dir_entry->d_name, "tx_bytes") == 0) {
        tx_bytes = get_intf_stat(socket_fd, intf_stats_path, "/tx_bytes");
      }

      if (strcmp(intf_dir_entry->d_name, "tx_dropped") == 0) {
        tx_dropped = get_intf_stat(socket_fd, intf_stats_path, "/tx_dropped");
      }

      if (strcmp(intf_dir_entry->d_name, "tx_errors") == 0) {
        tx_errors = get_intf_stat(socket_fd, intf_stats_path, "/tx_errors");
      }

      if (strcmp(intf_dir_entry->d_name, "tx_packets") == 0) {
        tx_packets = get_intf_stat(socket_fd, intf_stats_path, "/tx_packets");
      }
    }

    // when interface is down
    if (strcmp(state.c_str(), "down") == 0) {
      bzero(write_buffer, sizeof(write_buffer));
      // send Link Down to network monitor
      if (write(socket_fd, link_down.c_str(), strlen(link_down.c_str())) < strlen(link_down.c_str())) {
        cout << "intfMonitor: Failed to write Link Down command to network monitor – Terminating..." << endl;
        fail();
      }

      bzero(read_buffer, sizeof(read_buffer));
      // read Set Link Up response
      if (read(socket_fd, read_buffer, buffer_size - 1) < 0) {
        cout << "intfMonitor: Failed to read Set Link Up command – Terminating..." << endl;
        fail();
      }

      if (strcmp(read_buffer, link_up_cmd.c_str()) == 0) {
        int intf_socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct ifreq ifr;

        if (intf_socket_fd < 0) {
          cout << "intfMonitor: Failed to open interface socket – Terminating..." << endl;
          fail();
        }

        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, intf_name.c_str(), IFNAMSIZ);
        ifr.ifr_flags |= IFF_UP;
        // set interface link up
        if (ioctl(intf_socket_fd, SIOCSIFFLAGS, &ifr) < 0) {
          cout << "intfMonitor: Failed to Set Link Up using ioctl – Terminating..." << endl;
          fail();
        }
      }
    }

    bzero(write_buffer, sizeof(write_buffer));
    buff_len = sprintf(write_buffer, "Interface name: %s state:%s up_count:%d down_count:%d rx_bytes:%d rx_dropped:%d rx_errors:%d rx_packets:%d tx_bytes:%d tx_dropped:%d tx_errors:%d tx_packets:%d", intf_name.c_str(), state.c_str(), up_count, down_count, rx_bytes, rx_dropped, rx_errors, rx_packets, tx_bytes, tx_dropped, tx_errors, tx_packets);
    // send interface stats to network monitor
    if (write(socket_fd, write_buffer, buff_len) < 0) {
      cout << "intfMonitor: Failed to write to network monitor – Terminating..." << endl;
      fail();
    }
    sleep(1);
  }

  report_done(socket_fd);
  close(socket_fd);
  return 0;
}
