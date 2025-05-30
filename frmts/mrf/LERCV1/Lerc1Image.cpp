/*
Copyright 2015 - 2024 Esri

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
A local copy of the license and additional notices are located with the
source distribution at:

http://github.com/Esri/lerc/

Contributors:  Thomas Maurer
               Lucian Plesea
*/

#include "Lerc1Image.h"
#include <cstdint>
#include <cmath>
#include <cfloat>
#include <climits>
#include <string>
#include <algorithm>

NAMESPACE_LERC1_START

// max quantized value, 28 bits
// It is wasting a few bits, because a float has only 24bits of precision
static const double MAXQ = 0x1000000;

// RLE constants
static const int MAX_RUN = 32767;
static const int MIN_RUN = 5;
// End of Transmission
static const int EOT = -(MAX_RUN + 1);

// Decode a RLE bitmask, size should be already set
// Returns false if input seems wrong
// Zero size mask is fine, only checks the end marker
bool BitMaskV1::RLEdecompress(const Byte *src, size_t n)
{
    Byte *dst = bits.data();
    int sz = size();
    short int count;

// Read a low endian short int
#define READ_COUNT                                                             \
    if (true)                                                                  \
    {                                                                          \
        if (n < 2)                                                             \
            return false;                                                      \
        count = *src++;                                                        \
        count += (*src++ << 8);                                                \
    }

    while (sz > 0)
    {  // One sequence per loop
        READ_COUNT;
        n -= 2;
        if (count < 0)
        {  // negative count for repeats
            if (0 == n)
                return false;
            --n;  // only decrement after checking for 0 to avoid a (harmless)
                  // unsigned integer overflow warning with ossfuzz
            Byte b = *src++;
            sz += count;
            if (sz < 0)
                return false;
            while (0 != count++)
                *dst++ = b;
        }
        else
        {  // No repeats, count is positive
            if (sz < count || n < static_cast<size_t>(count))
                return false;
            sz -= count;
            n -= count;
            while (0 != count--)
                *dst++ = *src++;
        }
    }
    READ_COUNT;
    return (count == EOT);
}

// Encode helper function
// It returns how many times the byte at *s is repeated
// a value between 1 and min(max_count, MAX_RUN)
inline static int run_length(const Byte *s, int max_count)
{
    if (max_count > MAX_RUN)
        max_count = MAX_RUN;
    for (int i = 1; i < max_count; i++)
        if (s[0] != s[i])
            return i;
    return max_count;
}

// RLE compressed size is bound by n + 4 + 2 * (n - 1) / 32767
int BitMaskV1::RLEcompress(Byte *dst) const
{
    const Byte *src = bits.data();  // Next input byte
    Byte *start = dst;
    int sz = size();   // left to process
    Byte *pCnt = dst;  // Pointer to current sequence count
    int oddrun = 0;    // non-repeated byte count

// Store val as short low endian integer
#define WRITE_COUNT(val)                                                       \
    if (true)                                                                  \
    {                                                                          \
        *pCnt++ = Byte(val & 0xff);                                            \
        *pCnt++ = Byte(val >> 8);                                              \
    }
// Flush an existing odd run
#define FLUSH                                                                  \
    if (oddrun)                                                                \
    {                                                                          \
        WRITE_COUNT(oddrun);                                                   \
        pCnt += oddrun;                                                        \
        dst = pCnt + 2;                                                        \
        oddrun = 0;                                                            \
    }

    dst += 2;  // Skip the space for the first count
    while (sz > 0)
    {
        int run = run_length(src, sz);
        if (run < MIN_RUN)
        {  // Use one byte
            *dst++ = *src++;
            sz--;
            if (MAX_RUN == ++oddrun)
                FLUSH;
        }
        else
        {  // Found a run
            FLUSH;
            WRITE_COUNT(-run);
            *pCnt++ = *src;
            src += run;
            sz -= run;
            // cppcheck-suppress redundantAssignment
            dst = pCnt + 2;  // after the next marker
        }
    }
    // cppcheck-suppress uselessAssignmentPtrArg
    FLUSH;
    (void)oddrun;
    (void)dst;
    WRITE_COUNT(EOT);  // End marker
    // return compressed output size
    return int(pCnt - start);
}

