// __BEGIN_LICENSE__
// Copyright (C) 2006, 2007 United States Government as represented by
// the Administrator of the National Aeronautics and Space Administration.
// All Rights Reserved.
// __END_LICENSE__


/// \file DiskImageResourceGDAL.cc
///
/// Provides support for several geospatially referenced file formats
/// via GDAL.
///

#ifdef _MSC_VER
#pragma warning(disable:4244)
#pragma warning(disable:4267)
#pragma warning(disable:4996)
#endif

#include <vw/config.h>
#include <iostream>
#ifdef VW_HAVE_PKG_GDAL

#include <vw/FileIO/DiskImageResourceGDAL.h>
#include <vw/FileIO/JP2.h>

// GDAL Headers
#include "gdal.h"
#include "gdal_priv.h"
#include "cpl_string.h"
#include "cpl_multiproc.h"
#include "ogr_spatialref.h"
#include "ogr_api.h"

#include <vector>
#include <list>
#include <vw/Core/Exception.h>
#include <vw/Core/Cache.h>
#include <vw/Core/Thread.h>
#include <vw/Image/PixelTypes.h>
#include <boost/algorithm/string.hpp>

// GDAL is not thread-safe, so we keep a global GDAL lock (pointed to
// by gdal_mutex_ptr, below) that we hold anytime we call into the
// GDAL library itself.  Note that the mutex, along with the GDAL
// dataset cache, is created by the init_gdal function, so you need to
// make sure that gdal_init_once.run(init_gdal) has been called prior
// to anything else.

// This cache of GDAL file handles allows up to 200 files to be open
// at a time.
namespace {
  vw::RunOnce gdal_init_once = VW_RUNONCE_INIT;
  vw::Cache *gdal_cache_ptr = 0;
  vw::Mutex *gdal_mutex_ptr = 0;
  void init_gdal() {
    gdal_cache_ptr = new vw::Cache( 200 );
    gdal_mutex_ptr = new vw::Mutex();
    // Register all GDAL file readers and writers and open the data set.
    GDALAllRegister();
  }
}

void vw::UnloadGDAL() {
  if( gdal_cache_ptr || gdal_mutex_ptr ) {
    delete gdal_cache_ptr;
    delete gdal_mutex_ptr;
    GDALDumpOpenDatasets( stderr );
    GDALDestroyDriverManager();
    CPLDumpSharedList( NULL );
    CPLCleanupTLS();
  }
}

vw::Cache& vw::DiskImageResourceGDAL::gdal_cache() {
  gdal_init_once.run( init_gdal );
  return *gdal_cache_ptr;
}

namespace {

  bool has_gmljp2(vw::JP2File* f)
  {
    vw::JP2Box* b;
    vw::JP2Box* b2;
    vw::JP2SuperBox::JP2BoxIterator pos;
    bool retval = false;

    while((b = f->find_box(0x61736F63 /*"asoc"*/, &pos)))
    {
      // GMLJP2 outer Association box must contain a Label box (with label "gml.data")
      // as its first sub-box
      b2 = ((vw::JP2SuperBox*)b)->find_box(0);
      if(b2 && b2->box_type() == 0x6C626C20 /*"lbl\040"*/ && strncmp((char*)(((vw::JP2DataBox*)b2)->data()), "gml.data", ((vw::JP2DataBox*)b2)->bytes_dbox()) == 0)
      {
        retval = true;
        break;
      }
    }

    return retval;
  }

  class GdalCloseDatasetDeleter {
  public:
    void operator()(GDALDataset *dataset) {
      GDALClose(dataset);
    }
  };
}

namespace vw {

