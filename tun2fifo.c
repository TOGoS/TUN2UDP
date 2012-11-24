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

#define SLIP_END     ((char)192)
#define SLIP_ESC     ((char)219)
#define SLIP_ESC_END ((char)220)
#define SLIP_ESC_ESC ((char)221)

static int verbosity = 10;

/**
 * If there is a complete packet in the circular buffer at packetbegin
 * (i.e. a END byte is found before running into bufend), returns a pointer
 * to the END byte.
 * Returns NULL if no end was found.
 */
char *slip_packet_complete( char *buffer, int bufsize, int packetbegin, int bufend ) {
  while( packetbegin != bufend ) {
    if( buffer[packetbegin] == SLIP_END ) return buffer+packetbegin;
    packetbegin = (packetbegin + 1) % bufsize;
  }
  return NULL;
}

/**
 * Encode exacly len bytes from rawbuf into slipbuf.
 * Does not prepend or append END characters.
 * Returns a pointer to the end of the written data.
 * Caller must make sure there is at least (end-rawbuf)*2 capacity at dest
 */
char *slip_encode( char *rawbuf, char *end, char *dest ) {
  for( ; rawbuf < end ; ++rawbuf ) {
    switch( *rawbuf ) {
    case SLIP_ESC:
      *dest++ = SLIP_ESC;
      *dest++ = SLIP_ESC_ESC;
      break;
    case SLIP_END:
      *dest++ = SLIP_ESC;
      *dest++ = SLIP_ESC_END;
      break;
    default:
      *dest++ = *rawbuf;
    }
  }
  return dest;
}

/**
 * Decode at most len input bytes from the slip buffer to the output buffer.
 * Will also stop decoding if an END byte is encountered.
 * Returns the pointer to the end of the output buffer.
 */
char *slip_decode( char *slipbuf, char *slipend, char *dest ) {
  char c;
  
  for( ; slipbuf < slipend && *slipbuf != SLIP_END ; ++slipbuf ) {
    switch( *slipbuf ) {
    case SLIP_ESC:
      ++slipbuf;
      if( slipbuf == slipend ) {
	// Incomplete packet!
	return dest;
      }
      switch( *slipbuf ) {
      case SLIP_ESC_ESC:
	c = SLIP_ESC;
	break;
      case SLIP_ESC_END:
	c = SLIP_END;
	break;
      default:
	// Differently malformed packet!
	c = *slipbuf;
      }
      break;
    default:
      c = *slipbuf;
    }
    
    *dest++ = c;
  }
  return dest;
}

static const char *usage_metatext = "Run with -? for usage information.\n";
static const char *usage_text =
"Usage: tun2fifo\n"
"  -read <file>           -- read incoming packets from this file\n"
"  -write <file>          -- write outgoing packets to this file\n"
"  {-tun|-tap}            -- create a TUN or TAP device\n"
"  [-no-pi]               -- don't include extra packet framing\n"
"  [-debug]               -- be extra talkative\n"
"  [-dev <devname>]       -- create the TUN/TAP device with this name\n"
"\n"
"If <devname> is not specified, a name will be picked automatically.\n"
"\n"
"By default, standard input/output are used to read/write packets.\n";

