#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/bt.h>
#include <psp2kern/ctrl.h>
#include <psp2/touch.h>
#include <taihen.h>
#include "log.h"

#define DS3_VID 0x054C
#define DS3_PID 0x0268

#define DS3_ANALOG_THRESHOLD 3

#define abs(x) (((x) < 0) ? -(x) : (x))

struct ds3_input_report {
	unsigned char report_id;
	unsigned char unk0;

	unsigned char select : 1;
	unsigned char l3     : 1;
	unsigned char r3     : 1;
	unsigned char start  : 1;
	unsigned char up     : 1;
	unsigned char right  : 1;
	unsigned char down   : 1;
	unsigned char left   : 1;

	unsigned char l2       : 1;
	unsigned char r2       : 1;
	unsigned char l1       : 1;
	unsigned char r1       : 1;
	unsigned char triangle : 1;
	unsigned char circle   : 1;
	unsigned char cross    : 1;
	unsigned char square   : 1;

	unsigned char ps       : 1;
	unsigned char not_used : 7;

	unsigned char unk1;

	unsigned char left_x;
	unsigned char left_y;
	unsigned char right_x;
	unsigned char right_y;

	unsigned int unk2;

	unsigned char up_sens;
	unsigned char right_sens;
	unsigned char down_sens;
	unsigned char left_sens;

	unsigned char L2_sens;
	unsigned char R2_sens;
	unsigned char L1_sens;
	unsigned char R1_sens;

	unsigned char triangle_sens;
	unsigned char circle_sens;
	unsigned char cross_sens;
	unsigned char square_sens;

	unsigned short unk3;
	unsigned char unk4;

	unsigned char status;
	unsigned char power_rating;
	unsigned char comm_status;
	unsigned int unk5;
	unsigned int unk6;
	unsigned char unk7;

	unsigned short accel_x;
	unsigned short accel_y;
	unsigned short accel_z;

	union {
		unsigned short gyro_z;
		unsigned short roll;
	};
} __attribute__((packed, aligned(32)));

static SceUID bt_mempool_uid = -1;
static SceUID bt_thread_uid = -1;
static SceUID bt_cb_uid = -1;
static int bt_thread_run = 1;

static int ds3_connected = 0;
static unsigned int ds3_mac0 = 0;
static unsigned int ds3_mac1 = 0;

static struct ds3_input_report ds3_input;

/*static SceUID SceBt_sub_2292CE4_0x2292D18_patch_uid = -1;
static tai_hook_ref_t SceBt_sub_22999C8_ref;*/
static SceUID SceBt_sub_22999C8_hook_uid = -1;
static tai_hook_ref_t SceBt_sub_22999C8_ref;
static SceUID SceBt_sub_22947E4_hook_uid = -1;
static tai_hook_ref_t SceBt_sub_22947E4_ref;
static tai_hook_ref_t SceCtrl_sceCtrlReadBufferPositive2_ref;
static SceUID SceCtrl_sceCtrlReadBufferPositive2_hook_uid = -1;
static tai_hook_ref_t SceCtrl_sceCtrlPeekBufferPositive2_ref;
static SceUID SceCtrl_sceCtrlPeekBufferPositive2_hook_uid = -1;

static inline void ds3_input_reset(void)
{
	memset(&ds3_input, 0, sizeof(ds3_input));
}

static int is_ds3(const unsigned short vid_pid[2])
{
	return vid_pid[0] == DS3_VID && vid_pid[1] == DS3_PID;
}

static inline void *mempool_alloc(unsigned int size)
{
	return ksceKernelAllocHeapMemory(bt_mempool_uid, size);
}

static inline void mempool_free(void *ptr)
{
	ksceKernelFreeHeapMemory(bt_mempool_uid, ptr);
}

static int ds3_send_report(unsigned int mac0, unsigned int mac1, uint8_t flags, uint8_t report,
			    size_t len, const void *data)
{
	SceBtHidRequest *req;
	unsigned char *buf;

	req = mempool_alloc(sizeof(*req));
	if (!req) {
		LOG("Error allocatin BT HID Request\n");
		return -1;
	}

	if ((buf = mempool_alloc((len + 1) * sizeof(*buf))) == NULL) {
		LOG("Memory allocation error (mesg array)\n");
		return -1;
	}

	buf[0] = report;
	memcpy(buf + 1, data, len);

	memset(req, 0, sizeof(*req));
	req->type = 1; // 0xA2 -> type = 1
	req->buffer = buf;
	req->length = len + 1;
	req->next = req;

	TEST_CALL(ksceBtHidTransfer, mac0, mac1, req);

	mempool_free(buf);
	mempool_free(req);

	return 0;
}

