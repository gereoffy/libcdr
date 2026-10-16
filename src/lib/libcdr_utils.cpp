/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * This file is part of the libcdr project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "libcdr_utils.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <string.h>

#include <cerrno>
#include <iconv.h>

#define CDR_NUM_ELEMENTS(array) sizeof(array)/sizeof(array[0])

#define SURROGATE_VALUE(h,l) (((h) - 0xd800) * 0x400 + (l) - 0xdc00 + 0x10000)

namespace
{

// No charset given (or ANSI / DEFAULT, which old Hungarian files also use for Central European
// text): without ICU's charset detector, windows-1250 when the text has its o/u with double
// acute (0xd5 0xdb 0xf5 0xfb: Hungarian; the same bytes are rare in windows-1252), else 1252.
static unsigned short getEncoding(const unsigned char *buffer, unsigned long bufferLength)
{
  if (!buffer)
    return 0;
  for (unsigned long i = 0; i < bufferLength; i++)
    if (buffer[i] == 0xd5 || buffer[i] == 0xdb || buffer[i] == 0xf5 || buffer[i] == 0xfb)
      return 0xee;
  return 0;
}

static bool isUnicodeChar(uint32_t c)
{
  return c < 0x110000 && (c < 0xd800 || c > 0xdfff) && (c & 0xfffe) != 0xfffe && (c < 0xfdd0 || c > 0xfdef);
}

static void _appendUCS4(librevenge::RVNGString &text, uint32_t ucs4Character)
{
  // Convert carriage returns to new line characters
  // Writerperfect/LibreOffice will replace them by <text:line-break>
  if (ucs4Character == 0x0d)
    ucs4Character = '\n';

  unsigned char outbuf[5];
  int i = 0;
  if (ucs4Character < 0x80)
    outbuf[i++] = (unsigned char)ucs4Character;
  else if (ucs4Character < 0x800)
  {
    outbuf[i++] = (unsigned char)(0xc0 | (ucs4Character >> 6));
    outbuf[i++] = (unsigned char)(0x80 | (ucs4Character & 0x3f));
  }
  else if (ucs4Character < 0x10000)
  {
    outbuf[i++] = (unsigned char)(0xe0 | (ucs4Character >> 12));
    outbuf[i++] = (unsigned char)(0x80 | ((ucs4Character >> 6) & 0x3f));
    outbuf[i++] = (unsigned char)(0x80 | (ucs4Character & 0x3f));
  }
  else
  {
    outbuf[i++] = (unsigned char)(0xf0 | (ucs4Character >> 18));
    outbuf[i++] = (unsigned char)(0x80 | ((ucs4Character >> 12) & 0x3f));
    outbuf[i++] = (unsigned char)(0x80 | ((ucs4Character >> 6) & 0x3f));
    outbuf[i++] = (unsigned char)(0x80 | (ucs4Character & 0x3f));
  }
  outbuf[i] = 0;

  text.append((char *)outbuf);
}

// Converts bytes in a code page to UTF-8 with iconv (instead of ICU): characters it can't
// convert are skipped, as before.
static void _appendCodepage(librevenge::RVNGString &text, const std::vector<unsigned char> &characters, const char *codepage)
{
  iconv_t cd = iconv_open("UTF-32LE", codepage);
  if (cd == (iconv_t)-1)
    return;
  auto *src = const_cast<char *>(reinterpret_cast<const char *>(&characters[0]));
  size_t srcLeft = characters.size();
  while (srcLeft)
  {
    unsigned char out[4 * 64];
    auto *dst = reinterpret_cast<char *>(out);
    size_t dstLeft = sizeof(out);
    size_t r = iconv(cd, &src, &srcLeft, &dst, &dstLeft);
    for (size_t k = 0; k + 4 <= sizeof(out) - dstLeft; k += 4)
    {
      uint32_t c = out[k] | (out[k + 1] << 8) | (out[k + 2] << 16) | ((uint32_t)out[k + 3] << 24);
      if (isUnicodeChar(c))
        _appendUCS4(text, c);
    }
    if (r == (size_t)-1 && errno != E2BIG)
    {
      if (errno == EINVAL)   // an incomplete sequence at the end
        break;
      src++;   // EILSEQ: skip the byte
      srcLeft--;
    }
  }
  iconv_close(cd);
}

} // anonymous namespace