  /// \cond INTERNAL
  // Type conversion class for selecting the GDAL data type for
  // various VW channel formats.
  struct vw_channel_id_to_gdal_pix_fmt {
    static GDALDataType value(ChannelTypeEnum vw_type) {
      switch (vw_type) {
      case VW_CHANNEL_UINT8:   return GDT_Byte;
      case VW_CHANNEL_INT16:   return GDT_Int16;
      case VW_CHANNEL_UINT16:  return GDT_UInt16;
      case VW_CHANNEL_INT32:   return GDT_Int32;
      case VW_CHANNEL_UINT32:  return GDT_UInt32;
      case VW_CHANNEL_FLOAT32: return GDT_Float32;
      case VW_CHANNEL_FLOAT64: return GDT_Float64;
      default:
        vw_throw( IOErr() << "DiskImageResourceGDAL: Unsupported channel type (" << vw_type << ")." );
        return (GDALDataType)0; // never reached
      }
    }
  };


  // Type conversion class for selecting the GDAL data type for
  // various VW channel formats.
  struct gdal_pix_fmt_to_vw_channel_id {
    static ChannelTypeEnum value(GDALDataType gdal_type) {
      switch (gdal_type) {
      case GDT_Byte:    return VW_CHANNEL_UINT8;
      case GDT_Int16:   return VW_CHANNEL_INT16;
      case GDT_UInt16:  return VW_CHANNEL_UINT16;
      case GDT_Int32:   return VW_CHANNEL_INT32;
      case GDT_UInt32:  return VW_CHANNEL_UINT32;
      case GDT_Float32: return VW_CHANNEL_FLOAT32;
      case GDT_Float64: return VW_CHANNEL_FLOAT64;
      default:
        vw_throw( IOErr() << "DiskImageResourceGDAL: Unsupported channel type (" << gdal_type << ")." );
        return (ChannelTypeEnum)0; // never reached
      }
    }
  };

  static std::string file_extension( std::string const& filename ) {
    std::string::size_type dot = filename.find_last_of('.');
    std::string extension = filename.substr( dot );
    boost::to_lower( extension );
    return extension;
  }

  static bool is_jp2( std::string const& filename ) {
    std::string extension = file_extension( filename );
    return (extension == ".jp2" || extension == ".j2k" || extension == ".j2c");
  }

  // GDAL Support many file formats not specifically enabled here.
  // Consult http://www.remotesensing.org/gdal/formats_list.html for a
  // list of available formats.
  struct gdal_file_format_from_filename {
    static std::list<std::string> format(std::string const& filename) {
      std::list<std::string> retval;
      std::string ext = file_extension(filename);

      if (ext == ".tif" || ext == ".tiff")        // GeoTiff
        retval.push_back("GTiff");
      else if (ext == ".grd")                     // GMT compatible netcdf
        retval.push_back("GMT");
      else if (ext == ".dem")                     // ENVI Labelled raster
        retval.push_back("ENVI");
      else if (ext == ".bil")                     // ESRI .hdr labelled
        retval.push_back("EHdr");
      else if (ext == ".jpg" || ext == ".jpeg")   // JPEG JFIF
        retval.push_back("JPEG");
      else if (is_jp2(filename)) {                // JPEG 2000
        retval.push_back("JP2KAK");               // Kakadu 
        retval.push_back("JP2ECW");
        retval.push_back("JPEG2000");
      }
      else if (ext == ".png")                     // PNG
        retval.push_back("PNG");
      else if (ext == ".gif")                     // GIF
        retval.push_back("GIF");
      else if (ext == ".cub") {                   // ISIS Cube
        retval.push_back("ISIS3");
        retval.push_back("ISIS2");
      } else if (ext == ".img" || ext == ".pds" || ext == ".lbl") {  // Planetary data system image
        retval.push_back("PDS");
      } else if (ext == ".ddf") {                // USGS SDTS DEM
        retval.push_back("SDTS");
      } else if (ext == ".asc") {                // Arc/Info ASCII Grid
        retval.push_back("AAIGrid");
      } else if (ext == ".adf") {                // Arc/Info Binary Grid
        retval.push_back("AIG");
      } else if (ext == ".doq") {                // New Labelled USGS DOQ 
        retval.push_back("DOQ2");
      } else if (ext == ".dt0" || ext == ".dt1" || ext == ".dt2") { // Military Elevation Data
        retval.push_back("DTED");
      } else if (ext == ".fits") {                // FITS (needs GDAL w/ libcfitsio)
        retval.push_back("FITS");
      }
      else
        vw_throw( IOErr() << "DiskImageResourceGDAL: \"" << ext << "\" is an unsupported file extension." );
      return retval;
    }
  };