static int ds3_send_feature_report(unsigned int mac0, unsigned int mac1, uint8_t flags, uint8_t report,
			    size_t len, const void *data)
{
	SceBtHidRequest *req;
	unsigned char *buf;

	req = mempool_alloc(sizeof(*req));
	if (!req) {
		LOG("Error allocatin BT HID Request\n");
		return -1;
	}

	if ((buf = mempool_alloc((len + 1) * sizeof(*buf))) == NULL) {
		LOG("Memory allocation error (mesg array)\n");
		return -1;
	}

	buf[0] = report;
	memcpy(buf + 1, data, len);

	memset(req, 0, sizeof(*req));
	req->type = 3; // 0x53 -> type = 3
	req->buffer = buf;
	req->length = len + 1;
	req->next = req;

	TEST_CALL(ksceBtHidTransfer, mac0, mac1, req);

	mempool_free(buf);
	mempool_free(req);

	return 0;
}


static int ds3_send_leds_rumble(unsigned int mac0, unsigned int mac1)
{
	static const unsigned char led_pattern[] = {
		0x0, 0x02, 0x04, 0x08, 0x10, 0x12, 0x14, 0x18, 0x1A, 0x1C, 0x1E
	};

	unsigned char buf[] = {
		0x01,
		0x00, //Padding
		0x00, 0x00, 0x00, 0x00, //Rumble (r, r, l, l)
		0x00, 0x00, 0x00, 0x00, //Padding
		0x00, /* LED_1 = 0x02, LED_2 = 0x04, ... */
		0xff, 0x27, 0x10, 0x00, 0x32, /* LED_4 */
		0xff, 0x27, 0x10, 0x00, 0x32, /* LED_3 */
		0xff, 0x27, 0x10, 0x00, 0x32, /* LED_2 */
		0xff, 0x27, 0x10, 0x00, 0x32, /* LED_1 */
		0x00, 0x00, 0x00, 0x00, 0x00  /* LED_5 (not soldered) */
	};

	buf[8] = led_pattern[1]; /* Turn on LED 1 */

	if (ds3_send_report(mac0, mac1, 0, 0x01, sizeof(buf), buf)) {
		LOG("Send report error\n");
		return -1;
	}

	return 0;
}

static int ds3_set_operational(unsigned int mac0, unsigned int mac1)
{
	unsigned char data[] = {
		0x42, 0x03, 0x00, 0x00
	};

	if (ds3_send_feature_report(mac0, mac1, 0, 0xF4, sizeof(data), data)) {
		LOG("Set operational error\n");
		return -1;
	}

	return 0;
}

static void reset_input_emulation()
{
	ksceCtrlSetButtonEmulation(0, 0, 0, 0, 32);
	ksceCtrlSetAnalogEmulation(0, 0, 0x80, 0x80, 0x80, 0x80,
		0x80, 0x80, 0x80, 0x80, 0);
}

static void set_input_emulation(struct ds3_input_report *ds3)
{
	unsigned int buttons = 0;
	int js_moved = 0;

	if (ds3->cross)
		buttons |= SCE_CTRL_CROSS;
	if (ds3->circle)
		buttons |= SCE_CTRL_CIRCLE;
	if (ds3->triangle)
		buttons |= SCE_CTRL_TRIANGLE;
	if (ds3->square)
		buttons |= SCE_CTRL_SQUARE;

	if (ds3->up)
		buttons |= SCE_CTRL_UP;
	if (ds3->right)
		buttons |= SCE_CTRL_RIGHT;
	if (ds3->down)
		buttons |= SCE_CTRL_DOWN;
	if (ds3->left)
		buttons |= SCE_CTRL_LEFT;

	if (ds3->l1)
		buttons |= SCE_CTRL_L1;
	if (ds3->r1)
		buttons |= SCE_CTRL_R1;

	if (ds3->l2)
		buttons |= SCE_CTRL_LTRIGGER;
	if (ds3->r2)
		buttons |= SCE_CTRL_RTRIGGER;

	if (ds3->l3)
		buttons |= SCE_CTRL_L3;
	if (ds3->r3)
		buttons |= SCE_CTRL_R3;

	if (ds3->select)
		buttons |= SCE_CTRL_SELECT;
	if (ds3->start)
		buttons |= SCE_CTRL_START;
	if (ds3->ps)
		buttons |= SCE_CTRL_INTERCEPTED;

	if ((abs(ds3->left_x - 128) > DS3_ANALOG_THRESHOLD) ||
	    (abs(ds3->left_y - 128) > DS3_ANALOG_THRESHOLD) ||
	    (abs(ds3->right_x - 128) > DS3_ANALOG_THRESHOLD) ||
	    (abs(ds3->right_y - 128) > DS3_ANALOG_THRESHOLD)) {
		js_moved = 1;
	}

	ksceCtrlSetButtonEmulation(0, 0, buttons, buttons, 32);

	ksceCtrlSetAnalogEmulation(0, 0, ds3->left_x, ds3->left_y,
		ds3->right_x, ds3->right_y, ds3->left_x, ds3->left_y,
		ds3->right_x, ds3->right_y, 1);

	if (buttons != 0 || js_moved)
		ksceKernelPowerTick(0);
}