// calculate encoded size
int BitMaskV1::RLEsize() const
{
    const Byte *src = bits.data();  // Next input byte
    int sz = size();                // left to process
    int oddrun = 0;                 // current non-repeated byte count
    // Simulate an odd run flush
#define SIMFLUSH                                                               \
    if (oddrun)                                                                \
    {                                                                          \
        osz += oddrun + 2;                                                     \
        oddrun = 0;                                                            \
    }
    int osz = 2;  // output size, start with size of end marker
    while (sz)
    {
        int run = run_length(src, sz);
        if (run < MIN_RUN)
        {
            src++;
            sz--;
            if (MAX_RUN == ++oddrun)
                SIMFLUSH;
        }
        else
        {
            SIMFLUSH;
            src += run;
            sz -= run;
            osz += 3;  // Any run is 3 bytes
        }
    }
    return oddrun ? (osz + oddrun + 2) : osz;
}

// Lookup tables for number of bytes in float and int, forward and reverse
static const Byte bits67[4] = {0x80, 0x40, 0xc0, 0};  // shifted left 6 bits
static const Byte stib67[4] = {4, 2, 1, 0};           // Last one is not used

static int numBytesUInt(unsigned int k)
{
    return (k <= 0xff) ? 1 : (k <= 0xffff) ? 2 : 4;
}

// Index of top set bit, counting from 1
static int nBits(unsigned int v)
{
    int r = int(0 != (v >> 16)) << 4;
    v >>= r;
    int t = int(0 != (v >> 8)) << 3;
    v >>= t;
    r += t;
    t = int(0 != (v >> 4)) << 2;
    v = (v >> t) << 1;
    return 1 + r + t + int((0xffffaa50ul >> v) & 0x3);
}

static bool blockread(Byte **ppByte, size_t &size, std::vector<unsigned int> &d)
{
    if (!ppByte || !size)
        return false;

    Byte numBits = **ppByte;
    Byte n = stib67[numBits >> 6];
    numBits &= 63;  // bits 0-5;
    // cppcheck-suppress knownConditionTrueFalse
    if (numBits >= 32 || n == 0 || size < 1 + static_cast<size_t>(n))
        return false;
    *ppByte += 1;
    size -= 1;

    unsigned int numElements = 0;
    memcpy(&numElements, *ppByte, n);
    *ppByte += n;
    size -= n;
    if (static_cast<size_t>(numElements) > d.size())
        return false;
    if (numBits == 0)
    {  // Nothing to read, all zeros
        d.resize(0);
        d.resize(numElements, 0);
        return true;
    }

    d.resize(numElements);
    unsigned int numBytes = (numElements * numBits + 7) / 8;
    if (size < numBytes)
        return false;
    size -= numBytes;

    int bits = 0;  // Available in accumulator, at the high end
    unsigned int acc = 0;
    for (unsigned int &val : d)
    {
        if (bits >= numBits)
        {  // Enough bits in accumulator
            val = acc >> (32 - numBits);
            acc <<= numBits;
            bits -= numBits;
            continue;
        }

        // Need to reload the accumulator
        val = 0;
        if (bits)
        {
            val = acc >> (32 - bits);
            val <<= (numBits - bits);
        }
        unsigned int nb = std::min(numBytes, 4u);
        if (4u == nb)
            memcpy(&acc, *ppByte, 4);
        else  // Read only a few bytes at the high end of acc
            memcpy(reinterpret_cast<Byte *>(&acc) + (4 - nb), *ppByte, nb);
        *ppByte += nb;
        numBytes -= nb;

        bits += 32 - numBits;
        val |= acc >> bits;
        acc <<= 32 - bits;
    }
    return numBytes == 0;
}

