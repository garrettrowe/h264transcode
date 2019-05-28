// Compile: gcc -Wall zmodopipe.c -o zmodopipe

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <stdarg.h>
#include <sys/stat.h>



#define MAX_CHANNELS 8		// maximum channels to support

struct globalArgs_t {
    bool verbose;			// -v duh
    char *pipeName;			// -n name to use for filename (ch # will be appended)
    bool channel[MAX_CHANNELS];     // -c (support up to 16)
    char *hostname;			// -s hostname to connect to
    unsigned short port;		// -p port number
    int timer;			// -t alarm timer
} globalArgs = {0};

extern char *optarg;
const char *optString = "vn:c:p:s:m:u:a:t:h?";
int g_childPids[MAX_CHANNELS] = {0};
int g_cleanUp = false;
char g_errBuf[256];	// This will contain the error message for perror calls
int g_processCh = -1;	// Channel this process will be in charge of (-1 means parent)

void sigHandler(int sig);
void display_usage(char *name);
int printMessage(bool verbose, const char *message, ...);
int ConnectViaMedia(int sockFd, int channel);

void printBuffer(char *pbuf, size_t len)
{
    int n;
    
    printMessage(false, "Length: %lu\n", (unsigned long)len);
    for(n=0; n < len ; n++)
    {
        printf( "%02x",(unsigned char)(pbuf[n]));
        
        if( ((n + 1) % 8) == 0 )
            printf(" ");		// Make hex string slightly more readable
    }
    
    printf("\n");
}

