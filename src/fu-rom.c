/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <fwupd.h>
#include <appstream-glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#include "fu-cleanup.h"
#include "fu-rom.h"

static void fu_rom_finalize			 (GObject *object);

#define FU_ROM_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), FU_TYPE_ROM, FuRomPrivate))

/* data from http://resources.infosecinstitute.com/pci-expansion-rom/ */
typedef struct {
	guint8		*rom_data;
	guint32		 rom_len;
	guint32		 rom_offset;
	guint32		 entry_point;
	guint8		 reserved[18];
	guint16		 cpi_ptr;
	guint16		 vendor_id;
	guint16		 device_id;
	guint16		 device_list_ptr;
	guint16		 data_len;
	guint8		 data_rev;
	guint32		 class_code;
	guint32		 image_len;
	guint16		 revision_level;
	guint8		 code_type;
	guint8		 last_image;
	guint32		 max_runtime_len;
	guint16		 config_header_ptr;
	guint16		 dmtf_clp_ptr;
} FuRomPciHeader;

/**
 * FuRomPrivate:
 *
 * Private #FuRom data
 **/
struct _FuRomPrivate
{
	GChecksum			*checksum_wip;
	GInputStream			*stream;
	FuRomKind			 kind;
	gchar				*version;
	gchar				*guid;
	guint16				 vendor;
	guint16				 model;
	GPtrArray			*hdrs; /* of FuRomPciHeader */
};

G_DEFINE_TYPE (FuRom, fu_rom, G_TYPE_OBJECT)

/**
 * fu_rom_pci_header_free:
 **/
static void
fu_rom_pci_header_free (FuRomPciHeader *hdr)
{
	g_free (hdr->rom_data);
	g_free (hdr);
}

/**
 * fu_rom_kind_to_string:
 **/
const gchar *
fu_rom_kind_to_string (FuRomKind kind)
{
	if (kind == FU_ROM_KIND_UNKNOWN)
		return "unknown";
	if (kind == FU_ROM_KIND_ATI)
		return "ati";
	if (kind == FU_ROM_KIND_NVIDIA)
		return "nvidia";
	if (kind == FU_ROM_KIND_INTEL)
		return "intel";
	if (kind == FU_ROM_KIND_PCI)
		return "pci";
	return NULL;
}

/**
 * fu_rom_pci_strstr:
 **/
static guint8 *
fu_rom_pci_strstr (FuRomPciHeader *hdr, const gchar *needle)
{
	guint i;
	guint needle_len;
	guint8 *haystack;
	gsize haystack_len;

	if (needle == NULL || needle[0] == '\0')
		return NULL;
	if (hdr->rom_data == NULL)
		return NULL;
	if (hdr->data_len > hdr->rom_len)
		return NULL;
	haystack = &hdr->rom_data[hdr->data_len];
	haystack_len = hdr->rom_len - hdr->data_len;
	needle_len = strlen (needle);
	if (needle_len > haystack_len)
		return NULL;
	for (i = 0; i < haystack_len - needle_len; i++) {
		if (memcmp (haystack + i, needle, needle_len) == 0)
			return &haystack[i];
	}
	return NULL;
}

/**
 * fu_rom_blank_serial_numbers:
 **/
static guint
fu_rom_blank_serial_numbers (guint8 *buffer, guint buffer_sz)
{
	guint i;
	for (i = 0; i < buffer_sz; i++) {
		if (buffer[i] == 0xff ||
		    buffer[i] == '\0' ||
		    buffer[i] == '\n' ||
		    buffer[i] == '\r')
			break;
		buffer[i] = '\0';
	}
	return i;
}

/**
 * fu_rom_get_hex_dump:
 **/
