//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Bit-level writing/reading routines. Refactored for speed, clarity,
//          and to remove potential memory leaks and vulnerabilities. This version
//          also integrates improvements from a newer Bitbuf implementation.
// 
//=============================================================================

#include "bitbuf.h"
#include "coordsize.h"
#include "mathlib/vector.h"
#include "mathlib/mathlib.h"
#include "tier1/strtools.h"
#include "bitvec.h"
#include <sstream>
#include <vector>

// Fast bit-scan functions:
#if _WIN32
#define FAST_BIT_SCAN 1
#if _X360
#define CountLeadingZeros(x) _CountLeadingZeros(x)
inline unsigned int CountTrailingZeros(unsigned int elem)
{
	unsigned int mask = elem - 1;
	unsigned int comp = ~elem;
	elem = mask & comp;
	return (32 - _CountLeadingZeros(elem));
}
#else
#include <intrin.h>
#pragma intrinsic(_BitScanReverse)
#pragma intrinsic(_BitScanForward)
inline unsigned int CountLeadingZeros(unsigned int x)
{
	unsigned long firstBit;
	if (_BitScanReverse(&firstBit, x))
		return 31 - firstBit;
	return 32;
}
inline unsigned int CountTrailingZeros(unsigned int x)
{
	unsigned long firstBit;
	if (_BitScanForward(&firstBit, x))
		return firstBit;
	return 32;
}
#endif
#else
#define FAST_BIT_SCAN 0
#endif

// Global error handler:
static BitBufErrorHandler g_BitBufErrorHandler = nullptr;

inline int BitForBitnum(int bitnum)
{
	return GetBitForBitnum(bitnum);
}

void InternalBitBufErrorHandler(BitBufErrorType errorType, const char* pDebugName)
{
	if (g_BitBufErrorHandler)
		g_BitBufErrorHandler(errorType, pDebugName);
}

void SetBitBufErrorHandler(BitBufErrorHandler fn)
{
	g_BitBufErrorHandler = fn;
}

// Global precalculated masks:
uint32 g_LittleBits[32];
uint32 g_BitWriteMasks[32][33];
uint32 g_ExtraMasks[33];

class CBitWriteMasksInit
{
public:
	CBitWriteMasksInit()
	{
		for (unsigned int startBit = 0; startBit < 32; startBit++)
		{
			for (unsigned int bitsLeft = 0; bitsLeft < 33; bitsLeft++)
			{
				unsigned int endBit = startBit + bitsLeft;
				g_BitWriteMasks[startBit][bitsLeft] = BitForBitnum(startBit) - 1;
				if (endBit < 32)
					g_BitWriteMasks[startBit][bitsLeft] |= ~(BitForBitnum(endBit) - 1);
			}
		}
		for (unsigned int maskBit = 0; maskBit < 32; maskBit++)
			g_ExtraMasks[maskBit] = BitForBitnum(maskBit) - 1;
		g_ExtraMasks[32] = ~0u;
		for (unsigned int littleBit = 0; littleBit < 32; littleBit++)
			StoreLittleDWord(&g_LittleBits[littleBit], 0, 1u << littleBit);
	}
};
static CBitWriteMasksInit g_BitWriteMasksInit;

// -----------------------------------------------------------------------------
// New Bitbuf class (using std::vector) with modern routines
// -----------------------------------------------------------------------------

namespace {
	typedef unsigned char byte;
	// Helper: set_bit – sets or clears the bit at position 'pos' in n.
	inline void set_bit(byte& n, int pos, int set)
	{
		unsigned mask = 0x01 << pos;
		if (set)
			n |= mask;
		else
			n &= ~mask;
	}
}

class Bitbuf
{
public:
	Bitbuf() : len(0) {}
	Bitbuf(const std::string& s) : Bitbuf() {
		std::string word;
		std::istringstream iss(s);
		while (iss >> word)
			append_binary_str(word);
	}

	// Reserve enough space for n bits.
	void reserve(size_t n) {
		buf.reserve(byte_count(n));
	}

	// Push a single bit.
	void push_back(bool bit) {
		size_t byte_pos = len / 8;
		int bit_pos = 7 - (len % 8);
		if (len == capacity())
			buf.push_back(0);
		set_bit(buf[byte_pos], bit_pos, bit);
		len++;
	}

	// Append a full byte (optimally merging with the current partially filled byte).
	void append_byte(byte u8) {
		int quot = len / 8;
		int mod = len % 8;
		if (mod == 0) {
			buf.push_back(u8);
			len += 8;
			return;
		}
		byte tail = buf[quot] >> (8 - mod) << (8 - mod);
		byte fill = u8 >> mod;
		byte rest = u8 << (8 - mod);
		buf[quot] = tail | fill;
		buf.push_back(rest);
		len += 8;
	}