static void patch_analogdata(int port, SceCtrlData *pad_data, int count,
			    struct ds3_input_report *ds3)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		SceCtrlData k_data;

		ksceKernelMemcpyUserToKernel(&k_data, (uintptr_t)pad_data, sizeof(k_data));
		if (abs(ds3->left_x - 128) > DS3_ANALOG_THRESHOLD)
			k_data.lx = ds3->left_x;
		if (abs(ds3->left_y - 128) > DS3_ANALOG_THRESHOLD)
			k_data.ly = ds3->left_y;
		if (abs(ds3->right_x - 128) > DS3_ANALOG_THRESHOLD)
			k_data.rx = ds3->right_x;
		if (abs(ds3->right_y - 128) > DS3_ANALOG_THRESHOLD)
			k_data.ry = ds3->right_y;
		ksceKernelMemcpyKernelToUser((uintptr_t)pad_data, &k_data, sizeof(k_data));

		pad_data++;
	}
}

static int SceCtrl_sceCtrlPeekBufferPositive2_hook_func(int port, SceCtrlData *pad_data, int count)
{
	int ret = TAI_CONTINUE(int, SceCtrl_sceCtrlPeekBufferPositive2_ref, port, pad_data, count);

	if (ret >= 0 && ds3_connected)
		patch_analogdata(port, pad_data, count, &ds3_input);

	return ret;
}

static int SceCtrl_sceCtrlReadBufferPositive2_hook_func(int port, SceCtrlData *pad_data, int count)
{
	int ret = TAI_CONTINUE(int, SceCtrl_sceCtrlReadBufferPositive2_ref, port, pad_data, count);

	if (ret >= 0 && ds3_connected)
		patch_analogdata(port, pad_data, count, &ds3_input);

	return ret;
}

static void enqueue_read_request(unsigned int mac0, unsigned int mac1,
				 SceBtHidRequest *request, unsigned char *buffer,
				 unsigned int length)
{
	memset(request, 0, sizeof(*request));
	memset(buffer, 0, length);

	request->type = 0;
	request->buffer = buffer;
	request->length = length;
	request->next = request;

	ksceBtHidTransfer(mac0, mac1, request);
}

static int SceBt_sub_22999C8_hook_func(void *dev_base_ptr, int r1)
{
	unsigned int flags = *(unsigned int *)(r1 + 4);

	if (dev_base_ptr && !(flags & 2)) {
		const void *dev_info = *(const void **)(dev_base_ptr + 0x14A4);
		const unsigned short *vid_pid = (const unsigned short *)(dev_info + 0x28);

		if (is_ds3(vid_pid)) {
			unsigned int *v8_ptr = (unsigned int *)(*(unsigned int *)dev_base_ptr + 8);

			/*
			 * We need to enable the following bits in order to make the Vita
			 * accept the new connection, otherwise it will refuse it.
			 */
			*v8_ptr |= 0x11000;
		}
	}

	return TAI_CONTINUE(int, SceBt_sub_22999C8_ref, dev_base_ptr, r1);
}

static void *SceBt_sub_22947E4_hook_func(unsigned int r0, unsigned int r1, unsigned long long r2)
{
	void *ret = TAI_CONTINUE(void *, SceBt_sub_22947E4_ref, r0, r1, r2);

	if (ret) {
		/*
		 * We have to enable this bit in order to make the Vita
		 * accept the controller.
		 */
		*(unsigned int *)(ret + 0x24) |= 0x1000;
	}

	return ret;
}