  // Retrieves a GDAL driver for a specified filename. need_create specifies
  // whether the driver needs rw support
  static std::pair<GDALDriver*, bool> gdal_get_driver(std::string const& filename, bool need_create = false)
  {
    // Make sure GDAL is initialized
    gdal_init_once.run( init_gdal );

    bool unsupported_driver = false;
    GDALDriver *driver = NULL;

    // Open the appropriate GDAL I/O driver, depending on the fileFormat
    // argument specified by the user.
    std::list<std::string> gdal_format_string = gdal_file_format_from_filename::format(filename);
    std::list<std::string>::iterator i;

    for( i = gdal_format_string.begin(); i != gdal_format_string.end() && driver == NULL; i++ )
    {
      vw_out(DebugMessage, "fileio") << "Trying to retrieve GDAL Driver with the following type: " << (*i) << std::endl;
      driver = GetGDALDriverManager()->GetDriverByName((*i).c_str());
      if( driver == NULL )
        continue;

      if (need_create)
      {
        char** metadata = driver->GetMetadata();
        if( !CSLFetchBoolean( metadata, GDAL_DCAP_CREATE, FALSE ) )
        {
          vw_out(WarningMessage, "fileio") << "GDAL driver " << (*i) << " does not support create." << std::endl;
          driver = NULL;
          unsupported_driver = true;
        }
      }
    }
    if (!driver)
      vw_out(DebugMessage, "fileio") << "Could not get GDAL driver for filename:" << filename << std::endl;
    return std::make_pair(driver, unsupported_driver);
  }

  bool vw::DiskImageResourceGDAL::gdal_has_support(std::string const& filename) {
    gdal_init_once.run(init_gdal);
    Mutex::Lock lock(*gdal_mutex_ptr);
    std::pair<GDALDriver *, bool> ret = gdal_get_driver(filename, false);
    return bool(ret.first);
  }

  static void convert_jp2(std::string const& filename) {
    uint8* d_original = 0;
    uint8* d_converted = 0;
    FILE* fp;
    uint64 nbytes_read, nbytes_converted, nbytes_written;
    uint64 i;
    int c;
    bool has_gml;
    int retval;

    if (!(fp = fopen(filename.c_str(), "r")))
      vw_throw( IOErr() << "convert_jp2: Failed to read " << filename << "." );

    for (nbytes_read = 0; fgetc(fp) != EOF; nbytes_read++) /* seek */;
    rewind(fp);

    d_original = new uint8[nbytes_read];

    for (i = 0; i < nbytes_read && (c = fgetc(fp)) != EOF; d_original[i] = (uint8)c, i++) /* seek */;

    if (!(i == nbytes_read  && fgetc(fp) == EOF))
      vw_throw( IOErr() << "convert_jp2: Size of " << filename << " has changed." );

    fclose(fp);

    JP2File f(d_original, nbytes_read);
    //f.print();
    has_gml = has_gmljp2(&f);
    retval = f.convert_to_jpx();
    if (retval != 0)
      vw_throw( IOErr() << " ; ; ; ; convert_jp2: Failed to convert " << filename << " to jp2-compatible jpx." );
    if (has_gml) {
      JP2ReaderRequirementsList req;
      // 67 is the (standard) requirement number for GMLJP2
      req.push_back(std::make_pair(67, false));
      retval = f.add_requirements(req);
      if (retval != 0)
        vw_throw( IOErr() << "convert_jp2: Failed to add GMLJP2 requirement to jp2-compatible jpx " << filename << "." );
    }
    //f.print();

    nbytes_converted = f.bytes();
    d_converted = new uint8[nbytes_converted];
    f.serialize(d_converted);

    if (!(fp = fopen(filename.c_str(), "w")))
      vw_throw( IOErr() << "convert_jp2: Failed to open " << filename << " for writing." );

    nbytes_written = fwrite(d_converted, 1, nbytes_converted, fp);

    fclose(fp);

    if (nbytes_written != nbytes_converted)
      vw_throw( IOErr() << "convert_jp2: Failed to write " << filename << "." );

    if (d_original)
      delete[] d_original;
    if (d_converted)
      delete[] d_converted;
  }
  /// \endcond

