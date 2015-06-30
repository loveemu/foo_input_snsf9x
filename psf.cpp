#define MYVERSION "0.5.0"

/*
	changelog

2015-06-30 12:50 UTC - loveemu
- Added channel mute capability
- Added limited SA-1 support (I-RAM access, no coprocessor)

2015-06-28 08:50 UTC - loveemu
- Copied from foo_input_gsf code base

*/

#define _WIN32_WINNT 0x0501

#include "../SDK/foobar2000.h"
#include "../helpers/window_placement_helper.h"
#include "../ATLHelpers/ATLHelpers.h"

#include "resource.h"

#include <stdio.h>

#include "../../../snsf9x/snsf9x/SNESSystem.h"

#include <psflib.h>

#include "circular_buffer.h"

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlctrls.h>
#include <atlctrlx.h>

//#define DBG(a) OutputDebugStringA(a)
#define DBG(a)

typedef unsigned long u_long;

critical_section g_sync;
static int initialized = 0;

// {25FB629E-A680-467C-9B08-77CE488AE30A}
static const GUID guid_cfg_infinite = 
{ 0x25fb629e, 0xa680, 0x467c, { 0x9b, 0x8, 0x77, 0xce, 0x48, 0x8a, 0xe3, 0x0a } };
// {C15B5598-FCCB-4553-9D07-5A48AB1155EB}
static const GUID guid_cfg_deflength = 
{ 0xc15b5598, 0xfccb, 0x4553, { 0x9d, 0x7, 0x5a, 0x48, 0xab, 0x11, 0x55, 0xeb } };
// {B643FB13-3C2D-4FE2-ACE7-FF27B28E652F}
static const GUID guid_cfg_deffade = 
{ 0xb643fb13, 0x3c2d, 0x4fe2, { 0xac, 0xe7, 0xff, 0x27, 0xb2, 0x8e, 0x65, 0x2f } };
// {0D9294B5-2F8D-408B-AC86-04739251498D}
static const GUID guid_cfg_suppressopeningsilence = 
{ 0x0d9294b5, 0x2f8d, 0x408b, { 0xac, 0x86, 0x4, 0x73, 0x92, 0x51, 0x49, 0x8d } };
// {6BE93341-73D1-4290-A021-252B1CFD7D83}
static const GUID guid_cfg_suppressendsilence = 
{ 0x6be93341, 0x73d1, 0x4290, { 0xa0, 0x21, 0x25, 0x2b, 0x1c, 0xfd, 0x7d, 0x83 } };
// {8AF9A91E-436C-4C67-888F-F6B1CA2EA09F}
static const GUID guid_cfg_endsilenceseconds = 
{ 0x8af9a91e, 0x436c, 0x4c67, { 0x88, 0x8f, 0xf6, 0xb1, 0xca, 0x2e, 0xa0, 0x9f } };
// {6B644874-8399-4C99-8445-FF69211F5AFB}
static const GUID guid_cfg_placement = 
{ 0x6b644874, 0x8399, 0x4c99, { 0x84, 0x45, 0xff, 0x69, 0x21, 0x1f, 0x5a, 0xfb } };

enum
{
	default_cfg_infinite = 0,
	default_cfg_deflength = 170000,
	default_cfg_deffade = 10000,
	default_cfg_suppressopeningsilence = 1,
	default_cfg_suppressendsilence = 1,
	default_cfg_endsilenceseconds = 5,
};

static cfg_int cfg_infinite(guid_cfg_infinite,default_cfg_infinite);
static cfg_int cfg_deflength(guid_cfg_deflength,default_cfg_deflength);
static cfg_int cfg_deffade(guid_cfg_deffade,default_cfg_deffade);
static cfg_int cfg_suppressopeningsilence(guid_cfg_suppressopeningsilence,default_cfg_suppressopeningsilence);
static cfg_int cfg_suppressendsilence(guid_cfg_suppressendsilence,default_cfg_suppressendsilence);
static cfg_int cfg_endsilenceseconds(guid_cfg_endsilenceseconds,default_cfg_endsilenceseconds);
static cfg_window_placement cfg_placement(guid_cfg_placement);

static int cfg_mutedchannels = 0;

static const char field_length[]="snsf_length";
static const char field_fade[]="snsf_fade";

#define BORK_TIME 0xC0CAC01A

static unsigned long parse_time_crap(const char *input)
{
	unsigned long value = 0;
	unsigned long multiplier = 1000;
	const char * ptr = input;
	unsigned long colon_count = 0;

	while (*ptr && ((*ptr >= '0' && *ptr <= '9') || *ptr == ':'))
	{
		colon_count += *ptr == ':';
		++ptr;
	}
	if (colon_count > 2) return BORK_TIME;
	if (*ptr && *ptr != '.' && *ptr != ',') return BORK_TIME;
	if (*ptr) ++ptr;
	while (*ptr && *ptr >= '0' && *ptr <= '9') ++ptr;
	if (*ptr) return BORK_TIME;

	ptr = strrchr(input, ':');
	if (!ptr)
		ptr = input;
	for (;;)
	{
		char * end;
		if (ptr != input) ++ptr;
		if (multiplier == 1000)
		{
			double temp = pfc::string_to_float(ptr);
			if (temp >= 60.0) return BORK_TIME;
			value = (long)(temp * 1000.0f);
		}
		else
		{
			unsigned long temp = strtoul(ptr, &end, 10);
			if (temp >= 60 && multiplier < 3600000) return BORK_TIME;
			value += temp * multiplier;
		}
		if (ptr == input) break;
		ptr -= 2;
		while (ptr > input && *ptr != ':') --ptr;
		multiplier *= 60;
	}

	return value;
}

static void print_time_crap(int ms, char *out)
{
	char frac[8];
	int i,h,m,s;
	if (ms % 1000)
	{
		sprintf(frac, ".%3.3d", ms % 1000);
		for (i = 3; i > 0; i--)
			if (frac[i] == '0') frac[i] = 0;
		if (!frac[1]) frac[0] = 0;
	}
	else
		frac[0] = 0;
	h = ms / (60*60*1000);
	m = (ms % (60*60*1000)) / (60*1000);
	s = (ms % (60*1000)) / 1000;
	if (h) sprintf(out, "%d:%2.2d:%2.2d%s",h,m,s,frac);
	else if (m) sprintf(out, "%d:%2.2d%s",m,s,frac);
	else sprintf(out, "%d%s",s,frac);
}