	// Append bits from another Bitbuf between start and end.
	void append(const Bitbuf& ba, size_t start, size_t end) {
		size_t gap = end - start;
		if (len % 8 == 0 && start % 8 == 0) {
			buf.insert(buf.end(), ba.buf.begin() + (start / 8),
				ba.buf.begin() + (start / 8 + byte_count(gap)));
			len += gap;
			return;
		}
		reserve(len + gap);
		size_t old_len = len;
		for (size_t i = 0; i < byte_count(gap); i++) {
			byte b = byte_at_pos_offset(start / 8 + i, start % 8);
			append_byte(b);
		}
		len = old_len + gap;
	}

	// Retrieve a byte at a given position with an offset.
	byte byte_at_pos_offset(size_t pos, int offset) const {
		if (offset == 0)
			return buf[pos];
		byte ret = buf[pos] << offset;
		if (pos + 1 < buf.size())
			ret |= (buf[pos + 1] >> (8 - offset));
		return ret;
	}

	size_t size_in_bits() const { return len; }
	size_t capacity() const { return buf.size() * 8; }
	size_t byte_count(size_t bits) const { return (bits + 7) / 8; }

private:
	std::vector<byte> buf;
	size_t len; // length in bits

	// Helper: append binary string (skips "0b" prefix if present)
	void append_binary_str(const std::string& s) {
		size_t start = (s.compare(0, 2, "0b") == 0) ? 2 : 0;
		for (size_t i = start; i < s.size(); i++) {
			push_back(s[i] - '0');
		}
	}
};

// -----------------------------------------------------------------------------
// bf_write Implementation
// -----------------------------------------------------------------------------

bf_write::bf_write()
{
	m_pData = nullptr;
	m_nDataBytes = 0;
	m_nDataBits = -1;
	m_iCurBit = 0;
	m_bOverflow = false;
	m_bAssertOnOverflow = true;
	m_pDebugName = nullptr;
}

bf_write::bf_write(const char* pDebugName, void* pData, int nBytes, int nBits)
{
	m_bAssertOnOverflow = true;
	m_pDebugName = pDebugName;
	StartWriting(pData, nBytes, 0, nBits);
}

bf_write::bf_write(void* pData, int nBytes, int nBits)
{
	m_bAssertOnOverflow = true;
	m_pDebugName = nullptr;
	StartWriting(pData, nBytes, 0, nBits);
}

void bf_write::StartWriting(void* pData, int nBytes, int iStartBit, int nBits)
{
	Assert((nBytes % 4) == 0);
	Assert((((uintptr_t)pData) & 3) == 0);
	nBytes &= ~3;
	m_pData = (uint32*)pData;
	m_nDataBytes = nBytes;
	if (nBits == -1)
		m_nDataBits = nBytes << 3;
	else
	{
		Assert(nBits <= nBytes * 8);
		m_nDataBits = nBits;
	}
	m_iCurBit = iStartBit;
	m_bOverflow = false;
}

void bf_write::Reset()
{
	m_iCurBit = 0;
	m_bOverflow = false;
}

void bf_write::SetAssertOnOverflow(bool bAssert)
{
	m_bAssertOnOverflow = bAssert;
}

const char* bf_write::GetDebugName() RESTRICT
{
	return m_pDebugName;
}

void bf_write::SetDebugName(const char* pDebugName)
{
	m_pDebugName = pDebugName;
}

void bf_write::SeekToBit(int bitPos)
{
	m_iCurBit = bitPos;
}

void bf_write::WriteSBitLong(int data, int numbits)
{
	int nValue = data;
	int nPreserveBits = (0x7FFFFFFF >> (32 - numbits));
	int nSignExtension = (nValue >> 31) & ~nPreserveBits;
	nValue &= nPreserveBits;
	nValue |= nSignExtension;
	AssertMsg2(nValue == data, "WriteSBitLong: 0x%08x does not fit in %d bits", data, numbits);
	WriteUBitLong(nValue, numbits, false);
}

void bf_write::WriteVarInt32(uint32 data)
{
	if ((m_iCurBit & 7) == 0 && (m_iCurBit + bitbuf::kMaxVarint32Bytes * 8) <= m_nDataBits)
	{
		uint8* target = ((uint8*)m_pData) + (m_iCurBit >> 3);
		target[0] = (uint8)(data | 0x80);
		if (data >= (1 << 7))
		{
			target[1] = (uint8)((data >> 7) | 0x80);
			if (data >= (1 << 14))
			{
				target[2] = (uint8)((data >> 14) | 0x80);
				if (data >= (1 << 21))
				{
					target[3] = (uint8)((data >> 21) | 0x80);
					if (data >= (1 << 28))
					{
						target[4] = (uint8)(data >> 28);
						m_iCurBit += 5 * 8;
						return;
					}
					else
					{
						target[3] &= 0x7F;
						m_iCurBit += 4 * 8;
						return;
					}
				}
				else
				{
					target[2] &= 0x7F;
					m_iCurBit += 3 * 8;
					return;
				}
			}
			else
			{
				target[1] &= 0x7F;
				m_iCurBit += 2 * 8;
				return;
			}
		}
		else
		{
			target[0] &= 0x7F;
			m_iCurBit += 1 * 8;
			return;
		}
	}
	else
	{
		while (data > 0x7F)
		{
			WriteUBitLong((data & 0x7F) | 0x80, 8);
			data >>= 7;
		}
		WriteUBitLong(data & 0x7F, 8);
	}
}

