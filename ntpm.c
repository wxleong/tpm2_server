
#include <errno.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "ftdi_spi_tpm.h"
#include "tpm2lib_wrapper.h"

#define SOCKET_ERROR -1

static struct tpm_driver {
  /* Parameter(s) may be irrelevant for some drivers. */
  int (*drv_init)(uint32_t freq, int debug);
  size_t (*drv_process)(uint8_t *message, size_t message_size);
  void (*drv_stop)(void);  /* Could be NULL. */
} drivers[] = {
  {FtdiSpiInit, FtdiSendCommandAndWait, FtdiStop},
  {Tpm2LibInit, Tpm2LibProcess}
};

const static char *help_msg =
  " Command line options:\n"
  "  -d[d]     - enable debug tracing (more d's - more debug)\n"
  "  -f NUM    - ftdi clock frequency\n"
  "  -p NUM    - port number\n"
  "  -s        - use simulator instead of the USB interface\n";

int main( int argc, char *argv[] )
{
  // create and open network socket
  struct sockaddr_in serv_addr;
  int sockfd;
  int opt;
  uint16_t port = 9883; /* default port */
  uint32_t freq = 1000 * 1000; /* Default frequency 1 MHz */
  int driver_index = 0;
  int debug_level = 0;
  int c;

  debug_level = 0;

  while ((c = getopt(argc, argv, "df:p:s")) != -1) {
    switch (c) {
    case 'd':
      debug_level++;
      break;
    case 'f':
      freq = atoi(optarg);
      break;
    case 'p':
      port = atoi(optarg);
      break;
    case 's':
      driver_index = 1;
      break;
    case '?':
      if ((optopt == 'p') || (optopt == 'f')) {
        fprintf (stderr, "Option -%c requires an argument.\n", optopt);
        return -1;
      }
      if (!isprint (optopt))
        fprintf (stderr, "Unknown option character \\x%x'.\n", optopt);
    default:
      fprintf(stderr, "%s", help_msg);
      return -2;
    }
  }

  sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sockfd == SOCKET_ERROR) {
    fprintf(stderr, "failed to create socket, error %s\n", strerror(errno));
    return -1;
  }

  memset( (char *) &serv_addr, 0, sizeof( serv_addr ) );

  printf("Opening socket on port %d\n", port);
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(port);

  opt = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

  if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) == SOCKET_ERROR) {
    fprintf(stderr, "failed to bind, error %s\n", strerror(errno));
    return -1;
  }

  if (listen(sockfd, 1) == SOCKET_ERROR) {
    fprintf(stderr, "Error %s on listen()\n", strerror(errno));
    return -1;
  }

  if (!drivers[driver_index].drv_init(freq, debug_level)) {
    fprintf(stderr, "Failed to initialize FTDI SPI\n");
    return -1;
  }

  while (true) {
    // main loop
    uint8_t buffer[4096 + 4];
    int len;
    int newsockfd;

    printf("\nWaiting for new connection...");
    fflush(stdout);
    newsockfd = accept(sockfd, 0, 0);
    if (newsockfd == SOCKET_ERROR) {
      fprintf(stderr, "ERROR on accept (%s)\n", strerror(errno));
      shutdown(sockfd, SHUT_RDWR);
      return -1;
    }
    printf("connected.\n");

    do {
      int written = 0;

      len = recv(newsockfd, (char*) buffer + 4, sizeof(buffer) - 4, 0);

      if ( len == SOCKET_ERROR ) {
        fprintf(stderr, "ERROR reading from socket %s\n", strerror(errno));
        break;
      }
      if (!len) {
        /* Socket reset on the client side. */
        continue;
      }

      // write command to TPM and read result
      len = drivers[driver_index].drv_process(buffer + 4, len);

      {
        uint32_t i = 1;
        if (i & 0xF) {
          /* big-endian */
          buffer[0] = (uint8_t)(len >> 24);
          buffer[1] = (uint8_t)(len >> 16);
          buffer[2] = (uint8_t)(len >> 8);
          buffer[3] = (uint8_t)(len);
        } else {
          /* little-endian */
          buffer[3] = (uint8_t)(len >> 24);
          buffer[2] = (uint8_t)(len >> 16);
          buffer[1] = (uint8_t)(len >> 8);
          buffer[0] = (uint8_t)(len);
        }
      }

      len += 4;

      // write result to network
      while (written != len) {
       int count = send(newsockfd, buffer + written, len - written, 0);
       if (count == SOCKET_ERROR )
         fprintf(stderr, "ERROR writing to socket (%s)\n", strerror(errno));
       written += count;
      }
    } while ( len > 0 );
    shutdown(newsockfd, SHUT_RDWR);

    // clean up TPM context, TPM2_FlushContext(transient objects, loaded sessions, saved sessions)
    {
      int i = 0;
      int mode = 0;
      int handleCount = 0;
      uint8_t handles[128];
      uint8_t cmdGetCap[] = {
        0x80, 0x01,               // TPM_ST_NO_SESSIONS
        0x00, 0x00, 0x00, 0x16,   // commandSize
        0x00, 0x00, 0x01, 0x7A,   // TPM_CC_GetCapability
        0x00, 0x00, 0x00, 0x01,   // TPM_CAP_HANDLES
        0x80, 0x00, 0x00, 0x00,   // TRANSIENT_FIRST
        0x00, 0x00, 0x00, 0x10    // propertyCount: 16
      };
      uint8_t cmdFlushContext[] = {
        0x80, 0x01,               // TPM_ST_NO_SESSIONS
        0x00, 0x00, 0x00, 0x0E,   // commandSize
        0x00, 0x00, 0x01, 0x65,   // TPM_CC_FlushContext
        0x00, 0x00, 0x00, 0x00    // TPMI_DH_CONTEXT
      };

      //for (mode = 0; mode < 3; mode++)
      for (mode = 0; mode < 1; mode++)
      {
        switch (mode)
        {
          case 1:
            // LOADED_SESSION_FIRST
            cmdGetCap[14] = 0x02;
            break;
          case 2:
            // ACTIVE_SESSION_FIRST
            cmdGetCap[14] = 0x03;
            break;
          default:
            // TRANSIENT_FIRST
            break;
        }

        memcpy(buffer, cmdGetCap, sizeof(cmdGetCap));
        len = drivers[driver_index].drv_process(buffer, sizeof(cmdGetCap));
        memcpy(handles, buffer + 19, sizeof(handles));
        handleCount = (int) buffer[18];

        while (handleCount--)
        {
          memcpy(buffer, cmdFlushContext, sizeof(cmdFlushContext));
          memcpy(buffer + 10 + i, handles + i, 4);
          i += 4;

          len = drivers[driver_index].drv_process(buffer, sizeof(cmdFlushContext));
        }
      }
    }
  }

  shutdown(sockfd, SHUT_RDWR);
  if (drivers[driver_index].drv_stop)
    drivers[driver_index].drv_stop();

  return 0;
}
