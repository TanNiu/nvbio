/*
 * nvbio
 * Copyright (C) 2012-2014, NVIDIA Corporation
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

#include <nvbio/basic/types.h>
#include <nvbio/basic/numbers.h>
#include <nvbio/basic/popcount.h>
#include <nvbio/basic/packedstream.h>
#include <nvbio/basic/iterator.h>
#include <vector_types.h>
#include <vector_functions.h>

namespace nvbio {

///@addtogroup FMIndex
///@{

///\defgroup RankDictionaryModule Rank Dictionaries
///
/// A rank dictionary is a data-structure which, given a text and a sparse occurrence table, can answer,
/// in O(1) time, queries of the kind "how many times does character c occurr in the substring text[0:i] ?"

///@addtogroup RankDictionaryModule
///@{

///
/// A rank dictionary data-structure which, given a text and a sparse occurrence table, can answer,
/// in O(1) time, queries of the kind "how many times does character c occurr in the substring text[0:i] ?"
///
/// \tparam SYMBOL_SIZE_T       the size of the alphabet, in bits
/// \tparam K                   the sparsity of the occurrence table
/// \tparam TextString          the text string type
/// \tparam OccIterator         the occurrence table iterator type
/// \tparam CountTable          an auxiliary lookup table used to count the number of occurrences of all
///                             characters in a given byte
///
template <uint32 SYMBOL_SIZE_T, uint32 K, typename TextString, typename OccIterator, typename CountTable>
struct rank_dictionary
{
    static const uint32     BLOCK_INTERVAL  = K;
    static const uint32     SYMBOL_SIZE     = SYMBOL_SIZE_T;

    typedef TextString      text_type;
    typedef OccIterator     occ_iterator;
    typedef CountTable      count_table_type;

    // the indexing type of this container is determined by the value_type of the occurrence table
    typedef typename vector_traits<
        typename std::iterator_traits<occ_iterator>::value_type>::value_type index_type;

    typedef typename vector_type<index_type,2>::type range_type;
    typedef typename vector_type<index_type,2>::type vec2_type;
    typedef typename vector_type<index_type,4>::type vec4_type;

    /// default constructor
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    rank_dictionary() {}

    /// constructor
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    rank_dictionary(
        const TextString   _text,
        const OccIterator  _occ,
        const CountTable   _count_table) :
        text( _text ),
        occ( _occ ),
        count_table( _count_table ) {}

    TextString    text;                   ///< the dictionary's text
    OccIterator   occ;                    ///< the dictionary's occurrence table
    CountTable    count_table;            ///< a helper lookup table used to efficiently count the number
                                          ///  of occurrences of all the characters in a given byte
};

///
/// Build the occurrence table for a given string, packing a set of counters
/// every K elements.
/// The table must contain ((n+K-1)/K)*4 entries.
///
/// Optionally save the table of the global counters as well.
///
/// \param begin    symbol sequence begin
/// \param end      symbol sequence end
/// \param occ      output occurrence map
/// \param cnt      optional table of the global counters
///
template <uint32 K, typename SymbolIterator, typename IndexType>
void build_occurrence_table(
    SymbolIterator begin,
    SymbolIterator end,
    IndexType*     occ,
    IndexType*     cnt = NULL);

/// \relates rank_dictionary
/// fetch the text character at position i in the rank dictionary
///
template <uint32 SYMBOL_SIZE_T, uint32 K, typename TextString, typename OccIterator, typename CountTable>
NVBIO_FORCEINLINE NVBIO_HOST_DEVICE uint8 text(const rank_dictionary<SYMBOL_SIZE_T,K,TextString,OccIterator,CountTable>& dict, const uint32 i);

/// \relates rank_dictionary
/// fetch the text character at position i in the rank dictionary
///
template <uint32 SYMBOL_SIZE_T, uint32 K, typename TextString, typename OccIterator, typename CountTable>
NVBIO_FORCEINLINE NVBIO_HOST_DEVICE uint8 text(const rank_dictionary<SYMBOL_SIZE_T,K,TextString,OccIterator,CountTable>& dict, const uint64 i);

/// \relates rank_dictionary
/// fetch the number of occurrences of character c in the substring [0,i]
///
/// \param dict         the rank dictionary
/// \param i            the end of the query range [0,i]
/// \param c            the query character
///
template <uint32 SYMBOL_SIZE_T, uint32 K, typename TextString, typename OccIterator, typename CountTable, typename IndexType>
NVBIO_FORCEINLINE NVBIO_HOST_DEVICE IndexType rank(
    const rank_dictionary<SYMBOL_SIZE_T,K,TextString,OccIterator,CountTable>& dict, const IndexType i, const uint32 c);

/// \relates rank_dictionary
/// fetch the number of occurrences of character c in the substrings [0,l] and [0,r]
///
/// \param dict         the rank dictionary
/// \param range        the ends of the query ranges [0,range.x] and [0,range.y]
/// \param c            the query character
///
template <uint32 SYMBOL_SIZE_T, uint32 K, typename TextString, typename OccIterator, typename CountTable, typename IndexType>
NVBIO_FORCEINLINE NVBIO_HOST_DEVICE typename vector_type<IndexType,2>::type rank(
    const rank_dictionary<SYMBOL_SIZE_T,K,TextString,OccIterator,CountTable>& dict, const typename vector_type<IndexType,2>::type range, const uint32 c);

/// \relates rank_dictionary
/// fetch the number of occurrences of all characters c in the substring [0,i]
///
/// \param dict         the rank dictionary
/// \param i            the end of the query range [0,i]
///
template <uint32 K, typename TextString, typename OccIterator, typename CountTable, typename IndexType>
NVBIO_FORCEINLINE NVBIO_HOST_DEVICE typename vector_type<IndexType,4>::type rank4(
    const rank_dictionary<2,K,TextString,OccIterator,CountTable>& dict, const IndexType i);

/// \relates rank_dictionary
/// fetch the number of occurrences of all characters in the substrings [0,l] and [0,r]
///
/// \param dict         the rank dictionary
/// \param range        the ends of the query ranges [0,range.x] and [0,range.y]
/// \param outl         the output count of all characters in the first range
/// \param outl         the output count of all characters in the second range
///
template <uint32 K, typename TextString, typename OccIterator, typename CountTable>
NVBIO_FORCEINLINE NVBIO_HOST_DEVICE void rank4(
    const rank_dictionary<2,K,TextString,OccIterator,CountTable>& dict, const uint2 range, uint4* outl, uint4* outh);

/// \relates rank_dictionary
/// fetch the number of occurrences of all characters in the substrings [0,l] and [0,r]
///
/// \param dict         the rank dictionary
/// \param range        the ends of the query ranges [0,range.x] and [0,range.y]
/// \param outl         the output count of all characters in the first range
/// \param outl         the output count of all characters in the second range
///
template <uint32 K, typename TextString, typename OccIterator, typename CountTable>
NVBIO_FORCEINLINE NVBIO_HOST_DEVICE void rank4(
    const rank_dictionary<2,K,TextString,OccIterator,CountTable>& dict, const uint64_2 range, uint64_4* outl, uint64_4* outh);

///@} RankDictionaryModule
///@} FMIndex

} // namespace nvbio

#include <nvbio/fmindex/rank_dictionary_inl.h>