static gchar *
fu_rom_get_hex_dump (guint8 *buffer, gssize sz)
{
	GString *str = NULL;
	guint i;
	str = g_string_new ("");
	if (sz <= 0)
		return NULL;
	for (i = 0; i < sz; i++)
		g_string_append_printf (str, "%02x ", buffer[i]);
	g_string_append (str, "   ");
	for (i = 0; i < sz; i++) {
		gchar tmp = '?';
		if (g_ascii_isprint (buffer[i]))
			tmp = buffer[i];
		g_string_append_printf (str, "%c", tmp);
	}
	return g_string_free (str, FALSE);
}

typedef struct {
	guint8		 segment_kind;
	guint8		*data;
	guint16		 data_len;
	guint16		 next_offset;
} FooRomPciCertificateHdr;

/**
 * fu_rom_pci_print_certificate_data:
 **/
static void
fu_rom_pci_print_certificate_data (guint8 *buffer, gssize sz)
{
	guint16 off = 0;
	_cleanup_free_ gchar *hdr_str = NULL;

	/* 27 byte header, unknown purpose */
	hdr_str = fu_rom_get_hex_dump (buffer+off, 27);
	g_debug ("    ISBN header: %s", hdr_str);
	buffer += 27;

	while (TRUE) {
		/* 29 byte header to the segment, then data:
		 * 0x01      = type. 0x1 = certificate, 0x2 = hashes?
		 * 0x13,0x14 = offset to next segment */
		FooRomPciCertificateHdr h;
		_cleanup_free_ gchar *segment_str = NULL;
		segment_str = fu_rom_get_hex_dump (buffer+off, 29);
		g_debug ("     ISBN segment @%02x: %s", off, segment_str);
		h.segment_kind = buffer[off+1];
		h.next_offset = ((guint16) buffer[off+14] << 8) + buffer[off+13];
		h.data = &buffer[off+29];

		/* calculate last block length automatically */
		if (h.next_offset == 0)
			h.data_len = sz - off - 29 - 27;
		else
			h.data_len = h.next_offset - off - 29;

		/* print the certificate */
		if (h.segment_kind == 0x01) {
			_cleanup_free_ gchar *tmp = NULL;
			tmp = fu_rom_get_hex_dump (h.data, h.data_len);
			g_debug ("%s(%i)", tmp, h.data_len);
		} else if (h.segment_kind == 0x02) {
			_cleanup_free_ gchar *tmp = NULL;
			tmp = fu_rom_get_hex_dump (h.data,
						   h.data_len < 32 ? h.data_len : 32);
			g_debug ("%s(%i)", tmp, h.data_len);
		} else {
			g_warning ("unknown segment kind %i", h.segment_kind);
		}

		/* last block */
		if (h.next_offset == 0x0000)
			break;
		off = h.next_offset;
	}
}

/**
 * fu_rom_pci_code_type_to_string:
 **/
static const gchar *
fu_rom_pci_code_type_to_string (guint8 code_type)
{
	if (code_type == 0)
		return "Intel86";
	if (code_type == 1)
		return "OpenFirmware";
	if (code_type == 2)
		return "PA-RISC";
	if (code_type == 3)
		return "EFI";
	return "reserved";
}

/**
 * fu_rom_pci_header_get_checksum:
 **/
static guint8
fu_rom_pci_header_get_checksum (FuRomPciHeader *hdr)
{
	guint8 chksum_check = 0x00;
	guint i;
	for (i = 0; i < hdr->rom_len; i++)
		chksum_check += hdr->rom_data[i];
	return chksum_check;
}

/**
 * fu_rom_pci_print_header:
 **/
