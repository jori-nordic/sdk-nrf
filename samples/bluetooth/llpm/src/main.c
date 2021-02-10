/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <console/console.h>
#include <string.h>
#include <sys/printk.h>
#include <zephyr/types.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/gatt.h>
#include <bluetooth/hci.h>
#include <bluetooth/uuid.h>
#include <bluetooth/services/latency.h>
#include <bluetooth/services/latency_client.h>
#include <bluetooth/scan.h>
#include <bluetooth/gatt_dm.h>
#include <sdc_hci_vs.h>
#include <init.h>

/* #include <timing/timing.h> */
#include <debug/ppi_trace.h>
#include <hal/nrf_gpiote.h>
#include <nrfx_dppi.h>

#define DEVICE_NAME	CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)
#define INTERVAL_MIN    20     /* 80 units,  100 ms */
#define INTERVAL_MAX    20     /* 80 units,  100 ms */
#define INTERVAL_LLPM   0x0D01   /* Proprietary  1 ms */
#define INTERVAL_LLPM_US 1000

static volatile bool test_ready;
static struct bt_conn *default_conn;
static struct bt_latency latency;
static struct bt_latency_client latency_client;
static struct bt_le_conn_param *conn_param =
	BT_LE_CONN_PARAM(INTERVAL_MIN, INTERVAL_MAX, 0, 400);
static struct bt_conn_info conn_info = {0};

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_LATENCY_VAL),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static struct {
	uint32_t latency;
	uint32_t crc_mismatches;
} llpm_latency;

void scan_filter_match(struct bt_scan_device_info *device_info,
		       struct bt_scan_filter_match *filter_match,
		       bool connectable)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));

	printk("Filters matched. Address: %s connectable: %d\n",
	       addr, connectable);
}

void scan_filter_no_match(struct bt_scan_device_info *device_info,
			  bool connectable)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));

	/* printk("Filter does not match. Address: %s connectable: %d\n", */
	/*        addr, connectable); */
}

void scan_connecting_error(struct bt_scan_device_info *device_info)
{
	printk("Connecting failed\n");
}

BT_SCAN_CB_INIT(scan_cb, scan_filter_match, scan_filter_no_match,
		scan_connecting_error, NULL);

static void scan_init(void)
{
	int err;
	struct bt_le_scan_param scan_param = {
		.type = BT_LE_SCAN_TYPE_PASSIVE,
		.options = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
		.interval = 0x0010,
		.window = 0x0010,
	};

	struct bt_scan_init_param scan_init = {
		.connect_if_match = true,
		.scan_param = &scan_param,
		.conn_param = conn_param
	};

	bt_scan_init(&scan_init);
	bt_scan_cb_register(&scan_cb);

	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_LATENCY);
	if (err) {
		printk("Scanning filters cannot be set (err %d)\n", err);
		return;
	}

	err = bt_scan_filter_enable(BT_SCAN_UUID_FILTER, false);
	if (err) {
		printk("Filters cannot be turned on (err %d)\n", err);
	}
}

static void discovery_complete(struct bt_gatt_dm *dm, void *context)
{
	struct bt_latency_client *latency = context;

	printk("Service discovery completed\n");

	bt_gatt_dm_data_print(dm);
	bt_latency_handles_assign(dm, latency);
	bt_gatt_dm_data_release(dm);

	/* Start testing when the GATT service is discovered */
	test_ready = true;
}

static void discovery_service_not_found(struct bt_conn *conn, void *context)
{
	printk("Service not found\n");
}

static void discovery_error(struct bt_conn *conn, int err, void *context)
{
	printk("Error while discovering GATT database: (err %d)\n", err);
}

struct bt_gatt_dm_cb discovery_cb = {
	.completed         = discovery_complete,
	.service_not_found = discovery_service_not_found,
	.error_found       = discovery_error,
};

