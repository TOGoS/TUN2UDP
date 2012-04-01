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

/** Arguments taken by the function:
 *
 * @param char *dev the name of an interface (or '\0'). MUST have enough
 *   space to hold the interface name if '\0' is passed
 * @param int flags interface flags (eg, IFF_TUN etc.)
 * @return int file descriptor (if positive) or error code (negative)
 */
int tun2udp_create_device( char *dev, int flags ) {
  struct ifreq ifr;
  int fd, err;
  char *clonedev = "/dev/net/tun";
  
  /* open the clone device */
  if( (fd = open(clonedev, O_RDWR)) < 0 ) {
    return fd;
  }
  
  /* preparation of the struct ifr, of type "struct ifreq" */
  memset(&ifr, 0, sizeof(ifr));
  
  ifr.ifr_flags = flags;   /* IFF_TUN or IFF_TAP, plus maybe IFF_NO_PI */
  
  if( *dev ) {
    /* if a device name was specified, put it in the structure; otherwise,
     * the kernel will try to allocate the "next" device of the
     * specified type */
    strncpy(ifr.ifr_name, dev, IFNAMSIZ);
  }
  
  /* try to create the device */
  if( (err = ioctl(fd, TUNSETIFF, (void *) &ifr)) < 0 ) {
    close(fd);
    return err;
  }
  
  /* if the operation was successful, write back the name of the
   * interface to the variable "dev", so the caller can know
   * it. Note that the caller MUST reserve space in *dev (see calling
   * code below) */
  strcpy(dev, ifr.ifr_name);
  
  /* this is the special file descriptor that the caller will use to talk
   * with the virtual interface */
  return fd;
}

/**
 * @return int positive to indicate number of bytes read, 0 if no data was reed, negative to indicate an error
 */
int tun2udp_read_packet( int fd, char *buffer, int buffer_size, int timeout_us ) {
  fd_set readfds;
  struct timeval timeout;
  timeout.tv_sec  = timeout_us / 1000000;
  timeout.tv_usec = timeout_us % 1000000;
  
  FD_ZERO( &readfds );
  FD_SET( fd, &readfds );
  
  if( select( fd+1, &readfds, NULL, NULL, timeout_us == -1 ? NULL : &timeout ) < 0 ) {
    return -1;
  }
  
  if( FD_ISSET( fd, &readfds ) ) {
    return read( fd, buffer, buffer_size );
  } else {
    return 0;
  }
}

int tun2udp_open_udp_sock( struct sockaddr *addr, size_t addrsize ) {
  int sock;
  int z;

  sock = socket( PF_INET, SOCK_DGRAM, 0 );
  if( sock < 0 ) return sock;
  
  z = bind( sock, addr, addrsize );
  if( z < 0 ) {
    close( sock );
    return z;
  }
  
  return sock;
}

// TODO: Delete when obsolete
void set_default_local_sockaddr_in( struct sockaddr_in *addy ) {
  addy->sin_family = AF_INET;
  addy->sin_addr.s_addr = htonl(INADDR_ANY);
  addy->sin_port = htons(45713);
}
void set_default_remote_sockaddr_in( struct sockaddr_in *addy ) {
  addy->sin_family = AF_INET;
  addy->sin_addr.s_addr = htonl(0x7F000001);
  addy->sin_port = htons(45714);
}

int parse_address( const char *text, struct sockaddr *addr ) {
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

int main( int argc, char **argv ) {
  char devname[128];
  int z;
  int tunflags = 0;
  int tundev;
  int udpsock;
  int selectmax;
  // TODO: Allow INET6 addresses:
  struct sockaddr udp_local_addr;
  struct sockaddr udp_remote_addr;
  fd_set readfds;
  char buffer[2048];
  size_t bufread;
  int local_addr_given = 0;
  int remote_addr_given = 0;
  int debug = 0;
  
  devname[0] = 0;
  
  for( z=1; z<argc; ++z ) {
    if( strcmp("-debug",argv[z]) == 0 ) {
      debug = 1;
    } else if( strcmp("-tun",argv[z]) == 0 ) {
      tunflags |= IFF_TUN;
    } else if( strcmp("-tap",argv[z]) == 0 ) {
      tunflags |= IFF_TAP;
    } else if( strcmp("-no-pi",argv[z]) == 0 ) {
      tunflags |= IFF_NO_PI;
    } else if( strcmp("-pi",argv[z]) == 0 ) {
      tunflags &= ~IFF_NO_PI;
    } else if( strcmp("-tun-dev",argv[z]) == 0 ) {
      ++z;
      if( z >= argc ) {
	fprintf( stderr, "-local-address needs an additional <host>:<port> argument.\n" );
	return 1;
      }
      strncpy( devname, argv[z], sizeof(devname) );
      devname[sizeof(devname)-1] = 0;
    } else if( strcmp("-local-address",argv[z]) == 0 ) {
      ++z;
      if( z >= argc ) {
	fprintf( stderr, "-local-address needs an additional <host>:<port> argument.\n" );
	return 1;
      }
      if( parse_address( argv[z], &udp_local_addr ) ) return 1;
      local_addr_given = 1;
    } else if( strcmp("-remote-address",argv[z]) == 0 ) {
      ++z;
      if( z >= argc ) {
	fprintf( stderr, "-remote-address needs an additional <host>:<port> argument.\n" );
	return 1;
      }
      if( parse_address( argv[z], &udp_remote_addr ) ) return 1;
      remote_addr_given = 1;
    } else {
      fprintf( stderr, "Unrecgonized argument: %s.\n", argv[z] );
      return 1;
    }
  }
  if( !local_addr_given ) {
    fprintf( stderr, "No -local-address given.\n" );
    return 1;
  }
  if( !remote_addr_given ) {
    fprintf( stderr, "No -remote-address given.\n" );
    return 1;
  }
  
  tundev = tun2udp_create_device( devname, tunflags );
  if( tundev < 0 ) {
    perror( "Failed to create TUN/TAP device");
    return 1;
  } else {
    printf( "Device %s\n", devname );
  }
  udpsock = tun2udp_open_udp_sock( &udp_local_addr, sizeof(struct sockaddr) );
  if( udpsock < 0 ) {
    perror( "Failed to create UDP socket");
    return 1;
  }
  
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
	if( debug ) {
	  fprintf( stderr, "Read %d bytes from TUN/TAP.\n", bufread );
	}
	// printf( "tundev has %d bytes of data\n", bufread );
	z = sendto( udpsock, buffer, bufread, 0, &udp_remote_addr, sizeof(struct sockaddr) );
	if( z < 0 ) {
	  perror( "sendto() failed" );
	}
      } else if( FD_ISSET( udpsock, &readfds ) ) {
	bufread = recvfrom( udpsock, buffer, sizeof(buffer), 0, NULL, 0 );
	if( bufread < 0 ) {
	  perror( "Failed to read from UDP socket" );
	  continue;
	}
	if( debug ) {
	  fprintf( stderr, "Read %d bytes from UDP packet.\n", bufread );
	}
	z = write( tundev, buffer, bufread );
	if( z < 0 ) {
	  fprintf( stderr, "Failed to write %d bytes to %s", bufread, devname );
	}
      } else {
	printf( "?? has data\n" );
      }
    }
  }
  
  printf( "while(1) exited?" );
  
  return 0;
}