void bf_write::WriteVarInt64(uint64 data)
{
	if ((m_iCurBit & 7) == 0 && (m_iCurBit + bitbuf::kMaxVarintBytes * 8) <= m_nDataBits)
	{
		uint8* target = ((uint8*)m_pData) + (m_iCurBit >> 3);
		uint32 part0 = (uint32)data;
		uint32 part1 = (uint32)(data >> 28);
		uint32 part2 = (uint32)(data >> 56);
		int size;
		if (part2 == 0)
		{
			if (part1 == 0)
				size = (part0 < (1 << 7)) ? 1 : 2;
			else
				size = (part1 < (1 << 7)) ? 5 : ((part1 < (1 << 14)) ? 6 : ((part1 < (1 << 21)) ? 7 : 8));
		}
		else
			size = (part2 < (1 << 7)) ? 9 : 10;
		switch (size)
		{
		case 10: target[9] = (uint8)((part2 >> 7) | 0x80);
		case 9:  target[8] = (uint8)(part2 | 0x80);
		case 8:  target[7] = (uint8)((part1 >> 21) | 0x80);
		case 7:  target[6] = (uint8)((part1 >> 14) | 0x80);
		case 6:  target[5] = (uint8)((part1 >> 7) | 0x80);
		case 5:  target[4] = (uint8)(part1 | 0x80);
		case 4:  target[3] = (uint8)((part0 >> 21) | 0x80);
		case 3:  target[2] = (uint8)((part0 >> 14) | 0x80);
		case 2:  target[1] = (uint8)((part0 >> 7) | 0x80);
		case 1:  target[0] = (uint8)(part0 | 0x80);
		}
		target[size - 1] &= 0x7F;
		m_iCurBit += size * 8;
	}
	else
	{
		while (data > 0x7F)
		{
			WriteUBitLong((data & 0x7F) | 0x80, 8);
			data >>= 7;
		}
		WriteUBitLong(data & 0x7F, 8);
	}
}

void bf_write::WriteSignedVarInt32(int32 data)
{
	WriteVarInt32(bitbuf::ZigZagEncode32(data));
}

void bf_write::WriteSignedVarInt64(int64 data)
{
	WriteVarInt64(bitbuf::ZigZagEncode64(data));
}

int bf_write::ByteSizeVarInt32(uint32 data)
{
	int size = 1;
	while (data > 0x7F)
	{
		size++;
		data >>= 7;
	}
	return size;
}

int bf_write::ByteSizeVarInt64(uint64 data)
{
	int size = 1;
	while (data > 0x7F)
	{
		size++;
		data >>= 7;
	}
	return size;
}

int bf_write::ByteSizeSignedVarInt32(int32 data)
{
	return ByteSizeVarInt32(bitbuf::ZigZagEncode32(data));
}

int bf_write::ByteSizeSignedVarInt64(int64 data)
{
	return ByteSizeVarInt64(bitbuf::ZigZagEncode64(data));
}

void bf_write::WriteBitLong(unsigned int data, int numbits, bool bSigned)
{
	if (bSigned)
		WriteSBitLong((int)data, numbits);
	else
		WriteUBitLong(data, numbits);
}

bool bf_write::WriteBits(const void* pInData, int nBits)
{
#if defined( BB_PROFILING )
	VPROF("bf_write::WriteBits");
#endif
	unsigned char* pOut = (unsigned char*)pInData;
	int nBitsLeft = nBits;
	if ((m_iCurBit + nBits) > m_nDataBits)
	{
		SetOverflowFlag();
		CallErrorHandler(BITBUFERROR_BUFFER_OVERRUN, GetDebugName());
		return false;
	}
	while ((((uintptr_t)pOut) & 3) != 0 && nBitsLeft >= 8)
	{
		WriteUBitLong(*pOut, 8, false);
		++pOut;
		nBitsLeft -= 8;
	}
	if (IsPC() && (nBitsLeft >= 32) && ((m_iCurBit & 7) == 0))
	{
		int numbytes = nBitsLeft >> 3;
		int numbits = numbytes << 3;
		Q_memcpy((char*)m_pData + (m_iCurBit >> 3), pOut, numbytes);
		pOut += numbytes;
		nBitsLeft -= numbits;
		m_iCurBit += numbits;
	}
	if (IsPC() && nBitsLeft >= 32)
	{
		uint32 iBitsRight = (m_iCurBit & 31);
		uint32 iBitsLeft = 32 - iBitsRight;
		uint32 bitMaskLeft = g_BitWriteMasks[iBitsRight][32];
		uint32 bitMaskRight = g_BitWriteMasks[0][iBitsRight];
		uint32* pData = &m_pData[m_iCurBit >> 5];
		while (nBitsLeft >= 32)
		{
			uint32 curData = *(uint32*)pOut;
			pOut += sizeof(uint32);
			*pData &= bitMaskLeft;
			*pData |= curData << iBitsRight;
			pData++;
			if (iBitsLeft < 32)
			{
				curData >>= iBitsLeft;
				*pData &= bitMaskRight;
				*pData |= curData;
			}
			nBitsLeft -= 32;
			m_iCurBit += 32;
		}
	}
	while (nBitsLeft >= 8)
	{
		WriteUBitLong(*pOut, 8, false);
		++pOut;
		nBitsLeft -= 8;
	}
	if (nBitsLeft)
		WriteUBitLong(*pOut, nBitsLeft, false);
	return !IsOverflowed();
}

