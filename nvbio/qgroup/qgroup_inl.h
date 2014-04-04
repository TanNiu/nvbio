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

// setup an internal namespace to avoid polluting the global environment
namespace qgroup {

// a functor to set the q-group's I vector
//
template <uint32 SYMBOL_SIZE, typename string_type>
struct qgroup_setup_I
{
    typedef QGroupIndexDevice::bitstream_type                    bitstream_type;
    typedef string_qgram_functor<SYMBOL_SIZE,string_type>   qgram_functor_type;

    // constructor
    //
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    qgroup_setup_I(
        const uint32        _string_len,
        const string_type   _string,
        bitstream_type      _I) :
        string_len  ( _string_len ),
        string      ( _string ),
        I           ( _I )
        {}

    // operator functor
    //
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    void operator() (const uint32 p) const
    {
        const qgram_functor_type qgram( string_len, string );

        // set the bit corresponding to the i-th qgram
        I[ qgram(p) ] = 1u;
    }

    const uint32            string_len;
    const string_type       string;
    mutable bitstream_type  I;
};

// a functor to set the q-group's SS vector
//
template <uint32 SYMBOL_SIZE, typename string_type>
struct qgroup_setup_SS
{
    typedef QGroupIndexDevice::bitstream_type               bitstream_type;
    typedef string_qgram_functor<SYMBOL_SIZE,string_type>   qgram_functor_type;

    static const uint32 WORD_SIZE = 32;

    // constructor
    //
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    qgroup_setup_SS(
        const uint32        _string_len,
        const string_type   _string,
        const uint32*       _I,
        const uint32*       _S,
              uint32*       _SS) :
        string_len  ( _string_len ),
        string      ( _string ),
        I           ( _I ),
        S           ( _S ),
        SS          ( _SS )
        {}

    // operator functor
    //
    NVBIO_FORCEINLINE NVBIO_DEVICE
    void operator() (const uint32 p) const
    {
        const qgram_functor_type qgram( string_len, string );

        // compute the qgram g
        const uint64 g = qgram(p);

        // compute (i,j) from g
        const uint32 i = uint32( g / WORD_SIZE );
        const uint32 j = uint32( g % WORD_SIZE );

        // compute j' such that bit j is the j'-th set bit in I[i]
        const uint32 j_prime = popc( I[i] & ((2u << j) - 1u) );

        // atomically increase the appropriate counter in SS
        atomicAdd( SS + S[i] + j_prime, 1u );
    }

    const uint32        string_len;
    const string_type   string;
    const uint32*       I;
    const uint32*       S;
          uint32*       SS;
};

// a functor to set the q-group's SS vector
//
template <uint32 SYMBOL_SIZE, typename string_type>
struct qgroup_setup_P
{
    typedef QGroupIndexDevice::bitstream_type               bitstream_type;
    typedef string_qgram_functor<SYMBOL_SIZE,string_type>   qgram_functor_type;

    static const uint32 WORD_SIZE = 32;

    // constructor
    //
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    qgroup_setup_P(
        const uint32        _string_len,
        const string_type   _string,
        const uint32*       _I,
        const uint32*       _S,
              uint32*       _SS,
              uint32*       _P) :
        string_len  ( _string_len ),
        string      ( _string ),
        I           ( _I ),
        S           ( _S ),
        SS          ( _SS ),
        P           ( _P )
        {}

    // operator functor
    //
    NVBIO_FORCEINLINE NVBIO_DEVICE
    void operator() (const uint32 p) const
    {
        const qgram_functor_type qgram( string_len, string );

        // compute the qgram g
        const uint64 g = qgram(p);

        // compute (i,j) from g
        const uint32 i = uint32( g / WORD_SIZE );
        const uint32 j = uint32( g % WORD_SIZE );

        // compute j' such that bit j is the j'-th set bit in I[i]
        const uint32 j_prime = popc( I[i] & ((2u << j) - 1u) );

        // atomically increase the appropriate counter in SS to get the next free slot
        const uint32 slot = atomicAdd( SS + S[i] + j_prime, 1u );

        // and fill the corresponding slot of P
        P[ slot ] = p;
    }

