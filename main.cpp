#include <iostream>
#include <sstream>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include "shared_structs.h"
#include "err.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <arpa/inet.h>
#include <boost/algorithm/string.hpp>
#include <future>
#include <boost/log/trivial.hpp>
#include <fcntl.h>
#include <random>
#define N 100
#define BUFFER_SIZE   2000
#define QUEUE_LENGTH     5
std::atomic<bool> should_exit = false;
using namespace boost::program_options;
using namespace boost::algorithm;
namespace fs = boost::filesystem;
using std::string;
using std::cout;
using std::cin;
using std::getline;
using std::vector;
using std::future;
using std::cerr;
using std::map;
using namespace std::chrono;
const string HELLO = "HELLO";
const string GOOD_DAY = "GOOD_DAY";
const string LIST = "LIST";
const string MY_LIST = "MY_LIST";
const string CONNECT_ME = "CONNECT_ME";
const string DEL = "DEL";
const string ADD = "ADD";
const string GET = "GET";
namespace {
uint64_t cmd_seq = 1;
int timeout;
string savedir;
map<string, string> last_search;
}
//TODO może być w 1 pliku
//TODO naprawić ścieżkę
promise_message send_file_to_socket(int msg_sock, string file){
    cout << file << "\n";
    fs::path filePath(file);
    std::ifstream file_stream{filePath.c_str(), std::ios::binary};
    if(file_stream.is_open()){
        char buffer[50000];
        while(file_stream){
            file_stream.read(buffer, 50000);
            ssize_t len = file_stream.gcount();
            if(write(msg_sock, buffer, len) != len){
                return{false, ""};
            }
        }
    }
    else{
        return {false, ""};
    }
    file_stream.close();
    shutdown(msg_sock, SHUT_WR);
    return {true, ""};
}


int prepare_to_send(SIMPL_CMD &packet, const char cmd[10], const string &data) {
    for (int i = 0; i < 10; i++) {
        packet.cmd[i] = cmd[i];
    }
    strncpy(packet.data, data.c_str(), sizeof(packet.data));
    packet.cmd_seq = htobe64(cmd_seq);
    return data.length() + sizeof(packet.cmd_seq) + sizeof(packet.cmd);
}

int prepare_to_send_param(CMPLX_CMD &packet, uint64_t param, const string &data) {
    strncpy(packet.data, data.c_str(), sizeof(packet.data));
    packet.cmd_seq = cmd_seq;
    packet.param = htobe64(param);
    return data.length() + sizeof(packet.cmd_seq) + sizeof(packet.cmd) + sizeof(packet.param);
}

void on_timeout(int timeout) {
    if (timeout > 300 || timeout <= 0) {
        throw std::invalid_argument("Timeout value specified by -t or --TIMEOUT must be between 1 and 300");
    }
}

void perform_search(const string &s, int sock, struct sockaddr_in &remote_address) {
    last_search = map<string, string>();
    char cmd[10];
    set_cmd(cmd, LIST);
    SIMPL_CMD packet;

    int length = prepare_to_send(packet, cmd, s);
    if (sendto(sock, &packet, length, 0, (struct sockaddr *) &remote_address, sizeof remote_address) != length)
        syserr("write");
    SIMPL_CMD p3;
    struct sockaddr_in server;
    socklen_t len = sizeof(struct sockaddr_in);
    auto start = high_resolution_clock::now();

    while (true) {
        //TODO do poprawy
        auto end = high_resolution_clock::now();
        duration<double, std::milli> elapsed = end - start;
        if (elapsed.count() >= timeout * 1000) {
            break;
        }
        int x = recvfrom(sock, &p3, sizeof p3, 0, (struct sockaddr *) &server, &len);
        if (x < 0) {
            continue;
        }
        p3.data[x - 18] = '\0';
        //check if we got packet we are expecting
        int port = ntohs(server.sin_port);
        string ip = inet_ntoa(server.sin_addr);
        if (strncmp(p3.cmd, "MY_LIST", 8) == 0 && be64toh(p3.cmd_seq) == cmd_seq) {
            vector<string> result;
            boost::split(result, p3.data, boost::is_any_of("\n"));
            for (string &str : result) {
                cout << str << " (" << ip << ")\n";
                last_search[str] = ip;
            }
        } else {
            cout << "[PCKG ERROR]  Skipping invalid package from " << ip << ":" << port << ".\n";
        }
    }

}

