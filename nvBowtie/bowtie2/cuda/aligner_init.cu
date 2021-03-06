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

#include <nvBowtie/bowtie2/cuda/aligner.h>

namespace nvbio {
namespace bowtie2 {
namespace cuda {

template <typename T>
T* resize(bool do_alloc, thrust::device_vector<T>& vec, const uint32 size, uint64& bytes)
{
    bytes += size * sizeof(T);
    if (do_alloc)
    {
        vec.resize( size );
        return thrust::raw_pointer_cast(&vec.front());
    }
    return NULL;
}

template <typename T>
T* resize(bool do_alloc, thrust::host_vector<T>& vec, const uint32 size, uint64& bytes)
{
    bytes += size * sizeof(T);
    if (do_alloc)
    {
        vec.resize( size );
        return thrust::raw_pointer_cast(&vec.front());
    }
    return NULL;
}

template <typename T>
T* resize(bool do_alloc, std::vector<T>& vec, const uint32 size, uint64& bytes)
{
    bytes += size * sizeof(T);
    if (do_alloc)
    {
        vec.resize( size );
        return &vec[0];
    }
    return NULL;
}

std::pair<uint64,uint64> Aligner::init_alloc(const uint32 BATCH_SIZE, const Params& params, const EndType type, bool do_alloc)
{
    //const uint32 band_len = (type == kPairedEnds) ? MAXIMUM_BAND_LENGTH : band_length( params.max_dist );

    uint64 d_allocated_bytes = 0;
    uint64 h_allocated_bytes = 0;

    // alloc the seeding queues
    d_allocated_bytes += seed_queues.resize_arena( BATCH_SIZE, do_alloc );

    // alloc the hit deques
    d_allocated_bytes += hit_deques.resize( BATCH_SIZE, params.max_hits, do_alloc );
    if (params.randomized)
        rseeds_dptr = resize( do_alloc, rseeds_dvec, BATCH_SIZE, d_allocated_bytes );

    // alloc the scoring queues
    d_allocated_bytes += scoring_queues.resize( BATCH_SIZE, BATCH_SIZE, do_alloc ); // TODO: pass read-end 'type' field

                     resize( do_alloc, sorting_queue_dvec,    BATCH_SIZE*2, d_allocated_bytes );
    idx_queue_dptr = resize( do_alloc, idx_queue_dvec,        BATCH_SIZE*2, d_allocated_bytes );

    nvbio::cuda::check_error("allocating queues");

    if (params.mode != AllMapping)
    {
        trys_dptr      = resize( do_alloc, trys_dvec,       BATCH_SIZE,     d_allocated_bytes );
        best_data_dptr = resize( do_alloc, best_data_dvec,  BATCH_SIZE,     d_allocated_bytes );

        if (type == kPairedEnds)
            best_data_dptr_o = resize( do_alloc, best_data_dvec_o,  BATCH_SIZE, d_allocated_bytes );
    }
    else
    {
        // in all-mapping mode we store the temporary output in a double-buffered ring-buffer
        buffer_alignments_dptr = resize( do_alloc, buffer_alignments_dvec,  BATCH_SIZE*2,   d_allocated_bytes );
        buffer_read_info_dptr  = resize( do_alloc, buffer_read_info_dvec,   BATCH_SIZE*2,   d_allocated_bytes );

        output_alignments_dptr = resize( do_alloc, output_alignments_dvec,  BATCH_SIZE,     d_allocated_bytes );
        output_read_info_dptr  = resize( do_alloc, output_read_info_dvec,   BATCH_SIZE,     d_allocated_bytes );
    }
    nvbio::cuda::check_error("allocating output buffers");

    hits_stats_dptr = resize( do_alloc, hits_stats_dvec, 128, d_allocated_bytes );
                      resize( do_alloc, hits_stats_hvec, 128, d_allocated_bytes );

    if (params.mode == AllMapping)
    {
        hits_count_scan_dptr = resize( do_alloc, hits_count_scan_dvec,     BATCH_SIZE+1,                       d_allocated_bytes );
        hits_range_scan_dptr = resize( do_alloc, hits_range_scan_dvec,     params.max_hits * BATCH_SIZE+1,     d_allocated_bytes );
    }

    //const uint32 n_cigar_entries = BATCH_SIZE*(MAXIMUM_BAND_LEN_MULT*band_len+1);
    //const uint32 n_mds_entries   = BATCH_SIZE*MAX_READ_LEN;
    const uint32 n_cigar_entries = (128 * BATCH_SIZE)/sizeof(io::Cigar);    // 256MB
    const uint32 n_mds_entries   = (256 * BATCH_SIZE)/sizeof(uint8);        // 256MB
    if (do_alloc)
    {
        log_verbose(stderr, "    allocating %u MB of string storage\n      CIGARs : %u MB\n      MDs    : %u MB\n",
                    uint32(n_cigar_entries * sizeof(io::Cigar) + n_mds_entries)/(1024*1024),
                    uint32(n_cigar_entries * sizeof(io::Cigar))/(1024*1024),
                    n_mds_entries/(1024*1024) );
    }

    // allocate CIGARs & MDs
    d_allocated_bytes += cigar.resize( BATCH_SIZE, n_cigar_entries, do_alloc );
    d_allocated_bytes += mds.resize(   BATCH_SIZE, n_mds_entries,   do_alloc );

    // allocate CIGAR coords
    cigar_coords_dptr = resize( do_alloc, cigar_coords_dvec, BATCH_SIZE, d_allocated_bytes );
    nvbio::cuda::check_error("allocating CIGARs");

    // allocate DP storage
    uint32 dp_storage = 0;

    if (do_alloc)
    {
        if (type == kPairedEnds)
        {
            // allocate the device queue
            opposite_queue_dptr = resize( do_alloc, opposite_queue_dvec, BATCH_SIZE, d_allocated_bytes );
        }

        //
        // allocate two thirds of available device memory for scoring / traceback
        //

        size_t free, total;
        cudaMemGetInfo(&free, &total);
        const uint32 free_words    = uint32( free / 4u );
        const uint32 min_free_words = NVBIO_CUDA_DEBUG_SELECT( (500*1024*1024)/4, (400*1024*1024)/4 ); // we want to leave 320MB (500MB) free,
                                                                                                       // neeeded for kernels using lmem
        uint32 target_words  = (free_words * 2u) / 3u;
               target_words  = nvbio::min( target_words, free_words - min_free_words );

        const uint32 buffer_words = target_words;
        log_verbose(stderr, "    allocating %u MB of DP storage\n",
            (buffer_words*4)/(1024*1024) );

        dp_storage = buffer_words * sizeof(uint32);
    }

    // allocate a large temporary buffer to for scoring and traceback
    dp_buffer_dptr = resize( do_alloc, dp_buffer_dvec, dp_storage, d_allocated_bytes );

    nvbio::cuda::check_error("allocating alignment buffers");

    return std::make_pair( h_allocated_bytes, d_allocated_bytes );
}

bool Aligner::init(const uint32 batch_size, const Params& params, const EndType type)
{
    BATCH_SIZE = batch_size;

    output_file = NULL;

    // initialize the batch number
    batch_number = 0;

    try {
        std::pair<uint64,uint64> mem_stats = init_alloc( batch_size, params, type, false );

        log_stats(stderr, "  allocating alignment buffers... started\n    estimated: HOST %lu MB, DEVICE %lu MB)\n",
            mem_stats.first / (1024*1024),
            mem_stats.second / (1024*1024) );

        mem_stats = init_alloc( batch_size, params, type, true );

        log_stats(stderr, "  allocating alignment buffers... done\n    allocated: HOST %lu MB, DEVICE %lu MB)\n",
            mem_stats.first / (1024*1024),
            mem_stats.second / (1024*1024) );
    }
    catch (...) {
        log_error(stderr, "  allocating alignment buffers failed!\n");
        return false;
    }
    return true;
}

// Compute the total number of matches found
__global__ 
void hits_stats_kernel(
    const uint32 batch_size,
    const SeedHit* hit_data,
    const uint32*  hit_counts,
          uint64*  hit_stats)
{
    const uint32 read_id = threadIdx.x + BLOCKDIM*blockIdx.x;
    if (read_id >= batch_size) return;

    const uint32 hit_ranges = hit_counts[ read_id ];

    strided_iterator<const SeedHit*> hits( hit_data+read_id, batch_size );

    typedef vector_view< strided_iterator<const SeedHit*> > Storage;
    typedef priority_deque< SeedHit, Storage, hit_compare > HitQueue;
    Storage qStore( hit_ranges, hits );
    HitQueue hitheap( qStore, HitQueue::CONSTRUCTED );

    __shared__ uint32 shared_max_range;
    __shared__ uint32 shared_max_hits;
    __shared__ uint32 shared_top_max_hits;

    shared_max_range   = 0;
    shared_max_hits       = 0;
    shared_top_max_hits   = 0;

    __syncthreads();

    uint32 hits_cnt  = 0;
    uint32 max_range = 0;
    for (uint32 i = 0; i < hit_ranges; ++i)
    {
        hits_cnt += hits[i].get_range_size();
        max_range = nvbio::max( max_range, hits[i].get_range_size() );
    }

    const SeedHit top     = hit_ranges ? hitheap.top()                         : SeedHit();
    const uint32  top_cnt = hit_ranges ? top.get_range().y - top.get_range().x : 0u;

    // update the number of ranges and number of total hits
    atomicAdd( hit_stats + HIT_STATS_RANGES, uint64(hit_ranges) );
    atomicAdd( hit_stats + HIT_STATS_TOTAL, uint64(hits_cnt) );
    atomicAdd( hit_stats + HIT_STATS_TOP,   uint64(top_cnt) );

    // bin the number of hits, and update the bin counter
    const uint32 log_hits = hits_cnt == 0 ? 0u : nvbio::log2( hits_cnt )+1u;
    atomicAdd( hit_stats + HIT_STATS_BINS + log_hits, uint64(1u) );

    // bin the number of top hits, and update the bin counter
    const uint32 log_top_hits = top_cnt == 0 ? 0u : nvbio::log2( top_cnt )+1u;
    atomicAdd( hit_stats + HIT_STATS_TOP_BINS + log_top_hits, uint64(1u) );

    // update the maximum
    if (shared_max_range < max_range)
        atomicMax( &shared_max_range, max_range );

    // update the maximum
    if (shared_max_hits < hits_cnt)
        atomicMax( &shared_max_hits, hits_cnt );

    // update the maximum
    if (shared_top_max_hits < top_cnt)
        atomicMax( &shared_top_max_hits, top_cnt );

    __syncthreads();

    // update the maximum
    if (threadIdx.x == 0)
    {
        if (hit_stats[ HIT_STATS_MAX_RANGE ] < shared_max_range)
            atomicMax( (uint32*)(hit_stats + HIT_STATS_MAX_RANGE), shared_max_range );
        if (hit_stats[ HIT_STATS_MAX ] < shared_max_hits)
            atomicMax( (uint32*)(hit_stats + HIT_STATS_MAX), shared_max_hits );
        if (hit_stats[ HIT_STATS_TOP_MAX ] < shared_top_max_hits)
            atomicMax( (uint32*)(hit_stats + HIT_STATS_TOP_MAX), shared_top_max_hits );
    }
}

void Aligner::keep_stats(const uint32 count, Stats& stats)
{
    thrust::fill( hits_stats_dvec.begin(), hits_stats_dvec.end(), 0u );
    hits_stats(
        count,
        nvbio::device_view( hit_deques.hits() ),
        nvbio::device_view( hit_deques.counts() ),
        hits_stats_dptr );

    cudaThreadSynchronize();
    nvbio::cuda::check_error("hit stats kernel");

    nvbio::cuda::thrust_copy_vector(hits_stats_hvec, hits_stats_dvec);

    // poll until previous stats have been consumed
    //while (output_thread.stats.stats_ready) {}

    stats.hits_ranges    += hits_stats_hvec[ HIT_STATS_RANGES ];
    stats.hits_total     += hits_stats_hvec[ HIT_STATS_TOTAL ];
    stats.hits_max        = std::max( stats.hits_max, uint32( hits_stats_hvec[ HIT_STATS_MAX ] ) );
    stats.hits_max_range  = std::max( stats.hits_max_range, uint32( hits_stats_hvec[ HIT_STATS_MAX_RANGE ] ) );
    stats.hits_top_total += hits_stats_hvec[ HIT_STATS_TOP ];
    stats.hits_top_max    = std::max( stats.hits_top_max, uint32( hits_stats_hvec[ HIT_STATS_TOP_MAX ] ) );
    for (uint32 i = 0; i < 28; ++i)
    {
        stats.hits_bins[i]     += hits_stats_hvec[ HIT_STATS_BINS + i ];
        stats.hits_top_bins[i] += hits_stats_hvec[ HIT_STATS_TOP_BINS + i ];
    }
    stats.hits_stats++;

    // mark stats as ready to be consumed
    //output_thread.stats.stats_ready = true;
}

// Compute the total number of matches found
void hits_stats(
    const uint32    batch_size,
    const SeedHit*  hit_data,
    const uint32*   hit_counts,
          uint64*   hit_stats)
{
    const uint32 blocks = (batch_size + BLOCKDIM-1) / BLOCKDIM;

    hits_stats_kernel<<<blocks, BLOCKDIM>>>( batch_size, hit_data, hit_counts, hit_stats );
}

// copy the contents of a section of a ring buffer into a plain array
__global__ 
void ring_buffer_to_plain_array_kernel(
    const uint32* buffer,
    const uint32  buffer_size,
    const uint32  begin,
    const uint32  end,
          uint32* output)
{
    const uint32 thread_id = threadIdx.x + BLOCKDIM*blockIdx.x;

    if (begin + thread_id < end)
        output[ thread_id ] = buffer[ (begin + thread_id) % buffer_size ];
}

void ring_buffer_to_plain_array(
    const uint32* buffer,
    const uint32  buffer_size,
    const uint32  begin,
    const uint32  end,
          uint32* output)
{
    const uint32 blocks = (end - begin + BLOCKDIM-1) / BLOCKDIM;

    ring_buffer_to_plain_array_kernel<<<blocks, BLOCKDIM>>>(
        buffer,
        buffer_size,
        begin,
        end,
        output );
}

} // namespace cuda
} // namespace bowtie2
} // namespace nvbio