uint8_t libcdr::readU8(librevenge::RVNGInputStream *input, bool /* bigEndian */)
{
  if (!input || input->isEnd())
  {
    CDR_DEBUG_MSG(("Throwing EndOfStreamException\n"));
    throw EndOfStreamException();
  }
  unsigned long numBytesRead;
  uint8_t const *p = input->read(sizeof(uint8_t), numBytesRead);

  if (p && numBytesRead == sizeof(uint8_t))
    return *(uint8_t const *)(p);
  CDR_DEBUG_MSG(("Throwing EndOfStreamException\n"));
  throw EndOfStreamException();
}

uint16_t libcdr::readU16(librevenge::RVNGInputStream *input, bool bigEndian)
{
  if (!input || input->isEnd())
  {
    CDR_DEBUG_MSG(("Throwing EndOfStreamException\n"));
    throw EndOfStreamException();
  }
  unsigned long numBytesRead;
  uint8_t const *p = input->read(sizeof(uint16_t), numBytesRead);

  if (p && numBytesRead == sizeof(uint16_t))
  {
    if (bigEndian)
      return (uint16_t)(p[1]|((uint16_t)p[0]<<8));
    return (uint16_t)(p[0]|((uint16_t)p[1]<<8));
  }
  CDR_DEBUG_MSG(("Throwing EndOfStreamException\n"));
  throw EndOfStreamException();
}

int16_t libcdr::readS16(librevenge::RVNGInputStream *input, bool bigEndian)
{
  return (int16_t)readU16(input, bigEndian);
}

uint32_t libcdr::readU32(librevenge::RVNGInputStream *input, bool bigEndian)
{
  if (!input || input->isEnd())
  {
    CDR_DEBUG_MSG(("Throwing EndOfStreamException\n"));
    throw EndOfStreamException();
  }
  unsigned long numBytesRead;
  uint8_t const *p = input->read(sizeof(uint32_t), numBytesRead);

  if (p && numBytesRead == sizeof(uint32_t))
  {
    if (bigEndian)
      return (uint32_t)p[3]|((uint32_t)p[2]<<8)|((uint32_t)p[1]<<16)|((uint32_t)p[0]<<24);
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
  }
  CDR_DEBUG_MSG(("Throwing EndOfStreamException\n"));
  throw EndOfStreamException();
}

int32_t libcdr::readS32(librevenge::RVNGInputStream *input, bool bigEndian)
{
  return (int32_t)readU32(input, bigEndian);
}

uint64_t libcdr::readU64(librevenge::RVNGInputStream *input, bool bigEndian)
{
  if (!input || input->isEnd())
  {
    CDR_DEBUG_MSG(("Throwing EndOfStreamException\n"));
    throw EndOfStreamException();
  }
  unsigned long numBytesRead;
  uint8_t const *p = input->read(sizeof(uint64_t), numBytesRead);

  if (p && numBytesRead == sizeof(uint64_t))
  {
    if (bigEndian)
      return (uint64_t)p[7]|((uint64_t)p[6]<<8)|((uint64_t)p[5]<<16)|((uint64_t)p[4]<<24)|((uint64_t)p[3]<<32)|((uint64_t)p[2]<<40)|((uint64_t)p[1]<<48)|((uint64_t)p[0]<<56);
    return (uint64_t)p[0]|((uint64_t)p[1]<<8)|((uint64_t)p[2]<<16)|((uint64_t)p[3]<<24)|((uint64_t)p[4]<<32)|((uint64_t)p[5]<<40)|((uint64_t)p[6]<<48)|((uint64_t)p[7]<<56);
  }
  CDR_DEBUG_MSG(("Throwing EndOfStreamException\n"));
  throw EndOfStreamException();
}

double libcdr::readDouble(librevenge::RVNGInputStream *input, bool bigEndian)
{
  union
  {
    uint64_t u;
    double d;
  } tmpUnion;

  tmpUnion.u = readU64(input, bigEndian);

  return tmpUnion.d;
}

double libcdr::readFixedPoint(librevenge::RVNGInputStream *input, bool bigEndian)
{
  unsigned fixedPointNumber = readU32(input, bigEndian);
  auto fixedPointNumberIntegerPart = (short)((fixedPointNumber & 0xFFFF0000) >> 16);
  auto fixedPointNumberFractionalPart = (double)((double)(fixedPointNumber & 0x0000FFFF)/(double)0xFFFF);
  return ((double)fixedPointNumberIntegerPart + fixedPointNumberFractionalPart);
}

