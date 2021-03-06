/*
 * nvbio
 * Copyright (C) 2011-2014, NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#pragma once

namespace nvbio {

// return true if any item in the range [0,n) evaluates to true
//
template <typename PredicateIterator>
bool any(
    const host_tag          tag,
    const uint32            n,
    const PredicateIterator pred)
{
    return thrust::reduce(
        pred,
        pred + n,
        thrust::logical_or<bool>() );
}

// return true if all items in the range [0,n) evaluate to true
//
template <typename PredicateIterator>
bool all(
    const host_tag          tag,
    const uint32            n,
    const PredicateIterator pred)
{
    return thrust::reduce(
        pred,
        pred + n,
        thrust::logical_and<bool>() );
}

#if defined(__CUDACC__)

// return true if any item in the range [0,n) evaluates to true
//
template <typename PredicateIterator>
bool any(
    const device_tag        tag,
    const uint32            n,
    const PredicateIterator pred)
{
    return cuda::any( n, pred );
}

// return true if any item in the range [0,n) evaluates to true
//
template <typename PredicateIterator>
bool all(
    const device_tag        tag,
    const uint32            n,
    const PredicateIterator pred)
{
    return cuda::all( n, pred );
}

#endif

// return true if any item in the range [0,n) evaluates to true
//
template <typename system_tag, typename PredicateIterator>
bool any(
    const uint32            n,
    const PredicateIterator pred)
{
    return any( system_tag(), n, pred );
}

// return true if all items in the range [0,n) evaluate to true
//
template <typename system_tag, typename PredicateIterator>
bool all(
    const uint32            n,
    const PredicateIterator pred)
{
    return all( system_tag(), n, pred );
}

// a pseudo-iterator to evaluate the predicate (it1[i] <= it2[i]) for arbitrary iterator pairs
//
template <typename Iterator1, typename Iterator2>
struct is_sorted_iterator
{
    // constructor
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    is_sorted_iterator(const Iterator1 _it1, const Iterator2 _it2) : it1( _it1 ), it2( _it2 ) {}

    // dereference operator
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    bool operator[] (const uint32 i) const { return it1[i] <= it2[i]; }

    const Iterator1 it1;
    const Iterator2 it2;
};

// a pseudo-iterator to evaluate the predicate (hd[i] || (it1[i] <= it2[i])) for arbitrary iterator pairs
//
template <typename Iterator1, typename Iterator2, typename Headflags>
struct is_segment_sorted_iterator
{
    // constructor
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    is_segment_sorted_iterator(const Iterator1 _it1, const Iterator2 _it2, const Headflags _hd) : it1( _it1 ), it2( _it2 ), hd(_hd) {}

    // dereference operator
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    bool operator[] (const uint32 i) const { return hd[i] || (it1[i] <= it2[i]); }

    const Iterator1 it1;
    const Iterator2 it2;
    const Headflags hd;
};

// return true if the items in the range [0,n) are sorted
//
template <typename system_tag, typename Iterator>
bool is_sorted(
    const uint32    n,
    const Iterator  values)
{
    return all<system_tag>( n-1, is_sorted_iterator<Iterator,Iterator>( values, values+1 ) );
}

// return true if the items in the range [0,n) are sorted by segment, where
// the beginning of each segment is identified by a set head flag
//
template <typename system_tag, typename Iterator, typename Headflags>
bool is_segment_sorted(
    const uint32            n,
    const Iterator          values,
    const Headflags         flags)
{
    return all<system_tag>( n-1, is_segment_sorted_iterator<Iterator,Iterator,Headflags>( values, values+1, flags+1 ) );
}

// invoke a functor for each element of the given sequence
//
template <typename Iterator, typename Functor>
void for_each(
    const host_tag          tag,
    const uint32            n,
    const Iterator          in,
    const Functor           functor)
{
    #if defined(_OPENMP)
    #pragma omp parallel for if (n >= 256)
    #endif
    for (int64 i = 0; i < int64(n); ++i)
        functor( in[i] );
}

// invoke a functor for each element of the given sequence
//
template <typename Iterator, typename Functor>
void for_each(
    const device_tag        tag,
    const uint32            n,
    const Iterator          in,
    const Functor           functor)
{
    thrust::for_each( in, in + n, functor );
}

// invoke a functor for each element of the given sequence
//
template <typename system_tag, typename Iterator, typename Functor>
void for_each(
    const uint32            n,
    const Iterator          in,
    const Functor           functor)
{
    return for_each( system_tag(), n, in, functor );
}

// apply a functor to each element of the given sequence
//
template <typename Iterator, typename Output, typename Functor>
void transform(
    const device_tag        tag,
    const uint32            n,
    const Iterator          in,
    const Output            out,
    const Functor           functor)
{
    thrust::transform( in, in + n, out, functor );
}

// apply a functor to each element of the given sequence
//
template <typename Iterator, typename Output, typename Functor>
void transform(
    const host_tag          tag,
    const uint32            n,
    const Iterator          in,
    const Output            out,
    const Functor           functor)
{
    #if defined(_OPENMP)
    #pragma omp parallel for if (n >= 256)
    #endif
    for (int64 i = 0; i < int64(n); ++i)
        out[i] = functor( in[i] );
}

// apply a binary functor to each pair of elements of the given sequences
//
template <typename Iterator1, typename Iterator2, typename Output, typename Functor>
void transform(
    const device_tag        tag,
    const uint32            n,
    const Iterator1         in1,
    const Iterator2         in2,
    const Output            out,
    const Functor           functor)
{
    thrust::transform( in1, in1 + n, in2, out, functor );
}

// apply a binary functor to each pair of elements of the given sequences
//
template <typename Iterator1, typename Iterator2, typename Output, typename Functor>
void transform(
    const host_tag          tag,
    const uint32            n,
    const Iterator1         in1,
    const Iterator2         in2,
    const Output            out,
    const Functor           functor)
{
    #if defined(_OPENMP)
    #pragma omp parallel for if (n >= 256)
    #endif
    for (int64 i = 0; i < int64(n); ++i)
        out[i] = functor( in1[i], in2[i] );
}

// apply a functor to each element of the given sequence
//
template <typename system_tag, typename Iterator, typename Output, typename Functor>
void transform(
    const uint32            n,
    const Iterator          in,
    const Output            out,
    const Functor           functor)
{
    transform( system_tag(), n, in, out, functor );
}

// apply a binary functor to each pair of elements of the given sequences
//
template <typename system_tag, typename Iterator1, typename Iterator2, typename Output, typename Functor>
void transform(
    const uint32            n,
    const Iterator1         in1,
    const Iterator2         in2,
    const Output            out,
    const Functor           functor)
{
    transform( system_tag(), n, in1, in2, out, functor );
}

// host-wide reduce
//
// \param n                    number of items to reduce
// \param in                   a system iterator
// \param op                   the binary reduction operator
// \param temp_storage         some temporary storage
//
template <typename InputIterator, typename BinaryOp>
typename std::iterator_traits<InputIterator>::value_type reduce(
    host_tag                            tag,
    const uint32                        n,
    InputIterator                       in,
    BinaryOp                            op,
    nvbio::vector<host_tag,uint8>&      temp_storage)
{
    return thrust::reduce( in, in + n, 0u, op );
}

// host-wide inclusive scan
//
// \param n                    number of items to reduce
// \param in                 a device input iterator
// \param out                a device output iterator
// \param op                   the binary reduction operator
// \param temp_storage       some temporary storage
//
template <typename InputIterator, typename OutputIterator, typename BinaryOp>
void inclusive_scan(
    host_tag                            tag,
    const uint32                        n,
    InputIterator                       in,
    OutputIterator                      out,
    BinaryOp                            op,
    nvbio::vector<host_tag,uint8>&      temp_storage)
{
    thrust::inclusive_scan(
        in,
        in + n,
        out,
        op );
}

// host-wide exclusive scan
//
// \param n                    number of items to reduce
// \param in                 a device input iterator
// \param out                a device output iterator
// \param op                   the binary reduction operator
// \param identity             the identity element
// \param temp_storage       some temporary storage
//
template <typename InputIterator, typename OutputIterator, typename BinaryOp, typename Identity>
void exclusive_scan(
    host_tag                            tag,
    const uint32                        n,
    InputIterator                       in,
    OutputIterator                      out,
    BinaryOp                            op,
    Identity                            identity,
    nvbio::vector<host_tag,uint8>&      temp_storage)
{
    thrust::exclusive_scan(
        in,
        in + n,
        out,
        identity,
        op );
}

#if defined(__CUDACC__)

// system-wide reduce
//
// \param n                    number of items to reduce
// \param in                   a system iterator
// \param op                   the binary reduction operator
// \param temp_storage         some temporary storage
//
template <typename InputIterator, typename BinaryOp>
typename std::iterator_traits<InputIterator>::value_type reduce(
    device_tag                          tag,
    const uint32                        n,
    InputIterator                       in,
    BinaryOp                            op,
    nvbio::vector<device_tag,uint8>&    temp_storage)
{
    return cuda::reduce( n, in, op, temp_storage );
}

// device-wide inclusive scan
//
// \param n                    number of items to reduce
// \param in                 a device input iterator
// \param out                a device output iterator
// \param op                   the binary reduction operator
// \param temp_storage       some temporary storage
//
template <typename InputIterator, typename OutputIterator, typename BinaryOp>
void inclusive_scan(
    device_tag                          tag,
    const uint32                        n,
    InputIterator                       in,
    OutputIterator                      out,
    BinaryOp                            op,
    nvbio::vector<device_tag,uint8>&    temp_storage)
{
    cuda::inclusive_scan( n, in, out, op, temp_storage );
}

// device-wide exclusive scan
//
// \param n                    number of items to reduce
// \param in                 a device input iterator
// \param out                a device output iterator
// \param op                   the binary reduction operator
// \param identity             the identity element
// \param temp_storage       some temporary storage
//
template <typename InputIterator, typename OutputIterator, typename BinaryOp, typename Identity>
void exclusive_scan(
    device_tag                          tag,
    const uint32                        n,
    InputIterator                       in,
    OutputIterator                      out,
    BinaryOp                            op,
    Identity                            identity,
    nvbio::vector<device_tag,uint8>&    temp_storage)
{
    cuda::exclusive_scan( n, in, out, op, identity, temp_storage );
}

#endif

// system-wide reduce
//
// \param n                    number of items to reduce
// \param in                   a system iterator
// \param op                   the binary reduction operator
// \param temp_storage         some temporary storage
//
template <typename system_tag, typename InputIterator, typename BinaryOp>
typename std::iterator_traits<InputIterator>::value_type reduce(
    const uint32                        n,
    InputIterator                       in,
    BinaryOp                            op,
    nvbio::vector<system_tag,uint8>&    temp_storage)
{
    return reduce(
        system_tag(),
        n,
        in,
        op,
        temp_storage );
}

// device-wide inclusive scan
//
// \param n                    number of items to reduce
// \param in                 a device input iterator
// \param out                a device output iterator
// \param op                   the binary reduction operator
// \param temp_storage       some temporary storage
//
template <typename system_tag, typename InputIterator, typename OutputIterator, typename BinaryOp>
void inclusive_scan(
    const uint32                        n,
    InputIterator                       in,
    OutputIterator                      out,
    BinaryOp                            op,
    nvbio::vector<system_tag,uint8>&    temp_storage)
{
    inclusive_scan(
        system_tag(),
        n,
        in,
        out,
        op,
        temp_storage );
}

// device-wide exclusive scan
//
// \param n                    number of items to reduce
// \param in                 a device input iterator
// \param out                a device output iterator
// \param op                   the binary reduction operator
// \param identity             the identity element
// \param temp_storage       some temporary storage
//
template <typename system_tag, typename InputIterator, typename OutputIterator, typename BinaryOp, typename Identity>
void exclusive_scan(
    const uint32                        n,
    InputIterator                       in,
    OutputIterator                      out,
    BinaryOp                            op,
    Identity                            identity,
    nvbio::vector<system_tag,uint8>&    temp_storage)
{
    exclusive_scan(
        system_tag(),
        n,
        in,
        out,
        op,
        identity,
        temp_storage );
}

// host-wide copy of flagged items
//
// \param n                    number of input items
// \param in                    a input iterator
// \param flags                 a flags iterator
// \param out                   a output iterator
// \param temp_storage          some temporary storage
//
// \return                     the number of copied items
//
template <typename InputIterator, typename FlagsIterator, typename OutputIterator>
uint32 copy_flagged(
    const host_tag                  tag,
    const uint32                    n,
    InputIterator                   in,
    FlagsIterator                   flags,
    OutputIterator                  out,
    nvbio::vector<host_tag,uint8>&  temp_storage)
{
    return uint32( thrust::copy_if(
        in,
        in + n,
        flags,
        out,
        nvbio::is_true_functor<bool>() ) - out );
}

// host-wide copy of predicated items
//
// \param n                    number of input items
// \param in                   a input iterator
// \param flags                a flags iterator
// \param out                  a output iterator
// \param temp_storage         some temporary storage
//
// \return                     the number of copied items
//
template <typename InputIterator, typename OutputIterator, typename Predicate>
uint32 copy_if(
    const host_tag                      tag,
    const uint32                        n,
    InputIterator                       in,
    OutputIterator                      out,
    const Predicate                     pred,
    nvbio::vector<host_tag,uint8>&      temp_storage)
{
    return uint32( thrust::copy_if(
        in,
        in + n,
        out,
        pred ) - out );
}

// system-wide run-length encode
//
// \param n                     number of input items
// \param in                    a system input iterator
// \param out                   a system output iterator
// \param counts                a system output count iterator
// \param temp_storage          some temporary storage
//
// \return                     the number of copied items
//
template <typename InputIterator, typename OutputIterator, typename CountIterator>
uint32 runlength_encode(
    const host_tag                      tag,
    const uint32                        n,
    InputIterator                       in,
    OutputIterator                      out,
    CountIterator                       counts,
    nvbio::vector<host_tag,uint8>&      temp_storage)
{
    return uint32( thrust::reduce_by_key(
        in,
        in + n,
        thrust::make_constant_iterator<uint32>( 1u ),
        out,
        counts ) - out );
};


// system-wide run-length encode
//
// \param n                     number of input items
// \param keys_in               a system input iterator
// \param values_in             a system input iterator
// \param keys_out              a system output iterator
// \param values_out            a system output iterator
// \param reduction_op          a reduction operator
// \param temp_storage          some temporary storage
//
// \return                      the number of copied items
//
template <typename KeyIterator, typename ValueIterator, typename OutputKeyIterator, typename OutputValueIterator, typename ReductionOp>
uint32 reduce_by_key(
    const host_tag                      tag,
    const uint32                        n,
    KeyIterator                         keys_in,
    ValueIterator                       values_in,
    OutputKeyIterator                   keys_out,
    OutputValueIterator                 values_out,
    ReductionOp                         reduction_op,
    nvbio::vector<host_tag,uint8>&      temp_storage)
{
    typedef typename std::iterator_traits<KeyIterator>::value_type key_type;

    return thrust::reduce_by_key(
        keys_in,
        keys_in + n,
        values_in,
        keys_out,
        values_out,
        nvbio::equal_functor<key_type>(),
        reduction_op );
}

#if defined(__CUDACC__)

// device-wide copy of flagged items
//
// \param n                    number of input items
// \param in                   a input iterator
// \param flags                a flags iterator
// \param out                  a output iterator
// \param temp_storage         some temporary storage
//
// \return                     the number of copied items
//
template <typename InputIterator, typename FlagsIterator, typename OutputIterator>
uint32 copy_flagged(
    const device_tag                    tag,
    const uint32                        n,
    InputIterator                       in,
    FlagsIterator                       flags,
    OutputIterator                      out,
    nvbio::vector<device_tag,uint8>&    temp_storage)
{
    return cuda::copy_flagged( n, in, flags, out, temp_storage );
}

// device-wide copy of predicated items
//
// \param n                    number of input items
// \param in                   a input iterator
// \param flags                a flags iterator
// \param out                  a output iterator
// \param temp_storage         some temporary storage
//
// \return                     the number of copied items
//
template <typename InputIterator, typename OutputIterator, typename Predicate>
uint32 copy_if(
    const device_tag                    tag,
    const uint32                        n,
    InputIterator                       in,
    OutputIterator                      out,
    const Predicate                     pred,
    nvbio::vector<device_tag,uint8>&    temp_storage)
{
    return cuda::copy_if( n, in, out, pred, temp_storage );
}

// system-wide run-length encode
//
// \param n                     number of input items
// \param in                    a device input iterator
// \param out                   a device output iterator
// \param counts                a device output count iterator
// \param temp_storage          some temporary storage
//
// \return                     the number of copied items
//
template <typename InputIterator, typename OutputIterator, typename CountIterator>
uint32 runlength_encode(
    const device_tag                    tag,
    const uint32                        n,
    InputIterator                       in,
    OutputIterator                      out,
    CountIterator                       counts,
    nvbio::vector<device_tag,uint8>&    temp_storage)
{
    return cuda::runlength_encode( n, in, out, counts, temp_storage );
};

// device-wide run-length encode
//
// \param n                     number of input items
// \param keys_in               a device input iterator
// \param values_in             a device input iterator
// \param keys_out              a device output iterator
// \param values_out            a device output iterator
// \param reduction_op          a reduction operator
// \param temp_storage          some temporary storage
//
// \return                      the number of copied items
//
template <typename KeyIterator, typename ValueIterator, typename OutputKeyIterator, typename OutputValueIterator, typename ReductionOp>
uint32 reduce_by_key(
    const device_tag                    tag,
    const uint32                        n,
    KeyIterator                         keys_in,
    ValueIterator                       values_in,
    OutputKeyIterator                   keys_out,
    OutputValueIterator                 values_out,
    ReductionOp                         reduction_op,
    nvbio::vector<device_tag,uint8>&    temp_storage)
{
    return cuda::reduce_by_key(
        n,
        keys_in,
        values_in,
        keys_out,
        values_out,
        reduction_op,
        temp_storage );
}

#endif

// device-wide copy of flagged items
//
// \param n                    number of input items
// \param in                 a device input iterator
// \param flags              a device flags iterator
// \param out                a device output iterator
// \param temp_storage       some temporary storage
//
// \return                     the number of copied items
//
template <typename system_tag, typename InputIterator, typename FlagsIterator, typename OutputIterator>
uint32 copy_flagged(
    const uint32                        n,
    InputIterator                       in,
    FlagsIterator                       flags,
    OutputIterator                      out,
    nvbio::vector<system_tag,uint8>&    temp_storage)
{
    return copy_flagged( system_tag(), n, in, flags, out, temp_storage );
};

// device-wide copy of predicated items
//
// \param n                    number of input items
// \param in                 a device input iterator
// \param out                a device output iterator
// \param pred                 a unary predicate functor
// \param temp_storage       some temporary storage
//
// \return                     the number of copied items
//
template <typename system_tag, typename InputIterator, typename OutputIterator, typename Predicate>
uint32 copy_if(
    const uint32                        n,
    InputIterator                       in,
    OutputIterator                      out,
    const Predicate                     pred,
    nvbio::vector<system_tag,uint8>&    temp_storage)
{
    return copy_if( system_tag(), n, in, out, pred, temp_storage );
};

// system-wide run-length encode
//
// \param n                     number of input items
// \param in                    a system input iterator
// \param out                   a system output iterator
// \param counts                a system output count iterator
// \param temp_storage          some temporary storage
//
// \return                     the number of copied items
//
template <typename system_tag, typename InputIterator, typename OutputIterator, typename CountIterator>
uint32 runlength_encode(
    const uint32                        n,
    InputIterator                       in,
    OutputIterator                      out,
    CountIterator                       counts,
    nvbio::vector<system_tag,uint8>&    temp_storage)
{
    return runlength_encode( system_tag(), n, in, out, counts, temp_storage );
};

// system-wide run-length encode
//
// \param n                     number of input items
// \param keys_in               a system input iterator
// \param values_in             a system input iterator
// \param keys_out              a system output iterator
// \param values_out            a system output iterator
// \param reduction_op          a reduction operator
// \param temp_storage          some temporary storage
//
// \return                      the number of copied items
//
template <typename system_tag, typename KeyIterator, typename ValueIterator, typename OutputKeyIterator, typename OutputValueIterator, typename ReductionOp>
uint32 reduce_by_key(
    const uint32                        n,
    KeyIterator                         keys_in,
    ValueIterator                       values_in,
    OutputKeyIterator                   keys_out,
    OutputValueIterator                 values_out,
    ReductionOp                         reduction_op,
    nvbio::vector<system_tag,uint8>&    temp_storage)
{
    return reduce_by_key(
        system_tag(),
        n,
        keys_in,
        values_in,
        keys_out,
        values_out,
        reduction_op,
        temp_storage );
}

} // namespace nvbio
