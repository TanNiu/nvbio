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
#include <nvbio/strings/string_set.h>
#include <nvbio/basic/thrust_view.h>
#include <thrust/host_vector.h>
#include <thrust/device_vector.h>

namespace nvbio {

///@addtogroup Sufsort
///@{

/// base virtual interface used by all string-set BWT handlers
///
struct BaseBWTHandler
{
    /// virtual destructor
    ///
    virtual ~BaseBWTHandler() {}

    /// process a batch of BWT symbols
    ///
    virtual void process(
        const uint32  n_suffixes,
        const uint8*  h_bwt,
        const uint8*  d_bwt,
        const uint2*  h_suffixes,
        const uint2*  d_suffixes,
        const uint32* d_indices) {}
};

/// A class to output the BWT to a (potentially packed) device string
///
template <typename OutputIterator>
struct DeviceBWTHandler : public BaseBWTHandler
{
    /// constructor
    ///
    DeviceBWTHandler(OutputIterator _output) : output(_output), offset(0u) {}

    /// process a batch of BWT symbols
    ///
    void process(
        const uint32  n_suffixes,
        const uint8*  h_bwt,
        const uint8*  d_bwt,
        const uint2*  h_suffixes,
        const uint2*  d_suffixes,
        const uint32* d_indices)
    {
        priv::device_copy(
            n_suffixes,
            d_bwt,
            output,
            offset );

        offset += n_suffixes;
    }

    OutputIterator               output;
    uint64                       offset;
};

/// A class to output the BWT to a host string
///
template <typename OutputIterator>
struct HostBWTHandler : public BaseBWTHandler
{
    HostBWTHandler(OutputIterator _output) : output(_output) {}

    /// process a batch of BWT symbols
    ///
    void process(
        const uint32  n_suffixes,
        const uint8*  h_bwt,
        const uint8*  d_bwt,
        const uint2*  h_suffixes,
        const uint2*  d_suffixes,
        const uint32* d_indices)
    {
        for (uint32 i = 0; i < n_suffixes; ++i)
            output[i] = h_bwt[i];

        output += n_suffixes;
    }

    OutputIterator output;
};

/// A class to output the BWT to a packed host string
///
template <uint32 SYMBOL_SIZE, bool BIG_ENDIAN, typename word_type>
struct HostBWTHandler< PackedStream<word_type*,uint8,SYMBOL_SIZE,BIG_ENDIAN,uint64> > : public BaseBWTHandler
{
    typedef PackedStream<word_type*,uint8,SYMBOL_SIZE,BIG_ENDIAN,uint64> OutputIterator;

    static const uint32 WORD_SIZE = uint32( 8u * sizeof(word_type) );
    static const uint32 SYMBOLS_PER_WORD = WORD_SIZE / SYMBOL_SIZE;


    /// constructor
    ///
    HostBWTHandler(OutputIterator _output) : output(_output), offset(0) {}

    /// process a batch of BWT symbols
    ///
    void process(
        const uint32  n_suffixes,
        const uint8*  h_bwt,
        const uint8*  d_bwt,
        const uint2*  h_suffixes,
        const uint2*  d_suffixes,
        const uint32* d_indices)
    {
        const uint32 word_offset = offset & (SYMBOLS_PER_WORD-1);
              uint32 word_rem    = 0;
              uint32 word_idx    = offset / SYMBOLS_PER_WORD;

        word_type* words = output.stream();

        if (word_offset)
        {
            // compute how many symbols we still need to encode to fill the current word
            word_rem = SYMBOLS_PER_WORD - word_offset;

            // fetch the word in question
            word_type word = words[ word_idx ];

            for (uint32 i = 0; i < word_rem; ++i)
            {
                const uint32       bit_idx = (word_offset + i) * SYMBOL_SIZE;
                const uint32 symbol_offset = BIG_ENDIAN ? (WORD_SIZE - SYMBOL_SIZE - bit_idx) : bit_idx;
                const word_type     symbol = word_type(h_bwt[i]) << symbol_offset;

                // set bits
                word |= symbol;
            }

            // write out the word
            words[ word_idx ] = word;

            // advance word_idx
            ++word_idx;
        }

        for (uint32 i = word_rem; i < n_suffixes; i += SYMBOLS_PER_WORD)
        {
            // encode a word's worth of characters
            word_type word = 0u;

            const uint32 n_symbols = nvbio::min( SYMBOLS_PER_WORD, n_suffixes - i );

            for (uint32 j = 0; j < n_symbols; ++j)
            {
                const uint32       bit_idx = j * SYMBOL_SIZE;
                const uint32 symbol_offset = BIG_ENDIAN ? (WORD_SIZE - SYMBOL_SIZE - bit_idx) : bit_idx;
                const word_type     symbol = word_type(h_bwt[i + j]) << symbol_offset;

                // set bits
                word |= symbol;
            }

            // write out the word and advance word_idx
            words[ ++word_idx ] = word;
        }

        // advance the offset
        offset += n_suffixes;
    }

    OutputIterator output;
    uint64         offset;
};

/// A class to output the BWT to a packed host string
///
struct DiscardBWTHandler : public BaseBWTHandler
{
    /// process a batch of BWT symbols
    ///
    void process(
        const uint32  n_suffixes,
        const uint8*  h_bwt,
        const uint8*  d_bwt,
        const uint2*  h_suffixes,
        const uint2*  d_suffixes,
        const uint32* d_indices) {}
};

/// a utility StringSuffixHandler to compute the BWT of the sorted suffixes
///
template <typename string_type, typename output_iterator>
struct StringBWTHandler
{
    typedef typename string_type::index_type index_type;