unsigned long libcdr::getLength(librevenge::RVNGInputStream *const input)
{
  if (!input)
    throw EndOfStreamException();

  const long orig = input->tell();
  long end = 0;

  if (input->seek(0, librevenge::RVNG_SEEK_END) == 0)
  {
    end = input->tell();
  }
  else
  {
    // RVNG_SEEK_END does not work. Use the harder way.
    if (input->seek(0, librevenge::RVNG_SEEK_SET) != 0)
      throw EndOfStreamException();
    while (!input->isEnd())
    {
      readU8(input);
      ++end;
    }
  }
  assert(end >= 0);

  if (input->seek(orig, librevenge::RVNG_SEEK_SET) != 0)
    throw EndOfStreamException();

  return static_cast<unsigned long>(end);
}

unsigned long libcdr::getRemainingLength(librevenge::RVNGInputStream *const input)
{
  return getLength(input) - static_cast<unsigned long>(input->tell());
}

int libcdr::cdr_round(double d)
{
  return (d>0) ? int(d+0.5) : int(d-0.5);
}

void libcdr::writeU16(librevenge::RVNGBinaryData &buffer, const int value)
{
  buffer.append((unsigned char)(value & 0xFF));
  buffer.append((unsigned char)((value >> 8) & 0xFF));
}

void libcdr::writeU32(librevenge::RVNGBinaryData &buffer, const int value)
{
  buffer.append((unsigned char)(value & 0xFF));
  buffer.append((unsigned char)((value >> 8) & 0xFF));
  buffer.append((unsigned char)((value >> 16) & 0xFF));
  buffer.append((unsigned char)((value >> 24) & 0xFF));
}

void libcdr::appendCharacters(librevenge::RVNGString &text, std::vector<unsigned char> characters, unsigned short charset)
{
  if (characters.empty())
    return;
  static const uint32_t symbolmap [] =
  {
    0x0020, 0x0021, 0x2200, 0x0023, 0x2203, 0x0025, 0x0026, 0x220D, // 0x20 ..
    0x0028, 0x0029, 0x2217, 0x002B, 0x002C, 0x2212, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x2245, 0x0391, 0x0392, 0x03A7, 0x0394, 0x0395, 0x03A6, 0x0393,
    0x0397, 0x0399, 0x03D1, 0x039A, 0x039B, 0x039C, 0x039D, 0x039F,
    0x03A0, 0x0398, 0x03A1, 0x03A3, 0x03A4, 0x03A5, 0x03C2, 0x03A9,
    0x039E, 0x03A8, 0x0396, 0x005B, 0x2234, 0x005D, 0x22A5, 0x005F,
    0xF8E5, 0x03B1, 0x03B2, 0x03C7, 0x03B4, 0x03B5, 0x03C6, 0x03B3,
    0x03B7, 0x03B9, 0x03D5, 0x03BA, 0x03BB, 0x03BC, 0x03BD, 0x03BF,
    0x03C0, 0x03B8, 0x03C1, 0x03C3, 0x03C4, 0x03C5, 0x03D6, 0x03C9,
    0x03BE, 0x03C8, 0x03B6, 0x007B, 0x007C, 0x007D, 0x223C, 0x0020, // .. 0x7F
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087,
    0x0088, 0x0089, 0x008a, 0x008b, 0x008c, 0x008d, 0x008e, 0x008f,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097,
    0x0098, 0x0099, 0x009a, 0x009b, 0x009c, 0x009d, 0x009E, 0x009f,
    0x20AC, 0x03D2, 0x2032, 0x2264, 0x2044, 0x221E, 0x0192, 0x2663, // 0xA0 ..
    0x2666, 0x2665, 0x2660, 0x2194, 0x2190, 0x2191, 0x2192, 0x2193,
    0x00B0, 0x00B1, 0x2033, 0x2265, 0x00D7, 0x221D, 0x2202, 0x2022,
    0x00F7, 0x2260, 0x2261, 0x2248, 0x2026, 0x23D0, 0x23AF, 0x21B5,
    0x2135, 0x2111, 0x211C, 0x2118, 0x2297, 0x2295, 0x2205, 0x2229,
    0x222A, 0x2283, 0x2287, 0x2284, 0x2282, 0x2286, 0x2208, 0x2209,
    0x2220, 0x2207, 0x00AE, 0x00A9, 0x2122, 0x220F, 0x221A, 0x22C5,
    0x00AC, 0x2227, 0x2228, 0x21D4, 0x21D0, 0x21D1, 0x21D2, 0x21D3,
    0x25CA, 0x3008, 0x00AE, 0x00A9, 0x2122, 0x2211, 0x239B, 0x239C,
    0x239D, 0x23A1, 0x23A2, 0x23A3, 0x23A7, 0x23A8, 0x23A9, 0x23AA,
    0xF8FF, 0x3009, 0x222B, 0x2320, 0x23AE, 0x2321, 0x239E, 0x239F,
    0x23A0, 0x23A4, 0x23A5, 0x23A6, 0x23AB, 0x23AC, 0x23AD, 0x0020  // .. 0xFE
  };

  // East Asian charsets: old files (CorelDRAW 5) mark Hungarian text with them too, and
  // nitroview is not meant for CJK text: guess as when no charset is given
  if (charset == 0x80 || charset == 0x81 || charset == 0x86 || charset == 0x88)
    charset = 0;
  if (!charset && !characters.empty())
    charset = getEncoding(&characters[0], characters.size());

  if (charset == 0x02) // SYMBOL
  {
    uint32_t ucs4Character = 0;
    for (std::vector<unsigned char>::const_iterator iter = characters.begin();
         iter != characters.end(); ++iter)
    {
      if (*iter < 0x20)
        ucs4Character = 0x20;
      else
        ucs4Character = symbolmap[*iter - 0x20];
      _appendUCS4(text, ucs4Character);
    }
  }
  else
  {
    const char *codepage;
    switch (charset)
    {
    case 0x80: // SHIFTJIS
      codepage = "CP932";
      break;
    case 0x81: // HANGUL
      codepage = "CP949";
      break;
    case 0x86: // GB2312
      codepage = "CP936";
      break;
    case 0x88: // CHINESEBIG5
      codepage = "CP950";
      break;
    case 0xa1: // GREEEK
      codepage = "CP1253";
      break;
    case 0xa2: // TURKISH
      codepage = "CP1254";
      break;
    case 0xa3: // VIETNAMESE
      codepage = "CP1258";
      break;
    case 0xb1: // HEBREW
      codepage = "CP1255";
      break;
    case 0xb2: // ARABIC
      codepage = "CP1256";
      break;
    case 0xba: // BALTIC
      codepage = "CP1257";
      break;
    case 0xcc: // RUSSIAN
      codepage = "CP1251";
      break;
    case 0xde: // THAI
      codepage = "CP874";
      break;
    case 0xee: // CENTRAL EUROPE
      codepage = "CP1250";
      break;
    default:   // ANSI, DEFAULT and others: often Central European text (old Hungarian files)
      codepage = getEncoding(&characters[0], characters.size()) == 0xee ? "CP1250" : "CP1252";
      break;
    }
    _appendCodepage(text, characters, codepage);
  }
}