int main( int argc, char **argv ) {
  char devname[128];
  int z;
  int tunflags = 0;
  int tundev;
  int readsock;
  int writesock;
  int selectmax;
  char *write_filename; // Will write packets to here
  char *read_filename; // Will read packets from here
  fd_set readfds;
  char buffer[2048];
  char slipbuffer[4098];
  char *slipbuffer2;
  char readbuffer[4098];
  int readoffset = 0; // point in readbuffer at which incoming data will be written
  int readpacketoffset = 0; // beginning of unsent packet in readbuffer
  char *readpacketend; // pointer to end of completely read packet
  size_t bufread;
  int local_addr_given = 0;
  int remote_addr_given = 0;

  // TODO: if device not specified, print out unless -q given.
  devname[0] = 0;
  readoffset = 0;
  write_filename = "-";
  read_filename = "-";
  
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
    } else if( strcmp("-dev",argv[z]) == 0 ) {
      ++z;
      if( z >= argc ) {
	errx( 1, "%s needs an additional device-name argument.", argv[z-1] );
      }
      strncpy( devname, argv[z], sizeof(devname) );
      devname[sizeof(devname)-1] = 0;
    } else if( strcmp("-read",argv[z]) == 0 ) {
      ++z;
      if( z >= argc ) {
	errx( 1, "-read needs an additional path argument." );
      }
      read_filename = argv[z];
    } else if( strcmp("-write",argv[z]) == 0 ) {
      ++z;
      if( z >= argc ) {
	errx( 1, "-write needs an additional path argument." );
      }
      write_filename = argv[z];
    } else if( strcmp("-?",argv[z]) == 0 || strcmp("-h",argv[z]) == 0 || strcmp("-help",argv[z]) == 0 ) {
      fputs( usage_text, stdout );
      return 0;
    } else {
      warnx( "Error: Unrecgonized argument: %s.", argv[z] );
      fputs( usage_metatext, stderr );
      return 1;
    }
  }
  if( read_filename == NULL && write_filename == NULL ) {
    warnx( "Warning: No -read or -write given; this program will be useless!" );
  }
  
  tundev = create_tun_device( devname, tunflags );
  if( tundev < 0 ) {
    err( 1, "Failed to create TUN/TAP device" );
  }
  if( verbosity >= 10 ) {
    fprintf( stdout, "Created TUN/TAP device '%s'.\n", devname );
  }
  
  if( read_filename != NULL ) {
    // Use O_RDWR instead of O_RDONLY to make select work properly
    // http://www.outflux.net/blog/archives/2008/03/09/using-select-on-a-fifo/
    if( strcmp("-",read_filename) == 0 ) {
      readsock = 0;
    } else {
      readsock = open( read_filename, O_RDWR|O_NONBLOCK );
      if( readsock < 0 ) {
	err( 1, "Failed to open %s for reading", read_filename );
      }
    }
  } else {
    readsock = -1;
  }
  
  if( write_filename != NULL ) {
    if( strcmp("-",write_filename) == 0 ) {
      writesock = 1;
    } else {
      writesock = open( write_filename, O_WRONLY|O_CREAT );
      if( writesock < 0 ) {
	err( 1, "Failed to open %s for writing", write_filename );
      }
    }
  } else {
    writesock = -1;
  }
  
  selectmax = tundev > readsock ? tundev + 1 : readsock + 1;
  
  while( 1 ) {
    FD_ZERO( &readfds );
    FD_SET( tundev, &readfds );
    if( readsock != -1 ) FD_SET( readsock, &readfds );
    
    z = select( selectmax, &readfds, NULL, NULL, NULL );
    if( z < 0 ) {
      err( 1, "select() failed" );
    } else {
      if( FD_ISSET( tundev, &readfds ) ) {
	bufread = read( tundev, buffer, sizeof(buffer) );
	if( bufread < 0 ) {
	  warn( "Failed to read from %s: %s", devname );
	  continue;
	} else if( verbosity >= 30 ) {
	  fprintf( stderr, "Read %d bytes from TUN/TAP device %s.\n", bufread, devname );
	}
	
	if( writesock != -1 ) {
	  slipbuffer[0] = SLIP_END;
	  slipbuffer2 = slip_encode( buffer, buffer+bufread, slipbuffer+1 );
	  *slipbuffer2++ = SLIP_END;
	  z = write( writesock, slipbuffer, slipbuffer2-slipbuffer );
	  if( z < 0 ) {
	    perror( "write() to device failed" );
	  } else if( verbosity >= 30 ) {
	    fprintf( stderr, "Wrote %d bytes to %s.\n", z, write_filename );
	  }
	}
      } else if( FD_ISSET( readsock, &readfds ) ) {
	bufread = read( readsock, readbuffer+readoffset, sizeof(slipbuffer)-readoffset );
	if( bufread < 0 ) {
	  warn( "Failed to read from %s", read_filename );
	  continue;
	} else if( verbosity >= 30 ) {
	  fprintf( stderr, "Read %d bytes from %s.\n", bufread, read_filename );
	}
	readoffset = (readoffset + bufread) % sizeof(slipbuffer);
	
	// See if a packet has been completed
	while( (readpacketend = slip_packet_complete( readbuffer, sizeof(readbuffer), readpacketoffset, readoffset )) != NULL ) {
	  // Decode data from readbuffer into slipbuffer
	  slipbuffer2 = slipbuffer;
	  if( readpacketend < readbuffer+readpacketoffset ) {
	    // Read to the end, first...
	    slipbuffer2 = slip_decode( readbuffer+readpacketoffset, readbuffer+sizeof(readbuffer), slipbuffer2 );
	    readpacketoffset = 0;
	  }
	  slipbuffer2 = slip_decode( readbuffer+readpacketoffset, readpacketend, slipbuffer2 );
	  // Unless we read several packets at once, this should end up with
	  // readpacketoffset = readoffset, and this loop will stop.
	  readpacketoffset = (readpacketend - readbuffer)+1;
	  
	  if( verbosity >= 30 ) {
	    fprintf( stderr, "Read %d-byte packet from %s.\n", (slipbuffer2-slipbuffer), read_filename );
	    fprintf( stderr, "Read buffer state (packet/read/size) = %d / %d / %d.\n", readpacketoffset, readoffset, sizeof(readbuffer) );
	  }
	  
	  if( slipbuffer2 > slipbuffer ) {
	    z = write( tundev, slipbuffer, slipbuffer2-slipbuffer );
	    if( z < 0 ) {
	      warn( "Failed to write %d bytes to TUN/TAP device %s", (slipbuffer2-slipbuffer), devname );
	    } else if( verbosity >= 30 ) {
	      fprintf( stderr, "Wrote %d bytes to TUN/TAP device %s.\n", z, devname );
	    }
	  }
	}
      } else {
	warnx( "Some unknown socket is ready!" );
      }
    }
  }
  
  errx( 1, "while(1) exited.  That should not happen!" );
}