  boost::shared_ptr<GDALDataset> GdalDatasetGenerator::generate() const {
    GDALDataset *dataset = (GDALDataset*) GDALOpen( m_filename.c_str(), GA_ReadOnly );
    if ( !dataset )
      vw_throw(ArgumentErr() << "DiskImageResourceGDAL: Could not open \"" << m_filename << "\"");
    return boost::shared_ptr<GDALDataset>(dataset, GdalCloseDatasetDeleter());
  }

  DiskImageResourceGDAL::~DiskImageResourceGDAL() {
    flush();
    // Ensure that the read dataset gets destroyed while we're holding
    // the global lock.  (In the unlikely event that the user has
    // retained a reference to it, it's alredy their responsibility to
    // be holding the lock when they release it, too.)
    Mutex::Lock lock(*gdal_mutex_ptr);
    m_dataset_cache_handle.reset();
  }

  bool DiskImageResourceGDAL::has_nodata_value() const {
    Mutex::Lock lock(*gdal_mutex_ptr);
    boost::shared_ptr<GDALDataset> dataset = get_dataset_ptr();
    if( dataset == NULL ) {
      vw_throw( IOErr() << "DiskImageResourceGDAL: Failed to read no data value.  "
                << "Are you sure the file is open?" );
    }
    int success;
    dataset->GetRasterBand(1)->GetNoDataValue(&success);
    return success;
  }

  double DiskImageResourceGDAL::nodata_value() const {
    Mutex::Lock lock(*gdal_mutex_ptr);
    boost::shared_ptr<GDALDataset> dataset = get_dataset_ptr();
    if( dataset == NULL ) {
      vw_throw( IOErr() << "DiskImageResourceGDAL: Failed to read no data value.  "
                << "Are you sure the file is open?" );
    }
    int success;
    double val = dataset->GetRasterBand(1)->GetNoDataValue(&success);
    if (!success) {
      vw_throw( IOErr() << "DiskImageResourceGDAL: Error reading nodata value.  "
                << "This dataset does not have a nodata value.");
    }
    return val;
  }

