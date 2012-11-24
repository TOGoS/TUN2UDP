/*
 * Much of the TUN/TAP setup code is based on
 * instructions and code samples at
 * http://backreference.org/2010/03/26/tuntap-interface-tutorial/
 */

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/if_tun.h>
#include <string.h>
#include <unistd.h>
// For some debug functions:
#include <stdio.h>

#include "create_tun_device.h"

static int verbosity = 10;

static int tun2udp_open_udp_sock( struct sockaddr_storage *addr, size_t addrsize ) {
  int sock;
  int z;
  int pf;
  char namebuf[1024];
  
  switch( addr->ss_family ) {
  case( AF_INET  ): pf = PF_INET;  break;
  case( AF_INET6 ): pf = PF_INET6; break;
  default:
    fprintf( stderr, "Unsupported address family #%d.\n", (int)addr->ss_family );
    return -1;
  }
  
  sock = socket( addr->ss_family, SOCK_DGRAM, 0 );
  if( sock < 0 ) {
    perror( "Failed to open UDP socket" );
    return sock;
  }
  
  z = bind( sock, (struct sockaddr *)addr, addrsize );
  if( z < 0 ) {
    perror( "Failed to bind UDP socket" );
    close( sock );
    return z;
  }
  
  return sock;
}

static int tun2udp_parse_address( const char *text, struct sockaddr_storage *addr ) {
  int i;
  int colonIdx = -1;
  char namebuf[1024];
  short port;
  
  for( i=strlen(text)-1; i>=0; --i ) {
    if( text[i] == ':' ) {
      colonIdx = i;
      break;
    }
  }
  
  if( colonIdx == -1 ) {
    fprintf( stderr, "Socket address does not contain a colon: '%s'\n", text );
    return -1;
  }
  if( colonIdx >= sizeof(namebuf) ) {
    fprintf( stderr, "Hostname is too long: '%s'\n", text );
    return -1;
  }

  if( !sscanf( text+colonIdx+1, "%hd", &port ) ) {
    fprintf( stderr, "Failed to parse port number from '%s'.\n", text+colonIdx );
    return 1;
  }
  
  if( colonIdx >= 2 && text[0] == '[' && text[colonIdx-1] == ']' ) {
    memcpy( namebuf, text+1, colonIdx-2 );
    namebuf[colonIdx-2] = 0;
    if( inet_pton( AF_INET6, namebuf, &((struct sockaddr_in6 *)addr)->sin6_addr ) == 0 ) {
      fprintf( stderr, "Unrecognised IP6 address: %s.\n", namebuf );
      return 1;
    }
    ((struct sockaddr_in6 *)addr)->sin6_family = AF_INET6;
    ((struct sockaddr_in6 *)addr)->sin6_port = htons( port );
  } else {
    memcpy( namebuf, text, colonIdx );
    namebuf[colonIdx] = 0;
    if( inet_pton( AF_INET, namebuf, &((struct sockaddr_in *)addr)->sin_addr ) == 0 ) {
      fprintf( stderr, "Unrecognised IP4 address: %s.\n", namebuf );
      return 1;
    }
    ((struct sockaddr_in *)addr)->sin_family = AF_INET;
    ((struct sockaddr_in *)addr)->sin_port = htons( port );
  }
    
  return 0;
}

static const char *usage_metatext = "Run with -? for usage information.\n";
static const char *usage_text =
"Usage: tun2udp\n"
"  -local-address <host>:<port>   -- local address to bind to\n"
"  -remote-address <host>:<port>  -- remote address to forward packets to\n"
"  {-tun|-tap}                    -- create a TUN or TAP device\n"
"  [-no-pi]                       -- don't include extra packet framing\n"
"  [-debug]                       -- be extra talkative\n"
"  [-dev <devname>]               -- create the TUN/TAP device with this name\n"
"\n"
"If <devname> is not specified, a name will be picked automatically.\n"
"\n"
"Hostnames can be IPv4 or IPv6 addresses.  IPv6 addresses must be\n"
"enclosed in square brackets, e.g.\n"
"\n"
"  [2001:470:0:76::2]:12345       -- host 2001:470:0:76::2, port 12345\n";

