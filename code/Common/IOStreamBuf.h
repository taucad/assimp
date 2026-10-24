/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Copyright (c) 2006-2025, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
  contributors may be used to endorse or promote products
  derived from this software without specific prior
  written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

----------------------------------------------------------------------
*/

/** @file IOStreamBuf.h
 *  @brief A std::streambuf that writes through to an IOStream in fixed-size chunks.
 */
#pragma once
#ifndef AI_IOSTREAMBUF_H_INC
#define AI_IOSTREAMBUF_H_INC

#include <assimp/IOStream.hpp>

#include <cstring>
#include <ios>
#include <streambuf>
#include <vector>

namespace Assimp {

// ------------------------------------------------------------------------------------------------
/** Put-area streambuf over an IOStream. Text exporters write into a std::ostream over it, so their
 *  output reaches the IOStream in chunks of at most `capacity` bytes instead of being built in a
 *  std::ostringstream, copied out with str() and then written (three copies of the output at peak).
 *
 *  A failed IOStream::Write makes every later write fail too and sets the stream's badbit, so the
 *  exporter's `fail()` check reports it. Call `flush()` on the ostream (or `pubsync()` here) before
 *  checking `failed()`; the destructor flushes what is left but cannot report an error. tellp()
 *  returns the number of bytes written so far. */
class IOStreamBuf final : public std::streambuf {
public:
    static constexpr size_t kDefaultCapacity = size_t(1) << 20;

    explicit IOStreamBuf(IOStream *stream, size_t capacity = kDefaultCapacity) :
            mStream(stream), mBuffer(capacity == 0 ? 1 : capacity) {
        setp(mBuffer.data(), mBuffer.data() + mBuffer.size());
    }

    ~IOStreamBuf() override {
        flushBuffer();
    }

    IOStreamBuf(const IOStreamBuf &) = delete;
    IOStreamBuf &operator=(const IOStreamBuf &) = delete;

    /// True once a write to the IOStream failed.
    bool failed() const {
        return mFailed;
    }

    /// Bytes handed to the IOStream so far, plus those still buffered.
    size_t size() const {
        return mWritten + static_cast<size_t>(pptr() - pbase());
    }

protected:
    int_type overflow(int_type ch) override {
        if (!flushBuffer()) {
            return traits_type::eof();
        }
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            *pptr() = traits_type::to_char_type(ch);
            pbump(1);
        }
        return traits_type::not_eof(ch);
    }

    std::streamsize xsputn(const char *s, std::streamsize n) override {
        if (mFailed || n <= 0) {
            return mFailed ? 0 : n;
        }
        if (n > epptr() - pptr()) {
            if (!flushBuffer()) {
                return 0;
            }
            // Large blocks (binary vertex arrays) go straight through.
            if (n >= static_cast<std::streamsize>(mBuffer.size())) {
                if (mStream->Write(s, 1, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
                    mFailed = true;
                    return 0;
                }
                mWritten += static_cast<size_t>(n);
                return n;
            }
        }
        std::memcpy(pptr(), s, static_cast<size_t>(n));
        pbump(static_cast<int>(n));
        return n;
    }

    int sync() override {
        return flushBuffer() ? 0 : -1;
    }

    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override {
        if (off == 0 && dir == std::ios_base::cur && (which & std::ios_base::out)) {
            return pos_type(static_cast<off_type>(size()));
        }
        return pos_type(off_type(-1));
    }

private:
    bool flushBuffer() {
        const size_t n = static_cast<size_t>(pptr() - pbase());
        if (mFailed) {
            return false;
        }
        if (n != 0) {
            if (mStream == nullptr || mStream->Write(pbase(), 1, n) != n) {
                mFailed = true;
                return false;
            }
            mWritten += n;
        }
        setp(mBuffer.data(), mBuffer.data() + mBuffer.size());
        return true;
    }

    IOStream *mStream;
    std::vector<char> mBuffer;
    size_t mWritten = 0;
    bool mFailed = false;
};

} // namespace Assimp

#endif // AI_IOSTREAMBUF_H_INC
