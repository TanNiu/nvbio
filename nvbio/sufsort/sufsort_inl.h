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

#include <nvbio/sufsort/sufsort_priv.h>
#include <nvbio/sufsort/compression_sort.h>
#include <nvbio/sufsort/prefix_doubling_sufsort.h>
#include <nvbio/sufsort/blockwise_sufsort.h>
#include <nvbio/sufsort/dcs.h>
#include <nvbio/basic/string_set.h>
#include <nvbio/basic/thrust_view.h>
#include <nvbio/basic/cuda/sort.h>
#include <nvbio/basic/timer.h>
#include <thrust/device_vector.h>
#include <thrust/transform_scan.h>
#include <thrust/binary_search.h>
#include <thrust/iterator/constant_iterator.h>
#include <thrust/iterator/counting_iterator.h>
#include <thrust/sort.h>
#include <mgpuhost.cuh>
#include <moderngpu.cuh>


namespace nvbio {

namespace cuda {

// return the position of the primary suffix of a string
//
template <typename string_type>
typename string_type::index_type find_primary(
    const typename string_type::index_type  string_len,
    const string_type                       string)
{
    const uint32 SYMBOL_SIZE = string_type::SYMBOL_SIZE;

    // compute the primary by simply counting how many of the suffixes between 1 and N
    // are lexicographically less than the primary suffix
    return thrust::transform_reduce(
        thrust::make_counting_iterator<uint32>(1u),
        thrust::make_counting_iterator<uint32>(0u) + string_len,
        bind_second_functor< priv::string_suffix_less<SYMBOL_SIZE,string_type> >(
            priv::string_suffix_less<SYMBOL_SIZE,string_type>( string_len, string ),
            0u ),
        0u,
        thrust::plus<uint32>() ) + 1u;
}

// Sort the suffixes of all the strings in the given string_set
//
template <typename string_set_type, typename output_handler>
void suffix_sort(
    const string_set_type&   string_set,
          output_handler&    output,
          BWTParams*         params)
{
    typedef uint32 word_type;
    const uint32 WORD_BITS   = uint32( 8u * sizeof(word_type) );
    const uint32 DOLLAR_BITS = 4;

    const uint32 SYMBOL_SIZE      = 2u;
    const uint32 SYMBOLS_PER_WORD = priv::symbols_per_word<SYMBOL_SIZE,WORD_BITS,DOLLAR_BITS>();

    const uint32 n = string_set.size();

    int current_device;
    cudaGetDevice( &current_device );
    mgpu::ContextPtr mgpu_ctxt = mgpu::CreateCudaDevice( current_device ); 

    // instantiate a suffix flattener on the string set
    priv::SetSuffixFlattener<SYMBOL_SIZE> suffixes( mgpu_ctxt );
    suffixes.set( string_set );

    // compute the maximum number of words needed to represent a suffix
    const uint32 m = (suffixes.max_length( string_set ) + SYMBOLS_PER_WORD-1) / SYMBOLS_PER_WORD;

    // compute the number of suffixes
    const uint32 n_suffixes = suffixes.n_suffixes;

    thrust::device_vector<word_type> radices( n_suffixes*2 );
    thrust::device_vector<uint32>    indices( n_suffixes*2 );

    // initialize the list of suffix indices
    thrust::copy(
        thrust::make_counting_iterator<uint32>(0u),
        thrust::make_counting_iterator<uint32>(n_suffixes),
        indices.begin() );

    cuda::SortBuffers<word_type*,uint32*> sort_buffers;
    cuda::SortEnactor                     sort_enactor;

    sort_buffers.selector  = 0;
    sort_buffers.keys[0]   = nvbio::device_view( radices );
    sort_buffers.keys[1]   = nvbio::device_view( radices ) + n_suffixes;
    sort_buffers.values[0] = nvbio::device_view( indices );
    sort_buffers.values[1] = nvbio::device_view( indices ) + n_suffixes;

    // do what is essentially an LSD radix-sort on the suffixes, word by word
    for (int32 word_idx = m-1; word_idx >= 0; --word_idx)
    {
        // extract the given radix word from each of the partially sorted suffixes
        suffixes.flatten(
            string_set,
            word_idx,
            priv::Bits<WORD_BITS,DOLLAR_BITS>(),
            indices.begin() + sort_buffers.selector * n_suffixes,
            radices.begin() + sort_buffers.selector * n_suffixes );

        // and sort them
        sort_enactor.sort( n_suffixes, sort_buffers );
    }

    output.process(
        n_suffixes,
        nvbio::device_view( indices ) + sort_buffers.selector * n_suffixes,
        nvbio::device_view( suffixes.string_ids ),
        nvbio::device_view( suffixes.cum_lengths ));
}

// Sort all the suffixes of a given string
//
template <typename string_type, typename output_iterator>
void suffix_sort(
    const typename stream_traits<string_type>::index_type   string_len,
    const string_type                                       string,
    output_iterator                                         output,
    BWTParams*                                              params)
{
    PrefixDoublingSufSort sufsort;
    sufsort.sort(
        string_len,
        string,
        output + 1u );

    // assign the zero'th suffix
    output[0] = string_len;

    NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    extract  : %5.1f ms\n", 1.0e3f * sufsort.extract_time) );
    NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    gather   : %5.1f ms\n", 1.0e3f * sufsort.gather_time) );
    NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    r-sort   : %5.1f ms\n", 1.0e3f * sufsort.radixsort_time) );
    NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    segment  : %5.1f ms\n", 1.0e3f * sufsort.segment_time) );
    NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    invert   : %5.1f ms\n", 1.0e3f * sufsort.inverse_time) );
    NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    compact  : %5.1f ms\n", 1.0e3f * sufsort.compact_time) );
}

// a utility SuffixHandler to rank the sorted suffixes
//
template <typename string_type, typename output_iterator>
struct StringBWTHandler
{
    static const uint32 NULL_PRIMARY = uint32(-1); // TODO: switch to index_type

