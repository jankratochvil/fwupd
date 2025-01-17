/*
 * Copyright (C) 2019 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2019 Synaptics Inc
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <string.h>

#include "fu-synaprom-common.h"
#include "fu-synaprom-config.h"
#include "fu-synaprom-firmware.h"

struct _FuSynapromConfig {
	FuDevice		 parent_instance;
	FuSynapromDevice	*device;
	guint32			 configid1;		/* config ID1 */
	guint32			 configid2;		/* config ID2 */
};

/* Iotas can exceed the size of available RAM in the part.
 * In order to allow the host to read them the IOTA_FIND command supports
 * transferring iotas with multiple commands */
typedef struct __attribute__((packed)) {
	guint16			 itype;			/* type of iotas to find */
	guint16			 flags;			/* flags, see below */
	guint8			 maxniotas;		/* maximum number of iotas to return, 0 = unlimited */
	guint8			 firstidx;		/* first index of iotas to return */
	guint8			 dummy[2];
	guint32			 offset;		/* byte offset of data to return */
	guint32			 nbytes;		/* maximum number of bytes to return */
} FuSynapromCmdIotaFind;

/* this is followed by a chain of iotas, as follows */
typedef struct __attribute__((packed)) {
	guint16		 status;
	guint32		 fullsize;
	guint16		 nbytes;
	guint16		 itype;
} FuSynapromReplyIotaFindHdr;

/* this iota contains the configuration id and version */
typedef struct __attribute__((packed)) {
	guint32		 config_id1;	/* YYMMDD */
	guint32		 config_id2;	/* HHMMSS */
	guint16		 version;
	guint16		 unused[3];
} FuSynapromIotaConfigVersion;

#define FU_SYNAPROM_CMD_IOTA_FIND_FLAGS_ALLIOTAS	0x0001	/* itype ignored*/
#define FU_SYNAPROM_CMD_IOTA_FIND_FLAGS_READMAX		0x0002	/* nbytes ignored */
#define FU_SYNAPROM_MAX_IOTA_READ_SIZE			(64 * 1024) /* max size of iota data returned */

#define FU_SYNAPROM_IOTA_ITYPE_CONFIG_VERSION		0x0009	/* Configuration id and version */

G_DEFINE_TYPE (FuSynapromConfig, fu_synaprom_config, FU_TYPE_DEVICE)

enum {
	PROP_0,
	PROP_DEVICE,
	PROP_LAST
};

static gboolean
fu_synaprom_config_setup (FuDevice *device, GError **error)
{
	FuSynapromConfig *self = FU_SYNAPROM_CONFIG (device);
	FuSynapromCmdIotaFind cmd = { 0x0 };
	FuSynapromIotaConfigVersion cfg;
	FuSynapromReplyIotaFindHdr hdr;
	g_autofree gchar *version = NULL;
	g_autoptr(GByteArray) reply = NULL;
	g_autoptr(GByteArray) request = NULL;

	/* get IOTA */
	cmd.itype = GUINT16_TO_LE((guint16)FU_SYNAPROM_IOTA_ITYPE_CONFIG_VERSION);
	cmd.flags = GUINT16_TO_LE((guint16)FU_SYNAPROM_CMD_IOTA_FIND_FLAGS_READMAX);
	request = fu_synaprom_request_new (FU_SYNAPROM_CMD_IOTA_FIND, &cmd, sizeof(cmd));
	reply = fu_synaprom_reply_new (sizeof(FuSynapromReplyIotaFindHdr) + FU_SYNAPROM_MAX_IOTA_READ_SIZE);
	if (!fu_synaprom_device_cmd_send (self->device, request, reply, 5000, error))
		return FALSE;
	if (reply->len < sizeof(hdr) + sizeof(cfg)) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_SUPPORTED,
			     "CFG return data invalid size: 0x%04x",
			     reply->len);
		return FALSE;
	}
	memcpy (&hdr, reply->data, sizeof(hdr));
	if (GUINT32_FROM_LE(hdr.itype) != FU_SYNAPROM_IOTA_ITYPE_CONFIG_VERSION) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_SUPPORTED,
			     "CFG iota had invalid itype: 0x%04x",
			     GUINT32_FROM_LE(hdr.itype));
		return FALSE;
	}
	memcpy (&cfg, reply->data + sizeof(hdr), sizeof(cfg));
	self->configid1 = GUINT32_FROM_LE(cfg.config_id1);
	self->configid2 = GUINT32_FROM_LE(cfg.config_id2);
	g_debug ("id1=%u, id2=%u, ver=%u",
		 self->configid1, self->configid2,
		 GUINT16_FROM_LE(cfg.version));

	/* no downgrades are allowed */
	version = g_strdup_printf ("%04u", GUINT16_FROM_LE(cfg.version));
	fu_device_set_version (FU_DEVICE (self), version, FWUPD_VERSION_FORMAT_PLAIN);
	fu_device_set_version_lowest (FU_DEVICE (self), version);
	return TRUE;
}