static const int CNT_Z = 8;
static const int CNT_Z_VER = 11;
static const std::string sCntZImage("CntZImage ");  // Includes a space

// computes the size of a CntZImage of any width and height, but all void /
// invalid, and then compressed
unsigned int Lerc1Image::computeNumBytesNeededToWriteVoidImage()
{
    unsigned int sz =
        (unsigned int)sCntZImage.size() + 4 * sizeof(int) + sizeof(double);
    // cnt part
    sz += 3 * sizeof(int) + sizeof(float);
    // z part, 1 is the empty Tile if all invalid
    sz += 3 * sizeof(int) + sizeof(float) + 1;
    return sz;  // 67
}

unsigned int
Lerc1Image::computeNumBytesNeededToWrite(double maxZError, bool onlyZPart,
                                         InfoFromComputeNumBytes *info) const
{
    unsigned int sz =
        (unsigned int)(sCntZImage.size() + 4 * sizeof(int) + sizeof(double));
    if (!onlyZPart)
    {
        auto m = mask.IsValid(0);
        info->numTilesVertCnt = 0;
        info->numTilesHoriCnt = 0;
        info->maxCntInImg = m;
        info->numBytesCnt = 0;
        for (int i = 0; i < getSize(); i++)
            if (m != mask.IsValid(i))
            {
                info->numBytesCnt = mask.RLEsize();
                info->maxCntInImg = 1;
                break;
            }
        sz += 3 * sizeof(int) + sizeof(float) + info->numBytesCnt;
    }

    // z part
    int numTilesVert, numTilesHori, numBytesOpt;
    float maxValInImg;
    if (!findTiling(maxZError, numTilesVert, numTilesHori, numBytesOpt,
                    maxValInImg))
        return 0;

    info->maxZError = maxZError;
    info->numTilesVertZ = numTilesVert;
    info->numTilesHoriZ = numTilesHori;
    info->numBytesZ = numBytesOpt;
    info->maxZInImg = maxValInImg;

    sz += 3 * sizeof(int) + sizeof(float) + numBytesOpt;
    return sz;
}

// if you change the file format, don't forget to update not only write and
// read functions, and the file version number, but also the computeNumBytes...
// and numBytes... functions
bool Lerc1Image::write(Byte **ppByte, double maxZError, bool zPart) const
{
// Local macro, write an unaligned variable, adjust pointer
#define WRVAR(VAR, PTR)                                                        \
    memcpy((PTR), &(VAR), sizeof(VAR));                                        \
    (PTR) += sizeof(VAR)
    if (getSize() == 0)
        return false;

    // signature
    memcpy(*ppByte, sCntZImage.c_str(), sCntZImage.size());
    *ppByte += sCntZImage.size();

    int height = getHeight();
    int width = getWidth();
    WRVAR(CNT_Z_VER, *ppByte);
    WRVAR(CNT_Z, *ppByte);
    WRVAR(height, *ppByte);
    WRVAR(width, *ppByte);
    WRVAR(maxZError, *ppByte);

    InfoFromComputeNumBytes info;
    if (0 == computeNumBytesNeededToWrite(maxZError, zPart, &info))
        return false;

    do
    {
        int numTilesVert, numTilesHori, numBytesOpt, numBytesWritten = 0;
        float maxValInImg;

        if (!zPart)
        {
            numTilesVert = info.numTilesVertCnt;
            numTilesHori = info.numTilesHoriCnt;
            numBytesOpt = info.numBytesCnt;
            maxValInImg = info.maxCntInImg;
        }
        else
        {
            numTilesVert = info.numTilesVertZ;
            numTilesHori = info.numTilesHoriZ;
            numBytesOpt = info.numBytesZ;
            maxValInImg = info.maxZInImg;
        }

        WRVAR(numTilesVert, *ppByte);
        WRVAR(numTilesHori, *ppByte);
        WRVAR(numBytesOpt, *ppByte);
        WRVAR(maxValInImg, *ppByte);

        if (!zPart && numTilesVert == 0 && numTilesHori == 0)
        {                         // no tiling for cnt part
            if (numBytesOpt > 0)  // cnt part is binary mask, use fast RLE class
                numBytesWritten = mask.RLEcompress(*ppByte);
        }
        else
        {  // encode tiles to buffer, always z part
            float maxVal;
            if (!writeTiles(maxZError, numTilesVert, numTilesHori, *ppByte,
                            numBytesWritten, maxVal))
                return false;
        }

        if (numBytesWritten != numBytesOpt)
            return false;

        *ppByte += numBytesWritten;
        zPart = !zPart;
    } while (zPart);
    return true;
#undef WRVAR
}