    // constructor
    //
    StringBWTHandler(
        const uint32        _string_len,
        const string_type   _string,
        output_iterator     _output) :
        string_len( _string_len ),
        string( _string ),
        primary(NULL_PRIMARY),
        n_output(0),
        output(_output) {}

    // process the next batch of suffixes
    //
    void process_batch(
        const uint32  n_suffixes,
        const uint32* d_suffixes)
    {
        priv::alloc_storage( d_block_bwt, n_suffixes );

        // compute the bwt of the block
        thrust::transform(
            thrust::device_ptr<const uint32>( d_suffixes ),
            thrust::device_ptr<const uint32>( d_suffixes ) + n_suffixes,
            d_block_bwt.begin(),
            priv::string_bwt_functor<string_type>( string_len, string ) );

        // check if there is a $ sign
        const uint32 block_primary = uint32( thrust::find(
            d_block_bwt.begin(),
            d_block_bwt.begin() + n_suffixes,
            255u ) - d_block_bwt.begin() );

        if (block_primary < n_suffixes)
        {
            // keep track of the global primary position
            primary = n_output + block_primary + 1u;                // +1u for the implicit empty suffix
        }

        // and copy the transformed block to the output
        priv::device_copy(
            n_suffixes,
            d_block_bwt.begin(),
            output,
            n_output + 1u );                                        // +1u for the implicit empty suffix

        n_output += n_suffixes;
    }

    // process a sparse set of suffixes
    //
    void process_scattered(
        const uint32  n_suffixes,
        const uint32* d_suffixes,
        const uint32* d_slots)
    {
        priv::alloc_storage( d_block_bwt, n_suffixes );

        // compute the bwt of the block
        thrust::transform(
            thrust::device_ptr<const uint32>( d_suffixes ),
            thrust::device_ptr<const uint32>( d_suffixes ) + n_suffixes,
            d_block_bwt.begin(),
            priv::string_bwt_functor<string_type>( string_len, string ) );

        // check if there is a $ sign
        const uint32 block_primary = uint32( thrust::find(
            d_block_bwt.begin(),
            d_block_bwt.begin() + n_suffixes,
            255u ) - d_block_bwt.begin() );

        if (block_primary < n_suffixes)
        {
            // keep track of the global primary position
            primary = thrust::device_ptr<const uint32>( d_slots )[ block_primary ] + 1u; // +1u for the implicit empty suffix
        }

        // and scatter the resulting symbols in the proper place
        priv::device_scatter(
            n_suffixes,
            d_block_bwt.begin(),
            thrust::make_transform_iterator(
                thrust::device_ptr<const uint32>( d_slots ),
                priv::offset_functor(1u) ),                                              // +1u for the implicit empty suffix
            output );
    }

    const uint32                    string_len;
    const string_type               string;
    uint32                          primary;
    uint32                          n_output;
    output_iterator                 output;
    thrust::device_vector<uint32>   d_block_bwt;
};

// Compute the bwt of a device-side string
//
// \return         position of the primary suffix / $ symbol
//
template <typename string_type, typename output_iterator>
typename string_type::index_type bwt(
    const typename string_type::index_type  string_len,
    string_type                             string,
    output_iterator                         output,
    BWTParams*                              params)
{
    typedef typename string_type::index_type index_type;

    // build a table for our Difference Cover
    log_verbose(stderr, "  building DCS... started\n");

    DCS dcs;

    blockwise_build(
        dcs,
        string_len,
        string,
        params );

    log_verbose(stderr, "  building DCS... done\n");

    log_verbose(stderr, "  DCS-based sorting... started\n");

    // encode the first BWT symbol explicitly
    priv::device_copy( 1u, string + string_len-1, output, index_type(0u) );

    // and build the rest of the BWT
    StringBWTHandler<string_type,output_iterator> bwt_handler(
        string_len,
        string,
        output );

    blockwise_suffix_sort(
        string_len,
        string,
        string_len,
        thrust::make_counting_iterator<uint32>(0u),
        bwt_handler,
        &dcs,
        params );

    log_verbose(stderr, "  DCS-based sorting... done\n");

    NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"\n    primary at %llu\n", bwt_handler.primary) );

    // shift back all symbols following the primary
    {
        const uint32 max_block_size = 32*1024*1024;

        priv::alloc_storage( bwt_handler.d_block_bwt, max_block_size );

        for (index_type block_begin = bwt_handler.primary; block_begin < string_len; block_begin += max_block_size)
        {
            const index_type block_end = nvbio::min( block_begin + max_block_size, string_len );

            // copy all symbols to a temporary buffer
            priv::device_copy(
                block_end - block_begin,
                output + block_begin + 1u,
                bwt_handler.d_block_bwt.begin(),
                uint32(0) );

            // and copy the shifted block to the output
            priv::device_copy(
                block_end - block_begin,
                bwt_handler.d_block_bwt.begin(),
                output,
                block_begin );
        }
    }

