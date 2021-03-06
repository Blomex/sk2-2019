//
// Created by Beniamin on 18.05.2019.
//

#include <iostream>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include "shared_structs.h"
#include "err.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <boost/algorithm/string.hpp>
#include <future>
#include <boost/log/trivial.hpp>
#include <mutex>
#include <signal.h>
using namespace boost::program_options;
namespace fs = boost::filesystem;
using namespace boost::algorithm;
using std::string;
using std::cout;
using std::vector;
using std::cin;
using std::string;
using std::getline;
using std::future;
using std::map;
#define BSIZE         256
#define TTL_VALUE     4
#define REPEAT_COUNT  8
#define SLEEP_TIME    1
#define MAX_POOL 128
namespace {
uint64_t size;
struct timeval timeout;
string addr, disc_folder;
bool signal_called = false;
}

void catch_sig(int sig){
    (void)sig;
    signal_called = true;
}

void set_sigint_catching(){
    struct sigaction action;
    sigset_t block_mask;
    sigemptyset (&block_mask);
    sigaddset(&block_mask, SIGINT);
    action.sa_handler = catch_sig;
    action.sa_mask = block_mask;
    action.sa_flags = 0;
    if (sigaction (SIGINT, &action, 0) == -1)
        syserr("sigaction");

}
void on_timeout(int timeout){
    if(timeout> 300 || timeout <= 0){
        throw std::invalid_argument("Timeout value specified by -t or --TIMEOUT must be between 1 and 300");
    }
}
//DZIALA
void set_server_options(uint16_t &port, uint64_t &space, int argc, const char *argv[]){
    try {
        options_description desc{"Options"};
        desc.add_options()
            ("MCAST_ADDR,g", value<string>(&addr)->required(), "adress")
            ("CMD_PORT,p", value<uint16_t>(&port)->required(), "port")
            ("MAX_SPACE,b", value<uint64_t>(&space)->default_value(52428800), "max_space")
            ("SHRD_FLDR,f", value<string>(&disc_folder)->required(), "disc folder")
            ("TIMEOUT,t", value<time_t>(&(timeout.tv_sec))->default_value(5)->notifier(on_timeout), "timeout");
        variables_map vm;
        store(parse_command_line(argc, argv, desc), vm);
        notify(vm);
    }
    catch (const error &ex) {
        std::cerr << ex.what() << '\n';
        exit(1);
    }
    catch (std::invalid_argument &ex) {
        std::cerr << ex.what() << '\n';
        exit(1);
    }
}
//DZIALA
void send_file_list_packet(int sock, struct sockaddr_in dest, SIMPL_CMD &received, vector<fs::path> files){
    SIMPL_CMD packet;
    packet.data[0]='\0';
    set_cmd(packet.cmd, "MY_LIST");
    packet.cmd_seq = received.cmd_seq;
    int data_counter = 0;
    string s = string(received.data);
    int delimiter_counter = -1;
    vector<string> names;
    for(auto &f: files) {
        if (f.filename().string().find(s) != string::npos) {
            if (data_counter + delimiter_counter + f.filename().string().length() > MAX_DATA_SIZE) {
                strcpy(packet.data, boost::join(names, "\n").c_str());
                if (sendto(sock,
                           (SIMPL_CMD *)&packet,
                           (size_t) 18 + data_counter + delimiter_counter,
                           0,
                           (struct sockaddr *) &dest,
                           sizeof(dest)) != data_counter + 18 + delimiter_counter) {
                    syserr("sendto");
                }
                data_counter = 0;
                delimiter_counter = -1;
                names = vector<string>();
            }
            names.push_back(f.filename().string());
            delimiter_counter++;
            data_counter += f.filename().string().length();
        }
    }
        strcpy(packet.data, boost::join(names, "\n").c_str());
        if (data_counter > 0) {
            if (sendto(sock,
                       &packet,
                       (size_t) 18 + data_counter + delimiter_counter,
                       0,
                       (struct sockaddr *) &dest,
                       sizeof(dest)) != data_counter + 18 + delimiter_counter) {
                syserr("sendto");
            }
        }
}