// To avoid excessive memory allocation attempts, this is still 1.8GB!!
static size_t TOO_LARGE = 1800 * 1000 * 1000 / static_cast<int>(sizeof(float));

bool Lerc1Image::read(Byte **ppByte, size_t &nRemainingBytes, double maxZError,
                      bool ZPart)
{
// Local macro, read an unaligned variable, adjust pointer
#define RDVAR(PTR, VAR)                                                        \
    memcpy(&(VAR), (PTR), sizeof(VAR));                                        \
    (PTR) += sizeof(VAR)

    size_t len = sCntZImage.length();
    if (nRemainingBytes < len)
        return false;

    std::string typeStr(reinterpret_cast<char *>(*ppByte), len);
    if (typeStr != sCntZImage)
        return false;
    *ppByte += len;
    nRemainingBytes -= len;

    int version = 0, type = 0;
    int width = 0, height = 0;
    double maxZErrorInFile = 0;

    if (nRemainingBytes < (4 * sizeof(int) + sizeof(double)))
        return false;
    RDVAR(*ppByte, version);
    RDVAR(*ppByte, type);
    RDVAR(*ppByte, height);
    RDVAR(*ppByte, width);
    RDVAR(*ppByte, maxZErrorInFile);
    nRemainingBytes -= 4 * sizeof(int) + sizeof(double);

    if (version != CNT_Z_VER || type != CNT_Z)
        return false;
    if (width <= 0 || width > 20000 || height <= 0 || height > 20000 ||
        maxZErrorInFile > maxZError)
        return false;
    if (static_cast<size_t>(width) * height > TOO_LARGE)
        return false;

    if (ZPart)
    {
        if (width != getWidth() || height != getHeight())
            return false;
    }
    else
    {  // Resize clears the buffer
        resize(width, height);
    }

    do
    {
        int numTilesVert = 0, numTilesHori = 0, numBytes = 0;
        float maxValInImg = 0;
        if (nRemainingBytes < 3 * sizeof(int) + sizeof(float))
            return false;
        RDVAR(*ppByte, numTilesVert);
        RDVAR(*ppByte, numTilesHori);
        RDVAR(*ppByte, numBytes);
        RDVAR(*ppByte, maxValInImg);
        nRemainingBytes -= 3 * sizeof(int) + sizeof(float);

        if (numBytes < 0 || nRemainingBytes < static_cast<size_t>(numBytes))
            return false;
        if (ZPart)
        {
            if (!readTiles(maxZErrorInFile, numTilesVert, numTilesHori,
                           maxValInImg, *ppByte, numBytes))
                return false;
        }
        else
        {  // no tiling allowed for the cnt part
            if (numTilesVert != 0 && numTilesHori != 0)
                return false;
            if (numBytes == 0)
            {  // cnt part is const
                if (maxValInImg != 0.0 && maxValInImg != 1.0)
                    return false;  // Only 0 and 1 are valid
                bool v = (maxValInImg != 0.0);
                for (int k = 0; k < getSize(); k++)
                    mask.Set(k, v);
            }
            else
            {  // cnt part is binary mask, RLE compressed
                if (!mask.RLEdecompress(*ppByte, static_cast<size_t>(numBytes)))
                    return false;
            }
        }
        *ppByte += numBytes;
        nRemainingBytes -= numBytes;
        ZPart = !ZPart;
    } while (ZPart);  // Stop after writing Z
    return true;
}

