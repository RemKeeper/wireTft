// BLE GATT transport for setup and control (NimBLE peripheral, one link).
//
// Service 3f6a0001-8c2e-4d1b-9a77-5e9b1c7d0a10
//   RX 3f6a0002-... write / write without response, first byte is the frame type:
//     0x01 JSON fragment, more follows   0x02 last JSON fragment
//     0x10 image data: u32 little-endian byte offset, then payload
//   TX 3f6a0003-... notify, first byte is the frame type:
//     0x01 response fragment, more follows   0x02 last response fragment
//     0x03 event, one complete JSON object
//
// The host task only buffers commands and hands image data to
// bleOnImageData(). Commands are executed and every notification is sent from
// the Arduino loop task, so responses never interleave.
#pragma once
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// Arduino releases the BLE controller memory at boot unless this reports use.
extern "C" bool bleInUse(void) { return true; }

enum : uint8_t { BLE_RX_JSON_MORE = 0x01, BLE_RX_JSON_END = 0x02, BLE_RX_IMAGE = 0x10 };
enum : uint8_t { BLE_TX_MORE = 0x01, BLE_TX_END = 0x02, BLE_TX_EVENT = 0x03 };

static const ble_uuid128_t BLE_SERVICE_UUID =
	BLE_UUID128_INIT(0x10, 0x0a, 0x7d, 0x1c, 0x9b, 0x5e, 0x77, 0x9a, 0x1b, 0x4d, 0x2e, 0x8c, 0x01, 0x00, 0x6a, 0x3f);
static const ble_uuid128_t BLE_RX_UUID =
	BLE_UUID128_INIT(0x10, 0x0a, 0x7d, 0x1c, 0x9b, 0x5e, 0x77, 0x9a, 0x1b, 0x4d, 0x2e, 0x8c, 0x02, 0x00, 0x6a, 0x3f);
static const ble_uuid128_t BLE_TX_UUID =
	BLE_UUID128_INIT(0x10, 0x0a, 0x7d, 0x1c, 0x9b, 0x5e, 0x77, 0x9a, 0x1b, 0x4d, 0x2e, 0x8c, 0x03, 0x00, 0x6a, 0x3f);

static constexpr size_t BLE_CMD_MAX = 768;
static volatile uint16_t bleConn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t bleTxHandle;
static uint8_t bleOwnAddr;
static char bleName[16];
static char bleCmd[BLE_CMD_MAX + 1];
static size_t bleCmdLen;
// Set by the host task; the loop clears them after acting.
static volatile bool bleCmdReady, bleCmdOverflow, bleDisconnected;

static void bleOnImageData(uint32_t offset, const uint8_t *data, size_t len);
static void bleAdvertise();

static int bleAccess(uint16_t, uint16_t, ble_gatt_access_ctxt *ctxt, void *)
{
	if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
	uint8_t buf[512];
	uint16_t len = 0;
	if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0 || !len) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
	const uint8_t type = buf[0];
	if (type == BLE_RX_JSON_MORE || type == BLE_RX_JSON_END) {
		// The client waits for each response; a command still queued means it did not.
		if (bleCmdReady) return BLE_ATT_ERR_UNLIKELY;
		if (bleCmdLen + len - 1 > BLE_CMD_MAX) bleCmdOverflow = true;
		else { memcpy(bleCmd + bleCmdLen, buf + 1, len - 1); bleCmdLen += len - 1; }
		if (type == BLE_RX_JSON_END) { bleCmd[bleCmdLen] = 0; bleCmdReady = true; }
		return 0;
	}
	if (type == BLE_RX_IMAGE && len > 5) {
		uint32_t offset;
		memcpy(&offset, buf + 1, 4);
		bleOnImageData(offset, buf + 5, len - 5);
		return 0;
	}
	return BLE_ATT_ERR_UNLIKELY;
}

// Filled in by bleStart(); the zeroed last entries terminate each table.
static ble_gatt_chr_def bleCharacteristics[3];
static ble_gatt_svc_def bleServices[2];

static bool bleConnected() { return bleConn != BLE_HS_CONN_HANDLE_NONE; }

