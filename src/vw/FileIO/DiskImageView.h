// __BEGIN_LICENSE__
// 
// Copyright (C) 2006 United States Government as represented by the
// Administrator of the National Aeronautics and Space Administration
// (NASA).  All Rights Reserved.
// 
// Copyright 2006 Carnegie Mellon University. All rights reserved.
// 
// This software is distributed under the NASA Open Source Agreement
// (NOSA), version 1.3.  The NOSA has been approved by the Open Source
// Initiative.  See the file COPYING at the top of the distribution
// directory tree for the complete NOSA document.
// 
// THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY OF ANY
// KIND, EITHER EXPRESSED, IMPLIED, OR STATUTORY, INCLUDING, BUT NOT
// LIMITED TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO
// SPECIFICATIONS, ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR
// A PARTICULAR PURPOSE, OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT
// THE SUBJECT SOFTWARE WILL BE ERROR FREE, OR ANY WARRANTY THAT
// DOCUMENTATION, IF PROVIDED, WILL CONFORM TO THE SUBJECT SOFTWARE.
// 
// __END_LICENSE__

/// \file DiskImageView.h
///
/// A read-only disk image view.  This is now just a thin 
/// wrapper around the more general ImageResourceView.
///
#ifndef __VW_FILEIO_DISKIMAGEVIEW_H__
#define __VW_FILEIO_DISKIMAGEVIEW_H__

#include <string>
#include <map>

#include <vw/Core/Cache.h>
#include <vw/Image/ImageResourceView.h>
#include <vw/Image/BlockRasterize.h>
#include <vw/FileIO/DiskImageResource.h>

namespace vw {

  /// A view of an image on disk.
  template <class PixelT>
  class DiskImageView : public ImageViewBase<DiskImageView<PixelT> >
  {
    typedef BlockRasterizeView<ImageResourceView<PixelT> > impl_type;
    // This is sort of redundant, but holding both the resource and 
    // the block rasterize view simplifies construction and access 
    // to the underlying resource.
    boost::shared_ptr<DiskImageResource> m_rsrc;
    impl_type m_impl;

  public:
    typedef typename impl_type::pixel_type pixel_type;
    typedef typename impl_type::result_type result_type;
    typedef typename impl_type::pixel_accessor pixel_accessor;

    /// Constructs a DiskImageView of the given file on disk.
    DiskImageView( std::string const& filename, bool cache=true )
      : m_rsrc( DiskImageResource::open( filename ) ), m_impl( boost::shared_ptr<ImageResource>(m_rsrc), m_rsrc->block_size(), 1, cache ) {}

    /// Constructs a DiskImageView of the given file on disk 
    /// using the specified cache area.
    DiskImageView( std::string const& filename, Cache& cache )
      : m_rsrc( DiskImageResource::open( filename ) ), m_impl( boost::shared_ptr<ImageResource>(m_rsrc), m_rsrc->block_size(), 1, cache ) {}

    /// Constructs a DiskImageView of the given resource.
    DiskImageView( boost::shared_ptr<DiskImageResource> resource, bool cache=true )
      : m_rsrc( resource ), m_impl( boost::shared_ptr<ImageResource>(m_rsrc), m_rsrc->block_size(), 1, cache ) {}

    /// Constructs a DiskImageView of the given resource using the
    /// specified cache area.
    DiskImageView( boost::shared_ptr<DiskImageResource> resource, Cache& cache )
      : m_rsrc( resource ), m_impl( boost::shared_ptr<ImageResource>(m_rsrc), m_rsrc->block_size(), 1, cache ) {}

    /// Constructs a DiskImageView of the given resource.  Takes
    /// ownership of the resource object (i.e. deletes it when it's
    /// done using it).
    DiskImageView( DiskImageResource *resource, bool cache=true )
      : m_rsrc( resource ), m_impl( boost::shared_ptr<ImageResource>(m_rsrc), m_rsrc->block_size(), 1, cache ) {}

    /// Constructs a DiskImageView of the given resource using the
    /// specified cache area.  Takes ownership of the resource object
    /// (i.e. deletes it when it's done using it).
    DiskImageView( DiskImageResource *resource, Cache& cache )
      : m_rsrc( resource ), m_impl( boost::shared_ptr<ImageResource>(m_rsrc), m_rsrc->block_size(), 1, cache ) {}