// Initialize from the given header, return true if it worked
// It could read more info from the header, if needed
bool Lerc1Image::getwh(const Byte *pByte, size_t nBytes, int &width,
                       int &height)
{
    size_t len = sCntZImage.length();
    if (nBytes < len)
        return false;

    std::string typeStr(reinterpret_cast<const char *>(pByte), len);
    if (typeStr != sCntZImage)
        return false;
    pByte += len;
    nBytes -= len;

    int version = 0, type = 0;
    double maxZErrorInFile = 0;

    if (nBytes < (4 * sizeof(int) + sizeof(double)))
        return false;
    RDVAR(pByte, version);
    RDVAR(pByte, type);
    RDVAR(pByte, height);
    RDVAR(pByte, width);
    RDVAR(pByte, maxZErrorInFile);
    (void)pByte;

    if (version != CNT_Z_VER || type != CNT_Z)
        return false;
    if (width <= 0 || width > 20000 || height <= 0 || height > 20000)
        return false;
    if (static_cast<size_t>(width) * height > TOO_LARGE)
        return false;

    return true;
#undef RDVAR
}

bool Lerc1Image::findTiling(double maxZError, int &numTilesVertA,
                            int &numTilesHoriA, int &numBytesOptA,
                            float &maxValInImgA) const
{
    // entire image as 1 block, this is usually the worst case
    numTilesVertA = numTilesHoriA = 1;
    if (!writeTiles(maxZError, 1, 1, nullptr, numBytesOptA, maxValInImgA))
        return false;
    // The actual figure may be different due to round-down
    static const std::vector<int> tileWidthArr = {8, 11, 15, 20, 32, 64};
    for (auto tileWidth : tileWidthArr)
    {
        int numTilesVert = static_cast<int>(getHeight() / tileWidth);
        int numTilesHori = static_cast<int>(getWidth() / tileWidth);

        if (numTilesVert * numTilesHori < 2)
            return true;

        int numBytes = 0;
        float maxVal;
        if (!writeTiles(maxZError, numTilesVert, numTilesHori, nullptr,
                        numBytes, maxVal))
            return false;
        if (numBytes > numBytesOptA)
            break;  // Stop when size start to increase
        if (numBytes < numBytesOptA)
        {
            numTilesVertA = numTilesVert;
            numTilesHoriA = numTilesHori;
            numBytesOptA = numBytes;
        }
    }
    return true;
}

// n is 1, 2 or 4
static Byte *writeFlt(Byte *ptr, float z, int n)
{
    if (4 == n)
        memcpy(ptr, &z, 4);
    else if (1 == n)
        *ptr = static_cast<Byte>(static_cast<signed char>(z));
    else
    {
        signed short s = static_cast<signed short>(z);
        memcpy(ptr, &s, 2);
    }
    return ptr + n;
}

// Only small, exact integer values return 1 or 2, otherwise 4
static int numBytesFlt(float z)
{
    if (!std::isfinite(z) || z > SHRT_MAX || z < SHRT_MIN || z != int16_t(z))
        return 4;
    if (z > SCHAR_MAX || z < SCHAR_MIN)
        return 2;
    return 1;
}

static int numBytesZTile(int nValues, float zMin, float zMax, double maxZError)
{
    if (nValues == 0 || (zMin == 0 && zMax == 0))
        return 1;
    if (maxZError == 0 || !std::isfinite(zMin) || !std::isfinite(zMax) ||
        ((double)zMax - zMin) / (2 * maxZError) > MAXQ)  // max of 28 bits
        return (int)(1 + nValues * sizeof(float));       // Stored as such
    unsigned int maxElem = static_cast<unsigned int>(
        ((double)zMax - zMin) / (2 * maxZError) + 0.5);
    int nb = 1 + numBytesFlt(zMin);
    if (maxElem == 0)
        return nb;
    return nb + 1 + numBytesUInt(nValues) + (nValues * nBits(maxElem) + 7) / 8;
}