static void info_meta_add(file_info & info, const char * tag, pfc::ptr_list_t< const char > const& values)
{
	t_size count = info.meta_get_count_by_name( tag );
	if ( count )
	{
		// append as another line
		pfc::string8 final = info.meta_get(tag, count - 1);
		final += "\r\n";
		final += values[0];
		info.meta_modify_value( info.meta_find( tag ), count - 1, final );
	}
	else
	{
		info.meta_add(tag, values[0]);
	}
	for ( count = 1; count < values.get_count(); count++ )
	{
		info.meta_add( tag, values[count] );
	}
}

static void info_meta_ansi( file_info & info )
{
	for ( unsigned i = 0, j = info.meta_get_count(); i < j; i++ )
	{
		for ( unsigned k = 0, l = info.meta_enum_value_count( i ); k < l; k++ )
		{
			const char * value = info.meta_enum_value( i, k );
			info.meta_modify_value( i, k, pfc::stringcvt::string_utf8_from_ansi( value ) );
		}
	}
	for ( unsigned i = 0, j = info.info_get_count(); i < j; i++ )
	{
		const char * name = info.info_enum_name( i );
		if ( name[ 0 ] == '_' )
			info.info_set( pfc::string8( name ), pfc::stringcvt::string_utf8_from_ansi( info.info_enum_value( i ) ) );
	}
}

static int find_crlf(pfc::string8 & blah)
{
	int pos = blah.find_first('\r');
	if (pos >= 0 && *(blah.get_ptr()+pos+1) == '\n') return pos;
	return -1;
}

static const char * fields_to_split[] = {"ARTIST", "ALBUM ARTIST", "PRODUCER", "COMPOSER", "PERFORMER", "GENRE"};

static bool meta_split_value( const char * tag )
{
	for ( unsigned i = 0; i < _countof( fields_to_split ); i++ )
	{
		if ( !stricmp_utf8( tag, fields_to_split[ i ] ) ) return true;
	}
	return false;
}

static void info_meta_write(pfc::string_base & tag, const file_info & info, const char * name, int idx, int & first)
{
	pfc::string8 v = info.meta_enum_value(idx, 0);
	if (meta_split_value(name))
	{
		t_size count = info.meta_enum_value_count(idx);
		for (t_size i = 1; i < count; i++)
		{
			v += "; ";
			v += info.meta_enum_value(idx, i);
		}
	}

	int pos = find_crlf(v);

	if (pos == -1)
	{
		if (first) first = 0;
		else tag.add_byte('\n');
		tag += name;
		tag.add_byte('=');
		// r->write(v.c_str(), v.length());
		tag += v;
		return;
	}
	while (pos != -1)
	{
		pfc::string8 foo;
		foo = v;
		foo.truncate(pos);
		if (first) first = 0;
		else tag.add_byte('\n');
		tag += name;
		tag.add_byte('=');
		tag += foo;
		v = v.get_ptr() + pos + 2;
		pos = find_crlf(v);
	}
	if (v.length())
	{
		tag.add_byte('\n');
		tag += name;
		tag.add_byte('=');
		tag += v;
	}
}

struct psf_info_meta_state
{
	file_info * info;

	pfc::string8_fast name;

	bool utf8;

	int tag_song_ms;
	int tag_fade_ms;

	psf_info_meta_state()
		: info( 0 ), utf8( false ), tag_song_ms( 0 ), tag_fade_ms( 0 )
	{
	}
};

static int psf_info_meta(void * context, const char * name, const char * value)
{
	psf_info_meta_state * state = ( psf_info_meta_state * ) context;

	pfc::string8_fast & tag = state->name;

	tag = name;

	if (!stricmp_utf8(tag, "game"))
	{
		DBG("reading game as album");
		tag = "album";
	}
	else if (!stricmp_utf8(tag, "year"))
	{
		DBG("reading year as date");
		tag = "date";
	}

	if (!stricmp_utf8_partial(tag, "replaygain_"))
	{
		DBG("reading RG info");
		//info.info_set(tag, value);
		state->info->info_set_replaygain(tag, value);
	}
	else if (!stricmp_utf8(tag, "length"))
	{
		DBG("reading length");
		int temp = parse_time_crap(value);
		if (temp != BORK_TIME)
		{
			state->tag_song_ms = temp;
			state->info->info_set_int(field_length, state->tag_song_ms);
		}
	}
	else if (!stricmp_utf8(tag, "fade"))
	{
		DBG("reading fade");
		int temp = parse_time_crap(value);
		if (temp != BORK_TIME)
		{
			state->tag_fade_ms = temp;
			state->info->info_set_int(field_fade, state->tag_fade_ms);
		}
	}
	else if (!stricmp_utf8(tag, "utf8"))
	{
		state->utf8 = true;
	}
	else if (!stricmp_utf8_partial(tag, "_lib"))
	{
		DBG("found _lib");
		state->info->info_set(tag, value);
	}
	else if (tag[0] == '_')
	{
		DBG("found unknown required tag, failing");
		console::formatter() << "Unsupported tag found: " << tag << ", required to play file.";
		return -1;
	}
	else
	{
		state->info->meta_add( tag, value );
	}

	return 0;
}

struct snsf_loader_state
{
    int base_set;
    uint32_t base;
    uint8_t * data;
    size_t data_size;
    uint8_t * sram;
    size_t sram_size;
    snsf_loader_state() : base_set(0), data(0), data_size(0), sram(0), sram_size(0) { }
    ~snsf_loader_state() { if (data) free(data); if (sram) free(sram); }
};

inline unsigned get_le32( void const* p )
{
    return  (unsigned) ((unsigned char const*) p) [3] << 24 |
            (unsigned) ((unsigned char const*) p) [2] << 16 |
            (unsigned) ((unsigned char const*) p) [1] <<  8 |
            (unsigned) ((unsigned char const*) p) [0];
}