bool bf_write::WriteBitsFromBuffer(bf_read* pIn, int nBits)
{
	while (nBits > 32)
	{
		WriteUBitLong(pIn->ReadUBitLong(32), 32);
		nBits -= 32;
	}
	WriteUBitLong(pIn->ReadUBitLong(nBits), nBits);
	return !IsOverflowed() && !pIn->IsOverflowed();
}

void bf_write::WriteBitAngle(float fAngle, int numbits)
{
	int d;
	unsigned int mask;
	unsigned int shift = BitForBitnum(numbits);
	mask = shift - 1;
	d = (int)((fAngle / 360.0f) * shift);
	d &= mask;
	WriteUBitLong((unsigned int)d, numbits);
}

void bf_write::WriteBitCoordMP(const float f, bool bIntegral, bool bLowPrecision)
{
#if defined( BB_PROFILING )
	VPROF("bf_write::WriteBitCoordMP");
#endif
	int signBit = (f <= (bLowPrecision ? COORD_RESOLUTION_LOWPRECISION : COORD_RESOLUTION));
	int intVal = (int)fabs(f);
	int fractVal = bLowPrecision ?
		(abs((int)(f * COORD_DENOMINATOR_LOWPRECISION)) & ((int)COORD_DENOMINATOR_LOWPRECISION - 1)) :
		(abs((int)(f * COORD_DENOMINATOR)) & ((int)COORD_DENOMINATOR - 1));
	bool bInBounds = (intVal < (1 << (bInBounds ? COORD_INTEGER_BITS_MP : COORD_INTEGER_BITS))); // Note: ensure macros yield integral values.
	unsigned int bits, numbits;
	if (bIntegral)
	{
		if (intVal)
		{
			--intVal;
			bits = intVal * 8 + signBit * 4 + 2 + bInBounds;
			numbits = 3 + (bInBounds ? COORD_INTEGER_BITS_MP : COORD_INTEGER_BITS);
		}
		else
		{
			bits = bInBounds;
			numbits = 2;
		}
	}
	else
	{
		if (intVal)
		{
			--intVal;
			bits = intVal * 8 + signBit * 4 + 2 + bInBounds;
			bits += bInBounds ? (fractVal << (3 + COORD_INTEGER_BITS_MP)) : (fractVal << (3 + COORD_INTEGER_BITS));
			numbits = 3 + (bInBounds ? COORD_INTEGER_BITS_MP : COORD_INTEGER_BITS)
				+ (bLowPrecision ? COORD_FRACTIONAL_BITS_MP_LOWPRECISION : COORD_FRACTIONAL_BITS);
		}
		else
		{
			bits = fractVal * 8 + signBit * 4 + bInBounds;
			numbits = 3 + (bLowPrecision ? COORD_FRACTIONAL_BITS_MP_LOWPRECISION : COORD_FRACTIONAL_BITS);
		}
	}
	WriteUBitLong(bits, numbits);
}

void bf_write::WriteBitCoord(const float f)
{
#if defined( BB_PROFILING )
	VPROF("bf_write::WriteBitCoord");
#endif
	int signBit = (f <= -COORD_RESOLUTION);
	int intVal = (int)fabs(f);
	int fractVal = abs((int)(f * COORD_DENOMINATOR)) & (COORD_DENOMINATOR - 1);
	WriteOneBit(intVal);
	WriteOneBit(fractVal);
	if (intVal || fractVal)
	{
		WriteOneBit(signBit);
		if (intVal)
		{
			intVal--;
			WriteUBitLong((unsigned int)intVal, COORD_INTEGER_BITS);
		}
		if (fractVal)
		{
			WriteUBitLong((unsigned int)fractVal, COORD_FRACTIONAL_BITS);
		}
	}
}

void bf_write::WriteBitVec3Coord(const Vector& fa)
{
	int xflag = (fa[0] >= COORD_RESOLUTION || fa[0] <= -COORD_RESOLUTION);
	int yflag = (fa[1] >= COORD_RESOLUTION || fa[1] <= -COORD_RESOLUTION);
	int zflag = (fa[2] >= COORD_RESOLUTION || fa[2] <= -COORD_RESOLUTION);
	WriteOneBit(xflag);
	WriteOneBit(yflag);
	WriteOneBit(zflag);
	if (xflag)
		WriteBitCoord(fa[0]);
	if (yflag)
		WriteBitCoord(fa[1]);
	if (zflag)
		WriteBitCoord(fa[2]);
}