static void
fu_rom_pci_print_header (FuRomPciHeader *hdr)
{
	guint8 chksum_check;
	guint8 *buffer;
	_cleanup_free_ gchar *data_str = NULL;
	_cleanup_free_ gchar *reserved_str = NULL;

	g_debug ("PCI Header");
	g_debug (" RomOffset: 0x%04x", hdr->rom_offset);
	g_debug (" RomSize:   0x%04x", hdr->rom_len);
	g_debug (" EntryPnt:  0x%06x", hdr->entry_point);
	reserved_str = fu_rom_get_hex_dump (hdr->reserved, 18);
	g_debug (" Reserved:  %s", reserved_str);
	g_debug (" CpiPtr:    0x%04x", hdr->cpi_ptr);

	/* print the data */
	buffer = &hdr->rom_data[hdr->cpi_ptr];
	g_debug ("  PCI Data");
	g_debug ("   VendorID:  0x%04x", hdr->vendor_id);
	g_debug ("   DeviceID:  0x%04x", hdr->device_id);
	g_debug ("   DevList:   0x%04x", hdr->device_list_ptr);
	g_debug ("   DataLen:   0x%04x", hdr->data_len);
	g_debug ("   DataRev:   0x%04x", hdr->data_rev);
	if (hdr->image_len < 0x0f) {
		data_str = fu_rom_get_hex_dump (&buffer[hdr->data_len], hdr->image_len);
		g_debug ("   ImageLen:  0x%04x [%s]", hdr->image_len, data_str);
	} else {
		data_str = fu_rom_get_hex_dump (&buffer[hdr->data_len], 0x0f);
		g_debug ("   ImageLen:  0x%04x [%s...]", hdr->image_len, data_str);
	}
	g_debug ("   RevLevel:  0x%04x", hdr->revision_level);
	g_debug ("   CodeType:  0x%02x [%s]", hdr->code_type,
		 fu_rom_pci_code_type_to_string (hdr->code_type));
	g_debug ("   LastImg:   0x%02x [%s]", hdr->last_image,
		 hdr->last_image == 0x80 ? "yes" : "no");
	g_debug ("   MaxRunLen: 0x%04x", hdr->max_runtime_len);
	g_debug ("   ConfigHdr: 0x%04x", hdr->config_header_ptr);
	g_debug ("   ClpPtr:    0x%04x", hdr->dmtf_clp_ptr);

	/* dump the ISBN */
	if (hdr->code_type == 0x70 &&
	    memcmp (&buffer[hdr->data_len], "ISBN", 4) == 0) {
		fu_rom_pci_print_certificate_data (&buffer[hdr->data_len],
						   hdr->image_len);
	}

	/* verify the checksum byte */
	if (hdr->image_len <= hdr->rom_len && hdr->image_len > 0) {
		buffer = hdr->rom_data;
		chksum_check = fu_rom_pci_header_get_checksum (hdr);
		if (chksum_check == 0x00) {
			g_debug ("   ChkSum:    0x%02x [valid]",
				 buffer[hdr->image_len-1]);
		} else {
			g_debug ("   ChkSum:    0x%02x [failed, got 0x%02x]",
				 buffer[hdr->image_len-1],
				 chksum_check);
		}
	} else {
		g_debug ("   ChkSum:    0x?? [unknown]");
	}
}

/**
 * fu_rom_extract_all:
 **/
gboolean
fu_rom_extract_all (FuRom *rom, const gchar *path, GError **error)
{
	FuRomPrivate *priv = rom->priv;
	FuRomPciHeader *hdr;
	guint i;

	for (i = 0; i < priv->hdrs->len; i++) {
		_cleanup_free_ gchar *fn = NULL;
		hdr = g_ptr_array_index (priv->hdrs, i);
		fn = g_strdup_printf ("%s/%02i.bin", path, i);
		g_debug ("dumping ROM #%i at 0x%04x [0x%02x] to %s",
			 i, hdr->rom_offset, hdr->rom_len, fn);
		if (hdr->rom_len == 0)
			continue;
		if (!g_file_set_contents (fn,
					  (const gchar *) hdr->rom_data,
					  (gssize) hdr->rom_len, error))
			return FALSE;
	}
	return TRUE;
}

/**
 * fu_rom_find_and_blank_serial_numbers:
 **/