int snsf_loader(void * context, const uint8_t * exe, size_t exe_size,
                                  const uint8_t * reserved, size_t reserved_size)
{
    if ( exe_size < 8 ) return -1;

    struct snsf_loader_state * state = (struct snsf_loader_state *) context;

    unsigned char *iptr;
    unsigned isize;
    unsigned char *xptr;
    unsigned xofs = get_le32(exe + 0);
    unsigned xsize = get_le32(exe + 4);
    if ( xsize > exe_size - 8 ) return -1;
    if (!state->base_set)
    {
        state->base = xofs;
        state->base_set = 1;
    }
    else
    {
        xofs += state->base;
    }
    {
        iptr = state->data;
        isize = state->data_size;
        state->data = 0;
        state->data_size = 0;
    }
    if (!iptr)
    {
        unsigned rsize = xofs + xsize;
        {
            rsize -= 1;
            rsize |= rsize >> 1;
            rsize |= rsize >> 2;
            rsize |= rsize >> 4;
            rsize |= rsize >> 8;
            rsize |= rsize >> 16;
            rsize += 1;
        }
        iptr = (unsigned char *) malloc(rsize + 10);
        if (!iptr)
            return -1;
        memset(iptr, 0, rsize + 10);
        isize = rsize;
    }
    else if (isize < xofs + xsize)
    {
        unsigned rsize = xofs + xsize;
        {
            rsize -= 1;
            rsize |= rsize >> 1;
            rsize |= rsize >> 2;
            rsize |= rsize >> 4;
            rsize |= rsize >> 8;
            rsize |= rsize >> 16;
            rsize += 1;
        }
        xptr = (unsigned char *) realloc(iptr, xofs + rsize + 10);
        if (!xptr)
        {
            free(iptr);
            return -1;
        }
        iptr = xptr;
        isize = rsize;
    }
    memcpy(iptr + xofs, exe + 8, xsize);
    {
        state->data = iptr;
        state->data_size = isize;
    }

    // reserved section
    if (reserved_size >= 8)
    {
        unsigned rsvtype = get_le32(reserved + 0);
        unsigned rsvsize = get_le32(reserved + 4);

        if (rsvtype == 0)
        {
            // SRAM block
            if (reserved_size < 12 || rsvsize < 4)
            {
                DBG("Reserve section (SRAM) is too short");
                return -1;
            }

            // check offset and size
            unsigned sram_offset = get_le32(reserved + 8);
            unsigned sram_patch_size = rsvsize - 4;
            if (sram_offset + sram_patch_size > 0x20000)
            {
                DBG("SRAM size error");
                return -1;
            }

            if (!state->sram)
            {
                state->sram = (unsigned char *) malloc(0x20000);
                if (!state->sram)
                    return -1;
                memset(state->sram, 0, 0x20000);
            }

            // load SRAM data
            memcpy(state->sram + sram_offset, reserved + 12, sram_patch_size);

            // update SRAM size
            if (state->sram_size < sram_offset + sram_patch_size)
            {
                state->sram_size = sram_offset + sram_patch_size;
            }
        }
        else
        {
            DBG("Unsupported reserve section type");
            return -1;
        }
    }

    return 0;
}

struct snsf_sound_out : public SNESSoundOut
{
    unsigned long bytes_in_buffer;
    pfc::array_t<uint8_t> sample_buffer;
    virtual ~snsf_sound_out() { }
    // Receives signed 16-bit stereo audio and a byte count
    virtual void write(const void * samples, unsigned long bytes)
    {
        sample_buffer.grow_size( bytes_in_buffer + bytes );
        memcpy( &sample_buffer[ bytes_in_buffer ], samples, bytes );
        bytes_in_buffer += bytes;
    }
};

static class psf_file_container
{
	critical_section lock;

	struct psf_file_opened
	{
		pfc::string_simple path;
		file::ptr f;

		psf_file_opened() { }

		psf_file_opened( const char * _p )
			: path( _p ) { }

		psf_file_opened( const char * _p, file::ptr _f )
			: path( _p ), f( _f ) { }

		bool operator== ( psf_file_opened const& in ) const
		{
			return !strcmp( path, in.path );
		}
	};

	pfc::list_t<psf_file_opened> hints;

public:
	void add_hint( const char * path, file::ptr f )
	{
		insync( lock );
		hints.add_item( psf_file_opened( path, f ) );
	}

	void remove_hint( const char * path )
	{
		insync( lock );
		hints.remove_item( psf_file_opened( path ) );
	}

	bool try_hint( const char * path, file::ptr & out )
	{
		insync( lock );
		t_size index = hints.find_item( psf_file_opened( path ) );
		if ( index == ~0 ) return false;
		out = hints[ index ].f;
		out->reopen( abort_callback_dummy() );
		return true;
	}
} g_hint_list;

struct psf_file_state
{
	file::ptr f;
};

static void * psf_file_fopen( const char * uri )
{
	try
	{
		psf_file_state * state = new psf_file_state;
		if ( !g_hint_list.try_hint( uri, state->f ) )
			filesystem::g_open( state->f, uri, filesystem::open_mode_read, abort_callback_dummy() );
		return state;
	}
	catch (...)
	{
		return NULL;
	}
}

static size_t psf_file_fread( void * buffer, size_t size, size_t count, void * handle )
{
	try
	{
		psf_file_state * state = ( psf_file_state * ) handle;
		size_t bytes_read = state->f->read( buffer, size * count, abort_callback_dummy() );
		return bytes_read / size;
	}
	catch (...)
	{
		return 0;
	}
}

static int psf_file_fseek( void * handle, int64_t offset, int whence )
{
	try
	{
		psf_file_state * state = ( psf_file_state * ) handle;
		state->f->seek_ex( offset, (foobar2000_io::file::t_seek_mode) whence, abort_callback_dummy() );
		return 0;
	}
	catch (...)
	{
		return -1;
	}
}

static int psf_file_fclose( void * handle )
{
	try
	{
		psf_file_state * state = ( psf_file_state * ) handle;
		delete state;
		return 0;
	}
	catch (...)
	{
		return -1;
	}
}

static long psf_file_ftell( void * handle )
{
	try
	{
		psf_file_state * state = ( psf_file_state * ) handle;
		return state->f->get_position( abort_callback_dummy() );
	}
	catch (...)
	{
		return -1;
	}
}

const psf_file_callbacks psf_file_system =
{
	"\\/|:",
	psf_file_fopen,
	psf_file_fread,
	psf_file_fseek,
	psf_file_fclose,
	psf_file_ftell
};

class input_snsf9x
{
	bool no_loop, eof;

	circular_buffer<t_int16> silence_test_buffer;
	pfc::array_t<t_int16> sample_buffer;

	snsf_loader_state m_rom;

	snsf_sound_out m_output;

	SNESSystem * m_system;

	service_ptr_t<file> m_file;

	pfc::string8 m_path;

	int err;

	int data_written,remainder,pos_delta,startsilence,silence;

	double snsfemu_pos;

	int song_len,fade_len;
	int tag_song_ms,tag_fade_ms;

	file_info_impl m_info;

	bool do_filter, do_suppressendsilence;

public:
	input_snsf9x() : silence_test_buffer(0), m_system(0)
	{
	}

	~input_snsf9x()
	{
		g_hint_list.remove_hint( m_path );

		shutdown();
	}

	void shutdown()
	{
		if ( m_system )
		{
			m_system->Term();

			delete m_system;
			m_system = NULL;
		}
	}

