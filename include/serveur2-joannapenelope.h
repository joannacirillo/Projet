int acceptUDP(int server_port, int serverSocket, struct sockaddr_in client_addr, socklen_t sockaddr_length);
void synack(int serverSocket, struct sockaddr_in client_addr, socklen_t sockaddr_length, int server_port2);