static void
fu_rom_find_and_blank_serial_numbers (FuRom *rom)
{
	FuRomPrivate *priv = rom->priv;
	FuRomPciHeader *hdr;
	guint i;
	guint8 *tmp;

	/* bail if not likely */
	if (priv->kind == FU_ROM_KIND_PCI ||
	    priv->kind == FU_ROM_KIND_INTEL) {
		g_debug ("no serial numbers likely");
		return;
	}

	for (i = 0; i < priv->hdrs->len; i++) {
		hdr = g_ptr_array_index (priv->hdrs, i);
		g_debug ("looking for PPID at 0x%04x", hdr->rom_offset);
		tmp = fu_rom_pci_strstr (hdr, "PPID");
		if (tmp != NULL) {
			guint len;
			guint8 chk;
			len = fu_rom_blank_serial_numbers (tmp, hdr->rom_len - hdr->data_len);
			g_debug ("cleared %i chars @ 0x%04lx", len, tmp - &hdr->rom_data[hdr->data_len]);

			/* we have to fix the checksum */
			chk = fu_rom_pci_header_get_checksum (hdr);
			hdr->rom_data[hdr->rom_len - 1] -= chk;
			fu_rom_pci_print_header (hdr);
		}
	}
}

/**
 * fu_rom_pci_get_data:
 **/
static gboolean
fu_rom_pci_parse_data (FuRomPciHeader *hdr)
{
	guint8 *buffer;

	/* check valid */
	if (hdr == NULL ||
	    hdr->cpi_ptr == 0x0000) {
		g_debug ("No PCI DATA @ 0x%04x", hdr->rom_offset);
		return FALSE;
	}
	if (hdr->rom_len > 0 && hdr->cpi_ptr > hdr->rom_len) {
		g_debug ("Invalid PCI DATA @ 0x%04x", hdr->rom_offset);
		return FALSE;
	}

	/* gahh, CPI is out of the first chunk */
	if (hdr->cpi_ptr > hdr->rom_len) {
		g_debug ("No available PCI DATA @ 0x%04x : 0x%04x > 0x%04x",
			 hdr->rom_offset, hdr->cpi_ptr, hdr->rom_len);
		return FALSE;
	}

	/* check signature */
	buffer = &hdr->rom_data[hdr->cpi_ptr];
	if (memcmp (buffer, "PCIR", 4) != 0) {
		if (memcmp (buffer, "RGIS", 4) == 0 ||
		    memcmp (buffer, "NPDS", 4) == 0 ||
		    memcmp (buffer, "NPDE", 4) == 0) {
			g_debug ("-- using NVIDIA DATA quirk");
		} else {
			g_debug ("Not PCI DATA: %02x%02x%02x%02x [%c%c%c%c]",
				 buffer[0], buffer[1],
				 buffer[2], buffer[3],
				 buffer[0], buffer[1],
				 buffer[2], buffer[3]);
			return FALSE;
		}
	}

	/* parse */
	hdr->vendor_id = ((guint16) buffer[0x05] << 8) + buffer[0x04];
	hdr->device_id = ((guint16) buffer[0x07] << 8) + buffer[0x06];
	hdr->device_list_ptr = ((guint16) buffer[0x09] << 8) + buffer[0x08];
	hdr->data_len = ((guint16) buffer[0x0b] << 8) + buffer[0x0a];
	hdr->data_rev = buffer[0x0c];
	hdr->class_code = ((guint16) buffer[0x0f] << 16) +
			  ((guint16) buffer[0x0e] << 8) +
			  buffer[0x0d];
	hdr->image_len = (((guint16) buffer[0x11] << 8) + buffer[0x10]) * 512;
	hdr->revision_level = ((guint16) buffer[0x13] << 8) + buffer[0x12];
	hdr->code_type = buffer[0x14];
	hdr->last_image = buffer[0x15];
	hdr->max_runtime_len = (((guint16) buffer[0x17] << 8) +
				buffer[0x16]) * 512;
	hdr->config_header_ptr = ((guint16) buffer[0x19] << 8) + buffer[0x18];
	hdr->dmtf_clp_ptr = ((guint16) buffer[0x1b] << 8) + buffer[0x1a];
	return TRUE;
}