static void advertise_and_scan(void)
{
	int err;

	err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Advertising failed to start (err %d)\n", err);
		return;
	}

	printk("Advertising successfully started\n");

	err = bt_scan_start(BT_SCAN_TYPE_SCAN_PASSIVE);
	if (err) {
		printk("Starting scanning failed (err %d)\n", err);
		return;
	}

	printk("Scanning successfully started\n");
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("Connection failed (err %u)\n", err);
		return;
	}

	default_conn = bt_conn_ref(conn);
	err = bt_conn_get_info(default_conn, &conn_info);
	if (err) {
		printk("Getting conn info failed (err %d)\n", err);
		return;
	}

	printk("Connected as %s\n",
	       conn_info.role == BT_CONN_ROLE_MASTER ? "master" : "slave");
	printk("Conn. interval is %u units (1.25 ms/unit)\n",
	       conn_info.le.interval);

	/* make sure we're not scanning or advertising */
	bt_le_adv_stop();
	bt_scan_stop();

	err = bt_gatt_dm_start(default_conn, BT_UUID_LATENCY, &discovery_cb,
			       &latency_client);
	if (err) {
		printk("Discover failed (err %d)\n", err);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("Disconnected (reason %u)\n", reason);

	test_ready = false;

	if (default_conn) {
		bt_conn_unref(default_conn);
		default_conn = NULL;
	}

	advertise_and_scan();
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
			     uint16_t latency, uint16_t timeout)
{
	if (interval == INTERVAL_LLPM) {
		printk("Connection interval updated: LLPM (1 ms)\n");
	}
}

static int enable_llpm_mode(void)
{
	int err;
	struct net_buf *buf;
	sdc_hci_cmd_vs_llpm_mode_set_t *cmd_enable;

	buf = bt_hci_cmd_create(SDC_HCI_OPCODE_CMD_VS_LLPM_MODE_SET,
				sizeof(*cmd_enable));
	if (!buf) {
		printk("Could not allocate LLPM command buffer\n");
		return -ENOMEM;
	}

	cmd_enable = net_buf_add(buf, sizeof(*cmd_enable));
	cmd_enable->enable = true;

	err = bt_hci_cmd_send_sync(SDC_HCI_OPCODE_CMD_VS_LLPM_MODE_SET, buf, NULL);
	if (err) {
		printk("Error enabling LLPM %d\n", err);
		return err;
	}

	printk("LLPM mode enabled\n");
	return 0;
}

static int enable_llpm_short_connection_interval(void)
{
	int err;
	struct net_buf *buf;

	sdc_hci_cmd_vs_conn_update_t *cmd_conn_update;

	buf = bt_hci_cmd_create(SDC_HCI_OPCODE_CMD_VS_CONN_UPDATE,
				sizeof(*cmd_conn_update));
	if (!buf) {
		printk("Could not allocate command buffer\n");
		return -ENOMEM;
	}

	uint16_t conn_handle;

	err = bt_hci_get_conn_handle(default_conn, &conn_handle);
	if (err) {
		printk("Failed obtaining conn_handle (err %d)\n", err);
		return err;
	}

	cmd_conn_update = net_buf_add(buf, sizeof(*cmd_conn_update));
	cmd_conn_update->connection_handle   = conn_handle;
	cmd_conn_update->conn_interval_us    = INTERVAL_LLPM_US;
	cmd_conn_update->conn_latency        = 0;
	cmd_conn_update->supervision_timeout = 300;

	err = bt_hci_cmd_send_sync(SDC_HCI_OPCODE_CMD_VS_CONN_UPDATE, buf, NULL);
	if (err) {
		printk("Update connection parameters failed (err %d)\n", err);
		return err;
	}

	return 0;
}