	void open( service_ptr_t<file> p_file, const char * p_path, t_input_open_reason p_reason, abort_callback & p_abort )
	{
		input_open_file_helper( p_file, p_path, p_reason, p_abort );

		m_path = p_path;
		g_hint_list.add_hint( p_path, p_file );

		psf_info_meta_state info_state;
		info_state.info = &m_info;

		if ( psf_load( p_path, &psf_file_system, 0x23, 0, 0, psf_info_meta, &info_state, 0 ) <= 0 )
			throw exception_io_data( "Not a SNSF file" );

		if ( !info_state.utf8 )
			info_meta_ansi( m_info );

		tag_song_ms = info_state.tag_song_ms;
		tag_fade_ms = info_state.tag_fade_ms;

		if (!tag_song_ms)
		{
			tag_song_ms = cfg_deflength;
			tag_fade_ms = cfg_deffade;
		}

		m_info.set_length( (double)( tag_song_ms + tag_fade_ms ) * .001 );
		m_info.info_set_int( "samplerate", 32000 );
		m_info.info_set_int( "channels", 2 );

		m_file = p_file;
	}

	void get_info( file_info & p_info, abort_callback & p_abort )
	{
		p_info.copy( m_info );
	}

	t_filestats get_file_stats( abort_callback & p_abort )
	{
		return m_file->get_stats( p_abort );
	}

	void decode_initialize( unsigned p_flags, abort_callback & p_abort )
	{
		shutdown();

		m_system = new SNESSystem;

		if ( !m_rom.data )
		{
			if ( psf_load( m_path, &psf_file_system, 0x23, snsf_loader, &m_rom, 0, 0, 0 ) < 0 )
				throw exception_io_data( "Invalid SNSF" );
		}

		m_system->Load( m_rom.data, m_rom.data_size, m_rom.sram, m_rom.sram_size );

		m_system->soundSampleRate = 32000;

		m_system->SoundInit( &m_output );
		m_system->SoundReset();

		m_system->Init();
		m_system->Reset();

		snsfemu_pos = 0.;

		startsilence = silence = 0;

		eof = 0;
		err = 0;
		data_written = 0;
		remainder = 0;
		pos_delta = 0;
		snsfemu_pos = 0;
		no_loop = ( p_flags & input_flag_no_looping ) || !cfg_infinite;

		calcfade();

		do_suppressendsilence = !! cfg_suppressendsilence;

		unsigned skip_max = cfg_endsilenceseconds * 32000;

		if ( cfg_suppressopeningsilence ) // ohcrap
		{
			for (;;)
			{
				p_abort.check();

				unsigned skip_howmany = skip_max - silence;
				unsigned unskippable = 0;
				m_output.bytes_in_buffer = 0;
				m_system->soundEnableFlag = (uint8_t)cfg_mutedchannels ^ 0xff;
				m_system->CPULoop();
				if ( skip_howmany > m_output.bytes_in_buffer / 4 )
					skip_howmany = m_output.bytes_in_buffer / 4;
				else
					unskippable = ( m_output.bytes_in_buffer / 4 ) - skip_howmany;
				short * foo = ( short * ) m_output.sample_buffer.get_ptr();
				unsigned i;
				for ( i = 0; i < skip_howmany; ++i )
				{
					if ( foo[ 0 ] || foo[ 1 ] ) break;
					foo += 2;
				}
				silence += i;
				if ( i < skip_howmany )
				{
					remainder = skip_howmany - i + unskippable;
					memmove( m_output.sample_buffer.get_ptr(), foo, remainder * sizeof( short ) * 2 );
					break;
				}
				if ( silence >= skip_max )
				{
					eof = true;
					break;
				}
			}

			startsilence += silence;
			silence = 0;
		}

		if ( do_suppressendsilence ) silence_test_buffer.resize( skip_max * 2 );
	}

	bool decode_run( audio_chunk & p_chunk, abort_callback & p_abort )
	{
		p_abort.check();

		if ( ( eof || err < 0 ) && !silence_test_buffer.data_available() ) return false;

		if ( no_loop && tag_song_ms && ( pos_delta + MulDiv( data_written, 1000, 32000 ) ) >= tag_song_ms + tag_fade_ms )
			return false;

		UINT written = 0;

		int samples;

		if ( no_loop )
		{
			samples = ( song_len + fade_len ) - data_written;
			if ( samples > 1024 ) samples = 1024;
		}
		else
		{
			samples = 1024;
		}

		short * ptr;

		if ( do_suppressendsilence )
		{
			if ( !eof )
			{
				unsigned free_space = silence_test_buffer.free_space() / 2;
				while ( free_space )
				{
					p_abort.check();

					unsigned samples_to_render;
					if ( remainder )
					{
						samples_to_render = remainder;
						remainder = 0;
					}
					else
					{
						samples_to_render = free_space;
						m_output.bytes_in_buffer = 0;
						m_system->soundEnableFlag = (uint8_t)cfg_mutedchannels ^ 0xff;
						m_system->CPULoop();
						samples_to_render = m_output.bytes_in_buffer / 4;
						if ( samples_to_render > free_space )
						{
							remainder = samples_to_render - free_space;
							samples_to_render = free_space;
						}
					}
					silence_test_buffer.write( (short *) m_output.sample_buffer.get_ptr(), samples_to_render * 2 );
					free_space -= samples_to_render;
					if ( remainder )
					{
						memmove( m_output.sample_buffer.get_ptr(), ((short *) m_output.sample_buffer.get_ptr()) + samples_to_render * 2, remainder * 4 );
					}
				}
			}

			if ( silence_test_buffer.test_silence() )
			{
				eof = true;
				return false;
			}

			written = silence_test_buffer.data_available() / 2;
			if ( written > 1024 ) written = 1024;
			sample_buffer.grow_size( written * 2 );
			silence_test_buffer.read( sample_buffer.get_ptr(), written * 2 );
			ptr = sample_buffer.get_ptr();
		}
		else
		{
			if ( remainder )
			{
				written = remainder;
				remainder = 0;
			}
			else
			{
				m_output.bytes_in_buffer = 0;
				m_system->soundEnableFlag = (uint8_t)cfg_mutedchannels ^ 0xff;
				m_system->CPULoop();
				written = m_output.bytes_in_buffer / 4;
			}

			ptr = (short *) m_output.sample_buffer.get_ptr();
		}

		snsfemu_pos += double(written) / 32000.;

		int d_start, d_end;
		d_start = data_written;
		data_written += written;
		d_end = data_written;

		if ( tag_song_ms && d_end > song_len && no_loop )
		{
			short * foo = sample_buffer.get_ptr();
			int n;
			for( n = d_start; n < d_end; ++n )
			{
				if ( n > song_len )
				{
					if ( n > song_len + fade_len )
					{
						* ( DWORD * ) foo = 0;
					}
					else
					{
						int bleh = song_len + fade_len - n;
						foo[ 0 ] = MulDiv( foo[ 0 ], bleh, fade_len );
						foo[ 1 ] = MulDiv( foo[ 1 ], bleh, fade_len );
					}
				}
				foo += 2;
			}
		}

		p_chunk.set_data_fixedpoint( ptr, written * 4, 32000, 2, 16, audio_chunk::channel_config_stereo );

		return true;
	}