// Pass bArr == nullptr to estimate the size but skip the write
bool Lerc1Image::writeTiles(double maxZError, int numTilesV, int numTilesH,
                            Byte *bArr, int &numBytes, float &maxValInImg) const
{
    if (numTilesV == 0 || numTilesH == 0)
        return false;
    numBytes = 0;
    maxValInImg = -FLT_MAX;
    int tileHeight = static_cast<int>(getHeight() / numTilesV);
    int tileWidth = static_cast<int>(getWidth() / numTilesH);
    for (int v0 = 0; v0 < getHeight(); v0 += tileHeight)
    {
        int v1 = std::min(getHeight(), v0 + tileHeight);
        for (int h0 = 0; h0 < getWidth(); h0 += tileWidth)
        {
            int h1 = std::min(getWidth(), h0 + tileWidth);
            float zMin = 0, zMax = 0;
            int numValidPixel = 0, numFinite = 0;
            if (!computeZStats(v0, v1, h0, h1, zMin, zMax, numValidPixel,
                               numFinite))
                return false;

            if (maxValInImg < zMax)
                maxValInImg = zMax;

            int numBytesNeeded = 1;
            if (numValidPixel != 0)
            {
                if (numFinite == 0 && numValidPixel == (v1 - v0) * (h1 - h0) &&
                    isallsameval(v0, v1, h0, h1))
                    numBytesNeeded = 5;  // Stored as non-finite constant block
                else
                {
                    numBytesNeeded =
                        numBytesZTile(numValidPixel, zMin, zMax, maxZError);
                    // Try moving zMin up by almost maxZError,
                    // it may require fewer bytes
                    float zm = static_cast<float>(zMin + 0.999999 * maxZError);
                    if (numFinite == numValidPixel && zm <= zMax)
                    {
                        int nBN =
                            numBytesZTile(numValidPixel, zm, zMax, maxZError);
                        // Maybe an int value for zMin saves a few bytes?
                        if (zMin < floorf(zm))
                        {
                            int nBNi = numBytesZTile(numValidPixel, floorf(zm),
                                                     zMax, maxZError);
                            if (nBNi < nBN)
                            {
                                zm = floorf(zm);
                                nBN = nBNi;
                            }
                        }
                        if (nBN < numBytesNeeded)
                        {
                            zMin = zm;
                            numBytesNeeded = nBN;
                        }
                    }
                }
            }
            numBytes += numBytesNeeded;

            if (bArr)
            {  // Skip the write if no pointer was provided
                int numBytesWritten = 0;
                if (numFinite == 0 && numValidPixel == (v1 - v0) * (h1 - h0) &&
                    isallsameval(v0, v1, h0, h1))
                {
                    // direct write as non-finite const block, 4 byte float
                    *bArr++ = 3;  // 3 | bits67[3]
                    bArr = writeFlt(bArr, (*this)(v0, h0), sizeof(float));
                    numBytesWritten = 5;
                }
                else
                {
                    if (!writeZTile(&bArr, numBytesWritten, v0, v1, h0, h1,
                                    numValidPixel, zMin, zMax, maxZError))
                        return false;
                }
                if (numBytesWritten != numBytesNeeded)
                    return false;
            }
        }
    }
    return true;
}

bool Lerc1Image::readTiles(double maxZErrorInFile, int numTilesV, int numTilesH,
                           float maxValInImg, Byte *bArr,
                           size_t nRemainingBytes)
{
    if (numTilesV == 0 || numTilesH == 0)
        return false;
    int tileHeight = static_cast<int>(getHeight() / numTilesV);
    int tileWidth = static_cast<int>(getWidth() / numTilesH);
    if (tileWidth <= 0 || tileHeight <= 0)  // Prevent infinite loop
        return false;
    for (int r0 = 0; r0 < getHeight(); r0 += tileHeight)
    {
        int r1 = std::min(getHeight(), r0 + tileHeight);
        for (int c0 = 0; c0 < getWidth(); c0 += tileWidth)
        {
            int c1 = std::min(getWidth(), c0 + tileWidth);
            if (!readZTile(&bArr, nRemainingBytes, r0, r1, c0, c1,
                           maxZErrorInFile, maxValInImg))
                return false;
        }
    }
    return true;
}