/**
 * fu_rom_pci_get_header:
 **/
static FuRomPciHeader *
fu_rom_pci_get_header (guint8 *buffer, gssize sz)
{
	FuRomPciHeader *hdr;

	/* check signature */
	if (memcmp (buffer, "\x55\xaa", 2) != 0) {
		if (memcmp (buffer, "\x56\x4e", 2) == 0) {
			g_debug ("-- using NVIDIA ROM quirk");
		} else {
			_cleanup_free_ gchar *sig_str = NULL;
			sig_str = fu_rom_get_hex_dump (buffer, 16);
			g_debug ("Not PCI ROM %s", sig_str);
			return NULL;
		}
	}

	/* decode structure */
	hdr = g_new0 (FuRomPciHeader, 1);
	hdr->rom_len = buffer[0x02] * 512;

	/* fix up misreporting */
	if (hdr->rom_len == 0) {
		g_debug ("fixing up last image size");
		hdr->rom_len = sz;
	}

	/* copy this locally to the header */
	hdr->rom_data = g_memdup (buffer, hdr->rom_len);

	/* parse out CPI */
	hdr->entry_point = ((guint32) buffer[0x05] << 16) +
			   ((guint16) buffer[0x04] << 8) +
			   buffer[0x03];
	memcpy (&hdr->reserved, &buffer[6], 18);
	hdr->cpi_ptr = ((guint16) buffer[0x19] << 8) + buffer[0x18];

	/* parse the header data */
	g_debug ("looking for PCI DATA @ 0x%04x", hdr->cpi_ptr);
	fu_rom_pci_parse_data (hdr);
	return hdr;
}

/**
 * fu_rom_find_version_pci:
 **/
static gchar *
fu_rom_find_version_pci (FuRomPciHeader *hdr)
{
	gchar *str;

	/* ARC storage */
	if (memcmp (hdr->reserved, "\0\0ARC", 5) == 0) {
		str = (gchar *) fu_rom_pci_strstr (hdr, "BIOS: ");
		if (str != NULL)
			return g_strdup (str + 6);
	}
	return NULL;
}

/**
 * fu_rom_find_version_nvidia:
 **/
static gchar *
fu_rom_find_version_nvidia (FuRomPciHeader *hdr)
{
	gchar *str;

	/* static location for some firmware */
	if (memcmp (hdr->rom_data + 0x013d, "Version ", 8) == 0)
		return g_strdup ((gchar *) &hdr->rom_data[0x013d + 8]);

	/* usual search string */
	str = (gchar *) fu_rom_pci_strstr (hdr, "Version ");
	if (str != NULL)
		return g_strdup (str + 8);

	/* broken */
	str = (gchar *) fu_rom_pci_strstr (hdr, "Vension:");
	if (str != NULL)
		return g_strdup (str + 8);
	str = (gchar *) fu_rom_pci_strstr (hdr, "Version");
	if (str != NULL)
		return g_strdup (str + 7);

	/* fallback to VBIOS */
	if (memcmp (hdr->rom_data + 0xfa, "VBIOS Ver", 9) == 0)
		return g_strdup ((gchar *) &hdr->rom_data[0xfa + 9]);
	return NULL;
}

/**
 * fu_rom_find_version_intel:
 **/
static gchar *
fu_rom_find_version_intel (FuRomPciHeader *hdr)
{
	gchar *str;

	/* 2175_RYan PC 14.34  06/06/2013  21:27:53 */
	str = (gchar *) fu_rom_pci_strstr (hdr, "Build Number:");
	if (str != NULL) {
		guint i;
		_cleanup_strv_free_ gchar **split = NULL;
		split = g_strsplit (str + 14, " ", -1);
		for (i = 0; split[i] != NULL; i++) {
			if (g_strstr_len (split[i], -1, ".") == NULL)
				continue;
			return g_strdup (split[i]);
		}
	}

	/* fallback to VBIOS */
	str = (gchar *) fu_rom_pci_strstr (hdr, "VBIOS ");
	if (str != NULL)
		return g_strdup (str + 6);
	return NULL;
}

