/***************************************************************************
 * ROM Properties Page shell extension. (libromdata)                       *
 * Xbox_XBE.cpp: Microsoft Xbox executable reader.                         *
 *                                                                         *
 * Copyright (c) 2016-2019 by David Korth.                                 *
 * SPDX-License-Identifier: GPL-2.0-or-later                               *
 ***************************************************************************/

#include "Xbox_XBE.hpp"
#include "librpbase/RomData_p.hpp"

#include "xbox_xbe_structs.h"

// librpbase
#include "librpbase/common.h"
#include "librpbase/byteswap.h"
#include "librpbase/TextFuncs.hpp"
#include "librpbase/file/IRpFile.hpp"
#include "librpbase/img/rp_image.hpp"
#include "librpbase/img/RpPng.hpp"
using namespace LibRpBase;

// libi18n
#include "libi18n/i18n.h"

// DiscReader
#include "librpbase/disc/DiscReader.hpp"
#include "librpbase/disc/PartitionFile.hpp"

// Other RomData subclasses
#include "Texture/XboxXPR.hpp"
#include "Other/EXE.hpp"

// for XPR0 magic
#include "Texture/xbox_xpr_structs.h"

// C includes. (C++ namespace)
#include <cassert>
#include <cerrno>
#include <cstring>

// C++ includes.
#include <memory>
#include <sstream>
#include <string>
#include <vector>
using std::ostringstream;
using std::string;
using std::unique_ptr;
using std::vector;

namespace LibRomData {

ROMDATA_IMPL(Xbox_XBE)

// Workaround for RP_D() expecting the no-underscore naming convention.
#define Xbox_XBEPrivate Xbox_XBE_Private

class Xbox_XBE_Private : public RomDataPrivate
{
	public:
		Xbox_XBE_Private(Xbox_XBE *q, IRpFile *file);
		virtual ~Xbox_XBE_Private();

	private:
		typedef RomDataPrivate super;
		RP_DISABLE_COPY(Xbox_XBE_Private)

	public:
		// XBE header
		// NOTE: **NOT** byteswapped.
		XBE_Header xbeHeader;

		// XBE certificate
		// NOTE: **NOT** byteswapped.
		XBE_Certificate xbeCertificate;

		// RomData subclasses.
		// TODO: Also get the save image? ($$XSIMAGE)
		DiscReader *discReader;	// Common DiscReader
		EXE *pe_exe;		// PE executable

		// Title image.
		// NOTE: May be a PNG image on some discs.
		struct {
			bool isInit;
			bool isPng;
			union {
				XboxXPR *xpr0;
				rp_image *png;
			};
		} xtImage;

	public:
		/**
		 * Find an XBE section header.
		 * @param name		[in] Section header name.
		 * @param pOutHeader	[out] Buffer to store the header. (Byteswapped to host-endian.)
		 * @return 0 on success; negative POSIX error code on error.
		 */
		int findXbeSectionHeader(const char *name, XBE_Section_Header *pOutHeader);

		/**
		 * Initialize the title image object.
		 * NOTE: Check xtImage's union after initializing.
		 * @return 0 if initialized; negative POSIX error code on error.
		 */
		int initXPR0_xtImage(void);

