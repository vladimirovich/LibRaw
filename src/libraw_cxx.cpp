/* -*- C++ -*-
 * File: libraw_cxx.cpp
 * Copyright 2008-2010 LibRaw LLC (info@libraw.org)
 * Created: Sat Mar  8 , 2008
 *
 * LibRaw C++ interface (implementation)

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of three licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

3. LibRaw Software License 27032010
   (See file LICENSE.LibRaw.pdf provided in LibRaw distribution archive for details).

 */

#include <math.h>
#include <errno.h>
#include <float.h>
#include <new>
#include <sys/types.h>
#include <sys/stat.h>
#ifndef WIN32
#include <netinet/in.h>
#else
#include <winsock2.h>
#endif
#define LIBRAW_LIBRARY_BUILD
#include "libraw/libraw.h"
#include "internal/defines.h"

#ifdef __cplusplus
extern "C" 
{
#endif
    void default_memory_callback(void *,const char *file,const char *where)
    {
        fprintf (stderr,"%s: Out of memory in %s\n", file?file:"unknown file", where);
    }

    void default_data_callback(void*,const char *file, const int offset)
    {
        if(offset < 0)
            fprintf (stderr,"%s: Unexpected end of file\n", file?file:"unknown file");
        else
            fprintf (stderr,"%s: data corrupted at %d\n",file?file:"unknown file",offset); 
    }
    const char *libraw_strerror(int e)
    {
        enum LibRaw_errors errorcode = (LibRaw_errors)e;
        switch(errorcode)
            {
            case        LIBRAW_SUCCESS:
                return "No error";
            case        LIBRAW_UNSPECIFIED_ERROR:
                return "Unspecified error";
            case        LIBRAW_FILE_UNSUPPORTED:
                return "Unsupported file format or not RAW file";
            case        LIBRAW_REQUEST_FOR_NONEXISTENT_IMAGE:
                return "Request for nonexisting image number";
            case        LIBRAW_OUT_OF_ORDER_CALL:
                return "Out of order call of libraw function";
            case    LIBRAW_NO_THUMBNAIL:
                return "No thumbnail in file";
            case    LIBRAW_UNSUPPORTED_THUMBNAIL:
                return "Unsupported thumbnail format";
            case    LIBRAW_UNSUFFICIENT_MEMORY:
                return "Unsufficient memory";
            case    LIBRAW_DATA_ERROR:
                return "Corrupted data or unexpected EOF";
            case    LIBRAW_IO_ERROR:
                return "Input/output error";
            case LIBRAW_CANCELLED_BY_CALLBACK:
                return "Cancelled by user callback";
            case LIBRAW_BAD_CROP:
                return "Bad crop box";
            default:
                return "Unknown error code";
        }
    }

#ifdef __cplusplus
}
#endif


const double LibRaw_constants::xyz_rgb[3][3] = 
{
    { 0.412453, 0.357580, 0.180423 },
    { 0.212671, 0.715160, 0.072169 },
    { 0.019334, 0.119193, 0.950227 } 
};

const float LibRaw_constants::d65_white[3] =  { 0.950456f, 1.0f, 1.088754f };

#define P1 imgdata.idata
#define S imgdata.sizes
#define O imgdata.params
#define C imgdata.color
#define T imgdata.thumbnail
#define IO libraw_internal_data.internal_output_params
#define ID libraw_internal_data.internal_data

#define EXCEPTION_HANDLER(e) do{                        \
        /* fprintf(stderr,"Exception %d caught\n",e);*/ \
        switch(e)                                       \
            {                                           \
            case LIBRAW_EXCEPTION_ALLOC:                \
                recycle();                              \
                return LIBRAW_UNSUFFICIENT_MEMORY;      \
            case LIBRAW_EXCEPTION_DECODE_RAW:           \
            case LIBRAW_EXCEPTION_DECODE_JPEG:          \
                recycle();                              \
                return LIBRAW_DATA_ERROR;               \
            case LIBRAW_EXCEPTION_DECODE_JPEG2000:      \
                recycle();                              \
                return LIBRAW_DATA_ERROR;               \
            case LIBRAW_EXCEPTION_IO_EOF:               \
            case LIBRAW_EXCEPTION_IO_CORRUPT:           \
                recycle();                              \
                return LIBRAW_IO_ERROR;                 \
            case LIBRAW_EXCEPTION_CANCELLED_BY_CALLBACK:\
                recycle();                              \
                return LIBRAW_CANCELLED_BY_CALLBACK;    \
            case LIBRAW_EXCEPTION_BAD_CROP:             \
                recycle();                              \
                return LIBRAW_BAD_CROP;                 \
            default:                                    \
                return LIBRAW_UNSPECIFIED_ERROR;        \
            } \
    }while(0)

const char* LibRaw::version() { return LIBRAW_VERSION_STR;}
int LibRaw::versionNumber() { return LIBRAW_VERSION; }
const char* LibRaw::strerror(int p) { return libraw_strerror(p);}


void LibRaw::derror()
{
    if (!libraw_internal_data.unpacker_data.data_error && libraw_internal_data.internal_data.input) 
        {
            if (libraw_internal_data.internal_data.input->eof())
                {
                    if(callbacks.data_cb)(*callbacks.data_cb)(callbacks.datacb_data,
                                                              libraw_internal_data.internal_data.input->fname(),-1);
                    throw LIBRAW_EXCEPTION_IO_EOF;
                }
            else
                {
                    if(callbacks.data_cb)(*callbacks.data_cb)(callbacks.datacb_data,
                                                              libraw_internal_data.internal_data.input->fname(),
                                                              libraw_internal_data.internal_data.input->tell());
                    throw LIBRAW_EXCEPTION_IO_CORRUPT;
                }
        }
    libraw_internal_data.unpacker_data.data_error++;
}

void LibRaw::dcraw_clear_mem(libraw_processed_image_t* p)
{
    if(p) ::free(p);
}

#define ZERO(a) memset(&a,0,sizeof(a))


LibRaw:: LibRaw(unsigned int flags)
{
    double aber[4] = {1,1,1,1};
    double gamm[6] = { 0.45,4.5,0,0,0,0 };
    unsigned greybox[4] =  { 0, 0, UINT_MAX, UINT_MAX };
    unsigned cropbox[4] =  { 0, 0, UINT_MAX, UINT_MAX };
#ifdef DCRAW_VERBOSE
    verbose = 1;
#else
    verbose = 0;
#endif
    ZERO(imgdata);
    ZERO(libraw_internal_data);
    ZERO(callbacks);
    callbacks.mem_cb = (flags & LIBRAW_OPIONS_NO_MEMERR_CALLBACK) ? NULL:  &default_memory_callback;
    callbacks.data_cb = (flags & LIBRAW_OPIONS_NO_DATAERR_CALLBACK)? NULL : &default_data_callback;
    memmove(&imgdata.params.aber,&aber,sizeof(aber));
    memmove(&imgdata.params.gamm,&gamm,sizeof(gamm));
    memmove(&imgdata.params.greybox,&greybox,sizeof(greybox));
    memmove(&imgdata.params.cropbox,&cropbox,sizeof(cropbox));
    
    imgdata.params.bright=1;
    imgdata.params.use_camera_matrix=-1;
    imgdata.params.user_flip=-1;
    imgdata.params.user_black=-1;
    imgdata.params.user_sat=-1;
    imgdata.params.user_qual=-1;
    imgdata.params.output_color=1;
    imgdata.params.output_bps=8;
    imgdata.params.use_fuji_rotate=1;
    imgdata.params.exp_shift = 1.0;
    imgdata.params.auto_bright_thr = LIBRAW_DEFAULT_AUTO_BRIGHTNESS_THRESHOLD;
    imgdata.params.adjust_maximum_thr= LIBRAW_DEFAULT_ADJUST_MAXIMUM_THRESHOLD;
    imgdata.params.green_matching = 0;
    imgdata.parent_class = this;
    imgdata.progress_flags = 0;
    tls = new LibRaw_TLS;
    tls->init();
}


void* LibRaw:: malloc(size_t t)
{
    void *p = memmgr.malloc(t);
    return p;
}
void* LibRaw:: realloc(void *q,size_t t)
{
    void *p = memmgr.realloc(q,t);
    return p;
}


void* LibRaw::       calloc(size_t n,size_t t)
{
    void *p = memmgr.calloc(n,t);
    return p;
}
void  LibRaw::      free(void *p)
{
    memmgr.free(p);
}


int LibRaw:: fc (int row, int col)
{
    static const char filter[16][16] =
        { { 2,1,1,3,2,3,2,0,3,2,3,0,1,2,1,0 },
          { 0,3,0,2,0,1,3,1,0,1,1,2,0,3,3,2 },
          { 2,3,3,2,3,1,1,3,3,1,2,1,2,0,0,3 },
          { 0,1,0,1,0,2,0,2,2,0,3,0,1,3,2,1 },
          { 3,1,1,2,0,1,0,2,1,3,1,3,0,1,3,0 },
          { 2,0,0,3,3,2,3,1,2,0,2,0,3,2,2,1 },
          { 2,3,3,1,2,1,2,1,2,1,1,2,3,0,0,1 },
          { 1,0,0,2,3,0,0,3,0,3,0,3,2,1,2,3 },
          { 2,3,3,1,1,2,1,0,3,2,3,0,2,3,1,3 },
          { 1,0,2,0,3,0,3,2,0,1,1,2,0,1,0,2 },
          { 0,1,1,3,3,2,2,1,1,3,3,0,2,1,3,2 },
          { 2,3,2,0,0,1,3,0,2,0,1,2,3,0,1,0 },
          { 1,3,1,2,3,2,3,2,0,2,0,1,1,0,3,0 },
          { 0,2,0,3,1,0,0,1,1,3,3,2,3,2,2,1 },
          { 2,1,3,2,3,1,2,1,0,3,0,2,0,2,0,2 },
          { 0,3,1,0,0,2,0,3,2,1,3,1,1,3,1,3 } };
    
    if (imgdata.idata.filters != 1) return FC(row,col);
    return filter[(row+imgdata.sizes.top_margin) & 15][(col+imgdata.sizes.left_margin) & 15];
}

void LibRaw:: recycle() 
{
    if(libraw_internal_data.internal_data.input && libraw_internal_data.internal_data.input_internal) 
        { 
            delete libraw_internal_data.internal_data.input; 
            libraw_internal_data.internal_data.input = NULL;
        }
    libraw_internal_data.internal_data.input_internal = 0;
#define FREE(a) do { if(a) { free(a); a = NULL;} }while(0)
            
    FREE(imgdata.image); 
    FREE(imgdata.thumbnail.thumb);
    FREE(libraw_internal_data.internal_data.meta_data);
    FREE(libraw_internal_data.output_data.histogram);
    FREE(libraw_internal_data.output_data.oprof);
    FREE(imgdata.color.profile);
    FREE(imgdata.rawdata.ph1_black);
    FREE(imgdata.rawdata.raw_alloc); 
#undef FREE
    ZERO(imgdata.rawdata);
    ZERO(imgdata.sizes);
    ZERO(imgdata.color);
    ZERO(libraw_internal_data);
    memmgr.cleanup();
    imgdata.thumbnail.tformat = LIBRAW_THUMBNAIL_UNKNOWN;
    imgdata.progress_flags = 0;
    
    tls->init();
}

const char * LibRaw::unpack_function_name()
{
    libraw_decoder_info_t decoder_info;
    get_decoder_info(&decoder_info);
    return decoder_info.decoder_name;
}