static bool on_vs_evt(struct net_buf_simple *buf)
{
	uint8_t code;
	sdc_hci_subevent_vs_qos_conn_event_report_t *evt;

	code = net_buf_simple_pull_u8(buf);
	if (code != SDC_HCI_SUBEVENT_VS_QOS_CONN_EVENT_REPORT) {
		return false;
	}

	evt = (void *)buf->data;
	llpm_latency.crc_mismatches += evt->crc_error_count;

	return true;
}

static int enable_qos_conn_evt_report(void)
{
	int err;
	struct net_buf *buf;

	err = bt_hci_register_vnd_evt_cb(on_vs_evt);
	if (err) {
		printk("Failed registering vendor specific callback (err %d)\n",
		       err);
		return err;
	}

	sdc_hci_cmd_vs_qos_conn_event_report_enable_t *cmd_enable;

	buf = bt_hci_cmd_create(SDC_HCI_OPCODE_CMD_VS_QOS_CONN_EVENT_REPORT_ENABLE,
				sizeof(*cmd_enable));
	if (!buf) {
		printk("Could not allocate command buffer\n");
		return -ENOMEM;
	}

	cmd_enable = net_buf_add(buf, sizeof(*cmd_enable));
	cmd_enable->enable = true;

	err = bt_hci_cmd_send_sync(
		SDC_HCI_OPCODE_CMD_VS_QOS_CONN_EVENT_REPORT_ENABLE, buf, NULL);
	if (err) {
		printk("Could not send command buffer (err %d)\n", err);
		return err;
	}

	printk("Connection event reports enabled\n");
	return 0;
}

static void latency_response_handler(const void *buf, uint16_t len)
{
	uint32_t latency_time;

	if (len == sizeof(latency_time)) {
		/* compute how long the time spent */
		latency_time = *((uint32_t *)buf);
		uint32_t cycles_spent = k_cycle_get_32() - latency_time;
		llpm_latency.latency =
			(uint32_t)k_cyc_to_ns_floor64(cycles_spent) / 2000;
	}
}

static const struct bt_latency_client_cb latency_client_cb = {
	.latency_response = latency_response_handler
};

static void test_run(void)
{
	int err;

	if (!test_ready) {
		/* disconnected while blocking inside _getchar() */
		return;
	}

	test_ready = false;

	/* Switch to LLPM short connection interval */
	if (conn_info.role == BT_CONN_ROLE_MASTER) {
		printk("Press any key to set LLPM short connection interval (1 ms)\n");
		console_getchar();

		if (enable_llpm_short_connection_interval()) {
			printk("Enable LLPM short connection interval failed\n");
			return;
		}
	}

	printk("Press any key to start measuring transmission latency\n");
	console_getchar();

	/* Start sending the timestamp to its peer */
	while (default_conn) {
		uint32_t time = k_cycle_get_32();

		err = bt_latency_request(&latency_client, &time, sizeof(time));
		if (err && err != -EALREADY) {
			printk("Latency failed (err %d)\n", err);
		}

		k_sleep(K_MSEC(200)); /* wait for latency response */

		if (llpm_latency.latency) {
			printk("Transmission Latency: %u (us), CRC mismatches: %u\n",
			       llpm_latency.latency,
			       llpm_latency.crc_mismatches);
		} else {
			printk("Did not receive a latency response\n");
		}

		memset(&llpm_latency, 0, sizeof(llpm_latency));
	}
}


/** @brief Allocate GPIOTE channel.
 *
 * @param pin Pin.
 *
 * @return Allocated channel or -1 if failed to allocate.
 */
static int gpiote_channel_alloc(uint32_t pin)
{
	for (uint8_t channel = 0; channel < GPIOTE_CH_NUM; ++channel) {
		if (!nrf_gpiote_te_is_enabled(NRF_GPIOTE, channel)) {
			nrf_gpiote_task_configure(NRF_GPIOTE, channel, pin,
						  NRF_GPIOTE_POLARITY_TOGGLE,
						  NRF_GPIOTE_INITIAL_VALUE_LOW);
			nrf_gpiote_task_enable(NRF_GPIOTE, channel);
			return channel;
		}
	}

	return -1;
}