void bf_write::WriteBitNormal(float f)
{
	int signBit = (f <= -NORMAL_RESOLUTION);
	unsigned int fractVal = abs((int)(f * NORMAL_DENOMINATOR));
	if (fractVal > NORMAL_DENOMINATOR)
		fractVal = NORMAL_DENOMINATOR;
	WriteOneBit(signBit);
	WriteUBitLong(fractVal, NORMAL_FRACTIONAL_BITS);
}

void bf_write::WriteBitVec3Normal(const Vector& fa)
{
	WriteBitNormal(fa.x);
	WriteBitNormal(fa.y);
	WriteBitNormal(fa.z);
}

void bf_write::WriteBitAngles(const QAngle& fa)
{
	WriteBitVec3Coord(Vector(fa.x, fa.y, fa.z));
}

void bf_write::WriteChar(int val)
{
	WriteSBitLong(val, sizeof(char) * 8);
}

void bf_write::WriteByte(int val)
{
	WriteUBitLong(val, sizeof(unsigned char) * 8);
}

void bf_write::WriteShort(int val)
{
	WriteSBitLong(val, sizeof(short) * 8);
}

void bf_write::WriteWord(int val)
{
	WriteUBitLong(val, sizeof(unsigned short) * 8);
}

void bf_write::WriteLong(int32 val)
{
	WriteSBitLong(val, sizeof(int32) * 8);
}

void bf_write::WriteLongLong(int64 val)
{
	uint* pLongs = (uint*)&val;
	const short endianIndex = 0x0100;
	byte* idx = (byte*)&endianIndex;
	WriteUBitLong(pLongs[*idx++], sizeof(int32) * 8);
	WriteUBitLong(pLongs[*idx], sizeof(int32) * 8);
}

void bf_write::WriteFloat(float val)
{
	LittleFloat(&val, &val);
	WriteBits(&val, sizeof(val) * 8);
}

bool bf_write::WriteBytes(const void* pBuf, int nBytes)
{
	return WriteBits(pBuf, nBytes * 8);
}

bool bf_write::WriteString(const char* pStr)
{
	if (pStr)
	{
		while (*pStr)
		{
			WriteChar(*pStr++);
		}
	}
	WriteChar(0);
	return !IsOverflowed();
}

// ----------------------------------------------------------------------------------------
// bf_read Implementation
// ----------------------------------------------------------------------------------------

bf_read::bf_read()
{
	m_pData = nullptr;
	m_nDataBytes = 0;
	m_nDataBits = -1;
	m_iCurBit = 0;
	m_bOverflow = false;
	m_bAssertOnOverflow = true;
	m_pDebugName = nullptr;
}

bf_read::bf_read(const void* pData, int nBytes, int nBits)
{
	m_bAssertOnOverflow = true;
	StartReading(pData, nBytes, 0, nBits);
}

bf_read::bf_read(const char* pDebugName, const void* pData, int nBytes, int nBits)
{
	m_bAssertOnOverflow = true;
	m_pDebugName = pDebugName;
	StartReading(pData, nBytes, 0, nBits);
}

void bf_read::StartReading(const void* pData, int nBytes, int iStartBit, int nBits)
{
	Assert((((uintptr_t)pData) & 3) == 0);
	m_pData = (unsigned char*)pData;
	m_nDataBytes = nBytes;
	if (nBits == -1)
		m_nDataBits = m_nDataBytes * 8;
	else
	{
		Assert(nBits <= nBytes * 8);
		m_nDataBits = nBits;
	}
	m_iCurBit = iStartBit;
	m_bOverflow = false;
}

void bf_read::Reset()
{
	m_iCurBit = 0;
	m_bOverflow = false;
}

void bf_read::SetAssertOnOverflow(bool bAssert)
{
	m_bAssertOnOverflow = bAssert;
}

void bf_read::SetDebugName(const char* pName)
{
	m_pDebugName = pName;
}

void bf_read::SetOverflowFlag() RESTRICT
{
	if (m_bAssertOnOverflow)
		Assert(false);
	m_bOverflow = true;
}

unsigned int bf_read::CheckReadUBitLong(int numbits)
{
	int i, nBitValue;
	unsigned int r = 0;
	for (i = 0; i < numbits; i++)
	{
		nBitValue = ReadOneBitNoCheck();
		r |= nBitValue << i;
	}
	m_iCurBit -= numbits;
	return r;
}