int LibRaw::get_decoder_info(libraw_decoder_info_t* d_info)
{
    if(!d_info)   return LIBRAW_UNSPECIFIED_ERROR;
    if(!load_raw) return LIBRAW_OUT_OF_ORDER_CALL;
    
    d_info->decoder_flags = LIBRAW_DECODER_NOTSET;

    // sorted names order
    if (load_raw == &LibRaw::adobe_dng_load_raw_lj) 
        {
            // Check rbayer
            d_info->decoder_name = "adobe_dng_load_raw_lj()"; 
            d_info->decoder_flags = imgdata.idata.filters ? LIBRAW_DECODER_FLATFIELD : LIBRAW_DECODER_4COMPONENT;
        }
    else if (load_raw == &LibRaw::adobe_dng_load_raw_nc)
        {
            // Check rbayer
            d_info->decoder_name = "adobe_dng_load_raw_nc()"; 
            d_info->decoder_flags = imgdata.idata.filters ? LIBRAW_DECODER_FLATFIELD : LIBRAW_DECODER_4COMPONENT;
        }
    else if (load_raw == &LibRaw::canon_600_load_raw) 
        {
            d_info->decoder_name = "canon_600_load_raw()";   
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD; // WB set within decoder, no need to load raw
        }
    else if (load_raw == &LibRaw::canon_compressed_load_raw)
        {
            d_info->decoder_name = "canon_compressed_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::canon_sraw_load_raw) 
        {
            d_info->decoder_name = "canon_sraw_load_raw()";
            d_info->decoder_flags = LIBRAW_DECODER_LEGACY; 
        }
    else if (load_raw == &LibRaw::eight_bit_load_raw )
        {
            d_info->decoder_name = "eight_bit_load_raw()";
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::foveon_load_raw )
        {
            d_info->decoder_name = "foveon_load_raw()";
            d_info->decoder_flags = LIBRAW_DECODER_LEGACY; 
        }
    else if (load_raw == &LibRaw::fuji_load_raw ) 
        { 
            d_info->decoder_name = "fuji_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::hasselblad_load_raw )
        {
            d_info->decoder_name = "hasselblad_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::imacon_full_load_raw )
        {
            d_info->decoder_name = "imacon_full_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_4COMPONENT; 
        }
    else if (load_raw == &LibRaw::kodak_262_load_raw )
        {
            d_info->decoder_name = "kodak_262_load_raw()"; // UNTESTED!
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::kodak_65000_load_raw )
        {
            d_info->decoder_name = "kodak_65000_load_raw()";
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::kodak_dc120_load_raw )
        {
            d_info->decoder_name = "kodak_dc120_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::kodak_jpeg_load_raw )
        {
            // UNTESTED + RBAYER
            d_info->decoder_name = "kodak_jpeg_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::kodak_radc_load_raw )
        {
            d_info->decoder_name = "kodak_radc_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_4COMPONENT;
        }
    else if (load_raw == &LibRaw::kodak_rgb_load_raw ) 
        {
            // UNTESTED
            d_info->decoder_name = "kodak_rgb_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_4COMPONENT;
        }
    else if (load_raw == &LibRaw::kodak_yrgb_load_raw )    
        {
            d_info->decoder_name = "kodak_yrgb_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_4COMPONENT;
        }
    else if (load_raw == &LibRaw::kodak_ycbcr_load_raw )
        {
            // UNTESTED
            d_info->decoder_name = "kodak_ycbcr_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_4COMPONENT;
        }
    else if (load_raw == &LibRaw::leaf_hdr_load_raw )
        {
            d_info->decoder_name = "leaf_hdr_load_raw()"; 
            d_info->decoder_flags = imgdata.idata.filters ? LIBRAW_DECODER_FLATFIELD : LIBRAW_DECODER_4COMPONENT;
        }
    else if (load_raw == &LibRaw::lossless_jpeg_load_raw)
        {
            // Check rbayer
            d_info->decoder_name = "lossless_jpeg_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::minolta_rd175_load_raw ) 
        {  
            // UNTESTED
            d_info->decoder_name = "minolta_rd175_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::nikon_compressed_load_raw)
        {
            // Check rbayer
            d_info->decoder_name = "nikon_compressed_load_raw()";
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::nokia_load_raw )
        {
            // UNTESTED
            d_info->decoder_name = "nokia_load_raw()";
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::olympus_load_raw )
        {
            d_info->decoder_name = "olympus_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::packed_load_raw )
        {
            d_info->decoder_name = "packed_load_raw()";
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::panasonic_load_raw )
        {
            d_info->decoder_name = "panasonic_load_raw()";
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::pentax_load_raw )
        {
            d_info->decoder_name = "pentax_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::phase_one_load_raw )
        {
            d_info->decoder_name = "phase_one_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::phase_one_load_raw_c )
        {
            d_info->decoder_name = "phase_one_load_raw_c()"; 
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::quicktake_100_load_raw )
        {
            // UNTESTED
            d_info->decoder_name = "quicktake_100_load_raw()";
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::rollei_load_raw )
        {
            // UNTESTED
            d_info->decoder_name = "rollei_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::sinar_4shot_load_raw )
        {
            // UNTESTED
            d_info->decoder_name = "sinar_4shot_load_raw()";
            d_info->decoder_flags = LIBRAW_DECODER_4COMPONENT;
        }
    else if (load_raw == &LibRaw::smal_v6_load_raw )
        {
            // UNTESTED
            d_info->decoder_name = "smal_v6_load_raw()";
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::smal_v9_load_raw )
        {
            // UNTESTED
            d_info->decoder_name = "smal_v9_load_raw()";
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::sony_load_raw )
        {
            d_info->decoder_name = "sony_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::sony_arw_load_raw )
        {
            d_info->decoder_name = "sony_arw_load_raw()";
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::sony_arw2_load_raw )
        {
            d_info->decoder_name = "sony_arw2_load_raw()";
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD;
        }
    else if (load_raw == &LibRaw::unpacked_load_raw )
        {
            d_info->decoder_name = "unpacked_load_raw()"; 
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD | LIBRAW_DECODER_USEBAYER2;
        }
    else  if (load_raw == &LibRaw::redcine_load_raw)
        {
            d_info->decoder_name = "redcine_load_raw()";
            d_info->decoder_flags = LIBRAW_DECODER_FLATFIELD; 
        }
    else
        {
            d_info->decoder_name = "Unknown unpack function";
            d_info->decoder_flags = LIBRAW_DECODER_NOTSET;
        }
    return LIBRAW_SUCCESS;
}

int LibRaw::adjust_maximum()
{
    int i;
    ushort real_max;
    float  auto_threshold;

    if(O.adjust_maximum_thr < 0.00001)
        return LIBRAW_SUCCESS;
    else if (O.adjust_maximum_thr > 0.99999)
        auto_threshold = LIBRAW_DEFAULT_ADJUST_MAXIMUM_THRESHOLD;
    else
        auto_threshold = O.adjust_maximum_thr;
        
    
    real_max = C.channel_maximum[0];
    for(i = 1; i< 4; i++)
        if(real_max < C.channel_maximum[i])
            real_max = C.channel_maximum[i];

    if (real_max > 0 && real_max < C.maximum && real_max > C.maximum* auto_threshold)
        {
            C.maximum = real_max;
        }
    return LIBRAW_SUCCESS;
}


void LibRaw:: merror (void *ptr, const char *where)
{
    if (ptr) return;
    if(callbacks.mem_cb)(*callbacks.mem_cb)(callbacks.memcb_data,
                                            libraw_internal_data.internal_data.input
                                            ?libraw_internal_data.internal_data.input->fname()
                                            :NULL,
                                            where);
    throw LIBRAW_EXCEPTION_ALLOC;
}



int LibRaw::open_file(const char *fname, INT64 max_buf_size)
{
#ifndef WIN32
    struct stat st;
    if(stat(fname,&st))
        return LIBRAW_IO_ERROR;
    int big = (st.st_size > max_buf_size)?1:0;
#else
	struct _stati64 st;
    if(_stati64(fname,&st))	
        return LIBRAW_IO_ERROR;
    int big = (st.st_size > max_buf_size)?1:0;
#endif

    LibRaw_abstract_datastream *stream;
    try {
        if(big)
         stream = new LibRaw_bigfile_datastream(fname);
        else
         stream = new LibRaw_file_datastream(fname);
    }

    catch (std::bad_alloc)
        {
            recycle();
            return LIBRAW_UNSUFFICIENT_MEMORY;
        }
    if(!stream->valid())
        {
            delete stream;
            return LIBRAW_IO_ERROR;
        }
    ID.input_internal = 0; // preserve from deletion on error
    int ret = open_datastream(stream);
    if (ret == LIBRAW_SUCCESS)
        {
            ID.input_internal =1 ; // flag to delete datastream on recycle
        }
    else
        {
            delete stream;
            ID.input_internal = 0;
        }
    return ret;
}

int LibRaw::open_buffer(void *buffer, size_t size)
{
    // this stream will close on recycle()
    if(!buffer  || buffer==(void*)-1)
        return LIBRAW_IO_ERROR;

    LibRaw_buffer_datastream *stream;
    try {
        stream = new LibRaw_buffer_datastream(buffer,size);
    }
    catch (std::bad_alloc)
        {
            recycle();
            return LIBRAW_UNSUFFICIENT_MEMORY;
        }
    if(!stream->valid())
        {
            delete stream;
            return LIBRAW_IO_ERROR;
        }
    ID.input_internal = 0; // preserve from deletion on error
    int ret = open_datastream(stream);
    if (ret == LIBRAW_SUCCESS)
        {
            ID.input_internal =1 ; // flag to delete datastream on recycle
        }
    else
        {
            delete stream;
            ID.input_internal = 0;
        }
    return ret;
}


int LibRaw::open_datastream(LibRaw_abstract_datastream *stream)
{

    if(!stream)
        return ENOENT;
    if(!stream->valid())
        return LIBRAW_IO_ERROR;
    recycle();

    try {
        ID.input = stream;
        SET_PROC_FLAG(LIBRAW_PROGRESS_OPEN);

        if (O.use_camera_matrix < 0)
            O.use_camera_matrix = O.use_camera_wb;

        identify();

        if(IO.fuji_width)
            {
                IO.fwidth = S.width;
                IO.fheight = S.height;
                S.iwidth = S.width = IO.fuji_width << (int)(!libraw_internal_data.unpacker_data.fuji_layout);
                S.iheight = S.height = S.raw_height;
                S.raw_height += 2*S.top_margin;
            }

        if(C.profile_length)
            {
                if(C.profile) free(C.profile);
                C.profile = malloc(C.profile_length);
                merror(C.profile,"LibRaw::open_file()");
                ID.input->seek(ID.profile_offset,SEEK_SET);
                ID.input->read(C.profile,C.profile_length,1);
            }
        
        SET_PROC_FLAG(LIBRAW_PROGRESS_IDENTIFY);
    }
    catch ( LibRaw_exceptions err) {
        EXCEPTION_HANDLER(err);
    }

    if(P1.raw_count < 1) 
        return LIBRAW_FILE_UNSUPPORTED;

    
    write_fun = &LibRaw::write_ppm_tiff;
    
    if (load_raw == &LibRaw::kodak_ycbcr_load_raw) 
        {
            S.height += S.height & 1;
            S.width  += S.width  & 1;
        }

    IO.shrink = P1.filters && (O.half_size ||
	((O.threshold || O.aber[0] != 1 || O.aber[2] != 1) ));

    S.iheight = (S.height + IO.shrink) >> IO.shrink;
    S.iwidth  = (S.width  + IO.shrink) >> IO.shrink;

    // Save color,sizes and internal data into raw_image fields
    memmove(&imgdata.rawdata.color,&imgdata.color,sizeof(imgdata.color));
    memmove(&imgdata.rawdata.sizes,&imgdata.sizes,sizeof(imgdata.sizes));
    memmove(&imgdata.rawdata.iparams,&imgdata.idata,sizeof(imgdata.idata));
    memmove(&imgdata.rawdata.ioparams,&libraw_internal_data.internal_output_params,sizeof(libraw_internal_data.internal_output_params));
    
    SET_PROC_FLAG(LIBRAW_PROGRESS_SIZE_ADJUST);


    return LIBRAW_SUCCESS;
}

int LibRaw::unpack(void)
{
    CHECK_ORDER_HIGH(LIBRAW_PROGRESS_LOAD_RAW);
    CHECK_ORDER_LOW(LIBRAW_PROGRESS_IDENTIFY);
    try {

        RUN_CALLBACK(LIBRAW_PROGRESS_LOAD_RAW,0,2);
        if (O.shot_select >= P1.raw_count)
            return LIBRAW_REQUEST_FOR_NONEXISTENT_IMAGE;
        
        if(!load_raw)
            return LIBRAW_UNSPECIFIED_ERROR;
        
        if (O.use_camera_matrix && C.cmatrix[0][0] > 0.25) 
            {
                memcpy (C.rgb_cam, C.cmatrix, sizeof (C.cmatrix));
                IO.raw_color = 0;
            }
        // already allocated ?
        if(imgdata.image)
            {
                free(imgdata.image);
                imgdata.image = 0;
            }

        if (libraw_internal_data.unpacker_data.meta_length) 
            {
                libraw_internal_data.internal_data.meta_data = 
                    (char *) malloc (libraw_internal_data.unpacker_data.meta_length);
                merror (libraw_internal_data.internal_data.meta_data, "LibRaw::unpack()");
            }
        ID.input->seek(libraw_internal_data.unpacker_data.data_offset, SEEK_SET);
        int save_document_mode = O.document_mode;
        O.document_mode = 0;

        libraw_decoder_info_t decoder_info;
        get_decoder_info(&decoder_info);

        int save_iwidth = S.iwidth, save_iheight = S.iheight, save_shrink = IO.shrink;

        int rwidth = S.raw_width, rheight = S.raw_height;
        if( !IO.fuji_width)
            {
                // adjust non-Fuji allocation
                if(rwidth < S.width + S.left_margin)
                    rwidth = S.width + S.left_margin;
                if(rheight < S.height + S.top_margin)
                    rheight = S.height + S.top_margin;
            }
        
        if(decoder_info.decoder_flags &  LIBRAW_DECODER_FLATFIELD)
            {
                imgdata.rawdata.raw_alloc = malloc(rwidth*rheight*sizeof(imgdata.rawdata.raw_image[0]));
                imgdata.rawdata.raw_image = (ushort*) imgdata.rawdata.raw_alloc;
            }
        else if (decoder_info.decoder_flags &  LIBRAW_DECODER_4COMPONENT)
            {
                S.iwidth = S.width;
                S.iheight= S.height;
                IO.shrink = 0;
                imgdata.rawdata.raw_alloc = calloc(rwidth*rheight,sizeof(*imgdata.rawdata.color_image));
                imgdata.rawdata.color_image = (ushort(*)[4]) imgdata.rawdata.raw_alloc;
            }
        else if (decoder_info.decoder_flags & LIBRAW_DECODER_LEGACY)
            {
                // sRAW and Foveon only, so extra buffer size is just 1/4
                // Legacy converters does not supports half mode!
                S.iwidth = S.width;
                S.iheight= S.height;
                IO.shrink = 0;
                // allocate image as temporary buffer, size 
                imgdata.rawdata.raw_alloc = calloc(S.iwidth*S.iheight,sizeof(*imgdata.image));
                imgdata.image = (ushort (*)[4]) imgdata.rawdata.raw_alloc;
            }


        (this->*load_raw)();


        // recover saved
        if( decoder_info.decoder_flags & LIBRAW_DECODER_LEGACY)
            {
                imgdata.image = 0; 
                imgdata.rawdata.color_image = (ushort (*)[4]) imgdata.rawdata.raw_alloc;
            }

        // calculate channel maximum
        {
            for(int c=0;c<4;c++) C.channel_maximum[c] = 0;
            if(decoder_info.decoder_flags & LIBRAW_DECODER_LEGACY)
                {
                    for(int rc = 0; rc < S.iwidth*S.iheight; rc++)
                        {
                            if(C.channel_maximum[0]<imgdata.rawdata.color_image[rc][0]) 
                                C.channel_maximum[0]=imgdata.rawdata.color_image[rc][0];
                            if(C.channel_maximum[1]<imgdata.rawdata.color_image[rc][1]) 
                                C.channel_maximum[1]=imgdata.rawdata.color_image[rc][1];
                            if(C.channel_maximum[2]<imgdata.rawdata.color_image[rc][2]) 
                                C.channel_maximum[2]=imgdata.rawdata.color_image[rc][2];
                            if(C.channel_maximum[3]<imgdata.rawdata.color_image[rc][3]) 
                                C.channel_maximum[3]=imgdata.rawdata.color_image[rc][3];
                        }
                }
            else if(decoder_info.decoder_flags &  LIBRAW_DECODER_4COMPONENT)
                {
                    for(int row = S.top_margin; row < S.height+S.top_margin; row++)
                        for(int col = S.left_margin; col < S.width+S.left_margin; col++)
                        {
                            int rc = row*S.raw_width+col;
                            if(C.channel_maximum[0]<imgdata.rawdata.color_image[rc][0]) 
                                C.channel_maximum[0]=imgdata.rawdata.color_image[rc][0];
                            if(C.channel_maximum[1]<imgdata.rawdata.color_image[rc][1]) 
                                C.channel_maximum[1]=imgdata.rawdata.color_image[rc][1];
                            if(C.channel_maximum[2]<imgdata.rawdata.color_image[rc][2]) 
                                C.channel_maximum[2]=imgdata.rawdata.color_image[rc][2];
                            if(C.channel_maximum[3]<imgdata.rawdata.color_image[rc][3]) 
                                C.channel_maximum[3]=imgdata.rawdata.color_image[rc][4];
                        }
                }
            else if (decoder_info.decoder_flags &  LIBRAW_DECODER_FLATFIELD)
                {
                        for(int row = 0; row < S.height; row++)
                            {
                                int colors[4];
                                for (int xx=0;xx<4;xx++)
                                    colors[xx] = COLOR(row,xx);
                                for(int col = 0; col < S.width; col++)
                                    {
                                        int cc = colors[col&3];
                                        if(C.channel_maximum[cc] 
                                           < imgdata.rawdata.raw_image[(row+S.top_margin)*S.raw_width
                                                                       +(col+S.left_margin)])
                                            C.channel_maximum[cc] = 
                                                imgdata.rawdata.raw_image[(row+S.top_margin)*S.raw_width
                                                                          +(col+S.left_margin)];
                                    }
                            }
                }
        }
        // recover image sizes
        S.iwidth = save_iwidth;
        S.iheight = save_iheight;
        IO.shrink = save_shrink;

        // phase-one black
        if(imgdata.rawdata.ph1_black)
            C.ph1_black = imgdata.rawdata.ph1_black;
        O.document_mode = save_document_mode;

        // adjust black to possible maximum
        unsigned int i = C.cblack[3];
        unsigned int c;
        for(c=0;c<3;c++)
            if (i > C.cblack[c]) i = C.cblack[c];
        for (c=0;c<4;c++)
            C.cblack[c] -= i;
        C.black += i;


        // Save color,sizes and internal data into raw_image fields
        memmove(&imgdata.rawdata.color,&imgdata.color,sizeof(imgdata.color));
        memmove(&imgdata.rawdata.sizes,&imgdata.sizes,sizeof(imgdata.sizes));
        memmove(&imgdata.rawdata.iparams,&imgdata.idata,sizeof(imgdata.idata));
        memmove(&imgdata.rawdata.ioparams,&libraw_internal_data.internal_output_params,sizeof(libraw_internal_data.internal_output_params));

        SET_PROC_FLAG(LIBRAW_PROGRESS_LOAD_RAW);
        RUN_CALLBACK(LIBRAW_PROGRESS_LOAD_RAW,1,2);
        
        return 0;
    }
    catch ( LibRaw_exceptions err) {
        EXCEPTION_HANDLER(err);
    }
}

void LibRaw::free_image(void)
{
    if(imgdata.image)
        {
            free(imgdata.image);
            imgdata.image = 0;
            imgdata.progress_flags 
                = LIBRAW_PROGRESS_START|LIBRAW_PROGRESS_OPEN
                |LIBRAW_PROGRESS_IDENTIFY|LIBRAW_PROGRESS_SIZE_ADJUST|LIBRAW_PROGRESS_LOAD_RAW;
        }
}


void LibRaw::raw2image_start()
{
        // restore color,sizes and internal data into raw_image fields
        memmove(&imgdata.color,&imgdata.rawdata.color,sizeof(imgdata.color));
        memmove(&imgdata.sizes,&imgdata.rawdata.sizes,sizeof(imgdata.sizes));
        memmove(&imgdata.idata,&imgdata.rawdata.iparams,sizeof(imgdata.idata));
        memmove(&libraw_internal_data.internal_output_params,&imgdata.rawdata.ioparams,sizeof(libraw_internal_data.internal_output_params));

        if (O.user_flip >= 0)
            S.flip = O.user_flip;
        
        switch ((S.flip+3600) % 360) 
            {
            case 270:  S.flip = 5;  break;
            case 180:  S.flip = 3;  break;
            case  90:  S.flip = 6;  break;
            }

        // adjust for half mode!
        IO.shrink = P1.filters && (O.half_size ||
                                   ((O.threshold || O.aber[0] != 1 || O.aber[2] != 1) ));
        
        S.iheight = (S.height + IO.shrink) >> IO.shrink;
        S.iwidth  = (S.width  + IO.shrink) >> IO.shrink;

        if (O.user_black >= 0) 
            C.black = O.user_black;
}

// Same as raw2image, but
// 1) Do raw2image and rotate_fuji_raw in one pass
// 2) Do raw2image and cropping in one pass
#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
int LibRaw::raw2image_ex(void)
{
    CHECK_ORDER_LOW(LIBRAW_PROGRESS_LOAD_RAW);

    raw2image_start();

    // process cropping
    int do_crop = 0;
    unsigned save_filters = imgdata.idata.filters;
    unsigned save_width = S.width;
    if (~O.cropbox[2] && ~O.cropbox[3])
        {
            int crop[4],c,filt;
            for(int c=0;c<4;c++) 
                {
                    crop[c] = O.cropbox[c];
                    if(crop[c]<0)
                        crop[c]=0;
                }
            if(IO.fwidth) 
                {
                    crop[0] = (crop[0]/4)*4;
                    crop[1] = (crop[1]/4)*4;
                }
            do_crop = 1;
            crop[2] = MIN (crop[2], (signed) S.width-crop[0]);
            crop[3] = MIN (crop[3], (signed) S.height-crop[1]);
            if (crop[2] <= 0 || crop[3] <= 0)
                throw LIBRAW_EXCEPTION_BAD_CROP;
            
            // adjust sizes!
            S.left_margin+=crop[0];
            S.top_margin+=crop[1];
            S.width=crop[2];
            S.height=crop[3];
            
            S.iheight = (S.height + IO.shrink) >> IO.shrink;
            S.iwidth  = (S.width  + IO.shrink) >> IO.shrink;
            if(!IO.fwidth && imgdata.idata.filters)
                {
                    for (filt=c=0; c < 16; c++)
                        filt |= FC((c >> 1)+(crop[1]),
                                   (c &  1)+(crop[0])) << c*2;
                    imgdata.idata.filters = filt;
                }
        }

    if(IO.fwidth) 
        {
            ushort fiwidth,fiheight;
            if(do_crop)
                {
                    IO.fuji_width = S.width >> !libraw_internal_data.unpacker_data.fuji_layout;
                    IO.fwidth = (S.height >> libraw_internal_data.unpacker_data.fuji_layout) + IO.fuji_width;
                    IO.fheight = IO.fwidth - 1;
                }

            fiheight = (IO.fheight + IO.shrink) >> IO.shrink;
            fiwidth = (IO.fwidth + IO.shrink) >> IO.shrink;
            if(imgdata.image)
                    {
                        imgdata.image = (ushort (*)[4])realloc(imgdata.image,fiheight*fiwidth*sizeof (*imgdata.image));
                        memset(imgdata.image,0,fiheight*fiwidth *sizeof (*imgdata.image));
                    }
                else
                    imgdata.image = (ushort (*)[4]) calloc (fiheight*fiwidth, sizeof (*imgdata.image));
            merror (imgdata.image, "raw2image_ex()");

            int cblk[4],i;
            for(i=0;i<4;i++)
                cblk[i] = C.cblack[i]+C.black;
            ZERO(C.channel_maximum);

            int row,col;
            for(row=0;row<S.height;row++)
                {
                    for(col=0;col<S.width;col++)
                        {
                            int r,c;
                            if (libraw_internal_data.unpacker_data.fuji_layout) {
                                r = IO.fuji_width - 1 - col + (row >> 1);
                                c = col + ((row+1) >> 1);
                            } else {
                                r = IO.fuji_width - 1 + row - (col >> 1);
                                c = row + ((col+1) >> 1);
                            }
                            
                            int val = imgdata.rawdata.raw_image[(row+S.top_margin)*S.raw_width
                                                            +(col+S.left_margin)];
                            int cc = FCF(row,col);
                            if(val > cblk[cc])
                                val -= cblk[cc];
                            else
                                val = 0;
                            imgdata.image[((r) >> IO.shrink)*fiwidth + ((c) >> IO.shrink)][cc] = val;
                            if(C.channel_maximum[cc] < val) C.channel_maximum[cc] = val;
                        }
                }
            C.maximum -= C.black;
            ZERO(C.cblack);
            C.black = 0;

            // restore fuji sizes!
            S.height = IO.fheight;
            S.width = IO.fwidth;
            S.iheight = (S.height + IO.shrink) >> IO.shrink;
            S.iwidth  = (S.width  + IO.shrink) >> IO.shrink;
            S.raw_height -= 2*S.top_margin;
        }
    else
        {

                if(imgdata.image)
                    {
                        imgdata.image = (ushort (*)[4]) realloc (imgdata.image,S.iheight*S.iwidth 
                                                                 *sizeof (*imgdata.image));
                        memset(imgdata.image,0,S.iheight*S.iwidth *sizeof (*imgdata.image));
                    }
                else
                    imgdata.image = (ushort (*)[4]) calloc (S.iheight*S.iwidth, sizeof (*imgdata.image));

                merror (imgdata.image, "raw2image_ex()");
                
                libraw_decoder_info_t decoder_info;
                get_decoder_info(&decoder_info);


                if(decoder_info.decoder_flags & LIBRAW_DECODER_FLATFIELD)
                    {
                        if(decoder_info.decoder_flags & LIBRAW_DECODER_USEBAYER2)
#if defined(LIBRAW_USE_OPENMP)
#pragma omp parallel for default(shared)
#endif
                            for(int row = 0; row < S.height; row++)
                                for(int col = 0; col < S.width; col++)
                                    imgdata.image[(row >> IO.shrink)*S.iwidth + (col>>IO.shrink)][fc(row,col)]
                                        = imgdata.rawdata.raw_image[(row+S.top_margin)*S.raw_width
                                                                    +(col+S.left_margin)];
                        else
#if defined(LIBRAW_USE_OPENMP)
#pragma omp parallel for default(shared)
#endif
                            for(int row = 0; row < S.height; row++)
                                {
                                    int colors[2];
                                    for (int xx=0;xx<2;xx++)
                                        colors[xx] = COLOR(row,xx);
                                    for(int col = 0; col < S.width; col++)
                                        {
                                            int cc = colors[col&1];
                                            imgdata.image[(row >> IO.shrink)*S.iwidth + (col>>IO.shrink)][cc] =
                                                imgdata.rawdata.raw_image[(row+S.top_margin)*S.raw_width
                                                                          +(col+S.left_margin)];
                                        }
                                }
                    }
                else if (decoder_info.decoder_flags & LIBRAW_DECODER_4COMPONENT)
                    {
#define FC0(row,col) (save_filters >> ((((row) << 1 & 14) + ((col) & 1)) << 1) & 3)
                        if(IO.shrink)
#if defined(LIBRAW_USE_OPENMP)
#pragma omp parallel for default(shared)
#endif
                            for(int row = 0; row < S.height; row++)
                                for(int col = 0; col < S.width; col++)
                                    imgdata.image[(row >> IO.shrink)*S.iwidth + (col>>IO.shrink)][FC(row,col)] 
                                        = imgdata.rawdata.color_image[(row+S.top_margin)*S.raw_width
                                                                      +S.left_margin+col]
                                        [FC0(row+S.top_margin,col+S.left_margin)];
#undef FC0
                        else
#if defined(LIBRAW_USE_OPENMP)
#pragma omp parallel for default(shared)
#endif
                            for(int row = 0; row < S.height; row++)
                                memmove(&imgdata.image[row*S.width],
                                        &imgdata.rawdata.color_image[(row+S.top_margin)*S.raw_width+S.left_margin],
                                        S.width*sizeof(*imgdata.image));
                    }
                else if(decoder_info.decoder_flags & LIBRAW_DECODER_LEGACY)
                    {
                        if(do_crop)
#if defined(LIBRAW_USE_OPENMP)
#pragma omp parallel for default(shared)
#endif
                            for(int row = 0; row < S.height; row++)
                                memmove(&imgdata.image[row*S.width],
                                        &imgdata.rawdata.color_image[(row+S.top_margin)*save_width+S.left_margin],
                                        S.width*sizeof(*imgdata.image));
                                
                        else 
                            memmove(imgdata.image,imgdata.rawdata.color_image,
                                    S.width*S.height*sizeof(*imgdata.image));
                    }

                if(imgdata.rawdata.use_ph1_correct) // Phase one unpacked!
                        phase_one_correct();
            }
    return LIBRAW_SUCCESS;
}

#undef MIN




int LibRaw::raw2image(void)
{

    CHECK_ORDER_LOW(LIBRAW_PROGRESS_LOAD_RAW);

    try {
        raw2image_start();

        // free and re-allocate image bitmap
        if(imgdata.image)
            {
                imgdata.image = (ushort (*)[4]) realloc (imgdata.image,S.iheight*S.iwidth *sizeof (*imgdata.image));
                memset(imgdata.image,0,S.iheight*S.iwidth *sizeof (*imgdata.image));
            }
        else
            imgdata.image = (ushort (*)[4]) calloc (S.iheight*S.iwidth, sizeof (*imgdata.image));

        merror (imgdata.image, "raw2image()");

        libraw_decoder_info_t decoder_info;
        get_decoder_info(&decoder_info);
        
        // Move saved bitmap to imgdata.image
        if(decoder_info.decoder_flags & LIBRAW_DECODER_FLATFIELD)
            {
                if(decoder_info.decoder_flags & LIBRAW_DECODER_USEBAYER2)
                    {
                        for(int row = 0; row < S.height; row++)
                            for(int col = 0; col < S.width; col++)
                                imgdata.image[(row >> IO.shrink)*S.iwidth + (col>>IO.shrink)][fc(row,col)]
                                = imgdata.rawdata.raw_image[(row+S.top_margin)*S.raw_width
                                                                           +(col+S.left_margin)];
                    }
                else
                    {
                        for(int row = 0; row < S.height; row++)
                            {
                                int colors[4];
                                for (int xx=0;xx<4;xx++)
                                    colors[xx] = COLOR(row,xx);
                                for(int col = 0; col < S.width; col++)
                                    {
                                        int cc = colors[col&3];
                                        imgdata.image[(row >> IO.shrink)*S.iwidth + (col>>IO.shrink)][cc] =
                                            imgdata.rawdata.raw_image[(row+S.top_margin)*S.raw_width+(col
                                                                                                      +S.left_margin)];
                                    }
                            }
                    }
            }
        else if (decoder_info.decoder_flags & LIBRAW_DECODER_4COMPONENT)
            {
                if(IO.shrink)
                    {
                        for(int row = 0; row < S.height; row++)
                            for(int col = 0; col < S.width; col++)
                                {
                                    int cc = FC(row,col);
                                    imgdata.image[(row >> IO.shrink)*S.iwidth + (col>>IO.shrink)][cc] 
                                        = imgdata.rawdata.color_image[(row+S.top_margin)*S.raw_width
                                                                      +S.left_margin+col][cc];
                                }
                    }
                else
                    for(int row = 0; row < S.height; row++)
                        memmove(&imgdata.image[row*S.width],
                                &imgdata.rawdata.color_image[(row+S.top_margin)*S.raw_width+S.left_margin],
                                S.width*sizeof(*imgdata.image));
            }
        else if(decoder_info.decoder_flags & LIBRAW_DECODER_LEGACY)
            {
                // legacy is always 4channel and not shrinked!
                memmove(imgdata.image,imgdata.rawdata.color_image,S.width*S.height*sizeof(*imgdata.image));
            }

        if(imgdata.rawdata.use_ph1_correct) // Phase one unpacked!
            phase_one_correct();

        // hack - clear later flags!
        imgdata.progress_flags 
            = LIBRAW_PROGRESS_START|LIBRAW_PROGRESS_OPEN
            |LIBRAW_PROGRESS_IDENTIFY|LIBRAW_PROGRESS_SIZE_ADJUST|LIBRAW_PROGRESS_LOAD_RAW;
        return 0;
    }
    catch ( LibRaw_exceptions err) {
        EXCEPTION_HANDLER(err);
    }
}
    

int LibRaw::dcraw_document_mode_processing(void)
{
//    CHECK_ORDER_HIGH(LIBRAW_PROGRESS_PRE_INTERPOLATE);
    CHECK_ORDER_LOW(LIBRAW_PROGRESS_LOAD_RAW);

    try {

        int no_crop = 1;

        if (~O.cropbox[2] && ~O.cropbox[3])
            no_crop=0;

        raw2image_ex(); // raw2image+crop+rotate_fuji_raw

        if (IO.zero_is_bad)
            {
                remove_zeroes();
                SET_PROC_FLAG(LIBRAW_PROGRESS_REMOVE_ZEROES);
            }

        if(!!IO.fuji_width)
            subtract_black();
        
        O.document_mode = 2;
        
        if(P1.is_foveon)
            {
                // filter image data for foveon document mode
                short *iptr = (short *)imgdata.image;
                for (int i=0; i < S.height*S.width*4; i++)
                    {
                        if ((short) iptr[i] < 0) 
                            iptr[i] = 0;
                    }
                SET_PROC_FLAG(LIBRAW_PROGRESS_FOVEON_INTERPOLATE);
            }

        O.use_fuji_rotate = 0;

        if(O.bad_pixels && no_crop) 
            {
                bad_pixels(O.bad_pixels);
                SET_PROC_FLAG(LIBRAW_PROGRESS_BAD_PIXELS);
            }
        if (O.dark_frame && no_crop)
            {
                subtract (O.dark_frame);
                SET_PROC_FLAG(LIBRAW_PROGRESS_DARK_FRAME);
            }


        adjust_maximum();

        if (O.user_sat > 0) 
            C.maximum = O.user_sat;

        pre_interpolate();
        SET_PROC_FLAG(LIBRAW_PROGRESS_PRE_INTERPOLATE);

        if (libraw_internal_data.internal_output_params.mix_green)
            {
                int i;
                for (P1.colors=3, i=0; i < S.height*S.width; i++)
                    imgdata.image[i][1] = (imgdata.image[i][1] + imgdata.image[i][3]) >> 1;
            }
        SET_PROC_FLAG(LIBRAW_PROGRESS_MIX_GREEN);

        if (!P1.is_foveon && P1.colors == 3) 
            median_filter();
        SET_PROC_FLAG(LIBRAW_PROGRESS_MEDIAN_FILTER);

        if (!P1.is_foveon && O.highlight == 2) 
            blend_highlights();

        if (!P1.is_foveon && O.highlight > 2) 
            recover_highlights();
        SET_PROC_FLAG(LIBRAW_PROGRESS_HIGHLIGHTS);

        if (O.use_fuji_rotate) 
            fuji_rotate();
        SET_PROC_FLAG(LIBRAW_PROGRESS_FUJI_ROTATE);
#ifndef NO_LCMS
	if(O.camera_profile)
            {
                apply_profile(O.camera_profile,O.output_profile);
                SET_PROC_FLAG(LIBRAW_PROGRESS_APPLY_PROFILE);
            }
#endif
        if(!libraw_internal_data.output_data.histogram)
            {
                libraw_internal_data.output_data.histogram = (int (*)[LIBRAW_HISTOGRAM_SIZE]) malloc(sizeof(*libraw_internal_data.output_data.histogram)*4);
                merror(libraw_internal_data.output_data.histogram,"LibRaw::dcraw_document_mode_processing()");
            }
        convert_to_rgb();
        SET_PROC_FLAG(LIBRAW_PROGRESS_CONVERT_RGB);

        if (O.use_fuji_rotate)
            stretch();
        SET_PROC_FLAG(LIBRAW_PROGRESS_STRETCH);

        return 0;
    }
    catch ( LibRaw_exceptions err) {
        EXCEPTION_HANDLER(err);
    }

}

#if 1

libraw_processed_image_t * LibRaw::dcraw_make_mem_thumb(int *errcode)
{
    if(!T.thumb)
        {
            if ( !ID.toffset) 
                {
                    if(errcode) *errcode= LIBRAW_NO_THUMBNAIL;
                }
            else
                {
                    if(errcode) *errcode= LIBRAW_OUT_OF_ORDER_CALL;
                }
            return NULL;
        }

    if (T.tformat == LIBRAW_THUMBNAIL_BITMAP)
        {
            libraw_processed_image_t * ret = 
                (libraw_processed_image_t *)::malloc(sizeof(libraw_processed_image_t)+T.tlength);

            if(!ret)
                {
                    if(errcode) *errcode= ENOMEM;
                    return NULL;
                }

            memset(ret,0,sizeof(libraw_processed_image_t));
            ret->type   = LIBRAW_IMAGE_BITMAP;
            ret->height = T.theight;
            ret->width  = T.twidth;
            ret->colors = 3; 
            ret->bits   = 8;
            ret->data_size = T.tlength;
            memmove(ret->data,T.thumb,T.tlength);
            if(errcode) *errcode= 0;
            return ret;
        }
    else if (T.tformat == LIBRAW_THUMBNAIL_JPEG)
        {
            ushort exif[5];
            int mk_exif = 0;
            if(strcmp(T.thumb+6,"Exif")) mk_exif = 1;
            
            int dsize = T.tlength + mk_exif * (sizeof(exif)+sizeof(tiff_hdr));

            libraw_processed_image_t * ret = 
                (libraw_processed_image_t *)::malloc(sizeof(libraw_processed_image_t)+dsize);

            if(!ret)
                {
                    if(errcode) *errcode= ENOMEM;
                    return NULL;
                }

            memset(ret,0,sizeof(libraw_processed_image_t));

            ret->type = LIBRAW_IMAGE_JPEG;
            ret->data_size = dsize;
            
            ret->data[0] = 0xff;
            ret->data[1] = 0xd8;
            if(mk_exif)
                {
                    struct tiff_hdr th;
                    memcpy (exif, "\xff\xe1  Exif\0\0", 10);
                    exif[1] = htons (8 + sizeof th);
                    memmove(ret->data+2,exif,sizeof(exif));
                    tiff_head (&th, 0);
                    memmove(ret->data+(2+sizeof(exif)),&th,sizeof(th));
                    memmove(ret->data+(2+sizeof(exif)+sizeof(th)),T.thumb+2,T.tlength-2);
                }
            else
                {
                    memmove(ret->data+2,T.thumb+2,T.tlength-2);
                }
            if(errcode) *errcode= 0;
            return ret;
            
        }
    else
        {
            if(errcode) *errcode= LIBRAW_UNSUPPORTED_THUMBNAIL;
            return NULL;

        }
}



// jlb
// macros for copying pixels to either BGR or RGB formats
#define FORBGR for(c=P1.colors-1; c >=0 ; c--)
#define FORRGB for(c=0; c < P1.colors ; c++)

void LibRaw::get_mem_image_format(int* width, int* height, int* colors, int* bps) const

{
    if (S.flip & 4) {
        *width = S.height;
        *height = S.width;
    }
    else {
        *width = S.width;
        *height = S.height;
    }
    *colors = P1.colors;
    *bps = O.output_bps;
}

int LibRaw::copy_mem_image(void* scan0, int stride, int bgr)

{
    // the image memory pointed to by scan0 is assumed to be in the format returned by get_mem_image_format
    if((imgdata.progress_flags & LIBRAW_PROGRESS_THUMB_MASK) < LIBRAW_PROGRESS_PRE_INTERPOLATE)
        return LIBRAW_OUT_OF_ORDER_CALL;

    if(libraw_internal_data.output_data.histogram)
        {
            int perc, val, total, t_white=0x2000,c;
            perc = S.width * S.height * 0.01;        /* 99th percentile white level */
            if (IO.fuji_width) perc /= 2;
            if (!((O.highlight & ~2) || O.no_auto_bright))
                for (t_white=c=0; c < P1.colors; c++) {
                    for (val=0x2000, total=0; --val > 32; )
                        if ((total += libraw_internal_data.output_data.histogram[c][val]) > perc) break;
                    if (t_white < val) t_white = val;
                }
             gamma_curve (O.gamm[0], O.gamm[1], 2, (t_white << 3)/O.bright);
        }

    int s_iheight = S.iheight;
    int s_iwidth = S.iwidth;
    int s_width = S.width;
    int s_hwight = S.height;

    S.iheight = S.height;
    S.iwidth  = S.width;

    if (S.flip & 4) SWAP(S.height,S.width);
    uchar *ppm;
    ushort *ppm2;
    int c, row, col, soff, rstep, cstep;

    soff  = flip_index (0, 0);
    cstep = flip_index (0, 1) - soff;
    rstep = flip_index (1, 0) - flip_index (0, S.width);

    for (row=0; row < S.height; row++, soff += rstep) 
        {
            uchar *bufp = ((uchar*)scan0)+row*stride;
            ppm2 = (ushort*) (ppm = bufp);
            // keep trivial decisions in the outer loop for speed
            if (bgr) {
                if (O.output_bps == 8) {
                    for (col=0; col < S.width; col++, soff += cstep) 
                        FORBGR *ppm++ = imgdata.color.curve[imgdata.image[soff][c]]>>8;
                }
                else {
                    for (col=0; col < S.width; col++, soff += cstep) 
                        FORBGR *ppm2++ = imgdata.color.curve[imgdata.image[soff][c]];
                }
            }
            else {
                if (O.output_bps == 8) {
                    for (col=0; col < S.width; col++, soff += cstep) 
                        FORRGB *ppm++ = imgdata.color.curve[imgdata.image[soff][c]]>>8;
                }
                else {
                    for (col=0; col < S.width; col++, soff += cstep) 
                        FORRGB *ppm2++ = imgdata.color.curve[imgdata.image[soff][c]];
                }
            }

//            bufp += stride;           // go to the next line
        }
 
    S.iheight = s_iheight;
    S.iwidth = s_iwidth;
    S.width = s_width;
    S.height = s_hwight;

    return 0;


}
#undef FORBGR
#undef FORRGB

 

libraw_processed_image_t *LibRaw::dcraw_make_mem_image(int *errcode)

{
    int width, height, colors, bps;
    get_mem_image_format(&width, &height, &colors, &bps);
    int stride = width * (bps/8) * colors;
    unsigned ds = height * stride;
    libraw_processed_image_t *ret = (libraw_processed_image_t*)::malloc(sizeof(libraw_processed_image_t)+ds);
    if(!ret)
        {
                if(errcode) *errcode= ENOMEM;
                return NULL;
        }
    memset(ret,0,sizeof(libraw_processed_image_t));

    // metadata init
    ret->type   = LIBRAW_IMAGE_BITMAP;
    ret->height = height;
    ret->width  = width;
    ret->colors = colors;
    ret->bits   = bps;
    ret->data_size = ds;
    copy_mem_image(ret->data, stride, 0); 

    return ret;
}

#undef FORC
#undef FORCC
#undef SWAP
#endif


int LibRaw::dcraw_ppm_tiff_writer(const char *filename)
{
    CHECK_ORDER_LOW(LIBRAW_PROGRESS_LOAD_RAW);

    if(!imgdata.image) 
        return LIBRAW_OUT_OF_ORDER_CALL;

    if(!filename) 
        return ENOENT;
    FILE *f = fopen(filename,"wb");

    if(!f) 
        return errno;

    try {
        if(!libraw_internal_data.output_data.histogram)
            {
                libraw_internal_data.output_data.histogram = 
                    (int (*)[LIBRAW_HISTOGRAM_SIZE]) malloc(sizeof(*libraw_internal_data.output_data.histogram)*4);
                merror(libraw_internal_data.output_data.histogram,"LibRaw::dcraw_ppm_tiff_writer()");
            }
        libraw_internal_data.internal_data.output = f;
        write_ppm_tiff();
        SET_PROC_FLAG(LIBRAW_PROGRESS_FLIP);
        libraw_internal_data.internal_data.output = NULL;
        fclose(f);
        return 0;
    }
    catch ( LibRaw_exceptions err) {
        fclose(f);
        EXCEPTION_HANDLER(err);
    }
}

void LibRaw::kodak_thumb_loader()
{
    // some kodak cameras
    ushort s_height = S.height, s_width = S.width,s_iwidth = S.iwidth,s_iheight=S.iheight;
    int s_colors = P1.colors;
    unsigned s_filters = P1.filters;
    ushort (*s_image)[4] = imgdata.image;

    
    S.height = T.theight;
    S.width  = T.twidth;
    P1.filters = 0;

    if (thumb_load_raw == &CLASS kodak_ycbcr_load_thumb) 
        {
            S.height += S.height & 1;
            S.width  += S.width  & 1;
        }
    
    imgdata.image = (ushort (*)[4]) calloc (S.iheight*S.iwidth, sizeof (*imgdata.image));
    merror (imgdata.image, "LibRaw::kodak_thumb_loader()");

    ID.input->seek(ID.toffset, SEEK_SET);
    // read kodak thumbnail into T.image[]
    (this->*thumb_load_raw)();

    // copy-n-paste from image pipe
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define LIM(x,min,max) MAX(min,MIN(x,max))
#define CLIP(x) LIM(x,0,65535)
#define SWAP(a,b) { a ^= b; a ^= (b ^= a); }

    // from scale_colors
    {
        double   dmax;
        float scale_mul[4];
        int c,val;
        for (dmax=DBL_MAX, c=0; c < 3; c++) 
                if (dmax > C.pre_mul[c])
                    dmax = C.pre_mul[c];

        for( c=0; c< 3; c++)
                scale_mul[c] = (C.pre_mul[c] / dmax) * 65535.0 / C.maximum;
        scale_mul[3] = scale_mul[1];

        size_t size = S.height * S.width;
        for (unsigned i=0; i < size*4 ; i++) 
            {
                val = imgdata.image[0][i];
                if(!val) continue;
                val *= scale_mul[i & 3];
                imgdata.image[0][i] = CLIP(val);
            }
    }

    // from convert_to_rgb
    ushort *img;
    int row,col;
    
    int  (*t_hist)[LIBRAW_HISTOGRAM_SIZE] =  (int (*)[LIBRAW_HISTOGRAM_SIZE]) calloc(sizeof(*t_hist),4);
    merror (t_hist, "LibRaw::kodak_thumb_loader()");
    
    float out[3], 
        out_cam[3][4] = 
        {
            {2.81761312, -1.98369181, 0.166078627, 0}, 
            {-0.111855984, 1.73688626, -0.625030339, 0}, 
            {-0.0379119813, -0.891268849, 1.92918086, 0}
        };

    for (img=imgdata.image[0], row=0; row < S.height; row++)
        for (col=0; col < S.width; col++, img+=4)
            {
                out[0] = out[1] = out[2] = 0;
                int c;
                for(c=0;c<3;c++) 
                    {
                        out[0] += out_cam[0][c] * img[c];
                        out[1] += out_cam[1][c] * img[c];
                        out[2] += out_cam[2][c] * img[c];
                    }
                for(c=0; c<3; c++)
                    img[c] = CLIP((int) out[c]);
                for(c=0; c<P1.colors;c++)
                    t_hist[c][img[c] >> 3]++;
                    
            }

    // from gamma_lut
    int  (*save_hist)[LIBRAW_HISTOGRAM_SIZE] = libraw_internal_data.output_data.histogram;
    libraw_internal_data.output_data.histogram = t_hist;

    // make curve output curve!
    ushort (*t_curve) = (ushort*) calloc(sizeof(C.curve),1);
    merror (t_curve, "LibRaw::kodak_thumb_loader()");
    memmove(t_curve,C.curve,sizeof(C.curve));
    memset(C.curve,0,sizeof(C.curve));
        {
            int perc, val, total, t_white=0x2000,c;

            perc = S.width * S.height * 0.01;		/* 99th percentile white level */
            if (IO.fuji_width) perc /= 2;
            if (!((O.highlight & ~2) || O.no_auto_bright))
                for (t_white=c=0; c < P1.colors; c++) {
                    for (val=0x2000, total=0; --val > 32; )
                        if ((total += libraw_internal_data.output_data.histogram[c][val]) > perc) break;
                    if (t_white < val) t_white = val;
                }
            gamma_curve (O.gamm[0], O.gamm[1], 2, (t_white << 3)/O.bright);
        }
    
    libraw_internal_data.output_data.histogram = save_hist;
    free(t_hist);
    
    // from write_ppm_tiff - copy pixels into bitmap
    
    S.iheight = S.height;
    S.iwidth  = S.width;
    if (S.flip & 4) SWAP(S.height,S.width);

    if(T.thumb) free(T.thumb);
    T.thumb = (char*) calloc (S.width * S.height, P1.colors);
    merror (T.thumb, "LibRaw::kodak_thumb_loader()");
    T.tlength = S.width * S.height * P1.colors;

    // from write_tiff_ppm
    {
        int soff  = flip_index (0, 0);
        int cstep = flip_index (0, 1) - soff;
        int rstep = flip_index (1, 0) - flip_index (0, S.width);
        
        for (int row=0; row < S.height; row++, soff += rstep) 
            {
                char *ppm = T.thumb + row*S.width*P1.colors;
                for (int col=0; col < S.width; col++, soff += cstep)
                    for(int c = 0; c < P1.colors; c++)
                        ppm [col*P1.colors+c] = imgdata.color.curve[imgdata.image[soff][c]]>>8;
            }
    }

    memmove(C.curve,t_curve,sizeof(C.curve));
    free(t_curve);

    // restore variables
    free(imgdata.image);
    imgdata.image  = s_image;
    
    T.twidth = S.width;
    S.width = s_width;

    S.iwidth = s_iwidth;
    S.iheight = s_iheight;

    T.theight = S.height;
    S.height = s_height;

    T.tcolors = P1.colors;
    P1.colors = s_colors;

    P1.filters = s_filters;
}
#undef MIN
#undef MAX
#undef LIM
#undef CLIP
#undef SWAP




// ������� thumbnail �� �����, ������ thumb_format � ������������ � ��������
int LibRaw::unpack_thumb(void)
{
    CHECK_ORDER_LOW(LIBRAW_PROGRESS_IDENTIFY);
    CHECK_ORDER_BIT(LIBRAW_PROGRESS_THUMB_LOAD);

    try {
        if ( !ID.toffset) 
            {
                return LIBRAW_NO_THUMBNAIL;
            } 
        else if (thumb_load_raw) 
            {
                kodak_thumb_loader();
                T.tformat = LIBRAW_THUMBNAIL_BITMAP;
                SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
                return 0;
            } 
        else 
            {
                ID.input->seek(ID.toffset, SEEK_SET);
                if ( write_thumb == &LibRaw::jpeg_thumb)
                    {
                        if(T.thumb) free(T.thumb);
                        T.thumb = (char *) malloc (T.tlength);
                        merror (T.thumb, "jpeg_thumb()");
                        ID.input->read (T.thumb, 1, T.tlength);
                        T.tcolors = 3;
                        T.tformat = LIBRAW_THUMBNAIL_JPEG;
                        SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
                        return 0;
                    }
                else if (write_thumb == &LibRaw::ppm_thumb)
                    {
                        T.tlength = T.twidth * T.theight*3;
                        if(T.thumb) free(T.thumb);

                        T.thumb = (char *) malloc (T.tlength);
                        merror (T.thumb, "ppm_thumb()");

                        ID.input->read(T.thumb, 1, T.tlength);

                        T.tformat = LIBRAW_THUMBNAIL_BITMAP;
                        SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
                        return 0;

                    }
                else if (write_thumb == &LibRaw::foveon_thumb)
                    {
                        foveon_thumb_loader();
                        // may return with error, so format is set in
                        // foveon thumb loader itself
                        SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
                        return 0;
                    }
                // else if -- all other write_thumb cases!
                else
                    {
                        return LIBRAW_UNSUPPORTED_THUMBNAIL;
                    }
            }
        // last resort
        return LIBRAW_UNSUPPORTED_THUMBNAIL;
    }
    catch ( LibRaw_exceptions err) {
        EXCEPTION_HANDLER(err);
    }

}

int LibRaw::dcraw_thumb_writer(const char *fname)
{
//    CHECK_ORDER_LOW(LIBRAW_PROGRESS_THUMB_LOAD);

    if(!fname) 
        return ENOENT;
        
    FILE *tfp = fopen(fname,"wb");
    
    if(!tfp) 
        return errno;

    if(!T.thumb)
	{
		fclose(tfp);
        	return LIBRAW_OUT_OF_ORDER_CALL;
	}

    try {
        switch (T.tformat)
            {
            case LIBRAW_THUMBNAIL_JPEG:
                jpeg_thumb_writer (tfp,T.thumb,T.tlength);
                break;
            case LIBRAW_THUMBNAIL_BITMAP:
                fprintf (tfp, "P6\n%d %d\n255\n", T.twidth, T.theight);
                fwrite (T.thumb, 1, T.tlength, tfp);
                break;
            default:
                fclose(tfp);
                return LIBRAW_UNSUPPORTED_THUMBNAIL;
           }
        fclose(tfp);
        return 0;
    }
    catch ( LibRaw_exceptions err) {
        fclose(tfp);
        EXCEPTION_HANDLER(err);
    }
}

int LibRaw::adjust_sizes_info_only(void)
{
    CHECK_ORDER_LOW(LIBRAW_PROGRESS_IDENTIFY);

    raw2image_start();
    if (O.use_fuji_rotate)
        {
            if (IO.fuji_width) 
                {
                    // restore saved values
                    if(IO.fheight)
                        {
                            S.height = IO.fheight;
                            S.width = IO.fwidth;
                            S.iheight = (S.height + IO.shrink) >> IO.shrink;
                            S.iwidth  = (S.width  + IO.shrink) >> IO.shrink;
                            S.raw_height -= 2*S.top_margin;
                            IO.fheight = IO.fwidth = 0; // prevent repeated calls
                        }
                    // dcraw code
                    IO.fuji_width = (IO.fuji_width - 1 + IO.shrink) >> IO.shrink;
                    S.iwidth = (ushort)(IO.fuji_width / sqrt(0.5));
                    S.iheight = (ushort)( (S.iheight - IO.fuji_width) / sqrt(0.5));
                } 
            else 
                {
                    if (S.pixel_aspect < 1) S.iheight = (ushort)( S.iheight / S.pixel_aspect + 0.5);
                    if (S.pixel_aspect > 1) S.iwidth  = (ushort) (S.iwidth  * S.pixel_aspect + 0.5);
                }
        }
    SET_PROC_FLAG(LIBRAW_PROGRESS_FUJI_ROTATE);
    if ( S.flip & 4)
        {
            unsigned short t = S.iheight;
            S.iheight=S.iwidth;
            S.iwidth = t;
            SET_PROC_FLAG(LIBRAW_PROGRESS_FLIP);
        }
    return 0;
}

int LibRaw::wf_remove_banding()
{
#define WF_IMGMODE_BAYER4PLANE 4
#define WF_IMGMODE_BAYER1PLANE 1

#define WF_GREENMODE_IND   0
#define WF_GREENMODE_GX_XG 1
#define WF_GREENMODE_XG_GX 2

#define WF_DEBANDING_OK          0
#define WF_DEBANDING_NOTBAYER2X2 1
#define WF_DEBANDING_TOOSMALL    2

#define WF_GAUSS_PIRAMID_SIZE 4

#define WF_MAXTRESHOLD 65536

#define WF_BAYERSRC(row, col, c) ((ushort(*)[4])imgdata.image)[((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)][c] 
#define WF_BAYERGAU(l, row, col) (gauss_pyramid[l])[((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)] 
#define WF_BAYERDFG(l, row, col) (difwg_pyramid[l])[((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)] 
	
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define WF_i_1TO4 for(int i=0; i<4; i++)

	// too small?

	if (S.width<128 || S.height<128)
		return WF_DEBANDING_TOOSMALL;

	// is 2x2 bayer? 

	int bayer2x2flag=-1;

	for(int row_shift=0; row_shift<=8; row_shift+=2)
	{
		for(int col_shift=0; col_shift<=8; col_shift+=2)
		{
			if ((FC(0,0)!=FC(row_shift,   col_shift))   ||
				(FC(1,0)!=FC(row_shift+1, col_shift))   ||
				(FC(0,1)!=FC(row_shift,   col_shift+1)) ||
				(FC(1,1)!=FC(row_shift+1, col_shift+1)))
			{
				bayer2x2flag=0;
			}
		}
	}

	if (bayer2x2flag==0)
		return WF_DEBANDING_NOTBAYER2X2;

	int    x_green_flag = -1;

	int    width_d2,    height_d2;
	int    width_p1_d2, height_p1_d2;

	width_d2  = S.width/2;
	height_d2 = S.height/2;
	
	width_p1_d2  = (S.width+1)/2;
	height_p1_d2 = (S.height+1)/2;

	ushort  val_max_c[4]={0,0,0,0};
	ushort  val_max;

	ushort  dummy_pixel=0;
	ushort *dummy_line;

	dummy_line = (ushort*)calloc(S.width, sizeof(ushort)*4);

	for(int i=0; i<S.width*4; i++)
		dummy_line[i]=0;

	// Searching max value for increasing bit-depth

	for(int row_d2=0; row_d2<height_p1_d2; row_d2++)
	{
		int     row, row_p1;
		ushort *src[4];
		ushort *src_first, *src_plast, *src_last;
		
		row    = row_d2*2;
		row_p1 = row+1;

		WF_i_1TO4 src[i] = &WF_BAYERSRC((i<2)?row:row_p1, i&1, FC((i<2)?row:row_p1, i&1));

		if (row_p1==S.height)
			src[2]=src[3]=dummy_line;

		src_first   = &WF_BAYERSRC(row,   0,               FC(row,   0));
		src_plast   = &WF_BAYERSRC(row,   width_d2*2-2,    FC(row,   0));
		src_last    = &WF_BAYERSRC(row,   width_p1_d2*2-2, FC(row,   0));

		do
		{
			// Do

			WF_i_1TO4 val_max_c[i]=MAX(val_max_c[i], *src[i]);

			// Next 4 pixel or exit

			if     (src[0]<src_plast)
			{
				WF_i_1TO4 src[i]+=8;
			}
			else if(src[0]>src_first && src[0]<src_last)
			{
				WF_i_1TO4 src[i]=i&1?&dummy_pixel:src[i]+8;
			}
			else break;

		}
		while(1);
	}

	val_max=MAX(MAX(val_max_c[0], val_max_c[1]), MAX(val_max_c[2], val_max_c[3]));
	
	// end of searching max value

	if (val_max==0)
		return WF_DEBANDING_OK;
		
	int data_shift;
	int data_mult;
	int val_max_s;

	data_shift = 15;
	val_max_s  = val_max;

	if (val_max_s >= (1 << 8)) { val_max_s >>= 8; data_shift -=  8; }
	if (val_max_s >= (1 << 4)) { val_max_s >>= 4; data_shift -=  4; }
	if (val_max_s >= (1 << 2)) { val_max_s >>= 2; data_shift -=  2; }
	if (val_max_s >= (1 << 1)) {                  data_shift -=  1; }
  
	data_mult = 1<<data_shift;
	val_max <<= data_shift;
		
	// Bit shift

	for(int row_d2=0; row_d2<height_p1_d2; row_d2++)
	{
		int     row, row_p1;
		ushort *src[4];
		ushort *src_first, *src_plast, *src_last;
		
		row    = row_d2*2;
		row_p1 = row+1;

		WF_i_1TO4 src[i] = &WF_BAYERSRC((i<2)?row:row_p1, i&1, FC((i<2)?row:row_p1, i&1));

		if (row_p1==S.height)
			src[2]=src[3]=dummy_line;

		src_first   = &WF_BAYERSRC(row,   0,               FC(row,   0));
		src_plast   = &WF_BAYERSRC(row,   width_d2*2-2,    FC(row,   0));
		src_last    = &WF_BAYERSRC(row,   width_p1_d2*2-2, FC(row,   0));

		do
		{
			// Do

			WF_i_1TO4 (*src[i])<<=data_shift;

			// Next 4 pixel or exit

			if     (src[0]<src_plast)
			{
				WF_i_1TO4 src[i]+=8;
			}
			else if(src[0]>src_first && src[0]<src_last)
			{
				WF_i_1TO4 src[i]=i&1?&dummy_pixel:src[i]+8;
			}
			else break;

		}
		while(1);
	}

	ushort *gauss_pyramid[WF_GAUSS_PIRAMID_SIZE];
	ushort *difwg_pyramid[WF_GAUSS_PIRAMID_SIZE];

	for(int i=0; i<WF_GAUSS_PIRAMID_SIZE; i++)
	{
		gauss_pyramid[i] = (ushort*)calloc(S.width*S.height, sizeof(ushort));
		difwg_pyramid[i] = (ushort*)calloc(S.width*S.height, sizeof(ushort));
	}

	int radius3x3 [4]={3,  3,  3,  0}; // as gau r=24
	int radius3x14[4]={14, 14, 14, 0}; // as gau r=420
	int radius3x45[4]={45, 45, 45, 0}; // as gau r=4140

	// Making 4-level gaussian pyramid

	if (x_green_flag)
	{
		wf_bayer4_green_blur   (0,          imgdata.image,    WF_IMGMODE_BAYER4PLANE, gauss_pyramid[0], WF_IMGMODE_BAYER1PLANE);
		wf_bayer4_igauss_filter(1,          gauss_pyramid[0], WF_IMGMODE_BAYER1PLANE, gauss_pyramid[0], WF_IMGMODE_BAYER1PLANE);
	}
	else
	{
		wf_bayer4_igauss_filter(1,          imgdata.image,    WF_IMGMODE_BAYER4PLANE, gauss_pyramid[0], WF_IMGMODE_BAYER1PLANE);
	}

	wf_bayer4_block_filter (radius3x3,  gauss_pyramid[0], WF_IMGMODE_BAYER1PLANE, gauss_pyramid[1], WF_IMGMODE_BAYER1PLANE); // as gau r=24
	wf_bayer4_block_filter (radius3x14, gauss_pyramid[0], WF_IMGMODE_BAYER1PLANE, gauss_pyramid[2], WF_IMGMODE_BAYER1PLANE); // as gau r=420
	wf_bayer4_block_filter (radius3x45, gauss_pyramid[0], WF_IMGMODE_BAYER1PLANE, gauss_pyramid[3], WF_IMGMODE_BAYER1PLANE); // as gau r=4140 

	
	// Energy multiplyers for laplasyan pyramid

	float dfg_mult[WF_GAUSS_PIRAMID_SIZE]={1.560976, 8.196011, 180.413773, 3601.427246/3.0};

/*	dif_mult[0]=1.0/wf_filter_energy(0, 0,   0,    1);
	dif_mult[1]=1.0/wf_filter_energy(0, 1,   0,   24);
	dif_mult[2]=1.0/wf_filter_energy(0, 24,  0,  420);
	dif_mult[3]=1.0/wf_filter_energy(0, 420, 0, 4140);*/

	float dfg_mulg[WF_GAUSS_PIRAMID_SIZE]={1.235223, 19.813868, 365.148407, 7208.362793/3.0};

/*	dif_mulg[0]=1.0/wf_filter_energy(0, 0,    1,    1);
	dif_mulg[1]=1.0/wf_filter_energy(1, 1,    1,   24);
	dif_mulg[2]=1.0/wf_filter_energy(1, 24,   1,  420);
	dif_mulg[3]=1.0/wf_filter_energy(1, 420,  1, 4140);*/

	float    dfg_mlcc[WF_GAUSS_PIRAMID_SIZE][4];
	long int dfg_dmax[WF_GAUSS_PIRAMID_SIZE][4];

	int green_mode;

	if      ( x_green_flag && (imgdata.idata.cdesc[FC(0, 0)] == imgdata.idata.cdesc[FC(1, 1)]) )
		green_mode = WF_GREENMODE_GX_XG;
	else if ( x_green_flag && (imgdata.idata.cdesc[FC(0, 1)] == imgdata.idata.cdesc[FC(1, 0)]) )
		green_mode = WF_GREENMODE_XG_GX;
	else
		green_mode = WF_GREENMODE_IND;
	
	for(int l=0; l<WF_GAUSS_PIRAMID_SIZE; l++)
	{
		switch (green_mode)
		{
			case WF_GREENMODE_GX_XG:

				dfg_mlcc[l][0]=dfg_mlcc[l][3]=dfg_mulg[l];
				dfg_dmax[l][0]=dfg_dmax[l][3]=65535/dfg_mulg[l];

				dfg_mlcc[l][1]=dfg_mlcc[l][2]=dfg_mult[l];
				dfg_dmax[l][1]=dfg_dmax[l][2]=65535/dfg_mult[l];
				
				break;

			case WF_GREENMODE_XG_GX:

				dfg_mlcc[l][1]=dfg_mlcc[l][2]=dfg_mulg[l];
				dfg_dmax[l][1]=dfg_dmax[l][2]=65535/dfg_mulg[l];

				dfg_mlcc[l][0]=dfg_mlcc[l][3]=dfg_mult[l];
				dfg_dmax[l][0]=dfg_dmax[l][3]=65535/dfg_mult[l];
				
				break;
			
			case WF_GREENMODE_IND:

				dfg_mlcc[l][0]=dfg_mlcc[l][1]=dfg_mlcc[l][2]=dfg_mlcc[l][3]=dfg_mult[l];
				dfg_dmax[l][0]=dfg_dmax[l][1]=dfg_dmax[l][2]=dfg_dmax[l][3]=65535/dfg_mult[l];
				
				break;
		}
	}

	// laplasyan energy

	for(int row_d2=0; row_d2<height_p1_d2; row_d2++)
	{
		int     row, row_p1;
		ushort *src[4];

		ushort *gau[WF_GAUSS_PIRAMID_SIZE][4];
		ushort *dfg[WF_GAUSS_PIRAMID_SIZE][4];

		row    = row_d2*2;
		row_p1 = row+1;

		WF_i_1TO4 src[i] = &WF_BAYERSRC((i<2)?row:row_p1, i&1, FC((i<2)?row:row_p1, i&1));

		if (row_p1==S.height)
			src[2]=src[3]=dummy_line;

		for(int l=0; l<WF_GAUSS_PIRAMID_SIZE; l++)
		{
			WF_i_1TO4 gau[l][i] = &WF_BAYERGAU(l, (i<2)?row:row_p1, i&1);

			WF_i_1TO4 dfg[l][i] = &WF_BAYERDFG(l, (i<2)?row:row_p1, i&1);

			if ((row+1)==S.height)
				dfg[l][2]=dfg[l][3]=gau[l][2]=gau[l][3]=dummy_line;
		}

		ushort *src_first, *src_last, *src_last2;

		src_first   = &WF_BAYERSRC(row,   0,               FC(row,   0));
		src_last    = &WF_BAYERSRC(row,   width_d2*2-2,    FC(row,   0));
		src_last2   = &WF_BAYERSRC(row,   width_p1_d2*2-2, FC(row,   0));

		do
		{
			long int val_gau[4];
			long int val_dif[4];
			long int val_src[4];

			WF_i_1TO4 val_src[i]=*src[i];

			for(int l=0; l<WF_GAUSS_PIRAMID_SIZE; l++)
			{
				WF_i_1TO4 val_gau[i]=*gau[l][i];
				WF_i_1TO4 val_dif[i]=val_src[i]-val_gau[i];
				WF_i_1TO4 val_src[i]=val_gau[i];
				WF_i_1TO4 val_dif[i]*=val_dif[i];

				WF_i_1TO4
					if(val_dif[i]<dfg_dmax[l][i])
					{
						val_dif[i]*=dfg_mlcc[l][i];
						*dfg[l][i] =val_dif[i];
					}
					else
					{
						*dfg[l][i]=65535;
					}
			}

			// Next 4 pixel or exit

			if     (src[0]<src_last)
			{
				WF_i_1TO4 src[i]+=8;

				for (int l=0; l<WF_GAUSS_PIRAMID_SIZE; l++)
					WF_i_1TO4 gau[l][i]+=2;

				for (int l=0; l<WF_GAUSS_PIRAMID_SIZE; l++)
					WF_i_1TO4 dfg[l][i]+=2;
			}
			else if(src[0]>src_first && src[0]<src_last2)
			{
				WF_i_1TO4 src[i]=i&1?&dummy_pixel:src[i]+8;

				for (int l=0; l<WF_GAUSS_PIRAMID_SIZE; l++)
					WF_i_1TO4 gau[l][i]=i&1?&dummy_pixel:gau[l][i]+2;

				for (int l=0; l<WF_GAUSS_PIRAMID_SIZE; l++)
					WF_i_1TO4 dfg[l][i]=i&1?&dummy_pixel:dfg[l][i]+2;
			}
			else break;
		}
		while(1);
	}
	
	int radius2x32 [3]={32, 32, 0};
	int radius2x56 [3]={56, 56, 0};
	int radius2x90 [3]={90, 90, 0};
	int radius2x104[3]={104, 104, 0};

	if (x_green_flag)
	{
		for(int i=0;i<4;i++)
			wf_bayer4_green_blur   (0,           difwg_pyramid[i], WF_IMGMODE_BAYER1PLANE, difwg_pyramid[i], WF_IMGMODE_BAYER1PLANE);
	}

	wf_bayer4_block_filter (radius2x32,  difwg_pyramid[0], WF_IMGMODE_BAYER1PLANE, difwg_pyramid[0], WF_IMGMODE_BAYER1PLANE);
	wf_bayer4_block_filter (radius2x56,  difwg_pyramid[1], WF_IMGMODE_BAYER1PLANE, difwg_pyramid[1], WF_IMGMODE_BAYER1PLANE);
	wf_bayer4_block_filter (radius2x90,  difwg_pyramid[2], WF_IMGMODE_BAYER1PLANE, difwg_pyramid[2], WF_IMGMODE_BAYER1PLANE);
	wf_bayer4_block_filter (radius2x104, difwg_pyramid[3], WF_IMGMODE_BAYER1PLANE, difwg_pyramid[3], WF_IMGMODE_BAYER1PLANE);

	float (*banding_col)[4];
	float (*banding_row)[4];
	float (*banding_col_count)[4];
	float (*banding_row_count)[4];

	banding_col       = (float(*)[4])calloc(height_p1_d2, sizeof(float)*4);
	banding_col_count = (float(*)[4])calloc(height_p1_d2, sizeof(float)*4);

	banding_row       = (float(*)[4])calloc(width_p1_d2, sizeof(float)*4);
	banding_row_count = (float(*)[4])calloc(width_p1_d2, sizeof(float)*4);

	for(int row_d2=0; row_d2<height_p1_d2; row_d2++)
		WF_i_1TO4 banding_col[row_d2][i]=banding_col_count[row_d2][i]=0;

	for(int col_d2=0; col_d2<width_p1_d2; col_d2++)
		WF_i_1TO4 banding_row[col_d2][i]=banding_row_count[col_d2][i]=0;

	long int val_accepted;
	float    treshold[4];

	WF_i_1TO4 treshold[i]=imgdata.params.wf_deband_treshold[FC(i>>1,i&1)];

	val_accepted = val_max-3*MAX(MAX(treshold[0],treshold[1]),MAX(treshold[2],treshold[3]));

	float (*tr_weight)[4];

	tr_weight=(float(*)[4])calloc(WF_MAXTRESHOLD*4, sizeof(float));

	WF_i_1TO4 treshold[i]*=data_mult;

	for(int v=0; v<WF_MAXTRESHOLD; v++)
	{
		for(int i=0; i<4; i++)
		{
			if (v<treshold[i]*treshold[i])
				tr_weight[v][i] = 1.0;
			else if (v*5<6*treshold[i]*treshold[i])
				tr_weight[v][i] = 6.0-5.0*float(v)/(treshold[i]*treshold[i]);
			else
				tr_weight[v][i] = 0.0;
		}
	}

	for(int row_d2=0; row_d2<height_p1_d2; row_d2++)
	{
		int     row, row_p1;
		ushort *src[4];

		ushort *gau[WF_GAUSS_PIRAMID_SIZE][4];
		ushort *dfg[WF_GAUSS_PIRAMID_SIZE][4];

		row    = row_d2*2;
		row_p1 = row+1;

		WF_i_1TO4 src[i] = &WF_BAYERSRC((i<2)?row:row_p1, i&1, FC((i<2)?row:row_p1, i&1));

		if (row_p1==S.height)
			src[2]=src[3]=dummy_line;

		for(int l=0; l<WF_GAUSS_PIRAMID_SIZE; l++)
		{
			WF_i_1TO4 gau[l][i] = &WF_BAYERGAU(l, (i<2)?row:row_p1, i&1);

			WF_i_1TO4 dfg[l][i] = &WF_BAYERDFG(l, (i<2)?row:row_p1, i&1);

			if (row_p1==S.height)
				dfg[l][2]=dfg[l][3]=gau[l][2]=gau[l][3]=dummy_line;
		}

		ushort *src_first, *src_last, *src_last2;

		src_first   = &WF_BAYERSRC(row,   0,               FC(row,   0));
		src_last    = &WF_BAYERSRC(row,   width_d2*2-2,    FC(row,   0));
		src_last2   = &WF_BAYERSRC(row,   width_p1_d2*2-2, FC(row,   0));

		int col_d2 = 0;

		do
		{
			float val_src[4];

			float bsum[4]={0,0,0,0};
			float wsum[4]={0,0,0,0};

			WF_i_1TO4 val_src[i]=*src[i];

			for(int l=0; l<WF_GAUSS_PIRAMID_SIZE; l++)
			{
				float val_dif[4];
				float val_gau[4];
				float wght[4];

				WF_i_1TO4 val_gau[i] =  *gau[l][i];
				WF_i_1TO4 val_dif[i] =  val_src[i]-val_gau[i];
				WF_i_1TO4 val_src[i] =  val_gau[i];

				WF_i_1TO4 wght[i]    =  tr_weight[*dfg[l][i]][i];
				WF_i_1TO4 wsum[i]    += wght[i];
				WF_i_1TO4 bsum[i]    += wght[i]*val_dif[i];
			}

			//WF_i_1TO4 *src[i]=bsum[i];

			WF_i_1TO4 wsum[i]*=wsum[i];

			WF_i_1TO4 banding_col      [row_d2][i] += bsum[i]*wsum[i];
			WF_i_1TO4 banding_col_count[row_d2][i] += wsum[i];

			WF_i_1TO4 banding_row      [col_d2][i] += bsum[i]*wsum[i];
			WF_i_1TO4 banding_row_count[col_d2][i] += wsum[i];

			// Next 4 pixel or exit

			if     (src[0]<src_last)
			{
				WF_i_1TO4 src[i]+=8;

				for (int l=0; l<WF_GAUSS_PIRAMID_SIZE; l++)
					WF_i_1TO4 gau[l][i]+=2;

				for (int l=0; l<WF_GAUSS_PIRAMID_SIZE; l++)
					WF_i_1TO4 dfg[l][i]+=2;
			}
			else if(src[0]>src_first && src[0]<src_last2)
			{
				WF_i_1TO4 src[i]=i&1?&dummy_pixel:src[i]+8;

				for (int l=0; l<WF_GAUSS_PIRAMID_SIZE; l++)
					WF_i_1TO4 gau[l][i]=i&1?&dummy_pixel:gau[l][i]+2;

				for (int l=0; l<WF_GAUSS_PIRAMID_SIZE; l++)
					WF_i_1TO4 dfg[l][i]=i&1?&dummy_pixel:dfg[l][i]+2;
			}
			else break;

			col_d2++;
		}
		while(1);
	}

	float bsum[4], bmean[4];

	int (*banding_col_i)[4];
	int (*banding_row_i)[4];

	banding_col_i = (int(*)[4])calloc(height_p1_d2, sizeof(int)*4);
	banding_row_i = (int(*)[4])calloc(width_p1_d2,  sizeof(int)*4);
	
	// cols

	WF_i_1TO4 bsum[i]=bmean[i]=0;

	for(int row_d2=0; row_d2<height_p1_d2; row_d2++)
		for(int i=0; i<4; i++)
		{
			if (banding_col_count[row_d2][i]>0)
			{
				banding_col[row_d2][i]=banding_col[row_d2][i]/banding_col_count[row_d2][i];
				bsum[i]+=banding_col[row_d2][i];
			}
		}

	WF_i_1TO4 bmean[i]=bsum[i]/(i<2?height_d2:height_p1_d2);
		
	for(int row_d2=0; row_d2<height_p1_d2; row_d2++)
		for(int i=0; i<4; i++)
			banding_col_i[row_d2][i]=int(banding_col[row_d2][i]-bmean[i]);

	// rows

	WF_i_1TO4 bsum[i]=bmean[i]=0;

	for(int col_d2=0; col_d2<width_p1_d2; col_d2++)
		for(int i=0; i<4; i++)
		{
			if (banding_row_count[col_d2][i]>0)
			{
				banding_row[col_d2][i]=(banding_row[col_d2][i]/banding_row_count[col_d2][i]);
				bsum[i]+=banding_row[col_d2][i];
			}
		}

	WF_i_1TO4 bmean[i]=bsum[i]/(i<2?width_d2:width_p1_d2);

	for(int col_d2=0; col_d2<width_p1_d2; col_d2++)
		for(int i=0; i<4; i++)
			if (banding_row_count[col_d2][i]>0)
				banding_row_i[col_d2][i]=int(banding_row[col_d2][i]-bmean[i]);

	for(int row_d2=0; row_d2<height_p1_d2; row_d2++)
	{
		int     row, row_p1;
		ushort *src[4];
		ushort *src_first, *src_plast, *src_last;
		
		row    = row_d2*2;
		row_p1 = row+1;

		WF_i_1TO4 src[i] = &WF_BAYERSRC((i<2)?row:row_p1, i&1, FC((i<2)?row:row_p1, i&1));

		if (row_p1==S.height)
			src[2]=src[3]=dummy_line;

		src_first   = &WF_BAYERSRC(row,   0,               FC(row,   0));
		src_plast   = &WF_BAYERSRC(row,   width_d2*2-2,    FC(row,   0));
		src_last    = &WF_BAYERSRC(row,   width_p1_d2*2-2, FC(row,   0));

		int col_d2=0;

		do
		{
			// Do

			int val_new[4];

			WF_i_1TO4 val_new[i]=*src[i];
			WF_i_1TO4 val_new[i]-=banding_col_i[row_d2][i];
			WF_i_1TO4 val_new[i]-=banding_row_i[col_d2][i];

			for(int i=0; i<4; i++)
			{
				if (*src[i]>=val_accepted)
				{
					val_new[i]=*src[i]>>data_shift;
				}
				else
				{
					if      (val_new[i]>val_max)
						val_new[i]=val_max;
					else if (val_new[i]<0)
						val_new[i]=0;

					val_new[i]>>=data_shift;
				}
			}

			WF_i_1TO4 *src[i]=val_new[i];

			// Next 4 pixel or exit

			if     (src[0]<src_plast)
			{
				WF_i_1TO4 src[i]+=8;
			}
			else if(src[0]>src_first && src[0]<src_last)
			{
				WF_i_1TO4 src[i]=i&1?&dummy_pixel:src[i]+8;
			}
			else break;

			col_d2++;
		}
		while(1);
	}

	free(banding_col_i);
	free(banding_row_i);

	free(tr_weight);
	
	free(banding_col);
	free(banding_col_count);

	free(banding_row);
	free(banding_row_count);

	for(int i=0; i<WF_GAUSS_PIRAMID_SIZE; i++)
	{
		free(gauss_pyramid[i]);
		free(difwg_pyramid[i]);
	}

	free(dummy_line);
	
}

double LibRaw::wf_filter_energy(int r1_greenmode, int r1, int r2_greenmode, int r2)
{
	/* 
		This function caclulates energy of laplasyan piramid level.
		Laplasyan level is difference between two 2D gaussian (exactly, binominal) convolutions with radius r1 and r2
		Convolution is done on bayer data, 4 channels, and if (greenmode), additive on green channel.
	
		Not optimized, because now it's used only for precalculations.
	*/


#define WF_MAXFILTERSIZE 10000

	int rmin, rmax;
	int rmin_greenmode, rmax_greenmode;

	if (r1>r2)
	{
		rmax=r1;
		rmin=r2;
		rmax_greenmode=r1_greenmode;
		rmin_greenmode=r2_greenmode;
	}
	else
	{
		rmax=r2;
		rmin=r1;
		rmax_greenmode=r2_greenmode;
		rmin_greenmode=r1_greenmode;
	}


	int rmin_x2_p1, rmax_x2_p1;
	rmin_x2_p1=rmin*2+1;
	rmax_x2_p1=rmax*2+1;

	double gau_kernel_rmin[WF_MAXFILTERSIZE];
	double gau_kernel_rmax[WF_MAXFILTERSIZE];

	for(int i=0; i<rmax_x2_p1; i++)
		gau_kernel_rmin[i]=0;

	gau_kernel_rmin[1]=1.0;
				
	for(int i=2; i<=rmin_x2_p1; i++)
	{
		for(int j=i; j>0; j--)
			gau_kernel_rmin[j]=0.5*(gau_kernel_rmin[j]+gau_kernel_rmin[j-1]);
	}

	for(int i=0; i<=rmax_x2_p1; i++)
		gau_kernel_rmax[i]=gau_kernel_rmin[i];

	for(int i=rmin_x2_p1+1; i<=rmax_x2_p1; i++)
	{
		for(int j=i; j>0; j--)
			gau_kernel_rmax[j]=0.5*(gau_kernel_rmax[j]+gau_kernel_rmax[j-1]);
	}

	double wmin_sum, wmax_sum, energy_sum;

	wmin_sum=0;
	wmax_sum=0;
	energy_sum=0;

	for(int row=-rmax*2-1; row<=rmax*2+1; row++)
	{
		for(int col=-rmax*2-1; col<=rmax*2+1; col++)
		{
			double wght_rmax=0;
			double wght_rmin=0;

#define WF_WMAX(row, col) (((abs(row)<=rmax*2)&&(abs(col)<=rmax*2))?gau_kernel_rmax[abs(row)/2+rmax+1]*gau_kernel_rmax[abs(col)/2+rmax+1]:0)
#define WF_WMIN(row, col) (((abs(row)<=rmin*2)&&(abs(col)<=rmin*2))?gau_kernel_rmin[abs(row)/2+rmin+1]*gau_kernel_rmin[abs(col)/2+rmin+1]:0)

			if ( ((row&1)==0) && ((col&1)==0))
			{
				wght_rmax = WF_WMAX(row, col);
				wght_rmin = WF_WMIN(row, col);
			}

			if (rmax_greenmode)
			{
				if ( ((row&1)==0) && ((col&1)==0))
					wght_rmax = 0.5*wght_rmax;
				else if ( ((row&1)==1) && ((col&1)==1))
				{
					wght_rmax = 0.125*(WF_WMAX(row-1, col-1)+WF_WMAX(row-1, col+1)+WF_WMAX(row+1, col-1)+WF_WMAX(row+1, col+1));
				}
			}

			if (rmin_greenmode)
			{
				if ( ((row&1)==0) && ((col&1)==0))
					wght_rmin = 0.5*wght_rmin;
				else if ( ((row&1)==1) && ((col&1)==1))
				{
					wght_rmin = 0.125*(WF_WMIN(row-1, col-1)+WF_WMIN(row-1, col+1)+WF_WMIN(row+1, col-1)+WF_WMIN(row+1, col+1));
				}
			}

			wmin_sum+=wght_rmin;
			wmax_sum+=wght_rmax;

			energy_sum+=(wght_rmax-wght_rmin)*(wght_rmax-wght_rmin);
		}
		
	}

	return energy_sum;
}

void LibRaw::wf_bayer4_green_blur(int mode, void* src_image, int src_imgmode, void* dst_image, int dst_imgmode)
{
	/* 
		This function filters green (or any "diagonal") channel of bayer4 data with "X" kernel,

		1 1
		 4
		1 1
	*/

#define WF_BAYERSRC4(row, col, c) ((ushort(*)[4])src_image)[((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)][c] 
#define WF_BAYERSRC1(row, col)    ((ushort*)src_image)     [((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)] 
#define WF_BAYERDST4(row, col, c) ((ushort(*)[4])dst_image)[((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)][c] 
#define WF_BAYERDST1(row, col)    ((ushort*)dst_image)     [((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)]

	int green_mode;

	if ( imgdata.idata.cdesc[FC(0, 0)] == imgdata.idata.cdesc[FC(1, 1)] )
		green_mode = WF_GREENMODE_GX_XG;
	else if ( imgdata.idata.cdesc[FC(0, 1)] == imgdata.idata.cdesc[FC(1, 0)] )
		green_mode = WF_GREENMODE_XG_GX;
	else
		green_mode = WF_GREENMODE_IND;

	int src_h_shift, dst_h_shift, src_h_shift_x2;

	if      (src_imgmode == WF_IMGMODE_BAYER1PLANE)
		src_h_shift = 2 >> IO.shrink;
	else if (src_imgmode == WF_IMGMODE_BAYER4PLANE)
		src_h_shift = 8 >> IO.shrink;

	src_h_shift_x2 = src_h_shift*2;

	if      (dst_imgmode == WF_IMGMODE_BAYER1PLANE)
		dst_h_shift = 2 >> IO.shrink;
	else if (dst_imgmode == WF_IMGMODE_BAYER4PLANE)
		dst_h_shift = 8 >> IO.shrink;

	int row, col;

	long int *line_filtered;

	line_filtered = (long int*) calloc(S.width, sizeof(*line_filtered));
	
	ushort *src, *src_c, *src_u1, *src_u2, *src_d1, *src_d2, *dst_c, *src_ca, *dst_ca, *dst_rb; 
	int start_col, start_col_left, row_up, row_dn;

	if ( green_mode != WF_GREENMODE_IND)
	{
		for(row=0; row<S.height; row++)
		{

			if (row == 0)
				row_up = 1;
			else
				row_up = row-1;

			if (row == S.height-1)
				row_dn = S.height-2;
			else
				row_dn = row+1;

			if ( green_mode == WF_GREENMODE_GX_XG )
				start_col = row & 1;
			else
				start_col = ( row+1 ) & 1;

			if ( start_col == 0 )
				start_col_left = 1;
			else
				start_col_left = 0;

			switch (src_imgmode)
			{
				case WF_IMGMODE_BAYER1PLANE:

					src_c  = &WF_BAYERSRC1(row,    start_col);
					src_u1 = &WF_BAYERSRC1(row_up, start_col_left);
					src_d1 = &WF_BAYERSRC1(row_dn, start_col_left);
					src_u2 = &WF_BAYERSRC1(row_up, start_col+1);
					src_d2 = &WF_BAYERSRC1(row_dn, start_col+1);

					break;

				case WF_IMGMODE_BAYER4PLANE:

					src_c  = &WF_BAYERSRC4(row,    start_col,      FC(row,   start_col));
					src_u1 = &WF_BAYERSRC4(row_up, start_col_left, FC(row_up, start_col_left));
					src_d1 = &WF_BAYERSRC4(row_dn, start_col_left, FC(row_dn, start_col_left));
					src_u2 = &WF_BAYERSRC4(row_up, start_col+1,    FC(row_up, start_col+1));
					src_d2 = &WF_BAYERSRC4(row_dn, start_col+1,    FC(row_dn, start_col+1));

					break;
			}

			long int sum_l1, sum_l2;

			sum_l1 = *src_u1 + *src_d1;
			sum_l2 = *src_u2 + *src_d2;			
			
			if (start_col == 0)
			{
				// Edges

				line_filtered[start_col] = sum_l1 + sum_l2 + (*src_c)*4;

				src_u2 += src_h_shift;
				src_d2 += src_h_shift;				

				sum_l2 = *src_u2 + *src_d2;

				src_c += src_h_shift;
				start_col=2;
			}

			int width_m_3 = S.width-3;

			// Main

			for (col=start_col; col<width_m_3; col+=2)
			{
				line_filtered[col] = sum_l1 + sum_l2 + 4*(*src_c);

				src_u1 += src_h_shift_x2;
				src_d1 += src_h_shift_x2;

				sum_l1 = *src_u1 + *src_d1;

				src_c += src_h_shift;
				col+=2;

				line_filtered[col] = sum_l1 + sum_l2 + 4*(*src_c);

				src_u2 += src_h_shift_x2;
				src_d2 += src_h_shift_x2;

				sum_l2 = *src_u2 + *src_d2; 

				src_c += src_h_shift;
			}
			
			// Right edge

			if      (col == S.width-1)
			{
				line_filtered[col] = 2*sum_l1 + 4*(*src_c);
			}
			else if (col == S.width-2)
			{
				line_filtered[col] = sum_l1 + sum_l2 + 4*(*src_c);
			}
			else if (col == S.width-3)
			{
				line_filtered[col] = sum_l1 + sum_l2 + 4*(*src_c);
				
				src_c += src_h_shift;
				col+=2;

				line_filtered[col] = 2*sum_l2 + 4*(*src_c);
			}

			if (row>0)
			{

				if ( green_mode == WF_GREENMODE_GX_XG )
					start_col = ( row+1 ) & 1;
				else
					start_col = row & 1;
				

				switch (dst_imgmode)
				{
					case WF_IMGMODE_BAYER1PLANE:
						dst_c  = &WF_BAYERDST1(row-1,    start_col);
						break;

					case WF_IMGMODE_BAYER4PLANE:
						dst_c  = &WF_BAYERDST4(row-1,    start_col, FC(row-1, start_col));
						break;
				}

				for (col=start_col;  col<S.width; col+=2)
				{
					*dst_c=(line_filtered[col])>>3;
					dst_c+=dst_h_shift;
				}

				if (src_image != dst_image)
				{
					// copy red or blue channel

					if ( green_mode == WF_GREENMODE_GX_XG )
						start_col = row & 1;
					else
						start_col = (row+1) & 1;
					
					switch (src_imgmode)
					{
						case WF_IMGMODE_BAYER1PLANE:
							src     = &WF_BAYERSRC1(row-1, start_col);
							break;

						case WF_IMGMODE_BAYER4PLANE:
							src     = &WF_BAYERSRC4(row-1, start_col, FC(row-1, start_col));
							break;
					}

					switch (dst_imgmode)
					{
						case WF_IMGMODE_BAYER1PLANE:
							dst_rb  = &WF_BAYERDST1(row-1, start_col);
							break;

						case WF_IMGMODE_BAYER4PLANE:
							dst_rb  = &WF_BAYERDST4(row-1, start_col, FC(row-1, start_col));
							break;
					}

					for (col=start_col;  col<S.width; col+=2)
					{
						*dst_rb=*src;
						src   +=src_h_shift;
						dst_rb+=dst_h_shift;
					}
				}
			}
		}

		if ( green_mode == WF_GREENMODE_GX_XG )
			start_col = ( row+1 ) & 1;
		else
			start_col = row & 1;
				

		switch (dst_imgmode)
		{
			case WF_IMGMODE_BAYER1PLANE:
				dst_c  = &WF_BAYERDST1(row-1,    start_col);
				break;

			case WF_IMGMODE_BAYER4PLANE:
				dst_c  = &WF_BAYERDST4(row-1,    start_col, FC(row-1, start_col));
				break;
		}

		for (col=start_col;  col<S.width; col+=2)
		{
			*dst_c=(line_filtered[col])>>3;
			dst_c+=dst_h_shift;
		}

		if (src_image != dst_image)
		{
			// copy red or blue channel
			
			if ( green_mode == WF_GREENMODE_GX_XG )
				start_col = row & 1;
			else
				start_col = (row+1) & 1;
				
			switch (src_imgmode)
			{
				case WF_IMGMODE_BAYER1PLANE:
					src     = &WF_BAYERSRC1(row-1, start_col);
					break;

				case WF_IMGMODE_BAYER4PLANE:
					src     = &WF_BAYERSRC4(row-1, start_col, FC(row-1, start_col));
					break;
			}

			switch (dst_imgmode)
			{
				case WF_IMGMODE_BAYER1PLANE:
					dst_rb  = &WF_BAYERDST1(row-1, start_col);
					break;
				
				case WF_IMGMODE_BAYER4PLANE:
					dst_rb  = &WF_BAYERDST4(row-1, start_col, FC(row-1, start_col));
					break;
			}
			
			for (col=start_col;  col<S.width; col+=2)
			{
				*dst_rb=*src;
				src   +=src_h_shift;
				dst_rb+=dst_h_shift;
			}
		}
	}

	free(line_filtered);
}

void LibRaw::wf_bayer4_igauss_filter(int radius, void* src_image, int src_imgmode, void* dst_image, int dst_imgmode)
{
	/* 
	   This function filter source bayer4 data with gauss (binominal), 4 channels independently.
	*/
	   
#define WF_BAYERSRC4(row, col, c) ((ushort(*)[4])src_image)[((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)][c] 
#define WF_BAYERSRC1(row, col)    ((ushort*)src_image)     [((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)] 
#define WF_BAYERDST4(row, col, c) ((ushort(*)[4])dst_image)[((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)][c] 
#define WF_BAYERDST1(row, col)    ((ushort*)dst_image)     [((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)]

	if (radius <= 0 || radius > 8)
	   return;

	long int (*line_filtered)[4];

	long int gauss_conv_kernel[9][4];

	long int gauss_conv_kernel_c[8][9] = 
	{
		{32768, 16384},
		{24576, 16384, 4096},
		{20480,	15360, 6144, 1024},
		{17920, 14336, 7168, 2048, 256},
		{16128, 13440, 7680, 2880, 640,  64},
		{14784, 12672, 7920, 3520, 1056, 192, 16},
		{13728, 12012, 8008, 4004, 1456, 364, 56,  4},
		{12870, 11440, 8008, 4368, 1820, 560, 120, 16, 1},
	};

	int line_memory_len = (MAX(S.height, S.width)+1)/2+radius*2+1;
    line_filtered       = (long int(*)[4]) calloc(line_memory_len, sizeof(long int[4]));

	int src_h_shift, src_v_shift;
	int dst_h_shift, dst_v_shift;

	if      (src_imgmode == WF_IMGMODE_BAYER1PLANE)
		src_h_shift = 2 >> IO.shrink;
	else if (src_imgmode == WF_IMGMODE_BAYER4PLANE)
		src_h_shift = 8 >> IO.shrink;

	src_v_shift = S.width*src_h_shift;

	if      (dst_imgmode == WF_IMGMODE_BAYER1PLANE)
		dst_h_shift = 2 >> IO.shrink;
	else if (dst_imgmode == WF_IMGMODE_BAYER4PLANE)
		dst_h_shift = 8 >> IO.shrink;

	dst_v_shift = S.width*dst_h_shift;

	int width_d2  = S.width  / 2;
	int height_d2 = S.height / 2;

	int i, j;

	for (j=0; j<=radius; j++)
	{
		for (i=0; i<4; i++)
		{
			gauss_conv_kernel[j][i] = gauss_conv_kernel_c[radius-1][j];
		}
	}

	int row,  col;
	int rowf, colf;

	ushort   *src  [4], *dst[4];
	long int  src_c[4];

	// Horizontal 

	int right_edge[4];

	for (i=0; i<4; i++)
	{
		int padding = i<2 && (S.width & 1 == 1) ? 1 : 0;
		right_edge[i]=width_d2 + radius + padding;
	}
	
	for(row=0; row<S.height; row+=2)
	{
		int row_p1=MIN(row+1, S.height-1);

		switch (src_imgmode)
		{
			case WF_IMGMODE_BAYER1PLANE:

				src[0] = &WF_BAYERSRC1(row,    0);
				src[1] = &WF_BAYERSRC1(row_p1, 0);
				src[2] = &WF_BAYERSRC1(row,    1);
				src[3] = &WF_BAYERSRC1(row_p1, 1);
				break;

			case WF_IMGMODE_BAYER4PLANE:

				src[0] = &WF_BAYERSRC4(row,    0, FC(0,      0));
				src[1] = &WF_BAYERSRC4(row_p1, 0, FC(row_p1, 0));
				src[2] = &WF_BAYERSRC4(row,    1, FC(0,      1));
				src[3] = &WF_BAYERSRC4(row_p1, 1, FC(row_p1, 1));
				break;
		}

		colf = radius;

		for (int j=0; j<line_memory_len; j++)
		{
			for(i=0;i<4;i++)
				line_filtered[j][i]=0;
		}
		
		for(col=0; col<S.width-1; col+=2)
		{

			int col1, col2;

			col1=col2=colf;

			for (i=0; i<4; i++)
				src_c[i]=*src[i];

			for (i=0; i<4; i++)
				line_filtered[colf][i]+=gauss_conv_kernel[0][i]*(src_c[i]);

			for(int j=1; j<=radius; j++)
			{
				col1++;
				col2--;
			
				for (i=0; i<4; i++)
				{
					long int g;

					g = gauss_conv_kernel[j][i]*src_c[i];

					line_filtered[col1][i]+=g;
					line_filtered[col2][i]+=g;
				}
			}

			colf++;

			for (i=0; i<4; i++)
				src[i]+=src_h_shift;
		}

		// width is odd number

		if (col == S.width-1)
		{
			int col1, col2;

			col1=col2=colf;

			for (i=0; i<2; i++)
				src_c[i]=*src[i];

			for (i=0; i<2; i++)
				line_filtered[colf][i]+=gauss_conv_kernel[0][i]*(src_c[i]);

			for(int j=1; j<=radius; j++)
			{
				col1++;
				col2--;
			
				for (i=0; i<2; i++)
				{
					long int g;

					g = gauss_conv_kernel[j][i]*src_c[i];

					line_filtered[col1][i]+=g;
					line_filtered[col2][i]+=g;
				}
			}

			colf++;

			for (i=0; i<2; i++)
				src[i]+=src_h_shift;
		}

		// Edges mirroring

		for(j=0; j<radius; j++)
		{
			for (i=0; i<4; i++)
			{
				line_filtered[radius+j         ][i]+=line_filtered[radius-j-1     ][i];
				line_filtered[right_edge[i]-1-j][i]+=line_filtered[right_edge[i]+j][i];
			}
		}
		
		switch (dst_imgmode)
		{
			case WF_IMGMODE_BAYER1PLANE:
			
				dst[0] = &WF_BAYERDST1(row,    0);
				dst[1] = &WF_BAYERDST1(row_p1, 0);
				dst[2] = &WF_BAYERDST1(row,    1);
				dst[3] = &WF_BAYERDST1(row_p1, 1);
				break;

			case WF_IMGMODE_BAYER4PLANE:
		
				dst[0] = &WF_BAYERDST4(row,    0, FC(0,      0));
				dst[1] = &WF_BAYERDST4(row_p1, 0, FC(row_p1, 0));
				dst[2] = &WF_BAYERDST4(row,    1, FC(0,      1));
				dst[3] = &WF_BAYERDST4(row_p1, 1, FC(row_p1, 1));
				break;
		}

		colf = radius;

		for(col=0; col<S.width-1; col+=2)
		{
			for(i=0; i<4; i++)
			{
				*dst[i]=line_filtered[colf][i]>>16;
				dst[i]+=dst_h_shift;
			}

			colf++;
		}

		if (col == S.width-1)
		{
			for(i=0; i<2; i++)
				*dst[i]=line_filtered[colf][i]>>16;
		}
	}


   	// Vertical

	int lower_edge[4];

	for (i=0; i<4; i++)
	{
		int padding = i<2 && (S.height & 1 == 1) ? 1 : 0;
		lower_edge[i]=height_d2 + radius + padding;
	}


	for(col=0; col<S.width; col+=2)
	{
		int col_p1=MIN(col+1, S.width-1);	

		switch (dst_imgmode)
		{
			case WF_IMGMODE_BAYER1PLANE:

				src[0] = &WF_BAYERDST1(0, col);
				src[1] = &WF_BAYERDST1(0, col_p1);
				src[2] = &WF_BAYERDST1(1, col);
				src[3] = &WF_BAYERDST1(1, col_p1);
				break;

			case WF_IMGMODE_BAYER4PLANE:

				src[0] = &WF_BAYERDST4(0, col,    FC(0,      0));
				src[1] = &WF_BAYERDST4(0, col_p1, FC(0, col_p1));
				src[2] = &WF_BAYERDST4(1, col,    FC(1,      0));
				src[3] = &WF_BAYERDST4(1, col_p1, FC(1, col_p1));
				break;
		}

		rowf = radius;

		for (int j=0; j<line_memory_len; j++)
		{
			for(i=0;i<4;i++)
				line_filtered[j][i]=0;
		}
		
		for(row=0; row<S.height-1; row+=2)
		{

			int row1, row2;

			row1=row2=rowf;

			for (i=0; i<4; i++)
				src_c[i]=*src[i];

			for (i=0; i<4; i++)
				line_filtered[rowf][i]+=gauss_conv_kernel[0][i]*(src_c[i]);

			for(int j=1; j<=radius; j++)
			{
				row1++;
				row2--;
			
				long int g[4];

				for (i=0; i<4; i++)
				{

					g[i] = gauss_conv_kernel[j][i]*src_c[i];

					line_filtered[row1][i]+=g[i];
					line_filtered[row2][i]+=g[i];
				}
			}

			rowf++;

			for (i=0; i<4; i++)
				src[i]+=dst_v_shift;
		}

		// height is odd number

		if (row == S.height-1)
		{
			int row1, row2;

			row1=row2=rowf;

			for (i=0; i<2; i++)
				src_c[i]=*src[i];

			for (i=0; i<2; i++)
				line_filtered[rowf][i]+=gauss_conv_kernel[0][i]*(src_c[i]);

			for(int j=1; j<=radius; j++)
			{
				row1++;
				row2--;
			
				long int g[4];

				for (i=0; i<2; i++)
				{

					g[i] = gauss_conv_kernel[j][i]*src_c[i];

					line_filtered[row1][i]+=g[i];
					line_filtered[row2][i]+=g[i];
				}
			}

			rowf++;

			for (i=0; i<2; i++)
				src[i]+=dst_v_shift;
		}

		// Edge mirroring

		for(int j=0; j<radius; j++)
		{
			for (int i=0; i<4; i++)
			{
				line_filtered[radius+j][i]         +=line_filtered[radius-j-1][i];
				line_filtered[lower_edge[i]-1-j][i]+=line_filtered[lower_edge[i]+j][i];
			}
		}


		switch (dst_imgmode)
		{
			case WF_IMGMODE_BAYER1PLANE:
			
				dst[0] = &WF_BAYERDST1(0, col);
				dst[1] = &WF_BAYERDST1(0, col_p1);
				dst[2] = &WF_BAYERDST1(1, col);
				dst[3] = &WF_BAYERDST1(1, col_p1);
				break;

			case WF_IMGMODE_BAYER4PLANE:
		
				dst[0] = &WF_BAYERDST4(0, col,    FC(0, 0));
				dst[1] = &WF_BAYERDST4(0, col_p1, FC(0, col_p1));
				dst[2] = &WF_BAYERDST4(1, col,    FC(1, 0));
				dst[3] = &WF_BAYERDST4(1, col_p1, FC(1, col_p1));
				break;
		}

		rowf = radius;

		for(row=0; row<S.height-1; row+=2)
		{
			for(i=0; i<4; i++)
			{
				*dst[i]=line_filtered[rowf][i]>>16;
				dst[i]+=dst_v_shift;
			}

			rowf++;
		}

		if (row == S.height-1)
		{
			for(i=0; i<2; i++)
				*dst[i]=line_filtered[rowf][i]>>16;
		}
   }

   free(line_filtered);
}

void LibRaw::wf_bayer4_block_filter(int* radius_list, void* src_image, int src_imgmode, void* dst_image, int dst_imgmode)
{
#define WF_BLOCKFILTER_MAXF 8

#define WF_BAYERSRC4(row,col,c) ((ushort(*)[4])src_image)[((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)][c] 
#define WF_BAYERSRC1(row,col)   ((ushort*)src_image)     [((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)] 
#define WF_BAYERDST4(row,col,c) ((ushort(*)[4])dst_image)[((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)][c] 
#define WF_BAYERDST1(row,col)   ((ushort*)dst_image)     [((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)] 

	int filter_itrtns_num = 0;
	
	int block_radius      [WF_BLOCKFILTER_MAXF]; 
	int block_radius_x2   [WF_BLOCKFILTER_MAXF];
	int block_radius_x2_p1[WF_BLOCKFILTER_MAXF];

	int block_radius_max = 0;
	int block_radius_max_x2;
	int block_radius_max_x2_p1;

	for(int i=0; (i<WF_BLOCKFILTER_MAXF) && (radius_list[i]!=0); i++)
	{
		block_radius      [i]=radius_list[i];
		block_radius_x2   [i]=block_radius[i]*2;
		block_radius_x2_p1[i]=block_radius_x2[i]+1;

		if(block_radius_max<block_radius[i])
			block_radius_max=block_radius[i];

		filter_itrtns_num++;
	}

	long int divider[WF_BLOCKFILTER_MAXF];
	long int div_multiplication;

	div_multiplication=block_radius_x2_p1[0];

	for(int i=1; i<filter_itrtns_num; i++)
	{
		if (div_multiplication*((long int)block_radius_x2_p1[i])<65535)
		{
			div_multiplication*=block_radius_x2_p1[i];
			divider[i-1]=1;
		}
		else
		{
			divider[i-1]=block_radius_x2_p1[i];
		}
	}

	divider[filter_itrtns_num-1]=div_multiplication;

	block_radius_max_x2    = block_radius_max*2;
	block_radius_max_x2_p1 = block_radius_max_x2+1;
	
	int line_memory_len;

	long int (*source_line)[4];
	long int (*line_block_filtered)[4];
 
	line_memory_len = (MAX(S.height, S.width)+1)/2+block_radius_max_x2_p1*2;

	line_block_filtered=(long int(*)[4]) calloc(line_memory_len, sizeof(long int[4]));
	source_line        =(long int(*)[4]) calloc(line_memory_len, sizeof(long int[4]));
	

   	int   src_h_shift, dst_h_shift, src_v_shift, dst_v_shift;
 
	if      (src_imgmode == WF_IMGMODE_BAYER1PLANE)
		src_h_shift = 2 >> IO.shrink;
	else if (src_imgmode == WF_IMGMODE_BAYER4PLANE)
		src_h_shift = 8 >> IO.shrink;

	src_v_shift = S.width*src_h_shift;

	if      (dst_imgmode == WF_IMGMODE_BAYER1PLANE)
		dst_h_shift = 2 >> IO.shrink;
	else if (dst_imgmode == WF_IMGMODE_BAYER4PLANE)
		dst_h_shift = 8 >> IO.shrink;

	dst_v_shift = S.width*dst_h_shift;

	int width_d2     = S.width  / 2;
	int height_d2    = S.height / 2;

	int width_p1_d2  = (S.width+1)  / 2;
	int height_p1_d2 = (S.height+1) / 2;

	ushort   *src[4], *dst[4];

	long int  (*src_plus)[4], (*src_minus)[4];
	long int  block_sum[4];
	
	int row, col;

	int right_edge[4], lower_edge[4];

	for(row=0; row<S.height; row+=2)
	{
		int row_p1=MIN(row+1, S.height-1);

		switch (src_imgmode)
		{
			case WF_IMGMODE_BAYER1PLANE:

				src[0] = &WF_BAYERSRC1(row,    0);
				src[1] = &WF_BAYERSRC1(row_p1, 0);
				src[2] = &WF_BAYERSRC1(row,    1);
				src[3] = &WF_BAYERSRC1(row_p1, 1);
				break;

			case WF_IMGMODE_BAYER4PLANE:

				src[0] = &WF_BAYERSRC4(row,    0, FC(0,      0));
				src[1] = &WF_BAYERSRC4(row_p1, 0, FC(row_p1, 0));
				src[2] = &WF_BAYERSRC4(row,    1, FC(0,      1));
				src[3] = &WF_BAYERSRC4(row_p1, 1, FC(row_p1, 1));
				break;
		}

		for(col=0; col<width_d2; col++)
		{
			for (int i=0; i<4; i++)
			{
				source_line[col][i]=*src[i];
				src[i] += src_h_shift;
			}
		}

		if (S.width & 1 == 1)
		{
			for (int i=0; i<2; i++)
			{		   
				source_line[width_d2][i]=*src[i];
			}

			for (int i=2; i<4; i++)
			{		   
				source_line[width_d2][i]=0;
			}
		}

		for(int f=0; f<filter_itrtns_num; f++)
		{
			src_minus=src_plus=source_line;

			for (int i=0; i<4; i++)
				block_sum[i]=0;

			for(col=0; col<block_radius_x2_p1[f]; col++)
			{
				for (int i=0; i<4; i++)
				{
					block_sum[i]+=(*src_plus)[i];
					line_block_filtered[col][i]=block_sum[i];
				}

				src_plus++;
			}

			for(col=block_radius_x2_p1[f]; col<width_p1_d2; col++)
			{
				for (int i=0; i<4; i++)
				{		   
					block_sum[i]+=(*src_plus)[i];
					block_sum[i]-=(*src_minus)[i];
					line_block_filtered[col][i]=block_sum[i];
				}

				src_plus++;
				src_minus++;
			}

			for(col=width_p1_d2; col<width_p1_d2+block_radius_x2_p1[f]; col++)
			{
				for (int i=0; i<4; i++)
				{
					block_sum[i]-=(*src_minus)[i];
					line_block_filtered[col][i]=block_sum[i];
				}

				src_minus++;
			}

			// Edge mirroring

			for (int i=0; i<4; i++)
			{
				int padding = i<2 && (S.width & 1 == 1) ? 1 : 0;
				right_edge[i]=width_d2 + block_radius[f] + padding;
			}

			for(int j=0; j<block_radius[f]; j++)
			{
				for (int i=0; i<4; i++)
				{
					line_block_filtered[block_radius[f]+j][i]+=line_block_filtered[block_radius[f]-j-1][i];
					line_block_filtered[right_edge[i]-1-j][i]+=line_block_filtered[right_edge[i]+j][i];
				}
			}

			if (divider[f]==1)
			{
				for(col=0; col<width_d2; col++)
				{
					for (int i=0; i<4; i++)
						source_line[col][i]=line_block_filtered[col+block_radius[f]][i];
				}

				if (S.width & 1 == 1)
				{
					for (int i=0; i<2; i++)
						source_line[width_d2][i]=line_block_filtered[width_d2+block_radius[f]][i];

					for (int i=2; i<4; i++)
						source_line[width_d2][i]=0;
				}
			}
			else
			{
				for(col=0; col<width_d2; col++)
				{
					for (int i=0; i<4; i++)
						source_line[col][i]=line_block_filtered[col+block_radius[f]][i]/divider[f];
				}

				if (S.width & 1 == 1)
				{
					for (int i=0; i<2; i++)
						source_line[width_d2][i]=line_block_filtered[width_d2+block_radius[f]][i]/divider[f];

					for (int i=2; i<4; i++)
						source_line[width_d2][i]=0;
				}
			}
		}

		switch (dst_imgmode)
		{
			case WF_IMGMODE_BAYER1PLANE:

				dst[0] = &WF_BAYERDST1(row,    0);
				dst[1] = &WF_BAYERDST1(row_p1, 0);
				dst[2] = &WF_BAYERDST1(row,    1);
				dst[3] = &WF_BAYERDST1(row_p1, 1);
				break;

			case WF_IMGMODE_BAYER4PLANE:

				dst[0] = &WF_BAYERDST4(row,    0, FC(0,      0));
				dst[1] = &WF_BAYERDST4(row_p1, 0, FC(row_p1, 0));
				dst[2] = &WF_BAYERDST4(row,    1, FC(0,      1));
				dst[3] = &WF_BAYERDST4(row_p1, 1, FC(row_p1, 1));
				break;
		}

		for(col=0; col<width_d2; col++)
		{
			for (int i=0; i<4; i++)
			{		   
				*dst[i]=source_line[col][i];
				dst[i]+=dst_h_shift;
			}
		}

		if (S.width & 1 == 1)
		{
			for (int i=0; i<2; i++)
				*dst[i]=source_line[col][i];
		}
	}

	for(col=0; col<S.width; col+=2)
	{
		int col_p1=MIN(col+1, S.width-1);

		switch (dst_imgmode)
		{
			case WF_IMGMODE_BAYER1PLANE:

				src[0] = &WF_BAYERDST1(0, col);
				src[1] = &WF_BAYERDST1(0, col_p1);
				src[2] = &WF_BAYERDST1(1, col);
				src[3] = &WF_BAYERDST1(1, col_p1);
				break;

			case WF_IMGMODE_BAYER4PLANE:

				src[0] = &WF_BAYERDST4(0, col,    FC(0,      0));
				src[1] = &WF_BAYERDST4(0, col_p1, FC(0, col_p1));
				src[2] = &WF_BAYERDST4(1, col,    FC(1,      0));
				src[3] = &WF_BAYERDST4(1, col_p1, FC(1, col_p1));
				break;
		}

		for(row=0; row<height_d2; row++)
		{
			for (int i=0; i<4; i++)
			{
				source_line[row][i]=*src[i];
				src[i] += dst_v_shift;
			}
		}
		
		if (S.height & 1 == 1)
		{
			for (int i=0; i<2; i++)
			{		   
				source_line[height_d2][i]=*src[i];
			}

			for (int i=2; i<4; i++)
			{		   
				source_line[height_d2][i]=0;
			}
		}

		for(int f=0; f<filter_itrtns_num; f++)
		{
			src_minus=src_plus=source_line;

			for (int i=0; i<4; i++)
				block_sum[i]=0;

			for(row=0; row<block_radius_x2_p1[f]; row++)
			{
				for (int i=0; i<4; i++)
				{
					block_sum[i]+=(*src_plus)[i];
					line_block_filtered[row][i]=block_sum[i];
				}

				src_plus++;
			}

			for(row=block_radius_x2_p1[f]; row<height_p1_d2; row++)
			{
				for (int i=0; i<4; i++)
				{		   
					block_sum[i]+=(*src_plus)[i];
					block_sum[i]-=(*src_minus)[i];
					line_block_filtered[row][i]=block_sum[i];
				}

				src_plus++;
				src_minus++;
			}

			for(row=height_p1_d2; row<height_p1_d2+block_radius_x2_p1[f]; row++)
			{
				for (int i=0; i<4; i++)
				{
					block_sum[i]-=(*src_minus)[i];
					line_block_filtered[row][i]=block_sum[i];
				}

				src_minus++;
			}

			// Edge mirroring

			for (int i=0; i<4; i++)
			{
				int padding = (i<2) && (S.height & 1 == 1) ? 1 : 0;
				lower_edge[i]=height_d2 + block_radius[f] + padding;
			}

			for(int j=0; j<block_radius[f]; j++)
			{
				for (int i=0; i<4; i++)
				{
					line_block_filtered[block_radius[f]+j][i]+=line_block_filtered[block_radius[f]-j-1][i];
					line_block_filtered[lower_edge[i]-1-j][i]+=line_block_filtered[lower_edge[i]+j][i];
				}
			}

			if (divider[f]==1)
			{
				for(row=0; row<height_d2; row++)
				{
					for (int i=0; i<4; i++)
						source_line[row][i]=line_block_filtered[row+block_radius[f]][i];
				}

				if (S.height & 1 == 1)
				{
					for (int i=0; i<2; i++)
						source_line[height_d2][i]=line_block_filtered[height_d2+block_radius[f]][i];

					for (int i=2; i<4; i++)
						source_line[height_d2][i]=0;
				}
			}
			else
			{
				for(row=0; row<height_d2; row++)
				{
					for (int i=0; i<4; i++)
						source_line[row][i]=line_block_filtered[row+block_radius[f]][i]/divider[f];
				}

				if (S.height & 1 == 1)
				{
					for (int i=0; i<2; i++)
						source_line[height_d2][i]=line_block_filtered[height_d2+block_radius[f]][i]/divider[f];

					for (int i=2; i<4; i++)
						source_line[height_d2][i]=0;
				}
			}
		}

		switch (dst_imgmode)
		{
			case WF_IMGMODE_BAYER1PLANE:

				dst[0] = &WF_BAYERDST1(0, col);
				dst[1] = &WF_BAYERDST1(0, col_p1);
				dst[2] = &WF_BAYERDST1(1, col);
				dst[3] = &WF_BAYERDST1(1, col_p1);
				break;

			case WF_IMGMODE_BAYER4PLANE:

				dst[0] = &WF_BAYERDST4(0, col,    FC(0,      0));
				dst[1] = &WF_BAYERDST4(0, col_p1, FC(0, col_p1));
				dst[2] = &WF_BAYERDST4(1, col,    FC(1,      0));
				dst[3] = &WF_BAYERDST4(1, col_p1, FC(1, col_p1));
				break;
		}

		for(row=0; row<height_d2; row++)
		{
			for (int i=0; i<4; i++)
			{		   
				*dst[i]=source_line[row][i];
				dst[i]+=dst_v_shift;
			}
		}

		if (S.height & 1 == 1)
		{
			for (int i=0; i<2; i++)
				*dst[i]=source_line[height_d2][i];
		}
	}

	free(line_block_filtered);
	free(source_line);
}

void LibRaw::subtract_black()
{

#define BAYERC(row,col,c) imgdata.image[((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)][c] 

    if(C.ph1_black)
        {
            // Phase One compressed format
            int row,col,val,cc;
            for(row=0;row<S.height;row++)
                for(col=0;col<S.width;col++)
                    {
                        cc=FC(row,col);
                        val = BAYERC(row,col,cc) 
                            - C.phase_one_data.t_black 
                            + C.ph1_black[row+S.top_margin][(col + S.left_margin) 
                                                                                >=C.phase_one_data.split_col];
                        if(val<0) val = 0;
                        BAYERC(row,col,cc) = val;
                    }
            C.maximum -= C.black;
            phase_one_correct();
            // recalculate channel maximum
            ZERO(C.channel_maximum);
            for(row=0;row<S.height;row++)
                for(col=0;col<S.width;col++)
                    {
                        cc=FC(row,col);
                        val = BAYERC(row,col,cc);
                        if(C.channel_maximum[cc] > val) C.channel_maximum[cc] = val;
                    }
            // clear P1 black level data
            imgdata.color.phase_one_data.t_black = 0;
            C.ph1_black = 0;
            ZERO(C.cblack);
            C.black = 0;
        }
    else if((C.black || C.cblack[0] || C.cblack[1] || C.cblack[2] || C.cblack[3]))
        {
            int cblk[4],i,row,col,val,cc;
            for(i=0;i<4;i++)
                cblk[i] = C.cblack[i]+C.black;
            ZERO(C.channel_maximum);

            for(row=0;row<S.height;row++)
                for(col=0;col<S.width;col++)
                    {
                        cc=COLOR(row,col);
                        val = BAYERC(row,col,cc);
                        if(val > cblk[cc])
                            val -= cblk[cc];
                        else
                            val = 0;
                        if(C.channel_maximum[cc] < val) C.channel_maximum[cc] = val;
                        BAYERC(row,col,cc) = val;
                    }
            C.maximum -= C.black;
            ZERO(C.cblack);
            C.black = 0;
        }
    else
        {
            // only calculate channel maximum;
            int row,col,cc,val;
            ZERO(C.channel_maximum);
            for(row=0;row<S.height;row++)
                for(col=0;col<S.width;col++)
                    for(cc = 0; cc< 4; cc++)
                        {
                            int val = BAYERC(row,col,cc);
                            if(C.channel_maximum[cc] < val) C.channel_maximum[cc] = val;
                        }
            
        }
}

#define TBLN 65535

void LibRaw::exp_bef(float shift, float smooth)
{
    // params limits
    if(shift>8) shift = 8;
    if(shift<0.25) shift = 0.25;
    if(smooth < 0.0) smooth = 0.0;
    if(smooth > 1.0) smooth = 1.0;
    
    unsigned short *lut = (ushort*)malloc((TBLN+1)*sizeof(unsigned short));

    if(shift <=1.0)
        {
            for(int i=0;i<=TBLN;i++)
                lut[i] = (unsigned short)((float)i*shift);
        }
    else
        {
            float x1,x2,y1,y2;

            float cstops = log(shift)/log(2.0f);
            float room = cstops*2;
            float roomlin = powf(2.0f,room);
            x2 = (float)TBLN;
            x1 = (x2+1)/roomlin-1;
            y1 = x1*shift;
            y2 = x2*(1+(1-smooth)*(shift-1));
            float sq3x=powf(x1*x1*x2,1.0f/3.0f);
            float B = (y2-y1+shift*(3*x1-3.0f*sq3x)) / (x2+2.0f*x1-3.0f*sq3x);
            float A = (shift - B)*3.0f*powf(x1*x1,1.0f/3.0f);
            float CC = y2 - A*powf(x2,1.0f/3.0f)-B*x2;
            for(int i=0;i<=TBLN;i++)
                {
                    float X = (float)i;
                    float Y = A*powf(X,1.0f/3.0f)+B*X+CC;
                    if(i<x1)
                        lut[i] = (unsigned short)((float)i*shift);
                    else
                        lut[i] = Y<0?0:(Y>TBLN?TBLN:(unsigned short)(Y));
                }
        }
    for(int i=0; i< S.height*S.width; i++)
        {
            imgdata.image[i][0] = lut[imgdata.image[i][0]];
            imgdata.image[i][1] = lut[imgdata.image[i][1]];
            imgdata.image[i][2] = lut[imgdata.image[i][2]];
            imgdata.image[i][3] = lut[imgdata.image[i][3]];
        }
    for(int i=0;i<4;i++)
        C.channel_maximum[i] = lut[C.channel_maximum[i]];
    C.maximum = lut[C.maximum];
    // no need to adjust the minumum, black is already subtracted
    free(lut);
}
int LibRaw::dcraw_process(void)
{
    int quality,i;

    int iterations=-1, dcb_enhance=1, noiserd=0;
    int eeci_refine_fl=0, es_med_passes_fl=0;
    float cared=0,cablue=0;
    float linenoise=0; 
    float lclean=0,cclean=0;
    float thresh=0;
    float preser=0;
    float expos=1.0;


    CHECK_ORDER_LOW(LIBRAW_PROGRESS_LOAD_RAW);
//    CHECK_ORDER_HIGH(LIBRAW_PROGRESS_PRE_INTERPOLATE);

    try {

        int no_crop = 1;

        if (~O.cropbox[2] && ~O.cropbox[3])
            no_crop=0;

        raw2image_ex(); // raw2image+crop+rotate_fuji_raw + subtract_black for fuji

        int save_4color = O.four_color_rgb;

        if (IO.zero_is_bad) 
            {
                remove_zeroes();
                SET_PROC_FLAG(LIBRAW_PROGRESS_REMOVE_ZEROES);
            }

        if(!!IO.fuji_width)
            subtract_black();

        if(O.half_size) 
            O.four_color_rgb = 1;

        if(O.bad_pixels && no_crop) 
            {
                bad_pixels(O.bad_pixels);
                SET_PROC_FLAG(LIBRAW_PROGRESS_BAD_PIXELS);
            }

        if (O.dark_frame && no_crop)
            {
                subtract (O.dark_frame);
                SET_PROC_FLAG(LIBRAW_PROGRESS_DARK_FRAME);
            }

	if (O.wf_debanding)
	{
		wf_remove_banding();
	}

        quality = 2 + !IO.fuji_width;

        if (O.user_qual >= 0) quality = O.user_qual;

        adjust_maximum();

        if (O.user_sat > 0) C.maximum = O.user_sat;

        if (P1.is_foveon && !O.document_mode) 
            {
                foveon_interpolate();
                SET_PROC_FLAG(LIBRAW_PROGRESS_FOVEON_INTERPOLATE);
            }

        if (O.green_matching && !O.half_size)
            {
                green_matching();
            }

        if (!P1.is_foveon &&  O.document_mode < 2)
            {
                scale_colors();
                SET_PROC_FLAG(LIBRAW_PROGRESS_SCALE_COLORS);
            }

        pre_interpolate();

        SET_PROC_FLAG(LIBRAW_PROGRESS_PRE_INTERPOLATE);

        if (O.dcb_iterations >= 0) iterations = O.dcb_iterations;
        if (O.dcb_enhance_fl >=0 ) dcb_enhance = O.dcb_enhance_fl;
        if (O.fbdd_noiserd >=0 ) noiserd = O.fbdd_noiserd;
        if (O.eeci_refine >=0 ) eeci_refine_fl = O.eeci_refine;
        if (O.es_med_passes >0 ) es_med_passes_fl = O.es_med_passes;

// LIBRAW_DEMOSAIC_PACK_GPL3

        if (!O.half_size && O.cfa_green >0) {thresh=O.green_thresh ;green_equilibrate(thresh);} 
        if (O.exp_correc >0) {expos=O.exp_shift ; preser=O.exp_preser; exp_bef(expos,preser);} 
        if (O.ca_correc >0 ) {cablue=O.cablue; cared=O.cared; CA_correct_RT(cablue, cared);}
        if (O.cfaline >0 ) {linenoise=O.linenoise; cfa_linedn(linenoise);}
        if (O.cfa_clean >0 ) {lclean=O.lclean; cclean=O.cclean; cfa_impulse_gauss(lclean,cclean);}

        if (P1.filters && !O.document_mode) 
            {
                if (noiserd>0 && P1.colors==3 && P1.filters) fbdd(noiserd);

                if (quality == 0)
                    lin_interpolate();
                else if (quality == 1 || P1.colors > 3)
                    vng_interpolate();
                else if (quality == 2)
                    ppg_interpolate();

                else if (quality == 3) 
                    ahd_interpolate(); // really don't need it here due to fallback op

                else if (quality == 4)
                    dcb(iterations, dcb_enhance);

//  LIBRAW_DEMOSAIC_PACK_GPL2                
                else if (quality == 5)
                    ahd_interpolate_mod();
                else if (quality == 6)
                    afd_interpolate_pl(2,1);
                else if (quality == 7)
                    vcd_interpolate(0);
                else if (quality == 8)
                    vcd_interpolate(12);
                else if (quality == 9)
                    lmmse_interpolate(1);

// LIBRAW_DEMOSAIC_PACK_GPL3
                else if (quality == 10)
                    amaze_demosaic_RT();
 // fallback to AHD
                else
                    ahd_interpolate();
                
                SET_PROC_FLAG(LIBRAW_PROGRESS_INTERPOLATE);
            }
        if (IO.mix_green)
            {
                for (P1.colors=3, i=0; i < S.height * S.width; i++)
                    imgdata.image[i][1] = (imgdata.image[i][1] + imgdata.image[i][3]) >> 1;
                SET_PROC_FLAG(LIBRAW_PROGRESS_MIX_GREEN);
            }

        if(!P1.is_foveon)
            {
                if (P1.colors == 3) 
                    {
                        
                        if (quality == 8) 
                            {
                                if (eeci_refine_fl == 1) refinement();
                                if (O.med_passes > 0)    median_filter_new();
                                if (es_med_passes_fl > 0) es_median_filter();
                            } 
                        else {
                            median_filter();
                        }
                        SET_PROC_FLAG(LIBRAW_PROGRESS_MEDIAN_FILTER);
                    }
            }
        
        if (O.highlight == 2) 
            {
                blend_highlights();
                SET_PROC_FLAG(LIBRAW_PROGRESS_HIGHLIGHTS);
            }
        
        if (O.highlight > 2) 
            {
                recover_highlights();
                SET_PROC_FLAG(LIBRAW_PROGRESS_HIGHLIGHTS);
            }
        
        if (O.use_fuji_rotate) 
            {
                fuji_rotate();
                SET_PROC_FLAG(LIBRAW_PROGRESS_FUJI_ROTATE);
            }
    
        if(!libraw_internal_data.output_data.histogram)
            {
                libraw_internal_data.output_data.histogram = (int (*)[LIBRAW_HISTOGRAM_SIZE]) malloc(sizeof(*libraw_internal_data.output_data.histogram)*4);
                merror(libraw_internal_data.output_data.histogram,"LibRaw::dcraw_process()");
            }
#ifndef NO_LCMS
	if(O.camera_profile)
            {
                apply_profile(O.camera_profile,O.output_profile);
                SET_PROC_FLAG(LIBRAW_PROGRESS_APPLY_PROFILE);
            }
#endif

        convert_to_rgb();
        SET_PROC_FLAG(LIBRAW_PROGRESS_CONVERT_RGB);

        if (O.use_fuji_rotate) 
            {
                stretch();
                SET_PROC_FLAG(LIBRAW_PROGRESS_STRETCH);
            }
        O.four_color_rgb = save_4color; // also, restore

        return 0;
    }
    catch ( LibRaw_exceptions err) {
        EXCEPTION_HANDLER(err);
    }
}

// Supported cameras:
static const char  *static_camera_list[] = 
{
"Adobe Digital Negative (DNG)",
"AgfaPhoto DC-833m",
"Apple QuickTake 100",
"Apple QuickTake 150",
"Apple QuickTake 200",
"ARRIRAW format",
"AVT F-080C",
"AVT F-145C",
"AVT F-201C",
"AVT F-510C",
"AVT F-810C",
"Canon PowerShot 600",
"Canon PowerShot A5",
"Canon PowerShot A5 Zoom",
"Canon PowerShot A50",
"Canon PowerShot A460 (CHDK hack)",
"Canon PowerShot A470 (CHDK hack)",
"Canon PowerShot A530 (CHDK hack)",
"Canon PowerShot A570 (CHDK hack)",
"Canon PowerShot A590 (CHDK hack)",
"Canon PowerShot A610 (CHDK hack)",
"Canon PowerShot A620 (CHDK hack)",
"Canon PowerShot A630 (CHDK hack)",
"Canon PowerShot A640 (CHDK hack)",
"Canon PowerShot A650 (CHDK hack)",
"Canon PowerShot A710 IS (CHDK hack)",
"Canon PowerShot A720 IS (CHDK hack)",
"Canon PowerShot Pro70",
"Canon PowerShot Pro90 IS",
"Canon PowerShot Pro1",
"Canon PowerShot G1",
"Canon PowerShot G2",
"Canon PowerShot G3",
"Canon PowerShot G5",
"Canon PowerShot G6",
"Canon PowerShot G7 (CHDK hack)",
"Canon PowerShot G9",
"Canon PowerShot G10",
"Canon PowerShot G11",
"Canon PowerShot G12",
"Canon PowerShot S2 IS (CHDK hack)",
"Canon PowerShot S3 IS (CHDK hack)",
"Canon PowerShot S5 IS (CHDK hack)",
"Canon PowerShot SD300 (CHDK hack)",
"Canon PowerShot S30",
"Canon PowerShot S40",
"Canon PowerShot S45",
"Canon PowerShot S50",
"Canon PowerShot S60",
"Canon PowerShot S70",
"Canon PowerShot S90",
"Canon PowerShot S95",
"Canon PowerShot SX1 IS",
"Canon PowerShot SX110 IS (CHDK hack)",
"Canon PowerShot SX120 IS (CHDK hack)",
"Canon PowerShot SX20 IS (CHDK hack)",
"Canon PowerShot SX30 IS (CHDK hack)",
"Canon EOS D30",
"Canon EOS D60",
"Canon EOS 5D",
"Canon EOS 5D Mark II",
"Canon EOS 7D",
"Canon EOS 10D",
"Canon EOS 20D",
"Canon EOS 30D",
"Canon EOS 40D",
"Canon EOS 50D",
"Canon EOS 60D",
"Canon EOS 300D / Digital Rebel / Kiss Digital",
"Canon EOS 350D / Digital Rebel XT / Kiss Digital N",
"Canon EOS 400D / Digital Rebel XTi / Kiss Digital X",
"Canon EOS 450D / Digital Rebel XSi / Kiss Digital X2",
"Canon EOS 500D / Digital Rebel T1i / Kiss Digital X3",
"Canon EOS 550D / Digital Rebel T2i / Kiss Digital X4",
"Canon EOS 600D / Digital Rebel T3i / Kiss Digital X5",
"Canon EOS 1000D / Digital Rebel XS / Kiss Digital F",
"Canon EOS 1100D / Digital Rebel T3 / Kiss Digital X50",
"Canon EOS D2000C",
"Canon EOS-1D",
"Canon EOS-1DS",
"Canon EOS-1D Mark II",
"Canon EOS-1D Mark II N",
"Canon EOS-1D Mark III",
"Canon EOS-1D Mark IV",
"Canon EOS-1Ds Mark II",
"Canon EOS-1Ds Mark III",
"Casio QV-2000UX",
"Casio QV-3000EX",
"Casio QV-3500EX",
"Casio QV-4000",
"Casio QV-5700",
"Casio QV-R41",
"Casio QV-R51",
"Casio QV-R61",
"Casio EX-S20",
"Casio EX-S100",
"Casio EX-Z4",
"Casio EX-Z50",
"Casio EX-Z55",
"Casio EX-Z60",
"Casio EX-Z75",
"Casio EX-Z750",
"Casio EX-Z850",
"Casio EX-Z1050",
"Casio EX-Z1080",
"Casio Exlim Pro 505",
"Casio Exlim Pro 600",
"Casio Exlim Pro 700",
"Contax N Digital",
"Creative PC-CAM 600",
"Epson R-D1",
"Foculus 531C",
"Fuji FinePix E550",
"Fuji FinePix E900",
"Fuji FinePix F700",
"Fuji FinePix F710",
"Fuji FinePix F800",
"Fuji FinePix F810",
"Fuji FinePix S2Pro",
"Fuji FinePix S3Pro",
"Fuji FinePix S5Pro",
"Fuji FinePix S20Pro",
"Fuji FinePix S100FS",
"Fuji FinePix S5000",
"Fuji FinePix S5100/S5500",
"Fuji FinePix S5200/S5600",
"Fuji FinePix S6000fd",
"Fuji FinePix S7000",
"Fuji FinePix S9000/S9500",
"Fuji FinePix S9100/S9600",
"Fuji FinePix S200EXR",
"Fuji FinePix HS10/HS11",
"Fuji FinePix HS20EXR",
"Fuji FinePix F550EXR",
"Fuji FinePix X100",
"Fuji IS-1",
"Hasselblad CFV",
"Hasselblad H3D",
"Hasselblad H4D",
"Hasselblad V96C",
"Imacon Ixpress 16-megapixel",
"Imacon Ixpress 22-megapixel",
"Imacon Ixpress 39-megapixel",
"ISG 2020x1520",
"Kodak DC20",
"Kodak DC25",
"Kodak DC40",
"Kodak DC50",
"Kodak DC120 (also try kdc2tiff)",
"Kodak DCS200",
"Kodak DCS315C",
"Kodak DCS330C",
"Kodak DCS420",
"Kodak DCS460",
"Kodak DCS460A",
"Kodak DCS520C",
"Kodak DCS560C",
"Kodak DCS620C",
"Kodak DCS620X",
"Kodak DCS660C",
"Kodak DCS660M",
"Kodak DCS720X",
"Kodak DCS760C",
"Kodak DCS760M",
"Kodak EOSDCS1",
"Kodak EOSDCS3B",
"Kodak NC2000F",
"Kodak ProBack",
"Kodak PB645C",
"Kodak PB645H",
"Kodak PB645M",
"Kodak DCS Pro 14n",
"Kodak DCS Pro 14nx",
"Kodak DCS Pro SLR/c",
"Kodak DCS Pro SLR/n",
"Kodak C330",
"Kodak C603",
"Kodak P850",
"Kodak P880",
"Kodak Z980",
"Kodak Z981",
"Kodak Z990",
"Kodak Z1015",
"Kodak KAI-0340",
"Konica KD-400Z",
"Konica KD-510Z",
"Leaf AFi 7",
"Leaf AFi-II 5",
"Leaf AFi-II 6",
"Leaf AFi-II 7",
"Leaf AFi-II 8",
"Leaf AFi-II 10",
"Leaf AFi-II 10R",
"Leaf AFi-II 12",
"Leaf AFi-II 12R",
"Leaf Aptus 17",
"Leaf Aptus 22",
"Leaf Aptus 54S",
"Leaf Aptus 65",
"Leaf Aptus 75",
"Leaf Aptus 75S",
"Leaf Cantare",
"Leaf CatchLight",
"Leaf CMost",
"Leaf DCB2",
"Leaf Valeo 6",
"Leaf Valeo 11",
"Leaf Valeo 17",
"Leaf Valeo 22",
"Leaf Volare",
"Leica Digilux 2",
"Leica Digilux 3",
"Leica D-LUX2",
"Leica D-LUX3",
"Leica D-LUX4",
"Leica D-LUX5",
"Leica V-LUX1",
"Leica V-LUX2",
"Logitech Fotoman Pixtura",
"Mamiya ZD",
"Micron 2010",
"Minolta RD175",
"Minolta DiMAGE 5",
"Minolta DiMAGE 7",
"Minolta DiMAGE 7i",
"Minolta DiMAGE 7Hi",
"Minolta DiMAGE A1",
"Minolta DiMAGE A2",
"Minolta DiMAGE A200",
"Minolta DiMAGE G400",
"Minolta DiMAGE G500",
"Minolta DiMAGE G530",
"Minolta DiMAGE G600",
"Minolta DiMAGE Z2",
"Minolta Alpha/Dynax/Maxxum 5D",
"Minolta Alpha/Dynax/Maxxum 7D",
"Motorola PIXL",
"Nikon D1",
"Nikon D1H",
"Nikon D1X",
"Nikon D2H",
"Nikon D2Hs",
"Nikon D2X",
"Nikon D2Xs",
"Nikon D3",
"Nikon D3s",
"Nikon D3X",
"Nikon D40",
"Nikon D40X",
"Nikon D50",
"Nikon D60",
"Nikon D70",
"Nikon D70s",
"Nikon D80",
"Nikon D90",
"Nikon D100",
"Nikon D200",
"Nikon D300",
"Nikon D300s",
"Nikon D700",
"Nikon D3000",
"Nikon D3100",
"Nikon D5000",
"Nikon D5100",
"Nikon D7000",
"Nikon E700 (\"DIAG RAW\" hack)",
"Nikon E800 (\"DIAG RAW\" hack)",
"Nikon E880 (\"DIAG RAW\" hack)",
"Nikon E900 (\"DIAG RAW\" hack)",
"Nikon E950 (\"DIAG RAW\" hack)",
"Nikon E990 (\"DIAG RAW\" hack)",
"Nikon E995 (\"DIAG RAW\" hack)",
"Nikon E2100 (\"DIAG RAW\" hack)",
"Nikon E2500 (\"DIAG RAW\" hack)",
"Nikon E3200 (\"DIAG RAW\" hack)",
"Nikon E3700 (\"DIAG RAW\" hack)",
"Nikon E4300 (\"DIAG RAW\" hack)",
"Nikon E4500 (\"DIAG RAW\" hack)",
"Nikon E5000",
"Nikon E5400",
"Nikon E5700",
"Nikon E8400",
"Nikon E8700",
"Nikon E8800",
"Nikon Coolpix P6000",
"Nikon Coolpix P7000",
"Nikon Coolpix S6 (\"DIAG RAW\" hack)",
"Nokia N95",
"Nokia X2",
"Olympus C3030Z",
"Olympus C5050Z",
"Olympus C5060WZ",
"Olympus C7070WZ",
"Olympus C70Z,C7000Z",
"Olympus C740UZ",
"Olympus C770UZ",
"Olympus C8080WZ",
"Olympus X200,D560Z,C350Z",
"Olympus E-1",
"Olympus E-3",
"Olympus E-5",
"Olympus E-10",
"Olympus E-20",
"Olympus E-30",
"Olympus E-300",
"Olympus E-330",
"Olympus E-400",
"Olympus E-410",
"Olympus E-420",
"Olympus E-500",
"Olympus E-510",
"Olympus E-520",
"Olympus E-620",
"Olympus E-P1",
"Olympus E-P2",
"Olympus E-P3",
"Olympus E-PL1",
"Olympus E-PL1s",
"Olympus E-PL2",
"Olympus SP310",
"Olympus SP320",
"Olympus SP350",
"Olympus SP500UZ",
"Olympus SP510UZ",
"Olympus SP550UZ",
"Olympus SP560UZ",
"Olympus SP570UZ",
"Olympus XZ-1",
"Panasonic DMC-FZ8",
"Panasonic DMC-FZ18",
"Panasonic DMC-FZ28",
"Panasonic DMC-FZ30",
"Panasonic DMC-FZ35/FZ38",
"Panasonic DMC-FZ40",
"Panasonic DMC-FZ50",
"Panasonic DMC-FZ100",
"Panasonic DMC-FX150",
"Panasonic DMC-G1",
"Panasonic DMC-G10",
"Panasonic DMC-G2",
"Panasonic DMC-G3",
"Panasonic DMC-GF1",
"Panasonic DMC-GF2",
"Panasonic DMC-GF3",
"Panasonic DMC-GH1",
"Panasonic DMC-GH2",
"Panasonic DMC-L1",
"Panasonic DMC-L10",
"Panasonic DMC-LC1",
"Panasonic DMC-LX1",
"Panasonic DMC-LX2",
"Panasonic DMC-LX3",
"Panasonic DMC-LX5",
"Pentax *ist D",
"Pentax *ist DL",
"Pentax *ist DL2",
"Pentax *ist DS",
"Pentax *ist DS2",
"Pentax K10D",
"Pentax K20D",
"Pentax K100D",
"Pentax K100D Super",
"Pentax K200D",
"Pentax K2000/K-m",
"Pentax K-x",
"Pentax K-r",
"Pentax K-5",
"Pentax K-7",
"Pentax Optio S",
"Pentax Optio S4",
"Pentax Optio 33WR",
"Pentax Optio 750Z",
"Pentax 645D",
"Phase One LightPhase",
"Phase One H 10",
"Phase One H 20",
"Phase One H 25",
"Phase One P 20",
"Phase One P 25",
"Phase One P 30",
"Phase One P 45",
"Phase One P 45+",
"Phase One P 65",
"Pixelink A782",
#ifdef LIBRAW_DEMOSAIC_PACK_GPL2
"Polaroid x530",
#endif
#ifndef NO_JASPER
"Redcode R3D format",
#endif
"Rollei d530flex",
"RoverShot 3320af",
"Samsung EX1",
"Samsung GX-1S",
"Samsung GX10",
"Samsung GX20",
"Samsung NX10",
"Samsung NX11",
"Samsung NX100",
"Samsung WB550",
"Samsung WB2000",
"Samsung S85 (hacked)",
"Samsung S850 (hacked)",
"Sarnoff 4096x5440",
#ifdef LIBRAW_DEMOSAIC_PACK_GPL2
"Sigma SD9",
"Sigma SD10",
"Sigma SD14",
#endif
"Sinar 3072x2048",
"Sinar 4080x4080",
"Sinar 4080x5440",
"Sinar STI format",
"SMaL Ultra-Pocket 3",
"SMaL Ultra-Pocket 4",
"SMaL Ultra-Pocket 5",
"Sony DSC-F828",
"Sony DSC-R1",
"Sony DSC-V3",
"Sony DSLR-A100",
"Sony DSLR-A200",
"Sony DSLR-A230",
"Sony DSLR-A290",
"Sony DSLR-A300",
"Sony DSLR-A330",
"Sony DSLR-A350",
"Sony DSLR-A380",
"Sony DSLR-A390",
"Sony DSLR-A450",
"Sony DSLR-A500",
"Sony DSLR-A550",
"Sony DSLR-A580",
"Sony DSLR-A700",
"Sony DSLR-A850",
"Sony DSLR-A900",
"Sony NEX-3",
"Sony NEX-5",
"Sony NEX-C3",
"Sony SLT-A33",
"Sony SLT-A35",
"Sony SLT-A55V",
"Sony XCD-SX910CR",
"STV680 VGA",
   NULL
};

const char** LibRaw::cameraList() { return static_camera_list;}
int LibRaw::cameraCount() { return (sizeof(static_camera_list)/sizeof(static_camera_list[0]))-1; }


const char * LibRaw::strprogress(enum LibRaw_progress p)
{
    switch(p)
        {
        case LIBRAW_PROGRESS_START:
            return "Starting";
        case LIBRAW_PROGRESS_OPEN :
            return "Opening file";
        case LIBRAW_PROGRESS_IDENTIFY :
            return "Reading metadata";
        case LIBRAW_PROGRESS_SIZE_ADJUST:
            return "Adjusting size";
        case LIBRAW_PROGRESS_LOAD_RAW:
            return "Reading RAW data";
        case LIBRAW_PROGRESS_REMOVE_ZEROES:
            return "Clearing zero values";
        case LIBRAW_PROGRESS_BAD_PIXELS :
            return "Removing dead pixels";
        case LIBRAW_PROGRESS_DARK_FRAME:
            return "Subtracting dark frame data";
        case LIBRAW_PROGRESS_FOVEON_INTERPOLATE:
            return "Interpolating Foveon sensor data";
        case LIBRAW_PROGRESS_SCALE_COLORS:
            return "Scaling colors";
        case LIBRAW_PROGRESS_PRE_INTERPOLATE:
            return "Pre-interpolating";
        case LIBRAW_PROGRESS_INTERPOLATE:
            return "Interpolating";
        case LIBRAW_PROGRESS_MIX_GREEN :
            return "Mixing green channels";
        case LIBRAW_PROGRESS_MEDIAN_FILTER   :
            return "Median filter";
        case LIBRAW_PROGRESS_HIGHLIGHTS:
            return "Highlight recovery";
        case LIBRAW_PROGRESS_FUJI_ROTATE :
            return "Rotating Fuji diagonal data";
        case LIBRAW_PROGRESS_FLIP :
            return "Flipping image";
        case LIBRAW_PROGRESS_APPLY_PROFILE:
            return "ICC conversion";
        case LIBRAW_PROGRESS_CONVERT_RGB:
            return "Converting to RGB";
        case LIBRAW_PROGRESS_STRETCH:
            return "Stretching image";
        case LIBRAW_PROGRESS_THUMB_LOAD:
            return "Loading thumbnail";
        default:
            return "Some strange things";
        }
}