/**
 * fu_rom_find_version_ati:
 **/
static gchar *
fu_rom_find_version_ati (FuRomPciHeader *hdr)
{
	gchar *str;

	str = (gchar *) fu_rom_pci_strstr (hdr, " VER0");
	if (str != NULL)
		return g_strdup (str + 4);

	/* broken */
	str = (gchar *) fu_rom_pci_strstr (hdr, " VR");
	if (str != NULL)
		return g_strdup (str + 4);
	return NULL;
}

/**
 * fu_rom_find_version:
 **/
static gchar *
fu_rom_find_version (FuRomKind kind, FuRomPciHeader *hdr)
{
	if (kind == FU_ROM_KIND_PCI)
		return fu_rom_find_version_pci (hdr);
	if (kind == FU_ROM_KIND_NVIDIA)
		return fu_rom_find_version_nvidia (hdr);
	if (kind == FU_ROM_KIND_INTEL)
		return fu_rom_find_version_intel (hdr);
	if (kind == FU_ROM_KIND_ATI)
		return fu_rom_find_version_ati (hdr);
	return NULL;
}

/**
 * fu_rom_load_file:
 **/
gboolean
fu_rom_load_file (FuRom *rom, GFile *file, FuRomLoadFlags flags,
		  GCancellable *cancellable, GError **error)
{
	FuRomPrivate *priv = rom->priv;
	FuRomPciHeader *hdr = NULL;
	const gssize buffer_sz = 0x400000;
	gssize sz;
	guint32 jump = 0;
	guint hdr_sz = 0;
	guint i;
	guint number_reads = 0;
	_cleanup_error_free_ GError *error_local = NULL;
	_cleanup_free_ gchar *fn = NULL;
	_cleanup_free_ gchar *id = NULL;
	_cleanup_free_ guint8 *buffer = NULL;
	_cleanup_object_unref_ GFileOutputStream *output_stream = NULL;

	g_return_val_if_fail (FU_IS_ROM (rom), FALSE);

	/* open file */
	priv->stream = G_INPUT_STREAM (g_file_read (file, cancellable, &error_local));
	if (priv->stream == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_AUTH_FAILED,
				     error_local->message);
		return FALSE;
	}

	/* we have to enable the read for devices */
	fn = g_file_get_path (file);
	if (g_str_has_prefix (fn, "/sys")) {
		output_stream = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_NONE,
						cancellable, error);
		if (output_stream == NULL)
			return FALSE;
		if (g_output_stream_write (G_OUTPUT_STREAM (output_stream), "1", 1,
					   cancellable, error) < 0)
			return FALSE;
	}

	/* read out the header */
	buffer = g_malloc (buffer_sz);
	sz = g_input_stream_read (priv->stream, buffer, buffer_sz,
				  cancellable, error);
	if (sz < 0)
		return FALSE;
	if (sz < 1024) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INVALID_FILE,
			     "Firmware too small: %" G_GSIZE_FORMAT " bytes", sz);
		return FALSE;
	}

	/* ensure we got enough data to fill the buffer */
	while (sz < buffer_sz) {
		gssize sz_chunk;
		sz_chunk = g_input_stream_read (priv->stream,
						buffer + sz,
						buffer_sz - sz,
						cancellable,
						error);
		if (sz_chunk == 0)
			break;
		g_debug ("ROM returned 0x%04x bytes, adding 0x%04x...",
			 (guint) sz, (guint) sz_chunk);
		if (sz_chunk < 0)
			return FALSE;
		sz += sz_chunk;

		/* check the firmware isn't serving us small chunks */
		if (number_reads++ > 16) {
			g_set_error_literal (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_INVALID_FILE,
					     "firmware not fulfilling requests");
			return FALSE;
		}
	}
	g_debug ("ROM buffer filled %likb/%likb", sz / 0x400, buffer_sz / 0x400);

	/* detect optional IFR header and skip to option ROM */
	if (memcmp (buffer, "NVGI", 4) == 0)
		hdr_sz = GUINT16_FROM_BE (buffer[0x15]);

	/* read all the ROM headers */
	while (sz > hdr_sz + jump) {
		guint32 jump_sz;
		g_debug ("looking for PCI ROM @ 0x%04x", hdr_sz + jump);
		hdr = fu_rom_pci_get_header (&buffer[hdr_sz + jump], sz - hdr_sz - jump);
		if (hdr == NULL) {
			gboolean found_data = FALSE;

			/* check it's not just NUL padding */
			for (i = 0; i < hdr_sz + jump; i++) {
				if (buffer[hdr_sz + jump + i] != 0x00) {
					found_data = TRUE;
					break;
				}
			}
			if (found_data) {
				g_debug ("found junk data, adding fake");
				hdr = g_new0 (FuRomPciHeader, 1);
				hdr->vendor_id = 0x0000;
				hdr->device_id = 0x0000;
				hdr->code_type = 0x00;
				hdr->last_image = 0x80;
				hdr->rom_offset = hdr_sz + jump;
				hdr->rom_len = sz - hdr->rom_offset;
				hdr->rom_data = g_memdup (&buffer[hdr->rom_offset], hdr->rom_len);
				hdr->image_len = hdr->rom_len;
				g_ptr_array_add (priv->hdrs, hdr);
			} else {
				g_debug ("ignoring padding");
			}
			break;
		}

		/* save this so we can fix checksums */
		hdr->rom_offset = hdr_sz + jump;

		/* we can't break on hdr->last_image as
		 * NVIDIA uses packed but not merged extended headers */
		g_ptr_array_add (priv->hdrs, hdr);

		/* NVIDIA don't always set a ROM size for extensions */
		jump_sz = hdr->rom_len;
		if (jump_sz == 0)
			jump_sz = hdr->image_len;
		if (jump_sz == 0x0)
			break;
		jump += jump_sz;
	}

	/* we found nothing */
	if (priv->hdrs->len == 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INVALID_FILE,
			     "Failed to detect firmware header [%02x%02x]",
			     buffer[0], buffer[1]);
		return FALSE;
	}

	/* print all headers */
	for (i = 0; i < priv->hdrs->len; i++) {
		hdr = g_ptr_array_index (priv->hdrs, i);
		fu_rom_pci_print_header (hdr);
	}

	/* find first ROM header */
	hdr = g_ptr_array_index (priv->hdrs, 0);
	priv->vendor = hdr->vendor_id;
	priv->model = hdr->device_id;
	priv->kind = FU_ROM_KIND_PCI;

	/* detect intel header */
	if (memcmp (hdr->reserved, "00000000000", 11) == 0)
		hdr_sz = (buffer[0x1b] << 8) + buffer[0x1a];
	if (hdr_sz > sz) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "firmware corrupt (overflow)");
		return FALSE;
	}

	if (memcmp (buffer + hdr_sz + 0x04, "K74", 3) == 0) {
		priv->kind = FU_ROM_KIND_NVIDIA;
	} else if (memcmp (buffer + hdr_sz, "$VBT", 4) == 0) {
		priv->kind = FU_ROM_KIND_INTEL;
	} else if (memcmp(buffer + 0x30, " 761295520", 10) == 0) {
		priv->kind = FU_ROM_KIND_ATI;
	}

	/* nothing */
	if (priv->kind == FU_ROM_KIND_UNKNOWN) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "Failed to detect firmware kind");
		return FALSE;
	}

	/* find version string */
	priv->version = fu_rom_find_version (priv->kind, hdr);
	if (priv->version != NULL) {
		g_strstrip (priv->version);
		g_strdelimit (priv->version, "\r\n ", '\0');
	}

	/* update checksum */
	if (flags & FU_ROM_LOAD_FLAG_BLANK_PPID)
		fu_rom_find_and_blank_serial_numbers (rom);
	for (i = 0; i < priv->hdrs->len; i++) {
		hdr = g_ptr_array_index (priv->hdrs, i);
		g_checksum_update (priv->checksum_wip, hdr->rom_data, hdr->rom_len);
	}

	/* update guid */
	id = g_strdup_printf ("0x%04x:0x%04x", priv->vendor, priv->model);
	priv->guid = as_utils_guid_from_string (id);
	g_debug ("using %s for %s", priv->guid, id);

	/* not known */
	if (priv->version == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "Firmware version extractor not known");
		return FALSE;
	}

	return TRUE;
}

