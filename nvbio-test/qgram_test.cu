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

// qgroup_test.cu
//
//#define CUFMI_CUDA_DEBUG
//#define CUFMI_CUDA_ASSERTS

#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>
#include <nvbio/basic/timer.h>
#include <nvbio/basic/console.h>
#include <nvbio/basic/vector_wrapper.h>
#include <nvbio/basic/packedstream.h>
#include <nvbio/basic/shared_pointer.h>
#include <nvbio/io/reads/reads.h>
#include <nvbio/qgroup/qgroup.h>

namespace nvbio {

int qgroup_test(int argc, char* argv[])
{
    uint32 len   = 10000000;
    char*  reads = "./data/SRR493095_1.fastq.gz";

    for (int i = 0; i < argc; ++i)
    {
        if (strcmp( argv[i], "-length" ) == 0)
            len = atoi( argv[++i] )*1000;
        if (strcmp( argv[i], "-reads" ) == 0)
            reads = argv[++i];
    }

    log_info(stderr, "q-group test... started\n");

    const io::QualityEncoding qencoding = io::Phred33;

    log_info(stderr, "  loading reads... started\n");

    SharedPointer<io::ReadDataStream> read_data_file(
        io::open_read_file(
            reads,
            qencoding,
            uint32(-1),
            uint32(-1) ) );

    if (read_data_file == NULL || read_data_file->is_ok() == false)
    {
        log_error(stderr, "    failed opening file \"%s\"\n", reads);
        return 1u;
    }

    const uint32 batch_size = uint32(-1);
    const uint32 batch_bps  = len;

    // load a batch of reads
    SharedPointer<io::ReadData> h_read_data( read_data_file->next( batch_size, batch_bps ) );
    
    // build its device version
    io::ReadDataCUDA d_read_data( *h_read_data );

    log_info(stderr, "  loading reads... done\n");

    // fetch the actual string
    typedef io::ReadData::const_read_stream_type string_type;

    const uint32      string_len = d_read_data.bps();
    const string_type string     = string_type( d_read_data.read_stream() );

    log_info(stderr, "  building q-group index... started\n");
    log_info(stderr, "    symbols: %.1f M symbols\n", 1.0e-6f * float(string_len));

    // build the Q-Group
    QGroupIndexDevice qgroup_index;

    Timer timer;
    timer.start();

    qgroup_index.build<2u>(     // implicitly convert N to A
        16u,
        string_len,
        string );

    cudaDeviceSynchronize();
    timer.stop();
    const float time = timer.seconds();

    log_info(stderr, "  building q-group index... done\n");
    log_info(stderr, "    unique q-grams : %.2f M q-grams\n", 1.0e-6f * float( qgroup_index.n_unique_qgrams ));
    log_info(stderr, "    throughput     : %.1f M q-grams/s\n", 1.0e-6f * float( string_len ) / time);
    log_info(stderr, "    memory usage   : %.1f MB\n", float( qgroup_index.used_device_memory() ) / float(1024*1024) );

    log_info(stderr, "q-group test... done\n" );
    return 0;
}

} // namespace nvbio