/*
 * s302m.c - the unpacker, the exact inverse of ffmpeg's s302menc.c packing law.
 * See s302m.h for the wire format and why no library is involved.
 */
#include "s302m.h"

#include <string.h>

/* Reverse the bits of one byte. Computed, never tabulated - the table form in ffmpeg
 * (ff_reverse[]) is an optimisation for their hot path, not part of the format. */
static uint8_t rev8(uint8_t x)
{
	x = (uint8_t)(((x & 0xF0) >> 4) | ((x & 0x0F) << 4));
	x = (uint8_t)(((x & 0xCC) >> 2) | ((x & 0x33) << 2));
	x = (uint8_t)(((x & 0xAA) >> 1) | ((x & 0x55) << 1));
	return x;
}

/* Pair stride for the bit depths ST 302M defines: 2 samples of `bits` plus one 8-bit
 * VUCF/padding byte per pair. 16 -> 5, 20 -> 6, 24 -> 7. */
static bool pair_bytes_for(unsigned bits, size_t *out)
{
	switch (bits) {
	case 16:
		*out = 5;
		return true;
	case 20:
		*out = 6;
		return true;
	case 24:
		*out = 7;
		return true;
	default:
		return false;
	}
}

bool s302m_parse_header(const uint8_t *hdr, size_t n, s302m_frame_info_t *info)
{
	if (n < S302M_HEADER_BYTES)
		return false;

	size_t payload = ((size_t)hdr[0] << 8) | hdr[1];
	unsigned channels = (unsigned)(((hdr[2] >> 6) & 0x03) + 1) * 2;
	/* The 16 header bits after the length are: channels (2), channel id (8), bit-depth
	 * code (2), alignment (4) - so the depth code is bits 5:4 of the FOURTH byte, not
	 * of the third. Reading it from the third byte reports 16-bit for a 24-bit stream
	 * and every real frame is then rejected as implausible (it was: 7168 % 5 != 0). */
	unsigned code = (unsigned)((hdr[3] >> 4) & 0x03);

	static const unsigned bits_for_code[4] = { 16, 20, 24, 0 };
	unsigned bits = bits_for_code[code];
	if (bits == 0 || payload == 0)
		return false;

	size_t pair = 0;
	if (!pair_bytes_for(bits, &pair))
		return false;
	if (payload % pair != 0)
		return false;

	/* No size check against n beyond the header itself: whether the payload is
	 * actually present is the caller's business (the splitter waits for it, the
	 * harness checks before unpacking). An implausible length simply produces
	 * samples_per_ch of zero and is rejected downstream. */
	info->channels = channels;
	info->bits = bits;
	info->pair_bytes = pair;
	info->payload_bytes = payload;
	info->samples_per_ch = (unsigned)(payload / pair);
	return true;
}

bool s302m_unpack(const uint8_t *payload, size_t n,
		  const s302m_frame_info_t *info, int32_t *out,
		  s302m_unpack_state_t *state)
{
	if (!state)
		return false;
	/* The node and every fixture this rig uses are 24-bit stereo: the layout below is
	 * that case. Other depths parse (so the splitter can bound the frames) but are
	 * refused here rather than half-decoded. */
	if (info->bits != 24 || info->channels != 2)
		return false;
	if (n < info->payload_bytes)
		return false;

	size_t frames = info->payload_bytes / info->pair_bytes;

	/* Bit 4 of the fourth byte is the VUCF framing marker in the reversed domain, and
	 * it is OR'd in AFTER the nibble is reversed - so it is NOT part of sample 1 and
	 * must be cleared before reversing that byte back. It is only OR'd at every 192nd
	 * pair of the STREAM though: elsewhere that bit is ordinary sample data and
	 * clearing it there would corrupt s1's bit 8 on half the pairs. */
	for (size_t i = 0; i < frames; i++) {
		const uint8_t *o = payload + i * info->pair_bytes;
		bool boundary = (state->pair_index % 192) == 0;
		uint8_t p3 = o[3];

		if (boundary) {
			if (o[3] & S302M_VUCF_BIT) {
				state->vucf++;
				p3 = (uint8_t)(o[3] & ~S302M_VUCF_BIT);
			} else {
				state->vucf_missing++;
			}
		}
		state->pair_index++;

		/* sample 0: three whole bytes, left-aligned (sample << 8) */
		uint32_t s0 = ((uint32_t)rev8(o[0]) << 8) |
			      ((uint32_t)rev8(o[1]) << 16) |
			      ((uint32_t)rev8(o[2]) << 24);
		/* sample 1: nibble (bits 11:8), then three 8-bit groups (19:12, 27:20, 31:28) */
		uint32_t s1 = ((uint32_t)(rev8(p3) & 0xF0) << 4) |
			      ((uint32_t)rev8(o[4]) << 12) |
			      ((uint32_t)rev8(o[5]) << 20) |
			      ((uint32_t)rev8(o[6]) << 28);

		*out++ = (int32_t)s0;
		*out++ = (int32_t)s1;
	}
	return true;
}
