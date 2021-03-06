#include <stdio.h>
#include <assert.h>
#include "sxx-ril.h"

static int ussdStatus = 0;

void requestQueryClip(void *data, size_t datalen, RIL_Token t)
{
	RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
}
REGISTER_DEFAULT_ITEM(RIL_REQUEST_QUERY_CLIP, requestQueryClip)

void requestCancelUSSD(void *data, size_t datalen, RIL_Token t)
{
	ATResponse *p_response;
	int err;
	p_response = NULL;
	err = at_send_command_numeric("AT+CUSD=2", &p_response);
	if (err < 0 || p_response->success == 0) {
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	} else {
		RIL_onRequestComplete(t, RIL_E_SUCCESS,
				p_response->p_intermediates->line, sizeof(char *));
	}
	ussdStatus = 0;

	at_send_command("AT+CSCS=\"CUS2\"", NULL);
	at_response_free(p_response);
}
REGISTER_DEFAULT_ITEM(RIL_REQUEST_CANCEL_USSD, requestCancelUSSD)


void  requestSendUSSD(void *data, size_t datalen, RIL_Token t)
{
	ATResponse *p_response = NULL;
	int err = 0;
	char* ussdRequest;
	char* cmd;

	at_send_command("AT+CSCS=\"GSM\"", NULL);
	ussdStatus = 1;

	ussdRequest = (char*)(data);
	asprintf(&cmd, "AT+CUSD=1,%s,15", ussdRequest);
	err = at_send_command(cmd, &p_response);
	free(cmd);


	if (err < 0 || p_response->success == 0) {
		goto error;
	}
	
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;
	
error:
    at_response_free(p_response);
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

}
REGISTER_DEFAULT_ITEM(RIL_REQUEST_SEND_USSD, requestSendUSSD)


void onSuppServiceNotification(const char *s, int type)
{
	RIL_SuppSvcNotification ssnResponse;
	char *line;
	char *tok;
	int err;

	line = tok = strdup(s);

	memset(&ssnResponse, 0, sizeof(ssnResponse));
	ssnResponse.notificationType = type;

	err = at_tok_start(&tok);
	if (err < 0)
		goto error;

	err = at_tok_nextint(&tok, &ssnResponse.code);
	if (err < 0)
		goto error;

	if (ssnResponse.code == 16 || 
			(type == 0 && ssnResponse.code == 4) ||
			(type == 1 && ssnResponse.code == 1)) {
		err = at_tok_nextint(&tok, &ssnResponse.index);
		if (err < 0)
			goto error;
	}

	/* RIL_SuppSvcNotification has two more members that we won't
	   get from the +CSSI/+CSSU. Where do we get them, if we ever do? */

	RIL_onUnsolicitedResponse(RIL_UNSOL_SUPP_SVC_NOTIFICATION,
			&ssnResponse, sizeof(ssnResponse));

error:
	free(line);
}

//*133*950*118#
/**
 * RIL_UNSOL_ON_USSD
 *
 * Called when a new USSD message is received.
 */
int onUSSDReceived(const char *s, char* sms_pdu)
{
	char *line, *linestart;
	int typeCode, count, err, len;
	char *message;
	char *outputmessage;
	char *responseStr[2];

	linestart=line=strdup(s);
	err = at_tok_start(&line);
	if(err < 0) goto out;

	err = at_tok_nextint(&line, &typeCode);
	if(err < 0) goto out;

	if(at_tok_hasmore(&line)) {

		int format;
		char message[256];
		int n = sscanf(s+6,"%*d,\"%[^\"]\",%d",message,&format);

		LOGD("%s,%d",message,format);

		if(format == 15){
			responseStr[1] = malloc(strlen(message)+1);
			strcpy(responseStr[1],message);    
		}else{
			int len = strlen(message);
			outputmessage = malloc(len/2);
			gsm_hex_to_bytes((cbytes_t)message,len,(bytes_t)outputmessage);

			responseStr[1] = malloc(len);
			len = ucs2_to_utf8((cbytes_t)outputmessage,len/2,(bytes_t)responseStr[1]);
			free(outputmessage);    
		}
		count = 2;
	} else {
		responseStr[1]=NULL;
		count = 1;
	}
	free(linestart);
	asprintf(&responseStr[0], "%d", typeCode);
	RIL_onUnsolicitedResponse (RIL_UNSOL_ON_USSD, responseStr, count*sizeof(char*));
out:
	return UNSOLICITED_SUCCESSED;
}