    ~DiskImageView() {}

    int32 cols() const { return m_impl.cols(); }
    int32 rows() const { return m_impl.rows(); }
    int32 planes() const { return m_impl.planes(); }

    pixel_accessor origin() const { return m_impl.origin(); }
    result_type operator()( int32 x, int32 y, int32 p = 0 ) const { return m_impl(x,y,p); }
    
    typedef typename impl_type::prerasterize_type prerasterize_type;
    prerasterize_type prerasterize( BBox2i const& bbox ) const { return m_impl.prerasterize( bbox ); }
    template <class DestT> void rasterize( DestT const& dest, BBox2i const& bbox ) const { m_impl.rasterize( dest, bbox ); }
    
    std::string filename() const { return m_rsrc->filename(); }

  };


  template <class PixelT>
  class DiskCacheHandle {
    DiskImageView<PixelT> m_disk_image_view;
    std::string m_filename;

  public:
    template <class ViewT>
    DiskCacheHandle(ImageViewBase<ViewT> const& view, std::string const& filename) :
      m_disk_image_view(filename), m_filename(filename) {      
    }
    
    ~DiskCacheHandle() { 
      vw_out(DebugMessage, "fileio") << "DiskCacheImageView: deleting temporary cache file: " << m_filename << "\n";
      unlink(m_filename.c_str());
    }

    inline const DiskImageView<PixelT>& view() const { return m_disk_image_view; }
  };

  /// This is an assignable image view that stores the assigned data
  /// to a temporary file on disk, and then provides a cached
  /// interface (a la DiskImageView) to that data.  The temporary file
  /// persists until this object and all copies of this object are
  /// destroyed.
  template <class PixelT>
  class DiskCacheImageView : public ImageViewBase< DiskCacheImageView<PixelT> > {
  private:                                      
    boost::shared_ptr<DiskCacheHandle<PixelT> > m_handle;
    std::string m_file_type;

    template <class ViewT>
    void initialize(ImageViewBase<ViewT> const& view, 
                    const ProgressCallback &progress_callback = ProgressCallback::dummy_instance() ) {
      char base_name[] = "/tmp/vw_cache_XXXXXXX";
      std::string filename = mktemp(base_name);
      filename = filename + "." + m_file_type;
      vw_out(InfoMessage, "fileio") << "Creating disk cache of image in: " << filename << "\n";
      write_image(filename, pixel_cast_rescale<PixelT>(view), progress_callback);
      m_handle = boost::shared_ptr<DiskCacheHandle<PixelT> >(new DiskCacheHandle<PixelT>(view.impl(), filename)); 
    }

  public:
    typedef typename DiskImageView<PixelT>::pixel_type pixel_type;
    typedef typename DiskImageView<PixelT>::result_type result_type;
    typedef typename DiskImageView<PixelT>::pixel_accessor pixel_accessor;

    /// Create a temporary image view cache file on disk using a
    /// system supplied temporary filename.
    template <class ViewT>
    DiskCacheImageView(ImageViewBase<ViewT> const& view, std::string const& file_type = "tif",
                       const ProgressCallback &progress_callback = ProgressCallback::dummy_instance() ) : 
      m_file_type(file_type) {
      this->initialize(view.impl(), progress_callback);
    }

    inline int32 cols() const { return m_handle->view().cols(); }
    inline int32 rows() const { return m_handle->view().rows(); }
    inline int32 planes() const { return m_handle->view().planes(); }

    inline pixel_accessor origin() const { return m_handle->view().origin(); }

    inline result_type operator()( int32 i, int32 j, int32 p=0 ) const { return m_handle->view()(i, j, p); }

    template <class ViewT>
    DiskCacheImageView& operator=( ImageViewBase<ViewT> const& view ) {
      this->initialize(view.impl());
      return *this;
    }

    /// \cond INTERNAL
    typedef typename DiskImageView<PixelT>::prerasterize_type prerasterize_type;
    inline prerasterize_type prerasterize( BBox2i const& bbox ) const {
      return m_handle->view().prerasterize(bbox);
    }
    template <class DestT> inline void rasterize( DestT const& dest, BBox2i const& bbox ) const {
      vw::rasterize( prerasterize(bbox), dest, bbox );
    }
    /// \endcond
  };

} // namespace vw

#endif // __VW_FILEIO_DISKIMAGEVIEW_H__