int main(int argc, char**argv)
{
    char pipename[256];
    char ffCmd[2048];
    struct addrinfo hints, *server;
    struct sockaddr_in serverAddr;
    int retval = 0;
    char recvBuf[2048];
    struct sigaction sapipe, oldsapipe, saterm, oldsaterm, saint, oldsaint, sahup, oldsahup;
    char opt;
    int loopIdx;
    int outPipe = -1;
#ifdef DOMAIN_SOCKETS
    struct sockaddr_un addr;
#endif
    int sockFd = -1;
    struct timeval tv;
#ifdef NON_BLOCK_READ
    struct timeval tv, tv_sel;
#endif
    struct linger lngr;
    int status = 0;
    int pid = 0;
    
    lngr.l_onoff = false;
    lngr.l_linger = 0;
    
    // Process arguments
    // Clear and set defaults
    memset(&globalArgs, 0, sizeof(globalArgs));
    
    globalArgs.hostname =
    globalArgs.pipeName = "zmodo";
    
    // Read command-line
    while( ((opt = getopt(argc, argv, optString)) != -1) && (opt != 255))
    {
        switch( opt )
        {
            case 'v':
                globalArgs.verbose = true;
                break;
            case 'c':
                globalArgs.channel[atoi(optarg) - 1] = true;
                break;
            case 'n':
                globalArgs.pipeName = optarg;
                break;
            case 's':
                globalArgs.hostname = optarg;
                break;
            case 'p':
                globalArgs.port = atoi(optarg);
                break;
            case 't':
                globalArgs.timer = atoi(optarg);
                break;
            case 'h':
                // Fall through
            case '?':
                // Fall through
            default:
                display_usage(argv[0]);
                return 0;
        }
    }
    
    // Set up default values based on provided values (if any)
    if( !globalArgs.port )
    {
        globalArgs.port = 9000;
    }
    
    memset(&saint, 0, sizeof(saint));
    memset(&saterm, 0, sizeof(saterm));
    memset(&sahup, 0, sizeof(sahup));
    
    // Ignore SIGPIPE
    sapipe.sa_handler = sigHandler;
    sigaction(SIGPIPE, &sapipe, &oldsapipe);
    
    // Handle SIGTERM & SIGING
    saterm.sa_handler = sigHandler;
    sigaction(SIGTERM, &saterm, &oldsaterm);
    saint.sa_handler = sigHandler;
    sigaction(SIGINT, &saint, &oldsaint);
    
    signal( SIGUSR1, SIG_IGN );		// Ignore SIGUSR1 in parent process
    
    // SIGUSR2 is used to reset the pipe and connection
    sahup.sa_handler = sigHandler;
    sigaction(SIGUSR2, &sahup, &oldsahup);
    
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_protocol = IPPROTO_TCP;
    retval = getaddrinfo(globalArgs.hostname, NULL, &hints, &server);
    if( retval != 0 )
    {
        printMessage(false, "getaddrinfo failed: %s\n", gai_strerror(retval));
        return 1;
    }
    
    serverAddr.sin_addr = ((struct sockaddr_in*)server->ai_addr)->sin_addr;
    serverAddr.sin_port = htons(globalArgs.port);
    
    
    do
    {
        if( pid )
        {
            printMessage(true, "Child %i returned: %i\n", pid, status);
            
            if( g_cleanUp == 2 )
                g_cleanUp = false;	// Ignore SIGHUP
        }
        
        // Create a fork for each camera channel to stream
        for( loopIdx=0;loopIdx<MAX_CHANNELS;loopIdx++ )
        {
            //static bool hitFirst = false;
            if( globalArgs.channel[loopIdx] == true )
            {
                // Always fork if we're starting up, or if the pid of the dead child process matches
                if( pid == 0 || g_childPids[loopIdx] == pid )
                    g_childPids[loopIdx] = fork();
                
                // Child Process
                if( g_childPids[loopIdx] == 0 )
                {
                    // SIGUSR1 is used to reset the pipe and connection
                    sahup.sa_handler = sigHandler;
                    sigaction(SIGUSR1, &sahup, &oldsahup);
                    
                    memset(g_childPids, 0, sizeof(g_childPids));
                    g_processCh = loopIdx;
                    break;
                }
                // Error
                else if( g_childPids[loopIdx] == -1 )
                {
                    printMessage(false, "fork failed\n");
                    return 1;
                }
            }
        }
    }
    while( (pid = wait(&status)) > 0  && g_cleanUp != true );
    
    if( g_processCh != -1 )
    {
        // At this point, g_processCh contains the camera number to use
        sprintf(pipename, "/tmp/%s%i", globalArgs.pipeName, g_processCh);
        
        tv.tv_sec = 5;		// Wait 5 seconds for socket data
        tv.tv_usec = 0;
        
        FILE *ffmpegP = NULL;
        
        sprintf(ffCmd, "ffmpeg -y -f h264 -framerate 1 -i /tmp/zmodo%i -s 390x220  -r 1/2 -update 1 -f image2  /var/www/html/%i.jpg &", g_processCh, g_processCh+1);
        
        
#ifndef DOMAIN_SOCKETS
        
        retval = mkfifo(pipename, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
        
        if( retval != 0 )
        {
            sprintf(g_errBuf, "Ch %i: Failed to create pipe\n", g_processCh+1);
            perror(g_errBuf);
        }
        
#endif	
        
        while( !g_cleanUp )
        {
            int flag = true;
#ifdef NON_BLOCK_READ
            fd_set readfds;
#endif
            // Initialize the socket and connect
            sockFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            
            if( setsockopt(sockFd, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv)))
            {
                sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Failed to set socket timeout\n");
                perror(g_errBuf);
            }
            
            if( setsockopt(sockFd, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag)))
            {
                sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Failed to set TCP_NODELAY\n");
                perror(g_errBuf);
            }
            if( setsockopt(sockFd, SOL_SOCKET, SO_LINGER, (char*)&lngr, sizeof(lngr)))
            {
                sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Failed to set SO_LINGER\n");
                perror(g_errBuf);
            }
            
            retval = connect(sockFd, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
            
            if( globalArgs.verbose )
                printMessage(true, "Connect result: %i\n", retval);
            
            if( retval == -1 && errno != EINPROGRESS )
            {
                int sleeptime = 10;
                
                if( globalArgs.verbose )
                {
                    sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Failed to connect\n");
                    perror(g_errBuf);
                    printMessage(true, "Waiting %i seconds.\n", sleeptime);
                }
                close(sockFd);
                sockFd = -1;
                sleep(sleeptime);
                continue;
            }
            
            if( retval == -1 && errno != EINPROGRESS )
            {
                
                sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Failed to connect\n");
                perror(g_errBuf);
                return 1;
            }
            
            retval = 0;
            
            if( ConnectViaMedia(sockFd, g_processCh) != 0 )
            {
                printMessage(true, "Login failed, bailing.\nDid you select the right model?\n");
                close(sockFd);
                sockFd = -1;
                return 1;
            }
#ifdef NON_BLOCK_READ
            if( fcntl(sockFd, F_SETFL, O_NONBLOCK) == -1 )	// non-blocking sockets
            {
                sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Failed to set O_NONBLOCK\n");
                perror(g_errBuf);
            }
            FD_ZERO(&readfds);
            FD_SET(sockFd, &readfds	);
#endif
            // the stream sometimes goes grey,
            // this alarm should periodically reset the stream
            if(globalArgs.timer)
                alarm(globalArgs.timer);
            
            // Now we are connected and awaiting stream
            do
            {
                int read = 0;
#ifdef NON_BLOCK_READ
                tv_sel.tvsec = 0;
                tv_sel.tv_usec = 10000;	// Wait 10 ms
                
                if( select( sockFd+1, &readfds, NULL, NULL, &tv_sel) == -1)
                {
                    sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Select failed\n");
                    perror(g_errBuf);
                    
                    close(sockFd);
                    sockFd = -1;
                    break;			// Connection may have died, reset
                }
                
                if (!FD_ISSET(sockFd, &readfds))
                {
                    if( globalArgs.verbose )
                        printMessage(true, "Read would block\n");
                    continue;		// Not ready yet
                }
#endif
                
                read  = recv(sockFd, recvBuf, sizeof(recvBuf), 0);
                
                // Server disconnected, close the socket so we can try to reconnect
                if( read <= 0 )
                {
#ifdef NON_BLOCK_READ
                    if( errno == EAGAIN || errno == EWOULDBLOCK )
                    {
                        if( globalArgs.verbose )
                            printMessage(true, "Read would block\n");
                        continue;
                    }
#endif
                    if( globalArgs.verbose )
                        printMessage(true, "Socket closed. Receive result: %i\n", read);
                    
                    pclose (ffmpegP);
                    ffmpegP = NULL;
                    close(sockFd);
                    sockFd = -1;
                    break;
                }
                
                struct stat st;
                char fileloc[256];
                sprintf(fileloc, "/var/www/html/%i.jpg", g_processCh+1);
                stat(fileloc, &st);
                int fsize1 = st.st_size;
                
                if( fsize1 == 1957 ){
                    printMessage(true, "Stream %i ouput pic size %i, reconnecting\n",g_processCh+1,fsize1);
                    remove(fileloc);
                    pclose (ffmpegP);
                    ffmpegP = NULL;
                    close(sockFd);
                    sockFd = -1;
                    break;
                }
                             
                if( globalArgs.verbose )
                {
                    printf(".");
                    fflush(stdout);
                }
                
#ifdef DOMAIN_SOCKETS
                if( outPipe == -1 )
                {
                    outPipe = socket(AF_UNIX, SOCK_STREAM, 0);
                    if( outPipe == -1 )
                        perror("Error creating socket\n");
                    
                    memset(&addr, 0, sizeof(addr));
                    addr.sun_family = AF_UNIX;
                    strncpy(addr.sun_path, pipename, sizeof(addr.sun_path) - 1);
                    
                    if( bind(outPipe, (struct sockaddr*)&addr, sizeof(addr)) == -1 )
                        perror("Error binding socket\n");
                    
                    if(ffmpegP == NULL){
                        printMessage(true, "opening ffmpeg\n");
                        ffmpegP = popen (ffCmd, "r");
                        if (!ffmpegP)
                        {
                            printMessage(true, "failed to open ffmpeg\n");
                        }
                    }
                }
                
#else
                // Open the pipe if it wasn't previously opened
                
                if( outPipe == -1 ){
                    outPipe = open(pipename, O_WRONLY | O_NONBLOCK);
                    
                    if(ffmpegP == NULL){
                        printMessage(true, "opening ffmpeg\n");
                        ffmpegP = popen (ffCmd, "r");
                        if (!ffmpegP)
                        {
                            printMessage(true, "failed to open ffmpeg\n");
                        }
                    }
                }
                
#endif
                
                // send to pipe
                if( outPipe != -1 )
                {
                    if( (retval = write(outPipe, recvBuf, read)) == -1)
                    {
                        if( errno == EAGAIN || errno == EWOULDBLOCK )
                        {
                            if( globalArgs.verbose )
                                printMessage(true, "\nCh %i: %s", g_processCh+1, "Reader isn't reading fast enough, discarding data. Not enough processing power?\n");
                            
                            // Right now we discard data, should be a way to buffer maybe?
                            continue;
                        }
                        // reader closed the pipe, wait for it to be opened again.
                        else if( globalArgs.verbose )
                        {
                            sprintf(g_errBuf, "Ch %i: %s", g_processCh+1, "Pipe closed\n");
                            perror(g_errBuf);
                        }
                        
                        printMessage(true, "Closing ffmpeg\n");
                        pclose (ffmpegP);
                        ffmpegP = NULL;
                        
#ifndef DOMAIN_SOCKETS
                        
                        close(outPipe);
                        outPipe = -1;
                        
#endif
                        close(sockFd);
                        sockFd = -1;
                        continue;
                    }
                    else
                    {
                        if( globalArgs.verbose )
                        {
                            printf("\b \b");
                            fflush(stdout);
                        }
                    }
                }
            }
            while( sockFd != -1 && !g_cleanUp );
            
            // If we receive a SIGUSR1, close and reset everything
            // Then start the loop over again.
            if( g_cleanUp >= 2 )
            {
                g_cleanUp = false;
                
                if( sockFd != -1 )
                    close(sockFd);
                
                sockFd = -1;
                pclose (ffmpegP);
                ffmpegP = NULL;
                
                if( g_cleanUp != 3 )
                {
                    if( outPipe != -1 ){
                        // pclose (ffmpegP);
                        close(outPipe);
                    }
                    outPipe = -1;
                    //ffmpegP = NULL;
                }
            }
        }
        if( globalArgs.verbose )
            printMessage(true, "Exiting loop: %i\n", g_cleanUp);
        // Received signal to exit, cleanup
        printMessage(true, "closing ffmpeg\n");
        
        pclose (ffmpegP);
        close(outPipe);
        outPipe = -1;
        ffmpegP = NULL;
        close(sockFd);
        unlink(pipename);
    }
    
    // Restore old signal handler
    sigaction(SIGPIPE, &oldsapipe, NULL);
    sigaction(SIGTERM, &oldsaterm, NULL);
    sigaction(SIGINT, &oldsaint, NULL);
    sigaction(SIGUSR1, &oldsahup, NULL);
    freeaddrinfo(server);
    
    // Kill all children (if any)
    for( loopIdx=0;loopIdx<MAX_CHANNELS;loopIdx++ )
    {
        if( globalArgs.channel[loopIdx] > 0 )
            kill( globalArgs.channel[loopIdx], SIGTERM );
    }
    return 0;
}