REGISTER_DEFAULT_UNSOLICITED(CUSD, "+CUSD:", onUSSDReceived)

static int onCmeError(char* s, char* sms_pdu)
{
	char* line = NULL;
	int err;
	if (ussdStatus) {
		int  code;

		line = strdup(s);

		at_tok_start(&line);

		err = at_tok_nextint(&line, &code);

		free(line);
		if (err < 0)
			goto out;

		if(code == 258 || code == 257 || code == 100){
			char *responseStr[2];
			responseStr[1] = NULL;
			asprintf(&responseStr[0], "%d", -1);
			RIL_onUnsolicitedResponse (RIL_UNSOL_ON_USSD, responseStr, 1*sizeof(char*));				
		}	
	}
out:
	return UNSOLICITED_SUCCESSED;
}
REGISTER_DEFAULT_UNSOLICITED(CMEERROR, "+CME ERROR:", onCmeError)

/**  
 * RIL_REQUEST_GET_CLIR
 *
 * Gets current CLIR status.
 ok
 */
void requestGetCLIR(void *data, size_t datalen, RIL_Token t)
{
	ATResponse *p_response;
	p_response = NULL;
	int response[2] = {1, 1};

	int err = at_send_command_singleline("AT+CLIR?",
			"+CLIR: ", &p_response);
	if (err >= 0 && p_response->success) {
		char *line = p_response->p_intermediates->line;

		err = at_tok_start(&line);

		if (err >= 0) {
			err = at_tok_nextint(&line, &response[0]);

			if (err >= 0)
				err = at_tok_nextint(&line, &response[1]);
		}

	}
	RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
	at_response_free(p_response);
}
REGISTER_DEFAULT_ITEM(RIL_REQUEST_GET_CLIR, requestGetCLIR)

	/**
	 * RIL_REQUEST_SET_CLIR
	 ok
	 */
void requestSetCLIR(void *data, size_t datalen, RIL_Token t)
{
	RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
}
REGISTER_DEFAULT_ITEM(RIL_REQUEST_SET_CLIR, requestSetCLIR)

static int forwardFromCCFCLine(char *line, RIL_CallForwardInfo *p_forward)
{
	int err;
	int state;
	int mode;

	err = at_tok_start(&line);
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &(p_forward->status));
	if (err < 0) goto error;

	err = at_tok_nextint(&line, &(p_forward->serviceClass));
	if (err < 0) goto error;

	if (at_tok_hasmore(&line)) {
		err = at_tok_nextstr(&line, &(p_forward->number));

		/* tolerate null here */
		if (err < 0) return 0;

		if (p_forward->number != NULL
				&& 0 == strspn(p_forward->number, "+0123456789")
		   ) {
			p_forward->number = NULL;
		}

		err = at_tok_nextint(&line, &p_forward->toa);
		if (err < 0) goto error;
	}

	return 0;

error:
	LOGE("invalid CCFC line\n");
	return -1;
}

static void requestCallForward(RIL_CallForwardInfo *data, size_t datalen, RIL_Token t)
{
	ATResponse *p_response = NULL;
	ATLine *p_cur;
	int err;
	char *cmd;

	if (datalen != sizeof(*data))
		goto error;

	if (data->status == 2)
		asprintf(&cmd, "AT+CCFC=%d,%d",
				data->reason,
				data->status);
	else
		asprintf(&cmd, "AT+CCFC=%d,%d,\"%s\"",
				data->reason,
				data->status,
				data->number ? data->number : "");

	err = at_send_command_multiline (cmd, "+CCFC:", &p_response);
	free(cmd);

	if (err < 0)
		goto error;

	switch (at_get_cme_error(p_response)) {
		case CME_SUCCESS:
		case CME_ERROR_NON_CME:
			break;

		default:
			goto error;
	}

	if (data->status == 2 ) {
		RIL_CallForwardInfo **forwardList, *forwardPool;
		int forwardCount = 0;
		int validCount = 0;
		int i;

		for (p_cur = p_response->p_intermediates
				; p_cur != NULL
				; p_cur = p_cur->p_next, forwardCount++
		    );

		forwardList = (RIL_CallForwardInfo **)
			alloca(forwardCount * sizeof(RIL_CallForwardInfo *));

		forwardPool = (RIL_CallForwardInfo *)
			alloca(forwardCount * sizeof(RIL_CallForwardInfo));

		memset(forwardPool, 0, forwardCount * sizeof(RIL_CallForwardInfo));

		/* init the pointer array */
		for(i = 0; i < forwardCount ; i++)
			forwardList[i] = &(forwardPool[i]);

		for (p_cur = p_response->p_intermediates
				; p_cur != NULL
				; p_cur = p_cur->p_next
		    ) {
			err = forwardFromCCFCLine(p_cur->line, forwardList[validCount]);

			if (err == 0)
				validCount++;
		}

		RIL_onRequestComplete(t, RIL_E_SUCCESS,
				validCount ? forwardList : NULL,
				validCount * sizeof (RIL_CallForwardInfo *));
	} else
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

done:
	at_response_free(p_response);
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	at_response_free(p_response);
}


