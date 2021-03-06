/*  GNU ddrescue - Data recovery tool
    Copyright (C) 2004-2015 Antonio Diaz Diaz.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _FILE_OFFSET_BITS 64

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <stdint.h>
#include <unistd.h>

#include "block.h"
#include "ddrescue.h"
#include "loggers.h"


namespace {

int volatile signum_ = 0;		// user pressed Ctrl-C or similar
extern "C" void sighandler( int signum )
  { if( signum_ == 0 && signum > 0 ) signum_ = signum; }


int set_signal( const int signum, void (*handler)( int ) )
  {
  struct sigaction new_action;

  new_action.sa_handler = handler;
  sigemptyset( &new_action.sa_mask );
  new_action.sa_flags = SA_RESTART;
  return sigaction( signum, &new_action, 0 );
  }


bool block_is_zero( const uint8_t * const buf, const int size )
  {
  for( int i = 0; i < size; ++i ) if( buf[i] != 0 ) return false;
  return true;
  }


// Returns the number of bytes really read.
// If (returned value < size) and (errno == 0), means EOF was reached.
//
int readblock( const int fd, uint8_t * const buf, const int size,
               const long long pos )
  {
  int sz = 0;
  errno = 0;
  if( lseek( fd, pos, SEEK_SET ) >= 0 )
    while( sz < size )
      {
      errno = 0;
      const int n = read( fd, buf + sz, size - sz );
      if( n > 0 ) sz += n;
      else if( n == 0 ) break;				// EOF
      else if( errno != EINTR ) break;
      }
  return sz;
  }


// Returns the number of bytes really written.
// If (returned value < size), it is always an error.
//
int writeblock( const int fd, const uint8_t * const buf, const int size,
                const long long pos )
  {
  int sz = 0;
  errno = 0;
  if( lseek( fd, pos, SEEK_SET ) >= 0 )
    while( sz < size )
      {
      errno = 0;
      const int n = write( fd, buf + sz, size - sz );
      if( n > 0 ) sz += n;
      else if( n < 0 && errno != EINTR ) break;
      }
  return sz;
  }

} // end namespace


// Return values: 1 write error, 0 OK.
//
int Fillbook::fill_block( const Sblock & sb )
  {
  if( sb.size() <= 0 || sb.size() > softbs() )
    internal_error( "bad size filling a Block." );
  const int size = sb.size();

  if( write_location_data )	// write location data into each sector
    for( long long pos = sb.pos(); pos < sb.end(); pos += hardbs() )
      {
      char * const buf = (char *)iobuf() + ( pos - sb.pos() );
      const int bufsize = std::min( 80LL, sb.end() - pos );
      const int len = snprintf( buf, bufsize,
                                "\n# position      sector  status\n"
                                "0x%08llX  0x%08llX  %c\n",
                                pos, pos / hardbs(), sb.status() );
      if( len > 0 && len < bufsize )
        std::memset( buf + len, ' ', bufsize - len );
      }
  if( writeblock( odes_, iobuf(), size, sb.pos() + offset() ) != size ||
      ( synchronous_ && fsync( odes_ ) < 0 && errno != EINVAL ) )
    {
    if( !ignore_write_errors ) final_msg( "Write error", errno );
    return 1;
    }
  filled_size += size; remaining_size -= size;
  return 0;
  }


bool Fillbook::read_buffer( const int ides )
  {
  const int rd = readblock( ides, iobuf(), softbs(), 0 );
  if( rd <= 0 ) return false;
  for( int i = rd; i < softbs(); i *= 2 )
    {
    const int size = std::min( i, softbs() - i );
    std::memcpy( iobuf() + i, iobuf(), size );
    }
  return true;
  }


// If copied_size + error_size < b.size(), it means EOF has been reached.
//
void Genbook::check_block( const Block & b, int & copied_size, int & error_size )
  {
  if( b.size() <= 0 ) internal_error( "bad size checking a Block." );
  copied_size = readblock( odes_, iobuf(), b.size(), b.pos() + offset() );
  if( errno ) error_size = b.size() - copied_size;

  for( int pos = 0; pos < copied_size; )
    {
    const int size = std::min( hardbs(), copied_size - pos );
    if( !block_is_zero( iobuf() + pos, size ) )
      {
      change_chunk_status( Block( b.pos() + pos, size ),
                           Sblock::finished, domain() );
      recsize += size;
      }
    gensize += size;
    pos += size;
    }
  }


bool Rescuebook::extend_outfile_size()
  {
  if( min_outfile_size > 0 || sparse_size > 0 )
    {
    const long long min_size = std::max( min_outfile_size, sparse_size );
    const long long size = lseek( odes_, 0, SEEK_END );
    if( size < 0 ) return false;
    if( min_size > size )
      {
      const uint8_t zero = 0;
      if( writeblock( odes_, &zero, 1, min_size - 1 ) != 1 ) return false;
      fsync( odes_ );
      }
    }
  return true;
  }


// Return values: 1 write error, 0 OK.
// If !OK, copied_size and error_size are set to 0.
// If OK && copied_size + error_size < b.size(), it means EOF has been reached.
//
int Rescuebook::copy_block( const Block & b, int & copied_size, int & error_size )
  {
  if( b.size() <= 0 ) internal_error( "bad size copying a Block." );
  if( !test_domain || test_domain->includes( b ) )
    {
    copied_size = readblock( ides_, iobuf(), b.size(), b.pos() );
    error_size = errno ? b.size() - copied_size : 0;
    }
  else { copied_size = 0; error_size = b.size(); }

  if( copied_size > 0 )
    {
    iobuf_ipos = b.pos();
    const long long pos = b.pos() + offset();
    if( sparse_size >= 0 && block_is_zero( iobuf(), copied_size ) )
      {
      const long long end = pos + copied_size;
      if( end > sparse_size ) sparse_size = end;
      }
    else if( writeblock( odes_, iobuf(), copied_size, pos ) != copied_size ||
             ( synchronous_ && fsync( odes_ ) < 0 && errno != EINVAL ) )
      {
      copied_size = 0; error_size = 0;
      final_msg( "Write error", errno );
      return 1;
      }
    }
  else iobuf_ipos = -1;
  read_logger.print_line( b.pos(), b.size(), copied_size, error_size );
  return 0;
  }


const char * format_time( long t, const bool low_prec )
  {
  enum { buffers = 8, bufsize = 16 };
  static char buffer[buffers][bufsize];	// circle of static buffers for printf
  static int current = 0;
  if( t < 0 ) return "n/a";
  char * const buf = buffer[current++]; current %= buffers;
  const long d = t / 86400; t %= 86400;
  const long h = t / 3600; t %= 3600;
  const long m = t / 60; t %= 60;
  int len = 0;

  if( d > 0 ) len = snprintf( buf, bufsize, "%ldd", d );
  if( h > 0 && len >= 0 && len <= 7 - ( h > 9 ) )
    len += snprintf( buf + len, bufsize - len, "%s%ldh", len ? " " : "", h );
  if( m > 0 && len >= 0 && len <= 7 - ( m > 9 ) )
    len += snprintf( buf + len, bufsize - len, "%s%ldm", len ? " " : "", m );
  if( ( t > 0 && len >= 0 && len <= 7 - ( t > 9 ) && !low_prec ) || len == 0 )
    len += snprintf( buf + len, bufsize - len, "%s%lds", len ? " " : "", t );
  return buf;
  }


bool interrupted() { return ( signum_ > 0 ); }


void set_signals()
  {
  signum_ = 0;
  set_signal( SIGHUP, sighandler );
  set_signal( SIGINT, sighandler );
  set_signal( SIGTERM, sighandler );
  set_signal( SIGUSR1, SIG_IGN );
  set_signal( SIGUSR2, SIG_IGN );
  }

int signaled_exit()
  {
  set_signal( signum_, SIG_DFL );
  std::raise( signum_ );
  return 128 + signum_;			// in case std::raise fails to exit
  }