	void decode_seek( double p_seconds, abort_callback & p_abort )
	{
		eof = false;

		double buffered_time = (double)(silence_test_buffer.data_available() / 2) / 32000.0;

		snsfemu_pos += buffered_time;

		silence_test_buffer.reset();

		if ( p_seconds < snsfemu_pos )
		{
			decode_initialize( no_loop ? input_flag_no_looping : 0, p_abort );
		}
		unsigned int howmany = ( int )( audio_math::time_to_samples( p_seconds - snsfemu_pos, 32000 ) );

		// more abortable, and emu doesn't like doing huge numbers of samples per call anyway
		while ( howmany )
		{
			p_abort.check();

			m_output.bytes_in_buffer = 0;
			m_system->soundEnableFlag = (uint8_t)cfg_mutedchannels ^ 0xff;
			m_system->CPULoop();
			unsigned samples = m_output.bytes_in_buffer / 4;
			if ( samples > howmany )
			{
				memmove( m_output.sample_buffer.get_ptr(), ((short *) m_output.sample_buffer.get_ptr()) + howmany * 2, ( samples - howmany ) * 4 );
				remainder = samples - howmany;
				samples = howmany;
			}
			howmany -= samples;
		}

		data_written = 0;
		pos_delta = ( int )( p_seconds * 1000. );
		snsfemu_pos = p_seconds;

		calcfade();
	}

	bool decode_can_seek()
	{
		return true;
	}

	bool decode_get_dynamic_info( file_info & p_out, double & p_timestamp_delta )
	{
		return false;
	}

	bool decode_get_dynamic_info_track( file_info & p_out, double & p_timestamp_delta )
	{
		return false;
	}

	void decode_on_idle( abort_callback & p_abort )
	{
	}

	void retag( const file_info & p_info, abort_callback & p_abort )
	{
		m_info.copy( p_info );

		pfc::array_t<t_uint8> buffer;
		buffer.set_size( 16 );

		m_file->seek( 0, p_abort );

		BYTE *ptr = buffer.get_ptr();
		m_file->read_object( ptr, 16, p_abort );
		if (ptr[0] != 'P' || ptr[1] != 'S' || ptr[2] != 'F' ||
			ptr[3] != 0x22) throw exception_io_data();
		int reserved_size = pfc::byteswap_if_be_t( ((unsigned long*)ptr)[1] );
		int exe_size = pfc::byteswap_if_be_t( ((unsigned long*)ptr)[2] );
		m_file->seek(16 + reserved_size + exe_size, p_abort);
		m_file->set_eof(p_abort);

		pfc::string8 tag = "[TAG]utf8=1\n";

		int first = 1;
		// _lib and _refresh tags first
		int n, p = p_info.info_get_count();
		for (n = 0; n < p; n++)
		{
			const char *t = p_info.info_enum_name(n);
			if (*t == '_')
			{
				if (first) first = 0;
				else tag.add_byte('\n');
				tag += t;
				tag.add_byte('=');
				tag += p_info.info_enum_value(n);
			}
		}
		// Then info
		p = p_info.meta_get_count();
		for (n = 0; n < p; n++)
		{
			const char * t = p_info.meta_enum_name(n);
			if (*t == '_' ||
				!stricmp(t, "length") ||
				!stricmp(t, "fade")) continue; // dummy protection
			if (!stricmp(t, "album")) info_meta_write(tag, p_info, "game", n, first);
			else if (!stricmp(t, "date"))
			{
				const char * val = p_info.meta_enum_value(n, 0);
				char * end;
				strtoul(p_info.meta_enum_value(n, 0), &end, 10);
				if (size_t(end - val) < strlen(val))
					info_meta_write(tag, p_info, t, n, first);
				else
					info_meta_write(tag, p_info, "year", n, first);
			}
			else info_meta_write(tag, p_info, t, n, first);
		}
		// Then time and fade
		{
			int tag_song_ms = 0, tag_fade_ms = 0;
			const char *t = p_info.info_get(field_length);
			if (t)
			{
				char temp[16];
				tag_song_ms = atoi(t);
				if (first) first = 0;
				else tag.add_byte('\n');
				tag += "length=";
				print_time_crap(tag_song_ms, temp);
				tag += temp;
				t = p_info.info_get(field_fade);
				if (t)
				{
					tag_fade_ms = atoi(t);
					tag.add_byte('\n');
					tag += "fade=";
					print_time_crap(tag_fade_ms, (char *)&temp);
					tag += temp;
				}
			}
		}

		// Then ReplayGain
		/*
		p = p_info.info_get_count();
		for (n = 0; n < p; n++)
		{
			const char *t = p_info.info_enum_name(n);
			if (!strnicmp(t, "replaygain_",11))
			{
				if (first) first = 0;
				else tag.add_byte('\n');
				tag += t;
				else tag.add_byte('=');
				tag += p_info.info_enum_value(n);
			}
		}
		*/
		replaygain_info rg = p_info.get_replaygain();
		char rgbuf[replaygain_info::text_buffer_size];
		if (rg.is_track_gain_present())
		{
			rg.format_track_gain(rgbuf);
			if (first) first = 0;
			else tag.add_byte('\n');
			tag += "replaygain_track_gain";
			tag.add_byte('=');
			tag += rgbuf;
		}
		if (rg.is_track_peak_present())
		{
			rg.format_track_peak(rgbuf);
			if (first) first = 0;
			else tag.add_byte('\n');
			tag += "replaygain_track_peak";
			tag.add_byte('=');
			tag += rgbuf;
		}
		if (rg.is_album_gain_present())
		{
			rg.format_album_gain(rgbuf);
			if (first) first = 0;
			else tag.add_byte('\n');
			tag += "replaygain_album_gain";
			tag.add_byte('=');
			tag += rgbuf;
		}
		if (rg.is_album_peak_present())
		{
			rg.format_album_peak(rgbuf);
			if (first) first = 0;
			else tag.add_byte('\n');
			tag += "replaygain_album_peak";
			tag.add_byte('=');
			tag += rgbuf;
		}

		m_file->write_object( tag.get_ptr(), tag.length(), p_abort );
	}