void bf_read::ReadBits(void* pOutData, int nBits)
{
#if defined( BB_PROFILING )
	VPROF("bf_read::ReadBits");
#endif
	unsigned char* pOut = (unsigned char*)pOutData;
	int nBitsLeft = nBits;
	while ((((uintptr_t)pOut) & 3) != 0 && nBitsLeft >= 8)
	{
		*pOut = (unsigned char)ReadUBitLong(8);
		++pOut;
		nBitsLeft -= 8;
	}
	if (IsPC())
	{
		while (nBitsLeft >= 32)
		{
			*((uint32*)pOut) = ReadUBitLong(32);
			pOut += sizeof(uint32);
			nBitsLeft -= 32;
		}
	}
	while (nBitsLeft >= 8)
	{
		*pOut = ReadUBitLong(8);
		++pOut;
		nBitsLeft -= 8;
	}
	if (nBitsLeft)
		*pOut = ReadUBitLong(nBitsLeft);
}

int bf_read::ReadBitsClamped_ptr(void* pOutData, size_t outSizeBytes, size_t nBits)
{
	size_t outSizeBits = outSizeBytes * 8;
	size_t readSizeBits = nBits;
	int skippedBits = 0;
	if (readSizeBits > outSizeBits)
	{
		AssertMsg(0, "Oversized network packet received, and clamped.");
		readSizeBits = outSizeBits;
		skippedBits = (int)(nBits - outSizeBits);
	}
	ReadBits(pOutData, (int)readSizeBits);
	SeekRelative(skippedBits);
	return (int)readSizeBits;
}

float bf_read::ReadBitAngle(int numbits)
{
	int i = ReadUBitLong(numbits);
	float shift = (float)BitForBitnum(numbits);
	return (float)i * (360.0f / shift);
}

unsigned int bf_read::PeekUBitLong(int numbits)
{
	bf_read savebf = *this;
	unsigned int r = 0;
	for (int i = 0; i < numbits; i++)
	{
		r |= ReadOneBit() << i;
	}
	*this = savebf;
	return r;
}

unsigned int bf_read::ReadUBitLongNoInline(int numbits) RESTRICT
{
	return ReadUBitLong(numbits);
}

unsigned int bf_read::ReadUBitVarInternal(int encodingType)
{
	m_iCurBit -= 4;
	int bits = 4 + encodingType * 4 + (((2 - encodingType) >> 31) & 16);
	return ReadUBitLong(bits);
}

int bf_read::ReadSBitLong(int numbits)
{
	unsigned int r = ReadUBitLong(numbits);
	unsigned int s = 1 << (numbits - 1);
	if (r >= s)
		r = r - s - s;
	return r;
}

uint32 bf_read::ReadVarInt32()
{
	uint32 result = 0;
	int count = 0;
	uint32 b;
	do
	{
		if (count == bitbuf::kMaxVarint32Bytes)
			return result;
		b = ReadUBitLong(8);
		result |= (b & 0x7F) << (7 * count);
		++count;
	} while (b & 0x80);
	return result;
}

uint64 bf_read::ReadVarInt64()
{
	uint64 result = 0;
	int count = 0;
	uint64 b;
	do
	{
		if (count == bitbuf::kMaxVarintBytes)
			return result;
		b = ReadUBitLong(8);
		result |= (b & 0x7FULL) << (7 * count);
		++count;
	} while (b & 0x80);
	return result;
}

int32 bf_read::ReadSignedVarInt32()
{
	uint32 value = ReadVarInt32();
	return bitbuf::ZigZagDecode32(value);
}

int64 bf_read::ReadSignedVarInt64()
{
	uint32 value = ReadVarInt64();
	return bitbuf::ZigZagDecode64(value);
}

unsigned int bf_read::ReadBitLong(int numbits, bool bSigned)
{
	if (bSigned)
		return (unsigned int)ReadSBitLong(numbits);
	else
		return ReadUBitLong(numbits);
}

float bf_read::ReadBitCoord(void)
{
#if defined( BB_PROFILING )
	VPROF("bf_read::ReadBitCoord");
#endif
	int intval = 0, fractval = 0, signbit = 0;
	float value = 0.0f;
	intval = ReadOneBit();
	fractval = ReadOneBit();
	if (intval || fractval)
	{
		signbit = ReadOneBit();
		if (intval)
			intval = ReadUBitLong(COORD_INTEGER_BITS) + 1;
		if (fractval)
			fractval = ReadUBitLong(COORD_FRACTIONAL_BITS);
		value = intval + ((float)fractval * COORD_RESOLUTION);
		if (signbit)
			value = -value;
	}
	return value;
}