  /// Bind the resource to a file for reading.  Confirm that we can
  /// open the file and that it has a sane pixel format.
  void DiskImageResourceGDAL::open( std::string const& filename )
  {
    // Make sure GDAL is initialized
    gdal_init_once.run( init_gdal );
    Mutex::Lock lock(*gdal_mutex_ptr);

    // Add a cache generator that can regenerate this dataset if it
    // needs to be closed due to too many open files.
    m_dataset_cache_handle = gdal_cache().insert(GdalDatasetGenerator(filename));

    boost::shared_ptr<GDALDataset> dataset = get_dataset_ptr();
    if( dataset == NULL ) {
      vw_throw( IOErr() << "DiskImageResourceGDAL: Failed to read " << filename << "." );
    }

    m_filename = filename;
    m_format.cols = dataset->GetRasterXSize();
    m_format.rows = dataset->GetRasterYSize();

    // <test code>
    vw_out(DebugMessage, "fileio") << "\n\tMetadata description: " << dataset->GetDescription() << std::endl;
    char** metadata = dataset->GetMetadata();
    vw_out(DebugMessage, "fileio") << "\tCount: " << CSLCount(metadata) << std::endl;
    for (int i = 0; i < CSLCount(metadata); i++) {
      vw_out(DebugMessage, "fileio") << "\t\t" << CSLGetField(metadata,i) << std::endl;
    }

    vw_out(DebugMessage, "fileio") << "\tDriver: " <<
      dataset->GetDriver()->GetDescription() <<
      dataset->GetDriver()->GetMetadataItem( GDAL_DMD_LONGNAME ) << std::endl;

    vw_out(DebugMessage, "fileio") << "\tSize is " <<
      dataset->GetRasterXSize() << "x" <<
      dataset->GetRasterYSize() << "x" <<
      dataset->GetRasterCount() << std::endl;

    // We do our best here to determine what pixel format the GDAL image is in.
    // Commented out the color interpretation checks because the reader (below)
    // can't really cope well with the the default multi-plane interpretation
    // and this is a quicker work-around than actually fixing the problem. -mdh
    for( int i=1; i<=dataset->GetRasterCount(); ++i )
    if ( dataset->GetRasterCount() == 1 /* &&
                dataset->GetRasterBand(1)->GetColorInterpretation() == GCI_GrayIndex */ ) {
      m_format.pixel_format = VW_PIXEL_GRAY;
      m_format.planes = 1;
    } else if ( dataset->GetRasterCount() == 2 /* &&
                dataset->GetRasterBand(1)->GetColorInterpretation() == GCI_GrayIndex &&
                dataset->GetRasterBand(2)->GetColorInterpretation() == GCI_AlphaBand */ ) {
      m_format.pixel_format = VW_PIXEL_GRAYA;
      m_format.planes = 1;
    } else if ( dataset->GetRasterCount() == 3 /* &&
                dataset->GetRasterBand(1)->GetColorInterpretation() == GCI_RedBand &&
                dataset->GetRasterBand(2)->GetColorInterpretation() == GCI_GreenBand &&
                dataset->GetRasterBand(3)->GetColorInterpretation() == GCI_BlueBand */) {
      m_format.pixel_format = VW_PIXEL_RGB;
      m_format.planes = 1;
    } else if ( dataset->GetRasterCount() == 4 /* &&
                dataset->GetRasterBand(1)->GetColorInterpretation() == GCI_RedBand &&
                dataset->GetRasterBand(2)->GetColorInterpretation() == GCI_GreenBand &&
                dataset->GetRasterBand(3)->GetColorInterpretation() == GCI_BlueBand &&
                dataset->GetRasterBand(4)->GetColorInterpretation() == GCI_AlphaBand */) {
      m_format.pixel_format = VW_PIXEL_RGBA;
     m_format.planes = 1;
    } else {
      m_format.pixel_format = VW_PIXEL_SCALAR;
      m_format.planes = dataset->GetRasterCount();
    }
    m_format.channel_type = gdal_pix_fmt_to_vw_channel_id::value(dataset->GetRasterBand(1)->GetRasterDataType());
    // Special limited read-only support for palette-based images
    if( dataset->GetRasterCount() == 1 &&
        dataset->GetRasterBand(1)->GetColorInterpretation() == GCI_PaletteIndex &&
        m_format.channel_type == VW_CHANNEL_UINT8 ) {
      m_format.pixel_format = VW_PIXEL_RGBA;
      m_format.planes = 1;
      GDALColorTable *color_table = dataset->GetRasterBand(1)->GetColorTable();
      int num_entries = color_table->GetColorEntryCount();
      m_palette.resize( num_entries );
      GDALColorEntry color;
      for( int i=0; i<num_entries; ++i ) {
        color_table->GetColorEntryAsRGB( i, &color );
        m_palette[i] = PixelRGBA<uint8>( color.c1, color.c2, color.c3, color.c4 );
      }
    }

    m_blocksize = default_block_size();
  }

