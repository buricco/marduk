/*
 * Copyright 2022, 2023 S. V. Nickolas.
 * Copyright 2023 Marcin Wołoszczuk.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following condition:  The
 * above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * This code abstracts away the few differences between BSD Sockets (used by
 * Linux, OSX, etc.), Winsock (used by Windows) and Watt-32 (used by MS-DOS).
 * 
 * From our perspective all these APIs are more or less the same except for
 * whether/how they are set up and shut down, and whether they use the regular
 * close() function to close a socket, or they need their own closesocket()
 * instead (we define closesocket to close on *x to hide this difference).
 * 
 * The MS-DOS code has not been tested successfully, and the Windows code has
 * not been tested at all.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
# include <winsock2.h>
# include <ws2tcpip.h>
#else
# include <sys/socket.h>
# include <sys/times.h>
# include <sys/time.h>
# include <sys/types.h>
# include <netinet/in.h>
# include <netdb.h>
# ifdef __MSDOS__
#  include <tcp.h>
# else
#  define closesocket close /* BSD Socket API does not distinguish */
# endif
#endif

/*
 * Version of modem.c to interface with DJ Sures' emulator.
 * 
 * This is intended as a temporary solution until the emulator is 
 * reimplemented within Marduk itself.
 */

static int status;
static int mosock;

uint8_t modem_bytes_available (void)
{
 struct timeval timeval;
 fd_set fds;
 int e;
 
 if (!status) return 0;
 
 timeval.tv_sec=0;
 timeval.tv_usec=0;
 
 FD_ZERO(&fds);
 FD_SET(mosock, &fds);
 
 e=select(mosock+1, &fds, 0, 0, &timeval);
 if (e==-1)
 {
  perror("select()");
  return 0;
 }
 
 if (!e) /* Data on 0 sockets */
 {
  return 0;
 }
 return 1;    
}

uint8_t modem_read (uint8_t *b)
{
 if (!status) return 0;
 if (modem_bytes_available()) {
   recv(mosock, b, 1, 0);
   return 1;
 }
 return 0;
}

void modem_write (uint8_t data)
{
 if (status) send(mosock, &data, 1, 0);
 return;
}

int modem_init (char *server, char *port)
{
 int e;
 struct addrinfo hints, *result;
 
#ifdef _WIN32
 WSADATA wsadata;
#endif
 
 status=0;

#ifdef __MSDOS__
 /*
  * Watt-32 requires initialization (this is also where it does DHCP).
  * 
  * It simplifies things for us a ton that Watt-32 mostly uses the BSD socket
  * APIs, because very little modification is necessary to interface with it.
  * 
  * XXX: This is currently fatal if the packet driver is missing.  How to set
  *      up to be non-fatal, and just return us an error code?
  */
 if (sock_init())
 {
  fprintf (stderr, "TCP library failed to initialize\n");
  return -1;
 }
#endif

#ifdef _WIN32
 /*
  * Winsock also requires initialization, and like Watt-32, its interface is
  * more or less the same as that of BSD.
  */
 if (WSAStartup(MAKEWORD(2,2), &wsadata))
 {
  fprintf (stderr, "TCP library failed to initialize\n");
  return -1;
 }
#endif
 
 memset(&hints,0,sizeof(struct addrinfo));
 hints.ai_family=AF_INET;
 hints.ai_socktype=SOCK_STREAM;
 /* hints.ai_flags=(AI_NUMERICHOST | AI_NUMERICSERV); */
 hints.ai_protocol=IPPROTO_TCP;
 e=getaddrinfo(server, port, &hints, &result);
 if (e)
 {
  fprintf (stderr, "Modem init failed: %s\n", gai_strerror(e));
  return -1;
 }
 
 mosock=socket(result->ai_family, result->ai_socktype, result->ai_protocol);
#ifdef _WIN32
 if (mosock==INVALID_SOCKET)
#else
 if (mosock<0)
#endif
 {
  perror ("Could not get a socket");
  freeaddrinfo(result);
  return -1;
 }
 
 e=connect(mosock, result->ai_addr, result->ai_addrlen);
 freeaddrinfo(result);
 if (e==-1)
 {
  perror ("Connection to virtual modem failed");
  return -1;
 }
 printf ("Connection to virtual modem succeeded\n");
 status=1;
 
 return 0;
}

void modem_deinit (void)
{
 if (!status) return;
 printf ("Shutting down virtual modem.\n");
 closesocket(mosock);
#ifdef _WIN32
 WSACleanup();
#endif
}