/**
 * fu_rom_get_kind:
 **/
FuRomKind
fu_rom_get_kind (FuRom *rom)
{
	g_return_val_if_fail (FU_IS_ROM (rom), FU_ROM_KIND_UNKNOWN);
	return rom->priv->kind;
}

/**
 * fu_rom_get_version:
 **/
const gchar *
fu_rom_get_version (FuRom *rom)
{
	g_return_val_if_fail (FU_IS_ROM (rom), NULL);
	return rom->priv->version;
}

/**
 * fu_rom_get_guid:
 **/
const gchar *
fu_rom_get_guid (FuRom *rom)
{
	g_return_val_if_fail (FU_IS_ROM (rom), NULL);
	return rom->priv->guid;
}

/**
 * fu_rom_get_vendor:
 **/
guint16
fu_rom_get_vendor (FuRom *rom)
{
	g_return_val_if_fail (FU_IS_ROM (rom), 0x0000);
	return rom->priv->vendor;
}

/**
 * fu_rom_get_model:
 **/
guint16
fu_rom_get_model (FuRom *rom)
{
	g_return_val_if_fail (FU_IS_ROM (rom), 0x0000);
	return rom->priv->model;
}

/**
 * fu_rom_get_checksum:
 *
 * This returns the checksum of the firmware.
 **/
