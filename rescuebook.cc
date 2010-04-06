/*  GNU ddrescue - Data recovery tool
    Copyright (C) 2004, 2005, 2006, 2007, 2008, 2009, 2010
    Antonio Diaz Diaz.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _FILE_OFFSET_BITS 64

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <unistd.h>

#include "block.h"
#include "ddrescue.h"


// Return values: 0 OK, -1 interrupted, -2 logfile error.
//
int Rescuebook::check_all()
  {
  long long pos = ( (offset() >= 0) ? 0 : -offset() );
  if( current_status() == generating && domain().includes( current_pos() ) &&
      ( offset() >= 0 || current_pos() >= -offset() ) )
    pos = current_pos();
  bool first_post = true;

  while( pos >= 0 )
    {
    Block b( pos, softbs() );
    find_chunk( b, Sblock::non_tried );
    if( b.size() <= 0 ) break;
    pos = b.end();
    current_status( generating );
    if( verbosity >= 0 )
      { show_status( b.pos(), "Generating logfile...", first_post ); first_post = false; }
    int copied_size, error_size;
    const int retval = check_block( b, copied_size, error_size );
    if( !retval )
      {
      if( copied_size + error_size < b.size() )		// EOF
        truncate_vector( b.pos() + copied_size + error_size );
      }
    if( retval ) return retval;
    if( !update_logfile() ) return -2;
    }
  return 0;
  }


void Rescuebook::count_errors() throw()
  {
  bool good = true;
  errors = 0;
  for( int i = 0; i < sblocks(); ++i )
    {
    const Sblock & sb = sblock( i );
    if( !domain().includes( sb ) ) { if( domain() < sb ) break; else continue; }
    switch( sb.status() )
      {
      case Sblock::non_tried:
      case Sblock::finished:   good = true; break;
      case Sblock::non_trimmed:
      case Sblock::non_split:
      case Sblock::bad_sector: if( good ) { good = false; ++errors; } break;
      }
    }
  }


// Return values: 1 I/O error, 0 OK, -1 interrupted.
//
int Rescuebook::copy_and_update( const Block & b, const Sblock::Status st,
                                 int & copied_size, int & error_size,
                                 const char * const msg, bool & first_post )
  {
  if( verbosity >= 0 )
    { show_status( b.pos(), msg, first_post ); first_post = false; }
  int retval = copy_block( b, copied_size, error_size );
  if( !retval )
    {
    if( copied_size + error_size < b.size() )		// EOF
      truncate_vector( b.pos() + copied_size + error_size );
    if( copied_size > 0 )
      {
      change_chunk_status( Block( b.pos(), copied_size ), Sblock::finished );
      recsize += copied_size;
      }
    if( error_size > 0 )
      {
      if( error_size >= hardbs() && st != Sblock::bad_sector )
        {
        const Block b1( b.pos() + copied_size, hardbs() );
        const Block b2( b1.end(), error_size - b1.size() );
        change_chunk_status( b1, Sblock::bad_sector );
        change_chunk_status( b2, st );
        }
      else
        change_chunk_status( Block( b.pos() + copied_size, error_size ), st );
      if( max_errors_ >= 0 ) count_errors();
      if( iname_ && access( iname_, F_OK ) != 0 )
        {
        final_msg( "input file disappeared" ); final_errno( errno );
        retval = 1;
        }
      }
    }
  return retval;
  }


// Return values: 1 I/O error, 0 OK, -1 interrupted, -2 logfile error.
// Read the non-damaged part of the domain, skipping over the damaged areas.
//
int Rescuebook::copy_non_tried()
  {
  long long pos = 0;
  long long skip_size = hardbs();	// size to skip on error
  bool first_post = true;

  while( pos >= 0 )
    {
    Block b( pos, skip_size ? hardbs() : softbs() );
    find_chunk( b, Sblock::non_tried );
    if( pos != b.pos() ) skip_size = 0;	// reset size on block change
    pos = b.end();
    if( pos < 0 || b.size() <= 0 ) break;
    current_status( copying );
    int copied_size, error_size;
    const int retval = copy_and_update( b, skip_size ? Sblock::bad_sector : Sblock::non_trimmed,
                                        copied_size, error_size,
                                        "Copying non-tried blocks...", first_post );
    if( error_size > 0 )
      {
      errsize += error_size;
      if( skip_size < skipbs() ) skip_size = skipbs();
      else if( skip_size < LLONG_MAX / 4 ) skip_size *= 2;
      b.pos( pos ); b.size( skip_size ); b.fix_size();
      find_chunk( b, Sblock::non_tried );
      if( pos == b.pos() && b.size() > 0 )
        { change_chunk_status( b, Sblock::non_trimmed );
          pos = b.end(); errsize += b.size(); }
      }
    else if( skip_size > 0 && copied_size > 0 )
      { skip_size -= copied_size; if( skip_size < 0 ) skip_size = 0; }
    if( retval || too_many_errors() ) return retval;
    if( !update_logfile( odes_ ) ) return -2;
    }
  return 0;
  }


// Return values: 1 I/O error, 0 OK, -1 interrupted, -2 logfile error.
// Trim the damaged areas backwards.
//
int Rescuebook::trim_errors()
  {
  long long pos = LLONG_MAX - hardbs();
  bool first_post = true;

  while( pos >= 0 )
    {
    Block b( pos, hardbs() );
    rfind_chunk( b, Sblock::non_trimmed );
    if( b.size() <= 0 ) break;
    pos = b.pos() - hardbs();
    current_status( trimming );
    int copied_size, error_size;
    const int retval = copy_and_update( b, Sblock::bad_sector, copied_size,
                                        error_size, "Trimming failed blocks...",
                                        first_post );
    if( copied_size > 0 ) errsize -= copied_size;
    if( error_size > 0 && b.pos() > 0 )
      {
      const int index = find_index( b.pos() - 1 );
      if( index >= 0 && domain().includes( sblock( index ) ) &&
          sblock( index ).status() == Sblock::non_trimmed )
        change_chunk_status( sblock( index ), Sblock::non_split );
      }
    if( retval || too_many_errors() ) return retval;
    if( !update_logfile( odes_ ) ) return -2;
    }
  return 0;
  }


// Return values: 1 I/O error, 0 OK, -1 interrupted, -2 logfile error.
// Try to read the damaged areas, splitting them into smaller pieces.
//
int Rescuebook::split_errors()
  {
  bool first_post = true;
  bool resume = ( current_status() == splitting &&
                  domain().includes( current_pos() ) );
  while( true )
    {
    long long pos = 0;
    if( resume ) { resume = false; pos = current_pos(); }
    int error_counter = 0;
    bool block_found = false;

    while( pos >= 0 )
      {
      Block b( pos, hardbs() );
      find_chunk( b, Sblock::non_split );
      if( b.size() <= 0 ) break;
      pos = b.end();
      block_found = true;
      current_status( splitting );
      int copied_size, error_size;
      const int retval = copy_and_update( b, Sblock::bad_sector, copied_size,
                                          error_size, "Splitting failed blocks...",
                                          first_post );
      if( copied_size > 0 ) errsize -= copied_size;
      if( error_size <= 0 ) error_counter = 0;
      else if( ++error_counter >= 2 && error_counter * hardbs() >= 2 * skipbs() )
        {			// skip after enough consecutive errors
        error_counter = 0;
        const int index = find_index( pos );
        if( index >= 0 && sblock( index ).status() == Sblock::non_split )
          {
          const Sblock & sb = sblock( index );
          if( sb.size() >= 2 * skipbs() && sb.size() >= 4 * hardbs() )
            pos += ( sb.size() / ( 2 * hardbs() ) ) * hardbs();
          }
        }
      if( retval || too_many_errors() ) return retval;
      if( !update_logfile( odes_ ) ) return -2;
      }
    if( !block_found ) break;
    }
  return 0;
  }


// Return values: 1 I/O error, 0 OK, -1 interrupted, -2 logfile error.
// Try to read the damaged areas, one hard block at a time.
//
int Rescuebook::copy_errors()
  {
  char msgbuf[80] = "Retrying bad sectors... Retry ";
  const int msglen = std::strlen( msgbuf );
  bool resume = ( current_status() == retrying &&
                  domain().includes( current_pos() ) );

  for( int retry = 1; max_retries_ < 0 || retry <= max_retries_; ++retry )
    {
    long long pos = 0;
    if( resume ) { resume = false; pos = current_pos(); }
    bool first_post = true, block_found = false;
    snprintf( msgbuf + msglen, ( sizeof msgbuf ) - msglen, "%d", retry );

    while( pos >= 0 )
      {
      Block b( pos, hardbs() );
      find_chunk( b, Sblock::bad_sector );
      if( b.size() <= 0 ) break;
      pos = b.end();
      block_found = true;
      current_status( retrying );
      int copied_size, error_size;
      const int retval = copy_and_update( b, Sblock::bad_sector, copied_size,
                                          error_size, msgbuf, first_post );
      if( copied_size > 0 ) errsize -= copied_size;
      if( retval || too_many_errors() ) return retval;
      if( !update_logfile( odes_ ) ) return -2;
      }
    if( !block_found ) break;
    }
  return 0;
  }


Rescuebook::Rescuebook( const long long ipos, const long long opos,
                        Domain & dom, const long long isize, const char * const iname,
                        const char * const logname, const int cluster, const int hardbs,
                        const int max_errors, const int max_retries,
                        const bool complete_only, const bool nosplit,
                        const bool retrim, const bool sparse,
                        const bool synchronous, const bool try_again )
  : Logbook( ipos, opos, dom, isize, logname, cluster, hardbs, complete_only ),
    sparse_size( 0 ),
    iname_( ( iname && access( iname, F_OK ) == 0 ) ? iname : 0 ),
    max_errors_( max_errors ), max_retries_( max_retries ),
    skipbs_( std::max( 65536, hardbs ) ),
    nosplit_( nosplit ), sparse_( sparse ), synchronous_( synchronous ),
    a_rate( 0 ), c_rate( 0 ), first_size( 0 ), last_size( 0 ),
    last_ipos( 0 ), t0( 0 ), t1( 0 ), ts( 0 ), oldlen( 0 )
  {
  if( retrim )
    for( int index = 0; index < sblocks(); ++index )
      {
      const Sblock & sb = sblock( index );
      if( !domain().includes( sb ) )
        { if( domain() < sb ) break; else continue; }
      if( sb.status() == Sblock::non_split ||
          sb.status() == Sblock::bad_sector )
        change_sblock_status( index, Sblock::non_trimmed );
      }
  if( try_again )
    for( int index = 0; index < sblocks(); ++index )
      {
      const Sblock & sb = sblock( index );
      if( !domain().includes( sb ) )
        { if( domain() < sb ) break; else continue; }
      if( sb.status() == Sblock::non_split ||
          sb.status() == Sblock::non_trimmed )
        change_sblock_status( index, Sblock::non_tried );
      }
  }


// Return values: 1 write error, 0 OK.
//
int Rescuebook::do_generate( const int odes )
  {
  recsize = 0; errsize = 0;
  ides_ = -1; odes_ = odes;

  for( int i = 0; i < sblocks(); ++i )
    {
    const Sblock & sb = sblock( i );
    if( !domain().includes( sb ) ) { if( domain() < sb ) break; else continue; }
    switch( sb.status() )
      {
      case Sblock::non_tried:   break;
      case Sblock::non_trimmed: 			// fall through
      case Sblock::non_split:   			// fall through
      case Sblock::bad_sector:  errsize += sb.size(); break;
      case Sblock::finished:    recsize += sb.size(); break;
      }
    }
  count_errors();
  set_signals();
  if( verbosity >= 0 )
    {
    std::printf( "Press Ctrl-C to interrupt\n" );
    if( filename() )
      {
      std::printf( "Initial status (read from logfile)\n" );
      std::printf( "rescued: %10sB,", format_num( recsize ) );
      std::printf( "  errsize:%9sB,", format_num( errsize, 99999 ) );
      std::printf( "  errors: %7u\n", errors );
      std::printf( "Current status\n" );
      }
    }
  int retval = check_all();
  if( verbosity >= 0 )
    {
    show_status( -1, (retval ? 0 : "Finished"), true );
    if( retval == -2 ) std::printf( "Logfile error" );
    else if( retval < 0 ) std::printf( "\nInterrupted by user" );
    std::fputc( '\n', stdout );
    }
  if( retval == -2 ) retval = 1;		// logfile error
  else
    {
    if( retval == 0 ) current_status( finished );
    else if( retval < 0 ) retval = 0;		// interrupted by user
    compact_sblock_vector();
    if( !update_logfile( -1, true ) && retval == 0 ) retval = 1;
    }
  if( final_msg() ) show_error( final_msg(), final_errno() );
  return retval;
  }


// Return values: 1 I/O error, 0 OK.
//
int Rescuebook::do_rescue( const int ides, const int odes )
  {
  bool copy_pending = false, trim_pending = false, split_pending = false;
  recsize = 0; errsize = 0;
  ides_ = ides; odes_ = odes;

  for( int i = 0; i < sblocks(); ++i )
    {
    const Sblock & sb = sblock( i );
    if( !domain().includes( sb ) ) { if( domain() < sb ) break; else continue; }
    switch( sb.status() )
      {
      case Sblock::non_tried:   copy_pending = trim_pending = split_pending = true;
                                break;
      case Sblock::non_trimmed: trim_pending = true;	// fall through
      case Sblock::non_split:   split_pending = true;	// fall through
      case Sblock::bad_sector:  errsize += sb.size(); break;
      case Sblock::finished:    recsize += sb.size(); break;
      }
    }
  count_errors();
  set_signals();
  if( verbosity >= 0 )
    {
    std::printf( "Press Ctrl-C to interrupt\n" );
    if( filename() )
      {
      std::printf( "Initial status (read from logfile)\n" );
      std::printf( "rescued: %10sB,", format_num( recsize ) );
      std::printf( "  errsize:%9sB,", format_num( errsize, 99999 ) );
      std::printf( "  errors: %7u\n", errors );
      std::printf( "Current status\n" );
      }
    }
  int retval = 0;
  if( copy_pending && !too_many_errors() )
    retval = copy_non_tried();
  if( !retval && trim_pending && !too_many_errors() )
    retval = trim_errors();
  if( !retval && split_pending && !nosplit_ && !too_many_errors() )
    retval = split_errors();
  if( !retval && max_retries_ != 0 && !too_many_errors() )
    retval = copy_errors();
  if( verbosity >= 0 )
    {
    show_status( -1, (retval ? 0 : "Finished"), true );
    if( retval == -2 ) std::printf( "Logfile error" );
    else if( retval < 0 ) std::printf( "\nInterrupted by user" );
    else if( too_many_errors() ) std::printf("\nToo many errors in input file" );
    std::fputc( '\n', stdout );
    }
  if( retval == -2 ) retval = 1;		// logfile error
  else
    {
    if( retval == 0 ) current_status( finished );
    else if( retval < 0 ) retval = 0;		// interrupted by user
    if( !sync_sparse_file() )
      {
      show_error( "error syncing sparse output file" );
      if( retval == 0 ) retval = 1;
      }
    compact_sblock_vector();
    if( !update_logfile( odes_, true ) && retval == 0 ) retval = 1;
    }
  if( final_msg() ) show_error( final_msg(), final_errno() );
  return retval;
  }