/* Convert task address to associated subscribe register */
#define SUBSCRIBE_ADDR(task) (volatile uint32_t *)(task + 0x80)

/* Convert event address to associated publish register */
#define PUBLISH_ADDR(evt) (volatile uint32_t *)(evt + 0x80)

void ppi_trace_config_custom(uint32_t pin, uint32_t ppi_ch)
{
	uint32_t task;
	int gpiote_ch;
	nrf_gpiote_task_t task_id;

	/* Alloc gpiote channel */
	gpiote_ch = gpiote_channel_alloc(pin);

	/* Get gpiote task address */
	task_id = offsetof(NRF_GPIOTE_Type, TASKS_OUT[gpiote_ch]);
	task = nrf_gpiote_task_address_get(NRF_GPIOTE, task_id);

	/* Hook to already assigned DPPI channel */
	*SUBSCRIBE_ADDR(task) = DPPIC_SUBSCRIBE_CHG_EN_EN_Msk | (uint32_t)ppi_ch;
}

void ppi_trace_config_cpu(uint32_t pin, uint32_t ppi_ch)
{
	uint32_t task;
	uint32_t evt;
	int gpiote_ch;
	nrf_gpiote_task_t task_id;

	/* Alloc gpiote channel */
	gpiote_ch = gpiote_channel_alloc(pin);

	/* Get gpiote SET address */
	task_id = offsetof(NRF_GPIOTE_Type, TASKS_SET[gpiote_ch]);
	task = nrf_gpiote_task_address_get(NRF_GPIOTE, task_id);

	/* Publish CPU wakeup */
	evt = (uint32_t)&(NRF_POWER_NS->EVENTS_SLEEPEXIT);
	*PUBLISH_ADDR(evt) = DPPIC_SUBSCRIBE_CHG_EN_EN_Msk | (uint32_t)ppi_ch;

	/* Hook GPIOTE to configured DPPI channel */
	*SUBSCRIBE_ADDR(task) = DPPIC_SUBSCRIBE_CHG_EN_EN_Msk | (uint32_t)ppi_ch;

	/* Enable said channel */
	NRF_DPPIC_NS->CHENSET = 1<<ppi_ch;

	/* Get gpiote CLEAR address */
	task_id = offsetof(NRF_GPIOTE_Type, TASKS_CLR[gpiote_ch]);
	task = nrf_gpiote_task_address_get(NRF_GPIOTE, task_id);

	/* Publish CPU sleep */
	ppi_ch++;
	evt = (uint32_t)&(NRF_POWER_NS->EVENTS_SLEEPENTER);
	*PUBLISH_ADDR(evt) = DPPIC_SUBSCRIBE_CHG_EN_EN_Msk | (uint32_t)ppi_ch;

	/* Hook GPIOTE to configured DPPI channel */
	*SUBSCRIBE_ADDR(task) = DPPIC_SUBSCRIBE_CHG_EN_EN_Msk | (uint32_t)ppi_ch;

	/* Enable said channel */
	NRF_DPPIC_NS->CHENSET = 1<<ppi_ch;
}

static void setup_pin_toggling(void)
{
/* #define HAL_DPPI_REM_EVENTS_START_CHANNEL_IDX     3U */
#define HAL_DPPI_RADIO_EVENTS_READY_CHANNEL_IDX   4U
#define HAL_DPPI_RADIO_EVENTS_ADDRESS_CHANNEL_IDX 5U
#define HAL_DPPI_RADIO_EVENTS_END_CHANNEL_IDX     6U
#define HAL_DPPI_RADIO_EVENTS_DISABLED_CH_IDX     7U

#define GPIO7 32+9
#define GPIO3 32+5
#define GPIO4 32+6
#define GPIO5 32+7
#define GPIO6 32+8

	/* ppi_trace_config_custom(GPIO7, HAL_DPPI_RADIO_EVENTS_READY_CHANNEL_IDX); */
	/* ppi_trace_config_custom(GPIO3, HAL_DPPI_RADIO_EVENTS_ADDRESS_CHANNEL_IDX); */
	/* ppi_trace_config_custom(GPIO4, HAL_DPPI_RADIO_EVENTS_END_CHANNEL_IDX); */
	/* ppi_trace_config_custom(GPIO4, HAL_DPPI_RADIO_EVENTS_DISABLED_CH_IDX); */
	/* ppi_trace_config_cpu(GPIO6, 16U); */
}