    const uint32        string_len;
    const string_type   string;
    const uint32*       I;
    const uint32*       S;
          uint32*       SS;
          uint32*       P;
};

} // namespace qgroup

// build a q-group index from a given string
//
// \param q                the q parameter
// \param string_len       the size of the string
// \param string           the string iterator
//
template <uint32 SYMBOL_SIZE, typename string_type>
void QGroupIndexDevice::build(
    const uint32        q,
    const uint32        string_len,
    const string_type   string)
{
    typedef qgroup::qgroup_setup_I<SYMBOL_SIZE,string_type>     setup_I_type;
    typedef qgroup::qgroup_setup_SS<SYMBOL_SIZE,string_type>    setup_SS_type;
    typedef qgroup::qgroup_setup_P<SYMBOL_SIZE,string_type>     setup_P_type;

    thrust::device_vector<uint8> d_temp_storage;

    Q = q;

    const uint32 ALPHABET_SIZE = 1u << SYMBOL_SIZE;

    uint64 n_qgrams = 1;
    for (uint32 i = 0; i < q; ++i)
        n_qgrams *= ALPHABET_SIZE;

    const uint32 n_qblocks = uint32( n_qgrams / WORD_SIZE );

    I.resize( n_qblocks+1 );
    S.resize( n_qblocks+1 );

    //
    // setup I
    //

    bitstream_type I_bits( nvbio::plain_view( I ) );

    // fill I with zeros
    thrust::fill(
        I.begin(),
        I.begin() + n_qblocks + 1u,
        uint32(0) );

    const setup_I_type setup_I( string_len, string, I_bits );

    // set the bits in I corresponding to the used qgram slots
    thrust::for_each(
        thrust::make_counting_iterator<uint32>(0),
        thrust::make_counting_iterator<uint32>(0) + string_len,
        setup_I );

    //
    // setup S
    //

    // compute the exclusive prefix sum of the popcount of the words in I
    cuda::exclusive_scan(
        n_qblocks + 1u,
        thrust::make_transform_iterator( nvbio::plain_view(I), popc_functor<uint32>() ),
        S.begin(),
        thrust::plus<uint32>(),
        uint32(0),
        d_temp_storage );

    // fetch the number of used qgrams
    n_unique_qgrams = S[n_qblocks];

    //
    // setup SS
    //
    SS.resize( n_unique_qgrams + 1u );

    thrust::fill(
        SS.begin(),
        SS.begin() + n_unique_qgrams + 1u,
        uint32(0) );

    const setup_SS_type setup_SS(
        string_len, string,
        nvbio::plain_view( I ),
        nvbio::plain_view( S ),
        nvbio::plain_view( SS ) );

    thrust::for_each(
        thrust::make_counting_iterator<uint32>(0),
        thrust::make_counting_iterator<uint32>(0) + string_len,
        setup_SS );

    // compute the exclusive prefix sum of SS
    cuda::exclusive_scan(
        n_unique_qgrams + 1u,
        SS.begin(),
        SS.begin(),
        thrust::plus<uint32>(),
        uint32(0),
        d_temp_storage );

    //
    // setup P
    //
    P.resize( string_len );

    // copy SS into a temporary vector for the purpose of slot allocation
    thrust::device_vector<uint32> slots( SS );

    const setup_P_type setup_P(
        string_len, string,
        nvbio::plain_view( I ),
        nvbio::plain_view( S ),
        nvbio::plain_view( slots ),
        nvbio::plain_view( P ) );

    thrust::for_each(
        thrust::make_counting_iterator<uint32>(0),
        thrust::make_counting_iterator<uint32>(0) + string_len,
        setup_P );
}

} // namespace nvbio