    return bwt_handler.primary;
}

template <uint32 BUCKETING_BITS_T, uint32 SYMBOL_SIZE, bool BIG_ENDIAN, typename storage_type>
struct HostBWTConfig
{
    typedef typename std::iterator_traits<storage_type>::value_type word_type;

    static const uint32 WORD_BITS       = uint32( 8u * sizeof(word_type) );
    static const uint32 DOLLAR_BITS     = WORD_BITS <= 32 ? 4 : 5;
    static const uint32 BUCKETING_BITS  = BUCKETING_BITS_T;

    typedef ConcatenatedStringSet<
            PackedStreamIterator< PackedStream<storage_type,uint8,SYMBOL_SIZE,BIG_ENDIAN,uint64> >,
            uint64*>    string_set_type;

    typedef priv::HostChunkLoader<SYMBOL_SIZE,BIG_ENDIAN,storage_type>                      chunk_loader;
    typedef priv::HostStringSetRadices<string_set_type,SYMBOL_SIZE,DOLLAR_BITS,WORD_BITS>   string_set_handler;

    typedef typename priv::word_selector<BUCKETING_BITS>::type                              bucket_type;
    typedef priv::SetSuffixBucketer<SYMBOL_SIZE,BUCKETING_BITS,DOLLAR_BITS,bucket_type>     suffix_bucketer;
};

template <uint32 BUCKETING_BITS_T, uint32 SYMBOL_SIZE, bool BIG_ENDIAN, typename storage_type>
struct DeviceBWTConfig
{
    typedef typename std::iterator_traits<storage_type>::value_type word_type;

    static const uint32 WORD_BITS   = uint32( 8u * sizeof(word_type) );
    static const uint32 DOLLAR_BITS = WORD_BITS <= 32 ? 4 : 5;
    static const uint32 BUCKETING_BITS  = BUCKETING_BITS_T;

    typedef ConcatenatedStringSet<
            PackedStreamIterator< PackedStream<storage_type,uint8,SYMBOL_SIZE,BIG_ENDIAN,uint64> >,
            uint64*>    string_set_type;

    typedef priv::DeviceChunkLoader<SYMBOL_SIZE,BIG_ENDIAN,storage_type>                    chunk_loader;
    typedef priv::DeviceStringSetRadices<string_set_type,SYMBOL_SIZE,DOLLAR_BITS,WORD_BITS> string_set_handler;

    typedef typename priv::word_selector<BUCKETING_BITS>::type                              bucket_type;
    typedef priv::SetSuffixBucketer<SYMBOL_SIZE,BUCKETING_BITS,DOLLAR_BITS,bucket_type>     suffix_bucketer;
};

// simple status class
struct LargeBWTStatus
{
    enum {
        OK          = 0,
        LargeBucket = 1,
    };

    // default constructor
    LargeBWTStatus() : code(OK) {}

    // return whether the status is OK
    operator bool() const { return code == OK ? true : false; }

    uint32 code;
    uint32 bucket_size;
    uint32 bucket_index;
};

template <typename ConfigType, uint32 SYMBOL_SIZE, bool BIG_ENDIAN, typename storage_type>
struct LargeBWTSkeleton
{
    typedef typename std::iterator_traits<storage_type>::value_type word_type;
    typedef typename ConfigType::string_set_handler                 string_set_handler_type;
    typedef typename ConfigType::chunk_loader                       chunk_loader_type;
    typedef typename chunk_loader_type::chunk_set_type              chunk_set_type;
    typedef typename ConfigType::bucket_type                        bucket_type;
    typedef typename ConfigType::suffix_bucketer                    suffix_bucketer_type;

    typedef ConcatenatedStringSet<
            typename PackedStream<storage_type,uint8,SYMBOL_SIZE,BIG_ENDIAN,uint64>::iterator,
            uint64*>    string_set_type;

    // compute the maximum sub-bucket size
    //
    static uint32 max_subbucket_size(
        const thrust::host_vector<uint32>&  h_buckets,
        const uint32                        max_super_block_size,
        const uint32                        limit,
        LargeBWTStatus*                     status)
    {
        NVBIO_VAR_UNUSED const uint32 DOLLAR_BITS = ConfigType::DOLLAR_BITS;
        NVBIO_VAR_UNUSED const uint32 DOLLAR_MASK = (1u << DOLLAR_BITS) - 1u;

        uint32 max_size  = 0u;
        uint32 max_index = 0u;

        // build the subbucket pointers
        for (uint32 bucket_begin = 0, bucket_end = 0; bucket_begin < h_buckets.size(); bucket_begin = bucket_end)
        {
            // grow the block of buckets until we can
            uint32 bucket_size;
            for (bucket_size = 0; (bucket_end < h_buckets.size()) && (bucket_size + h_buckets[bucket_end] <= max_super_block_size); ++bucket_end)
                bucket_size += h_buckets[bucket_end];

            // check whether a single bucket exceeds our host buffer capacity
            // TODO: if this is a short-string bucket, we could handle it with special care,
            // but it requires modifying the collecting loop to output everything directly.
            if (bucket_end == bucket_begin)
                throw nvbio::runtime_error("bucket %u contains %u strings: buffer overflow!", bucket_begin, h_buckets[bucket_begin]);

            // loop through the sub-buckets
            for (uint32 subbucket = bucket_begin; subbucket < bucket_end; ++subbucket)
            {
                // only keep track of buckets that are NOT short-string buckets
                if ((subbucket & DOLLAR_MASK) == DOLLAR_MASK)
                {
                    if (max_size < h_buckets[subbucket])
                    {
                        max_size  = h_buckets[subbucket];
                        max_index = subbucket;
                    }
                }
            }
        }

        if (max_size > limit)
        {
            status->code = LargeBWTStatus::LargeBucket;
            status->bucket_size  = max_size;
            status->bucket_index = max_index;
        }

        return max_size;
    }

