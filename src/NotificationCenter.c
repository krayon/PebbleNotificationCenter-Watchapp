#include <pebble.h>
#include <pebble_fonts.h>
#include "NotificationsWindow/NotificationsWindow.h"
#include "MainMenuWindow.h"
#include "NotificationListWindow.h"
#include "NotificationsWindow/Comm.h"

const uint16_t PROTOCOL_VERSION = 36;

int8_t curWindow = 0;
bool gotConfig = false;

uint16_t config_timeout;
uint16_t config_periodicTimeout;
uint8_t config_lightTimeout;
uint8_t config_periodicVibrationPatternSize;
uint16_t config_periodicVibrationTotalDuration;
uint32_t* config_periodicVibrationPattern = NULL;

bool config_dontClose;
bool config_showActive;
bool config_lightScreen;
bool config_dontVibrateWhenCharging;
bool config_disableNotifications;
bool config_whiteText;
bool config_disableVibration;
bool config_displayScrollShadow;
bool config_scrollByPage;
bool config_disconnectedNotification;
bool main_noMenu;



uint32_t appmessage_max_size;

bool closingMode = false;
bool loadingMode = false;

static const char* fonts[] = {
		FONT_KEY_GOTHIC_14,
		FONT_KEY_GOTHIC_14_BOLD,
		FONT_KEY_GOTHIC_18,
		FONT_KEY_GOTHIC_18_BOLD,
		FONT_KEY_GOTHIC_24,
		FONT_KEY_GOTHIC_24_BOLD,
		FONT_KEY_GOTHIC_28,
		FONT_KEY_GOTHIC_28_BOLD,
		FONT_KEY_BITHAM_30_BLACK,
		FONT_KEY_BITHAM_42_BOLD,
		FONT_KEY_BITHAM_42_LIGHT,
		FONT_KEY_BITHAM_42_MEDIUM_NUMBERS,
		FONT_KEY_BITHAM_34_MEDIUM_NUMBERS,
		FONT_KEY_BITHAM_34_LIGHT_SUBSET,
		FONT_KEY_BITHAM_18_LIGHT_SUBSET,
		FONT_KEY_ROBOTO_CONDENSED_21,
		FONT_KEY_ROBOTO_BOLD_SUBSET_49,
		FONT_KEY_DROID_SERIF_28_BOLD
};

const char* config_getFontResource(int id)
{
	return fonts[id];
}

bool canVibrate(void)
{
	return !config_disableVibration && (!config_dontVibrateWhenCharging || !battery_state_service_peek().is_plugged);
}

uint8_t getCurWindow(void)
{
	return curWindow;
}

void setCurWindow(uint8_t newWindow)
{
	curWindow = newWindow;
}

void switchWindow(uint8_t newWindow)
{
	switch(newWindow)
	{
	case 0:
		curWindow = 0;
		main_menu_init();
		break;
	case 1:
		curWindow = 1;
		notification_window_init();
		break;
	case 2:
		curWindow = 2;
		list_window_init();
		break;
	}
}

static void received_config(DictionaryIterator *received)
{
	uint8_t* data = dict_find(received, 2)->value->data;

	uint16_t supportedVersion = (data[8] << 8) | (data[9]);
	if (supportedVersion > PROTOCOL_VERSION)
	{
		show_old_watchapp();
		return;
	}
	else if (supportedVersion < PROTOCOL_VERSION)
	{
		show_old_android();
		return;
	}

	config_timeout = (data[3] << 8) | (data[4]);
	config_dontClose = (data[7] & 0x02) != 0;
	config_showActive = (data[7] & 0x04) != 0;
	main_noMenu = (data[7] & 0x08) != 0;
	config_lightScreen = (data[7] & 0x10) != 0;
	config_dontVibrateWhenCharging = (data[7] & 0x20) != 0;
	config_disableNotifications = (data[7] & 0x80) != 0;
	config_whiteText = (data[7] & 0x740) != 0;
	config_disableVibration = (data[7] & 0x01) != 0;
	config_displayScrollShadow = (data[13] & 0x01) != 0;
	config_scrollByPage = PBL_IF_ROUND_ELSE(true, (data[13] & 0x02) != 0);
	config_disconnectedNotification = (data[13] & 0x04) != 0;

	config_periodicTimeout  = (data[11] << 8) | (data[12]);
	config_lightTimeout = data[5];

	config_periodicVibrationPatternSize = data[14];
	config_periodicVibrationTotalDuration = 0;
	config_periodicVibrationPattern = malloc(config_periodicVibrationPatternSize / 2 * sizeof(uint32_t));
	for (int i = 0; i < config_periodicVibrationPatternSize; i+= 2)
	{
		config_periodicVibrationPattern[i / 2] = data[15 + i] | (data[16 + i] << 8);
		config_periodicVibrationTotalDuration += config_periodicVibrationPattern[i / 2];
	}

	gotConfig = true;
	loadingMode = false;

	if (!main_noMenu)
		show_menu();

}