//DZIALA
uint64_t index_files(vector<fs::path> &Files, string &disc_folder, uint64_t space){
    uint64_t spaceTaken = 0;
    try {
        for (auto &p: fs::directory_iterator(disc_folder)) {
            if (fs::is_regular(p)) {
                spaceTaken += fs::file_size(p);
                Files.push_back(p.path());
            }
        }
    }
    catch(fs::filesystem_error &err){
        cout <<"Exception thrown: "<< err.what() << "\n";
        cout <<"Closing the server";
    }
    size =  space > spaceTaken ? space - spaceTaken : 0;
    return size;
}

//timeout same as timeout given in arguments
int create_new_tcp_socket(uint16_t &port){
  //  BOOST_LOG_TRIVIAL(trace) << "creating TCP";
    int sock;
    struct sockaddr_in serveraddr{};
    sock = socket(AF_INET, SOCK_STREAM, 0);//TCP
    if(sock<0){
        syserr("socket");
    }
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons(0);
    //timeout for accept

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *) &timeout, sizeof(timeout)) < 0) {
        error give_me_a_name("setsockopt rcvtimeout\n");
        close(sock);
        exit(1);
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *) &timeout, sizeof(timeout)) < 0) {
        error give_me_a_name("setsockopt sndtimeout\n");
        close(sock);
        exit(1);
    }

    if (bind(sock, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0)
        syserr("bind");
    if (listen(sock, 1) < 0)
        syserr("listen");
    socklen_t server_sock_len = sizeof serveraddr;
    if(getsockname(sock, (struct sockaddr*) &serveraddr, &server_sock_len)<0){
        syserr("getsockname");
    }
    port = ntohs(serveraddr.sin_port);
    return sock;
}
//chyba dziala
bool send_can_add(uint16_t port, struct sockaddr_in client, int main_sock, uint64_t cmd_seq){
    CMPLX_CMD complex;
    complex.cmd_seq = cmd_seq;
    set_cmd(complex.cmd, "CAN_ADD");
    complex.param = htobe64(uint64_t(port));
    ssize_t size_to_send = 26;
    if(sendto(main_sock, &complex, static_cast<size_t>(size_to_send), 0 , (struct sockaddr*)&client, sizeof(client)) != size_to_send){
        return false;
    }
    return true;
}
//DZIALA

bool receive_file_from_socket(int tcp_socket, const char* file){
    struct sockaddr_in source_address{};
    memset(&source_address, 0, sizeof(source_address));
    socklen_t len = sizeof(source_address);
    int sock = accept(tcp_socket, (struct sockaddr*)&source_address, &len);
    fs::path file_path(disc_folder + "/" +file);
    fs::ofstream ofs(file_path, std::ofstream::binary);
    char buffer[50000];
    int length;
    while((length = read(sock, buffer, sizeof(buffer))) > 0){
        ofs.write(buffer, length);
    }
    ofs.close();
    return length >= 0;
}
remote_file receive_file(uint64_t file_size, string file, int main_sock, struct sockaddr_in client, uint64_t cmd_seq){
    uint16_t port;
    size -= file_size;
    int sock = create_new_tcp_socket(port);
    if(sock < 0){
        return {false, file_size, file};
    }
    if(!send_can_add(port, client, main_sock, cmd_seq)){
        return {false, file_size, file};
    }
    if(!receive_file_from_socket(sock, file.c_str())){
        return {false, file_size, file};
    }
    return {true, fs::file_size(file), file};
}
void send_connect_me(string &file, uint16_t port, struct sockaddr_in client, int main_sock, uint64_t cmd_seq){
    CMPLX_CMD complex;
    set_cmd(complex.cmd, "CONNECT_ME");
    complex.param = htobe64(uint64_t(port));
    complex.cmd_seq = cmd_seq;
    strncpy(complex.data, file.c_str(), file.length());
    int size_to_send = 26 + file.length();
    if(sendto(main_sock, (char*)&complex, size_to_send,0 , (struct sockaddr*)&client, sizeof(client)) != size_to_send){
        syserr("partial sendto");
    }
}