bool Lerc1Image::computeZStats(int r0, int r1, int c0, int c1, float &zMin,
                               float &zMax, int &numValidPixel,
                               int &numFinite) const
{
    if (r0 < 0 || c0 < 0 || r1 > getHeight() || c1 > getWidth())
        return false;
    zMin = FLT_MAX;
    zMax = -FLT_MAX;
    numValidPixel = 0;
    numFinite = 0;
    for (int row = r0; row < r1; row++)
        for (int col = c0; col < c1; col++)
            if (IsValid(row, col))
            {
                numValidPixel++;
                float val = (*this)(row, col);
                if (std::isfinite(val))
                    numFinite++;
                else
                    zMin = NAN;  // Serves as a flag, this block will be stored
                if (val < zMin)
                    zMin = val;
                if (val > zMax)
                    zMax = val;
            }
    if (0 == numValidPixel)
        zMin = zMax = 0;
    return true;
}

// Returns true if all floats in the region have exactly the same binary
// representation This makes it usable for non-finite values
bool Lerc1Image::isallsameval(int r0, int r1, int c0, int c1) const
{
    uint32_t val = *reinterpret_cast<const uint32_t *>(&(*this)(r0, c0));
    for (int row = r0; row < r1; row++)
        for (int col = c0; col < c1; col++)
            if (val != *reinterpret_cast<const uint32_t *>(&(*this)(row, col)))
                return false;
    return true;
}

//
// Assumes that buffer at *ppByte is large enough for this particular block
// Returns number of bytes used in numBytes
//
bool Lerc1Image::writeZTile(Byte **ppByte, int &numBytes, int r0, int r1,
                            int c0, int c1, int numValidPixel, float zMin,
                            float zMax, double maxZError) const
{
    Byte *ptr = *ppByte;
    int cntPixel = 0;
    if (numValidPixel == 0 || (zMin == 0 && zMax == 0))
    {
        *(*ppByte)++ = 2;  // mark tile as constant 0
        numBytes = 1;
        return true;
    }
    if (maxZError == 0 || !std::isfinite(zMin) || !std::isfinite(zMax) ||
        ((double)zMax - zMin) / (2 * maxZError) > MAXQ)
    {  // store valid pixels as floating point
        *ptr++ = 0;
        for (int row = r0; row < r1; row++)
            for (int col = c0; col < c1; col++)
                if (IsValid(row, col))
                {
                    memcpy(ptr, &((*this)(row, col)), sizeof(float));
                    ptr += sizeof(float);
                    cntPixel++;
                }
        if (cntPixel != numValidPixel)
            return false;
    }
    else
    {
        Byte flag = 1;               // bitstuffed int array
        double f = 0.5 / maxZError;  // conversion to int multiplier
        unsigned int maxElem = (unsigned int)(((double)zMax - zMin) * f + 0.5);
        if (maxElem == 0)
            flag = 3;               // mark tile as constant zMin
        int n = numBytesFlt(zMin);  // n in { 1, 2, 4 }
        *ptr++ = (flag | bits67[n - 1]);
        ptr = writeFlt(ptr, zMin, n);
        if (maxElem > 0)
        {
            int numBits = nBits(maxElem);
            n = numBytesUInt(numValidPixel);
            // use bits67 to encode the type used for numElements: Byte, ushort, or uint
            // n is in {1, 2, 4}
            // 0xc0 is invalid, will trigger an error
            *ptr++ = static_cast<Byte>(numBits | bits67[n - 1]);
            memcpy(ptr, &numValidPixel, n);
            ptr += n;

            unsigned int acc = 0;  // Accumulator
            int bits = 32;         // Available

            for (int row = r0; row < r1; row++)
                for (int col = c0; col < c1; col++)
                    if (IsValid(row, col))
                    {
                        cntPixel++;
                        auto val = static_cast<unsigned int>(
                            ((double)(*this)(row, col) - zMin) * f + 0.5);

                        if (bits >= numBits)
                        {  // no accumulator overflow
                            acc |= val << (bits - numBits);
                            bits -= numBits;
                        }
                        else
                        {  // accum overflowing
                            acc |= val >> (numBits - bits);
                            memcpy(ptr, &acc, sizeof(acc));
                            ptr += sizeof(acc);
                            bits += 32 - numBits;  // under 32
                            acc = val << bits;
                        }
                    }

            if (cntPixel != numValidPixel)
                return false;

            // There are between 1 and 4 bytes left in the accumulator
            int nbytes = 4;
            while (bits >= 8)
            {
                acc >>= 8;
                bits -= 8;
                nbytes--;
            }
            memcpy(ptr, &acc, nbytes);
            ptr += nbytes;
        }
    }

    numBytes = static_cast<int>(ptr - *ppByte);
    *ppByte = ptr;
    return true;
}