int create_udp_socket(string &addr, uint16_t port, struct sockaddr_in &remote_address){
struct sockaddr_in localSock;
    struct timeval s_timeout{};
    s_timeout.tv_usec = 10;
    int sock = socket(PF_INET, SOCK_DGRAM, 0); // creating IPv4 UDP socket
    if (sock < 0) {
        syserr("socket");
    }

    //set timeout
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *) &s_timeout, sizeof(s_timeout)) < 0) {
        error give_me_a_name("setsockopt rcvtimeout\n");
        close(sock);
        exit(1);
    }
    /*
    //activate bcast
    int optval = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (void *) &optval, sizeof optval) < 0)
        syserr("setsockopt broadcast");
*/
/* Enable SO_REUSEADDR to allow multiple instances of this */
/* application to receive copies of the multicast datagrams. */
/*
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *) &reuse, sizeof(reuse)) < 0) {
        syserr("secksockopt: reuse");
    }*/

    //binding port number with ip address
    memset((char *) &localSock, 0, sizeof(localSock));
    localSock.sin_addr.s_addr = htonl(INADDR_ANY); // listening on all interfaces
    localSock.sin_port = htons(0); // listening on port PORT_NUM
    localSock.sin_family = AF_INET; // IPv4

    if (bind(sock, (struct sockaddr *) &localSock, sizeof(localSock)) < 0) {
        syserr("bind");
    }

    /* ustawienie adresu i portu odbiorcy */
    remote_address.sin_family = AF_INET;
    remote_address.sin_port = htons(port);
    if (inet_aton(addr.c_str(), &remote_address.sin_addr) == 0)
        syserr("inet_aton");
    return sock;
}


int create_tcp_socket(CMPLX_CMD message){
    int sock;
    struct sockaddr_in serveraddr{};
    sock = socket(AF_INET, SOCK_STREAM, 0);//TCP
    if(sock<0){
        syserr("socket");
    }
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons(static_cast<uint16_t>(be64toh(message.param)));
    //timeout for accept
    timeval timeout_tval;
    timeout_tval.tv_sec = 2;
    timeout_tval.tv_usec = 0;
    sleep(1);
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *) &timeout_tval, sizeof(timeout_tval)) < 0) {
        error give_me_a_name("setsockopt rcvtimeout\n");
        close(sock);
        exit(1);
    }
    cout <<"PORT: "<< serveraddr.sin_port <<" real: "<< ntohs(serveraddr.sin_port) <<"\n";
    //TODO zły timeout
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *) &timeout_tval, sizeof(timeout_tval)) < 0) {
        error give_me_a_name("setsockopt sndtimeout\n");
        close(sock);
        exit(1);
    }
    if(connect(sock, (struct sockaddr*)&serveraddr, sizeof serveraddr) < 0){
        syserr("connect");
    }

    return sock;
}

bool receive_file_from_socket(int tcp_socket, const char* file){
    cout << file << "\n";
    fs::path file_path( savedir + "/" +file);
    fs::ofstream ofs(file_path, std::ofstream::binary);
    char buffer[50000];
    int length;
    while((length = read(tcp_socket, buffer, sizeof(buffer))) > 0){
        ofs.write(buffer, length);
    }
    if(length<0){
        syserr("length");
    }
    ofs.close();
    return length >= 0;
}

