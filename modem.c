/*
 * Copyright 2022 S. V. Nickolas.
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

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/times.h>
#include <sys/types.h>
#include <netdb.h>

/*
 * Version of modem.c to interface with DJ Sures' emulator.
 * 
 * This is intended as a temporary solution until the emulator is 
 * reimplemented within Marduk itself.
 */

static int status;
static int mosock;

uint8_t modem_read (void)
{
 struct timeval timeval;
 fd_set fds;
 int e;
 uint8_t t;
 
 if (!status) return 0;
 
 timeval.tv_sec=0;
 timeval.tv_usec=100;
 
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
 
 recv(mosock, &t, 1, 0);
 return t;
}

void modem_write (uint8_t data)
{
 if (status) send(mosock, &data, 1, 0);
 return;
}

int modem_init (void)
{
 int e;
 
 status=0;
 
 struct addrinfo hints, *result;

 memset(&hints,0,sizeof(struct addrinfo));
 hints.ai_family=AF_INET;
 hints.ai_socktype=SOCK_STREAM;
 hints.ai_flags=(AI_NUMERICHOST | AI_NUMERICSERV);
 hints.ai_protocol=IPPROTO_TCP;
 e=getaddrinfo("127.0.0.1", "5816", &hints, &result);
 if (e)
 {
  fprintf (stderr, "Modem init failed: %s\n", gai_strerror(e));
  return -1;
 }
 
 mosock=socket(result->ai_family, result->ai_socktype, result->ai_protocol);
 if (mosock<0)
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
 close(mosock);
}