static GBytes *
fu_synaprom_config_prepare_firmware (FuDevice *device,
				     GBytes *fw,
				     FwupdInstallFlags flags,
				     GError **error)
{
	FuSynapromConfig *self = FU_SYNAPROM_CONFIG (device);
	FuSynapromFirmwareCfgHeader hdr;
	g_autoptr(GBytes) blob = NULL;
	g_autoptr(GPtrArray) firmware = NULL;
	guint32 product;
	guint32 id1;

	/* parse the firmware */
	fu_device_set_status (device, FWUPD_STATUS_DECOMPRESSING);
	firmware = fu_synaprom_firmware_new (fw, error);
	if (firmware == NULL)
		return NULL;

	/* check the update header product and version */
	blob = fu_synaprom_firmware_get_bytes_by_tag (firmware,
						      FU_SYNAPROM_FIRMWARE_TAG_CFG_HEADER,
						      error);
	if (blob == NULL)
		return NULL;
	if (g_bytes_get_size (blob) != sizeof(hdr)) {
		g_set_error_literal (error,
				     G_IO_ERROR,
				     G_IO_ERROR_INVALID_DATA,
				     "CFG metadata is invalid");
		return NULL;
	}
	memcpy (&hdr, g_bytes_get_data (blob, NULL), sizeof(hdr));
	product = GUINT32_FROM_LE(hdr.product);
	if (product != FU_SYNAPROM_PRODUCT_PROMETHEUS) {
		if (flags & FWUPD_INSTALL_FLAG_FORCE) {
			g_warning ("CFG metadata not compatible, "
				   "got 0x%02x expected 0x%02x",
				   product, (guint) FU_SYNAPROM_PRODUCT_PROMETHEUS);
		} else {
			g_set_error (error,
				     G_IO_ERROR,
				     G_IO_ERROR_NOT_SUPPORTED,
				     "CFG metadata not compatible, "
				     "got 0x%02x expected 0x%02x",
				     product, (guint) FU_SYNAPROM_PRODUCT_PROMETHEUS);
			return NULL;
		}
	}
	id1 = GUINT32_FROM_LE(hdr.id1);
	if (id1 != self->configid1) {
		if (flags & FWUPD_INSTALL_FLAG_FORCE) {
			g_warning ("CFG version not compatible, "
				   "got %u expected %u",
				   id1, self->configid1);
		} else {
			g_set_error (error,
				     G_IO_ERROR,
				     G_IO_ERROR_NOT_SUPPORTED,
				     "CFG version not compatible, "
				     "got %u expected %u",
				     id1, self->configid1);
			return NULL;
		}
	}

	/* get payload */
	return fu_synaprom_firmware_get_bytes_by_tag (firmware,
						      FU_SYNAPROM_FIRMWARE_TAG_CFG_PAYLOAD,
						      error);
}

static gboolean
fu_synaprom_config_write_firmware (FuDevice *device,
				   GBytes *fw,
				   FwupdInstallFlags flags,
				   GError **error)
{
	FuSynapromConfig *self = FU_SYNAPROM_CONFIG (device);
	/* I assume the CFG/MFW difference is detected in the device...*/
	return fu_synaprom_device_write_fw (self->device, fw, error);
}