	static bool g_is_our_content_type( const char * p_content_type )
	{
		return false;
	}

	static bool g_is_our_path( const char * p_full_path, const char * p_extension )
	{
		return (!stricmp(p_extension,"snsf") || !stricmp(p_extension,"minisnsf"));
	}

private:
	void calcfade()
	{
		song_len=MulDiv(tag_song_ms-pos_delta,32000,1000);
		fade_len=MulDiv(tag_fade_ms,32000,1000);
	}
};

class CMyPreferences : public CDialogImpl<CMyPreferences>, public preferences_page_instance {
public:
	//Constructor - invoked by preferences_page_impl helpers - don't do Create() in here, preferences_page_impl does this for us
	CMyPreferences(preferences_page_callback::ptr callback) : m_callback(callback) {}

	//Note that we don't bother doing anything regarding destruction of our class.
	//The host ensures that our dialog is destroyed first, then the last reference to our preferences_page_instance object is released, causing our object to be deleted.


	//dialog resource ID
	enum {IDD = IDD_PSF_CONFIG};
	// preferences_page_instance methods (not all of them - get_wnd() is supplied by preferences_page_impl helpers)
	t_uint32 get_state();
	void apply();
	void reset();

	//WTL message map
	BEGIN_MSG_MAP(CMyPreferences)
		MSG_WM_INITDIALOG(OnInitDialog)
		COMMAND_HANDLER_EX(IDC_INDEFINITE, BN_CLICKED, OnButtonClick)
		COMMAND_HANDLER_EX(IDC_SOS, BN_CLICKED, OnButtonClick)
		COMMAND_HANDLER_EX(IDC_SES, BN_CLICKED, OnButtonClick)
		COMMAND_HANDLER_EX(IDC_SILENCE, EN_CHANGE, OnEditChange)
		COMMAND_HANDLER_EX(IDC_DLENGTH, EN_CHANGE, OnEditChange)
		COMMAND_HANDLER_EX(IDC_DFADE, EN_CHANGE, OnEditChange)
		COMMAND_HANDLER_EX(IDC_MUTED_CHANNELS, LBN_SELCHANGE, OnListSelChange)
	END_MSG_MAP()
private:
	BOOL OnInitDialog(CWindow, LPARAM);
	void OnEditChange(UINT, int, CWindow);
	void OnButtonClick(UINT, int, CWindow);
	void OnListSelChange(UINT, int, CWindow);
	bool HasChanged();
	void OnChanged();

	const preferences_page_callback::ptr m_callback;

	CHyperLink m_link_github;
	CHyperLink m_link_kode54;
};

BOOL CMyPreferences::OnInitDialog(CWindow, LPARAM) {
	SendDlgItemMessage( IDC_INDEFINITE, BM_SETCHECK, cfg_infinite );
	SendDlgItemMessage( IDC_SOS, BM_SETCHECK, cfg_suppressopeningsilence );
	SendDlgItemMessage( IDC_SES, BM_SETCHECK, cfg_suppressendsilence );
	
	SetDlgItemInt( IDC_SILENCE, cfg_endsilenceseconds, FALSE );
	
	{
		char temp[16];
		// wsprintf((char *)&temp, "= %d Hz", 33868800 / cfg_divider);
		// SetDlgItemText(wnd, IDC_HZ, (char *)&temp);
		
		print_time_crap( cfg_deflength, (char *)&temp );
		uSetDlgItemText( m_hWnd, IDC_DLENGTH, (char *)&temp );
		
		print_time_crap( cfg_deffade, (char *)&temp );
		uSetDlgItemText( m_hWnd, IDC_DFADE, (char *)&temp );
	}
	
	m_link_github.SetLabel( _T( "snsf9x github page" ) );
	m_link_github.SetHyperLink( _T( "https://github.com/loveemu/snsf9x" ) );
	m_link_github.SubclassWindow( GetDlgItem( IDC_URL ) );
	
	m_link_kode54.SetLabel( _T( "kode's foobar2000 plug-ins" ) );
	m_link_kode54.SetHyperLink( _T( "http://kode54.foobar2000.org/" ) );
	m_link_kode54.SubclassWindow( GetDlgItem( IDC_K54 ) );
	
	{
		/*OSVERSIONINFO ovi = { 0 };
		ovi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
		BOOL bRet = ::GetVersionEx(&ovi);
		if ( bRet && ( ovi.dwMajorVersion >= 5 ) )*/
		{
			DWORD color = GetSysColor( 26 /* COLOR_HOTLIGHT */ );
			m_link_github.m_clrLink = color;
			m_link_github.m_clrVisited = color;
			m_link_kode54.m_clrLink = color;
			m_link_kode54.m_clrVisited = color;
		}
	}
	
	SendDlgItemMessage(IDC_MUTED_CHANNELS, LB_ADDSTRING, 0, (LPARAM)L"BRRPCM 1");
	SendDlgItemMessage(IDC_MUTED_CHANNELS, LB_ADDSTRING, 0, (LPARAM)L"BRRPCM 2");
	SendDlgItemMessage(IDC_MUTED_CHANNELS, LB_ADDSTRING, 0, (LPARAM)L"BRRPCM 3");
	SendDlgItemMessage(IDC_MUTED_CHANNELS, LB_ADDSTRING, 0, (LPARAM)L"BRRPCM 4");
	SendDlgItemMessage(IDC_MUTED_CHANNELS, LB_ADDSTRING, 0, (LPARAM)L"BRRPCM 5");
	SendDlgItemMessage(IDC_MUTED_CHANNELS, LB_ADDSTRING, 0, (LPARAM)L"BRRPCM 6");
	SendDlgItemMessage(IDC_MUTED_CHANNELS, LB_ADDSTRING, 0, (LPARAM)L"BRRPCM 7");
	SendDlgItemMessage(IDC_MUTED_CHANNELS, LB_ADDSTRING, 0, (LPARAM)L"BRRPCM 8");
	
	return FALSE;
}

void CMyPreferences::OnEditChange(UINT, int, CWindow) {
	OnChanged();
}

void CMyPreferences::OnButtonClick(UINT, int, CWindow) {
	OnChanged();
}

