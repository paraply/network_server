#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <vector>
#include <algorithm>


// Default port of the server
// May be overridden of an argument
const int server_port = 5703;

// Second parameter to listen()
const int server_backlog = 8;


// A single read from the socket may return this amount of data
// A single send may transfer this amount of data
const size_t kTransferBufferSize = 64;



/* Connection states.
 * A connection may either expect to receive data, or require data to be sent.
 */
enum EConnState{
	eConnStateReceiving,
	eConnStateSending
};

/* Per-connection data
 * In the iterative server, there is a single instance of this structure, 
 * holding data for the currently active connection. A concurrent server will
 * need an instance for each active connection.
 */
struct ConnectionData
{
    EConnState state; // state of the connection; see EConnState enum

    int sock; // file descriptor of the connections socket.

    // items related to buffering.
    size_t bufferOffset, bufferSize; 
    char buffer[kTransferBufferSize+1];
};


// Prototypes
static int setup_server_socket( short port );
static bool set_socket_nonblocking( int fd );
static bool process_client_send( ConnectionData& cd );
static bool process_client_recv( ConnectionData& cd );

int main( int arg_count, char* arguments[] ){
	int current_server_port = server_port;
	if (arg_count == 2){
		current_server_port = atoi(arguments[1]);
	}	
	
	// set up listening socket
	int listenfd = setup_server_socket( current_server_port );	
	if( listenfd == -1 ){
		return 1;
	}
    	
	std::vector<ConnectionData> connections;


    	fd_set read_fds; // set of file descriptors for reading the listening socket
	fd_set write_fds; // set of file descriptors for writing

	int fd_max; // we want to store which socket is the highest one since select() needs it
	fd_max = listenfd; // from the beginning we only have this one

	// empty the sets
	FD_ZERO(&read_fds); 
	FD_ZERO(&write_fds);
	
	FD_SET(listenfd, &read_fds); // add listening socket to reads

	while(1){

		if(select(fd_max+1, &read_fds, &write_fds, NULL, NULL) == -1){
    			perror("select() error");
    			exit(1);
		}
		printf("select() ok\n");

        	sockaddr_in clientAddr;
	        socklen_t addrSize = sizeof(clientAddr);		

		if (FD_ISSET(listenfd,&read_fds)){ // check if the listening socket is in the read_fds set. If so, a client wants to connect.
			int client_fd = accept( listenfd, (sockaddr*)&clientAddr, &addrSize );
					if( client_fd == -1 ){
						perror( "accept() failed" );
						continue; // we go on and try with other clients if any
					}else{

						ConnectionData connData; // create a connData object that we will store in connections vector
						memset( &connData, 0, sizeof(connData) );

						connData.sock = client_fd; // add the newly accepted client
						connData.state = eConnStateReceiving;

						connections.push_back(connData); // add the object to the vector

						printf("Accepted new connection from %s, socket %d\n", inet_ntoa(clientAddr.sin_addr), client_fd);

					}
		}	

	// loop through the connectionData objects and check if any wants to perform an action
        for (std::vector<ConnectionData>::iterator it = connections.begin(); it < connections.end(); it++ ){
            
		ConnectionData &connData = *it;
        bool processFurther = true;

		if (FD_ISSET(connData.sock, &read_fds)){
			processFurther = process_client_recv( connData ); // Receive the data
			FD_CLR(connData.sock, &read_fds); // Clear the socket now since it shouldn't read from this one again
		}

		if (FD_ISSET(connData.sock, &write_fds)){
			processFurther = process_client_send( connData ); // Send the data
			FD_CLR(connData.sock, &write_fds);
		}

		if (!processFurther){ // if something bad happened we will close the connection
			close(connData.sock); 
			it = connections.erase(it);
		}	
	}	

	// loop through the connectionData objects again
	// and update the sets if needed
	for (std::vector<ConnectionData>::iterator it = connections.begin(); it < connections.end(); it++ ){

		ConnectionData &connData = *it;
		fd_max = listenfd;
		if (connData.state == eConnStateReceiving){
			FD_SET(connData.sock, &read_fds);
		}

		if (connData.state == eConnStateSending){
			FD_SET(connData.sock, &write_fds);
		}

		if (connData.sock >= fd_max){
			fd_max = connData.sock;
		}

	        FD_SET(listenfd, &read_fds); // we need to add listenfd again to be able to listen for new connections		
	}

	

		
	}    	


    return 0;	
		
}