int isH264iFrame(char paket[])
{
    int RTPHeaderBytes = 0;
    
    int fragment_type = paket[RTPHeaderBytes + 0] & 0x1F;
    int nal_type = paket[RTPHeaderBytes + 1] & 0x1F;
    int start_bit = paket[RTPHeaderBytes + 1] & 0x80;
    
    if (((fragment_type == 28 || fragment_type == 29) && nal_type == 5 && start_bit == 128) || fragment_type == 5)
    {
        return 1;
    }
    
    return 0;
}

void display_usage(char *name)
{
    printf("Usage: %s [options]\n\n", name);
    printf("Where [options] is one of:\n\n"
           "    -s <string>\tIP to connect to\n"
           "    -t <int>\tSend a timer interrupt every x seconds.\n"
           "    -p <int>\tPort number to connect to\n"
           "    -c <int>\tChannels to stream (can be specified multiple times)\n"
           "    -n <string>\tBase filename of pipe (ch# will be appended)\n"
           "    -v\t\tVerbose output\n"
           "    -u <string>\tUsername\n"
           "    -a <string>\tPassword\n"
           "\n");
}

void sigHandler(int sig)
{
    printMessage(true, "Received signal: %i\n", sig);
    
    switch( sig )
    {
        case SIGTERM:
        case SIGINT:
            // Kill the main loop
            g_cleanUp = true;
            break;
        case SIGUSR1:
        case SIGALRM:
        case SIGPIPE:
            g_cleanUp = 2;
            break;
        case SIGUSR2:
            g_cleanUp = 3;
            break;
    }
}