void libcdr::appendCharacters(librevenge::RVNGString &text, std::vector<unsigned char> characters)
{
  if (characters.empty())
    return;

  // UTF-16LE (decoded here; ICU did it before): unpaired surrogates are skipped
  for (size_t i = 0; i + 1 < characters.size(); i += 2)
  {
    uint32_t c = characters[i] | (characters[i + 1] << 8);
    if (c >= 0xd800 && c <= 0xdbff && i + 3 < characters.size())
    {
      uint32_t l = characters[i + 2] | (characters[i + 3] << 8);
      if (l >= 0xdc00 && l <= 0xdfff)
      {
        c = SURROGATE_VALUE(c, l);
        i += 2;
      }
    }
    if (isUnicodeChar(c))
      _appendUCS4(text, c);
  }
}

void libcdr::appendUTF8Characters(librevenge::RVNGString &text, std::vector<unsigned char> characters)
{
  if (characters.empty())
    return;

  for (std::vector<unsigned char>::const_iterator iter = characters.begin(); iter != characters.end(); ++iter)
    text.append((char)*iter);
}

#ifdef DEBUG

void libcdr::debugPrint(const char *const format, ...)
{
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
}

const char *libcdr::toFourCC(unsigned value, bool bigEndian)
{
  static char sValue[5] = { 0, 0, 0, 0, 0 };
  if (bigEndian)
  {
    sValue[3] = (char)(value & 0xff);
    sValue[2] = (char)((value & 0xff00) >> 8);
    sValue[1] = (char)((value & 0xff0000) >> 16);
    sValue[0] = (char)((value & 0xff000000) >> 24);
  }
  else
  {
    sValue[0] = (char)(value & 0xff);
    sValue[1] = (char)((value & 0xff00) >> 8);
    sValue[2] = (char)((value & 0xff0000) >> 16);
    sValue[3] = (char)((value & 0xff000000) >> 24);
  }
  return sValue;
}
#endif

/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
