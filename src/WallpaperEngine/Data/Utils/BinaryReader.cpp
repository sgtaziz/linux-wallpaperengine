#include <bit>
#include <iostream>

#include "BinaryReader.h"

#include <cstring>
#include <limits>
#include <stdexcept>

using namespace WallpaperEngine::Data::Utils;

BinaryReader::BinaryReader (ReadStreamSharedPtr file) : m_input (std::move (file)) { }

uint32_t BinaryReader::nextUInt32 () const {
    char buffer[4];

    this->next (buffer, 4);

    if constexpr (std::endian::native == std::endian::little) {
	return uint32_t (uint8_t (buffer[3])) << 24 | uint32_t (uint8_t (buffer[2])) << 16 |
	       uint32_t (uint8_t (buffer[1])) << 8 | uint32_t (uint8_t (buffer[0]));
    } else {
	return uint32_t (uint8_t (buffer[0])) << 24 | uint32_t (uint8_t (buffer[1])) << 16 |
	       uint32_t (uint8_t (buffer[2])) << 8 | uint32_t (uint8_t (buffer[3]));
    }
}

int BinaryReader::nextInt () const { return std::bit_cast<int32_t> (this->nextUInt32 ()); }

float BinaryReader::nextFloat () const {
    float result;
    static_assert (std::endian::native == std::endian::little, "Only little endian is supported for floats");

    this->next (reinterpret_cast<char*> (&result), sizeof (result));

    return result;
}

std::string BinaryReader::nextNullTerminatedString () const {
    std::string output;

    while (const auto c = this->next ()) {
	output += c;
    }

    return output;
}

std::string BinaryReader::nextSizedString () const {
    uint32_t length = this->nextUInt32 ();
    std::string output (length, '\0');

    this->next (output.data (), length);

    return output;
}

void BinaryReader::next (char* out, size_t size) const {
    if (size == 0) return;
    if (size > static_cast<size_t> (std::numeric_limits<std::streamsize>::max ()))
        throw std::runtime_error ("Binary read exceeds stream size range");
    this->m_input->read (out, static_cast<std::streamsize> (size));
    if (this->m_input->gcount () != static_cast<std::streamsize> (size))
        throw std::runtime_error ("Unexpected end of binary data");
}

char BinaryReader::next () const {
    char buffer;
    this->next (&buffer, 1);
    return buffer;
}

std::istream& BinaryReader::base () const { return *this->m_input; }