    static const uint32 NULL_PRIMARY = uint32(-1); // TODO: switch to index_type

    // constructor
    //
    StringBWTHandler(
        const index_type    _string_len,
        const string_type   _string,
        output_iterator     _output) :
        string_len  ( _string_len ),
        string      ( _string ),
        primary     ( NULL_PRIMARY ),
        n_output    ( 0 ),
        output      ( _output )
    {
        // encode the first BWT symbol explicitly
        priv::device_copy( 1u, string + string_len-1, output, index_type(0u) );
    }

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

        // advance the output counter
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

    // remove the dollar symbol
    //
    void remove_dollar()
    {
        // shift back all symbols following the primary
        const uint32 max_block_size = 32*1024*1024;

        priv::alloc_storage( d_block_bwt, max_block_size );

        for (index_type block_begin = primary; block_begin < string_len; block_begin += max_block_size)
        {
            const index_type block_end = nvbio::min( block_begin + max_block_size, string_len );

            // copy all symbols to a temporary buffer
            priv::device_copy(
                block_end - block_begin,
                output + block_begin + 1u,
                d_block_bwt.begin(),
                uint32(0) );

            // and copy the shifted block to the output
            priv::device_copy(
                block_end - block_begin,
                d_block_bwt.begin(),
                output,
                block_begin );
        }
    }

    const index_type                string_len;
    const string_type               string;
    uint32                          primary;
    uint32                          n_output;
    output_iterator                 output;
    thrust::device_vector<uint8>    d_block_bwt;
};

/// a utility StringSuffixHandler to retain a Sampled Suffix Array
///
template <typename output_iterator>
struct StringSSAHandler
{
    // constructor
    //
    StringSSAHandler(
        const uint32        _string_len,
        const uint32        _mod,
        output_iterator     _output) :
        string_len  ( _string_len ),
        mod         ( _mod ),
        n_output    ( 1 ),
        output      ( _output )
    {
        // encode the implicit empty suffix directly
        output[0] = uint32(-1);
    }

    // process the next batch of suffixes
    //
    void process_batch(
        const uint32  n_suffixes,
        const uint32* d_suffixes)
    {
        priv::alloc_storage( h_suffixes, n_suffixes );

        thrust::copy(
            thrust::device_ptr<const uint32>(d_suffixes),
            thrust::device_ptr<const uint32>(d_suffixes) + n_suffixes,
            h_suffixes.begin() );

        // copy_if
        #pragma omp parallel for
        for (int i = 0; i < int( n_suffixes ); ++i)
        {
            const uint32 slot = i + n_output;

            if ((slot & (mod-1)) == 0)
                output[slot / mod] = h_suffixes[i];
        }

        // advance the output counter
        n_output += n_suffixes;
    }

    // process a sparse set of suffixes
    //
    void process_scattered(
        const uint32  n_suffixes,
        const uint32* d_suffixes,
        const uint32* d_slots)
    {
        priv::alloc_storage( h_slots,    n_suffixes );
        priv::alloc_storage( h_suffixes, n_suffixes );

        thrust::copy(
            thrust::device_ptr<const uint32>(d_slots),
            thrust::device_ptr<const uint32>(d_slots) + n_suffixes,
            h_slots.begin() );

        thrust::copy(
            thrust::device_ptr<const uint32>(d_suffixes),
            thrust::device_ptr<const uint32>(d_suffixes) + n_suffixes,
            h_suffixes.begin() );

        // scatter_if
        #pragma omp parallel for
        for (int i = 0; i < int( n_suffixes ); ++i)
        {
            const uint32 slot = h_slots[i] + 1u;    // +1 for the implicit empty suffix

            if ((slot & (mod-1)) == 0)
                output[slot / mod] = h_suffixes[i];
        }
    }

    const uint32                    string_len;
    const uint32                    mod;
    uint32                          n_output;
    output_iterator                 output;
    thrust::host_vector<uint32>     h_slots;
    thrust::host_vector<uint32>     h_suffixes;
};

/// a utility StringSuffixHandler to retain the BWT and a Sampled Suffix Array
///
template <typename string_type, typename output_bwt_iterator, typename output_ssa_iterator>
struct StringBWTSSAHandler
{
    StringBWTSSAHandler(
        const uint32        _string_len,
        const string_type   _string,
        const uint32        _mod,
        output_bwt_iterator _bwt,
        output_ssa_iterator _ssa) :
        bwt_handler( _string_len, _string, _bwt ),
        ssa_handler( _string_len, _mod, _ssa ) {}

    // process the next batch of suffixes
    //
    void process_batch(
        const uint32  n_suffixes,
        const uint32* d_suffixes)
    {
        bwt_handler.process_batch( n_suffixes, d_suffixes );
        ssa_handler.process_batch( n_suffixes, d_suffixes );
    }

    // process a sparse set of suffixes
    //
    void process_scattered(
        const uint32  n_suffixes,
        const uint32* d_suffixes,
        const uint32* d_slots)
    {
        bwt_handler.process_scattered( n_suffixes, d_suffixes, d_slots );
        ssa_handler.process_scattered( n_suffixes, d_suffixes, d_slots );
    }

    // return the primary
    //
    uint32 primary() const { return bwt_handler.primary; }

    // remove the dollar symbol
    //
    void remove_dollar()
    {
        bwt_handler.remove_dollar();
    }

    StringBWTHandler<string_type,output_bwt_iterator> bwt_handler;
    StringSSAHandler<output_ssa_iterator>             ssa_handler;
};

///@}

} // namespace nvbio
