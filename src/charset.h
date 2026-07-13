// charset.h -- UTF-8 <-> flap-byte transcoding for the configurable flap set.
//
// The split-flap protocol carries exactly ONE byte per displayed character (the
// `-<char>` command and the configurable flap set are byte arrays in the module
// firmware). Browsers and JSON, however, speak UTF-8, where a euro sign is 3
// bytes and accented letters are 2. To let people use euro signs and accented
// characters on the flaps, the gateway bridges the two: it maps each UTF-8 code
// point to a single flap byte on the way to the bus, and maps each stored flap
// byte back to UTF-8 on the way to the browser/MQTT.
//
// The single-byte flap encoding is **Windows-1252 (CP1252)**: a superset of
// Latin-1 that puts the euro sign at 0x80 and adds smart quotes, dashes, the
// ellipsis, the trademark sign, etc. in 0x80-0x9F, while keeping every
// Western-European accented letter in 0xA0-0xFF. (CP1252 is the de-facto Windows
// default, so text pasted from common tools maps cleanly.) The encoding is an
// implementation detail of charset.cpp -- callers use the neutral helpers below,
// so swapping the code page later touches only that one file.
//
// Reserved bytes that are never produced for a flap: 0x00, 0xA0 (NBSP), 0xAD
// (soft hyphen), the five CP1252 undefined slots (0x81/0x8D/0x8F/0x90/0x9D) and
// the C0/C1 control ranges. That leaves 216 printable glyphs -- exactly the
// character part of a virtual reel.
//
// 0xFF (y-diaeresis) IS a flap byte here, unlike on the RS-485 gateway. The real
// module firmware reserves 0xFF as its "unused flap" EEPROM sentinel and so can
// never show that glyph; the virtual modules keep an explicit flap-set length
// instead (see vmodule.h) and carry it like any other character.

#ifndef SFGW_CHARSET_H
#define SFGW_CHARSET_H

#include <stddef.h>
#include <stdint.h>

// Transcode a UTF-8 C-string to single flap bytes (Windows-1252). Only safe
// printable glyphs are produced (see the reserved-bytes note above). Control
// characters and any code point with no flap-byte representation are dropped.
// `out` is always NUL-terminated. Returns the number of bytes written. If
// `allMapped` is non-NULL it is set false when any input code point was dropped
// (unrepresentable, invalid, or buffer full).
size_t utf8ToFlap(const char* in, char* out, size_t outSize, bool* allMapped = nullptr);

// True if `b` is a byte the flap protocol may carry: a printable glyph in the
// flap encoding (Windows-1252) that is not a control, an undefined slot, or
// NBSP/soft-hyphen. Use this instead of hand-coded byte ranges so the encoding
// lives in one place. Exactly 216 bytes qualify.
bool isFlapByte(uint8_t b);

// A split-flap reel carries no lowercase -- the leaves are printed in capitals -- so a
// lowercase letter has no flap of its own and must fold to its uppercase one. These two
// answer "is this byte a lowercase LETTER" and "what is its uppercase", for the whole
// CP1252 repertoire, and they are the single definition of that rule: the gateway folds
// with them before transmitting, the virtual module folds with them when resolving a
// frame, and the reel is BUILT by excluding exactly what cp1252IsLower() accepts. Three
// places that must agree, one function that decides.
//
// The traps, all of which have bitten:
//   * 0xF7 is the division sign, not a letter -- its "uppercase" 0xD7 is multiplication;
//   * 0xFF (y-diaeresis) uppercases to 0x9F, NOT 0xDF -- folding it by 0x20 would
//     silently turn it into an eszett;
//   * 0xDF (eszett) has no uppercase in CP1252 at all, so it keeps its own flap;
//   * 0x9A/0x9C/0x9E (s-caron, oe-ligature, z-caron) uppercase to 0x8A/0x8C/0x8E, which
//     is nowhere near a 0x20 offset;
//   * 0xB5 (micro) and 0xAA/0xBA (ordinals) look lowercase but are symbols.
bool    cp1252IsLower(uint8_t b);
uint8_t cp1252ToUpper(uint8_t b);   // returns `b` unchanged if it is not a lowercase letter

// Decode ONE UTF-8 code point from `in`. Returns the bytes consumed (1-4), or 0 if `in`
// does not start a valid sequence. `*cp` receives the code point.
size_t utf8Next(const char* in, uint32_t* cp);

// The CP1252 byte for a Unicode code point, or -1 if it has none. A heart has none --
// which is exactly why the pictograph flaps are reachable only by index.
int cp1252FromUnicode(uint32_t cp);

// Append the UTF-8 encoding of one flap byte to `out` (1-3 bytes, NOT
// NUL-terminated); returns the number of bytes written. `out` must have room for
// at least 3 bytes.
size_t flapByteToUtf8(uint8_t b, char* out);

// Transcode `inLen` flap bytes into a JSON-string-safe UTF-8 sequence in `out`:
//   * `"` and `\` are backslash-escaped
//   * line breaks (`\n`/`\r`) are stripped (structural, not content)
//   * representable flap glyphs (see isFlapByte) become their UTF-8 encoding
//   * every other byte -- control, sentinel, undefined CP1252 slot -- becomes
//     `junk`, or is dropped when `junk` is 0
// `out` is NUL-terminated and never split mid-glyph if it fills: output is
// capped at a glyph/escape boundary. Returns bytes written (excluding the NUL).
// Size `out` for up to 3 UTF-8 bytes (or 2 for an escape) per input byte plus a
// terminator. `junk` defaults to 0 (drop) to preserve existing call sites.
size_t flapToJsonUtf8(const char* in, size_t inLen, char* out, size_t outSize, char junk = 0);

#endif // SFGW_CHARSET_H