void CMyPreferences::OnListSelChange(UINT, int id, CWindow wnd) {
	switch (id)
	{
	case IDC_MUTED_CHANNELS:
		{
			unsigned long muted_channels = 0;

			int cnt = (int)(INT_PTR)SendDlgItemMessage(IDC_MUTED_CHANNELS, LB_GETCOUNT);
			for (int ch = 0; ch < cnt; ch++)
			{
				bool mute = (INT_PTR)SendDlgItemMessage(IDC_MUTED_CHANNELS, LB_GETSEL, ch) > 0;
				if (mute)
				{
					muted_channels |= 1 << ch;
				}
			}

			cfg_mutedchannels = muted_channels;
		}
		break;
	}
}

t_uint32 CMyPreferences::get_state() {
	t_uint32 state = preferences_state::resettable;
	if (HasChanged()) state |= preferences_state::changed;
	return state;
}

void CMyPreferences::reset() {
	char temp[16];
	SendDlgItemMessage( IDC_INDEFINITE, BM_SETCHECK, default_cfg_infinite );
	SendDlgItemMessage( IDC_SOS, BM_SETCHECK, default_cfg_suppressopeningsilence );
	SendDlgItemMessage( IDC_SES, BM_SETCHECK, default_cfg_suppressendsilence );
	SetDlgItemInt( IDC_SILENCE, default_cfg_endsilenceseconds, FALSE );
	print_time_crap( default_cfg_deflength, (char *)&temp );
	uSetDlgItemText( m_hWnd, IDC_DLENGTH, (char *)&temp );
	print_time_crap( default_cfg_deffade, (char *)&temp );
	uSetDlgItemText( m_hWnd, IDC_DFADE, (char *)&temp );

	OnChanged();
}

void CMyPreferences::apply() {
	int t;
	char temp[16];
	cfg_infinite = SendDlgItemMessage( IDC_INDEFINITE, BM_GETCHECK );
	cfg_suppressopeningsilence = SendDlgItemMessage( IDC_SOS, BM_GETCHECK );
	cfg_suppressendsilence = SendDlgItemMessage( IDC_SES, BM_GETCHECK );
	t = GetDlgItemInt( IDC_SILENCE, NULL, FALSE );
	if ( t > 0 ) cfg_endsilenceseconds = t;
	SetDlgItemInt( IDC_SILENCE, cfg_endsilenceseconds, FALSE );
	t = parse_time_crap( string_utf8_from_window( GetDlgItem( IDC_DLENGTH ) ) );
	if ( t != BORK_TIME ) cfg_deflength = t;
	else
	{
		print_time_crap( cfg_deflength, (char *)&temp );
		uSetDlgItemText( m_hWnd, IDC_DLENGTH, (char *)&temp );
	}
	t = parse_time_crap( string_utf8_from_window( GetDlgItem( IDC_DFADE ) ) );
	if ( t != BORK_TIME ) cfg_deffade = t;
	else
	{
		print_time_crap( cfg_deffade, (char *)&temp );
		uSetDlgItemText( m_hWnd, IDC_DFADE, (char *)&temp );
	}
	
	OnChanged(); //our dialog content has not changed but the flags have - our currently shown values now match the settings so the apply button can be disabled
}

bool CMyPreferences::HasChanged() {
	//returns whether our dialog content is different from the current configuration (whether the apply button should be enabled or not)
	bool changed = false;
	if ( !changed && SendDlgItemMessage( IDC_INDEFINITE, BM_GETCHECK ) != cfg_infinite ) changed = true;
	if ( !changed && SendDlgItemMessage( IDC_SOS, BM_GETCHECK ) != cfg_suppressopeningsilence ) changed = true;
	if ( !changed && SendDlgItemMessage( IDC_SES, BM_GETCHECK ) != cfg_suppressendsilence ) changed = true;
	if ( !changed && GetDlgItemInt( IDC_SILENCE, NULL, FALSE ) != cfg_endsilenceseconds ) changed = true;
	if ( !changed )
	{
		int t = parse_time_crap( string_utf8_from_window( GetDlgItem( IDC_DLENGTH ) ) );
		if ( t != BORK_TIME && t != cfg_deflength ) changed = true;
	}
	if ( !changed )
	{
		int t = parse_time_crap( string_utf8_from_window( GetDlgItem( IDC_DFADE ) ) );
		if ( t != BORK_TIME && t != cfg_deffade ) changed = true;
	}
	return changed;
}
void CMyPreferences::OnChanged() {
	//tell the host that our state has changed to enable/disable the apply button appropriately.
	m_callback->on_state_changed();
}

class preferences_page_myimpl : public preferences_page_impl<CMyPreferences> {
	// preferences_page_impl<> helper deals with instantiation of our dialog; inherits from preferences_page_v3.
public:
	const char * get_name() {return "SNSF Decoder";}
	GUID get_guid() {
		// {85D573D2-B380-49DF-9209-F07C0B1E7455}
		static const GUID guid = { 0x85d573d2, 0xb380, 0x49df, { 0x92, 0x9, 0xf0, 0x7c, 0xb, 0x1e, 0x74, 0x55 } };
		return guid;
	}
	GUID get_parent_guid() {return guid_input;}
};

typedef struct
{
	unsigned song, fade;
} INFOSTRUCT;

static BOOL CALLBACK TimeProc(HWND wnd,UINT msg,WPARAM wp,LPARAM lp)
{
	switch(msg)
	{
	case WM_INITDIALOG:
		uSetWindowLong(wnd,DWL_USER,lp);
		{
			INFOSTRUCT * i=(INFOSTRUCT*)lp;
			char temp[16];
			if (!i->song && !i->fade) uSetWindowText(wnd, "Set length");
			else uSetWindowText(wnd, "Edit length");
			if ( i->song != ~0 )
			{
				print_time_crap(i->song, (char*)&temp);
				uSetDlgItemText(wnd, IDC_LENGTH, (char*)&temp);
			}
			if ( i->fade != ~0 )
			{
				print_time_crap(i->fade, (char*)&temp);
				uSetDlgItemText(wnd, IDC_FADE, (char*)&temp);
			}
		}
		cfg_placement.on_window_creation(wnd);
		return 1;
	case WM_COMMAND:
		switch(LOWORD(wp))
		{
		case IDOK:
			{
				INFOSTRUCT * i=(INFOSTRUCT*)uGetWindowLong(wnd,DWL_USER);
				int foo;
				foo = parse_time_crap(string_utf8_from_window(wnd, IDC_LENGTH));
				if (foo != BORK_TIME) i->song = foo;
				else i->song = ~0;
				foo = parse_time_crap(string_utf8_from_window(wnd, IDC_FADE));
				if (foo != BORK_TIME) i->fade = foo;
				else i->fade = ~0;
			}
			EndDialog(wnd,1);
			break;
		case IDCANCEL:
			EndDialog(wnd,0);
			break;
		}
		break;
	case WM_DESTROY:
		cfg_placement.on_window_destruction(wnd);
		break;
	}
	return 0;
}