    // construct the sub-bucket lists
    //
    static void build_subbuckets(
        const thrust::host_vector<uint32>&  h_buckets,
        thrust::host_vector<uint32>&        h_subbuckets,
        const uint32                        max_super_block_size,
        const uint32                        max_block_size)
    {
        NVBIO_VAR_UNUSED const uint32 DOLLAR_BITS = ConfigType::DOLLAR_BITS;
        NVBIO_VAR_UNUSED const uint32 DOLLAR_MASK = (1u << DOLLAR_BITS) - 1u;

        // build the subbucket pointers
        for (uint32 bucket_begin = 0, bucket_end = 0; bucket_begin < h_buckets.size(); bucket_begin = bucket_end)
        {
            // grow the block of buckets until we can
            uint32 bucket_size;
            for (bucket_size = 0; (bucket_end < h_buckets.size()) && (bucket_size + h_buckets[bucket_end] <= max_super_block_size); ++bucket_end)
                bucket_size += h_buckets[bucket_end];

            // check whether a single bucket exceeds our host buffer capacity
            // TODO: if this is a short-string bucket, we could handle it with special care,
            // but it requires modifying the collecting loop to output everything directly.
            if (bucket_end == bucket_begin)
                throw nvbio::runtime_error("bucket %u contains %u strings: buffer overflow!", bucket_begin, h_buckets[bucket_begin]);

            // build the sub-buckets
            for (uint32 subbucket_begin = bucket_begin, subbucket_end = bucket_begin; subbucket_begin < bucket_end; subbucket_begin = subbucket_end)
            {
                if (h_buckets[subbucket_begin] > max_block_size)
                {
                    // if this is NOT a short-string bucket, we can't cope with it
                    if ((subbucket_begin & DOLLAR_MASK) == DOLLAR_MASK)
                        throw nvbio::runtime_error("bucket %u contains %u strings: buffer overflow!", subbucket_begin, h_buckets[subbucket_begin]);

                    // this is a short-string bucket: we can handle it with special care
                    h_subbuckets[ subbucket_end++ ] = subbucket_begin; // point to the beginning of this sub-bucket
                }
                else
                {
                    // grow the block of sub-buckets until we can
                    uint32 subbucket_size;
                    for (subbucket_size = 0; (subbucket_end < bucket_end) && (subbucket_size + h_buckets[subbucket_end] <= max_block_size); ++subbucket_end)
                    {
                        subbucket_size += h_buckets[subbucket_end];

                        h_subbuckets[ subbucket_end ] = subbucket_begin; // point to the beginning of this sub-bucket
                    }
                }
            }
        }
    }