static int setup_server_socket( short port ){
	// create new socket file descriptor
	int fd = socket( AF_INET, SOCK_STREAM, 0 );
	if( fd == -1 ){
		perror( "socket() failed" );
		return -1;
	}

	// bind socket to local address
	sockaddr_in servAddr; 
	memset( &servAddr, 0, sizeof(servAddr) );

	servAddr.sin_family = AF_INET;
	servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servAddr.sin_port = htons(port);

	if( bind( fd, (const sockaddr*)&servAddr, sizeof(servAddr) ) == -1){
		perror( "bind() failed" );
		close( fd );
		return -1;
	}

	// get local address (i.e. the address we ended up being bound to)
	sockaddr_in actualAddr;
	socklen_t actualAddrLen = sizeof(actualAddr);
	memset( &actualAddr, 0, sizeof(actualAddr) );

	if( getsockname( fd, (sockaddr*)&actualAddr, &actualAddrLen ) == -1){
		perror( "getsockname() failed" );
		close( fd );
		return -1;
	}

	char actualBuff[128];
	printf( "Socket is bound to %s %d\n", 
		inet_ntop( AF_INET, &actualAddr.sin_addr, actualBuff, sizeof(actualBuff) ),
		ntohs(actualAddr.sin_port)
	);

	// and start listening for incoming connections
	if( listen( fd, server_backlog ) == -1 ){
		perror( "listen() failed" );
		close( fd );
		return -1;
	}

	// allow immediate reuse of the address (ip+port)
	int one = 1;
	if( setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int) ) == -1){
		perror( "setsockopt() failed" );
		close( fd );
		return -1;
	}

	// enable non-blocking mode
	if( !set_socket_nonblocking( fd ) ){
		close( fd );
		return -1;
	}

	return fd;
}


static bool set_socket_nonblocking( int fd ){
	int oldFlags = fcntl( fd, F_GETFL, 0 );
	if( -1 == oldFlags ){
		perror( "fcntl(F_GETFL) failed" );
		return false;
	}

	if( -1 == fcntl( fd, F_SETFL, oldFlags | O_NONBLOCK ) ){
		perror( "fcntl(F_SETFL) failed" );
		return false;
	}

	return true;
}

//--    process_client_recv()   ///{{{1///////////////////////////////////////
static bool process_client_recv( ConnectionData& cd )
{
    assert( cd.state == eConnStateReceiving );

    // receive from socket
    ssize_t ret = recv( cd.sock, cd.buffer, kTransferBufferSize, 0 );

    if( 0 == ret )
    {
#		if VERBOSE
        printf( "  socket %d - orderly shutdown\n", cd.sock );
        fflush( stdout );
#		endif

        return false;
    }

    if( -1 == ret )
    {
#		if VERBOSE
        printf( "  socket %d - error on receive: '%s'\n", cd.sock,
                strerror(errno) );
        fflush( stdout );
#		endif

        return false;
    }

    // update connection buffer
    cd.bufferSize += ret;

    // zero-terminate received data
    cd.buffer[cd.bufferSize] = '\0';

    // transition to sending state
    cd.bufferOffset = 0;
    cd.state = eConnStateSending;
    return true;
}

//--    process_client_send()   ///{{{1///////////////////////////////////////
static bool process_client_send( ConnectionData& cd )
{
    assert( cd.state == eConnStateSending );

    // send as much data as possible from buffer
    ssize_t ret = send( cd.sock, 
                        cd.buffer+cd.bufferOffset, 
                        cd.bufferSize-cd.bufferOffset,
                        MSG_NOSIGNAL // suppress SIGPIPE signals, generate EPIPE instead
        );

    if( -1 == ret )
    {
#		if VERBOSE
        printf( "  socket %d - error on send: '%s'\n", cd.sock, 
                strerror(errno) );
        fflush( stdout );
#		endif

        return false;
    }

    // update buffer data
    cd.bufferOffset += ret;

    // did we finish sending all data
    if( cd.bufferOffset == cd.bufferSize )
    {
        // if so, transition to receiving state again
        cd.bufferSize = 0;
        cd.bufferOffset = 0;
        cd.state = eConnStateReceiving;
    }

    return true;
}