static bool context_time_dialog(unsigned * song_ms, unsigned * fade_ms)
{
	bool ret;
	INFOSTRUCT * i = new INFOSTRUCT;
	if (!i) return 0;
	i->song = *song_ms;
	i->fade = *fade_ms;
	HWND hwnd = core_api::get_main_window();
	ret = uDialogBox(IDD_TIME, hwnd, TimeProc, (long)i) > 0;
	if (ret)
	{
		*song_ms = i->song;
		*fade_ms = i->fade;
	}
	delete i;
	return ret;
}

class length_info_filter : public file_info_filter
{
	bool set_length, set_fade;
	unsigned m_length, m_fade;

	metadb_handle_list m_handles;

public:
	length_info_filter( const pfc::list_base_const_t<metadb_handle_ptr> & p_list )
	{
		set_length = false;
		set_fade = false;

		pfc::array_t<t_size> order;
		order.set_size(p_list.get_count());
		order_helper::g_fill(order.get_ptr(),order.get_size());
		p_list.sort_get_permutation_t(pfc::compare_t<metadb_handle_ptr,metadb_handle_ptr>,order.get_ptr());
		m_handles.set_count(order.get_size());
		for(t_size n = 0; n < order.get_size(); n++) {
			m_handles[n] = p_list[order[n]];
		}

	}

	void length( unsigned p_length )
	{
		set_length = true;
		m_length = p_length;
	}

	void fade( unsigned p_fade )
	{
		set_fade = true;
		m_fade = p_fade;
	}

	virtual bool apply_filter(metadb_handle_ptr p_location,t_filestats p_stats,file_info & p_info)
	{
		t_size index;
		if (m_handles.bsearch_t(pfc::compare_t<metadb_handle_ptr,metadb_handle_ptr>,p_location,index))
		{
			if ( set_length )
			{
				if ( m_length ) p_info.info_set_int( field_length, m_length );
				else p_info.info_remove( field_length );
			}
			if ( set_fade )
			{
				if ( m_fade ) p_info.info_set_int( field_fade, m_fade );
				else p_info.info_remove( field_fade );
			}
			return set_length | set_fade;
		}
		else
		{
			return false;
		}
	}
};

class context_snsf : public contextmenu_item_simple
{
public:
	virtual unsigned get_num_items() { return 1; }

	virtual void get_item_name(unsigned n, pfc::string_base & out)
	{
		if (n) uBugCheck();
		out = "Edit length";
	}

	/*virtual void get_item_default_path(unsigned n, pfc::string_base & out)
	{
		out.reset();
	}*/
	GUID get_parent() {return contextmenu_groups::tagging;}

	virtual bool get_item_description(unsigned n, pfc::string_base & out)
	{
		if (n) uBugCheck();
		out = "Edits the length of the selected SNSF file, or sets the length of all selected SNSF files.";
		return true;
	}

	virtual GUID get_item_guid(unsigned p_index)
	{
		if (p_index) uBugCheck();
		static const GUID guid = { 0xb7cbf0ab, 0xbd5d, 0x4c69, { 0x9b, 0x11, 0xfd, 0xf4, 0x58, 0xf8, 0xb4, 0x80 } };
		return guid;
	}

	virtual bool context_get_display(unsigned n,const pfc::list_base_const_t<metadb_handle_ptr> & data,pfc::string_base & out,unsigned & displayflags,const GUID &)
	{
		if (n) uBugCheck();
		unsigned i, j;
		i = data.get_count();
		for (j = 0; j < i; j++)
		{
			pfc::string_extension ext(data.get_item(j)->get_path());
			if (stricmp_utf8(ext, "SNSF") && stricmp_utf8(ext, "MINISNSF")) return false;
		}
		if (i == 1) out = "Edit length";
		else out = "Set length";
		return true;
	}

	virtual void context_command(unsigned n,const pfc::list_base_const_t<metadb_handle_ptr> & data,const GUID& caller)
	{
		if (n) uBugCheck();
		unsigned tag_song_ms = 0, tag_fade_ms = 0;
		unsigned i = data.get_count();
		file_info_impl info;
		abort_callback_impl m_abort;
		if (i == 1)
		{
			// fetch info from single file
			metadb_handle_ptr handle = data.get_item(0);
			handle->metadb_lock();
			const file_info * p_info;
			if (handle->get_info_locked(p_info) && p_info)
			{
				const char *t = p_info->info_get(field_length);
				if (t) tag_song_ms = atoi(t);
				t = p_info->info_get(field_fade);
				if (t) tag_fade_ms = atoi(t);
			}
			handle->metadb_unlock();
		}
		if (!context_time_dialog(&tag_song_ms, &tag_fade_ms)) return;
		static_api_ptr_t<metadb_io_v2> p_imgr;

		service_ptr_t<length_info_filter> p_filter = new service_impl_t< length_info_filter >( data );
		if ( tag_song_ms != ~0 ) p_filter->length( tag_song_ms );
		if ( tag_fade_ms != ~0 ) p_filter->fade( tag_fade_ms );

		p_imgr->update_info_async( data, p_filter, core_api::get_main_window(), 0, 0 );
	}
};

class version_snsf : public componentversion
{
public:
	virtual void get_file_name(pfc::string_base & out) { out = core_api::get_my_file_name(); }
	virtual void get_component_name(pfc::string_base & out) { out = "SNSF Decoder"; }
	virtual void get_component_version(pfc::string_base & out) { out = MYVERSION; }
	virtual void get_about_message(pfc::string_base & out)
	{
		out = "Foobar2000 version by kode54\nOriginal library by an unknown Japanese author." /*"\n\nCore: ";
		out += psx_getversion();
		out +=*/ "\n\nhttps://github.com/loveemu/snsf9x\nhttp://kode54.foobar2000.org/";
	}
};

DECLARE_FILE_TYPE( "SNSF files", "*.SNSF;*.MINISNSF" );

static input_singletrack_factory_t<input_snsf9x>            g_input_snsf_factory;
static preferences_page_factory_t <preferences_page_myimpl> g_config_snsf_factory;
static contextmenu_item_factory_t <context_snsf>            g_contextmenu_item_snsf_factory;
static service_factory_single_t   <version_snsf>            g_componentversion_snsf_factory;

VALIDATE_COMPONENT_FILENAME("foo_input_snsf9x.dll");