  /// Bind the resource to a file for writing.
  void DiskImageResourceGDAL::create( std::string const& filename,
                                      ImageFormat const& format,
                                      Vector2i block_size,
                                      std::map<std::string,std::string> const& user_options )
  {
    // Make sure GDAL is initialized
    gdal_init_once.run( init_gdal );

    VW_ASSERT(format.planes == 1 || format.pixel_format==VW_PIXEL_SCALAR,
              NoImplErr() << "DiskImageResourceGDAL: Cannot create " << filename << "\n\t"
              << "The image cannot have both multiple channels and multiple planes.\n");
    VW_ASSERT((block_size[0] == -1 || block_size[1] == -1) || (block_size[0] % 16 == 0 && block_size[1] % 16 == 0),
              NoImplErr() << "DiskImageResourceGDAL: Cannot create " << filename << "\n\t"
              << "Block dimensions must be a multiple of 16.\n");

    // Store away relevent information into the internal data
    // structure for this DiskImageResource
    m_filename = filename;
    m_format = format;
    m_blocksize = block_size;
    m_options = user_options;

    Mutex::Lock lock(*gdal_mutex_ptr);
    initialize_write_resource();
  }

  void DiskImageResourceGDAL::initialize_write_resource() {
    if (m_write_dataset_ptr) {
      m_write_dataset_ptr.reset();
    }

    int num_bands = std::max( m_format.planes, num_channels( m_format.pixel_format ) );

    // returns Maybe driver, and whether it
    // found a ro driver when a rw one was requested
    std::pair<GDALDriver *, bool> ret = gdal_get_driver(m_filename, true);

    if( ret.first == NULL ) {
      if( ret.second )
        vw_throw( vw::NoImplErr() << "Could not write: " << m_filename << ".  Selected GDAL driver not supported." );
      else
        vw_throw( vw::IOErr() << "Error opening selected GDAL file I/O driver." );
    }

    GDALDriver *driver = ret.first;
    char **options = NULL;

    if( m_format.pixel_format == VW_PIXEL_GRAYA || m_format.pixel_format == VW_PIXEL_RGBA ) {
      options = CSLSetNameValue( options, "ALPHA", "YES" );
    }
    if( m_format.pixel_format != VW_PIXEL_SCALAR ) {
      options = CSLSetNameValue( options, "INTERLEAVE", "PIXEL" );
    }
    if( m_format.pixel_format == VW_PIXEL_RGB || m_format.pixel_format == VW_PIXEL_RGBA ||
        m_format.pixel_format == VW_PIXEL_GENERIC_3_CHANNEL || m_format.pixel_format == VW_PIXEL_GENERIC_4_CHANNEL) {
      options = CSLSetNameValue( options, "PHOTOMETRIC", "RGB" );
    }
    // If the user has specified a block size, we set the option for it here.
    if (m_blocksize[0] != -1 && m_blocksize[1] != -1) {
      std::ostringstream x_str, y_str;
      x_str << m_blocksize[0];
      y_str << m_blocksize[1];
      options = CSLSetNameValue( options, "TILED", "YES" );
      options = CSLSetNameValue( options, "BLOCKXSIZE", x_str.str().c_str() );
      options = CSLSetNameValue( options, "BLOCKYSIZE", y_str.str().c_str() );
    }

    for( std::map<std::string,std::string>::const_iterator i=m_options.begin(); i!=m_options.end(); ++i ) {
      options = CSLSetNameValue( options, i->first.c_str(), i->second.c_str() );
    }

    GDALDataType gdal_pix_fmt = vw_channel_id_to_gdal_pix_fmt::value(m_format.channel_type);
    GDALDataset *dataset = driver->Create( m_filename.c_str(), cols(), rows(), num_bands, gdal_pix_fmt, options );
    CSLDestroy( options );

    m_write_dataset_ptr.reset( dataset, GdalCloseDatasetDeleter() );

    if (m_blocksize[0] == -1 || m_blocksize[1] == -1) {
      m_blocksize = default_block_size();
    }
  }