// Loop task only. Retries while the controller has no free buffers.
static bool bleNotify(const uint8_t *data, size_t len)
{
	for (int tries = 0; tries < 100; ++tries) {
		const uint16_t conn = bleConn;
		if (conn == BLE_HS_CONN_HANDLE_NONE) return false;
		os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
		if (om) {
			const int rc = ble_gatts_notify_custom(conn, bleTxHandle, om);
			if (rc == 0) return true;
			if (rc != BLE_HS_ENOMEM) return false;
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	return false;
}

static void bleSend(uint8_t kind, const char *json)
{
	const uint16_t conn = bleConn;
	if (conn == BLE_HS_CONN_HANDLE_NONE) return;
	const uint16_t mtu = ble_att_mtu(conn);
	size_t room = (mtu > 23 ? mtu : 23) - 3 - 1;
	if (room > 500) room = 500;
	uint8_t frame[501];
	size_t len = strlen(json);
	if (kind == BLE_TX_EVENT && len > room) return;
	do {
		const size_t n = len < room ? len : room;
		len -= n;
		frame[0] = kind == BLE_TX_EVENT ? BLE_TX_EVENT : len ? BLE_TX_MORE : BLE_TX_END;
		memcpy(frame + 1, json, n);
		json += n;
		if (!bleNotify(frame, n + 1)) return;
	} while (len);
}

static void bleRespond(const char *json) { bleSend(BLE_TX_END, json); }
static void bleEvent(const char *json) { bleSend(BLE_TX_EVENT, json); }

// Payload bytes per image write for the negotiated MTU.
static size_t bleImageChunk()
{
	const uint16_t mtu = bleConnected() ? ble_att_mtu(bleConn) : 23;
	size_t n = (mtu > 23 ? mtu : 23) - 3 - 5;
	return n > 500 ? 500 : n;
}

static int bleGapEvent(ble_gap_event *e, void *)
{
	switch (e->type) {
	case BLE_GAP_EVENT_CONNECT:
		if (e->connect.status != 0) { bleAdvertise(); break; }
		bleConn = e->connect.conn_handle;
		{
			// Short enough for image transfer, long enough to leave Wi-Fi airtime.
			ble_gap_upd_params p{};
			p.itvl_min = 12; p.itvl_max = 24; p.latency = 0; p.supervision_timeout = 400;
			ble_gap_update_params(e->connect.conn_handle, &p);
		}
		break;
	case BLE_GAP_EVENT_DISCONNECT:
		bleConn = BLE_HS_CONN_HANDLE_NONE;
		bleDisconnected = true;
		bleAdvertise();
		break;
	case BLE_GAP_EVENT_ADV_COMPLETE:
		if (!bleConnected()) bleAdvertise();
		break;
	default:
		break;
	}
	return 0;
}

static void bleAdvertise()
{
	ble_hs_adv_fields adv{};
	adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
	adv.uuids128 = &BLE_SERVICE_UUID;
	adv.num_uuids128 = 1;
	adv.uuids128_is_complete = 1;
	if (ble_gap_adv_set_fields(&adv) != 0) return;
	ble_hs_adv_fields rsp{};
	rsp.name = (const uint8_t *)bleName;
	rsp.name_len = strlen(bleName);
	rsp.name_is_complete = 1;
	if (ble_gap_adv_rsp_set_fields(&rsp) != 0) return;
	ble_gap_adv_params params{};
	params.conn_mode = BLE_GAP_CONN_MODE_UND;
	params.disc_mode = BLE_GAP_DISC_MODE_GEN;
	params.itvl_min = 160; // 100 ms
	params.itvl_max = 320; // 200 ms
	ble_gap_adv_start(bleOwnAddr, nullptr, BLE_HS_FOREVER, &params, bleGapEvent, nullptr);
}

static void bleOnSync()
{
	ble_hs_util_ensure_addr(0);
	ble_hs_id_infer_auto(0, &bleOwnAddr);
	bleAdvertise();
}

static void bleHostTask(void *)
{
	nimble_port_run();
	nimble_port_freertos_deinit();
}

static bool bleStart(const char *name)
{
	strlcpy(bleName, name, sizeof(bleName));
	bleCharacteristics[0].uuid = &BLE_RX_UUID.u;
	bleCharacteristics[0].access_cb = bleAccess;
	bleCharacteristics[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;
	bleCharacteristics[1].uuid = &BLE_TX_UUID.u;
	bleCharacteristics[1].access_cb = bleAccess;
	bleCharacteristics[1].flags = BLE_GATT_CHR_F_NOTIFY;
	bleCharacteristics[1].val_handle = &bleTxHandle;
	bleServices[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
	bleServices[0].uuid = &BLE_SERVICE_UUID.u;
	bleServices[0].characteristics = bleCharacteristics;
	if (nimble_port_init() != ESP_OK) return false;
	ble_hs_cfg.sync_cb = bleOnSync;
	ble_svc_gap_init();
	ble_svc_gatt_init();
	if (ble_gatts_count_cfg(bleServices) != 0 || ble_gatts_add_svcs(bleServices) != 0) return false;
	ble_svc_gap_device_name_set(bleName);
	nimble_port_freertos_init(bleHostTask);
	return true;
}
