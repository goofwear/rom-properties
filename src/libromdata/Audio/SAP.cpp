/***************************************************************************
 * ROM Properties Page shell extension. (libromdata)                       *
 * SAP.cpp: Atari 8-bit SAP audio reader.                                  *
 *                                                                         *
 * Copyright (c) 2018 by David Korth.                                      *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU General Public License as published by the   *
 * Free Software Foundation; either version 2 of the License, or (at your  *
 * option) any later version.                                              *
 *                                                                         *
 * This program is distributed in the hope that it will be useful, but     *
 * WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License       *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.   *
 ***************************************************************************/

// Reference: http://asap.sourceforge.net/sap-format.html
// NOTE: The header format is plaintext, so we don't have a structs file.

#include "SAP.hpp"
#include "librpbase/RomData_p.hpp"

// librpbase
#include "librpbase/common.h"
#include "librpbase/byteswap.h"
#include "librpbase/TextFuncs.hpp"
#include "librpbase/file/IRpFile.hpp"
#include "libi18n/i18n.h"
using namespace LibRpBase;

// C includes. (C++ namespace)
#include "librpbase/ctypex.h"
#include <cassert>
#include <cerrno>
#include <cstring>
#include <ctime>

// C++ includes.
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
using std::string;
using std::unique_ptr;
using std::unordered_map;
using std::vector;

namespace LibRomData {

ROMDATA_IMPL(SAP)

class SAPPrivate : public RomDataPrivate
{
	public:
		SAPPrivate(SAP *q, IRpFile *file);

	private:
		typedef RomDataPrivate super;
		RP_DISABLE_COPY(SAPPrivate)

	public:
		// Parsed tags.
		struct TagData {
			bool tags_read;		// True if tags were read successfully.

			string author;		// Author.
			string name;		// Song title.
			string date;		// Date. TODO: Disambiguate year vs. date.
			uint8_t songs;		// Number of songs in the file. (Default is 1)
			uint8_t def_song;	// Default song. (zero-based; default is 0)

			// TODO: Use a bitfield for flags?
			bool ntsc;		// True if NTSC tag is present.
			bool stereo;		// True if STEREO tag is present. (dual POKEY)

			char type;		// B, C, D, S
			uint16_t fastplay;	// Number of scanlines between calls of the player routine.
						// Default is one frame: 312 lines for PAL, 262 lines for NTSC.
			uint16_t init_addr;	// Init address. (Required for Types B, D, and S; invalid for others.)
			uint16_t music_addr;	// Music data address. (Required for Type C; invalid for others.)
			uint16_t player_addr;	// Player address.
			uint16_t covox_addr;	// COVOX hardware address. (If not specified, set to 0.)

			// TODO: "TIME" tags for each song.

			TagData() : tags_read(false), songs(1), def_song(0), ntsc(false), stereo(false)
				  , type('\0'), fastplay(0), init_addr(0), music_addr(0), player_addr(0)
				  , covox_addr(0) { }
		};