// Read a float encoded as unsigned char, signed short or float
// n is the number of bytes
static float readFlt(const Byte *ptr, int n)
{
    if (n == 4)
    {
        float val;
        memcpy(&val, ptr, 4);
        return val;
    }
    if (n == 2)
    {
        signed short s;
        memcpy(&s, ptr, 2);
        return static_cast<float>(s);
    }
    return static_cast<float>(static_cast<signed char>(*ptr));
}

bool Lerc1Image::readZTile(Byte **ppByte, size_t &nRemainingBytes, int r0,
                           int r1, int c0, int c1, double maxZErrorInFile,
                           float maxZInImg)
{
    Byte *ptr = *ppByte;

    if (nRemainingBytes < 1)
        return false;
    Byte comprFlag = *ptr++;
    nRemainingBytes -= 1;
    // Used if bit-stuffed
    Byte n = stib67[comprFlag >> 6];
    comprFlag &= 63;
    // cppcheck-suppress knownConditionTrueFalse
    if (n == 0 || comprFlag > 3)
        return false;

    if (comprFlag == 2)
    {  // entire zTile is 0
        for (int row = r0; row < r1; row++)
            for (int col = c0; col < c1; col++)
                (*this)(row, col) = 0.0f;
        *ppByte = ptr;
        return true;
    }

    if (comprFlag == 0)
    {  // Stored
        for (int row = r0; row < r1; row++)
            for (int col = c0; col < c1; col++)
                if (IsValid(row, col))
                {
                    if (nRemainingBytes < sizeof(float))
                        return false;
                    memcpy(&(*this)(row, col), ptr, sizeof(float));
                    ptr += sizeof(float);
                    nRemainingBytes -= sizeof(float);
                }
        *ppByte = ptr;
        return true;
    }

    if (nRemainingBytes < n)
        return false;
    float minval = readFlt(ptr, n);
    ptr += n;
    nRemainingBytes -= n;

    if (comprFlag == 3)
    {  // all min val, regardless of mask
        for (int row = r0; row < r1; row++)
            for (int col = c0; col < c1; col++)
                (*this)(row, col) = minval;
        *ppByte = ptr;
        return true;
    }

    idataVec.resize(static_cast<size_t>(r1 - r0) *
                    (c1 - c0));  // max size, gets adjusted
    if (!blockread(&ptr, nRemainingBytes, idataVec))
        return false;

    size_t numValid = idataVec.size();
    size_t i = 0;
    double q = maxZErrorInFile * 2;  // quanta
    for (int row = r0; row < r1; row++)
        for (int col = c0; col < c1; col++)
            if (IsValid(row, col))
            {
                if (i >= numValid)
                    return false;
                (*this)(row, col) = std::min(
                    maxZInImg, static_cast<float>(minval + q * idataVec[i++]));
            }
    if (i != numValid)
        return false;

    *ppByte = ptr;
    return true;
}

NAMESPACE_LERC1_END