static void
fu_synaprom_config_init (FuSynapromConfig *self)
{
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_set_logical_id (FU_DEVICE (self), "cfg");
	fu_device_set_name (FU_DEVICE (self), "Prometheus IOTA Config");
}

static void
fu_synaprom_config_finalize (GObject *obj)
{
	FuSynapromConfig *self = FU_SYNAPROM_CONFIG (obj);
	g_object_unref (self->device);
}

static void
fu_synaprom_config_constructed (GObject *obj)
{
	FuSynapromConfig *self = FU_SYNAPROM_CONFIG (obj);
	g_autofree gchar *devid = NULL;

	/* append the firmware kind to the generated GUID */
	devid = g_strdup_printf ("USB\\VID_%04X&PID_%04X-cfg",
				 fu_usb_device_get_vid (FU_USB_DEVICE (self->device)),
				 fu_usb_device_get_pid (FU_USB_DEVICE (self->device)));
	fu_device_add_instance_id (FU_DEVICE (self), devid);

	G_OBJECT_CLASS (fu_synaprom_config_parent_class)->constructed (obj);
}

static void
fu_synaprom_config_get_property (GObject *obj, guint prop_id,
				 GValue *value, GParamSpec *pspec)
{
	FuSynapromConfig *self = FU_SYNAPROM_CONFIG (obj);
	switch (prop_id) {
	case PROP_DEVICE:
		g_value_set_object (value, self->device);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static void
fu_synaprom_config_set_property (GObject *obj, guint prop_id,
				 const GValue *value, GParamSpec *pspec)
{
	FuSynapromConfig *self = FU_SYNAPROM_CONFIG (obj);
	switch (prop_id) {
	case PROP_DEVICE:
		g_set_object (&self->device, g_value_get_object (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static gboolean
fu_synaprom_config_open (FuDevice *device, GError **error)
{
	FuSynapromConfig *self = FU_SYNAPROM_CONFIG (device);
	return fu_device_open (FU_DEVICE (self->device), error);
}

static gboolean
fu_synaprom_config_close (FuDevice *device, GError **error)
{
	FuSynapromConfig *self = FU_SYNAPROM_CONFIG (device);
	return fu_device_close (FU_DEVICE (self->device), error);
}

static gboolean
fu_synaprom_config_attach (FuDevice *device, GError **error)
{
	FuSynapromConfig *self = FU_SYNAPROM_CONFIG (device);
	return fu_device_attach (FU_DEVICE (self->device), error);
}

static gboolean
fu_synaprom_config_detach (FuDevice *device, GError **error)
{
	FuSynapromConfig *self = FU_SYNAPROM_CONFIG (device);
	return fu_device_detach (FU_DEVICE (self->device), error);
}

static void
fu_synaprom_config_class_init (FuSynapromConfigClass *klass)
{
	FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GParamSpec *pspec;

	object_class->constructed = fu_synaprom_config_constructed;
	object_class->finalize = fu_synaprom_config_finalize;
	object_class->get_property = fu_synaprom_config_get_property;
	object_class->set_property = fu_synaprom_config_set_property;
	klass_device->write_firmware = fu_synaprom_config_write_firmware;
	klass_device->prepare_firmware = fu_synaprom_config_prepare_firmware;
	klass_device->open = fu_synaprom_config_open;
	klass_device->close = fu_synaprom_config_close;
	klass_device->setup = fu_synaprom_config_setup;
	klass_device->attach = fu_synaprom_config_attach;
	klass_device->detach = fu_synaprom_config_detach;

	pspec = g_param_spec_object ("device", NULL, NULL,
				     FU_TYPE_SYNAPROM_DEVICE,
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT |
				     G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_DEVICE, pspec);
}

FuSynapromConfig *
fu_synaprom_config_new (FuSynapromDevice *device)
{
	FuSynapromConfig *self;
	self = g_object_new (FU_TYPE_SYNAPROM_CONFIG,
			     "device", device,
			     NULL);
	return FU_SYNAPROM_CONFIG (self);
}