  Vector2i DiskImageResourceGDAL::default_block_size() {
    boost::shared_ptr<GDALDataset> dataset = get_dataset_ptr();

    if (!dataset)
      vw_throw(LogicErr() << "DiskImageResourceGDAL: Could not get native block size.  No file is open.");

    // GDAL assumes a single-row stripsize even for file formats like PNG for 
    // which it does not support true strip access.  Thus, we check the file 
    // driver type before accepting GDAL's suggested block size.
    if ( dataset->GetDriver() == GetGDALDriverManager()->GetDriverByName("GTiff") ||
         dataset->GetDriver() == GetGDALDriverManager()->GetDriverByName("ISIS3") ||
         dataset->GetDriver() == GetGDALDriverManager()->GetDriverByName("JP2ECW") ||
	 dataset->GetDriver() == GetGDALDriverManager()->GetDriverByName("JP2KAK") ) {
      GDALRasterBand *band = dataset->GetRasterBand(1);
      int xsize, ysize;
      band->GetBlockSize(&xsize,&ysize);
      return Vector2i(xsize,ysize);
    }
    return Vector2i(cols(),rows());
  }

  /// Read the disk image into the given buffer.
  void DiskImageResourceGDAL::read( ImageBuffer const& dest, BBox2i const& bbox ) const
  {
    VW_ASSERT( channels() == 1 || planes()==1,
               LogicErr() << "DiskImageResourceGDAL: cannot read an image that has both multiple channels and multiple planes." );

    uint8 *data = new uint8[channel_size(channel_type()) * bbox.width() * bbox.height() * planes() * channels()];
    ImageBuffer src;
    src.data = data;
    src.format = m_format;
    src.format.cols = bbox.width();
    src.format.rows = bbox.height();
    src.cstride = channels() * channel_size(src.format.channel_type);
    src.rstride = src.cstride * src.format.cols;
    src.pstride = src.rstride * src.format.rows;

    {
      Mutex::Lock lock(*gdal_mutex_ptr);

      boost::shared_ptr<GDALDataset> dataset = get_dataset_ptr();
      if (!dataset)
        vw_throw( LogicErr() << "DiskImageResourceGDAL::read() Could not read file. No file has been opened." );

      if( m_palette.size() == 0 ) {
        for ( int32 p = 0; p < planes(); ++p ) {
          for ( int32 c = 0; c < channels(); ++c ) {
            // Only one of channels() or planes() will be nonzero.
            GDALRasterBand  *band = dataset->GetRasterBand(c+p+1);
            GDALDataType gdal_pix_fmt = vw_channel_id_to_gdal_pix_fmt::value(channel_type());
            band->RasterIO( GF_Read, bbox.min().x(), bbox.min().y(), bbox.width(), bbox.height(),
                            (uint8*)src(0,0,p) + channel_size(src.format.channel_type)*c,
                            src.format.cols, src.format.rows, gdal_pix_fmt, src.cstride, src.rstride );
          }
        }
      }
      else { // palette conversion
        GDALRasterBand  *band = dataset->GetRasterBand(1);
        uint8 *index_data = new uint8[bbox.width() * bbox.height()];
        band->RasterIO( GF_Read, bbox.min().x(), bbox.min().y(), bbox.width(), bbox.height(),
                        index_data, bbox.width(), bbox.height(), GDT_Byte, 1, bbox.width() );
        PixelRGBA<uint8> *rgba_data = (PixelRGBA<uint8>*) data;
        for( int i=0; i<bbox.width()*bbox.height(); ++i )
          rgba_data[i] = m_palette[index_data[i]];
        delete [] index_data;
      }
    }

    convert( dest, src, m_rescale );
    delete [] data;
  }