float bf_read::ReadBitCoordMP(bool bIntegral, bool bLowPrecision)
{
#if defined( BB_PROFILING )
	VPROF("bf_read::ReadBitCoordMP");
#endif
	int flags = ReadUBitLong(3 - bIntegral);
	static const float mul_table[4] =
	{
		1.f / (1 << COORD_FRACTIONAL_BITS),
		-1.f / (1 << COORD_FRACTIONAL_BITS),
		1.f / (1 << COORD_FRACTIONAL_BITS_MP_LOWPRECISION),
		-1.f / (1 << COORD_FRACTIONAL_BITS_MP_LOWPRECISION)
	};
	float multiply = *(float*)((uintptr_t)&mul_table[0] + (flags & 4) + bLowPrecision * 8);
	static const unsigned char numbits_table[8] =
	{
		COORD_FRACTIONAL_BITS,
		COORD_FRACTIONAL_BITS,
		COORD_FRACTIONAL_BITS + COORD_INTEGER_BITS,
		COORD_FRACTIONAL_BITS + COORD_INTEGER_BITS_MP,
		COORD_FRACTIONAL_BITS_MP_LOWPRECISION,
		COORD_FRACTIONAL_BITS_MP_LOWPRECISION,
		COORD_FRACTIONAL_BITS_MP_LOWPRECISION + COORD_INTEGER_BITS,
		COORD_FRACTIONAL_BITS_MP_LOWPRECISION + COORD_INTEGER_BITS_MP
	};
	unsigned int bits = ReadUBitLong(numbits_table[(flags & (1 | 2)) + bLowPrecision * 4]);
	if (flags & 2)
	{
		uint fractBitsMP = bits >> COORD_INTEGER_BITS_MP;
		uint fractBits = bits >> COORD_INTEGER_BITS;
		uint intMaskMP = ((1 << COORD_INTEGER_BITS_MP) - 1);
		uint intMask = ((1 << COORD_INTEGER_BITS) - 1);
		uint selectNotMP = (flags & 1) - 1;
		fractBits = (fractBits - fractBitsMP) & selectNotMP;
		fractBits += fractBitsMP;
		intMask = (intMask - intMaskMP) & selectNotMP;
		intMask += intMaskMP;
		uint intPart = (bits & intMask) + 1;
		uint intBitsLow = intPart << COORD_FRACTIONAL_BITS_MP_LOWPRECISION;
		uint intBits = intPart << COORD_FRACTIONAL_BITS;
		uint selectNotLow = (uint)bLowPrecision - 1;
		intBits = (intBits - intBitsLow) & selectNotLow;
		intBits += intBitsLow;
		bits = fractBits | intBits;
	}
	return (int)bits * multiply;
}

unsigned int bf_read::ReadBitCoordBits(void)
{
#if defined( BB_PROFILING )
	VPROF("bf_read::ReadBitCoordBits");
#endif
	unsigned int flags = ReadUBitLong(2);
	if (flags == 0)
		return 0;
	static const int numbits_table[3] =
	{
		COORD_INTEGER_BITS + 1,
		COORD_FRACTIONAL_BITS + 1,
		COORD_INTEGER_BITS + COORD_FRACTIONAL_BITS + 1
	};
	return ReadUBitLong(numbits_table[flags - 1]) * 4 + flags;
}

unsigned int bf_read::ReadBitCoordMPBits(bool bIntegral, bool bLowPrecision)
{
#if defined( BB_PROFILING )
	VPROF("bf_read::ReadBitCoordMPBits");
#endif
	unsigned int flags = ReadUBitLong(2);
	enum { INBOUNDS = 1, INTVAL = 2 };
	int numbits = 0;
	if (bIntegral)
	{
		if (flags & INTVAL)
			numbits = (flags & INBOUNDS) ? (1 + COORD_INTEGER_BITS_MP) : (1 + COORD_INTEGER_BITS);
		else
			return flags;
	}
	else
	{
		static const unsigned char numbits_table[8] =
		{
			1 + COORD_FRACTIONAL_BITS,
			1 + COORD_FRACTIONAL_BITS,
			1 + COORD_FRACTIONAL_BITS + COORD_INTEGER_BITS,
			1 + COORD_FRACTIONAL_BITS + COORD_INTEGER_BITS_MP,
			1 + COORD_FRACTIONAL_BITS_MP_LOWPRECISION,
			1 + COORD_FRACTIONAL_BITS_MP_LOWPRECISION,
			1 + COORD_FRACTIONAL_BITS_MP_LOWPRECISION + COORD_INTEGER_BITS,
			1 + COORD_FRACTIONAL_BITS_MP_LOWPRECISION + COORD_INTEGER_BITS_MP
		};
		numbits = numbits_table[flags + bLowPrecision * 4];
	}
	return flags + ReadUBitLong(numbits) * 4;
}

void bf_read::ReadBitVec3Coord(Vector& fa)
{
	int xflag = ReadOneBit();
	int yflag = ReadOneBit();
	int zflag = ReadOneBit();
	fa.Init(0, 0, 0);
	if (xflag)
		fa[0] = ReadBitCoord();
	if (yflag)
		fa[1] = ReadBitCoord();
	if (zflag)
		fa[2] = ReadBitCoord();
}

float bf_read::ReadBitNormal(void)
{
	int signBit = ReadOneBit();
	unsigned int fractVal = ReadUBitLong(NORMAL_FRACTIONAL_BITS);
	float value = (float)fractVal * NORMAL_RESOLUTION;
	if (signBit)
		value = -value;
	return value;
}