static int bt_cb_func(int notifyId, int notifyCount, int notifyArg, void *common)
{
	static SceBtHidRequest hid_request;
	static unsigned char recv_buff[0x100];

	while (1) {
		int ret;
		SceBtEvent hid_event;

		memset(&hid_event, 0, sizeof(hid_event));

		do {
			ret = ksceBtReadEvent(&hid_event, 1);
		} while (ret == SCE_BT_ERROR_CB_OVERFLOW);

		if (ret <= 0) {
			break;
		}

		LOG("->Event:");
		for (int i = 0; i < 0x10; i++)
			LOG(" %02X", hid_event.data[i]);
		LOG("\n");

		/*
		 * If we get an event with a MAC, and the MAC is different
		 * from the connected DS3, skip the event.
		 */
		if (ds3_connected) {
			if (hid_event.mac0 != ds3_mac0 || hid_event.mac1 != ds3_mac1)
				continue;
		}

		switch (hid_event.id) {
		case 0x01: /* Inquiry result event */
			break;

		case 0x02: /* Inquiry stop event */
			break;

		case 0x04: /* Link key request? event */
			break;

		case 0x05: { /* Connection accepted event */
			unsigned short vid_pid[2] = {0, 0};
			char name[0x79];
			unsigned int result1;
			unsigned int result2;

			/*
			 * Getting the VID/PID or device name of the DS3
			 * sometimes? returns an error.
			 */

			result1 = ksceBtGetVidPid(hid_event.mac0, hid_event.mac1, vid_pid);
			result2 = ksceBtGetDeviceName(hid_event.mac0, hid_event.mac1, name);

			if (is_ds3(vid_pid) || (result1 == 0x802F5001 &&
			    result2 == 0x802F0C01)) {
				ds3_input_reset();
				ds3_mac0 = hid_event.mac0;
				ds3_mac1 = hid_event.mac1;
				ds3_connected = 1;
				ds3_set_operational(hid_event.mac0, hid_event.mac1);
				//ds3_send_leds_rumble(hid_event.mac0, hid_event.mac1);
			}
			break;
		}


		case 0x06: /* Device disconnect event*/
			ds3_connected = 0;
			reset_input_emulation();
			break;

		case 0x08: /* Connection requested event */
			/*
			 * Do nothing since we will get a 0x05 event afterwards.
			 */
			break;

		case 0x09: /* Connection request without being paired? event */
			break;

		case 0x0A: /* HID reply to 0-type request */

			LOG("DS3 0x0A event: 0x%02X\n", recv_buff[0]);

			switch (recv_buff[0]) {
			case 0x01: /* Full report */
				memcpy(&ds3_input, recv_buff, sizeof(ds3_input));

				set_input_emulation(&ds3_input);

				enqueue_read_request(hid_event.mac0, hid_event.mac1,
					&hid_request, recv_buff, sizeof(recv_buff));
				break;

			default:
				LOG("Unknown DS3 event: 0x%02X\n", recv_buff[0]);
				break;
			}

			break;

		case 0x0B: /* HID reply to 1-type request */

			//LOG("DS3 0x0B event: 0x%02X\n", recv_buff[0]);

			enqueue_read_request(hid_event.mac0, hid_event.mac1,
				&hid_request, recv_buff, sizeof(recv_buff));

			break;

		case 0x0C: /* HID reply to 3-type request? */

			//LOG("DS3 0x0C event: 0x%02X\n", recv_buff[0]);

			enqueue_read_request(hid_event.mac0, hid_event.mac1,
				&hid_request, recv_buff, sizeof(recv_buff));

			break;


		}
	}

	return 0;
}

static int ds3vita_bt_thread(SceSize args, void *argp)
{
	bt_cb_uid = ksceKernelCreateCallback("ds3vita_bt_callback", 0, bt_cb_func, NULL);

	ksceBtRegisterCallback(bt_cb_uid, 0, 0xFFFFFFFF, 0xFFFFFFFF);

/*#ifndef RELEASE
	ksceBtStartInquiry();
	ksceKernelDelayThreadCB(4 * 1000 * 1000);
	ksceBtStopInquiry();
#endif*/

	while (bt_thread_run) {
		ksceKernelDelayThreadCB(200 * 1000);
	}

	if (ds3_connected) {
		ksceBtStartDisconnect(ds3_mac0, ds3_mac1);
		reset_input_emulation();
	}

	ksceBtUnregisterCallback(bt_cb_uid);

	ksceKernelDeleteCallback(bt_cb_uid);

	return 0;
}

void _start() __attribute__ ((weak, alias ("module_start")));