static void received_data(DictionaryIterator *received, void *context) {
	app_comm_set_sniff_interval(SNIFF_INTERVAL_REDUCED);

	uint8_t destModule = dict_find(received, 0)->value->uint8;
	uint8_t packetId = dict_find(received, 1)->value->uint8;
	bool autoSwitch = dict_find(received, 999) != NULL;

	if (destModule == 0 && packetId == 0)
	{
		received_config(received);
		return;
	}

	if (!gotConfig) //Do not react to anything until we got config
		return;

	if (destModule == 2)
	{
		if (curWindow != 2)
		{
			if (autoSwitch)
				switchWindow(2);
			else
				return;
		}

		list_window_data_received(packetId, received);

	}
	else
	{
		if (curWindow != 1)
		{
			if (autoSwitch)
				switchWindow(1);
			else
				return;
		}

		nw_received_data_callback(destModule, packetId, received);
	}
}

static void sent_data(DictionaryIterator *iterator, void *context)
{
	if (curWindow == 1)
		nw_data_sent_callback();
	else if (curWindow == 2)
		list_window_data_sent();
}

void closeApp(void)
{
	DictionaryIterator *iterator;
	app_message_outbox_begin(&iterator);
	dict_write_uint8(iterator, 0, 0);
	dict_write_uint8(iterator, 1, 3);
	app_comm_set_sniff_interval(SNIFF_INTERVAL_REDUCED);
	app_message_outbox_send();

	closingMode = true;
}

static uint32_t getCapabilities(uint16_t maxInboxSize)
{
	uint32_t serializedCapabilities = 0;

	serializedCapabilities |= PBL_IF_MICROPHONE_ELSE(0x01, 0x00);
	serializedCapabilities |= PBL_IF_COLOR_ELSE(0x02, 0x00);
	serializedCapabilities |= PBL_IF_ROUND_ELSE(0x04, 0x00);
	serializedCapabilities |= PBL_IF_SMARTSTRAP_ELSE(0x08, 0x00);
	serializedCapabilities |= PBL_IF_HEALTH_ELSE(0x10, 0x00);
	serializedCapabilities |= maxInboxSize << 16;

	return serializedCapabilities;
}

int main(void) {
	appmessage_max_size = app_message_inbox_size_maximum();
	if (appmessage_max_size > 4096)
		appmessage_max_size = 4096; //Limit inbox size to conserve RAM.

	#ifdef PBL_PLATFORM_APLITE
		//Aplite has very little memory, so we must have very conservative appmessage buffer.
		appmessage_max_size = 250;
	#endif

	app_message_register_inbox_received(received_data);
	app_message_register_outbox_sent(sent_data);
	app_message_open(appmessage_max_size, 408);

	DictionaryIterator *iterator;
	app_message_outbox_begin(&iterator);
	dict_write_uint8(iterator, 0, 0);
	dict_write_uint8(iterator, 1, 0);
	dict_write_uint16(iterator, 2, PROTOCOL_VERSION);

	dict_write_uint32(iterator, 3, getCapabilities(appmessage_max_size));

	app_comm_set_sniff_interval(SNIFF_INTERVAL_REDUCED);
	app_message_outbox_send();

	loadingMode = true;

	switchWindow(0);

	app_event_loop();

	window_stack_pop_all(false);

	free(config_periodicVibrationPattern);

	return 0;
}