		/**
		 * Initialize the PE executable object.
		 * @return EXE object on success; nullptr on error.
		 */
		const EXE *initEXE(void);
};

/** Xbox_XBE_Private **/

Xbox_XBE_Private::Xbox_XBE_Private(Xbox_XBE *q, IRpFile *file)
	: super(q, file)
	, discReader(nullptr)
	, pe_exe(nullptr)
{
	// Clear the XBE structs.
	memset(&xbeHeader, 0, sizeof(xbeHeader));
	memset(&xbeCertificate, 0, sizeof(xbeCertificate));

	// No xtImage initially.
	memset(&xtImage, 0, sizeof(xtImage));
}

Xbox_XBE_Private::~Xbox_XBE_Private()
{
	if (pe_exe) {
		pe_exe->unref();
	}

	if (xtImage.isInit) {
		if (!xtImage.isPng) {
			// XPR0 image
			if (xtImage.xpr0) {
				xtImage.xpr0->unref();
			}
		} else {
			// PNG image
			delete xtImage.png;
		}
	}

	delete discReader;
}

/**
 * Find an XBE section header.
 * @param name		[in] Section header name.
 * @param pOutHeader	[out] Buffer to store the header. (Byteswapped to host-endian.)
 * @return 0 on success; negative POSIX error code on error.
 */
int Xbox_XBE_Private::findXbeSectionHeader(const char *name, XBE_Section_Header *pOutHeader)
{
	// Load the section headers.
	if (!file || !file->isOpen()) {
		// File is not open.
		return -EBADF;
	}

	// We're loading the first 64 KB of the executable.
	// Section headers and names are usually there.
	// TODO: Find any exceptions?
	static const size_t XBE_READ_SIZE = 64*1024;

	// Load the section headers.
	const uint32_t base_address = le32_to_cpu(xbeHeader.base_address);
	const uint32_t section_headers_address = le32_to_cpu(xbeHeader.section_headers_address);
	if (section_headers_address <= base_address) {
		// Out of range.
		// NOTE: <= - base address would have the magic number.
		return -EIO;
	}

	const uint32_t shdr_address_phys = section_headers_address - base_address;
	if (shdr_address_phys >= XBE_READ_SIZE) {
		// Section headers is not in the first 64 KB.
		return -EIO;
	}

	// Read the XBE header.
	unique_ptr<uint8_t[]> first64KB(new uint8_t[XBE_READ_SIZE]);
	size_t size = file->seekAndRead(0, first64KB.get(), XBE_READ_SIZE);
	if (size != XBE_READ_SIZE) {
		// Seek and/or read error.
		return -EIO;
	}

	// Section count.
	unsigned int section_count = le32_to_cpu(xbeHeader.section_count);
	// If this goes over the 64 KB limit, reduce the section count.
	if (shdr_address_phys + (section_count * sizeof(XBE_Section_Header)) > XBE_READ_SIZE) {
		// Out of bounds. Reduce it.
		section_count = (XBE_READ_SIZE - shdr_address_phys) / sizeof(XBE_Section_Header);
	}

	// First section header.
	const XBE_Section_Header *pHdr = reinterpret_cast<const XBE_Section_Header*>(
		&first64KB[shdr_address_phys]);
	const XBE_Section_Header *const pHdr_end = pHdr + section_count;

	// Find the $$XTIMAGE section.
	// TODO: Cache a "not found" result so we don't have to
	// re-check the section headers again?
	for (; pHdr < pHdr_end; pHdr++) {
		char section_name[16];	// Allow up to 15 chars plus NULL terminator.

		const uint32_t name_address = le32_to_cpu(pHdr->section_name_address);
		if (name_address <= base_address) {
			// Out of range.
			continue;
		}

		// Read the name.
		size = file->seekAndRead(name_address - base_address, section_name, sizeof(section_name));
		if (size != sizeof(section_name)) {
			// Seek and/or read error.
			return -EIO;
		}
		section_name[sizeof(section_name)-1] = '\0';

		if (!strcmp(section_name, name)) {
			// Found it!
			pOutHeader->flags = le32_to_cpu(pHdr->flags);
			pOutHeader->vaddr = le32_to_cpu(pHdr->vaddr);
			pOutHeader->vsize = le32_to_cpu(pHdr->vsize);
			pOutHeader->paddr = le32_to_cpu(pHdr->paddr);
			pOutHeader->psize = le32_to_cpu(pHdr->psize);
			pOutHeader->section_name_address		= le32_to_cpu(pHdr->section_name_address);
			pOutHeader->section_name_refcount		= le32_to_cpu(pHdr->section_name_refcount);
			pOutHeader->head_shared_page_recount_address	= le32_to_cpu(pHdr->head_shared_page_recount_address);
			pOutHeader->tail_shared_page_recount_address	= le32_to_cpu(pHdr->tail_shared_page_recount_address);
			memcpy(pOutHeader->sha1_digest, pHdr->sha1_digest, sizeof(pHdr->sha1_digest));
			return 0;
		}
	}

	// Not found.
	return -ENOENT;
}

/**
 * Initialize the title image object.
 * NOTE: Check xtImage's union after initializing.
 * @return 0 if initialized; negative POSIX error code on error.
 */
int Xbox_XBE_Private::initXPR0_xtImage(void)
{
	if (xtImage.isInit) {
		// Title image is already initialized.
		return 0;
	}

	// Find the $$XTIMAGE section.
	XBE_Section_Header hdr_xtImage;
	int ret = findXbeSectionHeader("$$XTIMAGE", &hdr_xtImage);
	if (ret != 0) {
		// Not found.
		return -ENOENT;
	}

	// Create the DiscReader if it isn't open already.
	if (!discReader) {
		DiscReader *const discReader_tmp = new DiscReader(this->file);
		if (!discReader_tmp->isOpen()) {
			// Unable to open the DiscReader.
			delete discReader_tmp;
			return -EIO;
		}
		discReader = discReader_tmp;
	}

	// Open the XPR0 image.
	// paddr/psize have absolute addresses.
	ret = 0;
	IRpFile *const ptFile = new PartitionFile(discReader,
		hdr_xtImage.paddr, hdr_xtImage.psize);
	if (ptFile->isOpen()) {
		// $$XTIMAGE is usually an XPR0 image.
		// The Burger King games, wihch have both Xbox and Xbox 360
		// executables, incorrectly use PNG format here.
		uint32_t magic = 0;
		size_t size = ptFile->read(&magic, sizeof(magic));
		if (size != sizeof(magic)) {
			// Read error.
			ptFile->unref();
			return -EIO;
		}
		ptFile->rewind();

		if (magic == cpu_to_be32(XBOX_XPR0_MAGIC)) {
			XboxXPR *const xpr0 = new XboxXPR(ptFile);
			if (xpr0->isOpen()) {
				// XPR0 image opened.
				xtImage.isInit = true;
				xtImage.isPng = false;
				xtImage.xpr0 = xpr0;
			} else {
				// Unable to open the XPR0 image.
				xpr0->unref();
				ret = -EIO;
			}
		} else if (magic == cpu_to_be32('\x89PNG')) {
			// PNG image.
			rp_image *const img = RpPng::load(ptFile);
			if (img->isValid()) {
				// PNG image opened.
				xtImage.isInit = true;
				xtImage.isPng = true;
				printf("PNG!!\n");
				xtImage.png = img;
			} else {
				// Unable to open the PNG image.
				delete img;
				ret = -EIO;
			}
		}
		ptFile->unref();
	} else {
		// Unable to open the file.
		ret = -EIO;
		ptFile->unref();
	}

	// Image loaded.
	return 0;
}

/**
 * Initialize the PE executable object.
 * @return EXE object on success; nullptr on error.
 */
const EXE *Xbox_XBE_Private::initEXE(void)
{
	if (pe_exe) {
		// EXE is already initialized.
		return pe_exe;
	}

	if (!file || !file->isOpen()) {
		// File is not open.
		return nullptr;
	}

	// EXE is located at (pe_base_address - base_address).
	const int64_t fileSize = file->size();
	const uint32_t exe_address =
		le32_to_cpu(xbeHeader.pe_base_address) -
		le32_to_cpu(xbeHeader.base_address);
	if (exe_address < sizeof(xbeHeader) || exe_address >= fileSize) {
		// Out of range.
		return nullptr;
	}

	// Create the DiscReader if it isn't open already.
	if (!discReader) {
		DiscReader *const discReader_tmp = new DiscReader(this->file);
		if (!discReader_tmp->isOpen()) {
			// Unable to open the DiscReader.
			delete discReader_tmp;
			return nullptr;
		}
	}

	// Open the EXE file.
	IRpFile *const ptFile = new PartitionFile(discReader,
		exe_address, fileSize - exe_address);
	if (ptFile->isOpen()) {
		EXE *const pe_exe_tmp = new EXE(ptFile);
		ptFile->unref();
		if (pe_exe_tmp->isOpen()) {
			// EXE opened.
			this->pe_exe = pe_exe_tmp;
		} else {
			// Unable to open the XPR0 image.
			pe_exe_tmp->unref();
		}
	} else {
		// Unable to open the file.
		ptFile->unref();
	}

	// EXE loaded.
	return this->pe_exe;
}

/** Xbox_XBE **/

/**
 * Read an Xbox XBE file.
 *
 * A ROM image must be opened by the caller. The file handle
 * will be ref()'d and must be kept open in order to load
 * data from the disc image.
 *
 * To close the file, either delete this object or call close().
 *
 * NOTE: Check isValid() to determine if this is a valid ROM.
 *
 * @param file Open XBE file.
 */
Xbox_XBE::Xbox_XBE(IRpFile *file)
	: super(new Xbox_XBE_Private(this, file))
{
	// This class handles executables.
	RP_D(Xbox_XBE);
	d->className = "Xbox_XBE";
	d->fileType = FTYPE_EXECUTABLE;

	if (!d->file) {
		// Could not ref() the file handle.
		return;
	}

	// Read the XBE header.
	d->file->rewind();
	size_t size = d->file->read(&d->xbeHeader, sizeof(d->xbeHeader));
	if (size != sizeof(d->xbeHeader)) {
		d->xbeHeader.magic = 0;
		d->file->unref();
		d->file = nullptr;
		return;
	}

	// Check if this file is supported.
	DetectInfo info;
	info.header.addr = 0;
	info.header.size = sizeof(d->xbeHeader);
	info.header.pData = reinterpret_cast<const uint8_t*>(&d->xbeHeader);
	info.ext = nullptr;	// Not needed for XBE.
	info.szFile = 0;	// Not needed for XBE.
	d->isValid = (isRomSupported_static(&info) >= 0);

	if (!d->isValid) {
		d->xbeHeader.magic = 0;
		d->file->unref();
		d->file = nullptr;
		return;
	}

	// Load the certificate.
	const uint32_t base_address = le32_to_cpu(d->xbeHeader.base_address);
	const uint32_t cert_address = le32_to_cpu(d->xbeHeader.cert_address);
	if (cert_address > base_address) {
		size = d->file->seekAndRead(cert_address - base_address,
			&d->xbeCertificate, sizeof(d->xbeCertificate));
		if (size != sizeof(d->xbeCertificate)) {
			// Unable to load the certificate.
			// Continue anyway.
			d->xbeCertificate.size = 0;
		}
	}
}

/**
 * Close the opened file.
 */
void Xbox_XBE::close(void)
{
	RP_D(Xbox_XBE);

	if (d->pe_exe) {
		d->pe_exe->unref();
		d->pe_exe = nullptr;
	}

	if (d->xtImage.isInit) {
		d->xtImage.isInit = false;
		if (!d->xtImage.isPng) {
			// XPR0 image
			if (d->xtImage.xpr0) {
				d->xtImage.xpr0->unref();
			}
		} else {
			// PNG image
			d->xtImage.isPng = false;
			delete d->xtImage.png;
		}
	}

	delete d->discReader;
	d->discReader = nullptr;

	// Call the superclass function.
	super::close();
}

/** ROM detection functions. **/

/**
 * Is a ROM image supported by this class?
 * @param info DetectInfo containing ROM detection information.
 * @return Class-specific system ID (>= 0) if supported; -1 if not.
 */
int Xbox_XBE::isRomSupported_static(const DetectInfo *info)
{
	assert(info != nullptr);
	assert(info->header.pData != nullptr);
	assert(info->header.addr == 0);
	if (!info || !info->header.pData ||
	    info->header.addr != 0 ||
	    info->header.size < sizeof(XBE_Header))
	{
		// Either no detection information was specified,
		// or the header is too small.
		return -1;
	}

	// Check for XBE.
	const XBE_Header *const xbeHeader =
		reinterpret_cast<const XBE_Header*>(info->header.pData);
	if (xbeHeader->magic == cpu_to_be32(XBE_MAGIC)) {
		// We have an XBE file.
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
const char *Xbox_XBE::systemName(unsigned int type) const
{
	RP_D(const Xbox_XBE);
	if (!d->isValid || !isSystemNameTypeValid(type))
		return nullptr;

	// Xbox 360 has the same name worldwide, so we can
	// ignore the region selection.
	static_assert(SYSNAME_TYPE_MASK == 3,
		"Xbox_XBE::systemName() array index optimization needs to be updated.");

	// Bits 0-1: Type. (long, short, abbreviation)
	static const char *const sysNames[4] = {
		"Microsoft Xbox", "Xbox", "Xbox", nullptr
	};

	return sysNames[type & SYSNAME_TYPE_MASK];
}

/**
 * Get a list of all supported file extensions.
 * This is to be used for file type registration;
 * subclasses don't explicitly check the extension.
 *
 * NOTE: The extensions do not include the leading dot,
 * e.g. "bin" instead of ".bin".
 *
 * NOTE 2: The array and the strings in the array should
 * *not* be freed by the caller.
 *
 * @return NULL-terminated array of all supported file extensions, or nullptr on error.
 */
const char *const *Xbox_XBE::supportedFileExtensions_static(void)
{
	static const char *const exts[] = {
		".xbe",

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
const char *const *Xbox_XBE::supportedMimeTypes_static(void)
{
	static const char *const mimeTypes[] = {
		// Unofficial MIME types.
		// TODO: Get these upstreamed on FreeDesktop.org.
		"application/x-xbox-executable",

		nullptr
	};
	return mimeTypes;
}

/**
 * Get a bitfield of image types this class can retrieve.
 * @return Bitfield of supported image types. (ImageTypesBF)
 */
uint32_t Xbox_XBE::supportedImageTypes(void) const
{
	RP_D(const Xbox_XBE);
	if (!d->xtImage.isInit) {
		const_cast<Xbox_XBE_Private*>(d)->initXPR0_xtImage();
	}

	return (d->xtImage.isInit ? IMGBF_INT_ICON : 0);
}

/**
 * Get a list of all available image sizes for the specified image type.
 * @param imageType Image type.
 * @return Vector of available image sizes, or empty vector if no images are available.
 */
vector<RomData::ImageSizeDef> Xbox_XBE::supportedImageSizes(ImageType imageType) const
{
	ASSERT_supportedImageSizes(imageType);

	if (imageType != IMG_INT_ICON) {
		// Only icons are supported.
		return vector<ImageSizeDef>();
	}

	RP_D(const Xbox_XBE);
	if (!d->xtImage.isInit) {
		const_cast<Xbox_XBE_Private*>(d)->initXPR0_xtImage();
	}

	if (d->xtImage.isInit) {
		if (!d->xtImage.isPng) {
			// XPR0 image
			return d->xtImage.xpr0->supportedImageSizes(IMG_INT_IMAGE);
		} else {
			// PNG image
			const ImageSizeDef sz_INT_ICON[] = {
				{nullptr, (uint16_t)d->xtImage.png->width(), (uint16_t)d->xtImage.png->height(), 0},
			};
			return vector<ImageSizeDef>(sz_INT_ICON,
				sz_INT_ICON + ARRAY_SIZE(sz_INT_ICON));
		}
	}

	// No images.
	return vector<ImageSizeDef>();
}

/**
 * Get image processing flags.
 *
 * These specify post-processing operations for images,
 * e.g. applying transparency masks.
 *
 * @param imageType Image type.
 * @return Bitfield of ImageProcessingBF operations to perform.
 */
uint32_t Xbox_XBE::imgpf(ImageType imageType) const
{
	ASSERT_imgpf(imageType);

	if (imageType != IMG_INT_ICON) {
		// Only icons are supported.
		return 0;
	}

	RP_D(const Xbox_XBE);
	if (!d->xtImage.isInit) {
		const_cast<Xbox_XBE_Private*>(d)->initXPR0_xtImage();
	}

	uint32_t ret = 0;
	if (d->xtImage.isInit) {
		if (!d->xtImage.isPng) {
			// XPR0 image
			return d->xtImage.xpr0->imgpf(IMG_INT_IMAGE);
		} else {
			// PNG image
			// If both dimensions of the texture are 64 or less,
			// specify nearest-neighbor scaling.
			if (d->xtImage.png->width() <= 64 && d->xtImage.png->height() <= 64) {
				// 64x64 or smaller.
				ret = IMGPF_RESCALE_NEAREST;
			}
		}
	}
	return ret;
}

/**
 * Load field data.
 * Called by RomData::fields() if the field data hasn't been loaded yet.
 * @return Number of fields read on success; negative POSIX error code on error.
 */
int Xbox_XBE::loadFieldData(void)
{
	RP_D(Xbox_XBE);
	if (!d->fields->empty()) {
		// Field data *has* been loaded...
		return 0;
	} else if (!d->file || !d->file->isOpen()) {
		// File isn't open.
		return -EBADF;
	} else if (!d->isValid) {
		// XBE file isn't valid.
		return -EIO;
	}

	// Parse the XBE file.
	// NOTE: The magic number is NOT byteswapped in the constructor.
	const XBE_Header *const xbeHeader = &d->xbeHeader;
	const XBE_Certificate *const xbeCertificate = &d->xbeCertificate;
	if (xbeHeader->magic != cpu_to_be32(XBE_MAGIC)) {
		// Invalid magic number.
		return 0;
	}

	// Maximum of 11 fields.
	d->fields->reserve(11);
	d->fields->setTabName(0, "XBE");

	// Game name
	d->fields->addField_string(C_("RomData", "Title"),
		utf16le_to_utf8(xbeCertificate->title_name, ARRAY_SIZE(xbeCertificate->title_name)));

	// Original PE filename
	const uint32_t base_address = le32_to_cpu(d->xbeHeader.base_address);
	const uint32_t filenameW_address = le32_to_cpu(d->xbeHeader.debug_filenameW_address);
	const char *const s_filename_title = C_("Xbox_XBE", "PE Filename");
	if (filenameW_address > base_address) {
		char16_t pe_filename_W[260];
		size_t size = d->file->seekAndRead(filenameW_address - base_address,
			pe_filename_W, sizeof(pe_filename_W));
		if (size == sizeof(pe_filename_W)) {
			// Convert to UTF-8.
			pe_filename_W[ARRAY_SIZE(pe_filename_W)-1] = 0;
			string pe_filename = utf16_to_utf8(pe_filename_W, -1);
			if (!pe_filename.empty()) {
				d->fields->addField_string(s_filename_title, pe_filename);
			} else {
				d->fields->addField_string(s_filename_title, C_("RomData", "Unknown"));
			}
		}
	} else {
		d->fields->addField_string(s_filename_title, C_("RomData", "Unknown"));
	}

	// Title ID
	// FIXME: Verify behavior on big-endian.
	// TODO: Consolidate implementations into a shared function.
	string tid_str;
	char hexbuf[4];
	if (xbeCertificate->title_id.a >= 0x20) {
		tid_str += (char)xbeCertificate->title_id.a;
	} else {
		tid_str += "\\x";
		snprintf(hexbuf, sizeof(hexbuf), "%02X",
			(uint8_t)xbeCertificate->title_id.a);
		tid_str.append(hexbuf, 2);
	}
	if (xbeCertificate->title_id.b >= 0x20) {
		tid_str += (char)xbeCertificate->title_id.b;
	} else {
		tid_str += "\\x";
		snprintf(hexbuf, sizeof(hexbuf), "%02X",
			(uint8_t)xbeCertificate->title_id.b);
		tid_str.append(hexbuf, 2);
	}

	d->fields->addField_string(C_("Xbox_XBE", "Title ID"),
		rp_sprintf_p(C_("Xbox_XBE", "%1$08X (%2$s-%3$03u)"),
			le32_to_cpu(xbeCertificate->title_id.u32),
			tid_str.c_str(),
			le16_to_cpu(xbeCertificate->title_id.u16)),
		RomFields::STRF_MONOSPACE);

	// Timestamp
	// TODO: time_t is signed, so values greater than 2^31-1 may be negative.
	const char *const s_timestamp_title = C_("Xbox_XBE", "Timestamp");
	uint32_t timestamp = le32_to_cpu(xbeHeader->timestamp);
	if (timestamp != 0) {
		d->fields->addField_dateTime(s_timestamp_title,
			static_cast<time_t>(timestamp),
			RomFields::RFT_DATETIME_HAS_DATE |
			RomFields::RFT_DATETIME_HAS_TIME);
	} else {
		d->fields->addField_string(s_timestamp_title, C_("Xbox_XBE", "Not set"));
	}

	// Media types
	// NOTE: Using a string instead of a bitfield because very rarely
	// are all of these set, and in most cases, none are.
	// TODO: RFT_LISTDATA?
	static const char *const media_type_tbl[] = {
		// 0
		NOP_C_("Xbox_XBE", "Hard Disk"),
		NOP_C_("Xbox_XBE", "XGD1"),
		NOP_C_("Xbox_XBE", "DVD/CD"),
		NOP_C_("Xbox_XBE", "CD-ROM"),
		// 4
		NOP_C_("Xbox_XBE", "DVD-ROM SL"),
		NOP_C_("Xbox_XBE", "DVD-ROM DL"),
		NOP_C_("Xbox_XBE", "DVD-RW SL"),
		NOP_C_("Xbox_XBE", "DVD-RW DL"),
		// 8
		NOP_C_("Xbox_XBE", "Dongle"),
		NOP_C_("Xbox_XBE", "Media Board"),
		// TODO: Non-secure HDD
	};

	ostringstream oss;
	unsigned int found = 0;
	uint32_t media_types = le32_to_cpu(xbeCertificate->allowed_media_types);
	for (unsigned int i = 0; i < ARRAY_SIZE(media_type_tbl); i++, media_types >>= 1) {
		if (!(media_types & 1))
			continue;

		if (found > 0) {
			if (found % 4 == 0) {
				oss << ",\n";
			} else {
				oss << ", ";
			}
		}
		found++;

		if (media_type_tbl[i]) {
			oss << media_type_tbl[i];
		} else {
			oss << i;
		}
	}

	d->fields->addField_string(C_("Xbox_XBE", "Media Types"),
		found ? oss.str() : C_("Xbox_XBE", "None"));

	// Region code
	uint32_t region_code = le32_to_cpu(xbeCertificate->region_code);
	if (region_code & XBE_REGION_CODE_MANUFACTURING) {
		// Relocate this bit to make it easier to handle the
		// region code table.
		region_code &= ~XBE_REGION_CODE_MANUFACTURING;
		region_code |= 8;
	}
	static const char *const region_code_tbl[] = {
		NOP_C_("Region", "North America"),
		NOP_C_("Region", "Japan"),
		NOP_C_("Region", "Rest of World"),
		NOP_C_("Region", "Manufacturing"),
	};
	vector<string> *const v_region_code = RomFields::strArrayToVector_i18n(
		"Region", region_code_tbl, ARRAY_SIZE(region_code_tbl));
	d->fields->addField_bitfield(C_("RomData", "Region Code"),
		v_region_code, 3, region_code);

	// TODO: Age ratings, disc number

	// Can we get the EXE section?
	const EXE *const pe_exe = const_cast<Xbox_XBE_Private*>(d)->initEXE();
	if (pe_exe) {
		// Add the fields.
		const RomFields *const exeFields = pe_exe->fields();
		if (exeFields) {
			d->fields->addFields_romFields(exeFields, RomFields::TabOffset_AddTabs);
		}
	}

	// Finished reading the field data.
	return static_cast<int>(d->fields->count());
}

/**
 * Load metadata properties.
 * Called by RomData::metaData() if the field data hasn't been loaded yet.
 * @return Number of metadata properties read on success; negative POSIX error code on error.
 */
int Xbox_XBE::loadMetaData(void)
{
	RP_D(Xbox_XBE);
	if (d->metaData != nullptr) {
		// Metadata *has* been loaded...
		return 0;
	} else if (!d->file) {
		// File isn't open.
		return -EBADF;
	} else if (!d->isValid) {
		// XBE file isn't valid.
		return -EIO;
	}

	// Create the metadata object.
	d->metaData = new RomMetaData();
	d->metaData->reserve(1);	// Maximum of 1 metadata property.

	const XBE_Certificate *const xbeCertificate = &d->xbeCertificate;

	// Title
	d->metaData->addMetaData_string(Property::Title,
		utf16le_to_utf8(xbeCertificate->title_name, ARRAY_SIZE(xbeCertificate->title_name)));

	// Finished reading the metadata.
	return static_cast<int>(d->metaData->count());
}

/**
 * Load an internal image.
 * Called by RomData::image().
 * @param imageType	[in] Image type to load.
 * @param pImage	[out] Pointer to const rp_image* to store the image in.
 * @return 0 on success; negative POSIX error code on error.
 */
int Xbox_XBE::loadInternalImage(ImageType imageType, const rp_image **pImage)
{
	ASSERT_loadInternalImage(imageType, pImage);

	RP_D(Xbox_XBE);
	if (imageType != IMG_INT_ICON) {
		// Only IMG_INT_ICON is supported by XBE.
		// TODO: -EIO for unsupported imageType?
		*pImage = nullptr;
		return -ENOENT;
	} else if (d->xtImage.isInit) {
		// Image has already been loaded.
		// We'll get the actual image after this.
	} else if (!d->file) {
		// File isn't open.
		*pImage = nullptr;
		return -EBADF;
	} else if (!d->isValid) {
		// Save file isn't valid.
		*pImage = nullptr;
		return -EIO;
	}

	if (!d->xtImage.isInit) {
		const_cast<Xbox_XBE_Private*>(d)->initXPR0_xtImage();
	}

	if (!d->xtImage.isPng) {
		// XPR0 image
		return const_cast<XboxXPR*>(d->xtImage.xpr0)->loadInternalImage(IMG_INT_IMAGE, pImage);
	} else {
		// PNG image
		*pImage = d->xtImage.png;
	}

	return 0;
}

}