void send_file_to_socket(int msg_sock, string file){
        fs::path filePath(disc_folder + "/" + file);
        std::ifstream file_stream{filePath.c_str(), std::ios::binary};
        if (file_stream.is_open()) {
            char buffer[50000];
            while (file_stream) {
                file_stream.read(buffer, 50000);
                ssize_t len = file_stream.gcount();
                if (write(msg_sock, buffer, len) != len) {
                    return;
                }
            }
        } else {
            std::cerr << "File opening error";
        }
        file_stream.close();
        shutdown(msg_sock, SHUT_WR);
}

//creates tcp socket, sends "CONNECT_ME" packet to client and then sends file through tcp socket
void send_file(string file, int main_sock, struct sockaddr_in client, uint64_t cmd_seq){
    uint16_t port;
    struct sockaddr_in private_client{};
    socklen_t client_address_len = sizeof(private_client);
    int sock = create_new_tcp_socket(port);
    send_connect_me(file, port, client, main_sock, cmd_seq);
    //now wait for accept for timeout seconds.
    int msg_sock = accept(sock, (struct sockaddr*)&private_client,  &client_address_len);
    if(msg_sock<0){
        close(msg_sock);
        close(sock);
        return;
    }
    //we accepted so we can send now
    //send file
    try {
        send_file_to_socket(msg_sock, file);
    }
    catch(fs::filesystem_error &err){
        return;
    }
}

//sends simple packet with NO_WAY inside cmd
void send_no_way(string &fname, uint64_t cmd_seq, int sock, struct sockaddr_in src_addr){
    SIMPL_CMD answer;
    set_cmd(answer.cmd, "NO_WAY");
    answer.cmd_seq = cmd_seq;
    strcpy(answer.data, fname.c_str());
    int size = strlen(answer.data) + 18;
    // we dont care if
    if(sendto(sock, &answer, size, 0, (struct sockaddr*)&src_addr, sizeof src_addr)!= size){
        syserr("sendto");
    }
}

//returns file descriptor to udp socket
int create_new_udp_socket(uint16_t port, string &addr, struct ip_mreq &group){
    int sock/*, optval*/;
    struct sockaddr_in local_address{};
    /* opening socket */
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        syserr("socket");
    /*reuse addr*/
    {
        int reuse=1;
        if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse)) < 0) {
            perror("setting SO_REUSEADDR");
            close(sock);
            exit(1);
        }
    }
    /* joining multicast group*/
    group.imr_interface.s_addr = htonl(INADDR_ANY);
    if (inet_aton(addr.c_str(), &group.imr_multiaddr) == 0)
        syserr("inet_aton");
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (void*)&group, sizeof group) < 0)
        syserr("setsockopt");

    /* set local address and port*/
    local_address.sin_family = AF_INET;
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    local_address.sin_port = htons(port);
    if (bind(sock, (struct sockaddr *)&local_address, sizeof local_address) < 0) {
        syserr("bind");
    }
    return sock;
}

