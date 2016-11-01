/* -*- c++ -*-
 * Copyright (c) 2012-2016 by the GalSim developers team on GitHub
 * https://github.com/GalSim-developers
 *
 * This file is part of GalSim: The modular galaxy image simulation toolkit.
 * https://github.com/GalSim-developers/GalSim
 *
 * GalSim is free software: redistribution and use in source and binary forms,
 * with or without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions, and the disclaimer given in the accompanying LICENSE
 *    file.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the disclaimer given in the documentation
 *    and/or other materials provided with the distribution.
 */

//#define DEBUGLOGGING

#include <sstream>

#include "Image.h"
#include "ImageArith.h"
#include "FFT.h"

namespace galsim {

/////////////////////////////////////////////////////////////////////
//// Constructor for out-of-bounds that has coordinate info
///////////////////////////////////////////////////////////////////////


std::string MakeErrorMessage(
    const std::string& m, const int min, const int max, const int tried)
{
    // See discussion in Std.h about this initial value.
    std::ostringstream oss(" ");
    oss << "Attempt to access "<<m<<" number "<<tried
        << ", range is "<<min<<" to "<<max;
    return oss.str();
}
ImageBoundsError::ImageBoundsError(
    const std::string& m, const int min, const int max, const int tried) :
    ImageError(MakeErrorMessage(m,min,max,tried))
{}

std::string MakeErrorMessage(const int x, const int y, const Bounds<int> b)
{
    std::ostringstream oss(" ");
    bool found=false;
    if (x<b.getXMin() || x>b.getXMax()) {
        oss << "Attempt to access column number "<<x
            << ", range is "<<b.getXMin()<<" to "<<b.getXMax();
        found = true;
    }
    if (y<b.getYMin() || y>b.getYMax()) {
        if (found) oss << " and ";
        oss << "Attempt to access row number "<<y
            << ", range is "<<b.getYMin()<<" to "<<b.getYMax();
        found = true;
    }
    if (!found) return "Cannot find bounds violation ???";
    else return oss.str();
}
ImageBoundsError::ImageBoundsError(const int x, const int y, const Bounds<int> b) :
    ImageError(MakeErrorMessage(x,y,b))
{}

/////////////////////////////////////////////////////////////////////
//// Constructor (and related helpers) for the various Image classes
///////////////////////////////////////////////////////////////////////

template <typename T>
BaseImage<T>::BaseImage(const Bounds<int>& b) :
    AssignableToImage<T>(b), _owner(), _data(0), _nElements(0), _step(0), _stride(0),
    _ncol(0), _nrow(0)
{
    if (this->_bounds.isDefined()) allocateMem();
    // Else _data is left as 0, step,stride = 0.
}

// A custom deleter that finds the original address of the memory allocation directly
// before the stored pointer and frees that using delete []
template <typename T>
struct AlignedDeleter {
    void operator()(T* p) const { delete [] ((char**)p)[-1]; }
};

template <typename T>
void BaseImage<T>::allocateMem()
{
    // Note: this version always does the memory (re-)allocation.
    // So the various functions that call this should do their (different) checks
    // for whether this is necessary.
    _step = 1;
    _stride = _ncol = this->_bounds.getXMax() - this->_bounds.getXMin() + 1;
    _nrow = this->_bounds.getYMax() - this->_bounds.getYMin() + 1;

    _nElements = _stride * (this->_bounds.getYMax() - this->_bounds.getYMin() + 1);
    if (_stride <= 0 || _nElements <= 0) {
        FormatAndThrow<ImageError>() <<
            "Attempt to create an Image with defined but invalid Bounds ("<<this->_bounds<<")";
    }

    // This bit is based on the answers here:
    // http://stackoverflow.com/questions/227897/how-to-allocate-aligned-memory-only-using-the-standard-library/227900
    // The point of this is to get the _data pointer aligned to a 16 byte (128 bit) boundary.
    // Arrays that are so aligned can use SSE operations and so can be much faster than
    // non-aligned memroy.  FFTW in particular is faster if it gets aligned data.
    char* mem = new char[_nElements * sizeof(T) + sizeof(char*) + 15];
    _data = reinterpret_cast<T*>( (uintptr_t)(mem + sizeof(char*) + 15) & ~(size_t) 0x0F );
    ((char**)_data)[-1] = mem;
    _owner.reset(_data, AlignedDeleter<T>());
}

template <typename T>
ImageAlloc<T>::ImageAlloc(int ncol, int nrow, T init_value) :
    BaseImage<T>(Bounds<int>(1,ncol,1,nrow))
{
    if (ncol <= 0 || nrow <= 0) {
        std::ostringstream oss(" ");
        if (ncol <= 0) {
            if (nrow <= 0) {
                oss << "Attempt to create an Image with non-positive ncol ("<<
                    ncol<<") and nrow ("<<nrow<<")";
            } else {
                oss << "Attempt to create an Image with non-positive ncol ("<<
                    ncol<<")";
            }
        } else {
            oss << "Attempt to create an Image with non-positive nrow ("<<
                nrow<<")";
        }
        throw ImageError(oss.str());
    }
    fill(init_value);
}

template <typename T>
ImageAlloc<T>::ImageAlloc(const Bounds<int>& bounds, const T init_value) :
    BaseImage<T>(bounds)
{
    fill(init_value);
}

template <typename T>
void ImageAlloc<T>::resize(const Bounds<int>& new_bounds)
{
    if (!new_bounds.isDefined()) {
        // Then this is really a deallocation.  Clear out the existing memory.
        this->_bounds = new_bounds;
        this->_owner.reset();
        this->_data = 0;
        this->_nElements = 0;
        this->_step = 0;
        this->_stride = 0;
        this->_ncol = 0;
        this->_nrow = 0;
    } else if (this->_bounds.isDefined() &&
               new_bounds.area() <= this->_nElements &&
               this->_owner.unique()) {
        // Then safe to keep existing memory allocation.
        // Just redefine the bounds and stride.
        this->_bounds = new_bounds;
        this->_stride = this->_ncol = new_bounds.getXMax() - new_bounds.getXMin() + 1;
        this->_nrow = new_bounds.getYMax() - new_bounds.getYMin() + 1;
    } else {
        // Then we want to do the reallocation.
        this->_bounds = new_bounds;
        this->allocateMem();
    }
}


/////////////////////////////////////////////////////////////////////
//// Access methods
///////////////////////////////////////////////////////////////////////

template <typename T>
const T& BaseImage<T>::at(const int xpos, const int ypos) const
{
    if (!_data) throw ImageError("Attempt to access values of an undefined image");
    if (!this->_bounds.includes(xpos, ypos)) throw ImageBoundsError(xpos, ypos, this->_bounds);
    return _data[addressPixel(xpos, ypos)];
}

template <typename T>
T& ImageView<T>::at(const int xpos, const int ypos)
{
    if (!this->_data) throw ImageError("Attempt to access values of an undefined image");
    if (!this->_bounds.includes(xpos, ypos)) throw ImageBoundsError(xpos, ypos, this->_bounds);
    return this->_data[this->addressPixel(xpos, ypos)];
}

template <typename T>
T& ImageAlloc<T>::at(const int xpos, const int ypos)
{
    if (!this->_data) throw ImageError("Attempt to access values of an undefined image");
    if (!this->_bounds.includes(xpos, ypos)) throw ImageBoundsError(xpos, ypos, this->_bounds);
    return this->_data[this->addressPixel(xpos, ypos)];
}

template <typename T>
const T& ImageAlloc<T>::at(const int xpos, const int ypos) const
{
    if (!this->_data) throw ImageError("Attempt to access values of an undefined image");
    if (!this->_bounds.includes(xpos, ypos)) throw ImageBoundsError(xpos, ypos, this->_bounds);
    return this->_data[this->addressPixel(xpos, ypos)];
}

template <typename T>
ConstImageView<T> BaseImage<T>::subImage(const Bounds<int>& bounds) const
{
    if (!_data) throw ImageError("Attempt to make subImage of an undefined image");
    if (!this->_bounds.includes(bounds)) {
        FormatAndThrow<ImageError>() <<
            "Subimage bounds (" << bounds << ") are outside original image bounds (" <<
            this->_bounds << ")";
    }
    T* newdata = _data
        + (bounds.getYMin() - this->_bounds.getYMin()) * _stride
        + (bounds.getXMin() - this->_bounds.getXMin()) * _step;
    return ConstImageView<T>(newdata,_owner,_step,_stride,bounds);
}

template <typename T>
ImageView<T> ImageView<T>::subImage(const Bounds<int>& bounds)
{
    if (!this->_data) throw ImageError("Attempt to make subImage of an undefined image");
    if (!this->_bounds.includes(bounds)) {
        FormatAndThrow<ImageError>() <<
            "Subimage bounds (" << bounds << ") are outside original image bounds (" <<
            this->_bounds << ")";
    }
    T* newdata = this->_data
        + (bounds.getYMin() - this->_bounds.getYMin()) * this->_stride
        + (bounds.getXMin() - this->_bounds.getXMin()) * this->_step;
    return ImageView<T>(newdata,this->_owner,this->_step,this->_stride,bounds);
}

namespace {

template <typename T>
class ConstReturn
{
public:
    ConstReturn(const T v): val(v) {}
    T operator()(const T ) const { return val; }
private:
    T val;
};

template <typename T>
class ReturnInverse
{
public:
    T operator()(const T val) const { return val==T(0) ? T(0.) : T(1./val); }
};

template <typename T>
class ReturnSecond
{
public:
    T operator()(T, T v) const { return v; }
};

} // anonymous

template <typename T>
void ImageView<T>::fill(T x)
{
    transform_pixel(*this, ConstReturn<T>(x));
}

template <typename T>
void ImageView<T>::invertSelf()
{
    transform_pixel(*this, ReturnInverse<T>());
}

template <typename T>
void ImageView<T>::copyFrom(const BaseImage<T>& rhs)
{
    if (!this->_bounds.isSameShapeAs(rhs.getBounds()))
        throw ImageError("Attempt im1 = im2, but bounds not the same shape");
    transform_pixel(*this, rhs, ReturnSecond<T>());
}

// instantiate for expected types

template class BaseImage<double>;
template class BaseImage<float>;
template class BaseImage<int32_t>;
template class BaseImage<int16_t>;
template class BaseImage<uint32_t>;
template class BaseImage<uint16_t>;
template class BaseImage<std::complex<double> >;
template class ImageAlloc<double>;
template class ImageAlloc<float>;
template class ImageAlloc<int32_t>;
template class ImageAlloc<int16_t>;
template class ImageAlloc<uint32_t>;
template class ImageAlloc<uint16_t>;
template class ImageAlloc<std::complex<double> >;
template class ImageView<double>;
template class ImageView<float>;
template class ImageView<int32_t>;
template class ImageView<int16_t>;
template class ImageView<uint32_t>;
template class ImageView<uint16_t>;
template class ImageView<std::complex<double> >;
template class ConstImageView<double>;
template class ConstImageView<float>;
template class ConstImageView<int32_t>;
template class ConstImageView<int16_t>;
template class ConstImageView<uint32_t>;
template class ConstImageView<uint16_t>;
template class ConstImageView<std::complex<double> >;
} // namespace galsim

