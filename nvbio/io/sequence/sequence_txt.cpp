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

#include <nvbio/io/sequence/sequence_txt.h>
#include <nvbio/io/sequence/sequence_encoder.h>
#include <nvbio/basic/types.h>

#include <string.h>
#include <ctype.h>

namespace nvbio {
namespace io {

///@addtogroup IO
///@{

///@addtogroup SequenceIO
///@{

///@addtogroup SequenceIODetail
///@{

int SequenceDataFile_TXT::nextChunk(SequenceDataEncoder* output, uint32 max_reads, uint32 max_bps)
{
    const char* name = "";

    uint32 n_reads = 0;
    uint32 n_bps   = 0;

    const uint32 read_mult =
        ((m_flags & FORWARD)            ? 1u : 0u) +
        ((m_flags & REVERSE)            ? 1u : 0u) +
        ((m_flags & FORWARD_COMPLEMENT) ? 1u : 0u) +
        ((m_flags & REVERSE_COMPLEMENT) ? 1u : 0u);

    while (n_reads + read_mult                       <= max_reads &&
           n_bps + read_mult*SequenceDataFile::LONG_READ <= max_bps)
    {
        // reset the read
        m_read_bp.erase( m_read_bp.begin(), m_read_bp.end() );

        // read an entire line
        for (uint8 c = get(); c != '\n' && c != 0; c = get())
        {
            // if (isgraph(c))
            if (c >= 0x21 && c <= 0x7E)
                m_read_bp.push_back( c );
        }

        ++m_line;

        if (m_read_q.size() < m_read_bp.size())
        {
            // extend the quality score vector if needed
            const size_t old_size = m_read_q.size();
            m_read_q.resize( m_read_bp.size() );
            for (size_t i = old_size; i < m_read_bp.size(); ++i)
                m_read_q[i] = char(255);
        }

        if (m_read_bp.size())
        {
            if (m_flags & FORWARD)
            {
                output->push_back(uint32( m_read_bp.size() ),
                                  name,
                                  &m_read_bp[0],
                                  &m_read_q[0],
                                  m_quality_encoding,
                                  m_truncate_read_len,
                                  SequenceDataEncoder::NO_OP );
            }
            if (m_flags & REVERSE)
            {
                output->push_back(uint32( m_read_bp.size() ),
                                  name,
                                  &m_read_bp[0],
                                  &m_read_q[0],
                                  m_quality_encoding,
                                  m_truncate_read_len,
                                  SequenceDataEncoder::REVERSE_OP );
            }
            if (m_flags & FORWARD_COMPLEMENT)
            {
                output->push_back(uint32( m_read_bp.size() ),
                                  name,
                                  &m_read_bp[0],
                                  &m_read_q[0],
                                  m_quality_encoding,
                                  m_truncate_read_len,
                                  SequenceDataEncoder::COMPLEMENT_OP );
            }
            if (m_flags & REVERSE_COMPLEMENT)
            {
                output->push_back(uint32( m_read_bp.size() ),
                                  name,
                                  &m_read_bp[0],
                                  &m_read_q[0],
                                  m_quality_encoding,
                                  m_truncate_read_len,
                                  SequenceDataEncoder::REVERSE_COMPLEMENT_OP );
            }

            n_bps   += read_mult * (uint32)m_read_bp.size();
            n_reads += read_mult;
        }

        // check for end-of-file
        if (m_file_state != FILE_OK)
            break;
    }
    return n_reads;
}

SequenceDataFile_TXT_gz::SequenceDataFile_TXT_gz(
    const char*             read_file_name,
    const QualityEncoding   qualities,
    const uint32            max_reads,
    const uint32            max_read_len,
    const SequenceEncoding  flags,
    const uint32            buffer_size)
    : SequenceDataFile_TXT(read_file_name, qualities, max_reads, max_read_len, flags, buffer_size)
{
    m_file = gzopen(read_file_name, "r");
    if (!m_file) {
        m_file_state = FILE_OPEN_FAILED;
    } else {
        m_file_state = FILE_OK;
    }

    gzbuffer(m_file, m_buffer_size);
}

SequenceDataFile_TXT::FileState SequenceDataFile_TXT_gz::fillBuffer(void)
{
    m_buffer_size = gzread(m_file, &m_buffer[0], (uint32)m_buffer.size());
    if (m_buffer_size <= 0)
    {
        // check for EOF separately; zlib will not always return Z_STREAM_END at EOF below
        if (gzeof(m_file))
        {
            return FILE_EOF;
        } else {
            // ask zlib what happened and inform the user
            int err;
            const char *msg;

            msg = gzerror(m_file, &err);
            // we're making the assumption that we never see Z_STREAM_END here
            assert(err != Z_STREAM_END);

            log_error(stderr, "error processing TXT file: zlib error %d (%s)\n", err, msg);
            return FILE_STREAM_ERROR;
        }
    }

    return FILE_OK;
}

///@} // SequenceIODetail
///@} // SequenceIO
///@} // IO

} // namespace io
} // namespace nvbio