    template <typename output_handler>
    static LargeBWTStatus enact(
        const string_set_type       string_set,
        output_handler&             output,
        BWTParams*                  params)
    {
        NVBIO_VAR_UNUSED const uint32 BUCKETING_BITS = ConfigType::BUCKETING_BITS;
        NVBIO_VAR_UNUSED const uint32 DOLLAR_BITS    = ConfigType::DOLLAR_BITS;
        NVBIO_VAR_UNUSED const uint32 DOLLAR_MASK    = (1u << DOLLAR_BITS) - 1u;
        NVBIO_VAR_UNUSED const uint32 SLICE_SIZE     = 4;

        const uint32 M              = 128*1024;
        const uint32 N              = string_set.size();
        const uint32 n_chunks       = (N + M-1) / M;

        LargeBWTStatus          status;

        mgpu::ContextPtr        mgpu_ctxt = mgpu::CreateCudaDevice(0); 

        suffix_bucketer_type    bucketer( mgpu_ctxt );
        chunk_loader_type       chunk;
        string_set_handler_type string_set_handler( string_set );
        cuda::CompressionSort   string_sorter( mgpu_ctxt );

        const uint32 max_super_block_size = params ?              // requires max_super_block_size*8 host memory bytes
            (params->host_memory - (128u*1024u*1024u)) / 8u :     // leave 128MB for the bucket counters
            512*1024*1024;
        uint32 max_block_size = params ?
            params->device_memory / 32 :                          // requires max_block_size*32 device memory bytes
            32*1024*1024;                                         // default: 1GB

        NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"  super-block-size: %.1f M\n", float(max_super_block_size)/float(1024*1024)) );
        NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"        block-size: %.1f M\n", float(max_block_size)/float(1024*1024)) );
        thrust::host_vector<uint2>       h_suffixes( max_super_block_size );
        thrust::host_vector<uint2>       h_block_suffixes;
        thrust::host_vector<bucket_type> h_block_radices;
        thrust::host_vector<uint8>       h_block_bwt;

        // reuse some buffers
        thrust::device_vector<uint32>&   d_indices = bucketer.d_indices;
        thrust::device_vector<uint2>     d_bucket_suffixes;
        thrust::device_vector<uint8>     d_block_bwt;
        //thrust::device_vector<uint8>     d_temp_storage;

        // global bucket sizes
        thrust::device_vector<uint32> d_buckets( 1u << BUCKETING_BITS, 0u );

        // allocate an MGPU context
        mgpu::ContextPtr mgpu = mgpu::CreateCudaDevice(0);

        float bwt_time    = 0.0f;
        float output_time = 0.0f;

        // output the last character of each string (i.e. the symbols preceding all the dollar signs)
        const uint32 block_size = max_block_size / 4u; // this can be done in relatively small blocks
        for (uint32 block_begin = 0; block_begin < N; block_begin += block_size)
        {
            const uint32 block_end = nvbio::min( block_begin + block_size, N );

            // consume subbucket_size suffixes
            const uint32 n_suffixes = block_end - block_begin;

            Timer timer;
            timer.start();

            priv::alloc_storage( h_block_bwt, n_suffixes );
            priv::alloc_storage( d_block_bwt, n_suffixes );

            // load the BWT symbols
            string_set_handler.dollar_bwt(
                block_begin,
                block_end,
                plain_view( h_block_bwt ) );

            // copy them to the device
            thrust::copy(
                h_block_bwt.begin(),
                h_block_bwt.begin() + n_suffixes,
                d_block_bwt.begin() );

            timer.stop();
            bwt_time += timer.seconds();

            timer.start();

            // invoke the output handler
            output.process(
                n_suffixes,
                plain_view( h_block_bwt ),
                plain_view( d_block_bwt ),
                NULL,
                NULL,
                NULL );

            timer.stop();
            output_time += timer.seconds();
        }

        float load_time  = 0.0f;
        float merge_time = 0.0f;
        float count_time = 0.0f;
        Timer count_timer;
        count_timer.start();

        uint64 total_suffixes = 0;

        for (uint32 chunk_id = 0; chunk_id < n_chunks; ++chunk_id)
        {
            const uint32 chunk_begin = chunk_id * M;
            const uint32 chunk_end   = nvbio::min( chunk_begin + M, N );

            //
            // load a chunk in device memory
            //

            Timer timer;
            timer.start();

            chunk_set_type d_chunk_set = chunk.load( string_set, chunk_begin, chunk_end );

            NVBIO_CUDA_DEBUG_STATEMENT( cudaDeviceSynchronize() );
            timer.stop();
            load_time += timer.seconds();

            timer.start();

            // count the chunk's buckets
            bucketer.count( d_chunk_set );

            total_suffixes += bucketer.suffixes.n_suffixes;

            NVBIO_CUDA_DEBUG_STATEMENT( cudaDeviceSynchronize() );
            timer.stop();
            count_time += timer.seconds();

            timer.start();

            // and merge them in with the global buckets
            thrust::transform(
                bucketer.d_buckets.begin(),
                bucketer.d_buckets.end(),
                d_buckets.begin(),
                d_buckets.begin(),
                thrust::plus<uint32>() );

            NVBIO_CUDA_DEBUG_STATEMENT( cudaDeviceSynchronize() );
            timer.stop();
            merge_time += timer.seconds();
        }

        count_timer.stop();

        thrust::host_vector<uint32> h_buckets( d_buckets );
        thrust::host_vector<uint64> h_bucket_offsets( d_buckets.size() );
        thrust::host_vector<uint32> h_subbuckets( d_buckets.size() );

        const uint32 max_bucket_size = thrust::reduce(
            d_buckets.begin(),
            d_buckets.end(),
            0u,
            thrust::maximum<uint32>() );

        // scan the bucket offsets so as to have global positions
        thrust::exclusive_scan(
            thrust::make_transform_iterator( h_buckets.begin(), priv::cast_functor<uint32,uint64>() ),
            thrust::make_transform_iterator( h_buckets.end(),   priv::cast_functor<uint32,uint64>() ),
            h_bucket_offsets.begin() );

        // compute the largest non-elementary bucket
        const uint32 largest_subbucket = max_subbucket_size( h_buckets, max_super_block_size, max_block_size, &status );
        if (!status)
            return status;

        NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    max bucket size: %u (%u)\n", largest_subbucket, max_bucket_size) );
        NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    counting : %.1fs\n", count_timer.seconds() ) );
        NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"      load   : %.1fs\n", load_time) );
        NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"      merge  : %.1fs\n", merge_time) );
        NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"      setup    : %.1fs\n", bucketer.d_setup_time) );
        NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"        scan   : %.1fs\n", bucketer.suffixes.d_scan_time) );
        NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"        search : %.1fs\n", bucketer.suffixes.d_search_time) );
        NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"      count  : %.1fs\n", count_time) );
        NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"        flatten : %.1fs\n", bucketer.d_flatten_time) );
        NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"        sort    : %.1fs\n", bucketer.d_count_sort_time) );
        NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"        search  : %.1fs\n", bucketer.d_search_time) );

        bucketer.clear_timers();

        //
        // at this point, we have to do multiple passes through the input string set,
        // collecting in each pass as many buckets as we can fit in memory at once
        //

        float sufsort_time = 0.0f;
        float collect_time = 0.0f;
        float bin_time     = 0.0f;

        // reduce the scratchpads size if possible
        const uint32 optimal_block_size = 32*1024*1024;
        if (largest_subbucket <= optimal_block_size)
            max_block_size     = optimal_block_size;

        // reserve memory for scratchpads
        {
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"  allocating scratchpads\n" ) );

            string_set_handler.reserve( max_block_size, SLICE_SIZE );
            string_sorter.reserve( max_block_size );

            priv::alloc_storage( h_block_radices,   max_block_size );
            priv::alloc_storage( h_block_suffixes,  max_block_size );
            priv::alloc_storage( h_block_bwt,       max_block_size );
            priv::alloc_storage( d_block_bwt,       max_block_size );
            priv::alloc_storage( d_indices,         max_block_size );
            priv::alloc_storage( d_bucket_suffixes, max_block_size );

            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"  allocated device memory: %.1f MB\n",
                float( bucketer.allocated_device_memory() + string_set_handler.allocated_device_memory() + string_sorter.allocated_device_memory() ) / float(1024*1024) ) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    bucketer : %.1f MB\n", float( bucketer.allocated_device_memory() ) / float(1024*1024) ) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    handler  : %.1f MB\n", float( string_set_handler.allocated_device_memory() ) / float(1024*1024) ) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    sorter   : %.1f MB\n", float( string_sorter.allocated_device_memory() ) / float(1024*1024) ) );
        }

        // now build the sub-bucket lists
        build_subbuckets(
            h_buckets,
            h_subbuckets,
            max_super_block_size,
            max_block_size );

        // build the subbucket pointers
        thrust::device_vector<uint32> d_subbuckets( h_subbuckets );

        uint64 global_suffix_offset = 0;

        for (uint32 bucket_begin = 0, bucket_end = 0; bucket_begin < h_buckets.size(); bucket_begin = bucket_end)
        {
            // grow the block of buckets until we can
            uint32 bucket_size;
            for (bucket_size = 0; (bucket_end < h_buckets.size()) && (bucket_size + h_buckets[bucket_end] <= max_super_block_size); ++bucket_end)
                bucket_size += h_buckets[bucket_end];

            uint32 suffix_count   = 0;
            uint32 string_count   = 0;
            uint32 max_suffix_len = 0;

            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"  collect buckets[%u:%u] (%u suffixes)\n", bucket_begin, bucket_end, bucket_size) );
            Timer collect_timer;
            collect_timer.start();

            for (uint32 chunk_id = 0; chunk_id < n_chunks; ++chunk_id)
            {
                const uint32 chunk_begin = chunk_id * M;
                const uint32 chunk_end   = nvbio::min( chunk_begin + M, N );
                const uint32 chunk_size  = chunk_end - chunk_begin;

                //
                // load a chunk in device memory
                //

                chunk_set_type d_chunk_set = chunk.load( string_set, chunk_begin, chunk_end );

                // collect the chunk's suffixes within the bucket range
                uint32 suffix_len;

                const uint32 n_collected = bucketer.collect(
                    d_chunk_set,
                    bucket_begin,
                    bucket_end,
                    string_count,
                    suffix_len,
                    d_subbuckets.begin(),
                    h_block_radices,
                    h_block_suffixes );

                if (suffix_count + n_collected > max_super_block_size)
                {
                    log_error(stderr,"buffer size exceeded! (%u/%u)\n", suffix_count, max_super_block_size);
                    exit(1);
                }

                Timer timer;
                timer.start();

                // dispatch each suffix to their respective bucket
                for (uint32 i = 0; i < n_collected; ++i)
                {
                    const uint2  loc    = h_block_suffixes[i];
                    const uint32 bucket = h_block_radices[i];
                    const uint64 slot   = h_bucket_offsets[bucket]++; // this could be done in parallel using atomics

                    NVBIO_CUDA_DEBUG_ASSERT(
                        slot >= global_suffix_offset,
                        slot <  global_suffix_offset + max_super_block_size,
                        "[%u] = (%u,%u) placed at %llu - %llu (%u)\n", i, loc.x, loc.y, slot, global_suffix_offset, bucket );

                    h_suffixes[ slot - global_suffix_offset ] = loc;
                }

                timer.stop();
                bin_time += timer.seconds();

                suffix_count += n_collected;
                string_count += chunk_size;

                max_suffix_len = nvbio::max( max_suffix_len, suffix_len );
            }
            collect_timer.stop();
            collect_time += collect_timer.seconds();
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"  collect : %.1fs (%.1f M suffixes/s - %.1f M scans/s)\n", collect_time, 1.0e-6f*float(global_suffix_offset + suffix_count)/collect_time, 1.0e-6f*float(total_suffixes)/collect_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    setup    : %.1fs\n", bucketer.d_setup_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"      scan   : %.1fs\n", bucketer.suffixes.d_scan_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"      search : %.1fs\n", bucketer.suffixes.d_search_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    flatten  : %.1fs\n", bucketer.d_flatten_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    filter   : %.1fs\n", bucketer.d_filter_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    remap    : %.1fs\n", bucketer.d_remap_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    max      : %.1fs\n", bucketer.d_max_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    sort     : %.1fs\n", bucketer.d_collect_sort_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    copy     : %.1fs\n", bucketer.d_copy_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    bin      : %.1fs\n", bin_time) );

            //
            // at this point we have a large collection of localized suffixes to sort in h_suffixes;
            // we'll do it looping on multiple sub-buckets, on the GPU
            //

            suffix_count = 0u;

            const uint32 n_words = string_set_handler.num_words( max_suffix_len );

            for (uint32 subbucket_begin = bucket_begin, subbucket_end = bucket_begin; subbucket_begin < bucket_end; subbucket_begin = subbucket_end)
            {
                if (h_buckets[subbucket_begin] > max_block_size)
                {
                    // check if this is not a short-string bucket - it should never actually happen as we already tested for it
                    if ((subbucket_begin & DOLLAR_MASK) == DOLLAR_MASK)
                        throw nvbio::runtime_error("bucket %u contains %u strings: overflow!", subbucket_begin, h_buckets[subbucket_begin]);

                    // advance by one
                    ++subbucket_end;

                    const uint32 subbucket_size = h_buckets[subbucket_begin];

                    Timer suf_timer;
                    suf_timer.start();

                    // chop the bucket in multiple blocks
                    for (uint32 block_begin = 0; block_begin < subbucket_size; block_begin += max_block_size)
                    {
                        const uint32 block_end = nvbio::min( block_begin + max_block_size, subbucket_size );

                        // consume subbucket_size suffixes
                        const uint32 n_suffixes = block_end - block_begin;

                        // copy the host suffixes to the device
                        const uint2* h_bucket_suffixes = &h_suffixes[0] + suffix_count + block_begin;

                        // copy the suffix list to the device
                        priv::alloc_storage( d_bucket_suffixes, n_suffixes );
                        thrust::copy(
                            h_bucket_suffixes,
                            h_bucket_suffixes + n_suffixes,
                            d_bucket_suffixes.begin() );

                        // initialize the set radices
                        string_set_handler.init( n_suffixes, h_bucket_suffixes, nvbio::plain_view( d_bucket_suffixes ) );

                        Timer timer;
                        timer.start();

                        priv::alloc_storage( h_block_bwt, n_suffixes );
                        priv::alloc_storage( d_block_bwt, n_suffixes );

                        // load the BWT symbols
                        string_set_handler.bwt(
                            n_suffixes,
                            (const uint32*)NULL,
                            plain_view( h_block_bwt ),
                            plain_view( d_block_bwt ) );

                        timer.stop();
                        bwt_time += timer.seconds();

                        timer.start();

                        // invoke the output handler
                        output.process(
                            n_suffixes,
                            plain_view( h_block_bwt ),
                            plain_view( d_block_bwt ),
                            h_bucket_suffixes,
                            plain_view( d_bucket_suffixes ),
                            NULL );

                        timer.stop();
                        output_time += timer.seconds();
                    }
                    
                    suffix_count += subbucket_size;

                    suf_timer.stop();
                    sufsort_time += suf_timer.seconds();
                }
                else
                {
                    // grow the block of sub-buckets until we can
                    uint32 subbucket_size;
                    for (subbucket_size = 0; (subbucket_end < bucket_end) && (subbucket_size + h_buckets[subbucket_end] <= max_block_size); ++subbucket_end)
                        subbucket_size += h_buckets[subbucket_end];

                    NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"\r  sufsort buckets[%u:%u] (%.1f M suffixes/s)    ", subbucket_begin, subbucket_end, 1.0e-6f*float(global_suffix_offset + suffix_count)/sufsort_time) );
                    if (subbucket_size == 0)
                        continue;

                    // consume subbucket_size suffixes
                    const uint32 n_suffixes = subbucket_size;

                    try
                    {
                        // reserve enough space
                        priv::alloc_storage( d_indices, max_block_size );
                    }
                    catch (...)
                    {
                        log_error(stderr, "LargeBWTSkeleton: d_indices allocation failed!\n");
                        throw;
                    }

                    Timer suf_timer;
                    suf_timer.start();

                    // copy the host suffixes to the device
                    const uint2* h_bucket_suffixes = &h_suffixes[0] + suffix_count;

                    priv::alloc_storage( d_bucket_suffixes, n_suffixes );

                    // copy the suffix list to the device
                    thrust::copy(
                        h_bucket_suffixes,
                        h_bucket_suffixes + n_suffixes,
                        d_bucket_suffixes.begin() );

                    // initialize the set radices
                    string_set_handler.init( n_suffixes, h_bucket_suffixes, nvbio::plain_view( d_bucket_suffixes ) );

                    cuda::DiscardDelayList delay_list;

                    string_sorter.sort(
                        string_set_handler,
                        n_suffixes,
                        n_words,
                        thrust::make_counting_iterator<uint32>(0u),
                        d_indices.begin(),
                        uint32(-1),
                        delay_list,
                        SLICE_SIZE );

                    Timer timer;
                    timer.start();

                    priv::alloc_storage( h_block_bwt, n_suffixes );
                    priv::alloc_storage( d_block_bwt, n_suffixes );

                    // load the BWT symbols
                    string_set_handler.bwt(
                        n_suffixes,
                        plain_view( d_indices ),
                        plain_view( h_block_bwt ),
                        plain_view( d_block_bwt ) );

                    timer.stop();
                    bwt_time += timer.seconds();

                    timer.start();

                    // invoke the output handler
                    output.process(
                        n_suffixes,
                        plain_view( h_block_bwt ),
                        plain_view( d_block_bwt ),
                        h_bucket_suffixes,
                        plain_view( d_bucket_suffixes ),
                        plain_view( d_indices ) );

                    timer.stop();
                    output_time += timer.seconds();
                    
                    suffix_count += subbucket_size;

                    suf_timer.stop();
                    sufsort_time += suf_timer.seconds();
                }
            }
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"\r  sufsort : %.1fs (%.1f M suffixes/s)                     \n", sufsort_time, 1.0e-6f*float(global_suffix_offset + suffix_count)/sufsort_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    copy     : %.1fs\n", string_sorter.copy_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    extract  : %.1fs\n", string_sorter.extract_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    r-sort   : %.1fs\n", string_sorter.radixsort_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    compress : %.1fs\n", string_sorter.compress_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    compact  : %.1fs\n", string_sorter.compact_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    scatter  : %.1fs\n", string_sorter.scatter_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    bwt      : %.1fs\n", bwt_time) );
            NVBIO_CUDA_DEBUG_STATEMENT( log_verbose(stderr,"    output   : %.1fs\n", output_time) );

            global_suffix_offset += suffix_count;
        }
        return status;
    }
};