int main(int argc, const char *argv[]) {
    set_sigint_catching();
    struct ip_mreq group{};
    uint64_t space;
    socklen_t len = sizeof(struct sockaddr_in);
    struct sockaddr_in src_addr{};
    timeout.tv_usec = 0;
    uint16_t port;
    set_server_options(port, space, argc, argv);
    //index files in folder
    std::vector<fs::path> Files;
    size = index_files(Files, disc_folder, space);

    int sock = create_new_udp_socket(port, addr, group);
    vector<future<remote_file>> filesInProgress;
    char HELLO[10], GET[10], LIST[10], ADD[10], DEL[10];
    set_cmd(HELLO, "HELLO");
    set_cmd(GET, "GET");
    set_cmd(LIST, "LIST");
    set_cmd(ADD, "ADD");
    set_cmd(DEL, "DEL");
  /*continue reading from UDP*/
  do {
      SIMPL_CMD simple_cmd;
      memset((char *) &simple_cmd, 0, sizeof(simple_cmd));
      CMPLX_CMD *complex_cmd;
      complex_cmd = (CMPLX_CMD *) &simple_cmd;
      memset(&src_addr, 0, sizeof(src_addr));
      ssize_t recv_len = recvfrom(sock, &simple_cmd, sizeof simple_cmd, 0, (struct sockaddr *) &src_addr, &len);
      if(signal_called) {
          break;
      }
      else{ //update file list 
          for(unsigned long i = 0; i < filesInProgress.size(); ++i) {
              if (filesInProgress[i].wait_for(std::chrono::microseconds(0)) == std::future_status::ready) {
                  auto a = filesInProgress[i].get();
                  if (a.isSuccessful) {
                      Files.push_back(fs::path( disc_folder + "/" + a.filename));
                  }
                  else{
                      size -= a.size;
                  }
                  std::swap(filesInProgress[i], filesInProgress.back());
                  filesInProgress.pop_back();
                  --i;
              }
          }
      }
      //has to be memcmp because correct packet is only 'HELLO\0\0\0\0\0' so 'HELLO\0\0\0\0a' etc should be skipped
      if (memcmp(HELLO, simple_cmd.cmd, 10) == 0) {
          //react on discover
          CMPLX_CMD complex;
          complex.cmd_seq = simple_cmd.cmd_seq;
          complex.param = htobe64(size);
          strcpy(complex.data, inet_ntoa(group.imr_multiaddr));
          set_cmd(complex.cmd, "GOOD_DAY");
          int send = 26 + strlen(inet_ntoa(group.imr_multiaddr));
          if(sendto(sock, &complex, send, 0, (struct sockaddr *)&src_addr, (socklen_t)sizeof (src_addr)) != send) {
              syserr("sendto1");
          }
      }
      else if(memcmp(simple_cmd.cmd, GET, 10) == 0){
          //reacts on fetch
          simple_cmd.data[recv_len-18]='\0';
          string filename = simple_cmd.data;
          if(std::find(Files.begin(), Files.end(), fs::path{disc_folder + "/" + filename}) == Files.end()){
              cout << "[PCKG ERROR] Skipping invalid package from "<<inet_ntoa(src_addr.sin_addr) <<":"<<ntohs(src_addr.sin_port)<<". Server doesn't contain such file\n";
          }
          else {
              auto t1 = std::thread(send_file, filename, sock, src_addr, (uint64_t)simple_cmd.cmd_seq);
              t1.detach();
          }
      }
      else if(memcmp(simple_cmd.cmd, LIST, 10) == 0){
          //reacts on search
          simple_cmd.data[recv_len-18] = '\0';
          send_file_list_packet(sock, src_addr, simple_cmd, Files);
      }
      else if(memcmp(simple_cmd.cmd, ADD, 10) == 0){
          //reacts on upload
          complex_cmd->data[recv_len-26] = '\0';
          string fname (complex_cmd->data);
          int64_t file_size = be64toh(complex_cmd->param);
          if ((strcmp(complex_cmd->data, "")==0 || fname.find('/')!= string::npos || (uint64_t)file_size > size
              || (fs::exists(fs::path(disc_folder + "/" + complex_cmd->data))))){
              send_no_way(fname, complex_cmd->cmd_seq, sock, src_addr);
          }
          else{
              uint64_t cmd_seq = complex_cmd->cmd_seq;
              filesInProgress.push_back(std::async(std::launch::async, receive_file, file_size, fname, sock, src_addr, cmd_seq));
          }
      }
      else if(memcmp(simple_cmd.cmd, DEL, 10) == 0 ){
          //reacts on remove
          simple_cmd.data[recv_len-18] = '\0';
          //delete file
          for (auto it = Files.begin(); it != Files.end(); ++it){
              auto f = *it;
              if(strcmp(f.c_str(), string(disc_folder + "/" + string(simple_cmd.data)).c_str()) == 0){
                  try {
                      size+= fs::file_size(f);
                      fs::remove(f);
                      Files.erase(it);

                      break;
                  }
                  catch(fs::filesystem_error &err){
                      cout << err.what();
                  }
              }
          }
      }
      else{
          if(signal_called) break;
          cout << "[PCKG ERROR] Skipping invalid package from "<<inet_ntoa(src_addr.sin_addr) <<":"<<ntohs(src_addr.sin_port)<<".\n";
      }
  }while(!signal_called);

    //we shouldn't get there any other way than by signal
    if(signal_called) return 130;

    else return -1;

}
