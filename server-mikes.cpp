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
const size_t buffer_size = 64;



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
	char buffer[buffer_size+1];
};

// Prototypes

static int setup_server_socket( short port );
static bool set_socket_nonblocking( int fd );
static bool process_client_send( ConnectionData& cd );
static bool process_client_recv( ConnectionData& cd , fd_set& master_fds );

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
    	
	fd_set master_fds; 
    	fd_set temp_fds;  
    	int fd_count;

	FD_SET(listenfd, &master_fds); // add listening socket to master

   	// keep track of the highest file descriptor
    	fd_count = listenfd; // so far we only have one.

	int i;

	while (1){
		temp_fds = master_fds; // Copy master fd set to a temporary one
		
		if(select(fd_count+1, &temp_fds, NULL, NULL, NULL) == -1){
    			perror("select() error");
    			exit(1);
		}
		printf("select() ok\n");

		sockaddr_in clientAddr;
		socklen_t addrSize = sizeof(clientAddr);

		/*run through the connections, see if new incoming, or if anyone is sending data*/
		for(i = 0; i <= fd_count; i++ ){
    			if(FD_ISSET(i, &temp_fds)) { /* we got one... */
				if(i == listenfd){ /* handle new connections */
					// accept a single incoming connection
					int client_fd = accept( listenfd, (sockaddr*)&clientAddr, &addrSize );
					if( client_fd == -1 ){
						perror( "accept() failed" );
						continue; // We go on and try with other clients if any
					}else{
						FD_SET(client_fd,&master_fds);

						if(client_fd > fd_count){ /* Keep track of which one is the highest */
				   			fd_count = client_fd;
						}

						printf("Accepted new connection from %s, socket %d\n", inet_ntoa(clientAddr.sin_addr), client_fd);

					}
				}else{ // Another socket (not listenfd)
					// initialize connection data
					ConnectionData connData;
					memset( &connData, 0, sizeof(connData) );

					connData.sock = i;
					connData.state = eConnStateReceiving;
					if (FD_ISSET(i,&master_fds)){
						process_client_recv( connData , master_fds ); // Receive the data
					}
						
			
					if (FD_ISSET(i,&master_fds)){	
						process_client_send( connData ); // We have received data, now send it back
					}

					

				} // End if (i == list...
			} // End if (FD_ISSET....
		} // End for


	} // end while
    	

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

static bool process_client_send( ConnectionData& cd ){

	// send as much data as possible from buffer
	ssize_t ret = send( cd.sock, 
		cd.buffer+cd.bufferOffset, 
		cd.bufferSize-cd.bufferOffset,
		MSG_NOSIGNAL // suppress SIGPIPE signals, generate EPIPE instead
	);

	if( ret == -1 ){
		printf( "  socket %d - error on send: '%s'\n", cd.sock, strerror(errno) );
		fflush( stdout );
		return false;
	}

	// update buffer data
	cd.bufferOffset += ret;

	// did we finish sending all data
	if( cd.bufferOffset == cd.bufferSize ){
		// if so, transition to receiving state again
		cd.bufferSize = 0;
		cd.bufferOffset = 0;
		cd.state = eConnStateReceiving;
	}

	return true;
}

static bool process_client_recv( ConnectionData& cd, fd_set& master_fds ){
	assert( cd.state == eConnStateReceiving );
	// receive from socket
	ssize_t ret = recv( cd.sock, cd.buffer, buffer_size, 0 );

	if( 0 == ret ){
		printf( "  socket %d - orderly shutdown\n", cd.sock );
		fflush( stdout );
		close(cd.sock);
		FD_CLR(cd.sock,&master_fds);

		return false;
	}

	if( -1 == ret ){
		printf( "  socket %d - error on receive: '%s'\n", cd.sock, strerror(errno) );
		fflush( stdout );

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