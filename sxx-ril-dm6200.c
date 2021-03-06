#include "sxx-ril.h"

#define CDMA_CHAT_OPTION "/system/bin/chat -v ABORT 'BUSY' ABORT 'NO CARRIER' ABORT 'NO ANSWER' TIMEOUT 6 '' 'AT' '' 'ATD#777 CONNECT'"

typedef enum
{
	SIM_TYPE_SIM_USIM,
	SIM_TYPE_RUIM,
};

static void override_chat_option();
static int g_sim_type = SIM_TYPE_SIM_USIM;

static int check_sim_type()
{
#if 1
	return SIM_TYPE_SIM_USIM;
#else
	int err;
	ATResponse *p_response = NULL;

	err = at_send_command_singleline("AT+CRSM=192,12258,0,0,15", "+CRSM:", &p_response);

	if (err < 0 || p_response->success == 0) {
		return SIM_TYPE_RUIM;
	} else {
		return SIM_TYPE_SIM_USIM;
	}
#endif
}

static int getCardStatus(RIL_CardStatus **pp_card_status) 
{
	static RIL_AppStatus app_status_sim[] = {
		// SIM_ABSENT = 0
		{ RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
			NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
		// SIM_NOT_READY = 1
		{ RIL_APPTYPE_SIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
			NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
		// SIM_READY = 2
		{ RIL_APPTYPE_SIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
			NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
		// SIM_PIN = 3
		{ RIL_APPTYPE_SIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
			NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
		// SIM_PUK = 4
		{ RIL_APPTYPE_SIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
			NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
		// SIM_NETWORK_PERSONALIZATION = 5
		{ RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
			NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN }
	};

	static RIL_AppStatus app_status_ruim[] = {
		// SIM_ABSENT = 0
		{ RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
			NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
		// SIM_NOT_READY = 1
		{ RIL_APPTYPE_RUIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
			NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
		// SIM_READY = 2
		{ RIL_APPTYPE_RUIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
			NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
		// SIM_PIN = 3
		{ RIL_APPTYPE_RUIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
			NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
		// SIM_PUK = 4
		{ RIL_APPTYPE_RUIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
			NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
		// SIM_NETWORK_PERSONALIZATION = 5
		{ RIL_APPTYPE_RUIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
			NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN }
	};

	RIL_CardState card_state;
	int num_apps;

	int sim_status = getSIMStatus(0);
	if (sim_status == SIM_ABSENT) {
		card_state = RIL_CARDSTATE_ABSENT;
		num_apps = 0;
	} else {
		card_state = RIL_CARDSTATE_PRESENT;
		num_apps = 1;
	}

	// Allocate and initialize base card status.
	RIL_CardStatus *p_card_status = malloc(sizeof(RIL_CardStatus));
	p_card_status->card_state = card_state;
	p_card_status->universal_pin_state = RIL_PINSTATE_UNKNOWN;
	p_card_status->gsm_umts_subscription_app_index = RIL_CARD_MAX_APPS;
	p_card_status->cdma_subscription_app_index = RIL_CARD_MAX_APPS;
	p_card_status->num_applications = num_apps;

	// Initialize application status
	int i;

	for (i = 0; i < RIL_CARD_MAX_APPS; i++) {
		if (g_sim_type == SIM_TYPE_RUIM)
			p_card_status->applications[i] = app_status_ruim[SIM_ABSENT];
		else
			p_card_status->applications[i] = app_status_sim[SIM_ABSENT];
	}

	// Pickup the appropriate application status
	// that reflects sim_status for gsm.
	if (num_apps != 0) {
		// Only support one app, gsm
		p_card_status->num_applications = 1;
		p_card_status->gsm_umts_subscription_app_index = 0;
		p_card_status->cdma_subscription_app_index = 0;

		if (g_sim_type == SIM_TYPE_RUIM)
			p_card_status->applications[0] = app_status_ruim[sim_status];
		else
			p_card_status->applications[0] = app_status_sim[sim_status];
	}
	*pp_card_status = p_card_status;
	return RIL_E_SUCCESS;
}

/**
 * Free the card status returned by getCardStatus.
 */
static void freeCardStatus(RIL_CardStatus *p_card_status) {
	if(p_card_status == NULL ) 
		return ;

	free(p_card_status);
}

static void requestGetSimStatus(void *data, size_t datalen, RIL_Token t)
{
	RIL_CardStatus *p_card_status;
	char *p_buffer;
	int buffer_size;

	int result = getCardStatus(&p_card_status);
	if (result == RIL_E_SUCCESS) {
		p_buffer = (char *)p_card_status;
		buffer_size = sizeof(*p_card_status);
	} else {
		p_buffer = NULL;
		buffer_size = 0;
	}
	RIL_onRequestComplete(t, result, p_buffer, buffer_size);
	freeCardStatus(p_card_status);
}
REGISTER_REQUEST_ITEM(RIL_REQUEST_GET_SIM_STATUS, requestGetSimStatus, 0x05c6910e)

static void init()
{
	while(getSIMStatus(2) == SIM_ABSENT) {
		LOGD("wait for sim ready");
		sleep(1);
	}
	if (check_sim_type() == SIM_TYPE_RUIM)
		g_sim_type = SIM_TYPE_RUIM;

    //at_send_command("AT+CGATT=1", NULL);
	override_chat_option();
}

void bringup_dm6200()
{
	system("echo 05c6 910e>/sys/bus/usb-serial/drivers/option1/new_id");
}

modem_spec_t dm6200 = 
{
	.name = "DM6200",
	.at_port = "/dev/ttyUSB2",
	.data_port = "/dev/ttyUSB4",
	.chat_option = DEFAULT_CHAT_OPTION,
	.vid_pid = 0x05c6910e,
	.init = init,
	.bringup = bringup_dm6200,
};

static void override_chat_option()
{
	if (g_sim_type == SIM_TYPE_RUIM) {
		dm6200.chat_option = CDMA_CHAT_OPTION;
	}
}

REGISTER_MODEM(DM6200,&dm6200)