const gchar *
fu_rom_get_checksum (FuRom *rom)
{
	FuRomPrivate *priv = rom->priv;
	return g_checksum_get_string (priv->checksum_wip);
}

/**
 * fu_rom_class_init:
 **/
static void
fu_rom_class_init (FuRomClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = fu_rom_finalize;
	g_type_class_add_private (klass, sizeof (FuRomPrivate));
}

/**
 * fu_rom_init:
 **/
static void
fu_rom_init (FuRom *rom)
{
	rom->priv = FU_ROM_GET_PRIVATE (rom);
	rom->priv->checksum_wip = g_checksum_new (G_CHECKSUM_SHA1);
	rom->priv->hdrs = g_ptr_array_new_with_free_func ((GDestroyNotify) fu_rom_pci_header_free);
}

/**
 * fu_rom_finalize:
 **/
static void
fu_rom_finalize (GObject *object)
{
	FuRom *rom = FU_ROM (object);
	FuRomPrivate *priv = rom->priv;

	g_checksum_free (priv->checksum_wip);
	g_free (priv->version);
	g_free (priv->guid);
	g_ptr_array_unref (priv->hdrs);
	if (priv->stream != NULL)
		g_object_unref (priv->stream);

	G_OBJECT_CLASS (fu_rom_parent_class)->finalize (object);
}

/**
 * fu_rom_new:
 **/
FuRom *
fu_rom_new (void)
{
	FuRom *rom;
	rom = g_object_new (FU_TYPE_ROM, NULL);
	return FU_ROM (rom);
}