		/**
		 * Parse the tags from the open SAP file.
		 * @return TagData object.
		 */
		TagData parseTags(void);
};

/** SAPPrivate **/

SAPPrivate::SAPPrivate(SAP *q, IRpFile *file)
	: super(q, file)
{ }

/**
 * Parse the tags from the open SAP file.
 * @return TagData object.
 */
SAPPrivate::TagData SAPPrivate::parseTags(void)
{
	TagData tags;

	// Read up to 4 KB from the beginning of the file.
	// TODO: Support larger headers?
	unique_ptr<char[]> header(new char[4096+1]);
	size_t sz = file->seekAndRead(0, header.get(), 4096);
	if (sz < 6) {
		// Not enough data for "SAP\n" and 0xFFFF.
		return tags;
	}
	header[sz] = 0;	// ensure NULL-termination for strtok_r()

	// Verify the header.
	// NOTE: SAP is defined as using CRLF line endings,
	// but we'll allow LF line endings too.
	char *str;
	if (!memcmp(header.get(), "SAP\r\n", 5)) {
		// Standard SAP header.
		str = &header[5];
	} else if (!memcmp(header.get(), "SAP\n", 4)) {
		// SAP header with Unix line endings.
		str = &header[4];
	} else {
		// Invalid header.
		return tags;
	}

	// Parse each line.
	char *saveptr;
	for (char *token = strtok_r(str, "\n", &saveptr);
	     token != nullptr; token = strtok_r(nullptr, "\n", &saveptr))
	{
		// Check if this is the end of the tags.
		if (token[0] == (char)0xFF) {
			// End of tags.
			break;
		}

		// Find the first space. This delimits the keyword.
		char *space = strchr(token, ' ');
		char *params;
		if (space) {
			// Found a space. Zero it out so we can check the keyword.
			params = space+1;
			*space = '\0';

			// Check for the first non-space character in the parameter.
			for (; *params != '\0' && ISSPACE(*params); params++) { }
			if (*params == '\0') {
				// No non-space characters.
				// Ignore the parameter.
				params = nullptr;
			}
		} else {
			// No space. This means no parameters are present.
			params = nullptr;

			// Remove '\r' if it's present.
			char *cr = strchr(token, '\r');
			if (cr) {
				*cr = '\0';
			}
		}

		// Check the keyword.
		// NOTE: Official format uses uppercase tags, but we'll allow mixed-case.
		// NOTE: String encoding is the common subset of ASCII and ATASCII.
		// TODO: ascii_to_utf8()?
		// TODO: Check for duplicate keywords?
		enum KeywordType {
			KT_UNKNOWN = 0,

			KT_BOOL,	// bool: Keyword presence sets the value to true.
			KT_UINT16_DEC,	// uint16 dec: Parameter is a decimal uint16_t.
			KT_UINT16_HEX,	// uint16 hex: Parameter is a hexadecimal uint16_t.
			KT_CHAR,	// char: Parameter is a single character.
			KT_STRING,	// string: Parameter is a string, enclosed in double-quotes.
			KT_TIME_LOOP,	// time+loop: Parameter is a duration, plus an optional "LOOP" setting.
		};
		struct KeywordDef {
			const char *keyword;	// Keyword, e.g. "AUTHOR".
			KeywordType type;	// Keyword type.
			void *ptr;		// Data pointer.
		};

		const KeywordDef kwds[] = {
			{"AUTHOR",	KT_STRING,	&tags.author},
			{"NAME",	KT_STRING,	&tags.name},
			{"DATE",	KT_STRING,	&tags.date},
			{"SONGS",	KT_UINT16_DEC,	&tags.songs},
			{"DEFSONG",	KT_UINT16_DEC,	&tags.def_song},
			{"STEREO",	KT_BOOL,	&tags.stereo},
			{"NTSC",	KT_BOOL,	&tags.ntsc},
			{"TYPE",	KT_CHAR,	&tags.type},
			{"FASTPLAY",	KT_UINT16_DEC,	&tags.fastplay},
			{"INIT",	KT_UINT16_HEX,	&tags.init_addr},
			{"MUSIC",	KT_UINT16_HEX,	&tags.music_addr},
			{"PLAYER",	KT_UINT16_HEX,	&tags.player_addr},
			{"COVOX",	KT_UINT16_HEX,	&tags.covox_addr},
			// TODO
			//{"TIME",	KT_TIME_LOOP,	&tags.???},

			{nullptr, KT_UNKNOWN, nullptr}
		};

		// TODO: Show errors for unsupported tags?
		// TODO: Print errors?
		for (const KeywordDef *kwd = &kwds[0]; kwd->keyword != nullptr; kwd++) {
			if (strcasecmp(kwd->keyword, token) != 0) {
				// Not a match. Keep going.
				continue;
			}

			switch (kwd->type) {
				default:
					assert(!"Unsupported keyword type.");
					break;

				case KT_BOOL:
					// Presence of this keyword sets the value to true.
					*(static_cast<bool*>(kwd->ptr)) = true;
					break;

				case KT_UINT16_DEC: {
					// Decimal value.
					if (!params)
						break;

					char *endptr;
					long val = strtol(params, &endptr, 10);
					if (endptr > params && (*endptr == '\0' || ISSPACE(*endptr))) {
						*(static_cast<uint16_t*>(kwd->ptr)) = static_cast<uint16_t>(val);
					}
					break;
				}

				case KT_UINT16_HEX: {
					// Hexadecimal value.
					if (!params)
						break;

					char *endptr;
					long val = strtol(params, &endptr, 16);
					if (endptr > params && (*endptr == '\0' || ISSPACE(*endptr))) {
						*(static_cast<uint16_t*>(kwd->ptr)) = static_cast<uint16_t>(val);
					}
					break;
				}

				case KT_CHAR: {
					// Character.
					if (!params)
						break;

					if (!ISSPACE(params[0] && (params[1] == '\0' || ISSPACE(params[1])))) {
						// Single character.
						*(static_cast<char*>(kwd->ptr)) = params[0];
					}
					break;
				}

				case KT_STRING: {
					// String value.
					if (!params)
						break;

					// String must be enclosed in double-quotes.
					// TODO: Date parsing?
					if (*params != '"') {
						// Invalid tag. (TODO: Print an error?)
						continue;
					}
					// Find the second '"'.
					char *dblq = strchr(params+1, '"');
					if (!dblq) {
						// Invalid tag. (TODO: Print an error?)
						continue;
					}
					// Zero it out and take the string.
					*dblq = '\0';
					*(static_cast<string*>(kwd->ptr)) = latin1_to_utf8(params+1, -1);
					break;
				}
			}

			// Keyword parsed.
			break;
		}
	}
	
	// Tags parsed.
	tags.tags_read = true;
	return tags;
}

/** SAP **/

/**
 * Read an SAP audio file.
 *
 * A ROM image must be opened by the caller. The file handle
 * will be dup()'d and must be kept open in order to load
 * data from the ROM image.
 *
 * To close the file, either delete this object or call close().
 *
 * NOTE: Check isValid() to determine if this is a valid ROM.
 *
 * @param file Open ROM image.
 */
SAP::SAP(IRpFile *file)
	: super(new SAPPrivate(this, file))
{
	RP_D(SAP);
	d->className = "SAP";
	d->fileType = FTYPE_AUDIO_FILE;

	if (!d->file) {
		// Could not dup() the file handle.
		return;
	}

	// Read the SAP header.
	uint8_t buf[16];
	d->file->rewind();
	size_t size = d->file->read(buf, sizeof(buf));
	if (size != sizeof(buf)) {
		delete d->file;
		d->file = nullptr;
		return;
	}

	// Check if this file is supported.
	DetectInfo info;
	info.header.addr = 0;
	info.header.size = sizeof(buf);
	info.header.pData = buf;
	info.ext = nullptr;	// Not needed for SAP.
	info.szFile = 0;	// Not needed for SAP.
	d->isValid = (isRomSupported_static(&info) >= 0);

	if (!d->isValid) {
		delete d->file;
		d->file = nullptr;
		return;
	}
}

/**
 * Is a ROM image supported by this class?
 * @param info DetectInfo containing ROM detection information.
 * @return Class-specific system ID (>= 0) if supported; -1 if not.
 */
int SAP::isRomSupported_static(const DetectInfo *info)
{
	assert(info != nullptr);
	assert(info->header.pData != nullptr);
	assert(info->header.addr == 0);
	if (!info || !info->header.pData ||
	    info->header.addr != 0 ||
	    info->header.size < 6)
	{
		// Either no detection information was specified,
		// or the header is too small.
		return -1;
	}

	// Check for "SAP\r\n" and "SAP\n".
	if (info->header.size >= 7 && !memcmp(info->header.pData, "SAP\r\n", 5)) {
		// Found the SAP magic number. (CRLF line endings)
		return 0;
	} else if (!memcmp(info->header.pData, "SAP\n", 4)) {
		// Found the SAP magic number. (LF line endings)
		return 0;
	}

	// Not supported.
	return -1;
}

/**
 * Get the name of the system the loaded ROM is designed for.
 * @param type System name type. (See the SystemName enum.)
 * @return System name, or nullptr if type is invalid.
 */
const char *SAP::systemName(unsigned int type) const
{
	RP_D(const SAP);
	if (!d->isValid || !isSystemNameTypeValid(type))
		return nullptr;

	// SAP has the same name worldwide, so we can
	// ignore the region selection.
	static_assert(SYSNAME_TYPE_MASK == 3,
		"SAP::systemName() array index optimization needs to be updated.");

	static const char *const sysNames[4] = {
		"Atari 8-bit SAP Audio",
		"SAP",
		"SAP",
		nullptr
	};

	return sysNames[type & SYSNAME_TYPE_MASK];
}

/**
 * Get a list of all supported file extensions.
 * This is to be used for file type registration;
 * subclasses don't explicitly check the extension.
 *
 * NOTE: The extensions include the leading dot,
 * e.g. ".bin" instead of "bin".
 *
 * NOTE 2: The array and the strings in the array should
 * *not* be freed by the caller.
 *
 * @return NULL-terminated array of all supported file extensions, or nullptr on error.
 */
const char *const *SAP::supportedFileExtensions_static(void)
{
	static const char *const exts[] = {
		".sap",

		nullptr
	};
	return exts;
}

/**
 * Get a list of all supported MIME types.
 * This is to be used for metadata extractors that
 * must indicate which MIME types they support.
 *
 * NOTE: The array and the strings in the array should
 * *not* be freed by the caller.
 *
 * @return NULL-terminated array of all supported file extensions, or nullptr on error.
 */
const char *const *SAP::supportedMimeTypes_static(void)
{
	static const char *const mimeTypes[] = {
		// Unofficial MIME types.
		"audio/x-sap",

		nullptr
	};
	return mimeTypes;
}

/**
 * Load field data.
 * Called by RomData::fields() if the field data hasn't been loaded yet.
 * @return Number of fields read on success; negative POSIX error code on error.
 */
int SAP::loadFieldData(void)
{
	RP_D(SAP);
	if (!d->fields->empty()) {
		// Field data *has* been loaded...
		return 0;
	} else if (!d->file) {
		// File isn't open.
		return -EBADF;
	} else if (!d->isValid) {
		// Unknown file type.
		return -EIO;
	}

	// Get the tags.
	SAPPrivate::TagData tags = d->parseTags();
	if (!tags.tags_read) {
		// No tags.
		return 0;
	}

	// SAP header.
	d->fields->reserve(11);	// Maximum of 11 fields.

	// Author.
	if (!tags.author.empty()) {
		d->fields->addField_string(C_("SAP", "Author"), tags.author);
	}

	// Song title.
	if (!tags.name.empty()) {
		d->fields->addField_string(C_("SAP", "Song Title"), tags.name);
	}

	// Date. (TODO: Parse?)
	if (!tags.date.empty()) {
		d->fields->addField_string(C_("SAP", "Date"), tags.date);
	}

	// Number of songs.
	d->fields->addField_string_numeric(C_("SAP", "# of Songs"), tags.songs);

	// Default song number.
	if (tags.songs > 1) {
		d->fields->addField_string_numeric(C_("SAP", "Default Song #"), tags.def_song);
	}

	// Flags: NTSC/PAL, Stereo
	static const char *const flags_names[] = {
		// tr: PAL is default; if set, the file is for NTSC.
		NOP_C_("SAP|Flags", "NTSC"),
		NOP_C_("SAP|Flags", "Stereo"),
	};
	vector<string> *const v_flags_names = RomFields::strArrayToVector_i18n(
		"SAP|Flags", flags_names, ARRAY_SIZE(flags_names));
	// TODO: Use a bitfield in tags?
	uint32_t flags = 0;
	if (tags.ntsc)   flags |= (1 << 0);
	if (tags.stereo) flags |= (1 << 1);
	d->fields->addField_bitfield(C_("SAP", "Flags"),
		v_flags_names, 0, flags);

	// Type.
	// TODO: Verify that the type is valid?
	if (ISALPHA(tags.type)) {
		d->fields->addField_string(C_("SAP", "Type"),
			rp_sprintf("%c", tags.type));
	} else {
		d->fields->addField_string(C_("SAP", "Type"),
			rp_sprintf("0x%02X", tags.type),
			RomFields::STRF_MONOSPACE);
	}

	// Fastplay. (Number of scanlines)
	unsigned int scanlines = tags.fastplay;
	if (scanlines == 0) {
		// Use the default value for NTSC/PAL.
		scanlines = (tags.ntsc ? 262 : 312);
	}
	d->fields->addField_string_numeric(C_("SAP", "Fastplay"), scanlines);

	// Init address (Types B, D, S) / music address (Type C)
	switch (toupper(tags.type)) {
		default:
			// Skipping for invalid types.
			break;

		case 'B': case 'D': case 'S':
			d->fields->addField_string_numeric(C_("SAP", "Init Address"),
				tags.init_addr,
				RomFields::FB_HEX, 4, RomFields::STRF_MONOSPACE);
			break;

		case 'C':
			d->fields->addField_string_numeric(C_("SAP", "Music Address"),
				tags.music_addr,
				RomFields::FB_HEX, 4, RomFields::STRF_MONOSPACE);
			break;
	}

	// Player address.
	d->fields->addField_string_numeric(C_("SAP", "Player Address"),
		tags.player_addr,
		RomFields::FB_HEX, 4, RomFields::STRF_MONOSPACE);

	// COVOX address. (if non-zero)
	if (tags.covox_addr != 0) {
		d->fields->addField_string_numeric(C_("SAP", "COVOX Address"),
			tags.covox_addr,
			RomFields::FB_HEX, 4, RomFields::STRF_MONOSPACE);
	}

	// Finished reading the field data.
	return static_cast<int>(d->fields->count());
}

}