promise_message perform_fetch(string &s, struct sockaddr_in remote_address) {
    //wybranie serwera z listy po ostatnim "search"
    auto ip = last_search.find(s);
    if(ip != last_search.end()) {
        int msg_sock = create_udp_socket(ip->second, ntohs(remote_address.sin_port), remote_address);
        char cmd[10];
        set_cmd(cmd, GET);
        SIMPL_CMD packet;
        int length = prepare_to_send(packet, cmd, s);
        if (sendto(msg_sock, &packet, length, 0, (struct sockaddr *) &remote_address, sizeof remote_address) != length)
            syserr("sendto");
        //czekamy na CONNECT_ME
        socklen_t remote_len = sizeof(remote_address);
        while (1) {
            if (recvfrom(msg_sock, &packet, sizeof(SIMPL_CMD), 0, (struct sockaddr *) &remote_address, &remote_len)
                < 0) {
                //no answer = no file
                continue;
            }
            CMPLX_CMD *answer = (CMPLX_CMD *) &packet;
            if (strncmp(answer->cmd, "CONNECT_ME", 10) == 0 && be64toh(answer->cmd_seq) == cmd_seq) {
                cout << "correct\n";
                int tcp_sock = create_tcp_socket(*answer);
                promise_message status = receive_file_from_socket(tcp_sock, ip->first.c_str());
                char port[20];
                sprintf(port, "%u", ntohs(remote_address.sin_port));
                if(status.isSuccessful){
                    status.message = "File " + s + " downloaded (" + inet_ntoa(remote_address.sin_addr) + ":" + port + ")\n";
                }
                else{
                    status.message = "File " + s + " download failed (" + inet_ntoa(remote_address.sin_addr) + ":" + port + ")" + status.message;
                }
                return status;
            }
            else if(!be64toh(answer->cmd_seq) == cmd_seq){

            }
            else {
                return;
            }
        }
    }
    else{

    }

}

void perform_discover(int sock, struct sockaddr_in &remote_address) {
    char cmd[10];
    set_cmd(cmd, HELLO);
    SIMPL_CMD packet;
    int length = prepare_to_send(packet, cmd, "");

    if (sendto(sock, &packet, length, 0, (struct sockaddr *) &remote_address, sizeof remote_address) != length)
        syserr("sendto");
    CMPLX_CMD p3;
    struct sockaddr_in server{};
    socklen_t len = sizeof(struct sockaddr_in);
    auto start = high_resolution_clock::now();
    while (true) {
        //TODO do poprawy
        auto end = high_resolution_clock::now();
        duration<double, std::milli> elapsed = end - start;
        if (elapsed.count() >= timeout * 1000) {
            break;
        }
        int x = recvfrom(sock, &p3, sizeof p3, 0, (struct sockaddr *) &server, &len);
        if (x < 0) {
            continue;
        }
        p3.data[x - 26] = '\0';
        //check if we got packet we are expecting
        int port = ntohs(server.sin_port);
        string ip = inet_ntoa(server.sin_addr);
        if (strncmp(p3.cmd, "GOOD_DAY", 7) == 0 && be64toh(p3.cmd_seq) == cmd_seq) {
            cout << "Found " << ip << " (" << p3.data << ") with free space " << be64toh(p3.param) << "\n";
        } else {
            cout << "[PCKG ERROR]  Skipping invalid package from " << ip << ":" << port << ".\n";
        }
    }
}

