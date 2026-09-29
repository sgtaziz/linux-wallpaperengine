#pragma once

#include <iostream>
#include <memory>

namespace WallpaperEngine::Data::Utils {
struct MemoryStream : std::istream, private std::streambuf {
    MemoryStream (std::unique_ptr<char[]> buffer, const size_t size) :
	MemoryStream (std::shared_ptr<char[]> (std::move (buffer)), size) { }

    [[nodiscard]] std::shared_ptr<MemoryStream> clone () const {
	return std::shared_ptr<MemoryStream> (new MemoryStream (this->m_buffer, this->m_size));
    }

    std::streambuf::pos_type
    seekoff (std::streambuf::off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override {
	if (dir == std::ios_base::cur) {
	    gbump (off);
	} else if (dir == std::ios_base::end) {
	    setg (eback (), egptr () + off, egptr ());
	} else if (dir == std::ios_base::beg) {
	    setg (eback (), eback () + off, egptr ());
	}
	return gptr () - eback ();
    }

private:
    MemoryStream (std::shared_ptr<char[]> buffer, const size_t size) :
	std::istream (this), m_buffer (std::move (buffer)), m_size (size) {
	this->setg (this->m_buffer.get (), this->m_buffer.get (), this->m_buffer.get () + size);
    }

protected:
    std::shared_ptr<char[]> m_buffer;

private:
    size_t m_size;
};

using MemoryStreamSharedPtr = std::shared_ptr<MemoryStream>;
using MemoryStreamUniquePtr = std::unique_ptr<MemoryStream>;
}