static void setup_timer(void)
{
	/* Should be running at 1MHz already */
	/* Set to 32bit */
	NRF_TIMER2_NS->BITMODE = 3;
	NRF_TIMER2_NS->TASKS_START = 1;
}

void main(void)
{
	int err;
	static struct bt_conn_cb conn_callbacks = {
		.connected = connected,
		.disconnected = disconnected,
		.le_param_updated = le_param_updated,
	};

	printk("Increasing frequency\n");
	/* NRF_CLOCK_S->HFCLKCTRL = 0; */

	NRF_P1_NS->DIRSET = (1<<16) - 1;
	NRF_P1_NS->OUTSET = (1<<16) - 1;
	for(int i=0; i<1000; i++)
	{
		i++;
		i--;
	}
	NRF_P1_NS->OUTCLR = (1<<16) - 1;

	setup_pin_toggling();
	/* disable I-cache */
	NRF_NVMC_NS->ICACHECNF &= ~NVMC_ICACHECNF_CACHEEN_Msk;
	/* timing_init(); */
	setup_timer();

	printk("Starting Bluetooth LLPM example\n");

	console_init();

	bt_conn_cb_register(&conn_callbacks);

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	scan_init();

	err = bt_latency_init(&latency, NULL);
	if (err) {
		printk("Latency service initialization failed (err %d)\n", err);
		return;
	}

	err = bt_latency_client_init(&latency_client, &latency_client_cb);
	if (err) {
		printk("Latency client initialization failed (err %d)\n", err);
		return;
	}

	if (enable_llpm_mode()) {
		printk("Enable LLPM mode failed.\n");
		return;
	}

	advertise_and_scan();

	if (enable_qos_conn_evt_report()) {
		printk("Enable LLPM QoS failed.\n");
		return;
	}

	for (;;) {
		if (test_ready) {
			test_run();
		}
	}
}

/* #if defined(CONFIG_SOC_SERIES_NRF53X) */
#if 0
static int network_gpio_allow(const struct device *dev)
{
	ARG_UNUSED(dev);

	NRF_P1_S->DIRSET = (1<<16) - 1;
	NRF_P1_S->OUTSET = (1<<16) - 1;
	for(int i=0; i<1000; i++)
	{
		i++;
		i--;
	}
	NRF_P1_S->OUTCLR = (1<<16) - 1;

	for (uint32_t i = 0; i < P1_PIN_NUM; i++) {
		/* if (i == 4 || i == 8) continue; */
		if (i == 4) continue;
		NRF_P1_S->PIN_CNF[i] = (GPIO_PIN_CNF_MCUSEL_NetworkMCU <<
					GPIO_PIN_CNF_MCUSEL_Pos);
	}

	return 0;
}
SYS_INIT(network_gpio_allow, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_OBJECTS);
#endif

void sys_trace_thread_switched_out()
{
	/* if (k_current_get() == main_tid) { */
	/* 	NRF_P1_NS->OUTCLR = 1 << 7; */
	/* } */
	/* if (!z_is_idle_thread_object(k_current_get())) { */
	/* 	NRF_P1_NS->OUTCLR = 1 << 6; */
	/* } */
}