void perform_remove(string &name, int sock, struct sockaddr_in &remote_address){
    struct sockaddr_in debug = remote_address;
    remote_address = debug;
    SIMPL_CMD packet;
    int length = prepare_to_send(packet, "DEL", name);
    if (sendto(sock, &packet, length, 0, (struct sockaddr *) &remote_address, sizeof remote_address) != length)
        syserr("sendto");
}
promise_message send_add_and_wait_for_answer(int sock, string filename, struct sockaddr_in remote_address){
    CMPLX_CMD add_packet;
    SIMPL_CMD answer;
    promise_message status;
    CMPLX_CMD *complex_answer = (CMPLX_CMD*)&answer;
    set_cmd(add_packet.cmd, "ADD");
    int size = prepare_to_send_param(add_packet, fs::file_size(filename), filename);
    if(sendto(sock, &add_packet, size, 0, (struct sockaddr*) &remote_address, sizeof remote_address)!= size){
        syserr("sendto");
    }
    socklen_t len = sizeof(remote_address);
    //TODO zwiekszyc timeout - w tej chwili jest smiesznie niski. Ewentualnie ta dziwna petla jak wczesniej
    timeval t{timeout, 0};
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&t, sizeof(t)) < 0) {
        syserr("so receive");
        close(sock);
        exit(1);
    }
    int received = recvfrom(sock, &answer, sizeof(answer), 0, (struct sockaddr*)&remote_address, &len);
    if(strcmp("CAN_ADD", answer.cmd) == 0){
        //TODO polacz sie i wysylaj plik
        cout <<"CAN ADD received.. waiting for connection boys\n";
        complex_answer->data[received-26] = '\0';
        int msg_sock = create_tcp_socket(*complex_answer);
        status = send_file_to_socket(msg_sock, filename);
        char port[20];
        sprintf(port, "%u", ntohs(remote_address.sin_port));
        if(status.isSuccessful){
            status.message = "File "+ filename + " uploaded (" + inet_ntoa(remote_address.sin_addr) + ":" + port + ")\n";
        }
        else{
            status.message = "File "+ filename + " uploading failed (" + inet_ntoa(remote_address.sin_addr) + ":" + port + ")\n" + status.message;
        }
        return status;
    }
    else if(strcmp("NO_WAY", answer.cmd)){
        answer.data[received-18] = '\0';
        return {false, ""};
    }
    return {false, ""};
}

promise_message perform_upload(string &filename, string &addr, int port, struct sockaddr_in remote_address) {
    //discover servers first
    //create new socket to discover
    vector<std::pair<uint64_t, string>> servers;
    fs::path file = filename.c_str();
    if(!fs::exists(file)){
        string err = "File "+ fs::current_path().string() + filename + " does not exist\n";
        return{false, err};
    }

    //Znajduje serwery
    int sock = create_udp_socket(addr, port, remote_address);
    SIMPL_CMD packet;
    set_cmd(packet.cmd, "HELLO");
    int length = prepare_to_send(packet, packet.cmd, filename);
    if (sendto(sock, &packet, length, 0, (struct sockaddr *) &remote_address, sizeof remote_address) != length)
        syserr("sendto");

    CMPLX_CMD p3;
    struct sockaddr_in server{};
    socklen_t len = sizeof(struct sockaddr_in);
    auto start = high_resolution_clock::now();
    while (true) {
        //TODO do poprawy
        auto end = high_resolution_clock::now();
        duration<double, std::milli> elapsed = end - start;
        if (elapsed.count() >= timeout * 1000) {
            break;
        }
        int x = recvfrom(sock, &p3, sizeof p3, 0, (struct sockaddr *) &server, &len);
        if (x < 0) {
            continue;
        }
        p3.data[x - 26] = '\0';
        //check if we got packet we are expecting
        string ip = inet_ntoa(server.sin_addr);
        if (strncmp(p3.cmd, "GOOD_DAY", 10) == 0 && be64toh(p3.cmd_seq) == cmd_seq) {
            uint64_t freeSpace =  be64toh(p3.param);
            servers.push_back(std::make_pair(freeSpace, ip));
            cout << "found \n";
        }
    }
    promise_message result;
    //TODO teraz szukamy serwer i wysyłamy zapytania, oczekując na odpowiedź "NO_WAY" albo "CAN_ADD"
    std::sort(servers.begin(), servers.end(), std::greater<>());
    for(auto &serv: servers){
        if(serv.first < fs::file_size(file)){
            result = {false, ""};
            break;
        }
        result = send_add_and_wait_for_answer(sock, file.string(), remote_address);
        if(result.isSuccessful){
            break;
        }
    }
    return result;
}