  // Write the given buffer into the disk image.
  void DiskImageResourceGDAL::write( ImageBuffer const& src, BBox2i const& bbox )
  {
    uint8 *data = new uint8[channel_size(channel_type()) * bbox.width() * bbox.height() * planes() * channels()];
    ImageBuffer dst;
    dst.data = data;
    dst.format = m_format;
    dst.format.cols = bbox.width();
    dst.format.rows = bbox.height();
    dst.cstride = channels() * channel_size(dst.format.channel_type);
    dst.rstride = dst.format.cols * dst.cstride;
    dst.pstride = dst.format.rows * dst.rstride;
    convert( dst, src, m_rescale );

    {
      Mutex::Lock lock(*gdal_mutex_ptr);

      GDALDataType gdal_pix_fmt = vw_channel_id_to_gdal_pix_fmt::value(channel_type());
      // We've already ensured that either planes==1 or channels==1.
      for (int32 p = 0; p < dst.format.planes; p++) {
        for (int32 c = 0; c < num_channels(dst.format.pixel_format); c++) {
          GDALRasterBand *band = get_dataset_ptr()->GetRasterBand(c+p+1);

          band->RasterIO( GF_Write, bbox.min().x(), bbox.min().y(), bbox.width(), bbox.height(),
                          (uint8*)dst(0,0,p) + channel_size(dst.format.channel_type)*c,
                          dst.format.cols, dst.format.rows, gdal_pix_fmt, dst.cstride, dst.rstride );
        }
      }
    }

    //FIXME: if we allow partial writes, m_convert_jp2 should probably only be set on complete writes
    if (is_jp2(m_filename))
      m_convert_jp2 = true;

    delete [] data;
  }

  // Set the block size
  //
  // Be careful here -- you can set any block size here, but you
  // choice may lead to extremely inefficient FileIO operations.
  void DiskImageResourceGDAL::set_block_size(Vector2i const& block_size) {
    m_blocksize = block_size;
    Mutex::Lock lock(*gdal_mutex_ptr);
    initialize_write_resource();
  }

  Vector2i DiskImageResourceGDAL::block_size() const {
    return m_blocksize;
  }

  void DiskImageResourceGDAL::flush() {
    if (m_write_dataset_ptr) {
      Mutex::Lock lock(*gdal_mutex_ptr);
      m_write_dataset_ptr.reset();
      if (m_convert_jp2)
        convert_jp2(m_filename);
    }
  }

  // Provide read access to the file's metadata
  char **DiskImageResourceGDAL::get_metadata() const {
    boost::shared_ptr<GDALDataset> dataset = get_dataset_ptr();
    if( dataset == NULL ) {
      vw_throw( IOErr() << "DiskImageResourceGDAL: Failed to read " << m_filename << "." );
    }
    return dataset->GetMetadata();
  }

  // Provides access to the underlying GDAL Dataset object.
  boost::shared_ptr<GDALDataset> DiskImageResourceGDAL::get_dataset_ptr() const {
    if (m_write_dataset_ptr) return m_write_dataset_ptr;
    else return m_dataset_cache_handle;
  }

  // Provides access to the global GDAL lock, for users who need to go
  // tinkering around in GDAL directly for some reason.
  Mutex &DiskImageResourceGDAL::global_lock() {
    gdal_init_once.run(init_gdal);
    return *gdal_mutex_ptr;
  }

  // A FileIO hook to open a file for reading
  vw::DiskImageResource* DiskImageResourceGDAL::construct_open( std::string const& filename ) {
    return new DiskImageResourceGDAL( filename );
  }

  // A FileIO hook to open a file for writing
  vw::DiskImageResource* DiskImageResourceGDAL::construct_create( std::string const& filename,
                                                                  ImageFormat const& format ) {
    return new DiskImageResourceGDAL( filename, format );
  }


} // namespace vw

#endif // HAVE_PKG_GDAL