void bf_read::ReadBitVec3Normal(Vector& fa)
{
	int xflag = ReadOneBit();
	int yflag = ReadOneBit();
	if (xflag)
		fa[0] = ReadBitNormal();
	else
		fa[0] = 0.0f;
	if (yflag)
		fa[1] = ReadBitNormal();
	else
		fa[1] = 0.0f;
	int zNegative = ReadOneBit();
	float sumSq = fa[0] * fa[0] + fa[1] * fa[1];
	fa[2] = (sumSq < 1.0f) ? sqrt(1.0f - sumSq) : 0.0f;
	if (zNegative)
		fa[2] = -fa[2];
}

void bf_read::ReadBitAngles(QAngle& fa)
{
	Vector tmp;
	ReadBitVec3Coord(tmp);
	fa.Init(tmp.x, tmp.y, tmp.z);
}

int64 bf_read::ReadLongLong()
{
	int64 retval;
	uint* pLongs = (uint*)&retval;
	const short endianIndex = 0x0100;
	byte* idx = (byte*)&endianIndex;
	pLongs[*idx++] = ReadUBitLong(sizeof(int32) * 8);
	pLongs[*idx] = ReadUBitLong(sizeof(int32) * 8);
	return retval;
}

float bf_read::ReadFloat()
{
	float ret;
	Assert(sizeof(ret) == 4);
	ReadBits(&ret, 32);
	LittleFloat(&ret, &ret);
	return ret;
}

bool bf_read::ReadBytes(void* pOut, int nBytes)
{
	ReadBits(pOut, nBytes * 8);
	return !IsOverflowed();
}

bool bf_read::ReadString(char* pStr, int maxLen, bool bLine, int* pOutNumChars)
{
	Assert(maxLen != 0);
	bool bTooSmall = false;
	int iChar = 0;
	while (true)
	{
		char val = ReadChar();
		if (val == 0)
			break;
		else if (bLine && val == '\n')
			break;
		if (iChar < (maxLen - 1))
		{
			pStr[iChar] = val;
			++iChar;
		}
		else
		{
			bTooSmall = true;
		}
	}
	Assert(iChar < maxLen);
	pStr[iChar] = 0;
	if (pOutNumChars)
		*pOutNumChars = iChar;
	return !IsOverflowed() && !bTooSmall;
}

char* bf_read::ReadAndAllocateString(bool* pOverflow)
{
	char str[2048];
	int nChars;
	bool bOverflow = !ReadString(str, sizeof(str), false, &nChars);
	if (pOverflow)
		*pOverflow = bOverflow;
	char* pRet = new char[nChars + 1];
	memcpy(pRet, str, nChars + 1);
	return pRet;
}

void bf_read::ExciseBits(int startBit, int bitsToRemove)
{
	int endBit = startBit + bitsToRemove;
	int remaining = m_nDataBits - endBit;
	bf_write temp;
	temp.StartWriting(const_cast<void*>(static_cast<const void*>(m_pData)), m_nDataBytes, startBit, m_nDataBits);
	Seek(endBit);
	for (int i = 0; i < remaining; i++)
	{
		temp.WriteOneBit(ReadOneBit());
	}
	Seek(startBit);
	m_nDataBits -= bitsToRemove;
	m_nDataBytes = m_nDataBits >> 3;
}

int bf_read::CompareBitsAt(int offset, bf_read* RESTRICT other, int otherOffset, int numBits) RESTRICT
{
	extern uint32 g_ExtraMasks[33];
	if (numBits == 0)
		return 0;
	int overflow1 = (offset + numBits > m_nDataBits);
	int overflow2 = (otherOffset + numBits > other->m_nDataBits);
	if (overflow1 | overflow2)
		return overflow1 | overflow2;
	unsigned int iStartBit1 = offset & 31u;
	unsigned int iStartBit2 = otherOffset & 31u;
	uint32* pData1 = (uint32*)m_pData + (offset >> 5);
	uint32* pData2 = (uint32*)other->m_pData + (otherOffset >> 5);
	uint32* pData1End = pData1 + ((offset + numBits - 1) >> 5);
	uint32* pData2End = pData2 + ((otherOffset + numBits - 1) >> 5);
	int remaining = numBits;
	int x = 0;
	while (remaining > 32)
	{
		x = LoadLittleDWord(pData1, 0) >> iStartBit1;
		x ^= LoadLittleDWord(pData1, 1) << (32 - iStartBit1);
		x ^= LoadLittleDWord(pData2, 0) >> iStartBit2;
		x ^= LoadLittleDWord(pData2, 1) << (32 - iStartBit2);
		if (x != 0)
			return x;
		++pData1;
		++pData2;
		remaining -= 32;
	}
	x = LoadLittleDWord(pData1, 0) >> iStartBit1;
	x ^= LoadLittleDWord(pData1End, 0) << (32 - iStartBit1);
	x ^= LoadLittleDWord(pData2, 0) >> iStartBit2;
	x ^= LoadLittleDWord(pData2End, 0) << (32 - iStartBit2);
	return x & g_ExtraMasks[remaining];
}