void set_options(int argc, const char * argv[], string &addr, uint16_t &port, int &timeout){
    try {
        options_description desc{"Options"};
        desc.add_options()
            ("MCAST_ADDR,g", value<string>(&addr)->required(), "adress")
            ("CMD_PORT,p", value<uint16_t>(&port)->required(), "port")
            ("OUT_FLDR,o", value<string>(&savedir)->required(), "out folder")
            ("TIMEOUT,t", value<int>(&timeout)->default_value(5)->notifier(on_timeout), "timeout");
        variables_map vm;
        store(parse_command_line(argc, argv, desc), vm);
        notify(vm);
    }
    catch (const error &ex) {
        std::cerr << ex.what() << '\n';
        exit(0);
    }
    catch (std::invalid_argument &ex) {
        std::cerr << ex.what() << '\n';
        exit(0);
    }
    //TODO debug stuff
    cout << addr << "\n";
    cout << " port " << port << "\n";
    cout << "out: " << savedir << "\n";
    cout << "timeout" << timeout << "\n";
    cout << "ready to read: \n";
}

void init_cmd_seq(){
    std::random_device dev;
    std::mt19937_64 rng(dev());
    std::uniform_int_distribution<std::mt19937_64::result_type> dist(0, 18446744073709551615ULL);
    cmd_seq = dist(rng);
}

int main(int argc, const char *argv[]) {
    init_cmd_seq();
    cout << sizeof(CMPLX_CMD) << " "<< sizeof(SIMPL_CMD) << "\n";
    struct timeval s_timeout{};
    s_timeout.tv_usec = 10;
    s_timeout.tv_sec = 0;
    uint16_t port;
    string addr, savedir;
    set_options(argc, argv, addr, port, timeout);

    string command, param, line;
    struct sockaddr_in localSock{};
    struct sockaddr_in remote_address{};
    int sock = socket(PF_INET, SOCK_DGRAM, 0); // creating IPv4 UDP socket
    if (sock < 0) {
        syserr("socket");
    }

    //set timeout
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *) &s_timeout, sizeof(s_timeout)) < 0) {
        error give_me_a_name("setsockopt rcvtimeout\n");
        close(sock);
        exit(1);
    }
    //activate bcast
    int optval = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (void *) &optval, sizeof optval) < 0)
        syserr("setsockopt broadcast");

/* Enable SO_REUSEADDR to allow multiple instances of this */
/* application to receive copies of the multicast datagrams. */
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *) &reuse, sizeof(reuse)) < 0) {
        syserr("secksockopt: reuse");
    }

    //binding port number with ip address
    memset((char *) &localSock, 0, sizeof(localSock));
    localSock.sin_addr.s_addr = htonl(INADDR_ANY); // listening on all interfaces
    localSock.sin_port = htons(0); // listening on port PORT_NUM
    localSock.sin_family = AF_INET; // IPv4

    if (bind(sock, (struct sockaddr *) &localSock, sizeof(localSock)) < 0) {
        syserr("bind");
    }

    /* ustawienie adresu i portu odbiorcy */
    remote_address.sin_family = AF_INET;
    remote_address.sin_port = htons(port);
    if (inet_aton(addr.c_str(), &remote_address.sin_addr) == 0)
        syserr("inet_aton");


    while (getline(cin, line)) {
        string a, b;
        std::istringstream iss(line);
        if (!(iss >> a)) {
            continue;
        } else {
            to_lower(a);
        }
        if (!(iss >> b)) {
            //only discover, exit, search are OK
            if (a == "discover") {
                perform_discover(sock, remote_address);
            } else if (a == "exit") {
                exit(0);
            } else if (a == "search") {
                string s = b;
                perform_search(s, sock, remote_address);
            } else {
                cerr << a << " is unrecognized command or requires parameter\n";
            }
        } else {
            string c;
            while (iss >> c) {
                b += " " + c;
            }
            if (a == "fetch") {
                perform_fetch(b, remote_address);
            } else if (a == "search") {
                perform_search(b, sock, remote_address);
            } else if (a == "upload") {
               // perform_upload(b, addr, port, remote_address);
                auto t1 = std::async(std::launch::async, perform_upload, std::ref(b), std::ref(addr), port, std::ref(remote_address));
               auto res =  t1.get();
               cout << res.message;
            } else if(a == "remove") {
              perform_remove(b, sock, remote_address);
            } else {
                cerr << a << " is unrecognized command or requires to be without parameters\n";
            }
        }
    }

    return 0;
}