int main( int argc, char **argv ) {
  char devname[128];
  int z;
  int tunflags = 0;
  int tundev;
  int udpsock;
  int selectmax;
  struct sockaddr_storage udp_local_addr;
  struct sockaddr_storage udp_remote_addr;
  fd_set readfds;
  char buffer[2048];
  size_t bufread;
  int local_addr_given = 0;
  int remote_addr_given = 0;

  // TODO: if device not specified, print out unless -q given.
  devname[0] = 0;
  
  for( z=1; z<argc; ++z ) {
    if( strcmp("-q",argv[z]) == 0 ) {
      verbosity = 0;
    } else if( strcmp("-v",argv[z]) == 0 ) {
      verbosity = 20;
    } else if( strcmp("-debug",argv[z]) == 0 ) {
      verbosity = 30;
    } else if( strcmp("-tun",argv[z]) == 0 ) {
      tunflags |= IFF_TUN;
    } else if( strcmp("-tap",argv[z]) == 0 ) {
      tunflags |= IFF_TAP;
    } else if( strcmp("-no-pi",argv[z]) == 0 ) {
      tunflags |= IFF_NO_PI;
    } else if( strcmp("-pi",argv[z]) == 0 ) {
      tunflags &= ~IFF_NO_PI;
    } else if( strcmp("-tun-dev",argv[z]) == 0 || strcmp("-dev",argv[z]) == 0 ) {
      // ^ -tun-dev is allowed for combatability with older versions
      ++z;
      if( z >= argc ) {
	errx( 1, "%s needs an additional device-name argument.\n", argv[z-1] );
      }
      strncpy( devname, argv[z], sizeof(devname) );
      devname[sizeof(devname)-1] = 0;
    } else if( strcmp("-local-address",argv[z]) == 0 ) {
      ++z;
      if( z >= argc ) {
	errx( 1, "-local-address needs an additional <host>:<port> argument." );
      }
      if( tun2udp_parse_address( argv[z], &udp_local_addr ) ) return 1;
      local_addr_given = 1;
    } else if( strcmp("-remote-address",argv[z]) == 0 ) {
      ++z;
      if( z >= argc ) {
	errx( 1, "-remote-address needs an additional <host>:<port> argument." );
      }
      if( tun2udp_parse_address( argv[z], &udp_remote_addr ) ) return 1;
      remote_addr_given = 1;
    } else if( strcmp("-?",argv[z]) == 0 || strcmp("-h",argv[z]) == 0 || strcmp("-help",argv[z]) == 0 ) {
      fputs( usage_text, stdout );
      return 0;
    } else {
      warnx( "Error: Unrecgonized argument: %s.", argv[z] );
      fputs( usage_metatext, stderr );
      return 1;
    }
  }
  if( !local_addr_given ) {
    warnx( "Error: No -local-address given.\n" );
    fputs( usage_metatext, stderr );
    return 1;
  }
  if( !remote_addr_given ) {
    warnx( "Error: No -remote-address given.\n" );
    fputs( usage_metatext, stderr );
    return 1;
  }
  
  tundev = create_tun_device( devname, tunflags );
  if( tundev < 0 ) {
    err( 1, "Failed to create TUN/TAP device" );
  }
  if( verbosity >= 10 ) {
    fprintf( stdout, "Created TUN/TAP device '%s'.\n", devname );
  }
  udpsock = tun2udp_open_udp_sock( &udp_local_addr, sizeof(udp_local_addr) );
  if( udpsock < 0 ) return 1; // Error already reported
  
  selectmax = tundev > udpsock ? tundev + 1 : udpsock + 1;
  
  while( 1 ) {
    FD_ZERO( &readfds );
    FD_SET( tundev, &readfds );
    FD_SET( udpsock, &readfds );
    
    z = select( selectmax, &readfds, NULL, NULL, NULL );
    if( z < 0 ) {
      perror( "select() failed" );
      close( tundev );
      close( udpsock );
      return 1;
    } else {
      if( FD_ISSET( tundev, &readfds ) ) {
	bufread = read( tundev, buffer, sizeof(buffer) );
	if( bufread < 0 ) {
	  fprintf( stderr, "Failed to read from %s: %s", devname, strerror(errno) );
	  continue;
	}
	if( verbosity >= 30 ) {
	  fprintf( stderr, "Read %d bytes from TUN/TAP.\n", bufread );
	}
	// printf( "tundev has %d bytes of data\n", bufread );
	z = sendto( udpsock, buffer, bufread, 0, (struct sockaddr *)&udp_remote_addr, sizeof(udp_remote_addr) );
	if( z < 0 ) {
	  warn( "sendto() failed" );
	} else if( verbosity >= 30 ) {
	  fprintf( stderr, "Sent %d bytes over UDP.\n", z );
	}
      } else if( FD_ISSET( udpsock, &readfds ) ) {
	bufread = recvfrom( udpsock, buffer, sizeof(buffer), 0, NULL, 0 );
	if( bufread < 0 ) {
	  perror( "Failed to read from UDP socket" );
	  continue;
	}
	if( verbosity >= 30 ) {
	  fprintf( stderr, "Read %d bytes from UDP packet.\n", bufread );
	}
	z = write( tundev, buffer, bufread );
	if( z < 0 ) {
	  warn( "Failed to write %d bytes to TUN/TAP device %s", bufread, devname );
	} else if( verbosity >= 30 ) {
	  fprintf( stderr, "Wrote %d bytes to TUN/TAP device %s\n", z, devname );
	}
      } else {
	warnx( "Some unknown socket is ready!" );
      }
    }
  }
  
  errx( 1, "while(1) exited.  That should not happen!" );
}