/**
 * RIL_REQUEST_QUERY_CALL_FORWARD_STATUS
 ok
 */
void requestQueryCallForwardStatus(void *data, size_t datalen, RIL_Token t)
{
	RIL_CallForwardInfo request;
	request.status = ((int *)data)[0];
	request.reason = ((int *)data)[1];
	requestCallForward(&request, sizeof(request), t);
}
REGISTER_DEFAULT_ITEM(RIL_REQUEST_QUERY_CALL_FORWARD_STATUS, requestQueryCallForwardStatus)

	/**
	 * RIL_REQUEST_SET_CALL_FORWARD
	 *
	 * Configure call forward rule.
	 ok
	 */
void requestSetCallForward(void *data, size_t datalen, RIL_Token t)
{
	requestCallForward(data, datalen, t);
}
REGISTER_DEFAULT_ITEM(RIL_REQUEST_SET_CALL_FORWARD, requestSetCallForward)

	/**
	 * RIL_REQUEST_QUERY_CALL_WAITING
	 *
	 * Query current call waiting state.
	 ok
	 */
void requestQueryCallWaiting(void *data, size_t datalen, RIL_Token t)
{
	ATResponse *p_response;
	p_response = NULL;
	int response[2] = {0, 0};
	int c = ((int *)data)[0];
	char *cmd;
	asprintf(&cmd, "AT+CCWA=1,2,1,%d", c);
	int err = at_send_command_singleline(cmd, "+CCWA: ",
			&p_response);
	free(cmd);

	if (err >= 0 && p_response->success) {
		char *line = p_response->p_intermediates->line;

		err = at_tok_start(&line);

		if (err >= 0) {
			err = at_tok_nextint(&line, &response[0]);

			if (err >= 0)
				err = at_tok_nextint(&line, &response[1]);
		}
	}

	RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
	at_response_free(p_response);
}
REGISTER_DEFAULT_ITEM(RIL_REQUEST_QUERY_CALL_WAITING, requestQueryCallWaiting)

	/**
	 * RIL_REQUEST_SET_CALL_WAITING
	 *
	 * Configure current call waiting state.
	 ok
	 */
void requestSetCallWaiting(void *data, size_t datalen, RIL_Token t)
{
	ATResponse *p_response;
	int enable = ((int *)data)[0];
	int c = ((int *)data)[1];
	char *cmd;
	asprintf(&cmd, "AT+CCWA=1,%d,1,%d", enable, c);
	int err = at_send_command( cmd,
			&p_response);
	free(cmd);
	if (err < 0 || p_response->success == 0) {
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	} else {
		RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	}
	at_response_free(p_response);    
}
REGISTER_DEFAULT_ITEM(RIL_REQUEST_SET_CALL_WAITING, requestSetCallWaiting)

	/**
	 * RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION
	 *
	 * Enables/disables supplementary service related notifications
	 * from the network.
	 *
	 * Notifications are reported via RIL_UNSOL_SUPP_SVC_NOTIFICATION.
	 *
	 * See also: RIL_UNSOL_SUPP_SVC_NOTIFICATION.
	 */
void requestSetSuppSvcNotification(void *data, size_t datalen, RIL_Token t)
{
	int err;
	int ssn = ((int *) data)[0];
	char *cmd;

	assert(ssn == 0 || ssn == 1);

	asprintf(&cmd, "AT+CSSN=%d,%d", ssn, ssn);

	err = at_send_command(cmd, NULL);
	free(cmd);
	if (err < 0)
		goto error;

	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
	return;

error:
	RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
	goto finally;
}
REGISTER_DEFAULT_ITEM(RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION, requestSetSuppSvcNotification)