extern uint8_t recv_thread_ran;
void sys_trace_thread_switched_in()
{
	/* if (k_current_get() == main_tid) { */
	/* 	NRF_P1_NS->OUTSET = 1 << 7; */
	/* } */
	/* if (!z_is_idle_thread_object(k_current_get())) { */
		/* NRF_P1_NS->OUTSET = 1 << 6; */
	/* } */
	/* if(recv_thread_ran) { */
	/* 	recv_thread_ran--; */
	/* 	LOG_ERR("UNT: %s", log_strdup(k_current_get()->name)); */
	/* } */
}

void sys_trace_thread_priority_set(struct k_thread *thread)
{
}

void sys_trace_thread_create(struct k_thread *thread) {}

void sys_trace_thread_abort(struct k_thread *thread) {}

void sys_trace_thread_suspend(struct k_thread *thread)
{
	/* if (!(k_current_get() == main_tid)) { */
	/* 	NRF_P1_NS->OUTCLR = 1 << 6; */
	/* }; */
}

void sys_trace_thread_resume(struct k_thread *thread)
{
	/* if (!(k_current_get() == main_tid)) { */
	/* 	NRF_P1_NS->OUTSET = 1 << 6; */
	/* }; */
}

void sys_trace_thread_ready(struct k_thread *thread) {	\
	if(1) {	\
		/* NRF_P1_NS->OUTSET = 1<<5;				\ */
	};								\
	}

void sys_trace_thread_pend(struct k_thread *thread) {	\
	if(1) {	\
		/* NRF_P1_NS->OUTCLR = 1<<5;				\ */
	};								\
	}

void sys_trace_thread_info(struct k_thread *thread) {}

void sys_trace_thread_name_set(struct k_thread *thread) {}

__STATIC_FORCEINLINE void delay_unit(void) {
	/* for(int i=0; i<20; i++) */
	/* { */
	/* 	__NOP(); */
	/* } */
	__NOP();
	__NOP();
	__NOP();
	__NOP();
	/* k_busy_wait fails llpm */
	/* k_busy_wait(1); */
};


/* #pragma GCC optimize ("O0") */
void sys_trace_isr_enter()
{
	/* int8_t active = (((SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk) >> */
	/* 		  SCB_ICSR_VECTACTIVE_Pos) - */
	/* 		 16); */

	/* if(active <0) active = 0; */
	/* for(; active > 0; active--) */
	/* { */
	/* 	NRF_P1_NS->OUTCLR = 1 << 9; */
	/* 	delay_unit(); */
	/* 	NRF_P1_NS->OUTSET = 1 << 9; */
	/* 	delay_unit(); */
	/* } */
}

void sys_trace_isr_exit()
{
	if (1) {
		/* NRF_P1_NS->OUTCLR = 1 << 9; */
	};
}

void sys_trace_isr_exit_to_scheduler()
{
	if (1) {
		/* NRF_P1_NS->OUTCLR = 1 << 9; */
		/* delay_unit(); */
		/* NRF_P1_NS->OUTSET = 1 << 9; */
		/* delay_unit(); */
		/* NRF_P1_NS->OUTCLR = 1 << 9; */
	};
}

void sys_trace_void(int id)
{
}

void sys_trace_end_call(int id) {}

void sys_trace_idle() {}

void sys_trace_semaphore_init(struct k_sem *sem) {}

void sys_trace_semaphore_take(struct k_sem *sem)
{
	/* if (1) { */
	/* 	NRF_P1_NS->OUTSET = 1 << 8; */
	/* }; */
}

void sys_trace_semaphore_give(struct k_sem *sem)
{
	/* if (1) { */
	/* 	NRF_P1_NS->OUTCLR = 1 << 8; */
	/* }; */
}

void sys_trace_mutex_init(struct k_mutex *mutex) {}

void sys_trace_mutex_lock(struct k_mutex *mutex) {}

void sys_trace_mutex_unlock(struct k_mutex *mutex) {}