int printMessage(bool verbose, const char *message, ...)
{
    char msgBuf[2048];
    int ret=0;
    va_list argptr;
    va_start(argptr, message);
    
    if( g_processCh == -1 )
        sprintf(msgBuf, "Main: %s", message);
    else
        sprintf(msgBuf, "Ch %i: %s", g_processCh, message);
    
    if( !( verbose && !globalArgs.verbose) )
        ret = vprintf(msgBuf, argptr);
    
    va_end(argptr);
    return ret;
}


int ConnectViaMedia(int sockFd, int channel)
{
    int retval;
    char recvBuf[1500];
    char aloginBuf[] = { /* Packet 182 */
        0x31, 0x31, 0x31, 0x31, 0x88, 0x00, 0x00, 0x00,
        0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x61, 0x64, 0x6d, 0x69, 0x6e, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x6b, 0x61, 0x74, 0x68,
        0x72, 0x79, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xcd, 0xb8, 0x12, 0x7a,
        0x3a, 0x76, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00  };
    
    retval = send(sockFd, (char*)(&aloginBuf), sizeof(aloginBuf), 0);
    
    printMessage(true, "Ch %i: Send result: %i\n", channel+1, retval);
    
    char bloginBuf[]  = { /* Packet 186 */
        0x31, 0x31, 0x31, 0x31, 0x28, 0x00, 0x00, 0x00,
        0x03, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
        0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x03, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    
    retval = send(sockFd, (char*)(&bloginBuf), sizeof(bloginBuf), 0);

    retval = recv(sockFd, &recvBuf, sizeof(recvBuf), 0);
    retval = recv(sockFd, &recvBuf, sizeof(recvBuf), 0);
    
    if (channel ==0){
        printMessage(true, "logging on 1\n");
        char cloginBuf[] =  {
            0x31, 0x31, 0x31, 0x31, 0x34, 0x00, 0x00, 0x00,
            0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00  };
        retval = send(sockFd, (char*)(&cloginBuf), sizeof(cloginBuf), 0);
    }else if (channel ==1){
        printMessage(true, "Logging on 2\n");
        char cloginBuf[] =  {
            0x31, 0x31, 0x31, 0x31, 0x34, 0x00, 0x00, 0x00,
            0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00 };
        retval = send(sockFd, (char*)(&cloginBuf), sizeof(cloginBuf), 0);
    }
    else if (channel ==2){
        printMessage(true, "Logging on 3\n");
        char cloginBuf[] =  {
            0x31, 0x31, 0x31, 0x31, 0x34, 0x00, 0x00, 0x00,
            0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00 };
        retval = send(sockFd, (char*)(&cloginBuf), sizeof(cloginBuf), 0);
    }
    return 0;
}