// Compute the bwt of a device-side string set
//
template <uint32 SYMBOL_SIZE, bool BIG_ENDIAN, typename storage_type, typename output_handler>
void bwt(
    const ConcatenatedStringSet<
        PackedStreamIterator< PackedStream<storage_type,uint8,SYMBOL_SIZE,BIG_ENDIAN,uint64> >,
        uint64*>                    string_set,
        output_handler&             output,
        BWTParams*                  params)
{
    typedef cuda::DeviceBWTConfig<16,SYMBOL_SIZE,BIG_ENDIAN,storage_type> config_type_16; // 16-bits bucketing
    typedef cuda::DeviceBWTConfig<20,SYMBOL_SIZE,BIG_ENDIAN,storage_type> config_type_20; // 20-bits bucketing
    typedef cuda::DeviceBWTConfig<24,SYMBOL_SIZE,BIG_ENDIAN,storage_type> config_type_24; // 24-bits bucketing

    cuda::LargeBWTStatus status;

    // try 16-bit bucketing
    if (status = cuda::LargeBWTSkeleton<config_type_16,SYMBOL_SIZE,BIG_ENDIAN,storage_type>::enact(
        string_set,
        output,
        params ))
        return;

    // try 20-bit bucketing
    if (status = cuda::LargeBWTSkeleton<config_type_20,SYMBOL_SIZE,BIG_ENDIAN,storage_type>::enact(
        string_set,
        output,
        params ))
        return;

    // try 24-bit bucketing
    if (status = cuda::LargeBWTSkeleton<config_type_24,SYMBOL_SIZE,BIG_ENDIAN,storage_type>::enact(
        string_set,
        output,
        params ))
        return;

    if (status.code == LargeBWTStatus::LargeBucket)
        throw nvbio::runtime_error("subbucket %u contains %u strings: buffer overflow!\n  please try increasing the device memory limit to at least %u MB\n", status.bucket_index, status.bucket_size, util::divide_ri( status.bucket_size, 1024u*1024u )*32u);
}

} // namespace cuda