int module_start(SceSize argc, const void *args)
{
	int ret;
	tai_module_info_t SceBt_modinfo;

	log_reset();

	LOG("ds3vita by xerpi\n");

	SceBt_modinfo.size = sizeof(SceBt_modinfo);
	ret = taiGetModuleInfoForKernel(KERNEL_PID, "SceBt", &SceBt_modinfo);
	if (ret < 0) {
		LOG("Error finding SceBt module\n");
		goto error_find_scebt;
	}

	/* SceBt patch */
	/*unsigned short thumb_nop[] = {0x00bf};

	SceBt_sub_2292CE4_0x2292D18_patch_uid = taiInjectDataForKernel(KERNEL_PID,
		SceBt_modinfo.modid, 0, 0x2292D18 - 0x2280000, thumb_nop, sizeof(thumb_nop));

	LOG("SceBt_sub_2292CE4_0x2292D18_patch_uid: 0x%08X\n", SceBt_sub_2292CE4_0x2292D18_patch_uid);*/

	/* SceBt hooks */
	SceBt_sub_22999C8_hook_uid = taiHookFunctionOffsetForKernel(KERNEL_PID,
		&SceBt_sub_22999C8_ref, SceBt_modinfo.modid, 0,
		0x22999C8 - 0x2280000, 1, SceBt_sub_22999C8_hook_func);

	SceBt_sub_22947E4_hook_uid = taiHookFunctionOffsetForKernel(KERNEL_PID,
		&SceBt_sub_22947E4_ref, SceBt_modinfo.modid, 0,
		0x22947E4 - 0x2280000, 1, SceBt_sub_22947E4_hook_func);

	/* SceCtrl hooks (needed for PS4 remote play) */
	SceCtrl_sceCtrlPeekBufferPositive2_hook_uid = taiHookFunctionExportForKernel(KERNEL_PID,
		&SceCtrl_sceCtrlPeekBufferPositive2_ref, "SceCtrl", TAI_ANY_LIBRARY,
		0x15F81E8C, SceCtrl_sceCtrlPeekBufferPositive2_hook_func);

	SceCtrl_sceCtrlReadBufferPositive2_hook_uid = taiHookFunctionExportForKernel(KERNEL_PID,
		&SceCtrl_sceCtrlReadBufferPositive2_ref, "SceCtrl", TAI_ANY_LIBRARY,
		0xC4226A3E, SceCtrl_sceCtrlReadBufferPositive2_hook_func);

	SceKernelHeapCreateOpt opt;
	opt.size = 0x1C;
	opt.uselock = 0x100;
	opt.field_8 = 0x10000;
	opt.field_C = 0;
	opt.field_10 = 0;
	opt.field_14 = 0;
	opt.field_18 = 0;

	bt_mempool_uid = ksceKernelCreateHeap("ds3vita_mempool", 0x100, &opt);
	LOG("Bluetooth mempool UID: 0x%08X\n", bt_mempool_uid);

	bt_thread_uid = ksceKernelCreateThread("ds3vita_bt_thread", ds3vita_bt_thread,
		0x3C, 0x1000, 0, 0x10000, 0);
	LOG("Bluetooth thread UID: 0x%08X\n", bt_thread_uid);
	ksceKernelStartThread(bt_thread_uid, 0, NULL);

	LOG("module_start finished successfully!\n");

	return SCE_KERNEL_START_SUCCESS;

error_find_scebt:
	return SCE_KERNEL_START_FAILED;
}

int module_stop(SceSize argc, const void *args)
{
	SceUInt timeout = 0xFFFFFFFF;

	if (bt_thread_uid > 0) {
		bt_thread_run = 0;
		ksceKernelWaitThreadEnd(bt_thread_uid, NULL, &timeout);
		ksceKernelDeleteThread(bt_thread_uid);
	}

	if (bt_mempool_uid > 0) {
		ksceKernelDeleteHeap(bt_mempool_uid);
	}

	if (SceBt_sub_22999C8_hook_uid > 0) {
		taiHookReleaseForKernel(SceBt_sub_22999C8_hook_uid,
			SceBt_sub_22999C8_ref);
	}

	if (SceBt_sub_22947E4_hook_uid > 0) {
		taiHookReleaseForKernel(SceBt_sub_22947E4_hook_uid,
			SceBt_sub_22947E4_ref);
	}

	if (SceCtrl_sceCtrlPeekBufferPositive2_hook_uid > 0) {
		taiHookReleaseForKernel(SceCtrl_sceCtrlPeekBufferPositive2_hook_uid,
			SceCtrl_sceCtrlPeekBufferPositive2_ref);
	}

	if (SceCtrl_sceCtrlReadBufferPositive2_hook_uid > 0) {
		taiHookReleaseForKernel(SceCtrl_sceCtrlReadBufferPositive2_hook_uid,
			SceCtrl_sceCtrlReadBufferPositive2_ref);
	}

	/*if (SceBt_sub_2292CE4_0x2292D18_patch_uid > 0) {
		taiInjectReleaseForKernel(SceBt_sub_2292CE4_0x2292D18_patch_uid);
	}*/

	log_flush();

	return SCE_KERNEL_STOP_SUCCESS;
}