// Compute the bwt of a host-side string set
//
template <uint32 SYMBOL_SIZE, bool BIG_ENDIAN, typename storage_type, typename output_handler>
void large_bwt(
    const ConcatenatedStringSet<
        PackedStreamIterator< PackedStream<storage_type,uint8,SYMBOL_SIZE,BIG_ENDIAN,uint64> >,
        uint64*>                    string_set,
        output_handler&             output,
        BWTParams*                  params)
{
    typedef cuda::HostBWTConfig<16,SYMBOL_SIZE,BIG_ENDIAN,storage_type> config_type_16; // 16-bits bucketing
    typedef cuda::HostBWTConfig<20,SYMBOL_SIZE,BIG_ENDIAN,storage_type> config_type_20; // 20-bits bucketing
    typedef cuda::HostBWTConfig<24,SYMBOL_SIZE,BIG_ENDIAN,storage_type> config_type_24; // 24-bits bucketing

    cuda::LargeBWTStatus status;

    // try 16-bit bucketing
    if (status = cuda::LargeBWTSkeleton<config_type_16,SYMBOL_SIZE,BIG_ENDIAN,storage_type>::enact(
        string_set,
        output,
        params ))
        return;

    // try 20-bit bucketing
    if (status = cuda::LargeBWTSkeleton<config_type_20,SYMBOL_SIZE,BIG_ENDIAN,storage_type>::enact(
        string_set,
        output,
        params ))
        return;

    // try 24-bit bucketing
    if (status = cuda::LargeBWTSkeleton<config_type_24,SYMBOL_SIZE,BIG_ENDIAN,storage_type>::enact(
        string_set,
        output,
        params ))
        return;

    if (status.code == cuda::LargeBWTStatus::LargeBucket)
        throw nvbio::runtime_error("subbucket %u contains %u strings: buffer overflow!\n  please try increasing the device memory limit to at least %u MB\n", status.bucket_index, status.bucket_size, util::divide_ri( status.bucket_size, 1024u*1024u )*32u);
}

} // namespace nvbio